#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Live API message builders/parsers for the caption (STT) session that talks to
// models/gemini-3.5-transcribe-live. Pure functions, no libobs dependency.
// Spec: docs/specs/001-caption-translation-pipeline.md §3, §4.3, success criterion 5.
namespace lt {

constexpr const char *kCaptionModel = "models/gemini-3.5-transcribe-live";

// Builds the {"realtimeInput": {"audio": {...}}} JSON that carries one chunk of
// 16 kHz mono S16LE PCM, base64-encoded, to the Live API.
std::string build_realtime_input_message(const uint8_t *pcm, size_t len);

struct CaptionSetupOptions {
    // BCP-47 hints for inputAudioTranscription.languageCodes. Empty = auto-detect
    // (the default; the plugin never sets this in v2).
    std::vector<std::string> language_codes;
    // inputAudioTranscription.customVocabulary. Empty = field omitted.
    std::vector<std::string> custom_vocabulary;
    // true -> inputAudioTranscription.mode = "SMART" (disfluency cleanup,
    // punctuation/capitalization). false -> field omitted.
    bool smart_mode = true;
};

// Builds the {"setup": {...}} JSON sent right after the WebSocket opens:
//   setup.model = kCaptionModel
//   setup.generationConfig.responseModalities = ["TEXT"]
//   setup.inputAudioTranscription.languageCodes = [] (always present)
//   setup.inputAudioTranscription.customVocabulary (only when non-empty)
//   setup.inputAudioTranscription.mode = "SMART" (only when smart_mode)
std::string build_caption_setup_message(const CaptionSetupOptions &opts);

struct CaptionServerMessage {
    enum class Kind {
        Interim,       // serverContent.interimInputTranscription.text
        Final,         // serverContent.inputTranscription.text (speaker paused)
        SetupComplete, // {"setupComplete": {...}}
        Error,         // {"error": {"message": ...}}
        Other          // anything else, including unparsable JSON
    };
    Kind kind = Kind::Other;
    std::string text;          // Interim / Final transcript text
    std::string error_message; // Error
};

// If a message carries both a non-empty inputTranscription and an interim, the
// Final wins. Empty transcript strings are reported as Other.
CaptionServerMessage parse_caption_server_message(const std::string &json_text);

}
