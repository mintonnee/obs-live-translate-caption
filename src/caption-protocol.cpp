#include "caption-protocol.hpp"
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace lt {

std::string build_caption_setup_message(const CaptionSetupOptions &opts)
{
    json j;
    j["setup"]["model"] = kCaptionModel;
    j["setup"]["generationConfig"]["responseModalities"] = json::array({"TEXT"});
    j["setup"]["inputAudioTranscription"]["languageCodes"] = opts.language_codes;
    if (!opts.custom_vocabulary.empty())
        j["setup"]["inputAudioTranscription"]["customVocabulary"] = opts.custom_vocabulary;
    if (opts.smart_mode)
        j["setup"]["inputAudioTranscription"]["mode"] = "SMART";
    return j.dump();
}

CaptionServerMessage parse_caption_server_message(const std::string &json_text)
{
    CaptionServerMessage m;
    json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded()) {
        m.kind = CaptionServerMessage::Kind::Other;
        return m;
    }

    if (j.contains("error") && j["error"].is_object()) {
        m.kind = CaptionServerMessage::Kind::Error;
        m.error_message = j["error"].value("message", "");
        return m;
    }

    if (j.contains("setupComplete")) {
        m.kind = CaptionServerMessage::Kind::SetupComplete;
        return m;
    }

    if (j.contains("serverContent") && j["serverContent"].is_object()) {
        const auto &sc = j["serverContent"];

        std::string final_text;
        if (sc.contains("inputTranscription") && sc["inputTranscription"].is_object()) {
            const auto &it = sc["inputTranscription"];
            if (it.contains("text") && it["text"].is_string())
                final_text = it["text"].get<std::string>();
        }
        if (!final_text.empty()) {
            m.kind = CaptionServerMessage::Kind::Final;
            m.text = final_text;
            return m;
        }

        std::string interim_text;
        if (sc.contains("interimInputTranscription") &&
            sc["interimInputTranscription"].is_object()) {
            const auto &it = sc["interimInputTranscription"];
            if (it.contains("text") && it["text"].is_string())
                interim_text = it["text"].get<std::string>();
        }
        if (!interim_text.empty()) {
            m.kind = CaptionServerMessage::Kind::Interim;
            m.text = interim_text;
            return m;
        }

        m.kind = CaptionServerMessage::Kind::Other;
        return m;
    }

    m.kind = CaptionServerMessage::Kind::Other;
    return m;
}

}
