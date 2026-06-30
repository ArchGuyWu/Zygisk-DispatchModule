#pragma once

// Vanilla game QoL fixes — separate from dispatch logic and crash-stability hooks.
// Each fix should be opt-in, idempotent, and logged when applied.

void install_vanilla_qol_fixes(void* lib_handle);
void poll_vanilla_qol_fixes();