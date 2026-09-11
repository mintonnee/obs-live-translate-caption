#pragma once
#include "caption-segment.hpp"
#include <string>
#include <string_view>
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
// `model` is a generateContent model id such as "gemini-3.1-flash-lite"; empty
// selects kTranslateModel. The id is used verbatim in the URL path.
std::string translate_endpoint_url(std::string_view model = kTranslateModel);

struct TranslateRequest {
    std::string target_code; // BCP-47, e.g. "ja"
    std::string target_name; // Native name, e.g. "日本語"; unknown code falls back to itself.
    // Previously finalized source segments, oldest first, at most 3. Included as
    // context only; the model must translate only `text`.
    std::vector<std::string> context;
    std::string text; // the source segment to translate
    // Deprecated compatibility field. Pagination, not prompt compression,
    // enforces the display box, so this value is intentionally ignored.
    int max_chars = 0;
    // Proper nouns / terms the transcriber was biased toward (the filter's
    // custom vocabulary). Listed as names; never force source spelling.
    std::vector<std::string> glossary;
    QualitySuspectReason retry_reason = QualitySuspectReason::None;
};

// Builds the JSON body:
//   system_instruction.parts[0].text : short common instruction naming the
//       target language + optional STT names + a short
//       common correction sentence on retry. No language-specific rules or examples.
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
    TranslationFailure failure = TranslationFailure::Parse;
    std::string text;  // trimmed concatenation of candidates[0].content.parts[*].text
    std::string error; // reason when !ok (API error message, "no candidates", "parse error", ...)
    std::string finish_reason; // candidates[0].finishReason when present
};

// Feed HTTP chunks directly here (disable the transport's body accumulation).
// On false, request transport cancellation; do not parse the partial body.
class BoundedTranslationResponse {
public:
    static constexpr size_t MaxBytes = 64 * 1024;
    bool append(std::string_view chunk);
    bool exceeded() const { return exceeded_; }
    const std::string &body() const { return body_; }
private:
    std::string body_;
    bool exceeded_ = false;
};

// Parses a generateContent response body. {"error": {...}} -> !ok with the
// message; missing/empty candidates or parts, or an empty trimmed text -> !ok.
TranslateResult parse_translate_response(const std::string &json_text);

}
