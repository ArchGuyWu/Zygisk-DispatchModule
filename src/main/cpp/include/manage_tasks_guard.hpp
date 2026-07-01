#pragma once

#include <cstdint>

// In-body guard for CTaskManager::ManageTasks @ ELF+0x168 / +0x278 (tombstone 03–08).
bool install_manage_tasks_inbody_guards(void* manage_tasks_fn);

// Trampoline BLR target: resume (safe path in ManageTasks) or safe (skip vtable walk).
extern "C" uintptr_t manage_tasks_guard_pick_path(void* x20, uintptr_t x8, uintptr_t resume,
                                                  uintptr_t safe);