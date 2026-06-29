#!/usr/bin/env python3
"""Split dispatch_cop_attack.cpp by CopAttackContext helper functions."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/main/cpp"
BACKUP = CPP / "dispatch_cop_attack.cpp.monolith.bak"

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
'''

CTX_REPLACEMENTS = [
    ("cop_attack_assign_time_snapshot", "ctx.cop_attack_assign_time_snapshot"),
    ("armed_cops_time_snapshot", "ctx.armed_cops_time_snapshot"),
    ("cop_assigned_weapon_snapshot", "ctx.cop_assigned_weapon_snapshot"),
    ("dispatched_vehicles_time_snapshot", "ctx.dispatched_vehicles_time_snapshot"),
    ("stuck_vehicles_snapshot", "ctx.stuck_vehicles_snapshot"),
    ("vehicles_emptied_snapshot", "ctx.vehicles_emptied_snapshot"),
    ("vehicles_ordered_to_scene_snapshot", "ctx.vehicles_ordered_to_scene_snapshot"),
    ("vehicles_siren_awakened_snapshot", "ctx.vehicles_siren_awakened_snapshot"),
    ("pending_armed_cops_time", "ctx.pending_armed_cops_time"),
    ("pending_cop_assigned_weapon", "ctx.pending_cop_assigned_weapon"),
    ("pending_dispatched_vehicles_time", "ctx.pending_dispatched_vehicles_time"),
    ("pending_vehicles_emptied", "ctx.pending_vehicles_emptied"),
    ("pending_vehicles_ordered_to_scene", "ctx.pending_vehicles_ordered_to_scene"),
    ("pending_vehicles_siren_awakened", "ctx.pending_vehicles_siren_awakened"),
    ("pending_stuck_vehicles", "ctx.pending_stuck_vehicles"),
    ("pending_cop_attack_assign_time", "ctx.pending_cop_attack_assign_time"),
    ("pending_temp_closures", "ctx.pending_temp_closures"),
    ("counted_vehicles", "ctx.counted_vehicles"),
    ("is_active_firearm", "ctx.is_active_firearm"),
    ("active_vehicles_count", "ctx.active_vehicles_count"),
    ("active_foot_cops_count", "ctx.active_foot_cops_count"),
    ("max_vehicles", "ctx.max_vehicles"),
    ("max_foot_cops", "ctx.max_foot_cops"),
    ("crime_case", "ctx.crime_case"),
    ("crime_pos", "ctx.crime_pos"),
    ("criminal", "ctx.criminal"),
    ("byte_map", "ctx.byte_map"),
]

RESET_BODY_RANGE = (83, 127)  # reserve-once + clear (vectors are struct members)
SNAPSHOT_BODY_RANGE = (136, 168)
PASS1_RANGE = (190, 279)
QUOTA_RANGE = (284, 336)
PASS2_RANGE = (341, 1453)
COMMIT_RANGE = (1458, 1511)


def read_lines() -> list[str]:
    src = BACKUP if BACKUP.exists() else CPP / "dispatch_cop_attack.cpp"
    return src.read_text(encoding="utf-8").splitlines(keepends=True)


def extract(lines: list[str], start: int, end: int) -> str:
    return "".join(lines[start - 1 : end])


def ctxify(body: str) -> str:
    body = re.sub(r"\bint active_vehicles_count = 0\b", "ctx.active_vehicles_count = 0", body)
    body = re.sub(r"\bint active_foot_cops_count = 0\b", "ctx.active_foot_cops_count = 0", body)
    body = re.sub(r"\bint max_vehicles = 2\b", "ctx.max_vehicles = 2", body)
    body = re.sub(r"\bint max_foot_cops = 2\b", "ctx.max_foot_cops = 2", body)
    body = re.sub(r"\bbool is_active_firearm = false\b", "ctx.is_active_firearm = false", body)

    # size is special: only pool iteration bound
    body = re.sub(r"\bi < size\b", "i < ctx.pool_size", body)
    body = re.sub(r"\bfor \(int i = 0; i < size;", "for (int i = 0; i < ctx.pool_size;", body)
    body = re.sub(r"\bsigned char flag = byte_map\[i\]", "signed char flag = ctx.byte_map[i]", body)

    for old, new in CTX_REPLACEMENTS:
        body = re.sub(rf"\b{re.escape(old)}\b", new, body)

    body = re.sub(
        r"\bvector_contains\(",
        "ctx.vector_contains(",
        body,
    )
    return body


def main() -> None:
    if (CPP / "dispatch_cop_attack_pass.cpp").exists():
        print("Refusing: cop_attack already split.", file=sys.stderr)
        sys.exit(1)

    src = CPP / "dispatch_cop_attack.cpp"
    src.rename(BACKUP)
    lines = read_lines()

    reset_body = extract(lines, *RESET_BODY_RANGE)
    snapshot_body = ctxify(extract(lines, *SNAPSHOT_BODY_RANGE))
    pass1_body = ctxify(extract(lines, *PASS1_RANGE))
    quota_body = ctxify(extract(lines, *QUOTA_RANGE))
    pass2_body = ctxify(extract(lines, *PASS2_RANGE))
    commit_body = ctxify(extract(lines, *COMMIT_RANGE))

    state_cpp = COMMON + """

void CopAttackContext::reset() {
    thread_local static bool initialized = false;
""" + reset_body + """
}

bool CopAttackContext::vector_contains(const std::vector<void*>& vec, void* val) const {
    for (void* item : vec) {
        if (item == val) return true;
    }
    return false;
}

void cop_attack_snapshot_globals(CopAttackContext& ctx) {
""" + snapshot_body + """
}

void cop_attack_detect_firearm_threat(CopAttackContext& ctx) {
""" + ctxify(extract(lines, 171, 185)) + """
}

void cop_attack_pass1_count_active(CopAttackContext& ctx) {
""" + pass1_body + """
}

void cop_attack_compute_quotas(CopAttackContext& ctx) {
""" + quota_body + """
}

void cop_attack_commit_pending(CopAttackContext& ctx) {
""" + commit_body + """
}
"""

    pass_cpp = COMMON + """

void cop_attack_pass2_dispatch(CopAttackContext& ctx) {
""" + pass2_body + """
}
"""

    orchestrator = COMMON + """

void make_cops_attack_criminal(CPed* criminal) {
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

    cop_attack_snapshot_globals(ctx);
    cop_attack_detect_firearm_threat(ctx);
    cop_attack_pass1_count_active(ctx);
    cop_attack_compute_quotas(ctx);
    cop_attack_pass2_dispatch(ctx);
    cop_attack_commit_pending(ctx);
}
"""

    (CPP / "dispatch_cop_attack_state.cpp").write_text(state_cpp, encoding="utf-8")
    (CPP / "dispatch_cop_attack_pass.cpp").write_text(pass_cpp, encoding="utf-8")
    (CPP / "dispatch_cop_attack.cpp").write_text(orchestrator, encoding="utf-8")
    BACKUP.unlink()
    print("Split cop_attack complete")


if __name__ == "__main__":
    main()