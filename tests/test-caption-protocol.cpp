#include <catch2/catch_test_macros.hpp>
#include "caption-protocol.hpp"
#include <nlohmann/json.hpp>

using namespace lt;
using nlohmann::json;

TEST_CASE("caption setup message has model, responseModalities, and empty languageCodes by default")
{
    CaptionSetupOptions opts;
    std::string msg = build_caption_setup_message(opts);
    json j = json::parse(msg);
    REQUIRE(j["setup"]["model"] == "models/gemini-3.5-transcribe-live");
    REQUIRE(j["setup"]["generationConfig"]["responseModalities"].size() == 1);
    REQUIRE(j["setup"]["generationConfig"]["responseModalities"][0] == "TEXT");
    REQUIRE(j["setup"]["inputAudioTranscription"]["languageCodes"].is_array());
    REQUIRE(j["setup"]["inputAudioTranscription"]["languageCodes"].empty());
}

TEST_CASE("caption setup message carries non-empty languageCodes when provided")
{
    CaptionSetupOptions opts;
    opts.language_codes = {"en-US", "ko-KR"};
    std::string msg = build_caption_setup_message(opts);
    json j = json::parse(msg);
    REQUIRE(j["setup"]["inputAudioTranscription"]["languageCodes"].size() == 2);
    REQUIRE(j["setup"]["inputAudioTranscription"]["languageCodes"][0] == "en-US");
    REQUIRE(j["setup"]["inputAudioTranscription"]["languageCodes"][1] == "ko-KR");
}

TEST_CASE("caption setup message omits customVocabulary when empty")
{
    CaptionSetupOptions opts;
    std::string msg = build_caption_setup_message(opts);
    json j = json::parse(msg);
    REQUIRE_FALSE(j["setup"]["inputAudioTranscription"].contains("customVocabulary"));
}

TEST_CASE("caption setup message includes customVocabulary when non-empty")
{
    CaptionSetupOptions opts;
    opts.custom_vocabulary = {"OBS", "Gemini"};
    std::string msg = build_caption_setup_message(opts);
    json j = json::parse(msg);
    REQUIRE(j["setup"]["inputAudioTranscription"]["customVocabulary"].size() == 2);
    REQUIRE(j["setup"]["inputAudioTranscription"]["customVocabulary"][0] == "OBS");
}

TEST_CASE("caption setup message sets mode SMART when smart_mode is true")
{
    CaptionSetupOptions opts;
    opts.smart_mode = true;
    std::string msg = build_caption_setup_message(opts);
    json j = json::parse(msg);
    REQUIRE(j["setup"]["inputAudioTranscription"]["mode"] == "SMART");
}

TEST_CASE("caption setup message omits mode when smart_mode is false")
{
    CaptionSetupOptions opts;
    opts.smart_mode = false;
    std::string msg = build_caption_setup_message(opts);
    json j = json::parse(msg);
    REQUIRE_FALSE(j["setup"]["inputAudioTranscription"].contains("mode"));
}

TEST_CASE("parse caption message returns Other for invalid JSON")
{
    CaptionServerMessage m = parse_caption_server_message("not json {{{");
    REQUIRE(m.kind == CaptionServerMessage::Kind::Other);
}

TEST_CASE("parse caption message extracts Error with message")
{
    std::string s = R"({"error": {"code": 401, "message": "API key not valid"}})";
    CaptionServerMessage m = parse_caption_server_message(s);
    REQUIRE(m.kind == CaptionServerMessage::Kind::Error);
    REQUIRE(m.error_message == "API key not valid");
}

TEST_CASE("parse caption message recognizes setupComplete")
{
    CaptionServerMessage m = parse_caption_server_message(R"({"setupComplete": {}})");
    REQUIRE(m.kind == CaptionServerMessage::Kind::SetupComplete);
}

TEST_CASE("parse caption message extracts Final text from inputTranscription")
{
    std::string s = R"({"serverContent": {"inputTranscription": {"text": "hello world"}}})";
    CaptionServerMessage m = parse_caption_server_message(s);
    REQUIRE(m.kind == CaptionServerMessage::Kind::Final);
    REQUIRE(m.text == "hello world");
}

TEST_CASE("parse caption message extracts Interim text from interimInputTranscription")
{
    std::string s = R"({"serverContent": {"interimInputTranscription": {"text": "hel"}}})";
    CaptionServerMessage m = parse_caption_server_message(s);
    REQUIRE(m.kind == CaptionServerMessage::Kind::Interim);
    REQUIRE(m.text == "hel");
}

TEST_CASE("parse caption message prefers Final when both final and interim are present")
{
    std::string s = R"({"serverContent": {
        "inputTranscription": {"text": "final text"},
        "interimInputTranscription": {"text": "interim text"}
    }})";
    CaptionServerMessage m = parse_caption_server_message(s);
    REQUIRE(m.kind == CaptionServerMessage::Kind::Final);
    REQUIRE(m.text == "final text");
}

TEST_CASE("parse caption message treats empty final text with non-empty interim as Interim")
{
    std::string s = R"({"serverContent": {
        "inputTranscription": {"text": ""},
        "interimInputTranscription": {"text": "still talking"}
    }})";
    CaptionServerMessage m = parse_caption_server_message(s);
    REQUIRE(m.kind == CaptionServerMessage::Kind::Interim);
    REQUIRE(m.text == "still talking");
}

TEST_CASE("parse caption message returns Other when transcription text is empty")
{
    std::string s = R"({"serverContent": {"inputTranscription": {"text": ""}}})";
    CaptionServerMessage m = parse_caption_server_message(s);
    REQUIRE(m.kind == CaptionServerMessage::Kind::Other);
}

TEST_CASE("parse caption message returns Other when transcription text is missing")
{
    std::string s = R"({"serverContent": {"inputTranscription": {}}})";
    CaptionServerMessage m = parse_caption_server_message(s);
    REQUIRE(m.kind == CaptionServerMessage::Kind::Other);
}

TEST_CASE("parse caption message returns Other for unrelated serverContent fields")
{
    std::string s = R"({"serverContent": {"turnComplete": true}})";
    CaptionServerMessage m = parse_caption_server_message(s);
    REQUIRE(m.kind == CaptionServerMessage::Kind::Other);
}
