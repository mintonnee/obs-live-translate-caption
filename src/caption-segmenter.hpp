#pragma once
#include "caption-segment.hpp"
#include <deque>
#include <string_view>

namespace lt {

struct SegmentRetirement {
    SegmentKey key;
    CaptionRetireReason reason = CaptionRetireReason::None;
};

struct SegmenterResult {
    // Includes final-flag/order-only updates; consumers deduplicate by source_revision.
    std::vector<CaptionSegment> upserts;
    std::vector<SegmentRetirement> retired;
    size_t source_limit = 0;
    size_t segment_limit = 0;
    size_t reconcile_ambiguous = 0;
};

std::string normalize_caption_source(std::string_view text);
std::vector<std::string> split_caption_source(std::string_view text);

// Serialized by the caller. Times are monotonic milliseconds, including zero.
class CaptionSegmenter {
public:
    static constexpr size_t kMaxSourceBytes = 32 * 1024;
    static constexpr size_t kMaxSnapshots = 3;
    static constexpr size_t kMaxSegments = 256;

    explicit CaptionSegmenter(uint64_t generation = 1);
    void reset(uint64_t generation);
    // Optional; interim/final automatically start the next utterance after final.
    void begin_utterance(uint64_t now_ms);
    void set_incremental(bool enabled);
    void set_display_state(const SegmentKey &key, CaptionDisplayState state);
    SegmenterResult interim(std::string_view text, uint64_t now_ms);
    SegmenterResult final(std::string_view text, uint64_t now_ms);
    SegmenterResult tick(uint64_t now_ms);
    const std::vector<CaptionSegment> &segments() const { return segments_; }
    size_t snapshot_count() const { return snapshots_.size(); }
    size_t snapshot_bytes() const;
    size_t managed_count() const { return segments_.size() + tombstones_; }
    bool incremental_stopped() const { return stopped_; }

private:
    SegmenterResult evaluate(uint64_t now_ms);
    SegmenterResult reconcile(const std::string &text, bool final, uint64_t now_ms);
    uint64_t generation_;
    uint64_t next_utterance_ = 0;
    uint64_t next_segment_ = 0;
    uint64_t utterance_ = 0;
    uint64_t first_seen_ms_ = 0;
    bool closed_ = false;
    bool incremental_ = true;
    bool stopped_ = false;
    size_t tombstones_ = 0;
    std::deque<std::string> snapshots_;
    std::vector<uint64_t> unchanged_since_;
    std::vector<CaptionSegment> segments_;
    std::vector<CaptionDisplayState> states_;
};

}
