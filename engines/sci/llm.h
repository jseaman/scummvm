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

#ifndef SCI_LLM_H
#define SCI_LLM_H

#include "common/str.h"
#include "common/array.h"

namespace Sci {

enum LlmProvider {
	kLlmProviderNone   = 0,
	kLlmProviderOllama,
	kLlmProviderOpenAI,
	kLlmProviderClaude
};

struct LlmConfig {
	LlmProvider    provider;
	Common::String endpoint;
	Common::String apiKey;
	Common::String model;
	Common::String systemPrompt;
	Common::String personalityFile;
	int            timeoutMs;

	LlmConfig() : provider(kLlmProviderNone), timeoutMs(10000) {}
};

// Read LLM configuration from ScummVM's ConfMan (scummvm.ini).
// Keys (under the current game's section or the default section):
//   llm_provider          = none | ollama | openai | claude
//   llm_endpoint          = http://localhost:11434
//   llm_api_key           = <api key>
//   llm_model             = llama3
//   llm_system_prompt     = <custom prompt>
//   llm_personality_file  = personality.txt
//   llm_timeout_ms        = 10000
LlmConfig loadLlmConfig();

class LlmClient {
public:
	LlmClient();
	~LlmClient();

	void configure(const LlmConfig &config);
	bool isEnabled() const { return _config.provider != kLlmProviderNone; }

	// Call after SearchMan has the game directory registered.
	void applyPersonalityFile();

	// Query the LLM. Returns the rewritten text or empty string on failure.
	Common::String query(const Common::String &gameMessage,
	                     int roomNumber,
	                     const Common::String &playerInput);

private:
	LlmConfig _config;

	static const char *getDefaultSystemPrompt();

	Common::String buildPrompt(const Common::String &gameMessage,
	                           int roomNumber,
	                           const Common::String &playerInput) const;

	Common::String queryOllama(const Common::String &prompt) const;
	Common::String queryOpenAI(const Common::String &prompt) const;
	Common::String queryClaude(const Common::String &prompt) const;

	Common::String httpPost(const Common::String &url,
	                        const Common::String &body,
	                        const Common::String &contentType,
	                        const Common::String &extraHeaders) const;
};

} // End of namespace Sci

#endif // SCI_LLM_H
