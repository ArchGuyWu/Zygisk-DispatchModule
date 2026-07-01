#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "manage_tasks_guard.hpp"
#include "log.hpp"
#include "mod_shared.hpp"
#include "pointer_sanitizer.hpp"

namespace {

constexpr size_t kManageTasksGuardSite160 = 0x168;
constexpr size_t kManageTasksGuardSite278 = 0x278;
constexpr size_t kManageTasksResume168 = 0x168 + 8;
constexpr size_t kManageTasksSafe254 = 0x254;
constexpr size_t kManageTasksResume280 = 0x278 + 8;
constexpr size_t kManageTasksSafe3b0 = 0x3b0;

constexpr size_t kTrampLiteralPick = 40;
constexpr size_t kTrampLiteralSafe = 48;
constexpr size_t kTrampTotalBytes = 56;

uintptr_t g_pick_resume160 = 0;
uintptr_t g_pick_safe160 = 0;
uintptr_t g_pick_resume278 = 0;
uintptr_t g_pick_safe278 = 0;

uint32_t arm64_b(uintptr_t from, uintptr_t to) {
    const int64_t off = (static_cast<int64_t>(to) - static_cast<int64_t>(from)) / 4;
    return 0x14000000u | (static_cast<uint32_t>(off) & 0x03FFFFFFu);
}

uint32_t arm64_cbz_x20(uintptr_t from, uintptr_t to) {
    const int64_t off = (static_cast<int64_t>(to) - static_cast<int64_t>(from)) / 4;
    return 0xB4000000u | ((static_cast<uint32_t>(off) & 0x7FFFFu) << 5) | 20u;
}

uint32_t arm64_ldr_x16_pc_rel(uintptr_t insn_pc, uintptr_t literal_addr) {
    const int64_t off = static_cast<int64_t>(literal_addr) - static_cast<int64_t>(insn_pc);
    const uint32_t imm19 = (static_cast<uint32_t>(off) >> 2) & 0x7FFFFu;
    return 0x58000000u | (imm19 << 5) | 16u;
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

static bool manage_tasks_vtable_fn_readable(void* obj, size_t vtable_offset) {
    if (!obj || !is_pointer_readable(obj)) return false;
    void** vtable_slot = reinterpret_cast<void**>(obj);
    if (!is_pointer_readable(vtable_slot)) return false;
    void* vtable = *vtable_slot;
    if (!vtable || !is_pointer_readable(vtable)) return false;
    void** fn_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) + vtable_offset);
    if (!is_pointer_readable(fn_slot)) return false;
    void* fn = *fn_slot;
    return fn && is_pointer_readable(fn);
}

static bool manage_tasks_x20_resume_safe(void* x20, uintptr_t x8_loaded) {
    if (!x20 || !is_pointer_readable(x20)) return false;
    void** head = reinterpret_cast<void**>(x20);
    if (!is_pointer_readable(head)) return false;
    const uintptr_t vtable_ptr = reinterpret_cast<uintptr_t>(*head);
    if (x8_loaded != vtable_ptr) return false;
    if (!is_task_vtable_safe(x20)) return false;
    if (!manage_tasks_vtable_fn_readable(x20, 0x28)) return false;
    if (!manage_tasks_vtable_fn_readable(x20, 0x18)) return false;
    return true;
}

bool patch_branch_to(void* site, void* target) {
    if (!site || !target) return false;
    if (!make_page_writable(site)) return false;
    const uint32_t insn = arm64_b(reinterpret_cast<uintptr_t>(site), reinterpret_cast<uintptr_t>(target));
    std::memcpy(site, &insn, sizeof(insn));
    __builtin___clear_cache(reinterpret_cast<char*>(site),
                            reinterpret_cast<char*>(site) + static_cast<ptrdiff_t>(sizeof(insn)));
    return true;
}

