#include <jni.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <vector>
#include <map>
#include <unordered_map>
#include <fstream>
#include <string>
#include <cinttypes>
#include <set>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <random>
#include <functional>
#include <cmath>

#include "shadowhook.h"
#include "third_party/xdl/xdl.h"

#include "log.hpp"
#include "game_config.hpp"
#include "game_types.hpp"
#include "pointer_sanitizer.hpp"
#include "mod_shared.hpp"
#include "vanilla_qol_fixes.hpp"
#include "ecs_engine.hpp"

// =====================================================================
// Scene transition / interior entry state (yellow marker warp UX)
// =====================================================================
static std::atomic<bool> g_mod_interior_transition{false};
bool* g_entry_exit_ms_bWarping = nullptr;
fn_WeAreInInteriorTransition_t g_we_are_in_interior_transition = nullptr;
bool* g_generic_game_storage_ms_bLoading = nullptr;
bool* g_generic_game_storage_ms_b_failed = nullptr;
uint8_t* g_cgame_logic_game_state = nullptr;
uint32_t* g_streaming_ms_num_models_requested = nullptr;
uint32_t* g_cgame_logic_skip_state = nullptr;
uint32_t* g_cgame_logic_skip_timer = nullptr;
fn_IsSkipWaitingForScriptToFadeIn_t g_is_skip_waiting_for_script_to_fade_in = nullptr;

// eGameState (gta-reversed): 7=FRONTEND_IDLE, 8=LOADING_STARTED, 9=IDLE (in-game).
static constexpr uint8_t kGameStateFrontendIdle = 7;
static constexpr uint8_t kGameStateLoadingStarted = 8;
static constexpr uint8_t kGameStateIdle = 9;

// Session hold is signal-driven (ms_bLoading / GameState / streaming / skip), not a fixed
// post-spawn timer. kAbandonedSessionMs / kSessionHardCapMs are safety nets only.
static std::atomic<bool> g_save_load_session{false};
static std::atomic<int64_t> g_session_started_ms{0};
static std::atomic<int64_t> g_ms_b_loading_cleared_ms{0};
static std::atomic<int64_t> g_hydration_ready_since_ms{0};
static std::atomic<int64_t> g_save_load_quiesce_until_ms{0};
static std::atomic<bool> g_skip_pipeline_was_active{false};
static std::atomic<int64_t> g_skip_pipeline_cleared_ms{0};

// Menu read-save: direct scene, no intro. New game bootstrap may enter skip/intro pipeline.
enum class SaveLoadKind : uint8_t {
    Unknown = 0,
    MenuGenericLoad = 1,
    NewGameBootstrap = 2,
};
static std::atomic<uint8_t> g_save_load_kind{static_cast<uint8_t>(SaveLoadKind::Unknown)};
static std::atomic<bool> g_menu_read_save_path_seen{false};
static std::atomic<bool> g_explicit_new_game_bootstrap{false};

static constexpr int64_t kAbandonedSessionMs = 120000;
static constexpr int64_t kSessionHardCapMs = 180000;
static constexpr int64_t kSessionMinActiveMs = 28000;
static constexpr int64_t kHydrationSignalDwellMs = 15000;
// Main-menu load save (no cutscene): recover ManageTasks / touch HUD quickly.
static constexpr int64_t kDirectLoadSessionMinActiveMs = 5000;
static constexpr int64_t kDirectLoadHydrationDwellMs = 2000;
static constexpr int64_t kPostSkipSessionMinActiveMs = 500;
static constexpr int64_t kPostSkipHydrationDwellMs = 500;
static constexpr int64_t kUltraRelaxedPostSkipDwellMs = 500;
static constexpr int64_t kPostSkipForceSessionEndMs = 1000;
static constexpr int64_t kMinPostLoadingHoldMs = 5000;
static constexpr int64_t kRelaxedPostLoadingHoldMs = 2000;
static constexpr int64_t kSaveLoadPostSessionCooldownMs = 10000;

void begin_save_load_session();

void notify_menu_read_save_path(const char* via) {
    // Kind flags only — do NOT begin session here. Starting session before
    // ms_bLoading/game_state==8 skips ManageTasks during the loading screen (hang).
    g_explicit_new_game_bootstrap.store(false, std::memory_order_release);
    g_menu_read_save_path_seen.store(true, std::memory_order_release);
    g_save_load_kind.store(static_cast<uint8_t>(SaveLoadKind::MenuGenericLoad),
                           std::memory_order_release);
    LOGI("💾 [SaveLoad] Menu read-save via %s (direct scene, no intro expected)", via);
}

void notify_explicit_new_game_bootstrap(const char* via) {
    g_menu_read_save_path_seen.store(false, std::memory_order_release);
    g_explicit_new_game_bootstrap.store(true, std::memory_order_release);
    g_save_load_kind.store(static_cast<uint8_t>(SaveLoadKind::NewGameBootstrap),
                           std::memory_order_release);
    LOGI("💾 [SaveLoad] Explicit new-game via %s (intro may follow)", via);
}

static bool read_ms_b_loading() {
    return g_generic_game_storage_ms_bLoading &&
           is_pointer_readable(g_generic_game_storage_ms_bLoading) &&
           *g_generic_game_storage_ms_bLoading;
}

static bool read_ms_b_load_failed() {
    return g_generic_game_storage_ms_b_failed &&
           is_pointer_readable(g_generic_game_storage_ms_b_failed) &&
           *g_generic_game_storage_ms_b_failed;
}

static bool read_game_state(uint8_t* out) {
    if (!out) return false;
    if (!g_cgame_logic_game_state || !is_pointer_readable(g_cgame_logic_game_state)) return false;
    *out = *g_cgame_logic_game_state;
    return true;
}

static int read_streaming_num_requested() {
    if (!g_streaming_ms_num_models_requested ||
        !is_pointer_readable(g_streaming_ms_num_models_requested)) {
        return -1;
    }
    return static_cast<int>(*g_streaming_ms_num_models_requested);
}

static bool read_skip_pipeline_active() {
    if (g_cgame_logic_skip_state &&
        is_pointer_readable(g_cgame_logic_skip_state) &&
        *g_cgame_logic_skip_state != 0) {
        return true;
    }
    if (g_cgame_logic_skip_timer &&
        is_pointer_readable(g_cgame_logic_skip_timer) &&
        *g_cgame_logic_skip_timer != 0) {
        return true;
    }
    if (g_is_skip_waiting_for_script_to_fade_in &&
        g_is_skip_waiting_for_script_to_fade_in()) {
        return true;
    }
    return false;
}

static bool save_load_hydration_signals_settled() {
    uint8_t game_state = 0;
    const bool have_game_state = read_game_state(&game_state);
    if (read_ms_b_loading()) return false;
    const int64_t loading_cleared =
        g_ms_b_loading_cleared_ms.load(std::memory_order_acquire);
    if (loading_cleared > 0 &&
        now_ms() - loading_cleared < kMinPostLoadingHoldMs) {
        return false;
    }
    if (!is_player_world_active()) return false;
    if (!have_game_state || game_state != kGameStateIdle) return false;

    // Unreadable streaming counter (-1) must not count as "queue empty".
    const int pending = read_streaming_num_requested();
    if (pending != 0) return false;
    if (read_skip_pipeline_active()) return false;
    return true;
}

// Menu read-save / post-skip recovery: do not wait for streaming queue (touch HUD + ManageTasks).
static bool save_load_hydration_signals_settled_relaxed() {
    uint8_t game_state = 0;
    const bool have_game_state = read_game_state(&game_state);
    if (read_ms_b_loading()) return false;
    const int64_t loading_cleared =
        g_ms_b_loading_cleared_ms.load(std::memory_order_acquire);
    if (loading_cleared > 0 &&
        now_ms() - loading_cleared < kRelaxedPostLoadingHoldMs) {
        return false;
    }
    if (!is_player_world_active()) return false;
    if (!have_game_state || game_state != kGameStateIdle) return false;
    if (read_skip_pipeline_active()) return false;
    return true;
}

// Post-skip recovery: player valid + skip cleared — do not require game_state==9.
static bool save_load_hydration_signals_settled_ultra_relaxed() {
    if (read_ms_b_loading()) return false;
    if (!is_player_world_active()) return false;
    if (read_skip_pipeline_active()) return false;
    const int64_t skip_cleared =
        g_skip_pipeline_cleared_ms.load(std::memory_order_acquire);
    if (skip_cleared <= 0) return false;
    if (now_ms() - skip_cleared < kUltraRelaxedPostSkipDwellMs) return false;
    return true;
}

static bool use_relaxed_hydration_settled() {
    const uint8_t kind = g_save_load_kind.load(std::memory_order_acquire);
    if (kind == static_cast<uint8_t>(SaveLoadKind::MenuGenericLoad)) return true;
    const int64_t skip_cleared = g_skip_pipeline_cleared_ms.load(std::memory_order_acquire);
    return skip_cleared > 0 && now_ms() - skip_cleared < 120000;
}

static bool hydration_settled_for_session_end() {
    const int64_t skip_cleared =
        g_skip_pipeline_cleared_ms.load(std::memory_order_acquire);
    const bool post_skip_ultra =
        skip_cleared > 0 && now_ms() - skip_cleared < 120000;
    if (post_skip_ultra) {
        return save_load_hydration_signals_settled_ultra_relaxed();
    }
    if (use_relaxed_hydration_settled()) {
        return save_load_hydration_signals_settled_relaxed();
    }
    return save_load_hydration_signals_settled();
}

void begin_save_load_session() {
    const bool was_active = g_save_load_session.exchange(true, std::memory_order_acq_rel);
    if (!was_active) {
        const int64_t t = now_ms();
        g_session_started_ms.store(t, std::memory_order_release);
        g_ms_b_loading_cleared_ms.store(0, std::memory_order_release);
        g_hydration_ready_since_ms.store(0, std::memory_order_release);
        g_save_load_quiesce_until_ms.store(0, std::memory_order_release);
        g_skip_pipeline_was_active.store(false, std::memory_order_release);
        g_skip_pipeline_cleared_ms.store(0, std::memory_order_release);
        LOGI("💾 [SaveLoad] Session begin");
    }
}

void mark_save_load_quiesce(int64_t /*duration_ms*/) {
    begin_save_load_session();
}

static void end_save_load_session(const char* reason) {
    if (!g_save_load_session.exchange(false, std::memory_order_acq_rel)) return;
    g_session_started_ms.store(0, std::memory_order_release);
    g_ms_b_loading_cleared_ms.store(0, std::memory_order_release);
    g_hydration_ready_since_ms.store(0, std::memory_order_release);
    g_skip_pipeline_was_active.store(false, std::memory_order_release);
    g_skip_pipeline_cleared_ms.store(0, std::memory_order_release);
    g_save_load_kind.store(static_cast<uint8_t>(SaveLoadKind::Unknown),
                           std::memory_order_release);
    g_menu_read_save_path_seen.store(false, std::memory_order_release);
    g_explicit_new_game_bootstrap.store(false, std::memory_order_release);
    g_save_load_quiesce_until_ms.store(
        now_ms() + kSaveLoadPostSessionCooldownMs, std::memory_order_release);
    LOGI("💾 [SaveLoad] Session end — %s (cooldown %lldms)",
         reason, static_cast<long long>(kSaveLoadPostSessionCooldownMs));
    vanilla_qol_on_save_load_session_ended();
}

static void session_timing_thresholds(int64_t* min_active_ms, int64_t* dwell_ms) {
    const uint8_t kind = g_save_load_kind.load(std::memory_order_acquire);
    const int64_t skip_cleared = g_skip_pipeline_cleared_ms.load(std::memory_order_acquire);
    const bool post_skip_fast =
        skip_cleared > 0 && now_ms() - skip_cleared < 120000;

    if (post_skip_fast) {
        *min_active_ms = kPostSkipSessionMinActiveMs;
        *dwell_ms = kPostSkipHydrationDwellMs;
    } else if (kind == static_cast<uint8_t>(SaveLoadKind::MenuGenericLoad)) {
        *min_active_ms = kDirectLoadSessionMinActiveMs;
        *dwell_ms = kDirectLoadHydrationDwellMs;
    } else {
        *min_active_ms = kSessionMinActiveMs;
        *dwell_ms = kHydrationSignalDwellMs;
    }
}

