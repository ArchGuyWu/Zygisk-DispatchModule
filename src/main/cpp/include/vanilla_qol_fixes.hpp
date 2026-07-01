#pragma once

// Vanilla game QoL fixes — separate from dispatch logic and crash-stability hooks.
// Each fix should be opt-in, idempotent, and logged when applied.

void install_vanilla_qol_fixes(void* lib_handle);
void poll_vanilla_qol_fixes();
void vanilla_qol_on_save_load_session_ended();
void vanilla_qol_on_skip_pipeline_cleared();
void vanilla_qol_on_deserialize_complete();
void vanilla_qol_schedule_touch_rehydrate(const char* reason);
bool vanilla_qol_touch_rehydrate_pending();