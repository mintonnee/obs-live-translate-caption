#include <catch2/catch_test_macros.hpp>
#include "translate-protocol.hpp"
#include "languages.hpp"
#include <nlohmann/json.hpp>

using namespace lt;
using nlohmann::json;

TEST_CASE("HTTP response chunks are bounded before parsing")
{
    BoundedTranslationResponse response;
    REQUIRE(response.append(std::string(65535, 'x')));
    REQUIRE(response.append("x"));
    REQUIRE_FALSE(response.append("x"));
    REQUIRE(response.exceeded());
    REQUIRE(response.body().size() == 65536);
    REQUIRE_FALSE(response.append(""));
    REQUIRE(parse_translate_response(std::string(65537, 'x')).failure ==
            TranslationFailure::ResponseLimit);
}

TEST_CASE("translation bytes and malformed UTF8 are explicitly rejected")
{
    auto body = [](const std::string &value) {
        return json{{"candidates", {{{"content", {{"parts", {{{"text", value}}}}}}}}}}.dump();
    };
    REQUIRE(parse_translate_response(body(std::string(16384, 'a'))).ok);
    REQUIRE(parse_translate_response(body(std::string(16385, 'a'))).failure ==
            TranslationFailure::ResponseLimit);
    std::string invalid = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"";
    invalid += char(0xff);
    invalid += "\"}]}}]}";
    REQUIRE(parse_translate_response(invalid).failure == TranslationFailure::Parse);
}

TEST_CASE("translate endpoint url matches the documented generateContent path")
{
    REQUIRE(translate_endpoint_url() ==
            "https://generativelanguage.googleapis.com/v1beta/models/"
            "gemini-3.1-flash-lite:generateContent");
    REQUIRE(std::string(kTranslateModel) == "gemini-3.1-flash-lite");
    REQUIRE(translate_endpoint_url().find("key=") == std::string::npos);
    // Model selector: the id goes verbatim into the path; empty falls back.
    REQUIRE(translate_endpoint_url("gemini-3.5-flash") ==
            "https://generativelanguage.googleapis.com/v1beta/models/"
            "gemini-3.5-flash:generateContent");
    REQUIRE(translate_endpoint_url("") == translate_endpoint_url());
}

TEST_CASE("prompt language names use endonyms and preserve unknown code fallback")
{
    REQUIRE(target_language_name("ja") == "日本語");
    REQUIRE(target_language_name("ko") == "한국어");
    REQUIRE(target_language_name("fr") == "Français");
    REQUIRE(target_language_name("ru") == "Русский");
    REQUIRE(target_language_name("en") == "English");
    REQUIRE(target_language_name("pt-BR") == "Português");
    REQUIRE(target_language_name("pt-PT") == "Português");
    REQUIRE(target_language_name("ja-JP") == "ja-JP"); // Not registered in the UI list.
    REQUIRE(target_language_name("unknown-code") == "unknown-code");
}

TEST_CASE("system instruction names the target language and code")
{
    TranslateRequest req;
    req.target_code = "ja";
    req.target_name = target_language_name(req.target_code);
    req.text = "hello";

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.find("日本語 (ja)") != std::string::npos);
    REQUIRE(instr.find("Japanese") == std::string::npos);
}

TEST_CASE("system instruction demands translation-only output")
{
    TranslateRequest req;
    req.target_code = "fr";
    req.target_name = target_language_name(req.target_code);
    req.text = "hi";

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.find("no quotes") != std::string::npos);
    REQUIRE(instr.find("explanations") != std::string::npos);
    REQUIRE(instr.find("Output only the translation") != std::string::npos);
}

TEST_CASE("system instruction explicitly requests output in the target language")
{
    TranslateRequest req;
    req.target_code = "de";
    req.target_name = target_language_name(req.target_code);
    req.text = "hi";

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.find("Deutsch (de)") != std::string::npos);
    REQUIRE(
        instr.find("Translate the entire \"Translate:\" text into Deutsch (de)") !=
        std::string::npos);
}