void poll_save_load_hydration_state() {
    static bool prev_ms_loading = false;
    static bool prev_settled = false;
    static bool prev_skip_active = false;
    static uint8_t prev_game_state = kGameStateFrontendIdle;
    static int64_t last_diag_ms = 0;

    const bool ms_loading = read_ms_b_loading();
    uint8_t game_state = 0;
    const bool have_game_state = read_game_state(&game_state);
    const bool skip_active = read_skip_pipeline_active();
    const uint8_t kind = g_save_load_kind.load(std::memory_order_acquire);
    if (skip_active && !prev_skip_active &&
        kind == static_cast<uint8_t>(SaveLoadKind::MenuGenericLoad)) {
        LOGW("💾 [SaveLoad] Menu read-save entered skip/intro pipeline — save likely not applied");
    }
    if (g_skip_pipeline_was_active.load(std::memory_order_acquire) && !skip_active) {
        g_skip_pipeline_cleared_ms.store(now_ms(), std::memory_order_release);
        LOGI("💾 [SaveLoad] Skip pipeline cleared — gameState=%u",
             have_game_state ? game_state : 255u);
        vanilla_qol_on_skip_pipeline_cleared();
    }

    const int64_t skip_cleared_ms =
        g_skip_pipeline_cleared_ms.load(std::memory_order_acquire);
    if (g_save_load_session.load(std::memory_order_acquire) &&
        skip_cleared_ms > 0 &&
        now_ms() - skip_cleared_ms >= kPostSkipForceSessionEndMs &&
        !skip_active && is_player_world_active()) {
        end_save_load_session("post-skip fast recovery");
    }
    g_skip_pipeline_was_active.store(skip_active, std::memory_order_release);

    if (have_game_state &&
        prev_game_state == kGameStateFrontendIdle &&
        game_state != kGameStateFrontendIdle) {
        LOGI("💾 [SaveLoad] Left frontend idle — gameState=%u", game_state);
    }

    if (ms_loading || (have_game_state && game_state == kGameStateLoadingStarted)) {
        begin_save_load_session();
    } else if (prev_ms_loading && g_save_load_session.load(std::memory_order_acquire)) {
        g_ms_b_loading_cleared_ms.store(now_ms(), std::memory_order_release);
        LOGI("💾 [SaveLoad] ms_bLoading cleared — gameState=%u streaming=%d skip=%d",
             have_game_state ? game_state : 255u,
             read_streaming_num_requested(),
             read_skip_pipeline_active() ? 1 : 0);
    }

    if (read_ms_b_load_failed() && g_save_load_session.load(std::memory_order_acquire)) {
        end_save_load_session("ms_bFailed");
    }

    if (g_save_load_session.load(std::memory_order_acquire)) {
        const int64_t now = now_ms();
        if (now - last_diag_ms >= 5000) {
            last_diag_ms = now;
            const int64_t started = g_session_started_ms.load(std::memory_order_acquire);
            LOGI("💾 [SaveLoad] Session diag — kind=%u gameState=%u skip=%d sessionMs=%lld "
                 "touchPending=%d menuLoadSeen=%d",
                 kind,
                 have_game_state ? game_state : 255u,
                 skip_active ? 1 : 0,
                 started > 0 ? static_cast<long long>(now - started) : -1LL,
                 vanilla_qol_touch_rehydrate_pending() ? 1 : 0,
                 g_menu_read_save_path_seen.load(std::memory_order_acquire) ? 1 : 0);
        }

        const int64_t started = g_session_started_ms.load(std::memory_order_acquire);
        const bool settled = hydration_settled_for_session_end();
        int64_t min_active_ms = kSessionMinActiveMs;
        int64_t dwell_ms = kHydrationSignalDwellMs;
        session_timing_thresholds(&min_active_ms, &dwell_ms);

        if (settled && !prev_settled) {
            const int64_t t = now_ms();
            g_hydration_ready_since_ms.store(t, std::memory_order_release);
            LOGI("💾 [SaveLoad] Hydration signals settled — gameState=%u streaming=%d (dwell %lldms min %lldms)",
                 have_game_state ? game_state : 255u,
                 read_streaming_num_requested(),
                 static_cast<long long>(dwell_ms),
                 static_cast<long long>(min_active_ms));
        } else if (!settled) {
            g_hydration_ready_since_ms.store(0, std::memory_order_release);
        }
        prev_settled = settled;

        const int64_t ready_since = g_hydration_ready_since_ms.load(std::memory_order_acquire);
        if (settled && ready_since > 0 &&
            now_ms() - ready_since >= dwell_ms &&
            started > 0 && now_ms() - started >= min_active_ms) {
            end_save_load_session("engine signals settled");
        } else if (started > 0 && now_ms() - started >= kSessionHardCapMs) {
            end_save_load_session("session hard cap");
        } else if (!ms_loading && have_game_state && game_state <= kGameStateFrontendIdle) {
            const int64_t cleared =
                g_ms_b_loading_cleared_ms.load(std::memory_order_acquire);
            const int64_t anchor = cleared > 0 ? cleared : started;
            if (anchor > 0 && now_ms() - anchor >= kAbandonedSessionMs) {
                end_save_load_session("returned to frontend idle");
            }
        }
    } else {
        prev_settled = false;
        last_diag_ms = 0;
    }

    if (have_game_state) {
        prev_game_state = game_state;
    }

    prev_skip_active = skip_active;
    prev_ms_loading = ms_loading;
}

bool is_player_world_active() {
    if (!g_FindPlayerPed) return false;
    CPlayerPed* player = g_FindPlayerPed(0);
    return player && is_ped_pointer_valid_safe(reinterpret_cast<CPed*>(player));
}

static bool is_gameplay_world_stable() {
    if (!is_player_world_active()) return false;
    uint8_t game_state = 0;
    if (read_game_state(&game_state) && game_state != kGameStateIdle) {
        return false;
    }
    return true;
}

bool is_scene_transition_active() {
    if (g_mod_interior_transition.load(std::memory_order_acquire)) return true;
    if (g_entry_exit_ms_bWarping && is_pointer_readable(g_entry_exit_ms_bWarping) && *g_entry_exit_ms_bWarping) {
        return true;
    }
    if (g_we_are_in_interior_transition) {
        return g_we_are_in_interior_transition();
    }
    return false;
}

bool is_disk_deserialize_active() {
    return read_ms_b_loading();
}

bool is_save_load_session_or_loading() {
    if (read_ms_b_loading()) return true;
    return g_save_load_session.load(std::memory_order_acquire);
}

bool is_skip_cutscene_pipeline_active() {
    return read_skip_pipeline_active();
}

bool is_save_load_active() {
    if (is_save_load_session_or_loading()) return true;
    const int64_t quiesce_until =
        g_save_load_quiesce_until_ms.load(std::memory_order_acquire);
    return quiesce_until > 0 && now_ms() < quiesce_until;
}

bool is_mod_dispatch_paused() {
    return is_scene_transition_active() || is_save_load_active() || !is_gameplay_world_stable();
}

// Sanitize mutates task slots only when the player is in a stable in-game world.
// Frontend idle / save-load / scene warp: CALL_PREV only (tombstone_21–24 load/menu).
static bool is_gameplay_world_stable_for_sanitize() {
    if (is_save_load_active()) return false;
    return is_gameplay_world_stable();
}

inline bool is_stability_sanitize_paused() {
    return is_save_load_active() ||
           is_scene_transition_active() ||
           !is_gameplay_world_stable_for_sanitize();
}

// ManageTasks hot path: pause post-load hydration (gameState 9) — allow on loading screen (state 8).
static inline bool is_task_manager_hotpath_paused() {
    uint8_t game_state = 0;
    const bool have_game_state = read_game_state(&game_state);
    if (have_game_state && game_state == kGameStateLoadingStarted) {
        return false;
    }
    if (read_ms_b_loading()) {
        return true;
    }
    if (!g_save_load_session.load(std::memory_order_acquire)) return false;
    // Post-skip: resume scripts so right-side action widgets get re-enabled.
    const int64_t skip_cleared =
        g_skip_pipeline_cleared_ms.load(std::memory_order_acquire);
    if (skip_cleared > 0 &&
        now_ms() - skip_cleared >= kUltraRelaxedPostSkipDwellMs &&
        !read_skip_pipeline_active() &&
        is_player_world_active()) {
        return false;
    }
    return true;
}

// Script ControlSubTask / MakeAbortable: CALL_PREV during save-load (intro/load scripts, tombstone 27/28).
#define MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_fn, self, ped, priority, event) \
    do { \
        if (is_stability_sanitize_paused()) { \
            return SHADOWHOOK_CALL_PREV(proxy_fn, self, ped, priority, event); \
        } \
    } while (0)

#define CONTROL_SUB_TASK_SAVE_LOAD_FASTPATH(proxy_fn, self, ped) \
    do { \
        if (is_stability_sanitize_paused()) { \
            if (!self || !is_pointer_readable(self)) return nullptr; \
            return SHADOWHOOK_CALL_PREV(proxy_fn, self, ped); \
        } \
    } while (0)

#define STABILITY_VOID_SAVE_LOAD_FASTPATH(proxy_fn, self) \
    do { \
        if (is_stability_sanitize_paused()) { \
            if (!self || !is_pointer_readable(self)) return; \
            SHADOWHOOK_CALL_PREV(proxy_fn, self); \
            return; \
        } \
    } while (0)

// Ped intel / task-manager hot paths: skip engine during save-load; scene warp still CALL_PREV.
#define STABILITY_VOID_SAVE_LOAD_SKIP_HOTPATH(proxy_fn, self) \
    do { \
        if (is_stability_sanitize_paused()) { \
            if (is_task_manager_hotpath_paused()) return; \
            if (!self || !is_pointer_readable(self)) return; \
            SHADOWHOOK_CALL_PREV(proxy_fn, self); \
            return; \
        } \
    } while (0)

// =====================================================================
// Stability hook helpers
// =====================================================================
inline void sanitize_task_pointers(void* task, int max_size_bytes = 256) {
    // Blind memory scan disabled: 8-byte stepping can mistake floats/CVector fields
    // for stale pointers and null them out, causing fault-at-0x10 crashes.
    // Re-enable only with per-task-class offsets from reverse engineering.
}

inline void sanitize_unsafe_subtask_at(void* task, size_t offset) {
    if (is_stability_sanitize_paused()) return;
    if (!task || !is_pointer_readable(task)) return;
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task) + offset);
    if (!is_pointer_readable(sub_slot)) return;
    void* sub = *sub_slot;
    if (sub && !is_task_vtable_safe(sub)) {
        LOGW("⚠️ [Subtask Sanitizer] Clearing unsafe subtask %p inside task %p", sub, task);
        *sub_slot = nullptr;
    }
}

// Engine often cbnz task slots but dereferences vtable+N without null guard (tombstone_41/42).
inline bool task_vtable_fn_at_unsafe(void* task, size_t vtable_offset) {
    if (!task || !is_pointer_readable(task)) return true;
    void** vtable_slot = reinterpret_cast<void**>(task);
    if (!is_pointer_readable(vtable_slot) || !*vtable_slot) return true;
    void** fn_slot = reinterpret_cast<void**>(
        reinterpret_cast<char*>(*vtable_slot) + vtable_offset);
    return !is_pointer_readable(fn_slot) || !*fn_slot || !is_pointer_readable(*fn_slot);
}

inline bool task_slot_unsafe_for_vtable_call(void* task, size_t vtable_offset) {
    if (!task) return false;
    return !is_task_vtable_safe(task) || task_vtable_fn_at_unsafe(task, vtable_offset);
}

inline void sanitize_task_manager_slots(void* task_mgr, const char* log_tag, size_t vtable_fn_offset = 0x18) {
    if (is_stability_sanitize_paused()) return;
    if (!task_mgr || !is_pointer_readable(task_mgr)) return;
    for (int i = 0; i < 11; ++i) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
        if (!is_pointer_readable(task_slot)) continue;
        void* task = *task_slot;
        if (task && task_slot_unsafe_for_vtable_call(task, vtable_fn_offset)) {
            LOGW("⚠️ [%s] Clearing unsafe task %p at slot %d", log_tag, task, i);
            *task_slot = nullptr;
        }
    }
}

inline bool task_manager_has_unsafe_slot(void* task_mgr, int max_slots, size_t vtable_fn_offset) {
    if (!task_mgr || !is_pointer_readable(task_mgr)) return false;
    for (int i = 0; i < max_slots; ++i) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
        if (!is_pointer_readable(task_slot)) continue;
        void* task = *task_slot;
        if (task && task_slot_unsafe_for_vtable_call(task, vtable_fn_offset)) return true;
    }
    return false;
}

inline void sanitize_event_script_command_task_slot(void* self, const char* log_tag) {
    if (is_stability_sanitize_paused()) return;
    if (!self || !is_pointer_readable(self)) return;
    void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x18);
    if (!is_pointer_readable(task_slot)) return;
    void* task = *task_slot;
    // RE: task at +0x18 null → cbz 54ce69c; vtable+0x8 null → no cbz 54ce6a4 (tombstone_43).
    if (task && task_slot_unsafe_for_vtable_call(task, 0x8)) {
        LOGW("⚠️ [%s] Clearing unsafe task %p at +0x18", log_tag, task);
        *task_slot = nullptr;
    }
}

using TaskVtableWalkFn_t = void* (*)(void*);

inline void* call_task_vtable_fn_at(void* task, size_t vtable_offset) {
    if (!task || task_slot_unsafe_for_vtable_call(task, vtable_offset)) return nullptr;
    void** vtable = reinterpret_cast<void**>(task);
    auto fn = reinterpret_cast<TaskVtableWalkFn_t>(
        *reinterpret_cast<void**>(reinterpret_cast<char*>(*vtable) + vtable_offset));
    return fn(task);
}

inline bool task_chain_walk_unsafe(void* task, size_t vtable_offset, int max_depth = 32) {
    for (int depth = 0; task && depth < max_depth; ++depth) {
        if (task_slot_unsafe_for_vtable_call(task, vtable_offset)) return true;
        task = call_task_vtable_fn_at(task, vtable_offset);
    }
    return false;
}

inline void* simplest_in_task_chain(void* task, size_t vtable_offset = 0x18) {
    if (!task || task_slot_unsafe_for_vtable_call(task, vtable_offset)) return nullptr;
    void* current = task;
    for (int depth = 0; depth < 32; ++depth) {
        void* sub = call_task_vtable_fn_at(current, vtable_offset);
        if (!sub) return current;
        if (task_slot_unsafe_for_vtable_call(sub, vtable_offset)) return current;
        current = sub;
    }
    return current;
}

