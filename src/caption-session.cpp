#include "caption-session.hpp"
#include "audio-convert.hpp"
#include "backoff.hpp"
#include "caption-protocol.hpp"
#include "caption-wrap.hpp"
#include "pause-policy.hpp"
#include "translate-protocol.hpp"
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXWebSocket.h>
#include <obs.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

// Caption (STT + translate) session. A singleton with its own worker threads and
// reconnect backoff, driving a text pipeline:
// WebSocket -> transcripts -> translate job queue -> CaptionComposer -> sinks.
// Spec: docs/specs/001-caption-translation-pipeline.md §4.3, §4.4, §4.7;
// docs/specs/002-caption-text-box-limits.md §4.4, §4.5, criteria 8, 9;
// docs/specs/004-idle-pause-and-output-gating.md §4.4, criteria 8, 9, 11, 12
// (auto-pause on a silent mic or an inactive OBS output).

namespace lt {

namespace {

uint64_t now_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t elapsed_ms(uint64_t since, uint64_t now)
{
    return now >= since ? now - since : 0;
}

CaptionPipelineConfig pipeline_config(const CaptionConfig &cfg)
{
    CaptionPipelineConfig result;
    result.translation = {cfg.target_lang, cfg.target_name, cfg.translate_model,
        cfg.translation_vocabulary.empty() ? cfg.custom_vocabulary : cfg.translation_vocabulary};
    result.display.max_lines = cfg.max_lines;
    result.display.max_width = cfg.max_width;
    const double hold = std::isfinite(cfg.hold_seconds) ? cfg.hold_seconds : 4.0;
    result.display.hold_ms = static_cast<uint64_t>(std::clamp(hold * 1000.0, 1000.0, 30000.0));
    result.incremental = cfg.incremental;
    return result;
}

} // namespace

CaptionSession &CaptionSession::instance()
{
    static CaptionSession s;
    return s;
}

CaptionSession::CaptionSession()
{
    // Seed the detector from the CaptionConfig defaults so audio pushed before
    // the first configure() is judged with the values the UI already shows.
    idle_threshold_rms_.store(dbfs_to_rms(cfg_.idle_threshold_dbfs));
    idle_detector_.configure(
        static_cast<uint64_t>(cfg_.idle_timeout_seconds) * 1000);
}

CaptionSession::~CaptionSession() { stop(); }

void CaptionSession::set_status(ConnStatus s, const std::string &detail)
{
    std::lock_guard<std::mutex> lk(status_mtx_);
    status_ = s;
    status_detail_ = detail;
}

ConnStatus CaptionSession::status()
{
    std::lock_guard<std::mutex> lk(status_mtx_);
    return status_;
}

bool CaptionSession::is_running()
{
    return running_.load();
}

std::string CaptionSession::status_text()
{
    std::lock_guard<std::mutex> lk(status_mtx_);
    switch (status_) {
    case ConnStatus::Idle:
        return "Idle";
    case ConnStatus::Connecting:
        return "Connecting...";
    case ConnStatus::Connected:
        return "Connected";
    case ConnStatus::Reconnecting:
        return "Reconnecting...";
    case ConnStatus::AuthError:
        return "API key error: " + status_detail_;
    case ConnStatus::Paused:
        return "Paused (" + status_detail_ + ")";
    }
    return "Unknown";
}

void CaptionSession::set_output_active(bool active)
{
    output_active_.store(active);
}

void CaptionSession::set_sinks(TextSink caption_sink, TextSink source_sink)
{
    std::lock_guard<std::mutex> lk(out_mtx_);
    caption_sink_ = std::move(caption_sink);
    source_sink_ = std::move(source_sink);
}

void CaptionSession::configure(const CaptionConfig &cfg)
{
    if (cfg.api_key.empty()) { stop(); return; }
    std::lock_guard<std::mutex> lifecycle(lifecycle_mtx_);
    bool start = false;
    CaptionPipelineResult changes;
    {
        std::lock_guard<std::mutex> publication(publish_mtx_);
        std::lock_guard<std::mutex> state(out_mtx_);
        start = !running_.load();
        const bool reconnect = cfg_.api_key != cfg.api_key ||
            cfg_.target_lang != cfg.target_lang || cfg_.target_name != cfg.target_name ||
            cfg_.custom_vocabulary != cfg.custom_vocabulary;
        const bool semantic = reconnect || cfg_.translate_model != cfg.translate_model ||
            cfg_.translation_vocabulary != cfg.translation_vocabulary;
        const bool incremental_changed = cfg_.incremental != cfg.incremental;
        cfg_ = cfg;
        if (start || semantic) {
            changes = reset_pipeline_locked(start || reconnect);
        } else if (incremental_changed) {
            changes = pipeline_.set_incremental(cfg.incremental, now_ms());
            auto display = pipeline_.set_display_config(pipeline_config(cfg).display, now_ms());
            changes.cancel.insert(changes.cancel.end(), display.cancel.begin(), display.cancel.end());
            changes.events.insert(changes.events.end(), display.events.begin(), display.events.end());
        } else {
            changes = pipeline_.set_display_config(pipeline_config(cfg).display, now_ms());
        }
        apply_effects_locked(changes);
        if (reconnect) config_changed_ = true;
    }
    log_events(changes);
    // Reap an auth-stopped run while running is still false, without state locks.
    if (start) {
        job_cv_.notify_all();
        if (ws_thread_.joinable()) ws_thread_.join();
        for (auto &thread : translate_threads_) if (thread.joinable()) thread.join();
        translate_threads_.clear();
    }
    {
        idle_threshold_rms_.store(dbfs_to_rms(cfg.idle_threshold_dbfs));
        std::lock_guard<std::mutex> idle(idle_mtx_);
        idle_detector_.configure(static_cast<uint64_t>(std::max(0, cfg.idle_timeout_seconds)) * 1000);
        if (start) {
            idle_detector_.start_idle(now_ms());
            last_audio_ms_ = last_signal_ms_ = 0;
            audio_chunks_ = signal_chunks_ = 0;
            last_rms_ = peak_rms_ = 0.0;
        }
    }
    if (start) {
        running_ = true;
        for (size_t i = 0; i < kTranslateWorkers; ++i)
            translate_threads_.emplace_back([this] { translate_worker(); });
        ws_thread_ = std::thread([this] { run(); });
    }
    job_cv_.notify_all();
}

void CaptionSession::stop()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mtx_);
    CaptionPipelineResult changes;
    {
        std::lock_guard<std::mutex> publication(publish_mtx_);
        std::lock_guard<std::mutex> state(out_mtx_);
        running_ = false;
        changes = reset_pipeline_locked(true);
    }
    log_events(changes);
    job_cv_.notify_all();
    if (ws_thread_.joinable()) ws_thread_.join();
    for (auto &thread : translate_threads_) if (thread.joinable()) thread.join();
    translate_threads_.clear();
    input_.clear();
    {
        std::lock_guard<std::mutex> idle(idle_mtx_);
        idle_detector_.reset(now_ms());
    }
    announced_pause_detail_.clear();
    {
        std::lock_guard<std::mutex> publication(publish_mtx_);
        TextSink caption, source;
        {
            std::lock_guard<std::mutex> state(out_mtx_);
            caption = caption_sink_;
            source = source_sink_;
            source_shown_ = false;
            last_source_publish_ms_ = 0;
        }
        if (caption) caption("");
        if (source) source("");
    }
    set_status(ConnStatus::Idle);
}

