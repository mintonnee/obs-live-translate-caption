#include "translate-protocol.hpp"
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace lt {

namespace {

std::string build_system_instruction(const TranslateRequest &req)
{
    std::string s;
    s += "You are a live-subtitle translator. Translate the source text into ";
    s += req.target_name;
    s += " (";
    s += req.target_code;
    s += "). Output only the translation: no quotes, no explanations, "
         "and do not echo the source text. If the input is already written in the "
         "target language, return it cleaned up as-is (fix casing/punctuation only, "
         "do not translate it again). Keep the translation short and readable, in "
         "the concise style of an on-screen subtitle. Only translate the text in "
         "the \"Translate:\" block below; any \"Context:\" block is prior dialogue "
         "provided for reference only and must not be translated or echoed.";
    if (req.max_chars > 0) {
        s += " Keep the translation within about ";
        s += std::to_string(req.max_chars);
        s += " characters when possible; prefer shorter wording over dropping meaning.";
    }
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

std::string translate_endpoint_url()
{
    return std::string("https://generativelanguage.googleapis.com/v1beta/models/") +
           kTranslateModel + ":generateContent";
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

    json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        r.error = "parse error";
        return r;
    }

    if (j.contains("error")) {
        const auto &err = j["error"];
        if (err.is_object() && err.contains("message") && err["message"].is_string())
            r.error = err["message"].get<std::string>();
        else
            r.error = "error";
        return r;
    }

    if (!j.contains("candidates") || !j["candidates"].is_array() || j["candidates"].empty()) {
        r.error = "no candidates";
        return r;
    }

    const auto &candidate0 = j["candidates"][0];
    if (!candidate0.is_object() || !candidate0.contains("content") ||
        !candidate0["content"].is_object() || !candidate0["content"].contains("parts") ||
        !candidate0["content"]["parts"].is_array()) {
        r.error = "no candidates";
        return r;
    }

    std::string text;
    for (const auto &part : candidate0["content"]["parts"]) {
        if (part.is_object() && part.contains("text") && part["text"].is_string())
            text += part["text"].get<std::string>();
    }

    std::string trimmed = trim(text);
    if (trimmed.empty()) {
        r.error = "empty text";
        return r;
    }

    r.ok = true;
    r.text = trimmed;
    return r;
}

}
