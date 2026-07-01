#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "manage_tasks_guard.hpp"
#include "log.hpp"

namespace {

constexpr size_t kManageTasksGuardSite060 = 0x060;
constexpr size_t kManageTasksGuardSite14c = 0x14c;
constexpr size_t kManageTasksGuardSite168 = 0x168;
constexpr size_t kManageTasksGuardSite248 = 0x248;
constexpr size_t kManageTasksGuardSite280 = 0x280;
// After BLR: skip stale/null virtual calls; non-null continues at vtable ldr.
constexpr size_t kManageTasksNullSkip074 = 0x074;
constexpr size_t kManageTasksNullSkip15c = 0x15c;
constexpr size_t kManageTasksNullSkip178 = 0x178;
constexpr size_t kManageTasksNullSkip258 = 0x258;
constexpr size_t kManageTasksNullSkip294 = 0x294;
constexpr size_t kManageTasksNonnull068 = 0x068;
constexpr size_t kManageTasksNonnull150 = 0x150;
constexpr size_t kManageTasksNonnull170 = 0x170;
constexpr size_t kManageTasksNonnull250 = 0x250;
constexpr size_t kManageTasksNonnull28c = 0x28c;

struct GuardSiteState {
    void* site = nullptr;
    void* tramp = nullptr;
};

GuardSiteState g_guard060;
GuardSiteState g_guard14c;
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

bool install_cbz_x0_tramp(GuardSiteState* state, uintptr_t base, size_t site_off,
                          size_t nonnull_resume_off, size_t null_skip_off, const char* label) {
    void* site = reinterpret_cast<void*>(base + site_off);
    void* nonnull_resume = reinterpret_cast<void*>(base + nonnull_resume_off);
    void* null_skip = reinterpret_cast<void*>(base + null_skip_off);

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;

    void* tramp = mmap(nullptr, static_cast<size_t>(page_size), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        LOGE("❌ [ManageTasksGuard] mmap failed for %s", label);
        return false;
    }

    const uintptr_t tramp_addr = reinterpret_cast<uintptr_t>(tramp);
    uint32_t words[4] = {};
    words[0] = arm64_cbz(tramp_addr + 0, tramp_addr + 12, 0);
    words[1] = 0xF9400008u;  // ldr x8, [x0]
    words[2] = arm64_b(tramp_addr + 8, reinterpret_cast<uintptr_t>(nonnull_resume));
    words[3] = arm64_b(tramp_addr + 12, reinterpret_cast<uintptr_t>(null_skip));
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
    state->tramp = tramp;
    LOGI("✅ [ManageTasksGuard] %s: site=%p tramp=%p nonnull=%p null=%p",
         label, site, tramp, nonnull_resume, null_skip);
    return true;
}

bool install_cbz_x20_tramp(GuardSiteState* state, uintptr_t base, size_t site_off,
                           size_t nonnull_resume_off, size_t null_skip_off, const char* label) {
    void* site = reinterpret_cast<void*>(base + site_off);
    void* nonnull_resume = reinterpret_cast<void*>(base + nonnull_resume_off);
    void* null_skip = reinterpret_cast<void*>(base + null_skip_off);

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;

    void* tramp = mmap(nullptr, static_cast<size_t>(page_size), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        LOGE("❌ [ManageTasksGuard] mmap failed for %s", label);
        return false;
    }

    const uintptr_t tramp_addr = reinterpret_cast<uintptr_t>(tramp);
    uint32_t words[5] = {};
    words[0] = arm64_cbz(tramp_addr + 0, tramp_addr + 16, 20);
    words[1] = 0xF9400288u;  // ldr x8, [x20]
    words[2] = 0xAA1403E0u;  // mov x0, x20
    words[3] = arm64_b(tramp_addr + 12, reinterpret_cast<uintptr_t>(nonnull_resume));
    words[4] = arm64_b(tramp_addr + 16, reinterpret_cast<uintptr_t>(null_skip));
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
    state->tramp = tramp;
    LOGI("✅ [ManageTasksGuard] %s: site=%p tramp=%p nonnull=%p null=%p",
         label, site, tramp, nonnull_resume, null_skip);
    return true;
}

} // namespace

void set_manage_tasks_force_safe(bool enabled, const char* reason) {
    (void)reason;
    // Unconditional force BLR-skip breaks GenericLoad (hang before deserialize).
    // Tramp null/nonnull split handles load tail; keep sites on tramp always.
    if (!enabled) return;
}

void set_manage_tasks_load_force_safe(bool enabled) {
    set_manage_tasks_force_safe(enabled, enabled ? "GenericLoad" : "GenericLoad end");
}

bool install_manage_tasks_inbody_guards(void* manage_tasks_fn) {
    if (!manage_tasks_fn) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(manage_tasks_fn);

    const bool ok060 = install_cbz_x0_tramp(&g_guard060, base, kManageTasksGuardSite060,
                                            kManageTasksNonnull068, kManageTasksNullSkip074,
                                            "cbz-x0@57ab060");
    const bool ok14c = install_cbz_x0_tramp(&g_guard14c, base, kManageTasksGuardSite14c,
                                            kManageTasksNonnull150, kManageTasksNullSkip15c,
                                            "cbz-x0@57ab144");
    const bool ok168 = install_cbz_x20_tramp(&g_guard168, base, kManageTasksGuardSite168,
                                             kManageTasksNonnull170, kManageTasksNullSkip178,
                                             "cbz-x20@57ab160");
    const bool ok248 = install_cbz_x20_tramp(&g_guard248, base, kManageTasksGuardSite248,
                                             kManageTasksNonnull250, kManageTasksNullSkip258,
                                             "cbz-x20@57ab240");
    const bool ok280 = install_cbz_x20_tramp(&g_guard280, base, kManageTasksGuardSite280,
                                             kManageTasksNonnull28c, kManageTasksNullSkip294,
                                             "cbz-x20@57ab278");
    return ok060 && ok14c && ok168 && ok248 && ok280;
}