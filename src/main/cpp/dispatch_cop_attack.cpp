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
#include "dispatch_cop_attack_internal.hpp"
#include "dispatch_timing.hpp"


void make_cops_attack_criminal(CPed* criminal) {
    if (is_mod_dispatch_paused()) return;
    if (!criminal || !is_ped_pointer_valid_safe(criminal)) return;
    if (!g_ms_pPedPool || !g_GetPoolPed || !g_GetPedType) return;

    std::shared_ptr<CrimeEvent> crime_case = find_crime_containing_criminal(criminal);
    if (any_active_firearm_case_blocking(criminal)) return;

    void* pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!pool) return;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return;

    thread_local CopAttackContext ctx;
    ctx.reset();
    ctx.criminal = criminal;
    ctx.crime_case = crime_case;
    ctx.crime_pos = get_entity_pos(criminal);
    ctx.pool_size = size;
    ctx.byte_map = byte_map;

    if (crime_case && !crime_case->cancelled) {
        float av_range = dispatch_timing::get_av_range_for_crime(*crime_case);
        ctx.av_range_sq = av_range * av_range;
    }

    cop_attack_snapshot_globals(ctx);
    cop_attack_detect_firearm_threat(ctx);
    cop_attack_single_pass_dispatch(ctx);
    cop_attack_commit_pending(ctx);
}
