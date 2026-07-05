#include "translation-session.hpp"
#include "backoff.hpp"
#include "live-protocol.hpp"
#include <ixwebsocket/IXWebSocket.h>
#include <obs.h>
#include <chrono>
#include <vector>

namespace lt {

namespace {
uint64_t now_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}

TranslationSession &TranslationSession::instance()
{
    static TranslationSession s;
    return s;
}

TranslationSession::TranslationSession() = default;

TranslationSession::~TranslationSession() { stop(); }

void TranslationSession::set_status(ConnStatus s, const std::string &detail)
{
    std::lock_guard<std::mutex> lk(status_mtx_);
    status_ = s;
    status_detail_ = detail;
}

ConnStatus TranslationSession::status()
{
    std::lock_guard<std::mutex> lk(status_mtx_);
    return status_;
}

bool TranslationSession::is_running()
{
    return running_.load();
}

std::string TranslationSession::status_text()
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
    }
    return "Unknown";
}

void TranslationSession::configure(const std::string &api_key,
                                   const std::string &target_lang,
                                   bool echo_target)
{
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        api_key_ = api_key;
        target_lang_ = target_lang;
        echo_target_ = echo_target;
    }
    if (api_key.empty()) {
        blog(LOG_INFO, "[live-translate] API key cleared; stopping session");
        stop();
        return;
    }
    blog(LOG_INFO,
         "[live-translate] configuring session: target=%s echo=%s api_key=%s",
         target_lang.c_str(), echo_target ? "on" : "off",
         api_key.empty() ? "empty" : "set");
    config_changed_ = true;
    if (!running_.exchange(true)) {
        // A previous run() may have exited on its own (e.g. auth error) and left
        // thread_ joinable. Join it before reassigning, or the assignment would
        // std::terminate.
        if (thread_.joinable()) thread_.join();
        thread_ = std::thread([this] { run(); });
    }
}

bool TranslationSession::claim_output(const void *token)
{
    return output_owner_.claim(token);
}

void TranslationSession::release_output(const void *token)
{
    // Releasing output ownership does not stop the session: the worker thread is
    // driven by the filter input, so a source detaching must not stop a
    // still-producing translation.
    output_owner_.release(token);
}

bool TranslationSession::output_owned_by_other(const void *token)
{
    return output_owner_.owned_by_other(token);
}

void TranslationSession::set_output_delay_ms(uint32_t delay_ms)
{
    output_delay_ms_.store(delay_ms, std::memory_order_relaxed);
}

uint32_t TranslationSession::output_delay_ms() const
{
    return output_delay_ms_.load(std::memory_order_relaxed);
}

void TranslationSession::stop()
{
    running_.exchange(false);
    // Always join: run() may have already cleared running_ itself (auth error),
    // in which case the thread is still joinable and must be reaped here so the
    // singleton's destruction does not std::terminate.
    if (thread_.joinable()) thread_.join();
    input_.clear();
    output_.clear();
    set_status(ConnStatus::Idle);
}

void TranslationSession::push_input_pcm(const uint8_t *data, size_t len)
{
    last_input_ms_.store(now_ms(), std::memory_order_relaxed);
    input_.write(data, len);
}

void TranslationSession::append_output(const uint8_t *data, size_t len)
{
    output_.write(data, len);
    // Serialize with a consumer that is between checking the predicate and
    // entering wait_for, so the notify cannot be lost.
    { std::lock_guard<std::mutex> lk(output_wait_mtx_); }
    output_cv_.notify_one();
}

void TranslationSession::signal_interrupt()
{
    output_.clear();
    interrupted_.store(true, std::memory_order_relaxed);
    { std::lock_guard<std::mutex> lk(output_wait_mtx_); }
    output_cv_.notify_one();
}

size_t TranslationSession::wait_and_read_output(uint8_t *out, size_t max_len,
                                                uint32_t timeout_ms)
{
    {
        std::unique_lock<std::mutex> lk(output_wait_mtx_);
        output_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [this] { return output_.size() > 0; });
    }
    return output_.read(out, max_len);
}

bool TranslationSession::take_interrupted()
{
    return interrupted_.exchange(false, std::memory_order_relaxed);
}

uint64_t TranslationSession::input_idle_ms()
{
    uint64_t last = last_input_ms_.load(std::memory_order_relaxed);
    if (last == 0) return UINT64_MAX;
    uint64_t now = now_ms();
    return now >= last ? now - last : 0;
}

void TranslationSession::run()
{
    Backoff backoff(1000, 30000);

    while (running_) {
        std::string key, lang;
        bool echo;
        {
            std::lock_guard<std::mutex> lk(cfg_mtx_);
            key = api_key_;
            lang = target_lang_;
            echo = echo_target_;
        }
        config_changed_ = false;

        ix::WebSocket ws;
        std::string url =
            "wss://generativelanguage.googleapis.com/ws/"
            "google.ai.generativelanguage.v1beta.GenerativeService."
            "BidiGenerateContent?key=" + key;
        ws.setUrl(url);

        std::atomic<bool> auth_error{false};
        std::atomic<bool> open{false};

        ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr &msg) {
            if (msg->type == ix::WebSocketMessageType::Open) {
                open = true;
                std::string setup = build_setup_message(lang, echo);
                ws.send(setup);
                blog(LOG_INFO, "[live-translate] websocket opened; setup sent");
                set_status(ConnStatus::Connected);
                backoff.reset();
            } else if (msg->type == ix::WebSocketMessageType::Message) {
                ServerMessage m = parse_server_message(msg->str);
                if (m.interrupted) {
                    blog(LOG_INFO,
                         "[live-translate] turn interrupted; dropping pending audio");
                    signal_interrupt();
                }
                if (m.kind == ServerMessage::Kind::Audio && !m.audio.empty()) {
                    blog(LOG_DEBUG, "[live-translate] received audio bytes: %zu",
                         m.audio.size());
                    append_output(m.audio.data(), m.audio.size());
                } else if (m.kind == ServerMessage::Kind::Error) {
                    blog(LOG_ERROR, "[live-translate] server error: %s",
                         m.error_message.c_str());
                    set_status(ConnStatus::AuthError, m.error_message);
                    auth_error = true;
                    ws.stop();
                }
            } else if (msg->type == ix::WebSocketMessageType::Close ||
                       msg->type == ix::WebSocketMessageType::Error) {
                if (msg->type == ix::WebSocketMessageType::Error) {
                    blog(LOG_ERROR, "[live-translate] websocket error: %s",
                         msg->errorInfo.reason.c_str());
                } else {
                    blog(LOG_INFO, "[live-translate] websocket closed: %s",
                         msg->closeInfo.reason.c_str());
                }
                open = false;
            }
        });

        set_status(ConnStatus::Connecting);
        ws.start();

        std::vector<uint8_t> chunk(3200);
        while (running_ && !auth_error && !config_changed_) {
            size_t n = input_.read(chunk.data(), chunk.size());
            if (n == chunk.size() && open) {
                std::string m =
                    build_realtime_input_message(chunk.data(), chunk.size());
                ws.send(m);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        ws.stop();

        if (auth_error) {
            running_ = false;
            break;
        }
        if (config_changed_) continue;
        if (!running_) break;

        set_status(ConnStatus::Reconnecting);
        uint32_t wait = backoff.next_ms();
        for (uint32_t waited = 0; waited < wait && running_ && !config_changed_;
             waited += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    set_status(status() == ConnStatus::AuthError ? ConnStatus::AuthError
                                                 : ConnStatus::Idle);
}

}
