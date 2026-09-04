#include <catch2/catch_test_macros.hpp>
#include "translate-protocol.hpp"
#include <nlohmann/json.hpp>

using namespace lt;
using nlohmann::json;

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

TEST_CASE("system instruction names the target language and code")
{
    TranslateRequest req;
    req.target_code = "ja";
    req.target_name = "Japanese";
    req.text = "hello";

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.find("Japanese (ja)") != std::string::npos);
}

TEST_CASE("system instruction demands translation-only output")
{
    TranslateRequest req;
    req.target_code = "fr";
    req.target_name = "French";
    req.text = "hi";

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.find("no quotes") != std::string::npos);
    REQUIRE(instr.find("explanations") != std::string::npos);
    REQUIRE(instr.find("echo") != std::string::npos);
}

TEST_CASE("system instruction handles input already in the target language")
{
    TranslateRequest req;
    req.target_code = "de";
    req.target_name = "German";
    req.text = "hi";

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.find("already") != std::string::npos);
    REQUIRE(instr.find("cleaned up as-is") != std::string::npos);
}

TEST_CASE("system instruction adds the length hint when max_chars is positive")
{
    TranslateRequest req;
    req.target_code = "ja";
    req.target_name = "Japanese";
    req.text = "hello";
    req.max_chars = 80;

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.find("80 characters") != std::string::npos);
    REQUIRE(instr.find("Keep the translation within about 80 characters when possible; "
                        "prefer shorter wording over dropping meaning.") != std::string::npos);
    // Existing behaviour must still hold when max_chars is set.
    REQUIRE(instr.find("Japanese (ja)") != std::string::npos);
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
    req.target_name = "Japanese";
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
    req.target_name = "Japanese";
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
    req.target_name = "Spanish";
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
    req.target_name = "Korean";
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
    req.target_name = "Italian";
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
    req.target_name = "Portuguese";
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
    req.target_name = "Korean";
    req.text = "hello";
    req.glossary = {"Gemini", "OBS Studio", "곽민규"};

    std::string body = build_translate_request(req);
    json j = json::parse(body);
    std::string instr = j["system_instruction"]["parts"][0]["text"].get<std::string>();

    REQUIRE(instr.find("Glossary") != std::string::npos);
    REQUIRE(instr.find("\"Gemini\"") != std::string::npos);
    REQUIRE(instr.find("\"OBS Studio\"") != std::string::npos);
    REQUIRE(instr.find("\"곽민규\"") != std::string::npos);
    REQUIRE(instr.find("keep its spelling exactly as listed") != std::string::npos);
    // The glossary belongs to the instruction, not to the text being translated.
    std::string user = j["contents"][0]["parts"][0]["text"].get<std::string>();
    REQUIRE(user.find("Gemini") == std::string::npos);
}

TEST_CASE("no glossary sentence without terms")
{
    TranslateRequest req;
    req.target_code = "ko";
    req.target_name = "Korean";
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