TEST_CASE("system instruction ignores the obsolete box compression hint")
{
    TranslateRequest req;
    req.target_code = "ja";
    req.target_name = target_language_name(req.target_code);
    req.text = "hello";
    req.max_chars = 80;

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.find("characters") == std::string::npos);
    // Existing behaviour must still hold when max_chars is set.
    REQUIRE(instr.find("日本語 (ja)") != std::string::npos);
    REQUIRE(instr.find("no quotes") != std::string::npos);
    REQUIRE(j["contents"].size() == 1);
    REQUIRE(j["contents"].back()["role"] == "user");
    REQUIRE(!j["generationConfig"].contains("thinkingConfig"));
    REQUIRE(body.find("temperature") == std::string::npos);
    REQUIRE(body.find("topP") == std::string::npos);
    REQUIRE(body.find("topK") == std::string::npos);
}

TEST_CASE("system instruction omits the length hint when max_chars is zero")
{
    TranslateRequest req;
    req.target_code = "ja";
    req.target_name = target_language_name(req.target_code);
    req.text = "hello";
    req.max_chars = 0;

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.find("characters") == std::string::npos);
}

TEST_CASE("system instruction omits the length hint when max_chars is negative")
{
    TranslateRequest req;
    req.target_code = "ja";
    req.target_name = target_language_name(req.target_code);
    req.text = "hello";
    req.max_chars = -5;

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.find("characters") == std::string::npos);
}

TEST_CASE("contents has exactly one user-role entry")
{
    TranslateRequest req;
    req.target_code = "es";
    req.target_name = target_language_name(req.target_code);
    req.text = "hola?";

    std::string body = build_translate_request(req);
    json j = json::parse(body);

    REQUIRE(j["contents"].is_array());
    REQUIRE(j["contents"].size() == 1);
    REQUIRE(j["contents"].back()["role"] == "user");
}

TEST_CASE("contents text carries context block before the Translate block")
{
    TranslateRequest req;
    req.target_code = "ko";
    req.target_name = target_language_name(req.target_code);
    req.context = {"first segment", "second segment"};
    req.text = "third segment";

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string text = j["contents"][0]["parts"][0]["text"].get<std::string>();

    auto context_pos = text.find("Context:");
    auto first_pos = text.find("first segment");
    auto second_pos = text.find("second segment");
    auto translate_pos = text.find("Translate:");
    auto third_pos = text.find("third segment");

    REQUIRE(context_pos != std::string::npos);
    REQUIRE(first_pos != std::string::npos);
    REQUIRE(second_pos != std::string::npos);
    REQUIRE(translate_pos != std::string::npos);
    REQUIRE(third_pos != std::string::npos);
    // Oldest first, then Translate block, in order.
    REQUIRE(context_pos < first_pos);
    REQUIRE(first_pos < second_pos);
    REQUIRE(second_pos < translate_pos);
    REQUIRE(translate_pos < third_pos);
}

TEST_CASE("contents text omits the Context block when there is no context")
{
    TranslateRequest req;
    req.target_code = "it";
    req.target_name = target_language_name(req.target_code);
    req.text = "only this";

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string text = j["contents"][0]["parts"][0]["text"].get<std::string>();

    REQUIRE(text.find("Context:") == std::string::npos);
    REQUIRE(text.find("Translate:") != std::string::npos);
    REQUIRE(text.find("only this") != std::string::npos);
}