inline void sanitize_task_manager_primary_chains(void* task_mgr, const char* log_tag, size_t vtable_fn_offset = 0x18) {
    if (is_stability_sanitize_paused()) return;
    if (!task_mgr || !is_pointer_readable(task_mgr)) return;
    for (int i = 0; i < 5; ++i) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
        if (!is_pointer_readable(task_slot)) continue;
        void* task = *task_slot;
        if (!task) continue;
        if (task_chain_walk_unsafe(task, vtable_fn_offset)) {
            LOGW("⚠️ [%s] Clearing primary chain at slot %d (unsafe subtask node)", log_tag, i);
            *task_slot = nullptr;
        }
    }
}

// --- CTaskSimpleHoldEntity::SetPedPosition Hooks ---

void* g_stub_set_ped_pos = nullptr;
fn_SetPedPosition_t g_orig_set_ped_pos = nullptr;
void proxy_set_ped_pos(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) {
        LOGW("⚠️ [SetPedPosition] unsafe self! Skipping.");
        return;
    }
    if (ped && !is_pointer_readable(ped)) {
        LOGW("⚠️ [SetPedPosition] unsafe ped! Skipping.");
        return;
    }
    if (self && ped) {
        // Check if the pointer at ped + 0x648 is null or unreadable
        char* ped_bytes = reinterpret_cast<char*>(ped);
        void** clump_slot = reinterpret_cast<void**>(ped_bytes + 0x648);
        if (!is_pointer_readable(clump_slot)) {
            LOGW("⚠️ [SetPedPosition] ped + 0x648 slot unreadable! Skipping to prevent crash.");
            return;
        }
        void* clump = *clump_slot;
        if (clump && !is_pointer_readable(clump)) {
            LOGW("⚠️ [SetPedPosition] ped->clump (%p) is invalid/unreadable! Skipping original to prevent crash.", clump);
            return;
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_set_ped_pos, self, ped);
}


// =====================================================================
// 🛠️ [CTaskManager & CAttractorScanner Safety Hooks]
// =====================================================================
void* g_stub_manage_tasks = nullptr;
fn_ManageTasks_t g_orig_manage_tasks = nullptr;


inline bool is_task_simple(void* task) {
    if (!task || !is_pointer_readable(task)) return true;
    void** vtable = *reinterpret_cast<void***>(task);
    if (!is_pointer_readable(vtable)) return true;

    // IsSimple is at offset 0x20 (index 4 in 64-bit vtable)
    fn_IsSimple_t is_simple_fn = reinterpret_cast<fn_IsSimple_t>(vtable[4]);
    if (is_pointer_readable(reinterpret_cast<void*>(is_simple_fn))) {
        return is_simple_fn(task);
    }
    return true;
}

void sanitize_task_chain(void* task, int depth = 0) {
    if (!task || depth > 10) return;
    if (!is_pointer_readable(task)) return;

    sanitize_task_pointers(task);

    // Only CTaskComplex (where is_task_simple returns false) has m_pSubTask at offset 16.
    // Wiping offset 16 of a CTaskSimple will corrupt its subclass member variables.
    if (!is_task_simple(task)) {
        void** parent_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task) + 8);
        void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task) + 16);

        if (is_pointer_readable(parent_slot)) {
            void* parent = *parent_slot;
            if (parent && !is_task_vtable_safe(parent)) {
                LOGW("⚠️ [Task Chain Sanitizer] Clearing unsafe parent task %p inside task %p", parent, task);
                *parent_slot = nullptr;
            }
        }

        if (is_pointer_readable(sub_slot)) {
            void* sub = *sub_slot;
            if (sub) {
                if (!is_task_vtable_safe(sub)) {
                    LOGW("⚠️ [Task Chain Sanitizer] Clearing unsafe subtask %p inside task %p", sub, task);
                    *sub_slot = nullptr;
                } else {
                    sanitize_task_chain(sub, depth + 1);
                }
            }
        }
    } else {
        // For CTaskSimple, we only sanitize its parent task pointer at offset 8.
        void** parent_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task) + 8);
        if (is_pointer_readable(parent_slot)) {
            void* parent = *parent_slot;
            if (parent && !is_task_vtable_safe(parent)) {
                LOGW("⚠️ [Task Chain Sanitizer] Clearing unsafe parent task %p inside simple task %p", parent, task);
                *parent_slot = nullptr;
            }
        }
    }
}

// RE: slot[x23] null at 57ab090 → cbz 57ab160; x20 null → no cbz 57ab160 (tombstone_03–08, scene switch).
// RE: 57ab0f0/57ab140 reload slot → null → 57ab15c mov x20,xzr → 57ab160 — patched in manage_tasks_guard.cpp.
// RE: 57ab274 mov x20,xzr → ldr [x20] at 57ab278 — patched in manage_tasks_guard.cpp.
void proxy_manage_tasks(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return;
    if (is_stability_sanitize_paused()) {
        // Layered gate (tombstone 29–32): hot path skips CALL_PREV during ms_bLoading
        // and post-load hydration session; post-skip exception re-enables for touch HUD.
        if (is_task_manager_hotpath_paused()) return;
        SHADOWHOOK_CALL_PREV(proxy_manage_tasks, self);
        return;
    }

    sanitize_task_manager_slots(self, "CTaskManager::ManageTasks", 0x18);
    sanitize_task_manager_primary_chains(self, "CTaskManager::ManageTasks", 0x18);
    for (int i = 0; i < 11; ++i) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + i * 8);
        if (!is_pointer_readable(task_slot)) continue;
        void* task = *task_slot;
        if (!task) continue;
        if (task_chain_walk_unsafe(task, 0x18)) {
            LOGW("⚠️ [CTaskManager::ManageTasks] clearing unsafe chain at slot %d", i);
            *task_slot = nullptr;
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_manage_tasks, self);
}

void* g_stub_scan_for_attractors_in_range = nullptr;
fn_ScanForAttractorsInRange_t g_orig_scan_for_attractors_in_range = nullptr;

void proxy_scan_for_attractors_in_range(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (is_stability_sanitize_paused()) {
        if (is_task_manager_hotpath_paused()) return;
        if (!ped || !is_pointer_readable(ped)) return;
        SHADOWHOOK_CALL_PREV(proxy_scan_for_attractors_in_range, self, ped);
        return;
    }
    if (!ped || !is_pointer_readable(ped)) return;
    void* intel = get_ped_intelligence(reinterpret_cast<CPed*>(ped));
    if (intel && is_pointer_readable(intel)) {
        sanitize_task_manager_slots(reinterpret_cast<char*>(intel) + 8, "ScanForAttractorsInRange");
    }
    SHADOWHOOK_CALL_PREV(proxy_scan_for_attractors_in_range, self, ped);
}

// --- CTaskComplexGangFollower::ControlSubTask Hook ---
inline void sanitize_task_tree(void* task) {
    if (is_stability_sanitize_paused()) return;
    if (!task || !is_pointer_readable(task)) return;
    
    // Only CTaskComplex has m_pSubTask at offset 16 (0x10).
    // Wiping offset 16 of a CTaskSimple will corrupt its member variables!
    if (!is_task_simple(task)) {
        void** p_sub = reinterpret_cast<void**>(reinterpret_cast<char*>(task) + 0x10);
        if (is_pointer_readable(p_sub)) {
            void* sub = *p_sub;
            if (sub) {
                if (!is_task_vtable_safe(sub)) {
                    LOGW("⚠️ [Task Tree Sanitizer] Clearing unsafe subtask %p inside parent task %p", sub, task);
                    *p_sub = nullptr;
                } else {
                    sanitize_task_tree(sub);
                }
            }
        }
    }
}

inline void sanitize_ped_tasks(void* ped) {
    if (is_stability_sanitize_paused()) return;
    if (!ped || !is_pointer_readable(ped)) return;
    void** intel_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5e8);
    if (!is_pointer_readable(intel_slot)) return;
    void* intel = *intel_slot;
    if (!intel || !is_pointer_readable(intel)) return;
    void* task_mgr = reinterpret_cast<char*>(intel) + 8;
    if (!is_pointer_readable(task_mgr)) return;
    for (int i = 0; i < 11; ++i) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
        if (is_pointer_readable(task_slot)) {
            void* task = *task_slot;
            if (task) {
                if (!is_task_vtable_safe(task)) {
                    LOGW("⚠️ [Task Sanitizer] Clearing unsafe/zeroed task %p at slot %d in ped %p", task, i, ped);
                    *task_slot = nullptr;
                } else {
                    sanitize_task_tree(task);
                }
            }
        }
    }
}

// --- CGenericGameStorage / CGameLogic load lifecycle hooks ---
typedef void (*fn_GenericGameStorageLoad_t)(void* self, bool flag);
typedef void (*fn_GenericGameStorageLoadGame_t)(void* self);
typedef void (*fn_GenericGameStorageRestoreForStartLoad_t)();
typedef void (*fn_GenericGameStorageGenericLoad_t)(int slot, bool* out);
typedef void (*fn_GenericGameStorageAfterSuccessLoad_t)();
typedef void (*fn_CGameLogicInitAtStartOfGame_t)();

void* g_stub_generic_game_storage_load = nullptr;
fn_GenericGameStorageLoad_t g_orig_generic_game_storage_load = nullptr;
void* g_stub_generic_game_storage_load_game = nullptr;
fn_GenericGameStorageLoadGame_t g_orig_generic_game_storage_load_game = nullptr;
void* g_stub_generic_game_storage_restore_for_start_load = nullptr;
fn_GenericGameStorageRestoreForStartLoad_t g_orig_generic_game_storage_restore_for_start_load = nullptr;
void* g_stub_generic_game_storage_generic_load = nullptr;
fn_GenericGameStorageGenericLoad_t g_orig_generic_game_storage_generic_load = nullptr;
void* g_stub_generic_game_storage_after_success_load = nullptr;
fn_GenericGameStorageAfterSuccessLoad_t g_orig_generic_game_storage_after_success_load = nullptr;
void* g_stub_cgame_logic_init_at_start_of_game = nullptr;
fn_CGameLogicInitAtStartOfGame_t g_orig_cgame_logic_init_at_start_of_game = nullptr;

void proxy_generic_game_storage_load(void* self, bool flag) {
    SHADOWHOOK_STACK_SCOPE();
    notify_menu_read_save_path("CGenericGameStorage::Load");
    SHADOWHOOK_CALL_PREV(proxy_generic_game_storage_load, self, flag);
}

void proxy_generic_game_storage_load_game(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    notify_menu_read_save_path("CGenericGameStorage::LoadGame");
    SHADOWHOOK_CALL_PREV(proxy_generic_game_storage_load_game, self);
}

void proxy_generic_game_storage_restore_for_start_load() {
    SHADOWHOOK_STACK_SCOPE();
    notify_menu_read_save_path("RestoreForStartLoad");
    SHADOWHOOK_CALL_PREV(proxy_generic_game_storage_restore_for_start_load);
}

void proxy_generic_game_storage_generic_load(int slot, bool* out) {
    SHADOWHOOK_STACK_SCOPE();
    notify_menu_read_save_path("GenericLoad");
    LOGI("💾 [SaveLoad] GenericLoad(slot=%d)", slot);
    SHADOWHOOK_CALL_PREV(proxy_generic_game_storage_generic_load, slot, out);
    const bool load_ok = out && is_pointer_readable(out) && *out;
    LOGI("💾 [SaveLoad] GenericLoad(slot=%d) done — ok=%d ms_bFailed=%d",
         slot, load_ok ? 1 : 0, read_ms_b_load_failed() ? 1 : 0);
}

void proxy_generic_game_storage_after_success_load() {
    SHADOWHOOK_STACK_SCOPE();
    g_menu_read_save_path_seen.store(true, std::memory_order_release);
    g_save_load_kind.store(static_cast<uint8_t>(SaveLoadKind::MenuGenericLoad),
                           std::memory_order_release);
    begin_save_load_session();
    LOGI("💾 [SaveLoad] DoGameSpecificStuffAfterSucessLoad — deserialize done");
    SHADOWHOOK_CALL_PREV(proxy_generic_game_storage_after_success_load);
    vanilla_qol_on_deserialize_complete();
}

void proxy_cgame_logic_init_at_start_of_game() {
    SHADOWHOOK_STACK_SCOPE();
    const uint8_t kind = g_save_load_kind.load(std::memory_order_acquire);
    const bool menu_load_seen =
        g_menu_read_save_path_seen.load(std::memory_order_acquire);
    uint8_t game_state = 0;
    const bool have_game_state = read_game_state(&game_state);
    const bool explicit_new_game =
        g_explicit_new_game_bootstrap.load(std::memory_order_acquire);
    if (kind == static_cast<uint8_t>(SaveLoadKind::MenuGenericLoad) || menu_load_seen) {
        if (kind != static_cast<uint8_t>(SaveLoadKind::MenuGenericLoad)) {
            g_save_load_kind.store(static_cast<uint8_t>(SaveLoadKind::MenuGenericLoad),
                                   std::memory_order_release);
        }
        LOGI("💾 [SaveLoad] CGameLogic::InitAtStartOfGame — menu read-save (gameState=%u)",
             have_game_state ? game_state : 255u);
    } else if (explicit_new_game ||
               kind == static_cast<uint8_t>(SaveLoadKind::NewGameBootstrap)) {
        g_explicit_new_game_bootstrap.store(false, std::memory_order_release);
        g_save_load_kind.store(static_cast<uint8_t>(SaveLoadKind::NewGameBootstrap),
                               std::memory_order_release);
        LOGI("💾 [SaveLoad] CGameLogic::InitAtStartOfGame — new game bootstrap (gameState=%u)",
             have_game_state ? game_state : 255u);
    } else if (read_ms_b_loading() ||
               (have_game_state && game_state == kGameStateLoadingStarted)) {
        // DE menu load may hit InitAtStartOfGame during state 8 before storage hooks.
        begin_save_load_session();
        LOGI("💾 [SaveLoad] CGameLogic::InitAtStartOfGame — during engine load "
             "(defer new-game mark, gameState=%u)",
             have_game_state ? game_state : 255u);
    } else {
        LOGI("💾 [SaveLoad] CGameLogic::InitAtStartOfGame — kind deferred "
             "(await load/new-game hooks, gameState=%u)",
             have_game_state ? game_state : 255u);
    }
    SHADOWHOOK_CALL_PREV(proxy_cgame_logic_init_at_start_of_game);
}

