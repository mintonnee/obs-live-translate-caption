#pragma once
#include <string>
#include <vector>

// generateContent request builder / response parser for segment translation via
// the Flash-Lite model behind kTranslateModel. Pure functions, no libobs or
// network dependency.
// Spec: docs/specs/001-caption-translation-pipeline.md §3, §4.4, success criterion 6.
namespace lt {

// Pinned Flash-Lite release. A rolling alias (gemini-flash-lite-latest) or a
// newer pinned id (gemini-3.5-flash-lite) also works here; pinned ids keep the
// translation behaviour stable across Google releases.
constexpr const char *kTranslateModel = "gemini-3.1-flash-lite";

// "https://generativelanguage.googleapis.com/v1beta/models/<kTranslateModel>:generateContent"
// The API key is sent as the "x-goog-api-key" header, never in the URL.
std::string translate_endpoint_url();

struct TranslateRequest {
    std::string target_code; // BCP-47, e.g. "ja"
    std::string target_name; // English name, e.g. "Japanese"
    // Previously finalized source segments, oldest first, at most 3. Included as
    // context only; the model must translate only `text`.
    std::vector<std::string> context;
    std::string text; // the source segment to translate
};

// Builds the JSON body:
//   system_instruction.parts[0].text : live-subtitle translator instruction that
//       names "<target_name> (<target_code>)", demands translation-only output
//       (no quotes/explanations/source echo), and says input already in the
//       target language is returned cleaned up as-is.
//   contents : exactly one entry, role "user", whose text carries an optional
//       "Context:" block (one line per context segment) followed by a
//       "Translate:" block with `text`.
//   generationConfig : { "maxOutputTokens": 256 }. No thinkingConfig: Flash-Lite
//       already defaults to minimal thinking (3.1 Flash-Lite default level is
//       "minimal"), so sending it changes nothing and only adds a failure mode.
//   No temperature / topP / topK anywhere in the body.
std::string build_translate_request(const TranslateRequest &req);

struct TranslateResult {
    bool ok = false;
    std::string text;  // trimmed concatenation of candidates[0].content.parts[*].text
    std::string error; // reason when !ok (API error message, "no candidates", "parse error", ...)
};

// Parses a generateContent response body. {"error": {...}} -> !ok with the
// message; missing/empty candidates or parts, or an empty trimmed text -> !ok.
TranslateResult parse_translate_response(const std::string &json_text);

}
