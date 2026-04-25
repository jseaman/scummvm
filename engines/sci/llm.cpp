/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 * LLM message-rewriting for the SCI engine.
 *
 * Every scripted message shown to the player is intercepted and sent to a
 * language model (Ollama locally, or an OpenAI/Claude cloud API). The model
 * rewrites the response according to the active personality file, then returns
 * it for display.
 *
 * HTTP transport:
 *   - Plain HTTP (Ollama default): raw platform sockets, no extra deps.
 *   - HTTPS (OpenAI, Claude): libcurl, enabled when ScummVM is built with
 *     USE_CLOUD.  Without it a warning is logged and the original text shown.
 *
 * Configuration keys in scummvm.ini (game section or [scummvm] default):
 *   llm_provider          = none | ollama | openai | claude
 *   llm_endpoint          = <base URL>   (e.g. http://localhost:11434)
 *   llm_api_key           = <key>        (required for openai / claude)
 *   llm_model             = <model>      (optional, sensible defaults used)
 *   llm_system_prompt     = <text>       (optional inline override)
 *   llm_personality_file  = <path>       (text file that becomes system prompt)
 *   llm_timeout_ms        = 10000        (optional, default 10 s)
 */

// Windows socket headers must come before any ScummVM headers that pull in
// <windows.h>, otherwise winsock.h (v1) conflicts with winsock2.h.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET LlmSock;
#define LLM_INVALID_SOCK  INVALID_SOCKET
#define llm_close(s)      closesocket(s)
#define llm_send(s,b,l)   send(s, b, l, 0)
#define llm_recv(s,b,l)   recv(s, b, l, 0)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/select.h>
typedef int LlmSock;
#define LLM_INVALID_SOCK  (-1)
#define llm_close(s)      ::close(s)
#define llm_send(s,b,l)   ::send(s, b, l, 0)
#define llm_recv(s,b,l)   ::recv(s, b, l, 0)
#endif

// HTTPS via libcurl (available when ScummVM is built with USE_CLOUD)
#ifdef USE_CLOUD
#include <curl/curl.h>
#define LLM_HAS_CURL 1
#endif

#include "sci/llm.h"

#include "common/archive.h"
#include "common/config-manager.h"
#include "common/fs.h"
#include "common/stream.h"
#include "common/str.h"
#include "common/textconsole.h"

#include <cstdlib>
#include <cstring>
#include <cctype>

namespace Sci {

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------

struct LlmUrl {
	bool           isHttps;
	Common::String host;
	int            port;
	Common::String path;
};

static bool urlStartsWith(const char *url, const char *prefix) {
	while (*prefix) {
		if (tolower((unsigned char)*url) != (unsigned char)*prefix)
			return false;
		++url; ++prefix;
	}
	return true;
}

static bool parseUrl(const Common::String &urlStr, LlmUrl &out) {
	const char *p = urlStr.c_str();
	if (urlStartsWith(p, "https://")) {
		out.isHttps = true;
		p += 8;
	} else if (urlStartsWith(p, "http://")) {
		out.isHttps = false;
		p += 7;
	} else {
		warning("LLM: URL must start with http:// or https:// (got: %s)", urlStr.c_str());
		return false;
	}

	const char *slash = strchr(p, '/');
	Common::String hostPort;
	if (slash) {
		hostPort  = Common::String(p, (uint32)(slash - p));
		out.path  = Common::String(slash);
	} else {
		hostPort = p;
		out.path = "/";
	}

	const char *colon = strchr(hostPort.c_str(), ':');
	if (colon) {
		out.host = Common::String(hostPort.c_str(), (uint32)(colon - hostPort.c_str()));
		out.port = atoi(colon + 1);
	} else {
		out.host = hostPort;
		out.port = out.isHttps ? 443 : 80;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Plain-socket HTTP (used for HTTP endpoints, e.g. local Ollama)
// ---------------------------------------------------------------------------

#ifdef _WIN32
static bool g_wsaReady = false;
static void ensureWsa() {
	if (!g_wsaReady) {
		WSADATA wd;
		WSAStartup(MAKEWORD(2, 2), &wd);
		g_wsaReady = true;
	}
}
#endif

static LlmSock tcpConnect(const Common::String &host, int port) {
#ifdef _WIN32
	ensureWsa();
#endif
	char portBuf[16];
	snprintf(portBuf, sizeof(portBuf), "%d", port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res = nullptr;
	if (getaddrinfo(host.c_str(), portBuf, &hints, &res) != 0 || !res) {
		warning("LLM: getaddrinfo failed for %s:%d", host.c_str(), port);
		return LLM_INVALID_SOCK;
	}

	LlmSock sock = LLM_INVALID_SOCK;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (sock == LLM_INVALID_SOCK) continue;
		if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
		llm_close(sock);
		sock = LLM_INVALID_SOCK;
	}
	freeaddrinfo(res);

	if (sock == LLM_INVALID_SOCK)
		warning("LLM: could not connect to %s:%d", host.c_str(), port);
	return sock;
}

static void sockSetTimeout(LlmSock sock, int ms) {
#ifdef _WIN32
	DWORD t = (DWORD)ms;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
#else
	struct timeval tv;
	tv.tv_sec  = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

static bool sockSendAll(LlmSock sock, const char *buf, int len) {
	int sent = 0;
	while (sent < len) {
		int r = llm_send(sock, buf + sent, len - sent);
		if (r <= 0) return false;
		sent += r;
	}
	return true;
}

static Common::String readHeaders(LlmSock sock) {
	Common::String hdr;
	char c;
	while (true) {
		int r = llm_recv(sock, &c, 1);
		if (r <= 0) break;
		hdr += c;
		if (hdr.size() >= 4 && hdr[hdr.size()-4] == '\r' && hdr[hdr.size()-3] == '\n'
		                     && hdr[hdr.size()-2] == '\r' && hdr[hdr.size()-1] == '\n')
			break;
	}
	return hdr;
}

static int parseContentLength(const Common::String &headers) {
	Common::String lower = headers;
	lower.toLowercase();
	const char *p = strstr(lower.c_str(), "content-length:");
	if (!p) return -1;
	ptrdiff_t offset = p - lower.c_str();
	p = headers.c_str() + offset + 15;
	while (*p == ' ') ++p;
	return atoi(p);
}

static bool isChunked(const Common::String &headers) {
	Common::String lower = headers;
	lower.toLowercase();
	return strstr(lower.c_str(), "transfer-encoding: chunked") != nullptr;
}

static Common::String readChunkedBody(LlmSock sock) {
	Common::String body;
	char lineBuf[64];
	int lineLen;
	char c;
	while (true) {
		lineLen = 0;
		while (lineLen < 63) {
			if (llm_recv(sock, &c, 1) <= 0) return body;
			if (c == '\n') break;
			if (c != '\r') lineBuf[lineLen++] = c;
		}
		lineBuf[lineLen] = 0;
		int chunkSz = (int)strtol(lineBuf, nullptr, 16);
		if (chunkSz == 0) break;
		char tmp[4096];
		int rem = chunkSz;
		while (rem > 0) {
			int toRead = rem < 4096 ? rem : 4096;
			int r = llm_recv(sock, tmp, toRead);
			if (r <= 0) return body;
			body += Common::String(tmp, r);
			rem -= r;
		}
		llm_recv(sock, &c, 1); // \r
		llm_recv(sock, &c, 1); // \n
	}
	return body;
}

static Common::String socketHttpPost(const LlmUrl &url,
                                     const Common::String &body,
                                     const Common::String &contentType,
                                     const Common::String &extraHeaders,
                                     int timeoutMs) {
	LlmSock sock = tcpConnect(url.host, url.port);
	if (sock == LLM_INVALID_SOCK) return "";
	sockSetTimeout(sock, timeoutMs);

	Common::String req;
	req += "POST " + url.path + " HTTP/1.1\r\n";
	req += "Host: " + url.host + "\r\n";
	req += "Content-Type: " + contentType + "\r\n";
	req += Common::String::format("Content-Length: %u\r\n", (unsigned)body.size());
	if (!extraHeaders.empty())
		req += extraHeaders + "\r\n";
	req += "Connection: close\r\n\r\n";
	req += body;

	if (!sockSendAll(sock, req.c_str(), (int)req.size())) {
		llm_close(sock);
		warning("LLM: send failed");
		return "";
	}

	Common::String headers = readHeaders(sock);
	Common::String responseBody;

	if (isChunked(headers)) {
		responseBody = readChunkedBody(sock);
	} else {
		int clen = parseContentLength(headers);
		if (clen > 0) {
			char tmp[4096];
			int rem = clen;
			while (rem > 0) {
				int toRead = rem < 4096 ? rem : 4096;
				int r = llm_recv(sock, tmp, toRead);
				if (r <= 0) break;
				responseBody += Common::String(tmp, r);
				rem -= r;
			}
		} else {
			char tmp[4096];
			while (true) {
				int r = llm_recv(sock, tmp, 4096);
				if (r <= 0) break;
				responseBody += Common::String(tmp, r);
			}
		}
	}

	llm_close(sock);
	return responseBody;
}

// ---------------------------------------------------------------------------
// libcurl HTTPS (only when USE_CLOUD is defined)
// ---------------------------------------------------------------------------

#ifdef LLM_HAS_CURL
struct CurlBuf {
	Common::String data;
};

static size_t curlWrite(char *ptr, size_t size, size_t nmemb, void *ud) {
	CurlBuf *b = (CurlBuf *)ud;
	b->data += Common::String(ptr, (uint32)(size * nmemb));
	return size * nmemb;
}

static Common::String curlPost(const LlmUrl &url,
                                const Common::String &body,
                                const Common::String &contentType,
                                const Common::String &extraHeaders,
                                int timeoutMs) {
	CURL *curl = curl_easy_init();
	if (!curl) return "";

	Common::String fullUrl = (url.isHttps ? "https://" : "http://") + url.host;
	if ((url.isHttps && url.port != 443) || (!url.isHttps && url.port != 80))
		fullUrl += Common::String::format(":%d", url.port);
	fullUrl += url.path;

	struct curl_slist *hdrs = nullptr;
	hdrs = curl_slist_append(hdrs, ("Content-Type: " + contentType).c_str());

	if (!extraHeaders.empty()) {
		Common::String remaining = extraHeaders;
		while (!remaining.empty()) {
			uint32 nl = remaining.find('\r');
			if (nl == Common::String::npos) nl = remaining.size();
			Common::String line(remaining.c_str(), nl);
			if (!line.empty())
				hdrs = curl_slist_append(hdrs, line.c_str());
			if (nl + 2 <= remaining.size())
				remaining = Common::String(remaining.c_str() + nl + 2);
			else
				break;
		}
	}

	CurlBuf respBuf;
	curl_easy_setopt(curl, CURLOPT_URL,           fullUrl.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
	curl_easy_setopt(curl, CURLOPT_POST,          1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &respBuf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,    (long)timeoutMs);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

	CURLcode rc = curl_easy_perform(curl);
	curl_slist_free_all(hdrs);
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK) {
		warning("LLM: curl error: %s", curl_easy_strerror(rc));
		return "";
	}
	return respBuf.data;
}
#endif // LLM_HAS_CURL

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

// Strip non-ASCII and map Unicode typography to ASCII equivalents so the SCI
// font renderer (which covers 0x20-0x7E) can display the result cleanly.
static Common::String sanitizeForSci(const Common::String &s) {
	Common::String out;
	const unsigned char *p = (const unsigned char *)s.c_str();
	while (*p) {
		unsigned char c = *p;

		// U+2000-U+27FF general punctuation (smart quotes, dashes, ellipsis).
		if (c == 0xE2 && p[1] == 0x80) {
			unsigned char trail = p[2];
			switch (trail) {
			case 0x98: case 0x99:           // left/right single quotation mark
				out += '\''; p += 3; continue;
			case 0x9C: case 0x9D:           // left/right double quotation mark
				out += '"';  p += 3; continue;
			case 0x93: case 0x94:           // en-dash / em-dash
				out += '-';  p += 3; continue;
			case 0xA6:                      // horizontal ellipsis
				out += "..."; p += 3; continue;
			case 0xA2:                      // single low-9 quotation mark
				out += '\''; p += 3; continue;
			case 0xB9: case 0xBA:           // single angle quotation marks
				out += '\''; p += 3; continue;
			case 0xB2: case 0xB3:           // prime / double prime
				out += '"';  p += 3; continue;
			default:
				p += 3; continue;
			}
		}

		// Latin-1 Supplement (U+00C0-U+00FF): accented letters.
		if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
			static const char kLatin1Map[64] = {
				//  C0   C1   C2   C3   C4   C5   C6   C7
				    'A', 'A', 'A', 'A', 'A', 'A', 'A', 'C',
				//  C8   C9   CA   CB   CC   CD   CE   CF
				    'E', 'E', 'E', 'E', 'I', 'I', 'I', 'I',
				//  D0   D1   D2   D3   D4   D5   D6   D7
				    'D', 'N', 'O', 'O', 'O', 'O', 'O',  0,
				//  D8   D9   DA   DB   DC   DD   DE   DF
				    'O', 'U', 'U', 'U', 'U', 'Y',  0,  's',
				//  E0   E1   E2   E3   E4   E5   E6   E7
				    'a', 'a', 'a', 'a', 'a', 'a', 'a', 'c',
				//  E8   E9   EA   EB   EC   ED   EE   EF
				    'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
				//  F0   F1   F2   F3   F4   F5   F6   F7
				    'd', 'n', 'o', 'o', 'o', 'o', 'o',  0,
				//  F8   F9   FA   FB   FC   FD   FE   FF
				    'o', 'u', 'u', 'u', 'u', 'y',  0,  'y',
			};
			char mapped = kLatin1Map[p[1] - 0x80];
			if (mapped) out += mapped;
			p += 2;
			continue;
		}

		// Skip all other multi-byte UTF-8 sequences.
		if (c >= 0x80) {
			if      (c >= 0xF0) p += 4;
			else if (c >= 0xE0) p += 3;
			else if (c >= 0xC0) p += 2;
			else                p += 1;
			continue;
		}

		out += (char)c;
		++p;
	}
	return out;
}

static Common::String jsonEscape(const Common::String &s) {
	Common::String out;
	for (uint i = 0; i < s.size(); i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n";  break;
		case '\r': out += "\\r";  break;
		case '\t': out += "\\t";  break;
		default:
			if (c < 0x20) {
				char esc[8];
				snprintf(esc, sizeof(esc), "\\u%04x", c);
				out += esc;
			} else {
				out += (char)c;
			}
			break;
		}
	}
	return out;
}

static Common::String jsonExtractString(const char *jsonFragment, const char *key) {
	char pattern[128];
	snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	const char *p = strstr(jsonFragment, pattern);
	if (!p) return "";
	p += strlen(pattern);
	while (*p == ' ' || *p == '\t') ++p;
	if (*p != '"') return "";
	++p;

	Common::String result;
	while (*p && *p != '"') {
		if (*p == '\\' && *(p + 1)) {
			++p;
			switch (*p) {
			case 'n':  result += '\n'; break;
			case 'r':  result += '\r'; break;
			case 't':  result += '\t'; break;
			case '"':  result += '"';  break;
			case '\\': result += '\\'; break;
			case '/':  result += '/';  break;
			default:   result += *p;   break;
			}
		} else {
			result += *p;
		}
		++p;
	}
	return result;
}

// ---------------------------------------------------------------------------
// LlmClient implementation
// ---------------------------------------------------------------------------

LlmClient::LlmClient() {}
LlmClient::~LlmClient() {}

void LlmClient::configure(const LlmConfig &config) {
	_config = config;
}

// static
const char *LlmClient::getDefaultSystemPrompt() {
	return
		"You are an AI narrator for a classic Sierra SCI adventure game. "
		"The game has a scripted message to show the player. "
		"Rewrite that message to be more vivid, atmospheric and varied while "
		"preserving ALL the original information and gameplay meaning exactly. "
		"Keep it concise (2-3 sentences). Do not invent new gameplay elements, "
		"items, exits or puzzles that are not already in the original message.";
}

Common::String LlmClient::buildPrompt(const Common::String &gameMessage,
                                       int roomNumber,
                                       const Common::String &playerInput) const {
	uint32 charLimit = gameMessage.size();
	if (playerInput.empty()) {
		return Common::String::format(
			"Room number: %d\n"
			"Context: automated game message (room description, cutscene, or NPC line)\n"
			"Original game text: %s\n"
			"IMPORTANT: Your response must be %u characters or fewer to fit the game's text box.",
			roomNumber,
			gameMessage.c_str(),
			charLimit
		);
	}
	return Common::String::format(
		"Room number: %d\n"
		"Player command: %s\n"
		"Original game response: %s\n"
		"IMPORTANT: Your response must be %u characters or fewer to fit the game's text box.",
		roomNumber,
		playerInput.c_str(),
		gameMessage.c_str(),
		charLimit
	);
}

Common::String LlmClient::httpPost(const Common::String &urlStr,
                                    const Common::String &body,
                                    const Common::String &contentType,
                                    const Common::String &extraHeaders) const {
	LlmUrl url;
	if (!parseUrl(urlStr, url)) return "";

	if (url.isHttps) {
#ifdef LLM_HAS_CURL
		return curlPost(url, body, contentType, extraHeaders, _config.timeoutMs);
#else
		warning("LLM: HTTPS requires ScummVM built with USE_CLOUD (libcurl). "
		        "Use an http:// endpoint or rebuild with cloud support enabled.");
		return "";
#endif
	}
	return socketHttpPost(url, body, contentType, extraHeaders, _config.timeoutMs);
}

Common::String LlmClient::queryOllama(const Common::String &prompt) const {
	const Common::String &sys = _config.systemPrompt.empty()
		? Common::String(getDefaultSystemPrompt())
		: _config.systemPrompt;

	const Common::String model = _config.model.empty() ? "llama3" : _config.model;
	Common::String fullPrompt = sys + "\n\n" + prompt;

	Common::String reqBody =
		"{\"model\":\"" + jsonEscape(model) + "\","
		"\"prompt\":\"" + jsonEscape(fullPrompt) + "\","
		"\"stream\":false}";

	const Common::String base = _config.endpoint.empty()
		? "http://localhost:11434"
		: _config.endpoint;

	Common::String resp = httpPost(base + "/api/generate", reqBody, "application/json", "");
	if (resp.empty()) return "";
	return jsonExtractString(resp.c_str(), "response");
}

Common::String LlmClient::queryOpenAI(const Common::String &prompt) const {
	if (_config.apiKey.empty()) {
		warning("LLM: openai provider requires llm_api_key to be set");
		return "";
	}

	const Common::String &sys = _config.systemPrompt.empty()
		? Common::String(getDefaultSystemPrompt())
		: _config.systemPrompt;

	const Common::String model = _config.model.empty() ? "gpt-4o-mini" : _config.model;

	Common::String reqBody =
		"{\"model\":\"" + jsonEscape(model) + "\","
		"\"messages\":["
		"{\"role\":\"system\",\"content\":\"" + jsonEscape(sys) + "\"},"
		"{\"role\":\"user\",\"content\":\"" + jsonEscape(prompt) + "\"}"
		"]}";

	const Common::String base = _config.endpoint.empty()
		? "https://api.openai.com"
		: _config.endpoint;

	Common::String auth = "Authorization: Bearer " + _config.apiKey;

	Common::String resp = httpPost(base + "/v1/chat/completions", reqBody,
	                               "application/json", auth);
	if (resp.empty()) return "";

	const char *msgPos = strstr(resp.c_str(), "\"message\"");
	if (!msgPos) return "";
	return jsonExtractString(msgPos, "content");
}

Common::String LlmClient::queryClaude(const Common::String &prompt) const {
	if (_config.apiKey.empty()) {
		warning("LLM: claude provider requires llm_api_key to be set");
		return "";
	}

	const Common::String &sys = _config.systemPrompt.empty()
		? Common::String(getDefaultSystemPrompt())
		: _config.systemPrompt;

	const Common::String model = _config.model.empty() ? "claude-haiku-4-5-20251001" : _config.model;

	Common::String reqBody =
		"{\"model\":\"" + jsonEscape(model) + "\","
		"\"max_tokens\":256,"
		"\"system\":\"" + jsonEscape(sys) + "\","
		"\"messages\":["
		"{\"role\":\"user\",\"content\":\"" + jsonEscape(prompt) + "\"}"
		"]}";

	const Common::String base = _config.endpoint.empty()
		? "https://api.anthropic.com"
		: _config.endpoint;

	Common::String extraHdrs =
		"x-api-key: " + _config.apiKey + "\r\n"
		"anthropic-version: 2023-06-01";

	Common::String resp = httpPost(base + "/v1/messages", reqBody,
	                               "application/json", extraHdrs);
	if (resp.empty()) return "";

	const char *textBlock = strstr(resp.c_str(), "\"type\":\"text\"");
	if (!textBlock) return "";
	return jsonExtractString(textBlock, "text");
}

Common::String LlmClient::query(const Common::String &gameMessage,
                                  int roomNumber,
                                  const Common::String &playerInput) {
	if (!isEnabled()) return "";

	Common::String prompt = buildPrompt(gameMessage, roomNumber, playerInput);

	Common::String result;
	switch (_config.provider) {
	case kLlmProviderOllama:  result = queryOllama(prompt); break;
	case kLlmProviderOpenAI:  result = queryOpenAI(prompt); break;
	case kLlmProviderClaude:  result = queryClaude(prompt); break;
	default:                  return "";
	}
	return sanitizeForSci(result);
}

// ---------------------------------------------------------------------------
// Configuration loader
// ---------------------------------------------------------------------------

LlmConfig loadLlmConfig() {
	LlmConfig cfg;

	Common::String provider = ConfMan.get("llm_provider");
	if (provider == "ollama") {
		cfg.provider = kLlmProviderOllama;
	} else if (provider == "openai") {
		cfg.provider = kLlmProviderOpenAI;
	} else if (provider == "claude") {
		cfg.provider = kLlmProviderClaude;
	} else {
		cfg.provider = kLlmProviderNone;
		return cfg;
	}

	cfg.endpoint        = ConfMan.get("llm_endpoint");
	cfg.apiKey          = ConfMan.get("llm_api_key");
	cfg.model           = ConfMan.get("llm_model");
	cfg.systemPrompt    = ConfMan.get("llm_system_prompt");
	cfg.personalityFile = ConfMan.get("llm_personality_file");

	if (ConfMan.hasKey("llm_timeout_ms")) {
		cfg.timeoutMs = atoi(ConfMan.get("llm_timeout_ms").c_str());
		if (cfg.timeoutMs <= 0) cfg.timeoutMs = 10000;
	}

	return cfg;
}

// ---------------------------------------------------------------------------
// Personality file loader
// ---------------------------------------------------------------------------

void LlmClient::applyPersonalityFile() {
	if (_config.personalityFile.empty())
		return;

	const Common::String &filePath = _config.personalityFile;

	Common::SeekableReadStream *stream =
		SearchMan.createReadStreamForMember(
			Common::Path(filePath, Common::Path::kNativeSeparator));

	if (!stream && ConfMan.hasKey("path")) {
		Common::String gamePath = ConfMan.get("path");
		if (!gamePath.empty()) {
			char last = gamePath[gamePath.size() - 1];
			if (last != '/' && last != '\\')
				gamePath += '/';
		}
		Common::String fullPath = gamePath + filePath;
		Common::FSNode node(Common::Path(fullPath, Common::Path::kNativeSeparator));
		if (node.exists() && node.isReadable())
			stream = node.createReadStream();
	}

	if (!stream) {
		Common::FSNode node(Common::Path(filePath, Common::Path::kNativeSeparator));
		if (node.exists() && node.isReadable())
			stream = node.createReadStream();
	}

	if (!stream) {
		warning("LLM: personality file not found: %s  (game path: %s)",
		        filePath.c_str(),
		        ConfMan.hasKey("path") ? ConfMan.get("path").c_str() : "(unknown)");
		return;
	}

	int32 size = stream->size();
	if (size > 0) {
		char *buf = new char[size + 1];
		uint32 bytesRead = stream->read(buf, size);
		buf[bytesRead] = '\0';

		Common::String content(buf, bytesRead);
		delete[] buf;

		while (!content.empty()) {
			unsigned char last = (unsigned char)content[content.size() - 1];
			if (last == '\n' || last == '\r' || last == ' ' || last == '\t')
				content.deleteLastChar();
			else
				break;
		}
		if (content.size() >= 3 &&
		    (unsigned char)content[0] == 0xEF &&
		    (unsigned char)content[1] == 0xBB &&
		    (unsigned char)content[2] == 0xBF) {
			content = Common::String(content.c_str() + 3);
		}

		if (!content.empty()) {
			_config.systemPrompt = content;
			warning("LLM: personality applied from '%s' (%u bytes)",
			        filePath.c_str(), (unsigned)content.size());
		} else {
			warning("LLM: personality file is empty: %s", filePath.c_str());
		}
	}
	delete stream;
}

} // End of namespace Sci