// --- CEntryExit transition hooks (pause mod dispatch during interior warp/fade) ---
void* g_stub_entry_exit_transition_started = nullptr;
fn_EntryExitTransitionStarted_t g_orig_entry_exit_transition_started = nullptr;
void* g_stub_entry_exit_transition_finished = nullptr;
fn_EntryExitTransitionFinished_t g_orig_entry_exit_transition_finished = nullptr;

void proxy_entry_exit_transition_started(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    g_mod_interior_transition.store(true, std::memory_order_release);
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    SHADOWHOOK_CALL_PREV(proxy_entry_exit_transition_started, self, ped);
}

void proxy_entry_exit_transition_finished(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    g_mod_interior_transition.store(false, std::memory_order_release);
    SHADOWHOOK_CALL_PREV(proxy_entry_exit_transition_finished, self, ped);
}

inline void sanitize_optional_ped_at_slot(void** slot, const char* label) {
    if (is_stability_sanitize_paused()) return;
    if (!slot || !is_pointer_readable(slot)) return;
    void* ped = *slot;
    if (!ped) return;
    if (!is_ped_pointer_valid_safe(ped)) {
        LOGW("⚠️ [Ped Slot Sanitizer] Clearing invalid ped %p (%s)", ped, label);
        *slot = nullptr;
        return;
    }
    sanitize_ped_tasks(ped);
}

// GangFollower leader/partner: only clear unreadable or zero-filled peds (AGENTS.md).
// Do not clear merely because the ped is absent from the pool — that can false-positive
// and leave nullptr at task+0x18, which the original reads at +0x498 without a null check.
inline void sanitize_unsafe_task_slot_at(void* obj, size_t offset, const char* label) {
    if (is_stability_sanitize_paused()) return;
    if (!obj || !is_pointer_readable(obj)) return;
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + offset);
    if (!is_pointer_readable(slot)) return;
    void* task = *slot;
    if (task && !is_task_vtable_safe(task)) {
        LOGW("⚠️ [%s] Clearing unsafe task %p at +0x%zx", label, task, offset);
        *slot = nullptr;
    }
}

inline void sanitize_gang_follower_ped_slot(void** slot, const char* label) {
    if (is_stability_sanitize_paused()) return;
    if (!slot || !is_pointer_readable(slot)) return;
    void* ped = *slot;
    if (!ped) return;

    if (!is_pointer_readable(ped)) {
        LOGW("⚠️ [GangFollower] Clearing unreadable %s ped %p", label, ped);
        *slot = nullptr;
        return;
    }
    void** vtable_slot = reinterpret_cast<void**>(ped);
    if (is_pointer_readable(vtable_slot) && *vtable_slot == nullptr) {
        LOGW("⚠️ [GangFollower] Clearing zero-filled %s ped %p", label, ped);
        *slot = nullptr;
        return;
    }
    if (is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
}


void* g_stub_ccgf_control = nullptr;
fn_ControlSubTask_t g_orig_ccgf_control = nullptr;
void* proxy_ccgf_control(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    CONTROL_SUB_TASK_SAVE_LOAD_FASTPATH(proxy_ccgf_control, self, ped);
    if (!self || !is_pointer_readable(self)) return nullptr;
    if (ped && !is_pointer_readable(ped)) return nullptr;

    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }

    if (self) {
        char* self_bytes = reinterpret_cast<char*>(self);
        sanitize_unsafe_subtask_at(self, 0x10);
        sanitize_gang_follower_ped_slot(reinterpret_cast<void**>(self_bytes + 0x18), "leader");
        sanitize_gang_follower_ped_slot(reinterpret_cast<void**>(self_bytes + 0x20), "partner");

        // Original dereferences leader+0x498 with no null guard (tombstone_01–04, fault 0x498).
        // Skip only when leader slot is empty after sanitization — not intercepting ped args.
        void** leader_slot = reinterpret_cast<void**>(self_bytes + 0x18);
        if (!is_pointer_readable(leader_slot) || !*leader_slot) {
            LOGW("⚠️ [GangFollower::ControlSubTask] no leader after sanitize — skip original");
            return nullptr;
        }
    }

    return SHADOWHOOK_CALL_PREV(proxy_ccgf_control, self, ped);
}


// --- CTaskComplexUsePairedAttractor::CreateNextSubTask Hook ---
void* g_stub_paired_attractor_create_next_sub_task = nullptr;
fn_PairedAttractorCreateNextSubTask_t g_orig_paired_attractor_create_next_sub_task = nullptr;

void* proxy_paired_attractor_create_next_sub_task(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    CONTROL_SUB_TASK_SAVE_LOAD_FASTPATH(proxy_paired_attractor_create_next_sub_task, self, ped);
    if (ped && !is_pointer_readable(ped)) {
        return nullptr;
    }
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    return SHADOWHOOK_CALL_PREV(proxy_paired_attractor_create_next_sub_task, self, ped);
}

// --- CTaskComplexFacial Destructor Hook ---
void* g_stub_facial_dtor = nullptr;
fn_FacialDtor_t g_orig_facial_dtor = nullptr;

void proxy_facial_dtor(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (is_stability_sanitize_paused()) {
        if (!self || !is_pointer_readable(self)) return;
        SHADOWHOOK_CALL_PREV(proxy_facial_dtor, self);
        return;
    }
    if (!self || !is_pointer_readable(self)) return;

    if (self) {
        void** p_sub = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
        if (is_pointer_readable(p_sub)) {
            void* sub = *p_sub;
            if (sub && !is_task_vtable_safe(sub)) {
                LOGW("⚠️ [Facial Dtor] Clearing unsafe subtask %p inside facial task %p before destruction", sub, self);
                *p_sub = nullptr;
            }
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_facial_dtor, self);
}

// --- CTaskManager::FindActiveTaskByType Hook ---
void* g_stub_find_active_task = nullptr;
fn_FindActiveTask_t g_orig_find_active_task = nullptr;

void* proxy_find_active_task(void* self, int type) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return nullptr;
    if (is_stability_sanitize_paused()) {
        if (is_task_manager_hotpath_paused()) return nullptr;
        return SHADOWHOOK_CALL_PREV(proxy_find_active_task, self, type);
    }

    if (self) {
        sanitize_task_manager_slots(self, "CTaskManager::FindActiveTaskByType", 0x28);
        sanitize_task_manager_primary_chains(self, "CTaskManager::FindActiveTaskByType", 0x28);
        for (int i = 0; i < 11; ++i) {
            void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + i * 8);
            if (!is_pointer_readable(task_slot)) continue;
            void* task = *task_slot;
            if (task && is_task_vtable_safe(task)) {
                sanitize_task_tree(task);
            }
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_find_active_task, self, type);
}

// --- CPedIntelligence::FindTaskByType Hook ---
// RE: intel+0x28/+0x20 secondary tasks → vtable+0x28 chain 55c6a34/55c6a80 (tombstone_00).
void* g_stub_intel_find_task_by_type = nullptr;
fn_IntelFindTaskByType_t g_orig_intel_find_task_by_type = nullptr;

inline void sanitize_intel_for_task_lookup(void* intel) {
    if (is_stability_sanitize_paused()) return;
    if (!intel || !is_pointer_readable(intel)) return;
    sanitize_task_manager_slots(reinterpret_cast<char*>(intel) + 8, "Intel::FindTaskByType", 0x28);
    sanitize_task_manager_primary_chains(reinterpret_cast<char*>(intel) + 8, "Intel::FindTaskByType", 0x28);
    for (size_t off : {0x20u, 0x28u}) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(intel) + off);
        if (!is_pointer_readable(task_slot)) continue;
        void* task = *task_slot;
        if (task && task_slot_unsafe_for_vtable_call(task, 0x28)) {
            LOGW("⚠️ [Intel::FindTaskByType] Clearing unsafe task %p at intel+0x%zx", task, off);
            *task_slot = nullptr;
        }
    }
}

void* proxy_intel_find_task_by_type(void* self, int type) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return nullptr;
    if (is_stability_sanitize_paused()) {
        if (is_task_manager_hotpath_paused()) return nullptr;
        return SHADOWHOOK_CALL_PREV(proxy_intel_find_task_by_type, self, type);
    }
    sanitize_intel_for_task_lookup(self);
    for (size_t off : {0x20u, 0x28u}) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + off);
        if (!is_pointer_readable(task_slot)) continue;
        void* task = *task_slot;
        if (task && task_chain_walk_unsafe(task, 0x28)) return nullptr;
    }
    void* task_mgr = reinterpret_cast<char*>(self) + 8;
    for (int i = 0; i < 11; ++i) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
        if (!is_pointer_readable(task_slot)) continue;
        void* task = *task_slot;
        if (task && task_chain_walk_unsafe(task, 0x28)) return nullptr;
    }
    return SHADOWHOOK_CALL_PREV(proxy_intel_find_task_by_type, self, type);
}

// --- CTaskManager Destructor Hook ---
void* g_stub_task_manager_destructor = nullptr;
fn_TaskManagerDestructor_t g_orig_task_manager_destructor = nullptr;

void proxy_task_manager_destructor(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (is_stability_sanitize_paused()) {
        if (!self || !is_pointer_readable(self)) return;
        SHADOWHOOK_CALL_PREV(proxy_task_manager_destructor, self);
        return;
    }
    if (!self || !is_pointer_readable(self)) return;

    if (self) {
        for (int i = 0; i < 11; ++i) {
            void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + i * 8);
            if (is_pointer_readable(task_slot)) {
                void* task = *task_slot;
                if (task) {
                    if (!is_task_vtable_safe(task)) {
                        LOGW("⚠️ [CTaskManager Destructor Sanitizer] Sanitizing unsafe task %p at slot %d inside CTaskManager %p before destruction", task, i, self);
                        *task_slot = nullptr;
                    }
                }
            }
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_task_manager_destructor, self);
}

// --- CTaskComplexPartnerGreet::GetPartnerSequence Hook ---
void* g_stub_partner_greet_get_sequence = nullptr;
fn_GetPartnerSequence_t g_orig_partner_greet_get_sequence = nullptr;

void* proxy_partner_greet_get_sequence(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) {
        return nullptr;
    }
    {
        void** vtable_ptr = reinterpret_cast<void**>(self);
        if (is_pointer_readable(vtable_ptr) && *vtable_ptr == nullptr) {
            LOGW("⚠️ [PartnerGreet::GetPartnerSequence] zero-filled self %p — skip original", self);
            return nullptr;
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_partner_greet_get_sequence, self);
}

// --- CAEPedSpeechAudioEntity::PlayLoadedSound Hook ---
void* g_stub_play_loaded_sound = nullptr;
fn_PlayLoadedSound_t g_orig_play_loaded_sound = nullptr;

void proxy_play_loaded_sound(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return;

    // ped null: engine cbz at 52c120c handles safely. Only guard paths where
    // ped is non-null but intel/speech chain is null (52c1398–52c13a4, no cbz).
    if (self) {
        void** p_ped = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x8);
        if (is_pointer_readable(p_ped) && *p_ped) {
            void* ped = *p_ped;
            if (!is_pointer_readable(ped)) return;

            void** p_intel = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5e8);
            if (!is_pointer_readable(p_intel) || !*p_intel) {
                LOGW("⚠️ [PlayLoadedSound] ped %p has null intel — skip (engine path 52c1398 has no cbz)", ped);
                return;
            }
            void* intel = *p_intel;
            if (!is_pointer_readable(intel)) return;

            void** p_speech = reinterpret_cast<void**>(reinterpret_cast<char*>(intel) + 0x48);
            if (!is_pointer_readable(p_speech) || !*p_speech) {
                LOGW("⚠️ [PlayLoadedSound] intel %p has null speech mgr — skip (engine path 52c13a4 has no cbz)", intel);
                return;
            }
            if (!is_pointer_readable(*p_speech)) return;
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_play_loaded_sound, self);
}

