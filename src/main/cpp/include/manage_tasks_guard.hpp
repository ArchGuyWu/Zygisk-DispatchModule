#pragma once

// In-body guard for CTaskManager::ManageTasks @ ELF+0x168 / +0x278 (tombstone 03–08).
bool install_manage_tasks_inbody_guards(void* manage_tasks_fn);

// Patch guard sites to unconditional branch-to-safe (no mmap BLR). Idempotent.
void set_manage_tasks_force_safe(bool enabled, const char* reason);

// Legacy alias — prefer refresh_manage_tasks_force_safe() from hooks_stability.
void set_manage_tasks_load_force_safe(bool enabled);