TEST_CASE("generationConfig sets maxOutputTokens only: no thinking or sampling params")
{
    TranslateRequest req;
    req.target_code = "pt";
    req.target_name = target_language_name(req.target_code);
    req.text = "text";

    std::string body = build_translate_request(req);
    json j = json::parse(body);

    REQUIRE(j["generationConfig"]["maxOutputTokens"] == 256);
    // Flash-Lite defaults to minimal thinking, and some Flash-Lite versions
    // reject thinkingLevel; the SDK-style flat "thinking_level" is rejected
    // by the REST API with HTTP 400 "Unknown name". Send neither.
    REQUIRE(!j["generationConfig"].contains("thinkingConfig"));
    REQUIRE(body.find("thinking") == std::string::npos);

    REQUIRE(body.find("temperature") == std::string::npos);
    REQUIRE(body.find("topP") == std::string::npos);
    REQUIRE(body.find("topK") == std::string::npos);
    REQUIRE(body.find("top_p") == std::string::npos);
    REQUIRE(body.find("top_k") == std::string::npos);
}

TEST_CASE("parse rejects invalid JSON")
{
    TranslateResult r = parse_translate_response("not json{{{");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error == "parse error");
}

TEST_CASE("parse surfaces the API error message")
{
    TranslateResult r = parse_translate_response(
        R"({"error": {"code": 400, "message": "API key not valid"}})");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error == "API key not valid");
}

TEST_CASE("parse reports no candidates when candidates is missing")
{
    TranslateResult r = parse_translate_response(R"({"promptFeedback": {}})");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error == "no candidates");
}

TEST_CASE("parse reports no candidates when candidates is an empty array")
{
    TranslateResult r = parse_translate_response(R"({"candidates": []})");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error == "no candidates");
}

TEST_CASE("parse reports no candidates when content.parts is missing")
{
    TranslateResult r = parse_translate_response(
        R"({"candidates": [{"finishReason": "SAFETY"}]})");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error == "no candidates");
}

TEST_CASE("parse reports empty text when all parts are text-empty")
{
    TranslateResult r = parse_translate_response(
        R"({"candidates": [{"content": {"parts": [{"text": ""}]}}]})");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error == "empty text");
}

TEST_CASE("parse reports empty text when parts array is empty")
{
    TranslateResult r = parse_translate_response(
        R"({"candidates": [{"content": {"parts": []}}]})");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error == "empty text");
}

TEST_CASE("parse concatenates text parts in order and trims whitespace")
{
    std::string body = R"({
      "candidates": [
        { "content": { "parts": [
            { "text": "  Hello, " },
            { "text": "world!\n" }
        ] } }
      ]
    })";
    TranslateResult r = parse_translate_response(body);
    REQUIRE(r.ok);
    REQUIRE(r.text == "Hello, world!");
    REQUIRE(r.error.empty());
}

TEST_CASE("parse ignores non-text parts such as thought parts")
{
    std::string body = R"({
      "candidates": [
        { "content": { "parts": [
            { "thought": true },
            { "text": "translated text" }
        ] } }
      ]
    })";
    TranslateResult r = parse_translate_response(body);
    REQUIRE(r.ok);
    REQUIRE(r.text == "translated text");
}

TEST_CASE("parse trims tabs, carriage returns, and newlines from both ends")
{
    std::string body = R"({
      "candidates": [
        { "content": { "parts": [
            { "text": "\t\r\n  padded  \r\n\t" }
        ] } }
      ]
    })";
    TranslateResult r = parse_translate_response(body);
    REQUIRE(r.ok);
    REQUIRE(r.text == "padded");
}

TEST_CASE("parse never throws on malformed shapes")
{
    REQUIRE_NOTHROW(parse_translate_response(R"({"candidates": "not-an-array"})"));
    REQUIRE_NOTHROW(parse_translate_response(R"({"candidates": [ "not-an-object" ]})"));
    REQUIRE_NOTHROW(parse_translate_response(R"({"candidates": [ {"content": "not-an-object"} ]})"));
    REQUIRE_NOTHROW(parse_translate_response(
        R"({"candidates": [ {"content": {"parts": "not-an-array"}} ]})"));
    REQUIRE_NOTHROW(parse_translate_response(R"({"error": "not-an-object"})"));
    REQUIRE_NOTHROW(parse_translate_response(""));
    REQUIRE_NOTHROW(parse_translate_response("[]"));
    REQUIRE_NOTHROW(parse_translate_response("null"));
}

