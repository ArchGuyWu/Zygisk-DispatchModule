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
typedef void (*fn_TouchDrawAll_t)(bool);
typedef void (*fn_RemoveWidgetFlag_t)(int widget_id, int flag);
typedef void (*fn_ProcessLatentWidget_t)(int widget_id, bool);
typedef bool (*fn_IsWidgetEnabled_t)(int widget_id);
typedef bool (*fn_LoadDataInSlot_t)(void* self, int slot);
typedef void (*fn_LoadDataInSlotThunk_t)(void* self, int slot);
typedef void (*fn_LoadDataInSlotEvent_t)(void* self, bool success, int slot);
typedef void (*fn_StartNewGameFromMenu_t)(void* self);
typedef void (*fn_CutsceneMgrVoid_t)();
typedef void* (*fn_LoadGameFromSlot_t)(const void* slot_name, int user_index);

static fn_TouchVoid_t g_touch_load_defaults = nullptr;
static fn_TouchVoid_t g_touch_create_all = nullptr;
static fn_TouchVoid_t g_touch_setup_layout = nullptr;
static fn_TouchVoid_t g_touch_setup_steering = nullptr;
static fn_TouchVisualizeAll_t g_touch_visualize_all = nullptr;
static fn_TouchDrawAll_t g_touch_draw_all = nullptr;
static fn_TouchVoid_t g_touch_process_latent_all = nullptr;
static fn_RemoveWidgetFlag_t g_touch_remove_widget_flag = nullptr;
static fn_RemoveWidgetFlag_t g_touch_remove_button_flag = nullptr;
static fn_ProcessLatentWidget_t g_touch_process_latent_widget = nullptr;
static bool (*g_touch_is_widget_enabled)(int) = nullptr;
static fn_CutsceneMgrVoid_t g_cutscene_resume_cutscene_and_game = nullptr;
static fn_CutsceneMgrVoid_t g_cutscene_finish_cutscene = nullptr;

static std::atomic<bool> g_touch_rehydrate_pending{false};
static std::atomic<int64_t> g_touch_rehydrate_not_before_ms{0};
static std::atomic<int> g_touch_rehydrate_attempts{0};
static std::atomic<bool> g_prev_save_load_session{false};
static constexpr int64_t kTouchRehydrateDeferMs = 500;
static constexpr int kTouchRehydrateMaxAttempts = 30;
static constexpr int64_t kTouchRehydrateRetryMs = 500;

static constexpr int kGameplayWidgetIds[] = {
    0, 1, 22, 154, 161, 150, 160,
};

static void schedule_touch_rehydrate(const char* reason) {
    g_touch_rehydrate_pending.store(true, std::memory_order_release);
    g_touch_rehydrate_attempts.store(0, std::memory_order_release);
    g_touch_rehydrate_not_before_ms.store(now_ms() + kTouchRehydrateDeferMs,
                                          std::memory_order_release);
    LOGI("🛠️ [VanillaQoL] Touch rehydrate scheduled — %s", reason);
}

static void try_restore_hud_after_skip_cutscene() {
    if (g_cutscene_resume_cutscene_and_game) {
        g_cutscene_resume_cutscene_and_game();
        LOGI("🛠️ [VanillaQoL] Called CCutsceneMgr::ResumeCutsceneAndGame");
    } else if (g_cutscene_finish_cutscene) {
        g_cutscene_finish_cutscene();
        LOGI("🛠️ [VanillaQoL] Called CCutsceneMgr::FinishCutscene");
    }
}

static bool touch_rehydrate_world_ready() {
    if (is_scene_transition_active()) return false;
    if (!is_player_world_active()) return false;
    if (is_skip_cutscene_pipeline_active()) return false;
    if (is_disk_deserialize_active()) return false;
    return true;
}

static void clear_widget_hide_flags(int widget_id) {
    if (!g_touch_remove_widget_flag && !g_touch_remove_button_flag) return;
    for (int bit = 0; bit < 16; ++bit) {
        const int flag = 1 << bit;
        if (g_touch_remove_widget_flag) {
            g_touch_remove_widget_flag(widget_id, flag);
        }
        if (g_touch_remove_button_flag) {
            g_touch_remove_button_flag(widget_id, flag);
        }
    }
}