// --- CCarGenerator::CheckIfWithinRangeOfAnyPlayers Hook ---
void* g_stub_check_if_within_range = nullptr;
fn_CheckIfWithinRange_t g_orig_check_if_within_range = nullptr;
bool proxy_check_if_within_range(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return false;

    // nullptr player: let engine handle. Only block stale non-null ped pointers (tombstone_36).
    if (g_FindPlayerPed) {
        void* player = g_FindPlayerPed(0);
        if (player && !is_ped_pointer_valid_safe(player)) {
            return false;
        }
        void* player1 = g_FindPlayerPed(1);
        if (player1 && !is_ped_pointer_valid_safe(player1)) {
            return false;
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_check_if_within_range, self);
}

// --- CTaskComplexAvoidOtherPedWhileWandering::ControlSubTask Hook ---
void* g_stub_avoid_ped_control = nullptr;
fn_AvoidPedControl_t g_orig_avoid_ped_control = nullptr;

void* proxy_avoid_ped_control(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    CONTROL_SUB_TASK_SAVE_LOAD_FASTPATH(proxy_avoid_ped_control, self, ped);
    if (self && is_pointer_readable(self) && !is_stability_sanitize_paused()) {
        void** subtask_ptr = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
        if (is_pointer_readable(subtask_ptr)) {
            void* subtask = *subtask_ptr;
            if (subtask && !is_task_vtable_safe(subtask)) {
                LOGW("⚠️ [ControlSubTask Sanitizer] Sanitizing unsafe subtask %p inside CTaskComplexAvoidOtherPedWhileWandering %p", subtask, self);
                *subtask_ptr = nullptr;
            }
        }
        // Other ped at +0x18: engine walks its intel task slots → vtable+0x18 (tombstone_23).
        sanitize_optional_ped_at_slot(
            reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x18),
            "AvoidOtherPed other");
    }
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    return SHADOWHOOK_CALL_PREV(proxy_avoid_ped_control, self, ped);
}


void* g_stub_add_police_occupants = nullptr;
fn_AddPoliceOccupants_t g_orig_add_police_occupants = nullptr;

void proxy_add_police_occupants(CVehicle* vehicle, bool bSirenOrAlarm) {
    SHADOWHOOK_STACK_SCOPE();
    if (is_mod_dispatch_paused()) {
        return;
    }
    SHADOWHOOK_CALL_PREV(proxy_add_police_occupants, vehicle, bSirenOrAlarm);
    // bind_vehicle_occupants respects in-progress LeaveCar — avoids exit/enter flicker.
    bind_vehicle_occupants(vehicle);
}

// =====================================================================
// 🛡️ [CPed::PlayFootSteps Hook]：防止转场期间玩家 Clump 临时脱离导致空指针解引用闪退
// =====================================================================
void* g_stub_play_footsteps = nullptr;
fn_PlayFootSteps_t g_orig_play_footsteps = nullptr;

void proxy_play_footsteps(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;
    // Engine cbz chain at 559ac3c–559ac54 handles null rw_clump / sub-fields safely.
    SHADOWHOOK_CALL_PREV(proxy_play_footsteps, self);
}


// =====================================================================
// 🛡️ [cBuoyancy::ProcessBuoyancy Hook]：防止物理浮力计算期间/之后任务被销毁导致 CPed::ProcessBuoyancy 闪退 (静态函数，首参为 CPhysical*)
// =====================================================================
// =====================================================================
// 🛡️ [CPed::ProcessBuoyancy Hook]：防止 ProcessBuoyancy 期间任务槽被置空/野指针导致虚表解引用闪退
// =====================================================================
void* g_stub_process_buoyancy = nullptr;
fn_ProcessBuoyancy_t g_orig_process_buoyancy = nullptr;

void proxy_process_buoyancy(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    STABILITY_VOID_SAVE_LOAD_FASTPATH(proxy_process_buoyancy, self);
    if (!self || !is_pointer_readable(self)) return;
    sanitize_ped_tasks(self);
    SHADOWHOOK_CALL_PREV(proxy_process_buoyancy, self);
}

// --- CPedIntelligence::ProcessStaticCounter Hook ---
void* g_stub_process_static_counter = nullptr;
fn_ProcessStaticCounter_t g_orig_process_static_counter = nullptr;

void proxy_process_static_counter(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    STABILITY_VOID_SAVE_LOAD_FASTPATH(proxy_process_static_counter, self);
    if (!self || !is_pointer_readable(self)) return;
    if (self) {
        sanitize_task_manager_slots(reinterpret_cast<char*>(self) + 8, "ProcessStaticCounter");
    }
    SHADOWHOOK_CALL_PREV(proxy_process_static_counter, self);
}


void* g_stub_cbuoyancy_process_buoyancy = nullptr;
fn_cBuoyancy_ProcessBuoyancy_t g_orig_cbuoyancy_process_buoyancy = nullptr;

bool proxy_cbuoyancy_process_buoyancy(void* self, void* physical, float f1, void* vec1, void* vec2) {
    SHADOWHOOK_STACK_SCOPE();
    if (is_stability_sanitize_paused()) {
        if (!self || !is_pointer_readable(self)) return false;
        return SHADOWHOOK_CALL_PREV(proxy_cbuoyancy_process_buoyancy, self, physical, f1, vec1, vec2);
    }
    if (!self || !is_pointer_readable(self)) return false;
    if (physical && !is_pointer_readable(physical)) return false;
    // vec2 write at 57ff744 (tombstone_13) — reject wild output pointers from caller stack.
    if (vec1 && !is_pointer_readable(vec1)) return false;
    if (vec2 && !is_pointer_readable(vec2)) return false;
    if (physical && is_ped_pointer_valid_safe(physical)) {
        sanitize_ped_tasks(physical);
    }
    return SHADOWHOOK_CALL_PREV(proxy_cbuoyancy_process_buoyancy, self, physical, f1, vec1, vec2);
}

// =====================================================================
// 🛡️ [u_strlen_64 Hook]：防止 ICU 计算 Unicode 字符串长度时传入野指针闪退
// =====================================================================
void* g_stub_u_strlen = nullptr;
fn_u_strlen_t g_orig_u_strlen = nullptr;

int32_t proxy_u_strlen(const void* s) {
    SHADOWHOOK_STACK_SCOPE();
    if (s && !is_pointer_readable(s)) {
        LOGW("⚠️ [u_strlen_64 Hook] wild pointer detected! Returning 0.");
        return 0;
    }
    return SHADOWHOOK_CALL_PREV(proxy_u_strlen, s);
}

// --- CTaskComplexSequence::Flush Hook ---
void* g_stub_sequence_flush = nullptr;
fn_SequenceFlush_t g_orig_sequence_flush = nullptr;

inline void sanitize_sequence_task_slots(void* self) {
    if (is_stability_sanitize_paused()) return;
    if (!self || !is_pointer_readable(self)) return;
    for (int i = 0; i < 8; ++i) {
        void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + i * 8 + 0x20);
        if (!is_pointer_readable(slot)) continue;
        void* task = *slot;
        if (task && !is_task_vtable_safe(task)) {
            LOGW("⚠️ [Sequence] Clearing unsafe task %p at index %d", task, i);
            *slot = nullptr;
        }
    }
}

void proxy_sequence_flush(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    STABILITY_VOID_SAVE_LOAD_FASTPATH(proxy_sequence_flush, self);
    if (!self || !is_pointer_readable(self)) return;
    if (self) {
        sanitize_sequence_task_slots(self);
    }
    SHADOWHOOK_CALL_PREV(proxy_sequence_flush, self);
}

// --- CTaskComplexSequence::CreateNextSubTask Hook ---
void* g_stub_sequence_create_next_sub_task = nullptr;
fn_SequenceCreateNextSubTask_t g_orig_sequence_create_next_sub_task = nullptr;

void* proxy_sequence_create_next_sub_task(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    CONTROL_SUB_TASK_SAVE_LOAD_FASTPATH(proxy_sequence_create_next_sub_task, self, ped);
    if (!self || !is_pointer_readable(self)) return nullptr;
    if (ped && !is_pointer_readable(ped)) return nullptr;
    if (self) {
        sanitize_sequence_task_slots(self);
    }
    return SHADOWHOOK_CALL_PREV(proxy_sequence_create_next_sub_task, self, ped);
}

// --- CTaskSimpleEvasiveStep::FinishAnimEvasiveStepCB Hook ---
void* g_stub_finish_anim_evasive_step_cb = nullptr;
fn_FinishAnimEvasiveStepCB_t g_orig_finish_anim_evasive_step_cb = nullptr;

void proxy_finish_anim_evasive_step_cb(void* anim, void* context) {
    SHADOWHOOK_STACK_SCOPE();
    if (context && !is_pointer_readable(context)) {
        LOGW("⚠️ [EvasiveStep CB] context (%p) is invalid/unreadable! Intercepting callback to prevent crash.", context);
        return;
    }
    if (context) {
        // Check if the context object (the task) is zero-filled
        void** vtable_ptr = reinterpret_cast<void**>(context);
        if (is_pointer_readable(vtable_ptr) && *vtable_ptr == nullptr) {
            LOGW("⚠️ [EvasiveStep CB] context (%p) is zero-filled! Intercepting callback to prevent crash.", context);
            return;
        }
    }
    if (anim && !is_pointer_readable(anim)) {
        LOGW("⚠️ [EvasiveStep CB] anim (%p) is invalid/unreadable! Intercepting callback to prevent crash.", anim);
        return;
    }
    SHADOWHOOK_CALL_PREV(proxy_finish_anim_evasive_step_cb, anim, context);
}

// --- CTaskComplexBeInGroup::ControlSubTask Hook ---
void* g_stub_be_in_group_control_sub_task = nullptr;
fn_BeInGroupControlSubTask_t g_orig_be_in_group_control_sub_task = nullptr;

void* proxy_be_in_group_control_sub_task(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    CONTROL_SUB_TASK_SAVE_LOAD_FASTPATH(proxy_be_in_group_control_sub_task, self, ped);
    if (!self || !is_pointer_readable(self)) return nullptr;
    if (ped && !is_pointer_readable(ped)) return nullptr;

    if (self) {
        void** vtable_ptr = reinterpret_cast<void**>(self);
        if (is_pointer_readable(vtable_ptr) && *vtable_ptr == nullptr) {
            LOGW("⚠️ [BeInGroup] self (%p) is zero-filled! Intercepting ControlSubTask to prevent crash.", self);
            return nullptr;
        }
        sanitize_unsafe_subtask_at(self, 0x10);
        sanitize_unsafe_task_slot_at(self, 0x28, "BeInGroup secondary");
    }
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    return SHADOWHOOK_CALL_PREV(proxy_be_in_group_control_sub_task, self, ped);
}

// --- CPedGroupIntelligence::GetTaskMain Hook ---
// BeInGroup::ControlSubTask calls GetTaskMain then dereferences task+vtable+0x28 without
// a null-vtable guard (tombstone_05, fault 0x28 on zero-filled group task).
void* g_stub_get_task_main = nullptr;
fn_GetTaskMain_t g_orig_get_task_main = nullptr;

void* proxy_get_task_main(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    void* result = SHADOWHOOK_CALL_PREV(proxy_get_task_main, self, ped);
    if (result && !is_task_vtable_safe(result)) {
        LOGW("⚠️ [GetTaskMain] unsafe task %p — returning nullptr", result);
        return nullptr;
    }
    return result;
}

// --- CCarEnterExit::GetNearestCarDoor Hook ---
// Iterates ped task slots and calls vtable+0x28; zero-filled tasks trigger pure virtual (tombstone_06).
void* g_stub_get_nearest_car_door = nullptr;
fn_GetNearestCarDoor_t g_orig_get_nearest_car_door = nullptr;

bool proxy_get_nearest_car_door(void* ped, void* vehicle, void* out_vec, int* door_index) {
    SHADOWHOOK_STACK_SCOPE();
    if (is_stability_sanitize_paused()) {
        if (is_task_manager_hotpath_paused()) return false;
        return SHADOWHOOK_CALL_PREV(proxy_get_nearest_car_door, ped, vehicle, out_vec, door_index);
    }
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    return SHADOWHOOK_CALL_PREV(proxy_get_nearest_car_door, ped, vehicle, out_vec, door_index);
}

// --- IKChainManager_c::Update Hook ---
void* g_stub_ik_chain_update = nullptr;
fn_IKChainUpdate_t g_orig_ik_chain_update = nullptr;

inline bool ik_chain_has_null_entity_slot(void* self) {
    if (!self || !is_pointer_readable(self)) return false;
    void** head_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x1600);
    if (!is_pointer_readable(head_slot)) return false;
    void* node = *head_slot;
    for (int guard = 0; node && guard < 64; ++guard) {
        if (!is_pointer_readable(node)) return true;
        void** entity_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x10);
        if (!is_pointer_readable(entity_slot) || !*entity_slot) {
            return true;
        }
        void** next_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x8);
        if (!is_pointer_readable(next_slot)) return true;
        node = *next_slot;
    }
    return false;
}

void proxy_ik_chain_update(void* self, float dt) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return;
    // Original cbz x0 falls through to ldr [x0,#0x20] (tombstone_09, fault 0x20).
    if (self && ik_chain_has_null_entity_slot(self)) {
        LOGW("⚠️ [IKChainManager::Update] chain node with null entity — skip original");
        return;
    }
    SHADOWHOOK_CALL_PREV(proxy_ik_chain_update, self, dt);
}

// --- IKChainManager_c::IsFacingTarget Hook ---
// ped→intel→+0x58→indexed node→+0x18: engine skips null at +0x10 but not at +0x18 (tombstone_16–19, fault 0x20).
void* g_stub_ik_chain_is_facing_target = nullptr;
fn_IKChainIsFacingTarget_t g_orig_ik_chain_is_facing_target = nullptr;

// ped→intel→+0x58→+0x10 node (MakeAbortable / IK abort paths; tombstone_25).
inline bool ped_intel_ik_table_node_unsafe(void* ped) {
    if (!ped || !is_pointer_readable(ped)) return true;
    void** intel_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5e8);
    if (!is_pointer_readable(intel_slot) || !*intel_slot) return true;
    void* intel = *intel_slot;
    if (!is_pointer_readable(intel)) return true;
    void** table_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(intel) + 0x58);
    if (!is_pointer_readable(table_slot) || !*table_slot) return true;
    void* table = *table_slot;
    if (!is_pointer_readable(table)) return true;
    void** node_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(table) + 0x10);
    if (!is_pointer_readable(node_slot) || !*node_slot) return true;
    return !is_pointer_readable(*node_slot);
}

