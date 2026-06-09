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
 * LLM message-rewriting for the AGI engine.
 *
 * When a player types a command that matches a said() in the game's logic,
 * the original scripted message is intercepted and sent to a language model
 * (Ollama locally, or an OpenAI/Claude cloud API).  The model rewrites the
 * response to be more vivid and varied, then returns it for display.
 *
 * HTTP transport:
 *   - All requests (Ollama, OpenAI, Claude) go through ScummVM's portable
 *     networking layer (Networking::SessionRequest), backed by libcurl on
 *     desktop builds.  Requires ScummVM built with cloud/libcurl support
 *     (USE_CLOUD); without it a warning is logged and the original text shown.
 *
 * Configuration keys in scummvm.ini (game section or [scummvm] default):
 *   llm_provider      = none | ollama | openai | claude
 *   llm_endpoint      = <base URL>   (e.g. http://localhost:11434)
 *   llm_api_key       = <key>        (required for openai / claude)
 *   llm_model         = <model>      (optional, sensible defaults used)
 *   llm_system_prompt = <text>       (optional override)
 *   llm_timeout_ms    = 10000        (optional, default 10 s)
 */

// HTTP transport goes through ScummVM's portable networking layer
// (Networking::SessionRequest), backed by libcurl on desktop and platform-native
// HTTP elsewhere.  Routing through this backend keeps <winsock2.h>/<windows.h>
// and libcurl headers out of engine code, so the engine compiles cleanly and
// ports without any per-OS socket handling.
#ifdef USE_CLOUD
#include "backends/networking/http/sessionrequest.h"
#include "backends/networking/http/request.h"
#include "common/system.h"
#endif

#include "agi/llm.h"

#include "common/archive.h"
#include "common/config-manager.h"
#include "common/fs.h"
#include "common/stream.h"
#include "common/str.h"
#include "common/textconsole.h"

#include <cstdlib>
#include <cstring>
#include <cctype>

namespace Agi {

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

// Replace Unicode typography with ASCII equivalents and strip any remaining
// non-ASCII bytes so the AGI bitmap font (which only covers 0x20-0x7E) can
// render the text without garbage characters.
static Common::String sanitizeForAgi(const Common::String &s) {
	Common::String out;
	const unsigned char *p = (const unsigned char *)s.c_str();
	while (*p) {
		unsigned char c = *p;

		// Detect 3-byte UTF-8 sequences in the U+2000-U+27FF range (punctuation,
		// general punctuation block).  The lead byte is 0xE2, second is 0x80 or
		// 0x80-0x9F for the common typographic characters.
		if (c == 0xE2 && p[1] == 0x80) {
			unsigned char trail = p[2];
			switch (trail) {
			case 0x98: case 0x99:           // ' '  left/right single quotation mark
				out += '\''; p += 3; continue;
			case 0x9C: case 0x9D:           // " "  left/right double quotation mark
				out += '"';  p += 3; continue;
			case 0x93: case 0x94:           // – —  en-dash / em-dash
				out += '-';  p += 3; continue;
			case 0xA6:                      // …  horizontal ellipsis
				out += "..."; p += 3; continue;
			case 0xA2:                      // ‚  single low-9 quotation mark
				out += '\''; p += 3; continue;
			case 0xB9: case 0xBA:           // ‹ ›  single angle quotation marks
				out += '\''; p += 3; continue;
			case 0xB2: case 0xB3:           // ′ ″  ditto / double prime
				out += '"';  p += 3; continue;
			default:
				p += 3; continue;           // skip other U+20xx characters
			}
		}

		// Latin-1 Supplement (U+00C0-U+00FF, lead byte 0xC3): accented letters
		// common in Spanish, French, Portuguese, etc.  Strip diacritics to the
		// plain ASCII base letter so the AGI font can render them.
		if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
			// Each trail byte t maps U+00C0+offset → table[offset].
			// 0 means the character has no useful ASCII equivalent (skip it).
			static const char kLatin1Map[64] = {
				//  C0   C1   C2   C3   C4   C5   C6   C7
				    'A', 'A', 'A', 'A', 'A', 'A', 'A', 'C', // À Á Â Ã Ä Å Æ Ç
				//  C8   C9   CA   CB   CC   CD   CE   CF
				    'E', 'E', 'E', 'E', 'I', 'I', 'I', 'I', // È É Ê Ë Ì Í Î Ï
				//  D0   D1   D2   D3   D4   D5   D6   D7
				    'D', 'N', 'O', 'O', 'O', 'O', 'O',  0,  // Ð Ñ Ò Ó Ô Õ Ö ×
				//  D8   D9   DA   DB   DC   DD   DE   DF
				    'O', 'U', 'U', 'U', 'U', 'Y',  0,  's', // Ø Ù Ú Û Ü Ý Þ ß
				//  E0   E1   E2   E3   E4   E5   E6   E7
				    'a', 'a', 'a', 'a', 'a', 'a', 'a', 'c', // à á â ã ä å æ ç
				//  E8   E9   EA   EB   EC   ED   EE   EF
				    'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i', // è é ê ë ì í î ï
				//  F0   F1   F2   F3   F4   F5   F6   F7
				    'd', 'n', 'o', 'o', 'o', 'o', 'o',  0,  // ð ñ ò ó ô õ ö ÷
				//  F8   F9   FA   FB   FC   FD   FE   FF
				    'o', 'u', 'u', 'u', 'u', 'y',  0,  'y', // ø ù ú û ü ý þ ÿ
			};
			char mapped = kLatin1Map[p[1] - 0x80];
			if (mapped) out += mapped;
			p += 2;
			continue;
		}

		// Skip all other multi-byte UTF-8 lead bytes (non-ASCII)
		if (c >= 0x80) {
			// Advance past the full sequence (2-, 3-, or 4-byte)
			if      (c >= 0xF0) p += 4;
			else if (c >= 0xE0) p += 3;
			else if (c >= 0xC0) p += 2;
			else                p += 1; // stray continuation byte
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

// Extract the string value of a JSON key from within jsonFragment.
// Finds the first occurrence of "key":"..." and returns the decoded value.
static Common::String jsonExtractString(const char *jsonFragment, const char *key) {
	// Build the search pattern  "key":
	char pattern[128];
	snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	const char *p = strstr(jsonFragment, pattern);
	if (!p) return "";
	p += strlen(pattern);
	while (*p == ' ' || *p == '\t') ++p;
	if (*p != '"') return "";
	++p; // skip opening quote

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
		"You are an AI narrator for a classic Sierra AGI adventure game. "
		"The player has typed a command and the game has a scripted response. "
		"Rewrite that response to be more vivid, atmospheric and varied while "
		"preserving ALL the original information and tone exactly. "
		"Keep it concise (2-3 sentences). Do not invent new gameplay elements, "
		"items, exits or puzzles that are not already in the original response.";
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
#ifdef USE_CLOUD
	// SessionRequest registers itself with ConnMan on construction; we drive it
	// synchronously with start() + a bounded wait, then hand it back to ConnMan
	// for deletion via close() (we must NOT delete it ourselves).
	Networking::SessionRequest *rq =
		new Networking::SessionRequest(urlStr, Common::Path(), nullptr, nullptr, false);

	rq->addHeader("Content-Type: " + contentType);

	// extraHeaders is a \r\n-separated block; add each line as its own header.
	if (!extraHeaders.empty()) {
		Common::String remaining = extraHeaders;
		while (!remaining.empty()) {
			uint32 nl = remaining.find('\r');
			if (nl == Common::String::npos)
				nl = remaining.size();
			Common::String line(remaining.c_str(), nl);
			if (!line.empty())
				rq->addHeader(line);
			if (nl + 2 <= remaining.size())
				remaining = Common::String(remaining.c_str() + nl + 2);
			else
				break;
		}
	}

	// Providing a byte buffer makes SessionRequest issue a POST with this body.
	// SessionRequest takes ownership of the buffer and frees it.
	byte *buffer = new byte[body.size()];
	memcpy(buffer, body.c_str(), body.size());
	rq->setBuffer(buffer, body.size());

	// Start the request and wait for it, but cap the wait at the configured
	// timeout so a stalled endpoint can't hang the game. ConnMan's timer thread
	// drives the transfer while we spin here.
	rq->start();
	uint32 deadline = g_system->getMillis() + (uint32)_config.timeoutMs;
	while (rq->state() == Networking::PROCESSING && g_system->getMillis() < deadline)
		g_system->delayMillis(5);

	Common::String response;
	if (rq->success()) {
		const char *text = rq->text();
		if (text)
			response = text;
	} else if (rq->state() == Networking::PROCESSING) {
		warning("LLM: request to %s timed out after %d ms", urlStr.c_str(), _config.timeoutMs);
	} else {
		warning("LLM: request to %s failed", urlStr.c_str());
	}

	rq->close();
	return response;
#else
	(void)urlStr;
	(void)body;
	(void)contentType;
	(void)extraHeaders;
	warning("LLM: network support requires ScummVM built with cloud/libcurl support "
	        "(USE_CLOUD). Rebuild with cloud support enabled to use the LLM features.");
	return "";
#endif
}

Common::String LlmClient::queryOllama(const Common::String &prompt) const {
	const Common::String &sys = _config.systemPrompt.empty()
		? Common::String(getDefaultSystemPrompt())
		: _config.systemPrompt;

	const Common::String model = _config.model.empty() ? "llama3" : _config.model;

	// Combine system prompt and user prompt into a single Ollama prompt
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

	// Response: {"choices":[{"message":{"content":"..."}},...]}
	// Find "message" then extract "content" from that point forward.
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

	// Claude needs two extra headers; pass them as a \r\n-separated block.
	Common::String extraHdrs =
		"x-api-key: " + _config.apiKey + "\r\n"
		"anthropic-version: 2023-06-01";

	Common::String resp = httpPost(base + "/v1/messages", reqBody,
	                               "application/json", extraHdrs);
	if (resp.empty()) return "";

	// Response: {"content":[{"type":"text","text":"..."}],...}
	// Locate the first text block and extract its "text" value.
	const char *textBlock = strstr(resp.c_str(), "\"type\":\"text\"");
	if (!textBlock) return "";
	return jsonExtractString(textBlock, "text");
}

Common::String LlmClient::query(const Common::String &gameMessage,
                                  int roomNumber,
                                  const Common::String &playerInput) {
	if (!isEnabled()) return "";

	// Diagnostic: show which system prompt is active (first 80 chars).
	const Common::String &activeSys = _config.systemPrompt.empty()
		? Common::String(getDefaultSystemPrompt())
		: _config.systemPrompt;
	Common::String sysPrev(activeSys.c_str(),
	                       activeSys.size() < 80 ? activeSys.size() : 80);
	warning("LLM query: sys_prompt[0..80]='%s'", sysPrev.c_str());

	Common::String prompt = buildPrompt(gameMessage, roomNumber, playerInput);

	Common::String result;
	switch (_config.provider) {
	case kLlmProviderOllama:  result = queryOllama(prompt); break;
	case kLlmProviderOpenAI:  result = queryOpenAI(prompt); break;
	case kLlmProviderClaude:  result = queryClaude(prompt); break;
	default:                  return "";
	}
	return sanitizeForAgi(result);
}

// ---------------------------------------------------------------------------
// Configuration loader
// ---------------------------------------------------------------------------

LlmConfig loadLlmConfig() {
	LlmConfig cfg;

	Common::String provider = ConfMan.get("llm_provider");
	warning("LLM: loadLlmConfig() provider='%s' personalityFile='%s' gamePath='%s'",
	        provider.c_str(),
	        ConfMan.get("llm_personality_file").c_str(),
	        ConfMan.hasKey("path") ? ConfMan.get("path").c_str() : "(none)");

	if (provider == "ollama") {
		cfg.provider = kLlmProviderOllama;
	} else if (provider == "openai") {
		cfg.provider = kLlmProviderOpenAI;
	} else if (provider == "claude") {
		cfg.provider = kLlmProviderClaude;
	} else {
		// "none" or absent — LLM disabled, game runs normally.
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

	// Note: the personality file is NOT read here — this runs in the engine
	// constructor before SearchMan has the game directory.  Call
	// LlmClient::applyPersonalityFile() from AgiEngine::initialize() instead.

	return cfg;
}

// ---------------------------------------------------------------------------
// Personality file loader (call after SearchMan is ready)
// ---------------------------------------------------------------------------

void LlmClient::applyPersonalityFile() {
	if (_config.personalityFile.empty())
		return;

	const Common::String &filePath = _config.personalityFile;

	// Try SearchMan first — works for any path registered in the game directory
	// or engine extra paths.
	Common::SeekableReadStream *stream =
		SearchMan.createReadStreamForMember(
			Common::Path(filePath, Common::Path::kNativeSeparator));

	// For relative filenames, construct absolute path using the game directory
	// from ConfMan ("path" key).  This is the most reliable method for files
	// sitting alongside the game data.
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

	// Last resort: try as an absolute path exactly as given.
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

		// Strip trailing whitespace / BOM
		while (!content.empty()) {
			unsigned char last = (unsigned char)content[content.size() - 1];
			if (last == '\n' || last == '\r' || last == ' ' || last == '\t')
				content.deleteLastChar();
			else
				break;
		}
		// Strip UTF-8 BOM at start if present (0xEF 0xBB 0xBF)
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

} // End of namespace Agi
