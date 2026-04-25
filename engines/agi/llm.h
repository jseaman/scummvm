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

#ifndef AGI_LLM_H
#define AGI_LLM_H

#include "common/str.h"
#include "common/array.h"

namespace Agi {

enum LlmProvider {
	kLlmProviderNone   = 0,
	kLlmProviderOllama,
	kLlmProviderOpenAI,
	kLlmProviderClaude
};

struct LlmConfig {
	LlmProvider    provider;
	Common::String endpoint;         // base URL, e.g. "http://localhost:11434"
	Common::String apiKey;           // API key for cloud providers
	Common::String model;            // model name, e.g. "llama3", "gpt-4o-mini"
	Common::String systemPrompt;     // inline system prompt (llm_system_prompt key)
	Common::String personalityFile;  // path to personality text file (llm_personality_file key)
	int            timeoutMs;        // HTTP timeout in milliseconds

	LlmConfig() : provider(kLlmProviderNone), timeoutMs(10000) {}
};

// Read LLM configuration from ScummVM's ConfMan (scummvm.ini).
// Keys (under the current game's section or the default section):
//   llm_provider          = none | ollama | openai | claude
//   llm_endpoint          = http://localhost:11434  (optional, provider defaults if empty)
//   llm_api_key           = <api key>               (required for openai / claude)
//   llm_model             = llama3                  (optional, sensible defaults used)
//   llm_system_prompt     = <custom prompt>         (optional inline prompt)
//   llm_personality_file  = personality_rhyme.txt   (path to a personality text file;
//                                                    relative paths are resolved via
//                                                    SearchMan i.e. the game directory)
//   llm_timeout_ms        = 10000                   (optional)
LlmConfig loadLlmConfig();

class LlmClient {
public:
	LlmClient();
	~LlmClient();

	void configure(const LlmConfig &config);
	bool isEnabled() const { return _config.provider != kLlmProviderNone; }

	// Call this from AgiEngine::initialize(), after SearchMan has the game
	// directory registered.  Reads the personality file (if configured) and
	// stores its contents as the active system prompt.
	void applyPersonalityFile();

	// Query the configured LLM.  Returns the LLM's rewrite of gameMessage, or
	// an empty string on failure (caller should fall back to the original text).
	Common::String query(const Common::String &gameMessage,
	                     int roomNumber,
	                     const Common::String &playerInput);

private:
	LlmConfig _config;

	static const char *getDefaultSystemPrompt();

	Common::String buildPrompt(const Common::String &gameMessage,
	                           int roomNumber,
	                           const Common::String &playerInput) const;

	// Provider-specific request/response
	Common::String queryOllama(const Common::String &prompt) const;
	Common::String queryOpenAI(const Common::String &prompt) const;
	Common::String queryClaude(const Common::String &prompt) const;

	// Low-level HTTP POST.  extraHeaders is a \r\n-separated list of header
	// lines injected verbatim (no trailing \r\n needed).
	Common::String httpPost(const Common::String &url,
	                        const Common::String &body,
	                        const Common::String &contentType,
	                        const Common::String &extraHeaders) const;
};

} // End of namespace Agi

#endif // AGI_LLM_H
