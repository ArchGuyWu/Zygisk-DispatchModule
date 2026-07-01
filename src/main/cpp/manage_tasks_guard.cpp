#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "manage_tasks_guard.hpp"
#include "log.hpp"

namespace {

constexpr size_t kManageTasksGuardSite160 = 0x168;
constexpr size_t kManageTasksGuardSite278 = 0x278;
constexpr size_t kManageTasksResume168 = 0x168 + 8;
constexpr size_t kManageTasksSafe254 = 0x254;
constexpr size_t kManageTasksResume280 = 0x278 + 8;
constexpr size_t kManageTasksSafe3b0 = 0x3b0;

struct GuardSiteState {
    void* site = nullptr;
    void* safe = nullptr;
    void* tramp = nullptr;
};

GuardSiteState g_guard160;
GuardSiteState g_guard278;

uint32_t arm64_b(uintptr_t from, uintptr_t to) {
    const int64_t off = (static_cast<int64_t>(to) - static_cast<int64_t>(from)) / 4;
    return 0x14000000u | (static_cast<uint32_t>(off) & 0x03FFFFFFu);
}

uint32_t arm64_cbz_x20(uintptr_t from, uintptr_t to) {
    const int64_t off = (static_cast<int64_t>(to) - static_cast<int64_t>(from)) / 4;
    return 0xB4000000u | ((static_cast<uint32_t>(off) & 0x7FFFFu) << 5) | 20u;
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
    return patch_insn(site, arm64_b(reinterpret_cast<uintptr_t>(site), reinterpret_cast<uintptr_t>(target)));
}

bool install_branch_only_tramp(GuardSiteState* state, uintptr_t base, size_t site_off, size_t resume_off,
                             size_t safe_off, const char* label) {
    void* site = reinterpret_cast<void*>(base + site_off);
    void* resume = reinterpret_cast<void*>(base + resume_off);
    void* safe = reinterpret_cast<void*>(base + safe_off);

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;

    void* tramp = mmap(nullptr, static_cast<size_t>(page_size), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        LOGE("❌ [ManageTasksGuard] mmap failed for %s", label);
        return false;
    }

    const uintptr_t tramp_addr = reinterpret_cast<uintptr_t>(tramp);
    // No BLR into mod .text (PAC-safe): cbz → ldr → resume, else branch safe.
    uint32_t words[5] = {};
    words[0] = arm64_cbz_x20(tramp_addr + 0, tramp_addr + 16);
    words[1] = 0xF9400288u;  // ldr x8, [x20]
    words[2] = 0xAA1403E0u;  // mov x0, x20
    words[3] = arm64_b(tramp_addr + 12, reinterpret_cast<uintptr_t>(resume));
    words[4] = arm64_b(tramp_addr + 16, reinterpret_cast<uintptr_t>(safe));
    std::memcpy(tramp, words, sizeof(words));

    if (!finalize_trampoline_page(tramp, sizeof(words))) {
        munmap(tramp, static_cast<size_t>(page_size));
        return false;
    }

    if (!patch_branch_to(site, tramp)) {
        LOGE("❌ [ManageTasksGuard] patch failed for %s @ %p", label, site);
        munmap(tramp, static_cast<size_t>(page_size));
        return false;
    }

    state->site = site;
    state->safe = safe;
    state->tramp = tramp;
    LOGI("✅ [ManageTasksGuard] %s: site=%p tramp=%p resume=%p safe=%p", label, site, tramp, resume, safe);
    return true;
}

} // namespace

void set_manage_tasks_load_force_safe(bool enabled) {
    if (!g_guard160.site || !g_guard278.site) return;
    if (enabled) {
        LOGW("⚠️ [ManageTasksGuard] GenericLoad — force safe @ +0x168/+0x278");
        patch_branch_to(g_guard160.site, g_guard160.safe);
        patch_branch_to(g_guard278.site, g_guard278.safe);
    } else {
        patch_branch_to(g_guard160.site, g_guard160.tramp);
        patch_branch_to(g_guard278.site, g_guard278.tramp);
    }
}

bool install_manage_tasks_inbody_guards(void* manage_tasks_fn) {
    if (!manage_tasks_fn) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(manage_tasks_fn);

    const bool ok160 = install_branch_only_tramp(&g_guard160, base, kManageTasksGuardSite160,
                                                 kManageTasksResume168, kManageTasksSafe254,
                                                 "x20-null@57ab160");
    const bool ok278 = install_branch_only_tramp(&g_guard278, base, kManageTasksGuardSite278,
                                                 kManageTasksResume280, kManageTasksSafe3b0,
                                                 "x20-null@57ab278");
    return ok160 && ok278;
}