bool install_guard_at(uintptr_t base, size_t site_off, size_t resume_off, size_t safe_off,
                      uintptr_t* pick_resume, uintptr_t* pick_safe,
                      void* pick_fn, const char* label) {
    void* site = reinterpret_cast<void*>(base + site_off);
    void* resume = reinterpret_cast<void*>(base + resume_off);
    void* safe = reinterpret_cast<void*>(base + safe_off);

    *pick_resume = reinterpret_cast<uintptr_t>(resume);
    *pick_safe = reinterpret_cast<uintptr_t>(safe);

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;

    void* tramp = mmap(nullptr, static_cast<size_t>(page_size), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        LOGE("❌ [ManageTasksGuard] mmap failed for %s", label);
        return false;
    }

    const uintptr_t tramp_addr = reinterpret_cast<uintptr_t>(tramp);
    uint32_t words[10] = {};
    words[0] = arm64_cbz_x20(tramp_addr + 0, tramp_addr + 28);
    words[1] = 0xF9400288u;  // ldr x8, [x20]
    words[2] = 0xAA1403E0u;  // mov x0, x20
    words[3] = 0xAA0803E1u;  // mov x1, x8
    words[4] = arm64_ldr_x16_pc_rel(tramp_addr + 16, tramp_addr + kTrampLiteralPick);
    words[5] = 0xD63F0200u;  // blr x16
    words[6] = 0xD61F0000u;  // br x0
    words[7] = arm64_ldr_x16_pc_rel(tramp_addr + 28, tramp_addr + kTrampLiteralSafe);
    words[8] = 0xD61F0200u;  // br x16

    std::memcpy(tramp, words, 36);

    uintptr_t* literals = reinterpret_cast<uintptr_t*>(reinterpret_cast<char*>(tramp) + kTrampLiteralPick);
    literals[0] = reinterpret_cast<uintptr_t>(pick_fn);
    literals[1] = reinterpret_cast<uintptr_t>(safe);

    if (!finalize_trampoline_page(tramp, kTrampTotalBytes)) {
        munmap(tramp, static_cast<size_t>(page_size));
        return false;
    }

    if (!patch_branch_to(site, tramp)) {
        LOGE("❌ [ManageTasksGuard] patch failed for %s @ %p", label, site);
        munmap(tramp, static_cast<size_t>(page_size));
        return false;
    }

    LOGI("✅ [ManageTasksGuard] %s: site=%p tramp=%p resume=%p safe=%p", label, site, tramp, resume, safe);
    return true;
}

} // namespace

extern "C" uintptr_t manage_tasks_guard_pick_path(void* x20, uintptr_t x8, uintptr_t resume,
                                                  uintptr_t safe) {
    if (manage_tasks_x20_resume_safe(x20, x8)) {
        return resume;
    }
    if (is_save_load_active()) {
        LOGW("⚠️ [ManageTasksGuard] stale x20=%p x8=0x%llx — safe path (load)",
             x20, static_cast<unsigned long long>(x8));
    }
    return safe;
}

extern "C" uintptr_t manage_tasks_guard_pick_path_160(void* x20, uintptr_t x8) {
    return manage_tasks_guard_pick_path(x20, x8, g_pick_resume160, g_pick_safe160);
}

extern "C" uintptr_t manage_tasks_guard_pick_path_278(void* x20, uintptr_t x8) {
    return manage_tasks_guard_pick_path(x20, x8, g_pick_resume278, g_pick_safe278);
}

bool install_manage_tasks_inbody_guards(void* manage_tasks_fn) {
    if (!manage_tasks_fn) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(manage_tasks_fn);

    const bool ok160 = install_guard_at(base, kManageTasksGuardSite160, kManageTasksResume168,
                                        kManageTasksSafe254, &g_pick_resume160, &g_pick_safe160,
                                        reinterpret_cast<void*>(manage_tasks_guard_pick_path_160),
                                        "x20-null@57ab160");
    const bool ok278 = install_guard_at(base, kManageTasksGuardSite278, kManageTasksResume280,
                                        kManageTasksSafe3b0, &g_pick_resume278, &g_pick_safe278,
                                        reinterpret_cast<void*>(manage_tasks_guard_pick_path_278),
                                        "x20-null@57ab278");

    return ok160 && ok278;
}