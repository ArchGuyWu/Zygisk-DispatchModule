#pragma once

// In-body guard for CTaskManager::ManageTasks @ ELF+0x168 / +0x278 (tombstone 03–08).
bool install_manage_tasks_inbody_guards(void* manage_tasks_fn);

// During GenericLoad tail: patch guard sites to unconditional branch-to-safe (no mmap BLR).
void set_manage_tasks_load_force_safe(bool enabled);