TEST_CASE("parse treats a top-level non-object JSON as a parse error")
{
    TranslateResult r = parse_translate_response("[]");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error == "parse error");
}

TEST_CASE("glossary terms are listed in the system instruction")
{
    TranslateRequest req;
    req.target_code = "ko";
    req.target_name = target_language_name(req.target_code);
    req.text = "hello";
    req.glossary = {"Gemini", "OBS Studio", "곽민규"};

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.find("Glossary") != std::string::npos);
    REQUIRE(instr.find("\"Gemini\"") != std::string::npos);
    REQUIRE(instr.find("\"OBS Studio\"") != std::string::npos);
    REQUIRE(instr.find("\"곽민규\"") != std::string::npos);
    // The optional names remain hints, not a source-spelling preservation rule.
    REQUIRE(instr.find("Render names naturally in the target language") != std::string::npos);
    REQUIRE(instr.find("spelling exactly as listed") == std::string::npos);
    REQUIRE(instr.find("한국어 (ko)") != std::string::npos);
    // The glossary belongs to the instruction, not to the text being translated.
    std::string user = j["contents"][0]["parts"][0]["text"].get<std::string>();
    REQUIRE(user.find("Gemini") == std::string::npos);
}

TEST_CASE("Japanese instruction stays compact without glossary or built-in examples")
{
    TranslateRequest req;
    req.target_code = "ja";
    req.target_name = target_language_name(req.target_code);
    req.text = "hello";
    req.max_chars = 80;

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.size() <= 400);
    REQUIRE(instr.find("日本語 (ja)") != std::string::npos);
    REQUIRE(instr.find("80 characters") == std::string::npos);
    REQUIRE(instr.find("natural Japanese subtitles") == std::string::npos);
    REQUIRE(instr.find("로블록스") == std::string::npos);
    REQUIRE(instr.find("Roblox") == std::string::npos);
}

TEST_CASE("no glossary sentence without terms")
{
    TranslateRequest req;
    req.target_code = "ko";
    req.target_name = target_language_name(req.target_code);
    req.text = "hello";

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();
    REQUIRE(instr.find("Glossary") == std::string::npos);

    req.glossary = {""};
    body = build_translate_request(req);
    j = json::parse(body);
    instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();
    REQUIRE(instr.find("\"\"") == std::string::npos);
}

namespace {

std::string system_instruction(const TranslateRequest &req)
{
    const auto body = build_translate_request(req);
    return json::parse(body)["system_instruction"]["parts"][0]["text"].get<std::string>();
}

} // namespace

TEST_CASE("all target languages use the same short instruction without language rules")
{
    std::string common;
    for (const std::string code : {"ja", "ja-JP", "ko", "fr", "en", "pt-BR", "pt-PT"}) {
        CAPTURE(code);
        TranslateRequest req;
        req.target_code = code;
        req.target_name = target_language_name(code);
        req.text = "hello";
        const std::string instr = system_instruction(req);
        REQUIRE(instr.size() <= 400);
        REQUIRE(instr.find("katakana") == std::string::npos);
        REQUIRE(instr.find("Hangul") == std::string::npos);
        REQUIRE(instr.find("Examples:") == std::string::npos);
        REQUIRE(instr.find("로블록스") == std::string::npos);
        REQUIRE(instr.find("Roblox") == std::string::npos);
        REQUIRE(instr.find("\"Context:\" is reference only") != std::string::npos);
        REQUIRE(instr.find("Preserve meaning") != std::string::npos);
        REQUIRE(instr.find("do not complete unfinished speech") != std::string::npos);

        req.context = {"first", "second", "third"};
        REQUIRE(system_instruction(req) == instr);
        const std::string target = req.target_name + " (" + code + ")";
        const auto pos = instr.find(target);
        REQUIRE(pos != std::string::npos);
        std::string normalized = instr;
        for (auto occurrence = pos; occurrence != std::string::npos;
             occurrence = normalized.find(target, occurrence + 8)) {
            normalized.replace(occurrence, target.size(), "<target>");
        }
        if (common.empty()) common = normalized;
        REQUIRE(normalized == common);
    }
}

