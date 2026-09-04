#include "caption-composer.hpp"

#include "caption-wrap.hpp"

#include <algorithm>
#include <utility>

namespace lt {

namespace {

CaptionComposerConfig clamp_config(CaptionComposerConfig cfg)
{
    if (cfg.max_lines < 1)
        cfg.max_lines = 1;
    if (cfg.max_lines > 6)
        cfg.max_lines = 6;
    if (cfg.max_width < 10)
        cfg.max_width = 10;
    if (cfg.max_width > 120)
        cfg.max_width = 120;
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
    // A narrower window takes effect at once; lines already on screen keep the
    // width they were wrapped at (spec 002 §4.3).
    trim_window();
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
            if (emit_segment(it->first, it->second.text))
                emitted = true;
        }
        segments_.erase(it);
    }

    if (emitted)
        last_emit_ms_ = now_ms;

    std::string display = build_display();

    // Hold timeout: drop the whole on-screen window once nothing new arrived
    // for hold_ms. Dropped lines are never shown again.
    if (!emitted && !display.empty() && now_ms >= last_emit_ms_ + cfg_.hold_ms) {
        lines_.clear();
        display.clear();
    }

    if (display == last_display_)
        return std::nullopt;

    last_display_ = display;
    return display;
}

bool CaptionComposer::emit_segment(uint64_t seq, const std::string &text)
{
    std::vector<std::string> lines = wrap_text(text, cfg_.max_width);
    // A whitespace-only translation has nothing to show and is not a
    // truncation; it must not restart the hold timer either.
    if (lines.empty())
        return false;

    const size_t keep = static_cast<size_t>(cfg_.max_lines);
    if (lines.size() > keep) {
        truncations_.push_back(
            CaptionTruncation{seq, static_cast<int>(lines.size()), cfg_.max_lines});
        lines.resize(keep);
        lines.back() = truncate_with_ellipsis(lines.back(), cfg_.max_width);
    }

    for (std::string &line : lines)
        lines_.push_back(std::move(line));
    trim_window();
    return true;
}

void CaptionComposer::trim_window()
{
    const size_t keep = static_cast<size_t>(cfg_.max_lines);
    while (lines_.size() > keep)
        lines_.pop_front();
}

std::string CaptionComposer::build_display() const
{
    return join_lines(std::vector<std::string>(lines_.begin(), lines_.end()));
}

std::vector<CaptionTruncation> CaptionComposer::take_truncations()
{
    std::vector<CaptionTruncation> out;
    out.swap(truncations_);
    return out;
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
    lines_.clear();
    sources_.clear();
    truncations_.clear();
    last_emit_ms_ = 0;
    last_seq_ = 0;
    has_pushed_ = false;
}

}
