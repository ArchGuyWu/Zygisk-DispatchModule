#!/usr/bin/env python3
"""Split dispatch_tick_main into civilian, states, and orchestrator units."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/main/cpp"
BACKUP = CPP / "dispatch_tick_main.cpp.monolith.bak"

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
#include "dispatch_tick_internal.hpp"
'''

CIVILIAN_RANGE = (53, 228)
PROCESS_CRIME_RANGE = (423, 874)


def read_lines() -> list[str]:
    src = BACKUP if BACKUP.exists() else CPP / "dispatch_tick_main.cpp"
    return src.read_text(encoding="utf-8").splitlines(keepends=True)


def extract(lines: list[str], start: int, end: int | None) -> str:
    return "".join(lines[start - 1 : (end if end is not None else len(lines))])


def main() -> None:
    if (CPP / "dispatch_tick_states.cpp").exists():
        print("Refusing: dispatch_tick_states already exists.", file=sys.stderr)
        sys.exit(1)

    if not BACKUP.exists():
        src = CPP / "dispatch_tick_main.cpp"
        if not src.exists():
            print("No dispatch_tick_main.cpp found.", file=sys.stderr)
            sys.exit(1)
        src.rename(BACKUP)

    lines = read_lines()

    civilian_body = extract(lines, *CIVILIAN_RANGE).replace(
        "static void apply_civilian_avoidance_field",
        "void apply_civilian_avoidance_field",
        1,
    )
    process_body = extract(lines, *PROCESS_CRIME_RANGE)

    spawn_block = extract(lines, 39, 51).replace(
        "static void dispatch_spawn_emergency_car",
        "void dispatch_spawn_emergency_car",
        1,
    )
    tick_preamble = extract(lines, 230, 417)
    tick_epilogue = extract(lines, 877, None)

    internal_hpp = '''#pragma once

#include <cstdint>
#include <memory>

#include "game_types.hpp"
#include "dispatch_types.hpp"

void dispatch_spawn_emergency_car(unsigned int model, CVector pos);
void apply_civilian_avoidance_field();
void dispatch_tick_process_crime(const std::shared_ptr<CrimeEvent>& crime, int64_t cur_time, bool do_siren_refresh);
'''

    civilian_cpp = COMMON + "\n\n" + civilian_body

    states_cpp = COMMON + """

void dispatch_tick_process_crime(
    const std::shared_ptr<CrimeEvent>& crime,
    int64_t cur_time,
    bool do_siren_refresh) {
""" + process_body + """
}
"""

    main_cpp = COMMON.replace("dispatch_tick_internal.hpp", "dispatch_tick_internal.hpp") + """

// =====================================================================
// Main dispatch tick orchestrator
// =====================================================================
static int64_t g_last_tick_time_ms = 0;

""" + spawn_block + tick_preamble + """
    for (auto& crime : crimes_snapshot) {
        if (!crime || crime->cancelled) {
            continue;
        }
        dispatch_tick_process_crime(crime, cur_time, do_siren_refresh);
    }

""" + tick_epilogue

    (CPP / "include/dispatch_tick_internal.hpp").write_text(internal_hpp, encoding="utf-8")
    (CPP / "dispatch_tick_civilian.cpp").write_text(civilian_cpp, encoding="utf-8")
    (CPP / "dispatch_tick_states.cpp").write_text(states_cpp, encoding="utf-8")
    (CPP / "dispatch_tick_main.cpp").write_text(main_cpp, encoding="utf-8")
    BACKUP.unlink()
    print("Split dispatch_tick_states complete")


if __name__ == "__main__":
    main()