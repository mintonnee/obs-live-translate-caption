#include "caption-session.hpp"
#include <obs-module.h>

extern struct obs_source_info live_translate_filter_info;
extern struct obs_source_info live_translate_source_info;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-live-translate", "en-US")

extern "C" const char *obs_module_name(void) { return "OBS Live Translate"; }
extern "C" const char *obs_module_description(void)
{
    return "Real-time speech-to-speech translation via the Gemini Live API.";
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "[live-translate] module loaded");
    obs_register_source(&live_translate_filter_info);
    obs_register_source(&live_translate_source_info);
    return true;
}

void obs_module_unload(void)
{
    // The caption session's sinks call back into libobs (text source updates).
    // Stop it and drop the sinks now, while libobs is still alive; the
    // singleton's destructor runs at process exit, after libobs is gone.
    auto &captions = lt::CaptionSession::instance();
    captions.stop();
    captions.set_sinks({}, {});
    blog(LOG_INFO, "[live-translate] module unloaded");
}
