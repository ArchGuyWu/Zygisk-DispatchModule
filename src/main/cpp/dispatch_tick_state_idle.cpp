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
#include "dispatch_timing.hpp"


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
            dist_to_cop = g_FindDistToNearestCop(PED_TYPE_COP, get_crime_dispatch_position(*crime));
        }

        float av_range = dispatch_timing::get_av_range_for_crime(*crime);
        if (dispatch_timing::is_cop_within_native_av(dist_to_cop, *crime)) {
            LOGI("Cop within native AV (dist=%.1f < %.0fm, firearm=%d) for case %llu -> STATE_ON_SCENE",
                 dist_to_cop, av_range, crime->is_firearm ? 1 : 0, (unsigned long long)crime->case_id);
            crime->dispatch_sent = true;
            crime->on_scene_start = now_ms();
            crime->dispatch_state = STATE_ON_SCENE;
            crime->last_cops_killed = 0;
            make_cops_attack_criminal_immediate(crime->criminal);
        } else {
            // 视听外：先留与 AV 类型匹配的自然缓冲，再轮询附近警员，最后刷增援车
            int64_t cur_time = now_ms();
            crime->timer_start = cur_time;
            crime->last_nearby_dispatch_attempt_ms = 0;
            crime->dispatch_delay_ms = dispatch_timing::compute_spawn_fallback_delay_ms(crime->is_firearm);
            crime->dispatch_state = STATE_TIMING;
            crime->last_cops_killed = 0;
            LOGI("Case %llu: nearest cop %.1fm outside AV %.0fm -> TIMING (grace=%lldms, retry=%lldms, spawn=%dms)",
                 (unsigned long long)crime->case_id, dist_to_cop, av_range,
                 (long long)dispatch_timing::get_natural_response_grace_ms(*crime),
                 (long long)dispatch_timing::NEARBY_RETRY_INTERVAL_MS,
                 crime->dispatch_delay_ms);
        }
    }
}