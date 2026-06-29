#!/usr/bin/env python3
"""Split dispatch_cop_attack_pass_vehicle.cpp into focused helpers."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/main/cpp"
BACKUP = CPP / "dispatch_cop_attack_pass_vehicle.cpp.monolith.bak"

COMMON = '''\
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
#include "dispatch_cop_attack_vehicle_internal.hpp"
'''

ARM_EXIT_RANGE = (50, 137)
STUCK_RANGE = (147, 561)
UNSTUCK_RANGE = (563, 753)
ORDER_RANGE = (760, 793)

SESSION_REPLACEMENTS = [
    ("current_pos", "session.current_pos"),
    ("now_time", "session.now_time"),
    ("found_stuck", "session.found_stuck"),
    ("stuck_idx", "session.stuck_idx"),
    ("already_dispatched", "session.already_dispatched"),
    ("v_dist", "session.v_dist"),
    ("veh_pos", "session.veh_pos"),
    ("first_seen", "session.first_seen"),
    ("elapsed", "session.elapsed"),
]


def read_lines() -> list[str]:
    src = BACKUP if BACKUP.exists() else CPP / "dispatch_cop_attack_pass_vehicle.cpp"
    return src.read_text(encoding="utf-8").splitlines(keepends=True)


def extract(lines: list[str], start: int, end: int) -> str:
    return "".join(lines[start - 1 : end])


def sessionify(body: str, include_tracker: bool = True) -> str:
    if include_tracker:
        body = re.sub(r"\bStuckTracker tracker\b", "StuckTracker& tracker = session.tracker", body)
        body = re.sub(r"\btracker\.", "session.tracker.", body)
        body = re.sub(r"\btracker\b", "session.tracker", body)
        # fix double session.session.tracker
        body = body.replace("session.session.tracker", "session.tracker")
        body = body.replace("StuckTracker& tracker = session.tracker", "StuckTracker& tracker = session.tracker", 1)

    for old, new in SESSION_REPLACEMENTS:
        body = re.sub(rf"\b{re.escape(old)}\b", new, body)

    body = body.replace("session.session.", "session.")
    return body


def main() -> None:
    if (CPP / "dispatch_cop_attack_vehicle_stuck.cpp").exists():
        print("Refusing: vehicle split already done.", file=sys.stderr)
        sys.exit(1)

    src = CPP / "dispatch_cop_attack_pass_vehicle.cpp"
    src.rename(BACKUP)
    lines = read_lines()

    arm_exit = sessionify(extract(lines, *ARM_EXIT_RANGE), include_tracker=False)
    stuck_body = sessionify(extract(lines, *STUCK_RANGE))
    unstuck_body = sessionify(extract(lines, *UNSTUCK_RANGE))
    order_body = sessionify(extract(lines, *ORDER_RANGE), include_tracker=False)

    main_cpp = COMMON + """

void cop_attack_dispatch_vehicle_cop(
    CopAttackContext& ctx,
    CPed* ped,
    CPed* target_criminal,
    CVector target_crime_pos,
    void* veh) {
    if (!is_vehicle_pointer_valid(veh)) {
        return;
    }

    CopAttackVehicleSession session;
    session.veh_pos = get_entity_pos(veh);
    float v_dx = session.veh_pos.x - target_crime_pos.x;
    float v_dy = session.veh_pos.y - target_crime_pos.y;
    float v_dz = session.veh_pos.z - target_crime_pos.z;
    session.v_dist = sqrtf(v_dx * v_dx + v_dy * v_dy + v_dz * v_dz);

""" + arm_exit + """
        session.already_dispatched = ctx.vector_contains(ctx.vehicles_ordered_to_scene_snapshot, veh) ||
            ctx.vector_contains(ctx.vehicles_siren_awakened_snapshot, veh) ||
            veh == ctx.crime_case->spawned_vehicle;

        if (session.already_dispatched) {
            cop_attack_vehicle_stuck_monitor(ctx, session, ped, veh, target_criminal, target_crime_pos);
            cop_attack_vehicle_unstuck_intervene(ctx, session, ped, veh, target_criminal, target_crime_pos);
        }

        if (!session.already_dispatched && ctx.active_vehicles_count >= ctx.max_vehicles) {
            return;
        }
        cop_attack_vehicle_initial_order(ctx, session, ped, veh, target_criminal, target_crime_pos);
    }
}
"""

    # arm_exit ends with should_exit block - need to fix structure
    # Original arm_exit is 50-136 which includes should_exit if and its body, ending before } else {
    # Line 137 is } else {

    stuck_cpp = COMMON + """

void cop_attack_vehicle_stuck_monitor(
    CopAttackContext& ctx,
    CopAttackVehicleSession& session,
    CPed* ped,
    void* veh,
    CPed* target_criminal,
    CVector target_crime_pos) {
""" + stuck_body + """
}
"""

    unstuck_cpp = COMMON + """

void cop_attack_vehicle_unstuck_intervene(
    CopAttackContext& ctx,
    CopAttackVehicleSession& session,
    CPed* ped,
    void* veh,
    CPed* target_criminal,
    CVector target_crime_pos) {
""" + unstuck_body + """
}
"""

    order_cpp = COMMON + """

void cop_attack_vehicle_initial_order(
    CopAttackContext& ctx,
    CopAttackVehicleSession& session,
    CPed* ped,
    void* veh,
    CPed* target_criminal,
    CVector target_crime_pos) {
""" + order_body + """
}
"""

    (CPP / "dispatch_cop_attack_pass_vehicle.cpp").write_text(main_cpp, encoding="utf-8")
    (CPP / "dispatch_cop_attack_vehicle_stuck.cpp").write_text(stuck_cpp, encoding="utf-8")
    (CPP / "dispatch_cop_attack_vehicle_unstuck.cpp").write_text(unstuck_cpp, encoding="utf-8")
    (CPP / "dispatch_cop_attack_vehicle_order.cpp").write_text(order_cpp, encoding="utf-8")
    BACKUP.unlink()
    print("Split vehicle pass complete")


if __name__ == "__main__":
    main()