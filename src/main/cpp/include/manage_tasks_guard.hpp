#pragma once

// In-body guard for CTaskManager::ManageTasks @ ELF+0x168 / +0x248 / +0x280 (invalid x20 skips BLR).
bool install_manage_tasks_inbody_guards(void* manage_tasks_fn);

extern "C" int manage_tasks_x20_safe_to_ldr(void* x20);

// Patch guard sites to unconditional branch-to-safe (no mmap BLR). Idempotent.
void set_manage_tasks_force_safe(bool enabled, const char* reason);

// Legacy alias — prefer refresh_manage_tasks_force_safe() from hooks_stability.
void set_manage_tasks_load_force_safe(bool enabled);