inline bool ped_heading_abort_write_safe(void* ped) {
    if (!ped || !is_pointer_readable(ped)) return false;
    if (!is_ped_pointer_valid_safe(ped)) return false;
    if (!is_pointer_readable(reinterpret_cast<char*>(ped) + 0x784)) return false;
    if (!is_pointer_readable(reinterpret_cast<char*>(ped) + 0x788)) return false;
    void** aux_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x7e8);
    if (is_pointer_readable(aux_slot) && *aux_slot) {
        void* aux = *aux_slot;
        if (!is_pointer_readable(aux)) return false;
        if (!is_pointer_readable(reinterpret_cast<char*>(aux) + 0x20)) return false;
    }
    return true;
}

inline bool ik_facing_target_ped_chain_unsafe(void* ped, int index) {
    if (!ped || !is_pointer_readable(ped)) return true;
    if (index < 0 || index > 32) return true;

    void** intel_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5e8);
    if (!is_pointer_readable(intel_slot) || !*intel_slot) return true;
    void* intel = *intel_slot;
    if (!is_pointer_readable(intel)) return true;

    void** table_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(intel) + 0x58);
    if (!is_pointer_readable(table_slot) || !*table_slot) return true;
    void* table = *table_slot;
    if (!is_pointer_readable(table)) return true;

    void** node_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(table) + index * 8 + 0x10);
    if (!is_pointer_readable(node_slot) || !*node_slot) return true;
    void* node = *node_slot;
    if (!is_pointer_readable(node)) return true;

    void** target_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x18);
    if (!is_pointer_readable(target_slot) || !*target_slot) return true;
    void* target = *target_slot;
    if (!is_pointer_readable(target)) return true;

    void** matrix_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(target) + 0x20);
    return !is_pointer_readable(matrix_slot) || !*matrix_slot;
}

bool proxy_ik_chain_is_facing_target(void* self, void* ped, int index) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (self && ik_chain_has_null_entity_slot(self)) return false;
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    if (ped && ik_facing_target_ped_chain_unsafe(ped, index)) {
        LOGW("⚠️ [IKChainManager::IsFacingTarget] broken ped intel chain (idx=%d) — return false", index);
        return false;
    }
    return SHADOWHOOK_CALL_PREV(proxy_ik_chain_is_facing_target, self, ped, index);
}

// --- CTaskManager::GetSimplestActiveTask Hook ---
// RE: slot null → cbnz 57aaa98; vtable+0x18 null → no cbz 57aaac4 (tombstone_41, fault 0x18).
void* g_stub_get_simplest_active_task = nullptr;
fn_GetSimplestActiveTask_t g_orig_get_simplest_active_task = nullptr;

void* proxy_get_simplest_active_task(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return nullptr;
    if (is_stability_sanitize_paused()) {
        if (is_task_manager_hotpath_paused()) return nullptr;
        return SHADOWHOOK_CALL_PREV(proxy_get_simplest_active_task, self);
    }
    sanitize_task_manager_slots(self, "GetSimplestActiveTask", 0x18);
    sanitize_task_manager_primary_chains(self, "GetSimplestActiveTask", 0x18);
    // RE: walks subtask chain via vtable+0x18 loop 57aaabc — unsafe child node crashes 57aaac4 (tombstone_44).
    for (int i = 0; i < 5; ++i) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + i * 8);
        if (!is_pointer_readable(task_slot)) continue;
        void* task = *task_slot;
        if (!task) continue;
        void* simplest = simplest_in_task_chain(task, 0x18);
        if (simplest) return simplest;
    }
    return nullptr;
}

// --- CTaskManager::GetSimplestTaskEi Hook ---
// RE: indexed slot null → cbz 57aaaf8; vtable+0x18 null → no cbz 57aab04 (same pattern as 41).
void* g_stub_get_simplest_task_ei = nullptr;
fn_GetSimplestTaskEi_t g_orig_get_simplest_task_ei = nullptr;

void* proxy_get_simplest_task_ei(void* self, int index) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return nullptr;
    if (is_stability_sanitize_paused()) {
        return SHADOWHOOK_CALL_PREV(proxy_get_simplest_task_ei, self, index);
    }
    if (index < 0 || index > 10) return nullptr;
    sanitize_task_manager_slots(self, "GetSimplestTaskEi", 0x18);
    if (index < 5) {
        sanitize_task_manager_primary_chains(self, "GetSimplestTaskEi", 0x18);
    }
    void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + index * 8);
    if (!is_pointer_readable(task_slot)) return nullptr;
    void* task = *task_slot;
    if (!task) return nullptr;
    return simplest_in_task_chain(task, 0x18);
}

// --- CEventScriptCommand destructor hooks (D0/D1) ---
void* g_stub_event_script_command_d0 = nullptr;
fn_EventScriptCommandDtor_t g_orig_event_script_command_d0 = nullptr;
void* g_stub_event_script_command_d1 = nullptr;
fn_EventScriptCommandDtor_t g_orig_event_script_command_d1 = nullptr;

void proxy_event_script_command_d0(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return;
    sanitize_event_script_command_task_slot(self, "CEventScriptCommand::D0");
    void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x18);
    if (is_pointer_readable(task_slot) && *task_slot &&
        task_slot_unsafe_for_vtable_call(*task_slot, 0x8)) {
        LOGW("⚠️ [CEventScriptCommand::D0] stale task after sanitize — skip original");
        return;
    }
    SHADOWHOOK_CALL_PREV(proxy_event_script_command_d0, self);
}

void proxy_event_script_command_d1(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return;
    sanitize_event_script_command_task_slot(self, "CEventScriptCommand::D1");
    void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x18);
    if (is_pointer_readable(task_slot) && *task_slot &&
        task_slot_unsafe_for_vtable_call(*task_slot, 0x8)) {
        LOGW("⚠️ [CEventScriptCommand::D1] stale task after sanitize — skip original");
        return;
    }
    SHADOWHOOK_CALL_PREV(proxy_event_script_command_d1, self);
}

// --- CEventScriptCommand::GetEventPriority Hook ---
void* g_stub_event_script_command_get_priority = nullptr;
fn_EventScriptCommandGetPriority_t g_orig_event_script_command_get_priority = nullptr;

int proxy_event_script_command_get_priority(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && is_pointer_readable(self)) {
        sanitize_event_script_command_task_slot(self, "CEventScriptCommand::GetEventPriority");
    }
    return SHADOWHOOK_CALL_PREV(proxy_event_script_command_get_priority, self);
}

// --- CPedGroupIntelligence::FlushTasks Hook ---
void* g_stub_flush_tasks = nullptr;
fn_FlushTasks_t g_orig_flush_tasks = nullptr;

inline void sanitize_ped_task_pair_slots(void* pair) {
    if (is_stability_sanitize_paused()) return;
    if (!pair || !is_pointer_readable(pair)) return;
    static constexpr size_t kTaskOffsets[] = {0x8, 0x28, 0x48, 0x68, 0x88, 0xa8, 0xc8, 0xe8};
    for (size_t off : kTaskOffsets) {
        void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(pair) + off);
        if (!is_pointer_readable(slot)) continue;
        void* task = *slot;
        if (task && !is_task_vtable_safe(task)) {
            LOGW("⚠️ [FlushTasks] Clearing unsafe task %p at pair+0x%zx", task, off);
            *slot = nullptr;
        }
    }
}

void proxy_flush_tasks(void* self, void* pair, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (is_stability_sanitize_paused()) {
        SHADOWHOOK_CALL_PREV(proxy_flush_tasks, self, pair, ped);
        return;
    }
    if (pair) {
        sanitize_ped_task_pair_slots(pair);
    }
    SHADOWHOOK_CALL_PREV(proxy_flush_tasks, self, pair, ped);
}

// --- CCam::Process_FollowPed_SA Hook ---
void* g_stub_process_follow_ped_sa = nullptr;
fn_ProcessFollowPedSA_t g_orig_process_follow_ped_sa = nullptr;

void proxy_process_follow_ped_sa(void* self, const CVector& target, float f1, float f2, float f3, bool b1) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return;
    SHADOWHOOK_CALL_PREV(proxy_process_follow_ped_sa, self, target, f1, f2, f3, b1);
}

// --- MakeAbortable shared guards (CEventHandler::HandleEvents hot path) ---
inline bool make_abortable_event_unsafe(void* event) {
    if (!event) return false; // cbz-first: null event → caller must CALL_PREV
    if (!is_userspace_address(event) || !is_pointer_readable(event)) return true;
    void** vtable_slot = reinterpret_cast<void**>(event);
    return !is_pointer_readable(vtable_slot) || !*vtable_slot;
}

inline bool make_abortable_ped_field_unsafe(void* ped, size_t field_offset) {
    if (!ped) return false;
    if (!is_pointer_readable(ped)) return true;
    if (!is_ped_pointer_valid_safe(ped)) return true;
    return !is_pointer_readable(reinterpret_cast<char*>(ped) + field_offset);
}

inline bool task_subtask_vtable_fn_unsafe(void* self, size_t subtask_offset, size_t vtable_offset) {
    if (!self || !is_pointer_readable(self)) return true;
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + subtask_offset);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) return false;
    void* sub = *sub_slot;
    if (!is_task_vtable_safe(sub)) return true;
    void** fn_slot = reinterpret_cast<void**>(
        reinterpret_cast<char*>(*reinterpret_cast<void**>(sub)) + vtable_offset);
    return !is_pointer_readable(fn_slot) || !*fn_slot || !is_pointer_readable(*fn_slot);
}

// Complex task MakeAbortable often tailcalls subtask+0x10 → vtable+0x38 with no null cbz.
inline bool make_abortable_subtask_tailcall_unsafe(void* self, size_t vtable_offset = 0x38) {
    if (!self || !is_pointer_readable(self)) return true;
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) return true;
    return task_subtask_vtable_fn_unsafe(self, 0x10, vtable_offset);
}

// --- CTaskComplexLeaveCar::MakeAbortable Hook ---
void* g_stub_leave_car_make_abortable = nullptr;
fn_LeaveCarMakeAbortable_t g_orig_leave_car_make_abortable = nullptr;

bool proxy_leave_car_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_leave_car_make_abortable, self, ped, priority, event);
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;

    if (self) {
        sanitize_unsafe_subtask_at(self, 0x10);
    }
    return SHADOWHOOK_CALL_PREV(proxy_leave_car_make_abortable, self, ped, priority, event);
}

// --- CTaskSimpleGoToPoint::MakeAbortable Hook ---
void* g_stub_goto_point_make_abortable = nullptr;
fn_GoToPointMakeAbortable_t g_orig_goto_point_make_abortable = nullptr;

bool proxy_goto_point_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_goto_point_make_abortable, self, ped, priority, event);
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    if (ped && ped_intel_ik_table_node_unsafe(ped)) {
        LOGW("⚠️ [GoToPoint::MakeAbortable] broken ped intel ik chain — return false");
        return false;
    }
    return SHADOWHOOK_CALL_PREV(proxy_goto_point_make_abortable, self, ped, priority, event);
}

// --- CTaskSimpleAchieveHeading::MakeAbortable Hook ---
void* g_stub_achieve_heading_make_abortable = nullptr;
fn_AchieveHeadingMakeAbortable_t g_orig_achieve_heading_make_abortable = nullptr;

bool proxy_achieve_heading_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_achieve_heading_make_abortable, self, ped, priority, event);
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    if (ped && ped_intel_ik_table_node_unsafe(ped)) {
        LOGW("⚠️ [AchieveHeading::MakeAbortable] broken ped intel ik chain — return false");
        return false;
    }
    if (ped && !ped_heading_abort_write_safe(ped)) {
        LOGW("⚠️ [AchieveHeading::MakeAbortable] stale ped %p heading fields — return false", ped);
        return false;
    }
    return SHADOWHOOK_CALL_PREV(proxy_achieve_heading_make_abortable, self, ped, priority, event);
}

// --- CTaskComplexFollowPointRoute::MakeAbortable Hook ---
void* g_stub_follow_point_route_make_abortable = nullptr;
fn_FollowPointRouteMakeAbortable_t g_orig_follow_point_route_make_abortable = nullptr;

bool proxy_follow_point_route_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_follow_point_route_make_abortable, self, ped, priority, event);
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) {
        LOGW("⚠️ [FollowPointRoute::MakeAbortable] stale event %p — return false", event);
        return false;
    }
    if (self) {
        void** route_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x30);
        if (is_pointer_readable(route_slot) && *route_slot && !is_pointer_readable(*route_slot)) {
            return false;
        }
        sanitize_unsafe_subtask_at(self, 0x10);
        if (task_subtask_vtable_fn_unsafe(self, 0x10, 0x28)) return false;
    }
    return SHADOWHOOK_CALL_PREV(proxy_follow_point_route_make_abortable, self, ped, priority, event);
}

// --- CTaskComplexKillCriminal::MakeAbortable Hook ---
// Mod injects KillCriminal tasks; stale m_pTarget (typically +0x18) → fault 0x0 (tombstone_33).
void* g_stub_kill_criminal_make_abortable = nullptr;
fn_KillCriminalMakeAbortable_t g_orig_kill_criminal_make_abortable = nullptr;

inline bool kill_criminal_task_target_unsafe(void* self) {
    if (!self || !is_pointer_readable(self)) return true;
    void** target_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x18);
    if (!is_pointer_readable(target_slot)) return false;
    void* target = *target_slot;
    if (!target) return false;
    return !is_ped_pointer_valid_safe(reinterpret_cast<CPed*>(target));
}

