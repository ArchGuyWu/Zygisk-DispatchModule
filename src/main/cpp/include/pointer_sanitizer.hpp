#pragma once

#include <cstdint>

// Layered pointer readability (depth defense):
//   Tier 0 — nullptr (caller / engine semantics)
//   Tier 1 — userspace address range
//   Tier 2 — cached /proc/self/maps readable VMA lookup (hot path, no syscall)
//   Tier 3 — POSIX pipe probe fallback (cold / stale-cache edge cases)

inline bool is_userspace_address(const void* ptr) {
    if (!ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    return addr >= 0x10000ULL && addr <= 0x00007fffffffffffULL;
}

// Tier 2: VMA cache lookup (may refresh maps if TTL expired).
bool vma_is_readable(const void* ptr);

// Tier 3: pipe write probe (EFAULT on invalid pages).
bool pipe_probe_readable(const void* ptr);

// Tiered composite used throughout stability hooks.
bool is_pointer_readable(const void* ptr);

// Force Tier 3 only (diagnostics / rare strict checks).
bool is_pointer_readable_strict(const void* ptr);

// process_vm_readv UTF-16 unit read (never dereferences remote memory).
bool safe_read_u16(const void* addr, uint16_t* out);

// Bounded ICU-style UTF-16 strlen: vm_readv each unit (tombstone_01–10, #48–49).
int32_t safe_utf16_strlen_bounded(const void* s, int32_t max_units = 4096);