CaptionPipelineResult CaptionSession::reset_pipeline_locked(bool invalidate_socket)
{
    // Never wrap an identifier back into a live identity.
    if (generation_ == std::numeric_limits<uint64_t>::max() ||
        (invalidate_socket && socket_epoch_ == std::numeric_limits<uint64_t>::max()))
        throw std::overflow_error("caption generation exhausted");
    if (invalidate_socket) ++socket_epoch_;
    auto result = pipeline_.reset(++generation_, pipeline_config(cfg_), now_ms());
    apply_effects_locked(result);
    pending_interim_.clear();
    has_pending_interim_ = false;
    interim_pending_ = false;
    last_interim_ms_ = 0;
    return result;
}

void CaptionSession::apply_effects_locked(const CaptionPipelineResult &result)
{
    for (const auto &cancel : result.cancel)
        for (const auto &active : active_requests_)
            if (active.id == cancel) active.args->cancel = true;
}

void CaptionSession::invalidate_connection(uint64_t epoch)
{
    CaptionPipelineResult changes;
    {
        std::lock_guard<std::mutex> publication(publish_mtx_);
        std::lock_guard<std::mutex> state(out_mtx_);
        if (epoch != socket_epoch_) return;
        changes = reset_pipeline_locked(true);
    }
    log_events(changes);
    job_cv_.notify_all();
}

