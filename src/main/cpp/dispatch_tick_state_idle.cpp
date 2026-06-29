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
#include <algorithm>

#include "shadowhook.h"
#include "third_party/xdl/xdl.h"

#include "log.hpp"
#include "game_config.hpp"
#include "game_types.hpp"
#include "pointer_sanitizer.hpp"
#include "mod_shared.hpp"
#include "ecs_engine.hpp"
#include "dispatch_tick_internal.hpp"


void dispatch_tick_state_idle(const std::shared_ptr<CrimeEvent>& crime) {
    // 🚧 [动态警戒禁行区]：自动为活跃犯罪现场设置 50 米的路障/封路禁行，阻挡平民 NPC 车辆冲入现场
    if (!crime->road_closure_active) {
        if (g_ThePaths && g_SwitchRoadsOffInArea) {
            CVector center = crime->location;
            g_SwitchRoadsOffInArea(
                g_ThePaths,
                center.x - 50.0f, center.y - 50.0f, center.z - 15.0f,
                center.x + 50.0f, center.y + 50.0f, center.z + 15.0f,
                true, true, false
            );
            crime->road_closure_active = true;
            crime->road_closure_center = center;
            LOGI("🚧 [dispatchCenter - Cordon] Established dynamic police road closure within 50m around (%.1f, %.1f, %.1f) for case %llu",
                 center.x, center.y, center.z, (unsigned long long)crime->case_id);
        }
    }

    if (!crime->dispatch_sent) {
        float dist_to_cop = 9999.0f;
        if (g_FindDistToNearestCop) {
            dist_to_cop = g_FindDistToNearestCop(PED_TYPE_COP, crime->location);
        }

        if (dist_to_cop < 50.0f) {
            LOGI("Cops nearby (dist=%.1f) for case %llu, transition to STATE_ON_SCENE directly", dist_to_cop, (unsigned long long)crime->case_id);
            crime->dispatch_sent = true;
            crime->on_scene_start = now_ms();
            crime->dispatch_state = STATE_ON_SCENE;
            crime->last_cops_killed = 0;
        } else {
            float search_radius = compute_nearby_cop_search_radius(crime);
            int mobilized = dispatch_nearby_available_cops_for_crime_auto(crime);
            if (mobilized > 0) {
                LOGI("Nearby cop dispatch for case %llu mobilized %d officers (dist_to_nearest=%.1f) -> STATE_ON_SCENE",
                     (unsigned long long)crime->case_id, mobilized, dist_to_cop);
                crime->dispatch_sent = true;
                crime->on_scene_start = now_ms();
                crime->dispatch_state = STATE_ON_SCENE;
                crime->last_cops_killed = 0;
            } else {
                // 附近无可调度警员，回退计时后刷增援车
                crime->dispatch_delay_ms = crime->is_firearm ?
                    get_random_range(4000, 7000) : get_random_range(8000, 12000);
                crime->timer_start = now_ms();
                crime->dispatch_state = STATE_TIMING;
                crime->last_cops_killed = 0;
                LOGI("No mobilizable cops within %.0fm for case %llu, starting spawn timer: %d ms",
                     search_radius, (unsigned long long)crime->case_id, crime->dispatch_delay_ms);
            }
        }
    }
}