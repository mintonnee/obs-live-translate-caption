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
#include <chrono>
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

// Best-effort human-readable reason for a non-2xx / failed HTTP exchange.
std::string http_failure_reason(const ix::HttpResponsePtr &resp)
{
    if (!resp) return "no response";
    if (resp->errorCode != ix::HttpErrorCode::Ok) {
        if (!resp->errorMsg.empty()) return resp->errorMsg;
        return "http error " + std::to_string(static_cast<int>(resp->errorCode));
    }
    TranslateResult parsed = parse_translate_response(resp->body);
    std::string reason = "HTTP " + std::to_string(resp->statusCode);
    if (!parsed.ok && !parsed.error.empty() && parsed.error != "parse error")
        reason += ": " + parsed.error;
    return reason;
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
    bool reconnect = false;
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        reconnect = cfg_.api_key != cfg.api_key || cfg_.target_lang != cfg.target_lang ||
                    cfg_.custom_vocabulary != cfg.custom_vocabulary;
        cfg_ = cfg;
    }

    if (cfg.api_key.empty()) {
        blog(LOG_INFO, "[live-translate] caption API key cleared; stopping session");
        stop();
        return;
    }

    // Window settings apply live; the composer clamps them to its own range.
    {
        double hold = cfg.hold_seconds * 1000.0;
        if (hold < 0.0) hold = 0.0;
        if (hold > 30000.0) hold = 30000.0;
        CaptionComposerConfig cc;
        cc.max_lines = cfg.max_lines;
        cc.max_width = cfg.max_width;
        cc.hold_ms = static_cast<uint64_t>(hold);
        std::lock_guard<std::mutex> lk(out_mtx_);
        composer_.set_config(cc);
    }

    // Idle settings apply live and never force a reconnect (spec 004 §4.1),
    // so they are deliberately not part of the `reconnect` decision above.
    {
        int timeout = cfg.idle_timeout_seconds;
        if (timeout < 0) timeout = 0;
        idle_threshold_rms_.store(dbfs_to_rms(cfg.idle_threshold_dbfs));
        std::lock_guard<std::mutex> lk(idle_mtx_);
        idle_detector_.configure(static_cast<uint64_t>(timeout) * 1000);
    }

    blog(LOG_INFO,
         "[live-translate] configuring caption session: target=%s vocab=%zu "
         "max_lines=%d max_width=%d hold=%.1fs idle_timeout=%ds "
         "idle_threshold=%.1fdBFS only_while_output_active=%s",
         cfg.target_lang.c_str(), cfg.custom_vocabulary.size(), cfg.max_lines,
         cfg.max_width, cfg.hold_seconds, cfg.idle_timeout_seconds,
         cfg.idle_threshold_dbfs, cfg.only_while_output_active ? "true" : "false");

    if (reconnect) config_changed_ = true;

    if (!running_.exchange(true)) {
        // Session start: do not connect before anyone has spoken (spec 004
        // §4.4). The gate in run() polls and waits for the first signal
        // chunk; with the idle pause disabled start_idle() does nothing.
        {
            std::lock_guard<std::mutex> lk(idle_mtx_);
            idle_detector_.start_idle(now_ms());
            last_audio_ms_ = last_signal_ms_ = 0;
            audio_chunks_ = signal_chunks_ = 0;
            last_rms_ = peak_rms_ = 0.0;
        }
        // A previous run may have exited on its own (auth error) and left the
        // threads joinable. Reap them before reassigning, or the assignment
        // would std::terminate.
        if (ws_thread_.joinable()) ws_thread_.join();
        for (auto &t : translate_threads_) {
            if (t.joinable()) t.join();
        }
        translate_threads_.clear();

        translate_threads_.reserve(kTranslateWorkers);
        for (size_t i = 0; i < kTranslateWorkers; ++i)
            translate_threads_.emplace_back([this] { translate_worker(); });
        ws_thread_ = std::thread([this] { run(); });
    }
}

