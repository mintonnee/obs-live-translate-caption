#include "audio-convert.hpp"
#include "caption-output.hpp"
#include "caption-session.hpp"
#include "translate-protocol.hpp"
#include "languages.hpp"
#include <cstring>
#include <media-io/audio-resampler.h>
#include <obs-module.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

// Distinct from the original plugin's "gemini_live_translate_filter" so the two
// never collide or get mixed up; filters saved by the original are not ours.
constexpr const char *kFilterId = "gemini_translate_caption_filter";
// Caption text box (spec 002 §4.1). `caption_max_lines` supersedes the legacy
// `caption_max_segments` key, which is still read for old scene collections.
constexpr int kMinCaptionLines = 1;
constexpr int kMaxCaptionLines = 6;
constexpr int kDefaultCaptionLines = 2;
constexpr int kMinCaptionWidth = 10;
constexpr int kMaxCaptionWidth = 120;
constexpr int kDefaultCaptionWidth = 60;
constexpr double kMinCaptionHoldSeconds = 1.0;
constexpr double kMaxCaptionHoldSeconds = 30.0;
// Translation models offered in the properties combo (the field stays editable).
constexpr const char *kTranslateModelChoices[] = {
    "gemini-3.1-flash-lite", "gemini-3.5-flash-lite", "gemini-3.5-flash",
    "gemini-flash-lite-latest", "gemini-flash-latest",
};

struct FilterData {
    obs_source_t *context = nullptr;
    audio_resampler_t *resampler = nullptr;
    lt::Chunker chunker{3200};
    std::string api_key;
    std::string target_lang = "en";
    std::string caption_text_source;
    std::string caption_source_text_source;
    int caption_max_lines = kDefaultCaptionLines;
    int caption_max_width = kDefaultCaptionWidth;
    double caption_hold_seconds = 4.0;
    std::string caption_custom_vocabulary;
    std::string translate_model; // generateContent model id, "" = kTranslateModel
    bool caption_active = false; // true iff this filter drives the caption session
};

const char *filter_get_name(void *)
{
    return obs_module_text("Gemini Translate Caption");
}

// English name for the translation prompt: the display names in languages.hpp
// are "<endonym> (<English name>)" for non-English languages and a plain
// English name otherwise. Unknown codes fall back to the code itself.
std::string target_language_name(const std::string &code)
{
    for (int i = 0; i < lt::kLanguagesCount; ++i) {
        if (code != lt::kLanguages[i].code) continue;
        std::string name = lt::kLanguages[i].name;
        size_t close = name.rfind(')');
        if (close != std::string::npos) {
            size_t open = name.rfind('(', close);
            if (open != std::string::npos && close > open + 1)
                return name.substr(open + 1, close - open - 1);
        }
        return name;
    }
    return code;
}