void CaptionSession::log_events(const CaptionPipelineResult &result)
{
    for (const auto &event : result.events) {
        const bool ambiguous = event.kind == CaptionPipelineEventKind::ReconcileAmbiguous ||
            event.composer_reject == CaptionComposeReject::ReconcileAmbiguous;
        blog(ambiguous ? LOG_WARNING : LOG_DEBUG,
             "[live-translate] caption event=%d gen=%llu utterance=%llu segment=%llu "
             "revision=%llu attempt=%llu request_kind=%d reason=%d reject=%d compose_reject=%d "
             "failure=%d segment_wait_ms=%llu queue_wait_ms=%llu http_ms=%llu "
             "display_wait_ms=%llu first_seen_to_display_ms=%llu queue_depth=%zu count=%zu",
             static_cast<int>(event.kind), static_cast<unsigned long long>(event.id.segment.generation),
             static_cast<unsigned long long>(event.id.segment.utterance_id),
             static_cast<unsigned long long>(event.id.segment.segment_id),
             static_cast<unsigned long long>(event.id.source_revision),
             static_cast<unsigned long long>(event.id.attempt_id), static_cast<int>(event.request_kind),
             static_cast<int>(event.retire_reason), static_cast<int>(event.scheduler_reject),
             static_cast<int>(event.composer_reject), static_cast<int>(event.failure),
             static_cast<unsigned long long>(event.segment_wait_ms),
             static_cast<unsigned long long>(event.queue_wait_ms),
             static_cast<unsigned long long>(event.http_ms),
             static_cast<unsigned long long>(event.display_wait_ms),
             static_cast<unsigned long long>(event.first_seen_to_display_ms),
             event.queue_depth, event.count);
    }
}

void CaptionSession::push_input_pcm(const uint8_t *data, size_t len)
{
    // Buffered whether or not the session is paused: the resume trims this to
    // a 1 s pre-roll, so the first words of a returning speaker survive.
    input_.write(data, len);

    // Audio thread: an RMS over one 3200-byte chunk plus a short critical
    // section held by nobody else for long. No logging, no waiting on the
    // WebSocket thread.
    const double rms = s16le_rms(data, len);
    const bool has_signal = rms >= idle_threshold_rms_.load();
    std::lock_guard<std::mutex> lk(idle_mtx_);
    const uint64_t now = now_ms();
    idle_detector_.feed(has_signal, now);
    last_audio_ms_ = now;
    last_rms_ = rms;
    if (rms > peak_rms_) peak_rms_ = rms;
    ++audio_chunks_;
    if (has_signal) {
        last_signal_ms_ = now;
        ++signal_chunks_;
    }
}

bool CaptionSession::pop_job(const std::shared_ptr<ix::HttpRequestArgs> &args,
                             TranslationJobPtr &job, std::string &key)
{
    std::unique_lock<std::mutex> state(out_mtx_);
    while (running_) {
        job_cv_.wait(state, [this] {
            return !running_ || (pipeline_.queued_count() &&
                pipeline_.in_flight_count() < kTranslateWorkers);
        });
        if (!running_) return false;
        auto dispatch = pipeline_.dispatch(now_ms());
        apply_effects_locked(dispatch.result);
        job = dispatch.job;
        if (job) {
            key = cfg_.api_key; // Captured atomically with this generation's job.
            active_requests_.push_back({job->id, args});
            apply_effects_locked(dispatch.result);
        }
        state.unlock();
        log_events(dispatch.result);
        if (job) return true;
        state.lock();
    }
    return false;
}

