#include "vanilla_qol_fixes.hpp"

#include "log.hpp"
#include "mod_shared.hpp"

// =====================================================================
// Vanilla QoL fixes (engine bugs / UX gaps — NOT dispatch or stability)
// =====================================================================
//
// Add new fixes here as self-contained helpers + optional hooks.
// Stability / save-load session gating lives in hooks_stability.cpp only.

namespace {

// ---------------------------------------------------------------------
// TODO(qol-001): Interior save-load — shop clerk / ambient ped missing
// ---------------------------------------------------------------------
// Symptom (vanilla): auto-save while inside a shop (e.g. Ammu-Nation) restores
// the player in the interior but the clerk disappears until re-entering.
//
// Root cause: load restores position/interior layout but does not re-run the
// entry-time ambient ped setup path.
//
// Suggested libUE4 symbols (nm -D libUE4.so):
//   g_interiorMan
//   _ZN17InteriorManager_c20GetPedsInteriorGroupEP4CPed
//   _ZN17InteriorManager_c12ActivatePedsEh
//   _ZN15InteriorGroup_c13SetupShopPedsEv
//   _ZN15InteriorGroup_c14UpdateShopPedsEv
//   _ZN10CEntryExit18RequestAmbientPedsEv
//   _ZN17CEntryExitManager17ms_entryExitStackE
//   _ZN17CEntryExitManager21ms_entryExitStackPosnE
//
// Implementation sketch:
//   1. Resolve symbols in install_vanilla_qol_fixes() via xdl_sym.
//   2. After menu read-save settles (!is_save_load_active(), player valid):
//        a. CPed* player = FindPlayerPed(0);
//        b. InteriorGroup* grp = InteriorManager::GetPedsInteriorGroup(player);
//        c. If grp is a shop group (InteriorGroup::ContainsInteriorType / RE shop type id):
//             - Call SetupShopPeds() once, or ActivatePeds(groupIndex) if grp already active.
//             - Guard with std::atomic<bool> g_shop_peds_rehydrated_this_load to avoid duplicates.
//        d. Fallback: read CEntryExitManager::ms_entryExitStack top; call RequestAmbientPeds on it.
//   3. Invoke from poll_vanilla_qol_fixes() when save-load session just ended (edge detect).
//   4. Log: "🛠️ [QoL] Rehydrated shop peds after interior save-load".
//   5. Test: auto-save in gun shop → load → clerk present, no double spawn on re-enter.
//
// Risks: wrong interior type → duplicate peds; call too early (streaming) → null grp.
// Mitigate: defer until is_player_world_active() && GameState==IDLE && streaming queue==0.
//
// void try_rehydrate_interior_shop_peds();  // TODO(qol-001)

// ---------------------------------------------------------------------
// TODO(qol-002): placeholder for future vanilla fixes
// ---------------------------------------------------------------------
// Examples: stuck fade after failed load, touch HUD edge cases, etc.
// Keep each fix behind its own helper + idempotency flag.

static bool g_vanilla_qol_installed = false;

} // namespace

void install_vanilla_qol_fixes(void* lib_handle) {
    (void)lib_handle;
    if (g_vanilla_qol_installed) return;
    g_vanilla_qol_installed = true;
    LOGI("🛠️ [VanillaQoL] Module loaded (no fixes active yet — see TODO in vanilla_qol_fixes.cpp)");
    // TODO(qol-001): xdl_sym resolve InteriorManager / CEntryExit symbols here.
    // TODO(qol-001): optional shadowhook on AfterSuccessLoad for one-shot defer flag.
}

void poll_vanilla_qol_fixes() {
    // Called each frame from poll_save_load_transition() after hydration poll.
    // TODO(qol-001): if (!is_save_load_active() && pending_interior_rehydrate) try_rehydrate_interior_shop_peds();
}