#pragma once
#include "caption-composer.hpp"
#include "ring-buffer.hpp"
#include "translation-session.hpp" // ConnStatus
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Shared singleton for the captions output mode: one WebSocket to
// gemini-3.5-transcribe-live (worker thread, reconnect + backoff, 9-minute
// proactive reconnect), an HTTP worker that translates finalized segments with
// the Flash-Lite model (kTranslateModel), and a CaptionComposer that orders/windows the result.
// Display is delegated to sinks installed by the filter (caption-output.cpp),
// so this class knows nothing about OBS text sources.
// Spec: docs/specs/001-caption-translation-pipeline.md §4.3, §4.4, §4.7,
// criteria 7, 8, 12; docs/specs/002-caption-text-box-limits.md §4.4, §4.5,
// criteria 8, 9.
namespace lt {

struct CaptionConfig {
    std::string api_key;
    std::string target_lang;                    // BCP-47 code from languages.hpp
    std::string target_name;                    // English name for the prompt
    std::vector<std::string> custom_vocabulary; // empty = omitted from setup
    int max_lines = 2;                          // displayed lines, 1-6
    int max_width = 60;                         // line width in display units, 10-120
    double hold_seconds = 4.0;                  // 1-30
};

class CaptionSession {
public:
    static CaptionSession &instance();

    // Called with the full display string (may be "" to clear). Invoked from
    // worker threads; must be cheap and must not block on the audio thread.
    using TextSink = std::function<void(const std::string &text)>;

    // caption_sink receives the translated window; source_sink (may be empty)
    // receives interim/final source transcripts. Replaces previous sinks.
    void set_sinks(TextSink caption_sink, TextSink source_sink);

    // Starts the worker if needed, or applies a config change (reconnects when
    // the key/target/vocabulary changed; window settings apply live).
    // Empty api_key stops the session.
    void configure(const CaptionConfig &cfg);
    void stop(); // joins workers, clears sinks' display via a final ""

    // 16 kHz mono S16LE PCM, 3200-byte chunks from the filter.
    void push_input_pcm(const uint8_t *data, size_t len);

    ConnStatus status();
    std::string status_text(); // same wording as TranslationSession::status_text()
    bool is_running();

private:
    CaptionSession();
    ~CaptionSession();
    CaptionSession(const CaptionSession &) = delete;
    CaptionSession &operator=(const CaptionSession &) = delete;

    // One finalized transcript segment waiting for its translation.
    struct TranslateJob {
        uint64_t seq = 0;
        std::string text;                   // source transcript
        std::vector<std::string> context;   // up to 3 previous source segments
        uint64_t t_final_ms = 0;            // when the final arrived
        uint64_t stt_final_ms = 0;          // last interim -> final latency
    };

    void run();              // WebSocket worker (STT stream)
    void translate_worker(); // HTTP worker (generateContent)

    void set_status(ConnStatus s, const std::string &detail = "");

    void push_job(TranslateJob job);
    bool pop_job(TranslateJob &job); // false once the session is stopping

    // Renders the composer window and hands it to caption_sink_ when it
    // changed. Never calls the sink while holding out_mtx_.
    void render_and_publish();
    // Publishes source-transcript text; interim updates are coalesced to at
    // most one sink call per kSourceCoalesceMs, finals go out immediately.
    // The text handed to the sink is wrap_tail()'d to the configured
    // max_width x max_lines box, so the newest words stay visible (spec 002
    // §4.4, criterion 9). The coalescing buffer keeps the raw text.
    void publish_source_text(const std::string &text, bool force);
    void flush_pending_source(); // sends a coalesced interim once its window elapsed
    void reset_source_state();   // drops interim bookkeeping (reconnect / stop)

    // Current text-box limits. Takes cfg_mtx_ only: the lock order is cfg_mtx_
    // first, then out_mtx_, and the two are never held at the same time.
    void box_config(int &max_lines, int &max_width);

    // Proactive reconnect before the Live API's 10-minute session cap.
    static constexpr uint64_t kSessionMaxMs = 9 * 60 * 1000;
    static constexpr uint64_t kSourceCoalesceMs = 100;
    static constexpr uint64_t kTickMs = 100;
    static constexpr size_t kTranslateWorkers = 3;
    static constexpr size_t kContextSegments = 3;
    static constexpr int kHttpTimeoutSec = 5;

    ByteRingBuffer input_{16000 * 2 * 5};

    std::mutex cfg_mtx_;
    CaptionConfig cfg_;

    // Guards the composer plus the sinks and the interim coalescing state.
    // Sinks are always invoked after this mutex is released.
    std::mutex out_mtx_;
    // Serializes sink invocations across the WebSocket and translate workers so
    // two concurrent renders cannot reach the text source out of order. Never
    // held together with out_mtx_.
    std::mutex publish_mtx_;
    CaptionComposer composer_;
    TextSink caption_sink_;
    TextSink source_sink_;
    std::string pending_interim_;
    bool has_pending_interim_ = false;
    uint64_t last_source_publish_ms_ = 0;

    std::mutex job_mtx_;
    std::condition_variable job_cv_;
    std::deque<TranslateJob> jobs_;

    std::atomic<bool> running_{false};
    std::atomic<bool> config_changed_{false};
    std::atomic<bool> interim_pending_{false}; // an interim is not yet finalized
    std::atomic<uint64_t> last_interim_ms_{0};
    std::atomic<uint64_t> next_seq_{0};

    std::thread ws_thread_;
    std::vector<std::thread> translate_threads_;

    std::mutex status_mtx_;
    ConnStatus status_ = ConnStatus::Idle;
    std::string status_detail_;
};

}
