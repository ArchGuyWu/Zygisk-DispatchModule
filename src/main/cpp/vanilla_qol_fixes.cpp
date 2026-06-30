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
// TODO(qol-002): Vehicle ground sink — body partially buried, cannot drive out
// ---------------------------------------------------------------------
// Symptom (vanilla, rare): after collision / streaming / warp / save-load, a vehicle
// clips into the terrain; wheels spin but the body stays wedged below ground.
//
// Root cause: physics/collision desync — entity matrix Z below ground mesh; engine
// stuck-car recovery may not trigger for partial embed (not full "stuck" flag).
//
// Suggested libUE4 symbols (nm -D libUE4.so):
//   _ZN11CAutomobile19PlaceOnRoadProperlyEv
//   _ZN5CBike19PlaceOnRoadProperlyEv
//   _ZN14CStuckCarCheck20AttemptToWarpVehicleEP8CVehicleR7CVectorf
//   _ZN14CStuckCarCheck24HasCarBeenStuckForAWhileEi
//   _ZN14CStuckCarCheck7ProcessEv
//   _ZN6CWorld21FindGroundZFor3DCoordEfffPbPP7CEntityP7CVector
//   _ZN11CAutomobile21GetAllWheelsOffGroundEv
//   _ZN4AGTAVehicle14UpdateOnGroundEb  (UE layer — verify before hooking)
//
// Detection sketch (player vehicle only — do not scan entire pool every frame):
//   1. Throttle: check at most every 2s while player is driver (not on foot).
//   2. CVector pos = get_entity_pos(veh); float groundZ;
//      CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.f, ..., &groundZ);
//   3. float embed = groundZ - pos.z;  // positive => center below ground sample
//   4. Trigger if embed > kSinkThreshold (e.g. 0.35–0.5 m) for N consecutive checks
//      AND speed near zero AND player pressing accelerate (wheels spinning, no progress).
//   5. Optional: confirm with wheel contact — GetAllWheelsOffGround or col point probe.
//
// Recovery sketch (prefer engine-native fix over raw teleport):
//   a. Try CAutomobile::PlaceOnRoadProperly() / CBike::PlaceOnRoadProperly() on veh.
//   b. If still embedded: CStuckCarCheck::AttemptToWarpVehicle(veh, outPos, heading).
//   c. Last resort: set_entity_pos(veh, {x, y, groundZ + kClearance}) + zero velocity
//      (reuse mod_shared helpers — only after (a)(b) fail; log as last resort).
//   d. Cooldown per vehicle handle (e.g. 30s) so we do not fight physics every frame.
//
// Integration:
//   - poll_vanilla_qol_fixes() when is_mod_gameplay_active() && !is_scene_transition_active().
//   - Skip during is_save_load_active() and first N seconds after session end.
//   - Log: "🛠️ [QoL] Unwedged player vehicle (embed=%.2f method=PlaceOnRoad|Warp|Nudge)".
//
// Risks: false positive on slopes/ramps (embed probe on incline); multiplayer N/A.
// Mitigate: require low pitch/roll, multiple samples under bbox corners, min stuck duration.
//
// void try_unwedge_player_vehicle_if_sunk();  // TODO(qol-002)

static bool g_vanilla_qol_installed = false;

} // namespace

void install_vanilla_qol_fixes(void* lib_handle) {
    (void)lib_handle;
    if (g_vanilla_qol_installed) return;
    g_vanilla_qol_installed = true;
    LOGI("🛠️ [VanillaQoL] Module loaded (no fixes active yet — see TODO in vanilla_qol_fixes.cpp)");
    // TODO(qol-001): xdl_sym resolve InteriorManager / CEntryExit symbols here.
    // TODO(qol-001): optional shadowhook on AfterSuccessLoad for one-shot defer flag.
    // TODO(qol-002): xdl_sym resolve PlaceOnRoadProperly / CStuckCarCheck / FindGroundZFor3DCoord.
}

void poll_vanilla_qol_fixes() {
    // Called each frame from poll_save_load_transition() after hydration poll.
    // TODO(qol-001): if (!is_save_load_active() && pending_interior_rehydrate) try_rehydrate_interior_shop_peds();
    // TODO(qol-002): if (is_mod_gameplay_active()) try_unwedge_player_vehicle_if_sunk();
}