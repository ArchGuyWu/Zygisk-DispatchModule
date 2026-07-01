#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "manage_tasks_guard.hpp"
#include "pointer_sanitizer.hpp"
#include "log.hpp"

namespace {

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// RE offsets relative to CTaskManager::ManageTasks (ELF 0x57aaff8).
constexpr size_t kManageTasksGuardSite168 = 0x168;
constexpr size_t kManageTasksGuardSite248 = 0x248;
constexpr size_t kManageTasksGuardSite280 = 0x280;
constexpr size_t kManageTasksNullSkip178 = 0x178;
constexpr size_t kManageTasksNullSkip258 = 0x258;
constexpr size_t kManageTasksNullSkip294 = 0x294;
constexpr size_t kManageTasksNonnull170 = 0x170;
constexpr size_t kManageTasksNonnull250 = 0x250;
constexpr size_t kManageTasksNonnull288 = 0x288;

constexpr int64_t kArm64BranchRange = 0x7F000000;

struct GuardSiteState {
    void* site = nullptr;
    void* tramp = nullptr;
};

GuardSiteState g_guard168;
GuardSiteState g_guard248;
GuardSiteState g_guard280;

uint32_t arm64_b(uintptr_t from, uintptr_t to) {
    const int64_t off = (static_cast<int64_t>(to) - static_cast<int64_t>(from)) / 4;
    return 0x14000000u | (static_cast<uint32_t>(off) & 0x03FFFFFFu);
}

uint32_t arm64_cbz(uintptr_t from, uintptr_t to, unsigned reg) {
    const int64_t off = (static_cast<int64_t>(to) - static_cast<int64_t>(from)) / 4;
    return 0xB4000000u | ((static_cast<uint32_t>(off) & 0x7FFFFu) << 5) | (reg & 0x1Fu);
}

uint32_t arm64_cbzw(uintptr_t from, uintptr_t to, unsigned reg) {
    const int64_t off = (static_cast<int64_t>(to) - static_cast<int64_t>(from)) / 4;
    return 0x34000000u | ((static_cast<uint32_t>(off) & 0x7FFFFu) << 5) | (reg & 0x1Fu);
}

uint32_t arm64_ldr_x9_literal(uintptr_t insn_pc, uintptr_t literal_addr) {
    const int64_t off = (static_cast<int64_t>(literal_addr) - static_cast<int64_t>(insn_pc)) / 4;
    return 0x58000000u | ((static_cast<uint32_t>(off) & 0x7FFFFu) << 5) | 9u;
}

bool branch_reachable(uintptr_t from, uintptr_t to) {
    const int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    return delta >= -kArm64BranchRange && delta <= kArm64BranchRange;
}

bool make_page_writable(void* addr) {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    const uintptr_t page = reinterpret_cast<uintptr_t>(addr) & ~(static_cast<uintptr_t>(page_size) - 1);
    return mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

bool finalize_trampoline_page(void* page, size_t used_bytes) {
    if (!page || page == MAP_FAILED) return false;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    __builtin___clear_cache(reinterpret_cast<char*>(page),
                            reinterpret_cast<char*>(page) + static_cast<ptrdiff_t>(used_bytes));
    if (mprotect(page, static_cast<size_t>(page_size), PROT_READ | PROT_EXEC) != 0) {
        LOGE("❌ [ManageTasksGuard] mprotect RX failed: %s", strerror(errno));
        return false;
    }
    return true;
}

bool patch_insn(void* site, uint32_t insn) {
    if (!site) return false;
    if (!make_page_writable(site)) return false;
    std::memcpy(site, &insn, sizeof(insn));
    __builtin___clear_cache(reinterpret_cast<char*>(site),
                            reinterpret_cast<char*>(site) + static_cast<ptrdiff_t>(sizeof(insn)));
    return true;
}

bool patch_branch_to(void* site, void* target) {
    if (!site || !target) return false;
    const uintptr_t from = reinterpret_cast<uintptr_t>(site);
    const uintptr_t to = reinterpret_cast<uintptr_t>(target);
    if (!branch_reachable(from, to)) {
        LOGE("❌ [ManageTasksGuard] branch out of range: site=%p target=%p delta=%lld",
             site, target, static_cast<long long>(static_cast<int64_t>(to) - static_cast<int64_t>(from)));
        return false;
    }
    return patch_insn(site, arm64_b(from, to));
}

void* allocate_near(void* target, size_t size) {
    if (!target || size == 0) return MAP_FAILED;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return MAP_FAILED;

    const uintptr_t target_addr = reinterpret_cast<uintptr_t>(target);
    const uintptr_t page_mask = static_cast<uintptr_t>(page_size) - 1;
    const uintptr_t aligned_target = target_addr & ~page_mask;

    for (uintptr_t delta = static_cast<uintptr_t>(page_size);
         delta < static_cast<uintptr_t>(kArm64BranchRange);
         delta += static_cast<uintptr_t>(page_size)) {
        const uintptr_t candidates[] = {
            aligned_target >= delta ? aligned_target - delta : 0,
            aligned_target + delta,
        };
        for (uintptr_t base : candidates) {
            if (base == 0) continue;
            void* hint = reinterpret_cast<void*>(base);
            void* page = mmap(hint, size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (page != MAP_FAILED) return page;
        }
    }

    void* fallback = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fallback == MAP_FAILED) return MAP_FAILED;
    const uintptr_t fb = reinterpret_cast<uintptr_t>(fallback);
    if (!branch_reachable(fb, target_addr) && !branch_reachable(target_addr, fb)) {
        munmap(fallback, size);
        return MAP_FAILED;
    }
    return fallback;
}

// Null or unreadable x20 → skip BLR (fade); garbage x20 → skip too (Continue #43).
bool install_validated_x20_tramp(GuardSiteState* state, uintptr_t base, size_t site_off,
                                 size_t nonnull_resume_off, size_t null_skip_off,
                                 const char* label) {
    void* site = reinterpret_cast<void*>(base + site_off);
    void* nonnull_resume = reinterpret_cast<void*>(base + nonnull_resume_off);
    void* null_skip = reinterpret_cast<void*>(base + null_skip_off);

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;

    void* tramp = allocate_near(site, static_cast<size_t>(page_size));
    if (tramp == MAP_FAILED) {
        LOGE("❌ [ManageTasksGuard] near mmap failed for %s", label);
        return false;
    }

    const uintptr_t tramp_addr = reinterpret_cast<uintptr_t>(tramp);
    const uintptr_t literal_addr = tramp_addr + 40;
    const uintptr_t insn0_pc = tramp_addr;
    const uintptr_t insn4_pc = tramp_addr + 4;
    const uintptr_t insn16_pc = tramp_addr + 16;
    const uintptr_t insn32_pc = tramp_addr + 32;

    uint32_t words[12] = {};
    words[0] = arm64_ldr_x9_literal(insn0_pc, literal_addr);
    words[1] = arm64_cbz(insn4_pc, insn32_pc, 20);
    words[2] = 0xAA1403E0u;  // mov x0, x20
    words[3] = 0xD63F0120u;  // blr x9
    words[4] = arm64_cbzw(insn16_pc, insn32_pc, 0);
    words[5] = 0xF9400288u;  // ldr x8, [x20]
    words[6] = 0xAA1403E0u;  // mov x0, x20
    words[7] = arm64_b(tramp_addr + 28, reinterpret_cast<uintptr_t>(nonnull_resume));
    words[8] = arm64_b(insn32_pc, reinterpret_cast<uintptr_t>(null_skip));

    if (!branch_reachable(tramp_addr + 28, reinterpret_cast<uintptr_t>(nonnull_resume)) ||
        !branch_reachable(insn32_pc, reinterpret_cast<uintptr_t>(null_skip))) {
        LOGE("❌ [ManageTasksGuard] tramp exit out of range for %s", label);
        munmap(tramp, static_cast<size_t>(page_size));
        return false;
    }

    std::memcpy(tramp, words, 36);
    const uint64_t fn_ptr = reinterpret_cast<uint64_t>(&manage_tasks_x20_safe_to_ldr);
    std::memcpy(reinterpret_cast<char*>(tramp) + 40, &fn_ptr, sizeof(fn_ptr));

    if (!finalize_trampoline_page(tramp, 48)) {
        munmap(tramp, static_cast<size_t>(page_size));
        return false;
    }

    if (!patch_branch_to(site, tramp)) {
        LOGE("❌ [ManageTasksGuard] patch failed for %s @ %p", label, site);
        munmap(tramp, static_cast<size_t>(page_size));
        return false;
    }

    state->site = site;
    state->tramp = tramp;
    LOGI("✅ [ManageTasksGuard] %s: site=%p tramp=%p nonnull=%p null=%p",
         label, site, tramp, nonnull_resume, null_skip);
    return true;
}

} // namespace

extern "C" int manage_tasks_x20_safe_to_ldr(void* x20) {
    if (!x20 || !is_userspace_address(x20) || !is_pointer_readable(x20)) {
        return 0;
    }
    return 1;
}

void set_manage_tasks_force_safe(bool enabled, const char* reason) {
    (void)reason;
    (void)enabled;
}

void set_manage_tasks_load_force_safe(bool enabled) {
    set_manage_tasks_force_safe(enabled, enabled ? "GenericLoad" : "GenericLoad end");
}

bool install_manage_tasks_inbody_guards(void* manage_tasks_fn) {
    if (!manage_tasks_fn) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(manage_tasks_fn);

    const bool ok168 = install_validated_x20_tramp(&g_guard168, base, kManageTasksGuardSite168,
                                                   kManageTasksNonnull170, kManageTasksNullSkip178,
                                                   "valid-x20@57ab160");
    const bool ok248 = install_validated_x20_tramp(&g_guard248, base, kManageTasksGuardSite248,
                                                   kManageTasksNonnull250, kManageTasksNullSkip258,
                                                   "valid-x20@57ab240");
    const bool ok280 = install_validated_x20_tramp(&g_guard280, base, kManageTasksGuardSite280,
                                                   kManageTasksNonnull288, kManageTasksNullSkip294,
                                                   "valid-x20@57ab278");
    return ok168 && ok248 && ok280;
}