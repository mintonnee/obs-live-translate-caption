#include "audio-convert.hpp"
#include "languages.hpp"
#include "translation-session.hpp"
#include <cstring>
#include <media-io/audio-resampler.h>
#include <obs-module.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace {

constexpr const char *kFilterId = "gemini_live_translate_filter";
constexpr double kMinPlaybackDelaySeconds = 0.0;
constexpr double kMaxPlaybackDelaySeconds = 30.0;

struct FilterData {
    obs_source_t *context = nullptr;
    audio_resampler_t *resampler = nullptr;
    lt::Chunker chunker{3200};
    std::string api_key;
    std::string target_lang = "en";
    bool echo_target = true;
    uint32_t playback_delay_ms = 0;
    bool active = false; // true iff this filter is the primary one feeding the session
};

uint32_t delay_seconds_to_ms(double seconds)
{
    double clamped = std::clamp(seconds, kMinPlaybackDelaySeconds,
                               kMaxPlaybackDelaySeconds);
    return static_cast<uint32_t>(std::lround(clamped * 1000.0));
}

const char *filter_get_name(void *)
{
    return obs_module_text("Gemini Live Translate");
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
    d->echo_target = obs_data_get_bool(settings, "echo_target");
    d->playback_delay_ms =
        delay_seconds_to_ms(obs_data_get_double(settings, "playback_delay"));

    auto &session = lt::TranslationSession::instance();
    bool primary = is_primary_filter(d);
    if (primary)
        session.set_output_delay_ms(d->playback_delay_ms);
    bool run = !d->api_key.empty() && primary;
    if (run) {
        d->active = true;
        session.configure(d->api_key, d->target_lang, d->echo_target);
    } else if (d->active) {
        // We were the primary feeding the session but no longer should be
        // (key cleared, or another filter is now first). Stop it; a remaining
        // primary filter restarts it from its own audio callback.
        d->active = false;
        session.stop();
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
    if (d->active) lt::TranslationSession::instance().stop();
    if (d->resampler) audio_resampler_destroy(d->resampler);
    delete d;
}

struct obs_audio_data *filter_audio(void *data, struct obs_audio_data *audio)
{
    auto *d = static_cast<FilterData *>(data);
    if (!d->resampler || d->api_key.empty()) return audio;

    auto &session = lt::TranslationSession::instance();
    // Only the primary (first) Gemini filter on this source feeds the session;
    // a duplicate on the same source stays a passive pass-through. Recomputed
    // live each callback, so when the primary is removed this one takes over.
    bool primary = is_primary_filter(d);
    bool became_primary = primary && !d->active;
    if (primary != d->active) {
        d->active = primary;
        if (primary)
            session.set_output_delay_ms(d->playback_delay_ms);
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
    bool needs_start =
        !session.is_running() && session.status() != lt::ConnStatus::AuthError;
    if (became_primary || needs_start)
        session.configure(d->api_key, d->target_lang, d->echo_target);

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
            session.push_input_pcm(c.data(), c.size());
    }
    return audio;
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

    obs_properties_add_bool(
        props, "echo_target",
        obs_module_text("Output speech even when it is already in the target "
                        "language (otherwise stays silent)"));
    obs_properties_add_float_slider(
        props, "playback_delay",
        obs_module_text("Playback Delay (seconds)"),
        kMinPlaybackDelaySeconds, kMaxPlaybackDelaySeconds, 0.1);

    obs_properties_add_text(props, "api_key", obs_module_text("Gemini API Key"),
                            OBS_TEXT_PASSWORD);
    obs_properties_add_text(
        props, "warn",
        obs_module_text("Note: the API key is stored in plaintext in your "
                        "scene collection file. Do not share that file."),
        OBS_TEXT_INFO);
    auto *d = static_cast<FilterData *>(data);
    std::string status =
        (d && !is_primary_filter(d))
            ? obs_module_text("Another Gemini Live Translate filter on this "
                              "source is already active; this one is disabled.")
            : lt::TranslationSession::instance().status_text();
    obs_properties_add_text(props, "status", status.c_str(), OBS_TEXT_INFO);
    return props;
}

void filter_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "target_lang", "en");
    obs_data_set_default_bool(settings, "echo_target", true);
    obs_data_set_default_double(settings, "playback_delay", 0.0);
}

void filter_get_status(void *, obs_data_t *settings)
{
    obs_data_set_string(
        settings, "status",
        lt::TranslationSession::instance().status_text().c_str());
}

} // namespace

struct obs_source_info live_translate_filter_info = [] {
    struct obs_source_info info = {};
    info.id = "gemini_live_translate_filter";
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
