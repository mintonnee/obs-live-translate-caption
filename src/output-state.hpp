#pragma once

// OBS output state (streaming / recording / virtual camera) via obs-frontend-api
// (docs/specs/004-idle-pause-and-output-gating.md §4.5). Pushes the OR of the
// three into CaptionSession::set_output_active() and logs changes as
// "[live-translate] output active: <true|false>".
namespace lt {

// Registers the frontend event callback and pushes the initial state.
// Call from obs_module_load(). Safe when no frontend is present (all false).
void output_state_init();

// Removes the frontend event callback. Call from obs_module_unload().
void output_state_shutdown();

// Last value pushed to the session.
bool output_state_active();

}