void CaptionSession::box_config(int &max_lines, int &max_width, uint64_t *hold_ms)
{
    std::lock_guard<std::mutex> lk(out_mtx_);
    max_lines = cfg_.max_lines;
    max_width = cfg_.max_width;
    if (hold_ms) {
        double hold = cfg_.hold_seconds * 1000.0;
        if (hold < 1000.0) hold = 1000.0;
        if (hold > 30000.0) hold = 30000.0;
        *hold_ms = static_cast<uint64_t>(hold);
    }
}

void CaptionSession::render_and_publish()
{
    std::lock_guard<std::mutex> publication(publish_mtx_);
    CaptionPipelineFrame frame;
    TextSink sink;
    {
        std::lock_guard<std::mutex> state(out_mtx_);
        frame = pipeline_.tick(now_ms());
        apply_effects_locked(frame.result);
        sink = caption_sink_;
    }
    if (frame.changed && publication_gate_.claim(frame.snapshot.publication) && sink)
        sink(*frame.changed);
    log_events(frame.result);
    job_cv_.notify_all();
}

void CaptionSession::publish_source_text(const std::string &text, bool force, uint64_t epoch)
{
    size_t begin = text.size() > 32768 ? text.size() - 32768 : 0;
    while (begin < text.size() && (static_cast<unsigned char>(text[begin]) & 0xc0) == 0x80)
        ++begin;
    const std::string_view bounded(text.data() + begin, text.size() - begin);
    // Config first, then out_mtx_ (see box_config()).
    int max_lines = 2, max_width = 60;
    box_config(max_lines, max_width);

    std::lock_guard<std::mutex> plk(publish_mtx_);
    TextSink sink;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        if (epoch != socket_epoch_ || !running_) return;
        uint64_t now = now_ms();
        if (!force && elapsed_ms(last_source_publish_ms_, now) < kSourceCoalesceMs) {
            // Keep only the latest interim, unwrapped: flush_pending_source()
            // wraps it with the config in effect when it actually goes out.
            pending_interim_.assign(bounded);
            has_pending_interim_ = true;
            return;
        }
        last_source_publish_ms_ = now;
        pending_interim_.clear();
        has_pending_interim_ = false;
        source_shown_ = !text.empty();
        sink = source_sink_;
    }
    if (sink) sink(join_lines(wrap_tail(bounded, max_width, max_lines)));
}

void CaptionSession::flush_pending_source()
{
    int max_lines = 2, max_width = 60;
    box_config(max_lines, max_width);

    std::lock_guard<std::mutex> plk(publish_mtx_);
    std::string text;
    TextSink sink;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        if (!has_pending_interim_) return;
        uint64_t now = now_ms();
        if (elapsed_ms(last_source_publish_ms_, now) < kSourceCoalesceMs) return;
        text = pending_interim_;
        pending_interim_.clear();
        has_pending_interim_ = false;
        last_source_publish_ms_ = now;
        source_shown_ = !text.empty();
        sink = source_sink_;
    }
    if (sink) sink(join_lines(wrap_tail(text, max_width, max_lines)));
}

void CaptionSession::clear_source_if_idle()
{
    int max_lines = 2, max_width = 60;
    uint64_t hold_ms = 4000;
    box_config(max_lines, max_width, &hold_ms);

    std::lock_guard<std::mutex> plk(publish_mtx_);
    TextSink sink;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        if (!source_shown_ || has_pending_interim_) return;
        if (elapsed_ms(last_source_publish_ms_, now_ms()) < hold_ms) return;
        source_shown_ = false;
        sink = source_sink_;
    }
    if (sink) sink("");
}

void CaptionSession::reset_source_state()
{
    interim_pending_.store(false);
    last_interim_ms_.store(0);
    std::lock_guard<std::mutex> lk(out_mtx_);
    pending_interim_.clear();
    has_pending_interim_ = false;
}

