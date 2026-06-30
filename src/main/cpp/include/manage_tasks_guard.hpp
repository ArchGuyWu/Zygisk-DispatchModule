#pragma once

// In-body null guard for CTaskManager::ManageTasks @ ELF+0x168 / +0x278 (tombstone 03–08).
bool install_manage_tasks_inbody_guards(void* manage_tasks_fn);