static void unhide_gameplay_action_widgets() {
    for (int widget_id : kGameplayWidgetIds) {
        clear_widget_hide_flags(widget_id);
        if (g_touch_process_latent_widget) {
            g_touch_process_latent_widget(widget_id, true);
        }
    }
}

static bool core_action_widgets_enabled() {
    if (!g_touch_is_widget_enabled) return false;
    return g_touch_is_widget_enabled(0) &&
           g_touch_is_widget_enabled(1) &&
           g_touch_is_widget_enabled(161);
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
    unhide_gameplay_action_widgets();
    if (g_touch_visualize_all) {
        g_touch_visualize_all(true);
    }
    if (g_touch_draw_all) {
        g_touch_draw_all(true);
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

    if (core_action_widgets_enabled()) {
        g_touch_rehydrate_pending.store(false, std::memory_order_release);
        LOGI("🛠️ [VanillaQoL] Action widgets enabled after attempt %d "
             "(enter=%d attack=%d sprint=%d)",
             attempt,
             g_touch_is_widget_enabled(0) ? 1 : 0,
             g_touch_is_widget_enabled(1) ? 1 : 0,
             g_touch_is_widget_enabled(161) ? 1 : 0);
        return;
    }

    if (attempt < kTouchRehydrateMaxAttempts) {
        g_touch_rehydrate_not_before_ms.store(now_ms() + kTouchRehydrateRetryMs,
                                              std::memory_order_release);
        LOGI("🛠️ [VanillaQoL] Touch chain applied (attempt %d/%d, action widgets pending)",
             attempt, kTouchRehydrateMaxAttempts);
        return;
    }

    g_touch_rehydrate_pending.store(false, std::memory_order_release);
    LOGW("🛠️ [VanillaQoL] Touch rehydrate finished — action widgets still disabled "
         "(enter=%d attack=%d sprint=%d)",
         g_touch_is_widget_enabled ? (g_touch_is_widget_enabled(0) ? 1 : 0) : -1,
         g_touch_is_widget_enabled ? (g_touch_is_widget_enabled(1) ? 1 : 0) : -1,
         g_touch_is_widget_enabled ? (g_touch_is_widget_enabled(161) ? 1 : 0) : -1);
}

// --- Menu read-save / new-game entry hooks ---
static fn_LoadDataInSlot_t g_orig_san_andreas_load_data_in_slot = nullptr;
static void* g_stub_san_andreas_load_data_in_slot = nullptr;
static fn_LoadDataInSlotThunk_t g_orig_gameterface_load_data_in_slot = nullptr;
static void* g_stub_gameterface_load_data_in_slot = nullptr;
static fn_LoadDataInSlotEvent_t g_orig_ui_menu_load_data_in_slot_event = nullptr;
static void* g_stub_ui_menu_load_data_in_slot_event = nullptr;

static fn_StartNewGameFromMenu_t g_orig_gameterface_start_new_game = nullptr;
static void* g_stub_gameterface_start_new_game = nullptr;
static fn_StartNewGameFromMenu_t g_orig_san_andreas_start_new_game = nullptr;
static void* g_stub_san_andreas_start_new_game = nullptr;
static fn_CutsceneMgrVoid_t g_orig_skip_cutscene = nullptr;
static void* g_stub_skip_cutscene = nullptr;
static fn_CutsceneMgrVoid_t g_orig_jump_to_new_game = nullptr;
static void* g_stub_jump_to_new_game = nullptr;

bool proxy_san_andreas_load_data_in_slot(void* self, int slot) {
    SHADOWHOOK_STACK_SCOPE();
    notify_menu_gameterface_self(self);
    notify_menu_read_save_path("USanAndreasInterface::LoadDataInSlot");
    LOGI("💾 [SaveLoad] USanAndreasInterface::LoadDataInSlot(slot=%d)", slot);
    const bool ok = SHADOWHOOK_CALL_PREV(proxy_san_andreas_load_data_in_slot, self, slot);
    LOGI("💾 [SaveLoad] USanAndreasInterface::LoadDataInSlot(slot=%d) returned — ok=%d",
         slot, ok ? 1 : 0);
    poll_save_load_hydration_state();
    return ok;
}

void proxy_gameterface_load_data_in_slot(void* self, int slot) {
    SHADOWHOOK_STACK_SCOPE();
    notify_menu_gameterface_self(self);
    notify_menu_read_save_path("UGameterface::LoadDataInSlot");
    LOGI("💾 [SaveLoad] UGameterface::LoadDataInSlot(slot=%d) — awaiting commit event", slot);
    SHADOWHOOK_CALL_PREV(proxy_gameterface_load_data_in_slot, self, slot);
}

void proxy_ui_menu_load_data_in_slot_event(void* self, bool success, int slot) {
    SHADOWHOOK_STACK_SCOPE();
    if (success) {
        notify_ue_menu_load_committed("UUI_Menu_Base::LoadDataInSlotEvent", slot);
    } else {
        LOGW("💾 [SaveLoad] UUI_Menu_Base::LoadDataInSlotEvent(slot=%d) — failed", slot);
    }
    SHADOWHOOK_CALL_PREV(proxy_ui_menu_load_data_in_slot_event, self, success, slot);
}

void proxy_gameterface_start_new_game_from_menu(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    notify_explicit_new_game_bootstrap("UGameterface::StartNewGameFromMenu");
    SHADOWHOOK_CALL_PREV(proxy_gameterface_start_new_game_from_menu, self);
}

void proxy_san_andreas_start_new_game_from_menu(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    notify_explicit_new_game_bootstrap("USanAndreasInterface::StartNewGameFromMenu");
    SHADOWHOOK_CALL_PREV(proxy_san_andreas_start_new_game_from_menu, self);
}

void proxy_jump_to_new_game(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    notify_explicit_new_game_bootstrap("UGameterface::JumpToNewGame");
    SHADOWHOOK_CALL_PREV(proxy_jump_to_new_game, self);
}

void proxy_skip_cutscene() {
    SHADOWHOOK_STACK_SCOPE();
    SHADOWHOOK_CALL_PREV(proxy_skip_cutscene);
    schedule_touch_rehydrate("CCutsceneMgr::SkipCutscene");
}

static fn_LoadGameFromSlot_t g_orig_load_game_from_slot = nullptr;
static void* g_stub_load_game_from_slot = nullptr;

void* proxy_load_game_from_slot(const void* slot_name, int user_index) {
    SHADOWHOOK_STACK_SCOPE();
    notify_ue_menu_load_committed("UGameplayStatics::LoadGameFromSlot", user_index);
    return SHADOWHOOK_CALL_PREV(proxy_load_game_from_slot, slot_name, user_index);
}

static bool g_vanilla_qol_installed = false;

} // namespace