void CaptionSession::tick()
{
    flush_pending_source();
    // Both hold timeouts are driven from here even while nothing arrives: the
    // caption window via the composer, the source transcript via
    // clear_source_if_idle().
    clear_source_if_idle();
    render_and_publish();
    const uint64_t now = now_ms();
    if (elapsed_ms(last_diagnostic_ms_, now) >= 30000) {
        last_diagnostic_ms_ = now;
        log_idle_diagnostics(LOG_DEBUG, true);
    }
}

PauseReason CaptionSession::current_pause_reason()
{
    PauseInputs in;
    {
        // Release out_mtx_ before taking idle_mtx_.
        std::lock_guard<std::mutex> lk(out_mtx_);
        in.only_while_output = cfg_.only_while_output_active;
    }
    in.output_active = output_active_.load();
    const bool output_blocked = in.only_while_output && !in.output_active;
    {
        std::lock_guard<std::mutex> lk(idle_mtx_);
        const uint64_t now = now_ms();
        // Explicit output resume gets a new window, even if silence would
        // otherwise keep the gate closed. Ordinary reconnects never reset it.
        if (output_was_blocked_ && !output_blocked)
            idle_detector_.reset(now);
        in.idle = idle_detector_.poll(now);
    }
    output_was_blocked_ = output_blocked;
    return resolve_pause(in);
}

PauseReason CaptionSession::wait_while_paused(PauseReason held)
{
    while (running_) {
        PauseReason now_reason = current_pause_reason();
        if (now_reason == PauseReason::None) return held;
        held = now_reason;
        announce_pause(held);
        // Only the display tick runs here: no connect attempt, no backoff, so
        // a long pause leaves no Connecting/reconnect lines (criterion 8).
        tick();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(kTickMs)));
    }
    return PauseReason::None; // stopping
}

void CaptionSession::log_idle_diagnostics(int level, bool reset_window)
{
    long long audio_age, signal_age;
    uint64_t chunks, signals, timeout;
    double rms, peak;
    {
        std::lock_guard<std::mutex> lk(idle_mtx_);
        const uint64_t now = now_ms();
        audio_age = last_audio_ms_ ? static_cast<long long>(elapsed_ms(last_audio_ms_, now)) : -1;
        signal_age = last_signal_ms_ ? static_cast<long long>(elapsed_ms(last_signal_ms_, now)) : -1;
        chunks = audio_chunks_;
        signals = signal_chunks_;
        rms = last_rms_;
        peak = peak_rms_;
        timeout = idle_detector_.timeout_ms();
        if (reset_window) {
            audio_chunks_ = signal_chunks_ = 0;
            peak_rms_ = 0.0;
        }
    }
    const auto dbfs = [](double value) {
        return value > 0.0 ? 20.0 * std::log10(value / 32767.0)
                           : -std::numeric_limits<double>::infinity();
    };
    blog(level, "[live-translate] caption idle diagnostics: "
         "last_audio_ms_ago=%lld last_signal_ms_ago=%lld "
         "last_rms_dbfs=%.1f peak_rms_dbfs=%.1f chunks=%llu signal_chunks=%llu "
         "timeout_ms=%llu threshold_dbfs=%.1f",
         audio_age, signal_age, dbfs(rms), dbfs(peak),
         static_cast<unsigned long long>(chunks),
         static_cast<unsigned long long>(signals),
         static_cast<unsigned long long>(timeout), dbfs(idle_threshold_rms_.load()));
}

void CaptionSession::announce_pause(PauseReason reason)
{
    // A session-start wait is an idle pause that has never heard a signal;
    // it gets its own wording so a fresh key does not read as a failure.
    bool waiting;
    {
        std::lock_guard<std::mutex> lk(idle_mtx_);
        waiting = reason == PauseReason::Idle && idle_detector_.awaiting_signal();
    }
    const std::string detail = waiting ? "waiting for sound" : pause_reason_name(reason);
    if (detail != announced_pause_detail_) {
        int timeout = 0;
        if (reason == PauseReason::Idle) {
            std::lock_guard<std::mutex> lk(out_mtx_);
            timeout = cfg_.idle_timeout_seconds;
        }
        if (waiting)
            blog(LOG_INFO,
                 "[live-translate] caption session paused: waiting for sound");
        else if (reason == PauseReason::Idle)
            blog(LOG_INFO,
                 "[live-translate] caption session paused: idle %ds", timeout);
        else
            blog(LOG_INFO,
                 "[live-translate] caption session paused: output inactive");
        announced_pause_detail_ = detail;
        log_idle_diagnostics(LOG_INFO);
    }
    set_status(ConnStatus::Paused, detail);
}

