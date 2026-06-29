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
#include "dispatch_threat.hpp"


void cop_attack_pass2_dispatch(CopAttackContext& ctx) {
    for (int i = 0; i < ctx.pool_size; i++) {
        signed char flag = ctx.byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            CPed* ped = g_GetPoolPed(handle);
            
            // 判定该 NPC 是否属于并案列表中的罪犯之一
            bool is_cop_not_criminal = true;
            if (ctx.crime_case && !ctx.crime_case->cancelled) {
                for (CPed* c : ctx.crime_case->consolidated_criminals) {
                    if (ped == c) {
                        is_cop_not_criminal = false;
                        break;
                    }
                }
            } else {
                if (ped == ctx.criminal) is_cop_not_criminal = false;
            }

            if (ped && is_ped_pointer_valid_safe(ped) && is_cop_not_criminal) {
                if (g_IsAlive && !g_IsAlive(ped)) {
                    continue;
                }
                int ped_type = g_GetPedType(ped);
                if (ped_type == PED_TYPE_COP) {
                    CVector cop_pos = get_entity_pos(ped);
                    CVector target_crime_pos = ctx.crime_pos;
                    CPed* target_criminal = ctx.criminal;

                    // 并案下按威胁优先、距离次之选择攻击对象
                    if (ctx.crime_case && !ctx.crime_case->cancelled) {
                        CPed* picked = dispatch_threat::pick_criminal_target_for_cop(ped, *ctx.crime_case);
                        if (picked) {
                            target_criminal = picked;
                            target_crime_pos = get_entity_pos(picked);
                        }
                    }

                    float dx = cop_pos.x - target_crime_pos.x;
                    float dy = cop_pos.y - target_crime_pos.y;
                    float dz = cop_pos.z - target_crime_pos.z;
                    float dist_sq = dx * dx + dy * dy + dz * dz;

                    // 1. 如果警车驾驶中的警员距离还很远，先给他发合适武器（防手无寸铁），并开启警车自主避障驾驶

                    void* veh = find_vehicle_of_cop(ped);
                    if (veh) {
                        if (is_vehicle_pointer_valid(veh)) {
                            cop_attack_dispatch_vehicle_cop(ctx, ped, target_criminal, target_crime_pos, veh);
                        }
                        continue;
                    }
                    cop_attack_dispatch_foot_cop(ctx, ped, target_criminal, target_crime_pos, dist_sq);
                }
            }
        }
    }
}