void vanilla_qol_on_save_load_session_ended() {
    schedule_touch_rehydrate("save-load session ended");
}

void vanilla_qol_on_skip_pipeline_cleared() {
    try_restore_hud_after_skip_cutscene();
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
        g_touch_draw_all = reinterpret_cast<fn_TouchDrawAll_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface7DrawAllEb", nullptr));
        g_touch_process_latent_all = reinterpret_cast<fn_TouchVoid_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface32ProcessLatentEventsForAllWidgetsEv", nullptr));
        g_touch_remove_widget_flag = reinterpret_cast<fn_RemoveWidgetFlag_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface16RemoveWidgetFlagENS_9WidgetIDsEi", nullptr));
        g_touch_remove_button_flag = reinterpret_cast<fn_RemoveWidgetFlag_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface16RemoveButtonFlagENS_9WidgetIDsEi", nullptr));
        g_touch_process_latent_widget = reinterpret_cast<fn_ProcessLatentWidget_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface28ProcessLatentEventsForWidgetENS_9WidgetIDsEb",
            nullptr));
        g_touch_is_widget_enabled = reinterpret_cast<fn_IsWidgetEnabled_t>(xdl_sym(
            lib_handle, "_ZN15CTouchInterface15IsWidgetEnabledENS_9WidgetIDsE", nullptr));
        g_cutscene_resume_cutscene_and_game = reinterpret_cast<fn_CutsceneMgrVoid_t>(xdl_sym(
            lib_handle, "_ZN12CCutsceneMgr21ResumeCutsceneAndGameEv", nullptr));
        g_cutscene_finish_cutscene = reinterpret_cast<fn_CutsceneMgrVoid_t>(xdl_sym(
            lib_handle, "_ZN12CCutsceneMgr14FinishCutsceneEv", nullptr));

        if (g_touch_create_all && g_touch_setup_layout && g_touch_visualize_all) {
            LOGI("🛠️ [VanillaQoL] Resolved CTouchInterface rehydrate chain");
        } else {
            LOGW("🛠️ [VanillaQoL] CTouchInterface symbols missing — touch rehydrate disabled");
        }

        #define HOOK_QOL_SYM(mangled, proxy_fn, stub_var, orig_var, label) do { \
            (stub_var) = shadowhook_hook_sym_name( \
                TARGET_LIB, (mangled), reinterpret_cast<void*>(proxy_fn), \
                reinterpret_cast<void**>(&(orig_var))); \
            if (stub_var) LOGI("🛠️ [VanillaQoL] Hooked %s", (label)); \
            else LOGW("🛠️ [VanillaQoL] Failed to hook %s: %s", (label), \
                     shadowhook_to_errmsg(shadowhook_get_errno())); \
        } while (0)

        HOOK_QOL_SYM("_ZN13UUI_Menu_Base19LoadDataInSlotEventEbi",
                     proxy_ui_menu_load_data_in_slot_event,
                     g_stub_ui_menu_load_data_in_slot_event,
                     g_orig_ui_menu_load_data_in_slot_event,
                     "UUI_Menu_Base::LoadDataInSlotEvent");
        HOOK_QOL_SYM("_ZN12UGameterface14LoadDataInSlotEi",
                     proxy_gameterface_load_data_in_slot,
                     g_stub_gameterface_load_data_in_slot,
                     g_orig_gameterface_load_data_in_slot,
                     "UGameterface::LoadDataInSlot");
        HOOK_QOL_SYM("_ZN20USanAndreasInterface14LoadDataInSlotEi",
                     proxy_san_andreas_load_data_in_slot,
                     g_stub_san_andreas_load_data_in_slot,
                     g_orig_san_andreas_load_data_in_slot,
                     "USanAndreasInterface::LoadDataInSlot");
        HOOK_QOL_SYM("_ZN12UGameterface20StartNewGameFromMenuEv",
                     proxy_gameterface_start_new_game_from_menu,
                     g_stub_gameterface_start_new_game,
                     g_orig_gameterface_start_new_game,
                     "UGameterface::StartNewGameFromMenu");
        HOOK_QOL_SYM("_ZN20USanAndreasInterface20StartNewGameFromMenuEv",
                     proxy_san_andreas_start_new_game_from_menu,
                     g_stub_san_andreas_start_new_game,
                     g_orig_san_andreas_start_new_game,
                     "USanAndreasInterface::StartNewGameFromMenu");
        HOOK_QOL_SYM("_ZN12UGameterface13JumpToNewGameEv",
                     proxy_jump_to_new_game,
                     g_stub_jump_to_new_game,
                     g_orig_jump_to_new_game,
                     "UGameterface::JumpToNewGame");
        HOOK_QOL_SYM("_ZN12CCutsceneMgr12SkipCutsceneEv",
                     proxy_skip_cutscene,
                     g_stub_skip_cutscene,
                     g_orig_skip_cutscene,
                     "CCutsceneMgr::SkipCutscene");
        HOOK_QOL_SYM("_ZN16UGameplayStatics16LoadGameFromSlotERK7FStringi",
                     proxy_load_game_from_slot,
                     g_stub_load_game_from_slot,
                     g_orig_load_game_from_slot,
                     "UGameplayStatics::LoadGameFromSlot");
        #undef HOOK_QOL_SYM
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
}

bool vanilla_qol_touch_rehydrate_pending() {
    return g_touch_rehydrate_pending.load(std::memory_order_acquire);
}