bool proxy_kill_criminal_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_kill_criminal_make_abortable, self, ped, priority, event);
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    if (kill_criminal_task_target_unsafe(self)) {
        void** target_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x18);
        if (is_pointer_readable(target_slot) && *target_slot) {
            LOGW("⚠️ [KillCriminal::MakeAbortable] stale target %p — clear to nullptr, delegate abort to engine",
                 *target_slot);
            *target_slot = nullptr;
        }
    }
    sanitize_unsafe_subtask_at(self, 0x10);
    // RE: subtask at +0x10 null → no cbz 57c2910; tailcall vtable+0x38 57c292c (tombstone_49).
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) {
        LOGW("⚠️ [KillCriminal::MakeAbortable] null subtask — return true");
        return true;
    }
    if (make_abortable_subtask_tailcall_unsafe(self, 0x38)) {
        LOGW("⚠️ [KillCriminal::MakeAbortable] unsafe subtask after sanitize — return true");
        return true;
    }
    return SHADOWHOOK_CALL_PREV(proxy_kill_criminal_make_abortable, self, ped, priority, event);
}

// --- CTaskComplexFallAndGetUp::MakeAbortable Hook ---
void* g_stub_fall_and_get_up_make_abortable = nullptr;
fn_FallAndGetUpMakeAbortable_t g_orig_fall_and_get_up_make_abortable = nullptr;

bool proxy_fall_and_get_up_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_fall_and_get_up_make_abortable, self, ped, priority, event);
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    sanitize_unsafe_subtask_at(self, 0x10);
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) {
        LOGW("⚠️ [FallAndGetUp::MakeAbortable] missing subtask — return true");
        return true;
    }
    return SHADOWHOOK_CALL_PREV(proxy_fall_and_get_up_make_abortable, self, ped, priority, event);
}

// --- CTaskComplexPlayHandSignalAnim::ControlSubTask Hook ---
void* g_stub_play_hand_signal_control_sub_task = nullptr;
fn_PlayHandSignalControlSubTask_t g_orig_play_hand_signal_control_sub_task = nullptr;

void* proxy_play_hand_signal_control_sub_task(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    CONTROL_SUB_TASK_SAVE_LOAD_FASTPATH(proxy_play_hand_signal_control_sub_task, self, ped);
    if (!self || !is_pointer_readable(self)) return nullptr;
    if (ped && !is_pointer_readable(ped)) return nullptr;
    sanitize_unsafe_subtask_at(self, 0x10);
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) {
        LOGW("⚠️ [PlayHandSignalAnim::ControlSubTask] no subtask — skip original");
        return nullptr;
    }
    return SHADOWHOOK_CALL_PREV(proxy_play_hand_signal_control_sub_task, self, ped);
}

// --- CTaskComplexKillPedOnFoot::MakeAbortable Hook ---
void* g_stub_kill_ped_on_foot_make_abortable = nullptr;
fn_KillPedOnFootMakeAbortable_t g_orig_kill_ped_on_foot_make_abortable = nullptr;

bool proxy_kill_ped_on_foot_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_kill_ped_on_foot_make_abortable, self, ped, priority, event);
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    if (ped && make_abortable_ped_field_unsafe(ped, 0x63c)) {
        LOGW("⚠️ [KillPedOnFoot::MakeAbortable] stale ped %p +0x63c — return true", ped);
        return true;
    }
    if (self) {
        sanitize_unsafe_subtask_at(self, 0x10);
        void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
        if (!is_pointer_readable(sub_slot) || !*sub_slot) {
            LOGW("⚠️ [KillPedOnFoot::MakeAbortable] null subtask — return true");
            return true;
        }
        if (make_abortable_subtask_tailcall_unsafe(self, 0x38)) return true;
    }
    return SHADOWHOOK_CALL_PREV(proxy_kill_ped_on_foot_make_abortable, self, ped, priority, event);
}

// --- CTaskComplexKillPedOnFootArmed::MakeAbortable Hook ---
// RE: subtask at +0x10 null → no cbz 56e9308; tailcall vtable+0x38 56e9324 (tombstone_47).
void* g_stub_kill_ped_on_foot_armed_make_abortable = nullptr;
fn_KillPedOnFootArmedMakeAbortable_t g_orig_kill_ped_on_foot_armed_make_abortable = nullptr;

bool proxy_kill_ped_on_foot_armed_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_kill_ped_on_foot_armed_make_abortable, self, ped, priority, event);
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    sanitize_unsafe_subtask_at(self, 0x10);
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) {
        LOGW("⚠️ [KillPedOnFootArmed::MakeAbortable] null subtask — return true");
        return true;
    }
    if (make_abortable_subtask_tailcall_unsafe(self, 0x38)) return true;
    return SHADOWHOOK_CALL_PREV(proxy_kill_ped_on_foot_armed_make_abortable, self, ped, priority, event);
}

// --- CTaskSimpleAnim::MakeAbortable Hook ---
void* g_stub_simple_anim_make_abortable = nullptr;
fn_SimpleAnimMakeAbortable_t g_orig_simple_anim_make_abortable = nullptr;

bool proxy_simple_anim_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_simple_anim_make_abortable, self, ped, priority, event);
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) {
        LOGW("⚠️ [SimpleAnim::MakeAbortable] stale event %p — return false", event);
        return false;
    }
    if (self) {
        sanitize_unsafe_subtask_at(self, 0x18);
        if (task_subtask_vtable_fn_unsafe(self, 0x18, 0x28)) return false;
    }
    return SHADOWHOOK_CALL_PREV(proxy_simple_anim_make_abortable, self, ped, priority, event);
}

// cbz-first policy (.agents/AGENTS.md): nullptr → CALL_PREV only when RE shows cbz/cbnz;
// no cbz → mod may skip/short-circuit. Sanitize/clear only non-null stale slots.

inline bool ped_slot_stale_nonnull(void* owner, size_t offset) {
    if (!owner || !is_pointer_readable(owner)) return false;
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(owner) + offset);
    if (!is_pointer_readable(slot)) return false;
    void* ped = *slot;
    if (!ped) return false;
    return !is_ped_pointer_valid_safe(ped);
}

inline void clear_ped_slot_if_stale(void* owner, size_t offset, const char* label) {
    if (is_stability_sanitize_paused()) return;
    if (!ped_slot_stale_nonnull(owner, offset)) return;
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(owner) + offset);
    LOGW("⚠️ [%s] stale ped %p at +0x%zx — clear, delegate to engine (cbz path)",
         label, *slot, offset);
    *slot = nullptr;
}

inline bool task_slot_stale_nonnull(void* self, size_t offset) {
    if (!self || !is_pointer_readable(self)) return false;
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + offset);
    if (!is_pointer_readable(slot)) return false;
    void* target = *slot;
    if (!target) return false;
    return !is_ped_pointer_valid_safe(target);
}

inline void clear_task_ped_slot_if_stale(void* self, size_t offset, const char* label) {
    if (is_stability_sanitize_paused()) return;
    if (!task_slot_stale_nonnull(self, offset)) return;
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + offset);
    LOGW("⚠️ [%s] stale target %p at +0x%zx — clear, delegate to engine (cbz path)",
         label, *slot, offset);
    *slot = nullptr;
}

// Non-null pointer to zero-filled CPedIntelligence (vtable == null).
inline bool intelligence_zero_filled(void* self) {
    if (!self || !is_pointer_readable(self)) return false;
    void** vtable_slot = reinterpret_cast<void**>(self);
    return is_pointer_readable(vtable_slot) && !*vtable_slot;
}

inline void sanitize_intel_tasks_if_present(void* ped, const char* tag) {
    if (!ped || !is_ped_pointer_valid_safe(ped)) return;
    void* intel = get_ped_intelligence(reinterpret_cast<CPed*>(ped));
    if (!intel || intelligence_zero_filled(intel)) return;
    sanitize_task_manager_slots(reinterpret_cast<char*>(intel) + 8, tag);
}

inline bool ped_intel_primary_tasks_unsafe_for_event(void* ped, size_t vtable_fn_offset = 0x18) {
    if (!ped || !is_ped_pointer_valid_safe(ped)) return true;
    void* intel = get_ped_intelligence(reinterpret_cast<CPed*>(ped));
    if (!intel || !is_pointer_readable(intel) || intelligence_zero_filled(intel)) return true;
    void* task_mgr = reinterpret_cast<char*>(intel) + 8;
    sanitize_task_manager_slots(task_mgr, "ped_intel_primary_tasks", vtable_fn_offset);
    sanitize_task_manager_primary_chains(task_mgr, "ped_intel_primary_tasks", vtable_fn_offset);
    for (int i = 0; i < 5; ++i) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
        if (!is_pointer_readable(task_slot)) continue;
        void* task = *task_slot;
        if (!task) continue;
        if (task_chain_walk_unsafe(task, vtable_fn_offset)) return true;
        if (task_slot_unsafe_for_vtable_call(task, 0x28)) return true;
    }
    return false;
}

// --- CTaskComplexWanderCop::LookForCriminals Hook ---
// RE: ped+0x5e8 task slots → vtable+0x28 57855c4 (tombstone_01).
void* g_stub_wander_cop_look_for_criminals = nullptr;
fn_WanderCopLookForCriminals_t g_orig_wander_cop_look_for_criminals = nullptr;

void proxy_wander_cop_look_for_criminals(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (is_stability_sanitize_paused()) {
        if (!self || !is_pointer_readable(self)) return;
        SHADOWHOOK_CALL_PREV(proxy_wander_cop_look_for_criminals, self, ped);
        return;
    }
    if (!self || !is_pointer_readable(self)) return;
    if (!ped || !is_pointer_readable(ped) || !is_ped_pointer_valid_safe(ped)) return;
    if (ped_intel_primary_tasks_unsafe_for_event(ped, 0x28)) {
        LOGW("⚠️ [WanderCop::LookForCriminals] unsafe ped %p tasks — skip original", ped);
        return;
    }
    SHADOWHOOK_CALL_PREV(proxy_wander_cop_look_for_criminals, self, ped);
}

// --- CTaskComplexKillCriminal::CreateFirstSubTask Hook ---
// RE: target at +0x18 null → cbz 57c2c80; stale target intel+0x5e8 no cbz 57c309c (tombstone_02).
void* g_stub_kill_criminal_create_first_sub_task = nullptr;
fn_KillCriminalCreateFirstSubTask_t g_orig_kill_criminal_create_first_sub_task = nullptr;

void* proxy_kill_criminal_create_first_sub_task(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    CONTROL_SUB_TASK_SAVE_LOAD_FASTPATH(proxy_kill_criminal_create_first_sub_task, self, ped);
    if (!self || !is_pointer_readable(self)) return nullptr;
    if (ped && !is_pointer_readable(ped)) return nullptr;
    clear_task_ped_slot_if_stale(self, 0x18, "KillCriminal::CreateFirstSubTask");
    void** target_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x18);
    if (!is_pointer_readable(target_slot) || !*target_slot) {
        LOGW("⚠️ [KillCriminal::CreateFirstSubTask] null target — skip hate inject");
        return nullptr;
    }
    if (!is_ped_pointer_valid_safe(*target_slot)) {
        LOGW("⚠️ [KillCriminal::CreateFirstSubTask] stale target %p — clear slot", *target_slot);
        *target_slot = nullptr;
        return nullptr;
    }
    return SHADOWHOOK_CALL_PREV(proxy_kill_criminal_create_first_sub_task, self, ped);
}

// --- CEventPotentialWalkIntoVehicle::AffectsPed Hook ---
// RE: ped intel task chain vtable+0x18/0x28; null task deref 54ca9e8 (tombstone_48).
void* g_stub_event_walk_into_vehicle_affects_ped = nullptr;
fn_EventPotentialWalkIntoVehicleAffectsPed_t g_orig_event_walk_into_vehicle_affects_ped = nullptr;

bool proxy_event_walk_into_vehicle_affects_ped(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (is_stability_sanitize_paused()) {
        if (is_task_manager_hotpath_paused()) return false;
        if (!self || !is_pointer_readable(self)) return false;
        if (!ped || !is_pointer_readable(ped)) return false;
        return SHADOWHOOK_CALL_PREV(proxy_event_walk_into_vehicle_affects_ped, self, ped);
    }
    if (!self || !is_pointer_readable(self)) return false;
    if (!ped || !is_pointer_readable(ped) || !is_ped_pointer_valid_safe(ped)) return false;
    if (ped_intel_primary_tasks_unsafe_for_event(ped, 0x18)) {
        LOGW("⚠️ [WalkIntoVehicle::AffectsPed] unsafe ped %p task chain — return false", ped);
        return false;
    }
    return SHADOWHOOK_CALL_PREV(proxy_event_walk_into_vehicle_affects_ped, self, ped);
}

// RE: ComputePedCollision — no cbz on event* before 54d6fd0.
inline bool compute_collision_event_unreadable(void* event) {
    return !event || !is_pointer_readable(event) || make_abortable_event_unsafe(event);
}

// --- CTaskSimpleArrestPed::MakeAbortable Hook ---
// RE: arrest ped at task+0x10 (StartAnim 57bee4c); event nullptr cbz 57bed74; stale non-null event deref 57bed78.
void* g_stub_simple_arrest_ped_make_abortable = nullptr;
fn_SimpleArrestPedMakeAbortable_t g_orig_simple_arrest_ped_make_abortable = nullptr;