bool CaptionSession::pause_until_resumed(PauseReason reason)
{
    announce_pause(reason);
    PauseReason held = wait_while_paused(reason);
    if (!running_) return false;

    blog(LOG_INFO, "[live-translate] caption session resumed: %s",
         held == PauseReason::Idle ? "audio" : "output");
    announced_pause_detail_.clear();
    // Drop the silence that piled up while paused, keeping only the pre-roll.
    // Audio that arrives during the handshake stays queued and the send loop
    // flushes it right after open (the 5 s buffer drops the oldest on overflow).
    input_.keep_last(kResumePrerollBytes);
    // Audio already set the signal timestamp. Output resume is handled in the
    // gate. Do not overwrite either with a transport/handshake timestamp.
    return true;
}

void CaptionSession::run()
{
    Backoff backoff(1000, 30000);
    announced_pause_detail_.clear();
    output_was_blocked_ = false;
    last_diagnostic_ms_ = now_ms();

    while (running_) {
        std::string key;
        std::vector<std::string> vocab;
        uint64_t epoch;
        {
            std::lock_guard<std::mutex> lk(out_mtx_);
            key = cfg_.api_key;
            vocab = cfg_.custom_vocabulary;
            epoch = socket_epoch_;
            config_changed_ = false;
        }

        // Nothing connects while a reason holds. Settings are read live by the
        // gate; after resume the outer loop re-reads transport configuration.
        PauseReason gate = current_pause_reason();
        if (gate != PauseReason::None) {
            backoff.reset(); // a pause is not a failure
            if (!pause_until_resumed(gate)) break;
            continue;        // re-read the config, re-check the gate, then connect
        }

        // The preceding generation reset invalidated translation state.
        reset_source_state();

        ix::WebSocket ws;
        std::string url =
            "wss://generativelanguage.googleapis.com/ws/"
            "google.ai.generativelanguage.v1beta.GenerativeService."
            "BidiGenerateContent?key=" + key;
        ws.setUrl(url);
        // Reconnect handling lives in this loop (status text + spec backoff +
        // the reconnect log line), so the library must not do its own.
        ws.disableAutomaticReconnection();

        std::atomic<bool> auth_error{false};
        std::atomic<bool> open{false};
        std::atomic<bool> disconnected{false};
        std::atomic<uint64_t> connected_at{0};
        std::mutex reason_mtx;
        std::string close_reason;

        ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr &msg) {
            if (msg->type == ix::WebSocketMessageType::Open) {
                {
                    std::lock_guard<std::mutex> state(out_mtx_);
                    if (epoch != socket_epoch_ || !running_) return;
                    set_status(ConnStatus::Connected);
                }
                open = true;
                connected_at = now_ms();
                CaptionSetupOptions opts;
                opts.custom_vocabulary = vocab;
                opts.smart_mode = true;
                ws.send(build_caption_setup_message(opts));
                blog(LOG_INFO,
                     "[live-translate] caption websocket opened; setup sent");
                // Backoff is reset by the WebSocket owner, never this callback.
                // The idle window belongs to audio activity, not this socket.
                // Preserve it across rotation, network and settings reconnects.
            } else if (msg->type == ix::WebSocketMessageType::Message) {
                CaptionServerMessage m = parse_caption_server_message(msg->str);
                switch (m.kind) {
                case CaptionServerMessage::Kind::Interim:
                case CaptionServerMessage::Kind::Final: {
                    const bool final = m.kind == CaptionServerMessage::Kind::Final;
                    const uint64_t now = now_ms();
                    uint64_t stt_final_ms = 0;
                    CaptionPipelineResult changes;
                    {
                        std::lock_guard<std::mutex> state(out_mtx_);
                        if (epoch != socket_epoch_ || !running_) return;
                        if (final) {
                            const uint64_t last = last_interim_ms_.exchange(0);
                            stt_final_ms = last ? elapsed_ms(last, now) : 0;
                            changes = pipeline_.final(m.text, now);
                        } else {
                            last_interim_ms_ = now;
                            changes = pipeline_.interim(m.text, now);
                        }
                        interim_pending_ = !final;
                        apply_effects_locked(changes);
                    }
                    log_events(changes);
                    if (final)
                        blog(LOG_DEBUG, "[live-translate] caption stt_final_ms=%llu",
                             static_cast<unsigned long long>(stt_final_ms));
                    job_cv_.notify_all();
                    publish_source_text(m.text, final, epoch);
                    break;
                }
                case CaptionServerMessage::Kind::Error: {
                    {
                        std::lock_guard<std::mutex> state(out_mtx_);
                        if (epoch != socket_epoch_ || !running_) return;
                        set_status(ConnStatus::AuthError, "STT request rejected");
                        auth_error = true;
                        running_ = false;
                    }
                    // No server body or connection URL is logged.
                    invalidate_connection(epoch);
                    break;
                }
                default:
                    break;
                }
            } else if (msg->type == ix::WebSocketMessageType::Close ||
                       msg->type == ix::WebSocketMessageType::Error) {
                const std::string reason = msg->type == ix::WebSocketMessageType::Error
                    ? "transport error" : "closed";
                invalidate_connection(epoch);
                {
                    std::lock_guard<std::mutex> lk(reason_mtx);
                    if (close_reason.empty()) close_reason = reason;
                }
                open = false;
                disconnected = true;
            }
        });

        {
            std::lock_guard<std::mutex> state(out_mtx_);
            if (epoch != socket_epoch_ || !running_) continue;
            set_status(ConnStatus::Connecting);
        }
        ws.start();

        std::vector<uint8_t> chunk(3200);
        uint64_t last_tick = 0;
        bool age_reconnect = false;
        PauseReason pause_reason = PauseReason::None;

        while (running_ && !auth_error && !config_changed_ && !disconnected) {
            // Keep pre-roll and newly arriving speech until the socket opens.
            size_t n = open ? input_.read(chunk.data(), chunk.size()) : 0;
            if (n == chunk.size()) {
                ws.send(build_realtime_input_message(chunk.data(), chunk.size()));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            uint64_t now = now_ms();
            if (elapsed_ms(last_tick, now) >= kTickMs) {
                last_tick = now;
                tick();
                // Advance elapsed-time detection even without audio callbacks.
                pause_reason = current_pause_reason();
                if (pause_reason != PauseReason::None) break;
            }

            uint64_t since_open = connected_at.load();
            if (since_open != 0 && elapsed_ms(since_open, now) >= kSessionMaxMs &&
                !interim_pending_.load()) {
                blog(LOG_INFO,
                     "[live-translate] caption session reconnect: session age");
                age_reconnect = true;
                break;
            }
        }

        // Invalidate before joining callbacks or waiting through reconnect/pause.
        invalidate_connection(epoch);
        ws.stop();
        if (connected_at.load()) backoff.reset();

        if (auth_error) {
            running_ = false;
            break;
        }
        if (pause_reason != PauseReason::None) {
            // Closed on purpose: keep the backoff at its floor for the
            // reconnect that follows the resume, and stop the age check from
            // seeing an open session that no longer exists.
            backoff.reset();
            connected_at = 0;
            if (!pause_until_resumed(pause_reason)) break;
            continue;
        }
        if (age_reconnect) {
            // Planned rotation, not a failure: reconnect straight away.
            backoff.reset();
            continue;
        }
        if (config_changed_) continue;
        if (!running_) break;

        std::string reason;
        {
            std::lock_guard<std::mutex> lk(reason_mtx);
            reason = close_reason.empty() ? "closed" : close_reason;
        }
        blog(LOG_INFO, "[live-translate] caption session reconnect: %s",
             reason.c_str());
        set_status(ConnStatus::Reconnecting);
        uint32_t wait = backoff.next_ms();
        // A pause reason appearing mid-backoff cuts the wait short: the gate at
        // the top of the loop then parks us instead of retrying a connection.
        for (uint32_t waited = 0; waited < wait && running_ && !config_changed_ &&
                                  current_pause_reason() == PauseReason::None;
             waited += 50) {
            if (waited % kTickMs == 0) tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // Wake the translate workers if this thread stopped the session itself.
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
    }
    job_cv_.notify_all();

    render_and_publish();
    {
        std::lock_guard<std::mutex> publication(publish_mtx_);
        TextSink source;
        {
            std::lock_guard<std::mutex> state(out_mtx_);
            source = source_sink_;
            source_shown_ = false;
        }
        if (source) source("");
    }
    if (status() != ConnStatus::AuthError) set_status(ConnStatus::Idle);
}

