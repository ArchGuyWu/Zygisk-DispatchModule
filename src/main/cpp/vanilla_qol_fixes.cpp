#include "vanilla_qol_fixes.hpp"

#include "shadowhook.h"
#include "third_party/xdl/xdl.h"

#include "log.hpp"
#include "game_config.hpp"
#include "mod_shared.hpp"

// =====================================================================
// Vanilla QoL fixes (engine bugs / UX gaps — NOT dispatch or stability)
// =====================================================================

namespace {

// --- Touch HUD rehydrate (missing right-side buttons after load / skip) ---
typedef void (*fn_TouchVoid_t)();
static fn_TouchVoid_t g_touch_create_all = nullptr;
static fn_TouchVoid_t g_touch_setup_layout = nullptr;

static std::atomic<bool> g_touch_rehydrate_pending{false};
static std::atomic<int64_t> g_touch_rehydrate_not_before_ms{0};
static std::atomic<bool> g_prev_save_load_active{false};
static constexpr int64_t kTouchRehydrateDeferMs = 500;

static void schedule_touch_rehydrate(const char* reason) {
    g_touch_rehydrate_pending.store(true, std::memory_order_release);
    g_touch_rehydrate_not_before_ms.store(now_ms() + kTouchRehydrateDeferMs,
                                          std::memory_order_release);
    LOGI("🛠️ [VanillaQoL] Touch rehydrate scheduled — %s", reason);
}

static void try_rehydrate_touch_controls() {
    if (!g_touch_rehydrate_pending.load(std::memory_order_acquire)) return;
    if (now_ms() < g_touch_rehydrate_not_before_ms.load(std::memory_order_acquire)) return;
    if (is_save_load_active() || is_scene_transition_active()) return;
    if (!is_player_world_active()) return;

    if (g_touch_create_all) {
        g_touch_create_all();
    }
    if (g_touch_setup_layout) {
        g_touch_setup_layout();
    }

    g_touch_rehydrate_pending.store(false, std::memory_order_release);
    LOGI("🛠️ [VanillaQoL] Touch controls rehydrated (CreateAll + SetupLayoutObjects)");
}

// --- StartGameScreen::OnLoadGame — menu read-save entry (DE may run before GenericLoad) ---
typedef void (*fn_StartGameScreenOnLoadGame_t)(void* self);
static fn_StartGameScreenOnLoadGame_t g_orig_start_game_screen_on_load_game = nullptr;
static void* g_stub_start_game_screen_on_load_game = nullptr;

void proxy_start_game_screen_on_load_game(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    notify_menu_read_save_path("StartGameScreen::OnLoadGame");
    SHADOWHOOK_CALL_PREV(proxy_start_game_screen_on_load_game, self);
}

// ---------------------------------------------------------------------
// TODO(qol-001): Interior save-load — shop clerk / ambient ped missing
// ---------------------------------------------------------------------
// void try_rehydrate_interior_shop_peds();  // TODO(qol-001)

// ---------------------------------------------------------------------
// TODO(qol-002): Vehicle ground sink — body partially buried, cannot drive out
// ---------------------------------------------------------------------
// void try_unwedge_vehicle_if_sunk(void* veh, bool is_player_vehicle);  // TODO(qol-002)
// void scan_nearby_npc_vehicles_for_sink();  // TODO(qol-002)

static bool g_vanilla_qol_installed = false;

} // namespace

void vanilla_qol_on_save_load_session_ended() {
    schedule_touch_rehydrate("save-load session ended");
}

void vanilla_qol_on_skip_pipeline_cleared() {
    schedule_touch_rehydrate("skip pipeline cleared");
}

void install_vanilla_qol_fixes(void* lib_handle) {
    if (g_vanilla_qol_installed) return;

    if (lib_handle) {
        g_touch_create_all = reinterpret_cast<fn_TouchVoid_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface9CreateAllEv", nullptr));
        g_touch_setup_layout = reinterpret_cast<fn_TouchVoid_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface18SetupLayoutObjectsEv", nullptr));
        if (g_touch_create_all && g_touch_setup_layout) {
            LOGI("🛠️ [VanillaQoL] Resolved CTouchInterface::CreateAll / SetupLayoutObjects");
        } else {
            LOGW("🛠️ [VanillaQoL] CTouchInterface symbols missing — touch rehydrate disabled");
        }

        g_stub_start_game_screen_on_load_game = shadowhook_hook_sym_name(
            TARGET_LIB,
            "_ZN15StartGameScreen10OnLoadGameEv",
            reinterpret_cast<void*>(proxy_start_game_screen_on_load_game),
            reinterpret_cast<void**>(&g_orig_start_game_screen_on_load_game));
        if (g_stub_start_game_screen_on_load_game) {
            LOGI("🛠️ [VanillaQoL] Hooked StartGameScreen::OnLoadGame");
        } else {
            LOGW("🛠️ [VanillaQoL] Failed to hook StartGameScreen::OnLoadGame: %s",
                 shadowhook_to_errmsg(shadowhook_get_errno()));
        }
    }

    g_vanilla_qol_installed = true;
    LOGI("🛠️ [VanillaQoL] Module installed");
}

void poll_vanilla_qol_fixes() {
    const bool save_load = is_save_load_active();
    const bool prev = g_prev_save_load_active.exchange(save_load, std::memory_order_acq_rel);
    if (prev && !save_load) {
        schedule_touch_rehydrate("is_save_load_active cleared");
    }
    try_rehydrate_touch_controls();
    // TODO(qol-001): interior shop peds
    // TODO(qol-002): vehicle ground sink
}