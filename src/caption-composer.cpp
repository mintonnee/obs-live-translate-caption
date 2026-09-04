#include "caption-composer.hpp"

#include <algorithm>

namespace lt {

namespace {

CaptionComposerConfig clamp_config(CaptionComposerConfig cfg)
{
    if (cfg.max_segments < 1)
        cfg.max_segments = 1;
    if (cfg.max_segments > 4)
        cfg.max_segments = 4;
    if (cfg.hold_ms < 1000)
        cfg.hold_ms = 1000;
    if (cfg.hold_ms > 30000)
        cfg.hold_ms = 30000;
    return cfg;
}

}

CaptionComposer::CaptionComposer(CaptionComposerConfig cfg)
    : cfg_(clamp_config(cfg))
{}

void CaptionComposer::set_config(const CaptionComposerConfig &cfg)
{
    cfg_ = clamp_config(cfg);
}

CaptionComposerConfig CaptionComposer::config() const
{
    return cfg_;
}

void CaptionComposer::push_final(uint64_t seq, const std::string &source_text,
                                 uint64_t now_ms)
{
    (void)now_ms;
    if (has_pushed_ && seq <= last_seq_)
        return;

    segments_.emplace(seq, Segment{});
    last_seq_ = seq;
    has_pushed_ = true;

    sources_.push_back(source_text);
    while (sources_.size() > kMaxContext)
        sources_.pop_front();
}

void CaptionComposer::on_translated(uint64_t seq, const std::string &translated_text,
                                    uint64_t now_ms)
{
    (void)now_ms;
    auto it = segments_.find(seq);
    if (it == segments_.end() || it->second.state != SegmentState::Pending)
        return;

    it->second.state = SegmentState::Translated;
    it->second.text = translated_text;
}

void CaptionComposer::on_failed(uint64_t seq, const std::string &reason, uint64_t now_ms)
{
    (void)reason;
    (void)now_ms;
    auto it = segments_.find(seq);
    if (it == segments_.end() || it->second.state != SegmentState::Pending)
        return;

    it->second.state = SegmentState::Failed;
    it->second.text.clear();
}

std::optional<std::string> CaptionComposer::render(uint64_t now_ms)
{
    bool emitted = false;
    while (!segments_.empty()) {
        auto it = segments_.begin();
        if (it->second.state == SegmentState::Pending)
            break;

        if (it->second.state == SegmentState::Translated) {
            emitted_.push_back(it->second.text);
            while (emitted_.size() > kMaxSegments)
                emitted_.pop_front();
            emitted = true;
        }
        segments_.erase(it);
    }

    if (emitted)
        last_emit_ms_ = now_ms;

    std::string display = build_display();

    // Hold timeout: drop the whole on-screen window once nothing new arrived
    // for hold_ms. Dropped texts are never shown again.
    if (!emitted && !display.empty() && now_ms >= last_emit_ms_ + cfg_.hold_ms) {
        emitted_.clear();
        display.clear();
    }

    if (display == last_display_)
        return std::nullopt;

    last_display_ = display;
    return display;
}

std::string CaptionComposer::build_display() const
{
    size_t window = std::min(static_cast<size_t>(cfg_.max_segments), emitted_.size());
    std::string display;
    for (size_t i = emitted_.size() - window; i < emitted_.size(); ++i) {
        if (!display.empty())
            display += "\n";
        display += emitted_[i];
    }
    return display;
}

std::vector<std::string> CaptionComposer::context(size_t max) const
{
    size_t count = std::min(max, sources_.size());
    return std::vector<std::string>(sources_.end() - static_cast<ptrdiff_t>(count),
                                    sources_.end());
}

size_t CaptionComposer::pending_count() const
{
    size_t count = 0;
    for (const auto &entry : segments_) {
        if (entry.second.state == SegmentState::Pending)
            ++count;
    }
    return count;
}

void CaptionComposer::clear()
{
    segments_.clear();
    emitted_.clear();
    sources_.clear();
    last_emit_ms_ = 0;
    last_seq_ = 0;
    has_pushed_ = false;
}

}