bool proxy_simple_arrest_ped_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_simple_arrest_ped_make_abortable, self, ped, priority, event);
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    // event == nullptr: engine cbz 57bed74 — do not intercept.
    if (event && make_abortable_event_unsafe(event)) return false;
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    clear_task_ped_slot_if_stale(self, 0x10, "SimpleArrestPed::MakeAbortable");
    return SHADOWHOOK_CALL_PREV(proxy_simple_arrest_ped_make_abortable, self, ped, priority, event);
}

// --- CTaskComplexBeInGroup::MakeAbortable Hook ---
// RE: subtask at +0x10 null → no cbz 5708650 (tombstone_45, fault 0x0).
void* g_stub_be_in_group_make_abortable = nullptr;
fn_BeInGroupMakeAbortable_t g_orig_be_in_group_make_abortable = nullptr;

bool proxy_be_in_group_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_be_in_group_make_abortable, self, ped, priority, event);
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    sanitize_unsafe_subtask_at(self, 0x10);
    sanitize_unsafe_task_slot_at(self, 0x28, "BeInGroup secondary");
    sanitize_unsafe_task_slot_at(self, 0x38, "BeInGroup tertiary");
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) {
        LOGW("⚠️ [BeInGroup::MakeAbortable] null subtask (no cbz) — return false");
        return false;
    }
    if (task_subtask_vtable_fn_unsafe(self, 0x10, 0x38)) return false;
    return SHADOWHOOK_CALL_PREV(proxy_be_in_group_make_abortable, self, ped, priority, event);
}

// --- CTaskComplexArrestPed::MakeAbortable Hook ---
// RE: ctor stores ped at task+0x20 (57bf210); subtask at +0x10 tailcalled (57bf3b8).
void* g_stub_complex_arrest_ped_make_abortable = nullptr;
fn_ComplexArrestPedMakeAbortable_t g_orig_complex_arrest_ped_make_abortable = nullptr;

bool proxy_complex_arrest_ped_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    MAKE_ABORTABLE_SAVE_LOAD_FASTPATH(proxy_complex_arrest_ped_make_abortable, self, ped, priority, event);
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    clear_task_ped_slot_if_stale(self, 0x20, "ComplexArrestPed::MakeAbortable");
    sanitize_unsafe_subtask_at(self, 0x10);
    // RE: subtask at +0x10 null → no cbz 57bf3b8 — mod short-circuit.
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) {
        LOGW("⚠️ [ComplexArrestPed::MakeAbortable] null subtask (no cbz) — return false");
        return false;
    }
    if (!is_task_vtable_safe(*sub_slot)) {
        LOGW("⚠️ [ComplexArrestPed::MakeAbortable] stale subtask — return false");
        return false;
    }
    if (task_subtask_vtable_fn_unsafe(self, 0x10, 0x28)) return true;
    return SHADOWHOOK_CALL_PREV(proxy_complex_arrest_ped_make_abortable, self, ped, priority, event);
}

// --- CPedIntelligence::ProcessAfterProcCol Hook ---
// RE: task slots null → cbnz/cbz 55debf8–55dec18; vtable+0x18 null → no cbz 55dec24 (tombstone_37/42).
void* g_stub_process_after_proc_col = nullptr;
fn_ProcessAfterProcCol_t g_orig_process_after_proc_col = nullptr;

void proxy_process_after_proc_col(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    STABILITY_VOID_SAVE_LOAD_FASTPATH(proxy_process_after_proc_col, self);
    // RE: self null → no cbz 55debf4; zero-filled vtable → no cbz.
    if (!self || !is_pointer_readable(self) || intelligence_zero_filled(self)) return;
    void* task_mgr = reinterpret_cast<char*>(self) + 8;
    sanitize_task_manager_slots(task_mgr, "ProcessAfterProcCol", 0x18);
    sanitize_task_manager_primary_chains(task_mgr, "ProcessAfterProcCol", 0x18);
    // RE: primary + secondary loops walk vtable+0x18 chain 55dec1c/55dec70 (tombstone_46).
    for (int i = 0; i < 11; ++i) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
        if (!is_pointer_readable(task_slot)) continue;
        void* task = *task_slot;
        if (task && task_chain_walk_unsafe(task, 0x18)) {
            LOGW("⚠️ [ProcessAfterProcCol] unsafe chain at slot %d — skip original", i);
            return;
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_process_after_proc_col, self);
}

// --- CCollisionEventScanner::ScanForCollisionEvents Hook ---
// RE: ped+0x5e8 intel loaded with no cbz 55da968; stale task vtable+0x18 fault 55da9a0 (tombstone_38).
void* g_stub_scan_for_collision_events = nullptr;
fn_ScanForCollisionEvents_t g_orig_scan_for_collision_events = nullptr;

void proxy_scan_for_collision_events(void* self, void* ped, void* event_group) {
    SHADOWHOOK_STACK_SCOPE();
    if (is_stability_sanitize_paused()) {
        if (!ped || !is_pointer_readable(ped)) return;
        SHADOWHOOK_CALL_PREV(proxy_scan_for_collision_events, self, ped, event_group);
        return;
    }
    // RE: ped null → no cbz before 55da968; ped+0x5e8 intel null → no cbz 55da968.
    if (!ped || !is_pointer_readable(ped) || !is_ped_pointer_valid_safe(ped)) return;
    void* intel = get_ped_intelligence(reinterpret_cast<CPed*>(ped));
    if (!intel || !is_pointer_readable(intel) || intelligence_zero_filled(intel)) return;
    sanitize_task_manager_slots(reinterpret_cast<char*>(intel) + 8, "ScanForCollisionEvents");
    SHADOWHOOK_CALL_PREV(proxy_scan_for_collision_events, self, ped, event_group);
}

// --- CEventHandler::ComputePedCollisionWithPedResponse Hook ---
// RE: event+0x18 ped null → cbz 54d6fd4; stale ped intel tasks → 54d7014; task3 null → cbz 54d70ec.
void* g_stub_compute_ped_collision_with_ped_response = nullptr;
fn_ComputePedCollisionWithPedResponse_t g_orig_compute_ped_collision_with_ped_response = nullptr;

void* proxy_compute_ped_collision_with_ped_response(void* self, void* event, void* task, void* task2) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return nullptr;
    if (is_stability_sanitize_paused()) {
        if (is_task_manager_hotpath_paused()) return nullptr;
        return SHADOWHOOK_CALL_PREV(proxy_compute_ped_collision_with_ped_response, self, event, task, task2);
    }
    if (compute_collision_event_unreadable(event)) return nullptr;
    clear_ped_slot_if_stale(event, 0x18, "ComputePedCollision");
    void** ped_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(event) + 0x18);
    if (is_pointer_readable(ped_slot) && *ped_slot) {
        sanitize_intel_tasks_if_present(*ped_slot, "ComputePedCollisionWithPedResponse");
    }
    // task/task2 null → cbz 54d70ec on x3; non-null stale vtable → no cbz.
    if (task && !is_task_vtable_safe(task)) return nullptr;
    if (task2 && !is_task_vtable_safe(task2)) return nullptr;
    return SHADOWHOOK_CALL_PREV(proxy_compute_ped_collision_with_ped_response, self, event, task, task2);
}

// --- CCarAI::UpdateCarAI Hook ---
void* g_stub_update_car_ai = nullptr;
fn_UpdateCarAI_t g_orig_update_car_ai = nullptr;

void proxy_update_car_ai(void* vehicle) {
    SHADOWHOOK_STACK_SCOPE();
    if (vehicle && !is_pointer_readable(vehicle)) return;
    if (vehicle) {
        void** vtable_ptr = reinterpret_cast<void**>(vehicle);
        if (is_pointer_readable(vtable_ptr) && *vtable_ptr == nullptr) {
            LOGW("⚠️ [UpdateCarAI] vehicle (%p) is zero-filled! Intercepting to prevent crash.", vehicle);
            return;
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_update_car_ai, vehicle);
}

// --- CTaskComplexFacial::ControlSubTask Hook ---
void* g_stub_facial_control_sub_task = nullptr;
fn_FacialControlSubTask_t g_orig_facial_control_sub_task = nullptr;

void* proxy_facial_control_sub_task(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    CONTROL_SUB_TASK_SAVE_LOAD_FASTPATH(proxy_facial_control_sub_task, self, ped);
    if (!self || !is_pointer_readable(self)) return nullptr;
    if (ped && !is_pointer_readable(ped)) return nullptr;

    if (self) {
        sanitize_unsafe_subtask_at(self, 0x10);
        // Original dereferences [self+0x10] without null check (tombstone_07, fault 0x0).
        void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
        if (!is_pointer_readable(sub_slot) || !*sub_slot) {
            LOGW("⚠️ [Facial::ControlSubTask] no subtask after sanitize — skip original");
            return nullptr;
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_facial_control_sub_task, self, ped);
}

// --- CTaskSimpleIKManager::ProcessPed Hook ---
// Subtasks at +0x10/+0x18: engine loads vtable then [vtable+0x48] with no null-vtable guard (tombstone_12).
void* g_stub_ik_manager_process_ped = nullptr;
fn_IKManagerProcessPed_t g_orig_ik_manager_process_ped = nullptr;

void proxy_ik_manager_process_ped(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (is_stability_sanitize_paused()) {
        if (!self || !is_pointer_readable(self)) return;
        SHADOWHOOK_CALL_PREV(proxy_ik_manager_process_ped, self, ped);
        return;
    }
    if (!self || !is_pointer_readable(self)) return;
    if (ped && !is_pointer_readable(ped)) return;

    if (self) {
        for (size_t off : {0x10u, 0x18u}) {
            sanitize_unsafe_subtask_at(self, off);
        }
    }
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    SHADOWHOOK_CALL_PREV(proxy_ik_manager_process_ped, self, ped);
}

// --- CCarCtrl::IsPoliceVehicleInPursuit Hook ---
// Outer entry takes pool index; inner thunk at +0x2b8 loads vehicle+0x10 without guard (tombstone_14/21/22/24).
void* g_stub_is_police_vehicle_in_pursuit = nullptr;
fn_IsPoliceVehicleInPursuit_t g_orig_is_police_vehicle_in_pursuit = nullptr;
void* g_stub_vehicle_pursuit_ai_thunk = nullptr;
fn_VehiclePursuitAiThunk_t g_orig_vehicle_pursuit_ai_thunk = nullptr;

inline bool vehicle_ai_subobject_chain_safe(void* vehicle) {
    if (!vehicle || !is_pointer_readable(vehicle)) return false;
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(vehicle) + 0x10);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) return false;
    void* sub = *sub_slot;
    if (!is_pointer_readable(sub)) return false;
    void** vtable_slot = reinterpret_cast<void**>(sub);
    if (!is_pointer_readable(vtable_slot) || !*vtable_slot) return false;
    void** fn_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(*vtable_slot) + 0x38);
    return is_pointer_readable(fn_slot) && *fn_slot && is_pointer_readable(*fn_slot);
}

void* proxy_vehicle_pursuit_ai_thunk(void* vehicle) {
    SHADOWHOOK_STACK_SCOPE();
    if (!vehicle_ai_subobject_chain_safe(vehicle)) {
        LOGW("⚠️ [IsPoliceVehicleInPursuit thunk] vehicle %p ai chain unsafe — skip", vehicle);
        return nullptr;
    }
    return SHADOWHOOK_CALL_PREV(proxy_vehicle_pursuit_ai_thunk, vehicle);
}

bool proxy_is_police_vehicle_in_pursuit(int vehicle_index) {
    SHADOWHOOK_STACK_SCOPE();
    if (vehicle_index < 0) return false;
    if (g_GetPoolVehicle) {
        void* vehicle = g_GetPoolVehicle(vehicle_index);
        if (vehicle && !vehicle_ai_subobject_chain_safe(vehicle)) {
            return false;
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_is_police_vehicle_in_pursuit, vehicle_index);
}

// --- CTaskComplexWanderStandard::LookForChatPartners Hook ---
// Scans partner intel task slots +0x8…+0x28; zero-filled task → vtable+0x28 (tombstone_15).
void* g_stub_wander_look_for_chat_partners = nullptr;
fn_WanderLookForChatPartners_t g_orig_wander_look_for_chat_partners = nullptr;

inline void sanitize_wander_chat_partner_cache(void* ped) {
    if (!ped || !is_pointer_readable(ped)) return;
    void** intel_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5e8);
    if (!is_pointer_readable(intel_slot)) return;
    void* intel = *intel_slot;
    if (!intel || !is_pointer_readable(intel)) return;

    for (int i = 0; i < 16; ++i) {
        void** partner_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(intel) + 0x228 + i * 8);
        if (!is_pointer_readable(partner_slot)) continue;
        void* partner = *partner_slot;
        if (partner && is_ped_pointer_valid_safe(partner)) {
            sanitize_ped_tasks(partner);
        }
    }
    sanitize_task_manager_slots(reinterpret_cast<char*>(intel) + 8, "WanderLookForChatPartners");
}

void proxy_wander_look_for_chat_partners(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (is_stability_sanitize_paused()) {
        if (!self || !is_pointer_readable(self)) return;
        SHADOWHOOK_CALL_PREV(proxy_wander_look_for_chat_partners, self, ped);
        return;
    }
    if (!self || !is_pointer_readable(self)) return;
    if (ped && !is_pointer_readable(ped)) return;

    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_wander_chat_partner_cache(ped);
        sanitize_ped_tasks(ped);
    }
    SHADOWHOOK_CALL_PREV(proxy_wander_look_for_chat_partners, self, ped);
}
