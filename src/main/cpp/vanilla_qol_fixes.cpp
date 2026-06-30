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
typedef void (*fn_TouchVisualizeAll_t)(bool);
typedef void (*fn_MobileMenuVoid_t)(void* self);
typedef void* (*fn_LoadGameFromSlot_t)(const void* slot_name, int user_index);

static fn_TouchVoid_t g_touch_load_defaults = nullptr;
static fn_TouchVoid_t g_touch_create_all = nullptr;
static fn_TouchVoid_t g_touch_setup_layout = nullptr;
static fn_TouchVoid_t g_touch_setup_steering = nullptr;
static fn_TouchVisualizeAll_t g_touch_visualize_all = nullptr;
static fn_TouchVoid_t g_touch_process_latent_all = nullptr;

static std::atomic<bool> g_touch_rehydrate_pending{false};
static std::atomic<int64_t> g_touch_rehydrate_not_before_ms{0};
static std::atomic<int> g_touch_rehydrate_attempts{0};
static std::atomic<bool> g_prev_save_load_session{false};
static constexpr int64_t kTouchRehydrateDeferMs = 500;
static constexpr int kTouchRehydrateMaxAttempts = 30;
static constexpr int64_t kTouchRehydrateRetryMs = 500;

static void schedule_touch_rehydrate(const char* reason) {
    g_touch_rehydrate_pending.store(true, std::memory_order_release);
    g_touch_rehydrate_attempts.store(0, std::memory_order_release);
    g_touch_rehydrate_not_before_ms.store(now_ms() + kTouchRehydrateDeferMs,
                                          std::memory_order_release);
    LOGI("🛠️ [VanillaQoL] Touch rehydrate scheduled — %s", reason);
}

static bool touch_rehydrate_world_ready() {
    if (is_scene_transition_active()) return false;
    if (is_save_load_session_or_loading()) return false;
    if (is_skip_cutscene_pipeline_active()) return false;
    return is_player_world_active();
}

static void run_touch_rehydrate_chain() {
    if (g_touch_load_defaults) {
        g_touch_load_defaults();
    }
    if (g_touch_create_all) {
        g_touch_create_all();
    }
    if (g_touch_setup_layout) {
        g_touch_setup_layout();
    }
    if (g_touch_setup_steering) {
        g_touch_setup_steering();
    }
    if (g_touch_visualize_all) {
        g_touch_visualize_all(true);
    }
    if (g_touch_process_latent_all) {
        g_touch_process_latent_all();
    }
}

static void try_rehydrate_touch_controls() {
    if (!g_touch_rehydrate_pending.load(std::memory_order_acquire)) return;
    if (now_ms() < g_touch_rehydrate_not_before_ms.load(std::memory_order_acquire)) return;
    if (!touch_rehydrate_world_ready()) return;

    const bool have_chain =
        g_touch_create_all && g_touch_setup_layout && g_touch_visualize_all;
    if (!have_chain) {
        LOGW("🛠️ [VanillaQoL] Touch rehydrate skipped — CTouchInterface symbols missing");
        g_touch_rehydrate_pending.store(false, std::memory_order_release);
        return;
    }

    run_touch_rehydrate_chain();
    const int attempt =
        g_touch_rehydrate_attempts.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (attempt < kTouchRehydrateMaxAttempts) {
        g_touch_rehydrate_not_before_ms.store(now_ms() + kTouchRehydrateRetryMs,
                                              std::memory_order_release);
        LOGI("🛠️ [VanillaQoL] Touch chain applied (attempt %d/%d)",
             attempt, kTouchRehydrateMaxAttempts);
        return;
    }

    g_touch_rehydrate_pending.store(false, std::memory_order_release);
    LOGI("🛠️ [VanillaQoL] Touch controls rehydrated — full chain after %d attempts",
         attempt);
}

// --- Menu read-save entry hooks (DE has no StartGameScreen::OnLoadGame) ---
static fn_MobileMenuVoid_t g_orig_mobile_menu_load = nullptr;
static void* g_stub_mobile_menu_load = nullptr;