void CaptionSession::translate_worker()
{
    ix::HttpClient client(false);
    while (true) {
        auto args = client.createRequest();
        TranslationJobPtr job;
        std::string key;
        if (!pop_job(args, job, key)) return;
        TranslationCompletion completion;
        completion.id = job->id;
        completion.started_ms = now_ms();
        completion.outcome = TranslationOutcome::Failed;
        completion.failure = TranslationFailure::Network;
        try {
            TranslateRequest request;
            request.target_code = job->settings.target_code;
            request.target_name = job->settings.target_name;
            request.glossary = job->settings.glossary;
            request.context = job->context;
            request.text = job->source_text;
            const auto url = translate_endpoint_url(job->settings.model);
            const auto body = build_translate_request(request);
            args->extraHeaders["x-goog-api-key"] = key;
            args->extraHeaders["Content-Type"] = "application/json";
            args->extraHeaders["Accept-Encoding"] = "identity";
            args->connectTimeout = kHttpTimeoutSec;
            args->transferTimeout = kHttpTimeoutSec;
            args->followRedirects = false; // Do not forward credentials to another origin.
            args->compress = false;
            BoundedTranslationResponse response;
            // IX readBytes uses bounded receive chunks and skips its own body
            // accumulation when this callback is present. No allocation by Content-Length.
            args->onChunkCallback = [&response, raw = args.get()](const std::string &chunk) {
                if (!response.append(chunk)) raw->cancel = true;
            };
            ix::HttpResponsePtr transport;
            if (!args->cancel.load()) transport = client.post(url, body, args);
            args->onChunkCallback = nullptr;
            if (response.exceeded()) completion.failure = TranslationFailure::ResponseLimit;
            else if (args->cancel.load()) completion.outcome = TranslationOutcome::Cancelled;
            else if (transport && transport->errorCode == ix::HttpErrorCode::Ok) {
                if (transport->statusCode == 401 || transport->statusCode == 403)
                    completion.failure = TranslationFailure::Auth;
                else if (transport->statusCode != 200) completion.failure = TranslationFailure::Http;
                else {
                    const auto parsed = parse_translate_response(response.body());
                    completion.failure = parsed.failure;
                    if (parsed.ok) {
                        completion.outcome = TranslationOutcome::Success;
                        completion.translated_text = parsed.text;
                    }
                }
            }
        } catch (...) {
            // Even a synchronous transport exception must release the physical
            // scheduler slot through its one terminal completion, not a reset.
            args->onChunkCallback = nullptr;
            completion.failure = TranslationFailure::Network;
        }
        completion.completed_ms = now_ms();
        CaptionPipelineResult changes, reset;
        {
            std::lock_guard<std::mutex> publication(publish_mtx_);
            std::lock_guard<std::mutex> state(out_mtx_);
            active_requests_.erase(std::remove_if(active_requests_.begin(), active_requests_.end(),
                [&](const auto &active) { return active.id == job->id; }), active_requests_.end());
            changes = pipeline_.complete(completion, completion.completed_ms);
            apply_effects_locked(changes);
            if (changes.auth_error && running_) {
                set_status(ConnStatus::AuthError, "translation HTTP authentication rejected");
                running_ = false;
                reset = reset_pipeline_locked(true);
            }
        }
        log_events(changes);
        log_events(reset);
        job_cv_.notify_all();
        // The session tick publishes once after all currently received results.
    }
}

}