void CaptionSession::stop()
{
    running_.exchange(false);
    // Serialize with a worker between predicate check and wait(), so the
    // wake-up cannot be lost.
    {
        std::lock_guard<std::mutex> lk(job_mtx_);
    }
    job_cv_.notify_all();

    // Always join: run() may have cleared running_ itself (auth error), leaving
    // the threads joinable; the singleton's destruction would std::terminate.
    if (ws_thread_.joinable()) ws_thread_.join();
    for (auto &t : translate_threads_) {
        if (t.joinable()) t.join();
    }
    translate_threads_.clear();

    {
        std::lock_guard<std::mutex> lk(job_mtx_);
        jobs_.clear();
    }
    input_.clear();
    next_seq_.store(0);
    interim_pending_.store(false);
    last_interim_ms_.store(0);
    // The idle timer follows the audio that just got dropped.
    // output_active_ is left alone: it is a plugin-wide fact, not session state.
    {
        std::lock_guard<std::mutex> lk(idle_mtx_);
        idle_detector_.reset(now_ms());
    }
    announced_pause_detail_.clear(); // run() is joined above

    TextSink caption_sink, source_sink;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        composer_.clear();
        pending_interim_.clear();
        has_pending_interim_ = false;
        source_shown_ = false;
        last_source_publish_ms_ = 0;
        caption_sink = caption_sink_;
        source_sink = source_sink_;
    }
    if (caption_sink) caption_sink("");
    if (source_sink) source_sink("");

    set_status(ConnStatus::Idle);
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

void CaptionSession::push_job(TranslateJob job)
{
    {
        std::lock_guard<std::mutex> lk(job_mtx_);
        jobs_.push_back(std::move(job));
    }
    job_cv_.notify_one();
}

bool CaptionSession::pop_job(TranslateJob &job)
{
    std::unique_lock<std::mutex> lk(job_mtx_);
    job_cv_.wait(lk, [this] { return !jobs_.empty() || !running_.load(); });
    if (!running_.load()) return false;
    if (jobs_.empty()) return false;
    job = std::move(jobs_.front());
    jobs_.pop_front();
    return true;
}

void CaptionSession::box_config(int &max_lines, int &max_width, uint64_t *hold_ms)
{
    std::lock_guard<std::mutex> lk(cfg_mtx_);
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
    // Hold publish_mtx_ across render + sink so a concurrent caller cannot
    // render a newer window and publish it before this older one lands.
    std::lock_guard<std::mutex> plk(publish_mtx_);
    std::optional<std::string> v;
    TextSink sink;
    std::vector<CaptionTruncation> truncations;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        v = composer_.render(now_ms());
        // Collected even when the display did not change, so a truncation is
        // never silently dropped; blog() must not run under out_mtx_.
        truncations = composer_.take_truncations();
        sink = caption_sink_;
    }
    if (v && sink) sink(*v);
    for (const CaptionTruncation &t : truncations)
        blog(LOG_INFO,
             "[live-translate] caption seg=%llu truncated lines=%d kept=%d",
             static_cast<unsigned long long>(t.seq), t.lines, t.kept);
}

