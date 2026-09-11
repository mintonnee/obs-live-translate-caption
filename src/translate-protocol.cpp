#include "translate-protocol.hpp"
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace lt {

namespace {

// Kept short: every token here is paid and read on every sentence, and a
// longer instruction measurably raised Flash-Lite latency. Order still
// matters for a minimal-thinking model: the glossary comes first, framed as
// "names to write as loanwords", and the instruction ends with the target
// language so the last thing the model reads is "answer in <target>". A
// verbatim-spelling rule at the end made it return whole Korean sentences
// untranslated when the glossary held Korean names.
std::string build_system_instruction(const TranslateRequest &req)
{
    const std::string target = req.target_name + " (" + req.target_code + ")";
    std::string s;
    s += "Live-subtitle translator into ";
    s += target;
    s += ".";
    if (!req.glossary.empty()) {
        // Same list the transcriber was biased toward, so STT and translation
        // agree on which words are names.
        s += " Glossary (proper names): ";
        bool first = true;
        for (const auto &term : req.glossary) {
            if (term.empty()) continue;
            if (!first) s += ", ";
            s += "\"";
            s += term;
            s += "\"";
            first = false;
        }
        s += ". Write them as a ";
        s += target;
        s += " speaker writes a foreign name (transliterated, or Latin spelling "
             "if usual); never translate them into common words. Translate "
             "everything else.";
    }
    s += " Translate the \"Translate:\" text; \"Context:\" is earlier dialogue, "
         "reference only. Output only the translation: no quotes, no "
         "explanations, do not echo the source. Short, subtitle style. Only if "
         "the whole input is already in ";
    s += target;
    s += ", return it as-is with punctuation fixed.";
    s += " Always answer in ";
    s += target;
    s += ".";
    return s;
}

std::string build_user_text(const TranslateRequest &req)
{
    std::string s;
    if (!req.context.empty()) {
        s += "Context:\n";
        for (const auto &segment : req.context) {
            s += segment;
            s += "\n";
        }
        s += "\n";
    }
    s += "Translate:\n";
    s += req.text;
    return s;
}

bool is_ascii_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trim(const std::string &s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && is_ascii_ws(s[b]))
        ++b;
    while (e > b && is_ascii_ws(s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

} // namespace

std::string translate_endpoint_url(std::string_view model)
{
    return std::string("https://generativelanguage.googleapis.com/v1beta/models/") +
           std::string(model.empty() ? std::string_view(kTranslateModel) : model) +
           ":generateContent";
}

std::string build_translate_request(const TranslateRequest &req)
{
    json j;
    j["system_instruction"]["parts"][0]["text"] = build_system_instruction(req);

    json content;
    content["role"] = "user";
    content["parts"][0]["text"] = build_user_text(req);
    j["contents"] = json::array({content});

    // No thinkingConfig on purpose: Flash-Lite already defaults to minimal
    // thinking, so sending it changes nothing and some Flash-Lite versions
    // reject thinkingLevel with HTTP 400. (If it is ever added, the REST
    // shape is generationConfig.thinkingConfig.thinkingLevel — camelCase, nested;
    // the SDK-style flat "thinking_level" is rejected with "Unknown name".)
    j["generationConfig"]["maxOutputTokens"] = 256;

    return j.dump();
}

TranslateResult parse_translate_response(const std::string &json_text)
{
    TranslateResult r;
    if (json_text.size() > BoundedTranslationResponse::MaxBytes) {
        r.failure = TranslationFailure::ResponseLimit;
        r.error = "response limit";
        return r;
    }

    json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        r.error = "parse error";
        return r;
    }

    if (j.contains("error")) {
        r.failure = TranslationFailure::Http;
        const auto &err = j["error"];
        if (err.is_object() && err.contains("message") && err["message"].is_string())
            r.error = err["message"].get<std::string>();
        else
            r.error = "error";
        return r;
    }

    if (!j.contains("candidates") || !j["candidates"].is_array() || j["candidates"].empty()) {
        r.failure = TranslationFailure::Empty;
        r.error = "no candidates";
        return r;
    }

    const auto &candidate0 = j["candidates"][0];
    if (!candidate0.is_object() || !candidate0.contains("content") ||
        !candidate0["content"].is_object() || !candidate0["content"].contains("parts") ||
        !candidate0["content"]["parts"].is_array()) {
        r.failure = TranslationFailure::Empty;
        r.error = "no candidates";
        return r;
    }

    std::string text;
    for (const auto &part : candidate0["content"]["parts"]) {
        if (part.is_object() && part.contains("text") && part["text"].is_string()) {
            const auto &piece = part["text"].get_ref<const std::string &>();
            if (piece.size() > 16 * 1024 - text.size()) {
                r.failure = TranslationFailure::ResponseLimit;
                r.error = "translation limit";
                return r;
            }
            text += piece;
        }
    }

    std::string trimmed = trim(text);
    if (trimmed.empty()) {
        r.failure = TranslationFailure::Empty;
        r.error = "empty text";
        return r;
    }

    r.ok = true;
    r.failure = TranslationFailure::None;
    r.text = trimmed;
    return r;
}

bool BoundedTranslationResponse::append(std::string_view chunk)
{
    if (exceeded_ || chunk.size() > MaxBytes - body_.size()) {
        exceeded_ = true;
        return false;
    }
    body_.append(chunk.data(), chunk.size());
    return true;
}

}