void proxy_mobile_menu_load(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    notify_menu_read_save_path("MobileMenu::Load");
    SHADOWHOOK_CALL_PREV(proxy_mobile_menu_load, self);
}

static fn_LoadGameFromSlot_t g_orig_load_game_from_slot = nullptr;
static void* g_stub_load_game_from_slot = nullptr;

void* proxy_load_game_from_slot(const void* slot_name, int user_index) {
    SHADOWHOOK_STACK_SCOPE();
    notify_menu_read_save_path("UGameplayStatics::LoadGameFromSlot");
    LOGI("💾 [SaveLoad] UGameplayStatics::LoadGameFromSlot(userIndex=%d)", user_index);
    return SHADOWHOOK_CALL_PREV(proxy_load_game_from_slot, slot_name, user_index);
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

void vanilla_qol_on_deserialize_complete() {
    schedule_touch_rehydrate("deserialize complete");
}

void install_vanilla_qol_fixes(void* lib_handle) {
    if (g_vanilla_qol_installed) return;

    if (lib_handle) {
        g_touch_load_defaults = reinterpret_cast<fn_TouchVoid_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface26LoadDefaultWidgetPositionsEv", nullptr));
        g_touch_create_all = reinterpret_cast<fn_TouchVoid_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface9CreateAllEv", nullptr));
        g_touch_setup_layout = reinterpret_cast<fn_TouchVoid_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface18SetupLayoutObjectsEv", nullptr));
        g_touch_setup_steering = reinterpret_cast<fn_TouchVoid_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface17SetupSteeringModeEv", nullptr));
        g_touch_visualize_all = reinterpret_cast<fn_TouchVisualizeAll_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface12VisualizeAllEb", nullptr));
        g_touch_process_latent_all = reinterpret_cast<fn_TouchVoid_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface32ProcessLatentEventsForAllWidgetsEv", nullptr));

        if (g_touch_create_all && g_touch_setup_layout && g_touch_visualize_all) {
            LOGI("🛠️ [VanillaQoL] Resolved CTouchInterface rehydrate chain");
        } else {
            LOGW("🛠️ [VanillaQoL] CTouchInterface symbols missing — touch rehydrate disabled");
        }

        g_stub_mobile_menu_load = shadowhook_hook_sym_name(
            TARGET_LIB,
            "_ZN10MobileMenu4LoadEv",
            reinterpret_cast<void*>(proxy_mobile_menu_load),
            reinterpret_cast<void**>(&g_orig_mobile_menu_load));
        if (g_stub_mobile_menu_load) {
            LOGI("🛠️ [VanillaQoL] Hooked MobileMenu::Load");
        } else {
            LOGW("🛠️ [VanillaQoL] Failed to hook MobileMenu::Load: %s",
                 shadowhook_to_errmsg(shadowhook_get_errno()));
        }

        g_stub_load_game_from_slot = shadowhook_hook_sym_name(
            TARGET_LIB,
            "_ZN16UGameplayStatics16LoadGameFromSlotERK7FStringi",
            reinterpret_cast<void*>(proxy_load_game_from_slot),
            reinterpret_cast<void**>(&g_orig_load_game_from_slot));
        if (g_stub_load_game_from_slot) {
            LOGI("🛠️ [VanillaQoL] Hooked UGameplayStatics::LoadGameFromSlot");
        } else {
            LOGW("🛠️ [VanillaQoL] Failed to hook LoadGameFromSlot: %s",
                 shadowhook_to_errmsg(shadowhook_get_errno()));
        }
    }

    g_vanilla_qol_installed = true;
    LOGI("🛠️ [VanillaQoL] Module installed");
}

void poll_vanilla_qol_fixes() {
    const bool session = is_save_load_session_or_loading();
    const bool prev =
        g_prev_save_load_session.exchange(session, std::memory_order_acq_rel);
    if (prev && !session) {
        schedule_touch_rehydrate("save-load session cleared");
    }
    try_rehydrate_touch_controls();
    // TODO(qol-001): interior shop peds
    // TODO(qol-002): vehicle ground sink
}

bool vanilla_qol_touch_rehydrate_pending() {
    return g_touch_rehydrate_pending.load(std::memory_order_acquire);
}