// "foo, bar ,, baz" -> {"foo", "bar", "baz"}
std::vector<std::string> split_custom_vocabulary(const std::string &raw)
{
    static const char *kSpace = " \t\r\n";
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t comma = raw.find(',', start);
        size_t end = (comma == std::string::npos) ? raw.size() : comma;
        size_t b = raw.find_first_not_of(kSpace, start);
        if (b != std::string::npos && b < end) {
            size_t e = raw.find_last_not_of(kSpace, end - 1);
            out.push_back(raw.substr(b, e - b + 1));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

lt::CaptionConfig make_caption_config(const FilterData *d)
{
    lt::CaptionConfig cfg;
    cfg.api_key = d->api_key;
    cfg.target_lang = d->target_lang;
    cfg.target_name = target_language_name(d->target_lang);
    cfg.custom_vocabulary = split_custom_vocabulary(d->caption_custom_vocabulary);
    cfg.translate_model = d->translate_model;
    cfg.max_lines = d->caption_max_lines;
    cfg.max_width = d->caption_max_width;
    cfg.hold_seconds = d->caption_hold_seconds;
    return cfg;
}

// (Re)installs the display sinks for the names this filter currently targets.
// Done on every update and on takeover, because the names may have changed or
// may still belong to a filter that is no longer primary.
void install_caption_sinks(const FilterData *d)
{
    std::string caption_name = d->caption_text_source;
    std::string source_name = d->caption_source_text_source;
    lt::CaptionSession::TextSink caption_sink =
        [caption_name](const std::string &text) {
            lt::caption_output_write(caption_name, text);
        };
    lt::CaptionSession::TextSink source_sink;
    if (!source_name.empty())
        source_sink = [source_name](const std::string &text) {
            lt::caption_output_write(source_name, text);
        };
    lt::CaptionSession::instance().set_sinks(std::move(caption_sink),
                                             std::move(source_sink));
}

// stop() resets the status to Idle, which would hide an API key error from the
// properties panel. A settings change that is not about the key must not wipe
// that message, so leave an auth-stopped session alone (spec criterion 7).
void stop_caption_session_for_settings()
{
    auto &session = lt::CaptionSession::instance();
    if (!session.is_running() && session.status() == lt::ConnStatus::AuthError)
        return;
    session.stop();
}

struct PrimaryScan {
    obs_source_t *first; // first Gemini filter on the parent, in filter order
};

void primary_scan_cb(obs_source_t * /*parent*/, obs_source_t *child, void *param)
{
    auto *s = static_cast<PrimaryScan *>(param);
    if (!s->first && std::strcmp(obs_source_get_id(child), kFilterId) == 0)
        s->first = child;
}

// Per-source first-wins: a filter is "primary" iff it is the first Gemini Live
// Translate filter on its parent source. A second copy on the SAME source is
// not primary (it gets disabled + warned). This queries live OBS state on every
// call, so there is no persistent owner that can go stale. Note: this does NOT
// coordinate across different sources — the shared session has a single stream,
// so putting the filter on two different sources is unsupported (see README).
bool is_primary_filter(FilterData *d)
{
    obs_source_t *parent = obs_filter_get_parent(d->context);
    if (!parent) return true; // not attached yet; don't show a false warning
    PrimaryScan scan{nullptr};
    obs_source_enum_filters(parent, primary_scan_cb, &scan);
    return scan.first == nullptr || scan.first == d->context;
}

void create_resampler(FilterData *d)
{
    if (d->resampler) {
        audio_resampler_destroy(d->resampler);
        d->resampler = nullptr;
    }
    audio_t *audio = obs_get_audio();
    struct resample_info src = {};
    src.samples_per_sec = audio_output_get_sample_rate(audio);
    src.format = AUDIO_FORMAT_FLOAT_PLANAR;
    src.speakers = audio_output_get_channels(audio) == 1 ? SPEAKERS_MONO
                                                          : SPEAKERS_STEREO;
    struct resample_info dst = {};
    dst.samples_per_sec = 16000;
    dst.format = AUDIO_FORMAT_16BIT;
    dst.speakers = SPEAKERS_MONO;
    d->resampler = audio_resampler_create(&dst, &src);
}

void filter_update(void *data, obs_data_t *settings)
{
    auto *d = static_cast<FilterData *>(data);
    d->api_key = obs_data_get_string(settings, "api_key");
    d->target_lang = obs_data_get_string(settings, "target_lang");
    std::string caption_text_source =
        obs_data_get_string(settings, "caption_text_source");
    std::string caption_source_text_source =
        obs_data_get_string(settings, "caption_source_text_source");

    // A name we no longer target keeps whatever text was written to it last;
    // blank it so a stale caption does not linger on screen.
    if (!d->caption_text_source.empty() &&
        d->caption_text_source != caption_text_source)
        lt::caption_output_write(d->caption_text_source, "");
    if (!d->caption_source_text_source.empty() &&
        d->caption_source_text_source != caption_source_text_source)
        lt::caption_output_write(d->caption_source_text_source, "");

    d->caption_text_source = caption_text_source;
    d->caption_source_text_source = caption_source_text_source;
    // Scene collections written before spec 002 only carry the legacy segment
    // count; read it as the line count when the new key was never stored
    // (spec 002 §4.1, criterion 12). filter_defaults deliberately registers no
    // default for the legacy key, so obs_data_has_user_value can tell a stored
    // value from an absent one.
    long long lines = kDefaultCaptionLines;
    if (obs_data_has_user_value(settings, "caption_max_lines"))
        lines = obs_data_get_int(settings, "caption_max_lines");
    else if (obs_data_has_user_value(settings, "caption_max_segments"))
        lines = obs_data_get_int(settings, "caption_max_segments");
    d->caption_max_lines = static_cast<int>(
        std::clamp<long long>(lines, kMinCaptionLines, kMaxCaptionLines));
    d->caption_max_width = static_cast<int>(std::clamp<long long>(
        obs_data_get_int(settings, "caption_max_chars_per_line"),
        kMinCaptionWidth, kMaxCaptionWidth));
    d->caption_hold_seconds =
        std::clamp(obs_data_get_double(settings, "caption_hold_seconds"),
                   kMinCaptionHoldSeconds, kMaxCaptionHoldSeconds);
    d->translate_model = obs_data_get_string(settings, "translate_model");
    d->caption_custom_vocabulary =
        obs_data_get_string(settings, "caption_custom_vocabulary");

    bool run = !d->api_key.empty() && is_primary_filter(d);
    if (run) {
        d->caption_active = true;
        // Re-install every update: the target names may have changed.
        install_caption_sinks(d);
        lt::CaptionSession::instance().configure(make_caption_config(d));
    } else if (d->caption_active) {
        // Key cleared, or another filter is now first. A remaining primary
        // filter restarts the session from its own audio callback.
        d->caption_active = false;
        stop_caption_session_for_settings();
    }
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
    auto *d = new FilterData();
    d->context = source;
    create_resampler(d);
    filter_update(d, settings);
    return d;
}

void filter_destroy(void *data)
{
    auto *d = static_cast<FilterData *>(data);
    // If we were feeding the session, stop it. A remaining same-source filter
    // (if any) becomes primary and restarts it from its next audio callback.
    if (d->caption_active) {
        lt::CaptionSession::instance().stop();
        // stop() already blanks the sources through our sinks; clear the names
        // directly as well, in case the sinks were replaced meanwhile. Only the
        // driving filter does this, so a passive duplicate cannot wipe the
        // captions of the filter that is actually running.
        if (!d->caption_text_source.empty())
            lt::caption_output_write(d->caption_text_source, "");
        if (!d->caption_source_text_source.empty())
            lt::caption_output_write(d->caption_source_text_source, "");
    }
    if (d->resampler) audio_resampler_destroy(d->resampler);
    delete d;
}

struct obs_audio_data *filter_audio(void *data, struct obs_audio_data *audio)
{
    auto *d = static_cast<FilterData *>(data);
    if (!d->resampler || d->api_key.empty()) return audio;

    auto &captions = lt::CaptionSession::instance();
    // Only the primary (first) Gemini filter on this source feeds the session;
    // a duplicate on the same source stays a passive pass-through. Recomputed
    // live each callback, so when the primary is removed this one takes over.
    bool primary = is_primary_filter(d);
    bool became_primary = primary && !d->caption_active;
    if (primary != d->caption_active) {
        d->caption_active = primary;
        // Primary status changed: refresh the (possibly open) properties panel
        // so a stale "disabled" warning clears. Safe from the audio thread —
        // OBS marshals the refresh to the UI thread.
        obs_source_update_properties(d->context);
    }
    if (!primary) return audio;

    // (Re)configure when we take over, and also if the session was cleanly
    // stopped while we are primary — e.g. the previous primary's teardown
    // raced our takeover and stopped the session right after we claimed it.
    // Skip when it stopped on an auth error, so a bad key doesn't spin-restart.
    // configure() only spawns the worker (or reaps an already-exited one); it
    // never joins a live connection, so it won't stall the audio callback.
    bool needs_start = !captions.is_running() &&
                       captions.status() != lt::ConnStatus::AuthError;
    if (became_primary || needs_start) {
        // The sinks may still be the previous primary's; claim them.
        install_caption_sinks(d);
        captions.configure(make_caption_config(d));
    }

    uint8_t *out[MAX_AV_PLANES] = {};
    uint32_t out_frames = 0;
    uint64_t ts_offset = 0;
    bool ok = audio_resampler_resample(
        d->resampler, out, &out_frames, &ts_offset,
        (const uint8_t *const *)audio->data, audio->frames);
    if (ok && out_frames > 0) {
        // Send the audio continuously, including silence, the way the official
        // Live API client streams the mic. Gating out silence made our input
        // stream discontinuous, which disrupts the model's real-time VAD/turn
        // handling and delays translation output until the next speech resumes.
        auto chunks = d->chunker.push(out[0], out_frames * 2);
        for (auto &c : chunks)
            captions.push_input_pcm(c.data(), c.size());
    }
    return audio;
}

struct TextSourceLists {
    obs_property_t *caption;
    obs_property_t *source;
};

bool add_text_source_cb(void *param, obs_source_t *source)
{
    const char *id = obs_source_get_unversioned_id(source);
    if (!id) return true;
    // Windows uses text_gdiplus, macOS/Linux text_ft2_source. The versioned id
    // (text_gdiplus_v3) is not usable for this check.
    if (std::strcmp(id, "text_gdiplus") != 0 &&
        std::strcmp(id, "text_ft2_source") != 0)
        return true;
    const char *name = obs_source_get_name(source);
    if (!name || !*name) return true;
    auto *lists = static_cast<TextSourceLists *>(param);
    obs_property_list_add_string(lists->caption, name, name);
    obs_property_list_add_string(lists->source, name, name);
    return true;
}

std::string filter_status_text(FilterData *d)
{
    if (d && !is_primary_filter(d))
        return obs_module_text("Another Gemini Translate Caption filter on this "
                               "source is already active; this one is disabled.");
    std::string missing = lt::caption_output_missing_source();
    if (!missing.empty())
        return "Caption text source \"" + missing + "\" not found";
    if (d && d->caption_text_source.empty())
        return obs_module_text("Set a caption text source to show captions");
    return lt::CaptionSession::instance().status_text();
}

obs_properties_t *filter_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();

    obs_property_t *list = obs_properties_add_list(
        props, "target_lang", obs_module_text("Target Language"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    for (int i = 0; i < lt::kLanguagesCount; ++i)
        obs_property_list_add_string(list, lt::kLanguages[i].name,
                                     lt::kLanguages[i].code);

    // Editable combos: the text source may not exist yet, so its name can be
    // typed in before the source is created (spec §4.1).
    TextSourceLists lists = {};
    lists.caption = obs_properties_add_list(
        props, "caption_text_source", obs_module_text("Caption Text Source"),
        OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    lists.source = obs_properties_add_list(
        props, "caption_source_text_source",
        obs_module_text("Source Transcript Text Source (optional)"),
        OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(lists.caption, obs_module_text("(none)"), "");
    obs_property_list_add_string(lists.source, obs_module_text("(none)"), "");
    obs_enum_sources(add_text_source_cb, &lists);

    obs_properties_add_int_slider(
        props, "caption_max_lines", obs_module_text("Caption Lines"),
        kMinCaptionLines, kMaxCaptionLines, 1);
    obs_properties_add_int_slider(
        props, "caption_max_chars_per_line",
        obs_module_text("Max Characters per Line (CJK count as 2)"),
        kMinCaptionWidth, kMaxCaptionWidth, 1);
    obs_properties_add_float_slider(
        props, "caption_hold_seconds",
        obs_module_text("Caption Hold (seconds)"), kMinCaptionHoldSeconds,
        kMaxCaptionHoldSeconds, 0.5);
    obs_properties_add_text(
        props, "caption_custom_vocabulary",
        obs_module_text("Custom Vocabulary (comma-separated)"), OBS_TEXT_DEFAULT);

    // Editable so a model id that is not listed yet can be typed in. Ids come
    // from the Gemini model docs; availability depends on the account.
    obs_property_t *model_list = obs_properties_add_list(
        props, "translate_model", obs_module_text("Translation Model"),
        OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    for (const char *id : kTranslateModelChoices)
        obs_property_list_add_string(model_list, id, id);

    obs_properties_add_text(props, "api_key", obs_module_text("Gemini API Key"),
                            OBS_TEXT_PASSWORD);
    obs_properties_add_text(
        props, "warn",
        obs_module_text("Note: the API key is stored in plaintext in your "
                        "scene collection file. Do not share that file."),
        OBS_TEXT_INFO);
    auto *d = static_cast<FilterData *>(data);
    std::string status = filter_status_text(d);
    obs_properties_add_text(props, "status", status.c_str(), OBS_TEXT_INFO);
    return props;
}

void filter_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "target_lang", "en");
    obs_data_set_default_string(settings, "caption_text_source", "");
    obs_data_set_default_string(settings, "caption_source_text_source", "");
    obs_data_set_default_int(settings, "caption_max_lines", kDefaultCaptionLines);
    obs_data_set_default_int(settings, "caption_max_chars_per_line",
                             kDefaultCaptionWidth);
    // No default for the legacy "caption_max_segments": filter_update relies on
    // obs_data_has_user_value() to detect a stored legacy value.
    obs_data_set_default_double(settings, "caption_hold_seconds", 4.0);
    obs_data_set_default_string(settings, "caption_custom_vocabulary", "");
    obs_data_set_default_string(settings, "translate_model", lt::kTranslateModel);
}

void filter_get_status(void *data, obs_data_t *settings)
{
    obs_data_set_string(
        settings, "status",
        filter_status_text(static_cast<FilterData *>(data)).c_str());
}

} // namespace

struct obs_source_info live_translate_filter_info = [] {
    struct obs_source_info info = {};
    info.id = kFilterId;
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = filter_get_name;
    info.create = filter_create;
    info.destroy = filter_destroy;
    info.update = filter_update;
    info.filter_audio = filter_audio;
    info.get_properties = filter_properties;
    info.get_defaults = filter_defaults;
    return info;
}();
