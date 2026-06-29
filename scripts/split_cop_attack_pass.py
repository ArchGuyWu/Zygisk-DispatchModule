#!/usr/bin/env python3
"""Split cop_attack_pass2 into vehicle and foot cop dispatch units."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/main/cpp"
BACKUP = CPP / "dispatch_cop_attack_pass.cpp.monolith.bak"

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

LOOP_PREFIX_RANGE = (38, 91)   # for-loop through dist_sq (before veh branch)
VEHICLE_BODY_RANGE = (94, 846)
FOOT_BODY_RANGE = (850, 1147)


def read_lines() -> list[str]:
    src = BACKUP if BACKUP.exists() else CPP / "dispatch_cop_attack_pass.cpp"
    return src.read_text(encoding="utf-8").splitlines(keepends=True)


def extract(lines: list[str], start: int, end: int) -> str:
    body = "".join(lines[start - 1 : end])
    return body.replace("by ctx.criminal %p", "by criminal %p")


def main() -> None:
    if (CPP / "dispatch_cop_attack_pass_vehicle.cpp").exists():
        print("Refusing: cop_attack_pass already split.", file=sys.stderr)
        sys.exit(1)

    src = CPP / "dispatch_cop_attack_pass.cpp"
    src.rename(BACKUP)
    lines = read_lines()

    loop_prefix = extract(lines, *LOOP_PREFIX_RANGE)
    vehicle_body = extract(lines, *VEHICLE_BODY_RANGE)
    foot_body = extract(lines, *FOOT_BODY_RANGE)

    vehicle_cpp = COMMON + """

void cop_attack_dispatch_vehicle_cop(
    CopAttackContext& ctx,
    CPed* ped,
    CPed* target_criminal,
    const CVector& target_crime_pos,
    void* veh) {
""" + vehicle_body + """
}
"""

    foot_cpp = COMMON + """

void cop_attack_dispatch_foot_cop(
    CopAttackContext& ctx,
    CPed* ped,
    CPed* target_criminal,
    const CVector& target_crime_pos,
    float dist_sq) {
""" + foot_body + """
}
"""

    orchestrator = COMMON + """

void cop_attack_pass2_dispatch(CopAttackContext& ctx) {
""" + loop_prefix + """
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
"""

    (CPP / "dispatch_cop_attack_pass_vehicle.cpp").write_text(vehicle_cpp, encoding="utf-8")
    (CPP / "dispatch_cop_attack_pass_foot.cpp").write_text(foot_cpp, encoding="utf-8")
    (CPP / "dispatch_cop_attack_pass.cpp").write_text(orchestrator, encoding="utf-8")
    BACKUP.unlink()
    print("Split cop_attack_pass complete")


if __name__ == "__main__":
    main()