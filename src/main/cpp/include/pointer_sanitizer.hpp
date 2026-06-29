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