void CaptionSession::publish_source_text(const std::string &text, bool force)
{
    // Config first, then out_mtx_ (see box_config()).
    int max_lines = 2, max_width = 60;
    box_config(max_lines, max_width);

    std::lock_guard<std::mutex> plk(publish_mtx_);
    TextSink sink;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        uint64_t now = now_ms();
        if (!force && elapsed_ms(last_source_publish_ms_, now) < kSourceCoalesceMs) {
            // Keep only the latest interim, unwrapped: flush_pending_source()
            // wraps it with the config in effect when it actually goes out.
            pending_interim_ = text;
            has_pending_interim_ = true;
            return;
        }
        last_source_publish_ms_ = now;
        pending_interim_.clear();
        has_pending_interim_ = false;
        source_shown_ = !text.empty();
        sink = source_sink_;
    }
    if (sink) sink(join_lines(wrap_tail(text, max_width, max_lines)));
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
        // Release cfg_mtx_ before taking idle_mtx_.
        std::lock_guard<std::mutex> lk(cfg_mtx_);
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
            std::lock_guard<std::mutex> lk(cfg_mtx_);
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
        {
            std::lock_guard<std::mutex> lk(cfg_mtx_);
            key = cfg_.api_key;
            vocab = cfg_.custom_vocabulary;
        }
        config_changed_ = false;

        // Nothing connects while a reason holds. Settings are read live by the
        // gate; after resume the outer loop re-reads transport configuration.
        PauseReason gate = current_pause_reason();
        if (gate != PauseReason::None) {
            backoff.reset(); // a pause is not a failure
            if (!pause_until_resumed(gate)) break;
            continue;        // re-read the config, re-check the gate, then connect
        }

        // Already-emitted captions survive a reconnect; only the interim
        // bookkeeping is dropped.
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
                open = true;
                connected_at = now_ms();
                CaptionSetupOptions opts;
                opts.custom_vocabulary = vocab;
                opts.smart_mode = true;
                ws.send(build_caption_setup_message(opts));
                blog(LOG_INFO,
                     "[live-translate] caption websocket opened; setup sent");
                set_status(ConnStatus::Connected);
                backoff.reset();
                // The idle window belongs to audio activity, not this socket.
                // Preserve it across rotation, network and settings reconnects.
            } else if (msg->type == ix::WebSocketMessageType::Message) {
                CaptionServerMessage m = parse_caption_server_message(msg->str);
                switch (m.kind) {
                case CaptionServerMessage::Kind::Interim: {
                    last_interim_ms_.store(now_ms());
                    publish_source_text(m.text, false);
                    interim_pending_.store(true);
                    break;
                }
                case CaptionServerMessage::Kind::Final: {
                    uint64_t now = now_ms();
                    uint64_t seq = ++next_seq_;
                    TranslateJob job;
                    job.seq = seq;
                    job.text = m.text;
                    job.t_final_ms = now;
                    uint64_t last_interim = last_interim_ms_.exchange(0);
                    job.stt_final_ms =
                        last_interim ? elapsed_ms(last_interim, now) : 0;
                    {
                        std::lock_guard<std::mutex> lk(out_mtx_);
                        job.context = composer_.context(kContextSegments);
                        composer_.push_final(seq, m.text, now);
                    }
                    push_job(std::move(job));
                    publish_source_text(m.text, true);
                    interim_pending_.store(false);
                    break;
                }
                case CaptionServerMessage::Kind::Error: {
                    blog(LOG_ERROR, "[live-translate] caption server error: %s",
                         m.error_message.c_str());
                    set_status(ConnStatus::AuthError, m.error_message);
                    auth_error = true;
                    ws.stop();
                    break;
                }
                default:
                    break;
                }
            } else if (msg->type == ix::WebSocketMessageType::Close ||
                       msg->type == ix::WebSocketMessageType::Error) {
                std::string reason;
                if (msg->type == ix::WebSocketMessageType::Error) {
                    reason = msg->errorInfo.reason.empty() ? "error"
                                                           : msg->errorInfo.reason;
                    blog(LOG_ERROR, "[live-translate] caption websocket error: %s",
                         reason.c_str());
                } else {
                    reason = "closed";
                    blog(LOG_INFO, "[live-translate] caption websocket closed: %s",
                         msg->closeInfo.reason.c_str());
                }
                {
                    std::lock_guard<std::mutex> lk(reason_mtx);
                    if (close_reason.empty()) close_reason = reason;
                }
                open = false;
                disconnected = true;
            }
        });

        set_status(ConnStatus::Connecting);
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

        ws.stop();

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
             waited += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Wake the translate workers if this thread stopped the session itself.
    {
        std::lock_guard<std::mutex> lk(job_mtx_);
    }
    job_cv_.notify_all();

    set_status(status() == ConnStatus::AuthError ? ConnStatus::AuthError
                                                 : ConnStatus::Idle);
}

