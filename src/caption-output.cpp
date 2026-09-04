#include "caption-output.hpp"
#include <mutex>
#include <obs.h>
#include <set>

// Writes translated/source caption text into OBS text sources. Called from the
// CaptionSession worker threads, so the bookkeeping below is mutex-protected.
// Spec: docs/specs/001-caption-translation-pipeline.md §4.6, criterion 11.
namespace lt {
namespace {

std::mutex g_mtx;
// Names we already logged as missing, so a wrong name warns once instead of
// once per segment. A name is forgotten again as soon as a write to it
// succeeds, so a source that disappears later warns again.
std::set<std::string> g_warned;
// Name of the source the last failed write targeted; cleared by any successful
// write (see caption-output.hpp).
std::string g_missing;

} // namespace

void caption_output_write(const std::string &source_name, const std::string &text)
{
    if (source_name.empty()) return;

    obs_source_t *src = obs_get_source_by_name(source_name.c_str());
    if (src) {
        // Only the "text" setting is touched; font/position/style stay whatever
        // the user configured on the source.
        obs_data_t *s = obs_data_create();
        obs_data_set_string(s, "text", text.c_str());
        obs_source_update(src, s);
        obs_data_release(s);
        obs_source_release(src);

        std::lock_guard<std::mutex> lk(g_mtx);
        g_warned.erase(source_name);
        g_missing.clear();
        return;
    }

    bool first_time = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_missing = source_name;
        first_time = g_warned.insert(source_name).second;
    }
    if (first_time)
        blog(LOG_WARNING, "[live-translate] caption text source \"%s\" not found",
             source_name.c_str());
}

std::string caption_output_missing_source()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_missing;
}

} // namespace lt