TEST_CASE("STT glossary does not inject target spellings")
{
    TranslateRequest req;
    req.target_code = "fr";
    req.target_name = target_language_name(req.target_code);
    req.text = "마인크래프트";
    req.glossary = {"로블록스", "마인크래프트"};

    const std::string instr = system_instruction(req);
    REQUIRE(instr.find("\"로블록스\"") != std::string::npos);
    REQUIRE(instr.find("\"마인크래프트\"") != std::string::npos);
    REQUIRE(instr.find("Roblox") == std::string::npos);
    REQUIRE(instr.find("マインクラフト") == std::string::npos);
    REQUIRE(instr.find("Translation glossary") == std::string::npos);
}

TEST_CASE("every retry reason adds only a short correction and preserves request inputs")
{
    for (const std::string code : {"ja", "ja-JP", "ko", "fr", "en", "pt-BR", "pt-PT"}) {
        CAPTURE(code);
        TranslateRequest req;
        req.target_code = code;
        req.target_name = target_language_name(code);
        req.text = "오늘 로블록스 할까요?";
        req.context = {"이전 문맥"};
        req.glossary = {"로블록스"};
        const auto first = json::parse(build_translate_request(req));
        const std::string initial = system_instruction(req);
        REQUIRE(initial.find("Retranslate the source") == std::string::npos);

        std::string correction;
        for (const auto reason : {QualitySuspectReason::SourceCopy, QualitySuspectReason::NearCopy,
                                  QualitySuspectReason::HangulResidue}) {
            CAPTURE(static_cast<int>(reason));
            req.retry_reason = reason;
            const auto retry = json::parse(build_translate_request(req));
            const std::string instr = system_instruction(req);
            REQUIRE(instr.compare(0, initial.size(), initial) == 0);
            const std::string extra = instr.substr(initial.size());
            REQUIRE(extra.size() <= 128);
            REQUIRE(extra.find("Retranslate the source into " + req.target_name + " (" + code + ")") !=
                    std::string::npos);
            REQUIRE(extra.find("output only the corrected translation") != std::string::npos);
            if (correction.empty()) correction = extra;
            REQUIRE(extra == correction);
            REQUIRE(retry["contents"] == first["contents"]);
            REQUIRE(retry["generationConfig"] == first["generationConfig"]);
        }
    }
}

TEST_CASE("context stays out of the system instruction")
{
    TranslateRequest req;
    req.target_code = "ja";
    req.target_name = target_language_name(req.target_code);
    req.context = {"UNIQUE_CONTEXT_TOKEN_XYZ"};
    req.text = "hello";

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();
    std::string user = j["contents"][0]["parts"][0]["text"].get<std::string>();
    REQUIRE(instr.find("UNIQUE_CONTEXT_TOKEN_XYZ") == std::string::npos);
    REQUIRE(user.find("UNIQUE_CONTEXT_TOKEN_XYZ") != std::string::npos);
}

TEST_CASE("parse preserves finishReason and keeps MAX_TOKENS text as ok")
{
    TranslateResult truncated = parse_translate_response(
        R"({"candidates":[{"finishReason":"MAX_TOKENS","content":{"parts":[{"text":"部分"}]}}]})");
    REQUIRE(truncated.ok);
    REQUIRE(truncated.text == "部分");
    REQUIRE(truncated.finish_reason == "MAX_TOKENS");

    TranslateResult safety = parse_translate_response(
        R"({"candidates": [{"finishReason": "SAFETY"}]})");
    REQUIRE_FALSE(safety.ok);
    REQUIRE(safety.finish_reason == "SAFETY");
}