void CaptionSession::translate_worker()
{
    // One client per thread: ix::HttpClient owns a single socket and is not
    // meant to be shared across concurrent requests.
    ix::HttpClient client(false);
    // The model (and so the URL) is read per job: it can change live.

    TranslateJob job;
    while (pop_job(job)) {
        std::string key, lang, name, model;
        std::vector<std::string> glossary;
        int max_lines = 2, max_width = 60;
        {
            std::lock_guard<std::mutex> lk(cfg_mtx_);
            key = cfg_.api_key;
            lang = cfg_.target_lang;
            name = cfg_.target_name;
            glossary = cfg_.custom_vocabulary;
            model = cfg_.translate_model;
            max_lines = cfg_.max_lines;
            max_width = cfg_.max_width;
        }

        TranslateRequest req;
        req.target_code = lang;
        req.target_name = name;
        req.context = job.context;
        req.text = job.text;
        // Length hint so truncation stays rare (spec 002 §4.5).
        req.max_chars = max_lines * max_width;
        // Same terms the transcriber is biased toward, so names survive translation.
        req.glossary = std::move(glossary);
        std::string body = build_translate_request(req);
        const std::string url = translate_endpoint_url(model);

        auto args = client.createRequest(url, ix::HttpClient::kPost);
        args->extraHeaders["x-goog-api-key"] = key;
        args->extraHeaders["Content-Type"] = "application/json";
        args->connectTimeout = kHttpTimeoutSec;
        args->transferTimeout = kHttpTimeoutSec;

        uint64_t sent_at = now_ms();
        ix::HttpResponsePtr resp = client.post(url, body, args);
        uint64_t translate_ms = elapsed_ms(sent_at, now_ms());

        int code = resp ? resp->statusCode : 0;
        bool transport_ok = resp && resp->errorCode == ix::HttpErrorCode::Ok;

        if (transport_ok && code == 200) {
            TranslateResult r = parse_translate_response(resp->body);
            if (r.ok) {
                {
                    std::lock_guard<std::mutex> lk(out_mtx_);
                    composer_.on_translated(job.seq, r.text, now_ms());
                }
                blog(LOG_INFO,
                     "[live-translate] caption seg=%llu stt_final_ms=%llu "
                     "translate_ms=%llu chars=%zu",
                     static_cast<unsigned long long>(job.seq),
                     static_cast<unsigned long long>(job.stt_final_ms),
                     static_cast<unsigned long long>(translate_ms), r.text.size());
                blog(LOG_DEBUG, "[live-translate] caption seg=%llu source=%s",
                     static_cast<unsigned long long>(job.seq), job.text.c_str());
                blog(LOG_DEBUG, "[live-translate] caption seg=%llu translated=%s",
                     static_cast<unsigned long long>(job.seq), r.text.c_str());
                render_and_publish();
                continue;
            }
            // 200 with an unusable body: drop this segment, keep the session.
            {
                std::lock_guard<std::mutex> lk(out_mtx_);
                composer_.on_failed(job.seq, r.error, now_ms());
            }
            blog(LOG_WARNING,
                 "[live-translate] caption seg=%llu translate failed: %s",
                 static_cast<unsigned long long>(job.seq), r.error.c_str());
            render_and_publish();
            continue;
        }

        if (transport_ok && (code == 401 || code == 403)) {
            TranslateResult r = parse_translate_response(resp->body);
            std::string detail = r.error.empty() || r.error == "parse error"
                                     ? "HTTP " + std::to_string(code)
                                     : r.error;
            blog(LOG_ERROR, "[live-translate] caption translate auth error: %s",
                 detail.c_str());
            set_status(ConnStatus::AuthError, detail);
            {
                std::lock_guard<std::mutex> lk(out_mtx_);
                composer_.on_failed(job.seq, "auth", now_ms());
            }
            render_and_publish();
            // Stop the whole session, like the speech path does on a bad key.
            running_ = false;
            {
                std::lock_guard<std::mutex> lk(job_mtx_);
            }
            job_cv_.notify_all();
            continue;
        }

        std::string reason = http_failure_reason(resp);
        {
            std::lock_guard<std::mutex> lk(out_mtx_);
            composer_.on_failed(job.seq, reason, now_ms());
        }
        blog(LOG_WARNING, "[live-translate] caption seg=%llu translate failed: %s",
             static_cast<unsigned long long>(job.seq), reason.c_str());
        render_and_publish();
    }
}

}
