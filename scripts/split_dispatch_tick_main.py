#!/usr/bin/env python3
"""Extract on_main_thread_tick block from dispatch_tick.cpp."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/main/cpp"
BACKUP = CPP / "dispatch_tick.cpp.monolith.bak"

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
'''

MAIN_RANGES = [(41, 41), (85, 992)]  # g_last_tick_time_ms + spawn/civilian/main tick
TICK_RANGES = [(36, 40), (43, 84), (993, None)]


def read_lines() -> list[str]:
    src = BACKUP if BACKUP.exists() else CPP / "dispatch_tick.cpp"
    return src.read_text(encoding="utf-8").splitlines(keepends=True)


def extract(lines: list[str], ranges: list[tuple[int, int | None]]) -> list[str]:
    out: list[str] = []
    for start, end in ranges:
        out.extend(lines[start - 1 : (end if end is not None else len(lines))])
    return out


def demote_static(chunk: list[str]) -> list[str]:
    out: list[str] = []
    for line in chunk:
        if line.startswith("static void on_main_thread_tick"):
            out.append(line.replace("static void on_main_thread_tick", "void on_main_thread_tick", 1))
        else:
            out.append(line)
    return out


def main() -> None:
    if (CPP / "dispatch_tick_main.cpp").exists():
        print("Refusing: dispatch_tick_main.cpp already exists.", file=sys.stderr)
        sys.exit(1)

    src = CPP / "dispatch_tick.cpp"
    src.rename(BACKUP)
    lines = read_lines()

    main_chunk = demote_static(extract(lines, MAIN_RANGES))
    tick_chunk = extract(lines, TICK_RANGES)

    (CPP / "dispatch_tick_main.cpp").write_text(
        COMMON
        + "\n\n// =====================================================================\n"
        + "// Main dispatch tick (state machine + spawn orchestration)\n"
        + "// =====================================================================\n"
        + "".join(main_chunk),
        encoding="utf-8",
    )
    (CPP / "dispatch_tick.cpp").write_text(
        COMMON
        + "\n\n"
        + "// =====================================================================\n"
        + "// Dispatch tick entry + case cleanup\n"
        + "// =====================================================================\n"
        + "".join(tick_chunk),
        encoding="utf-8",
    )
    BACKUP.unlink()
    print(f"  dispatch_tick_main.cpp: {len(main_chunk)} lines")
    print(f"  dispatch_tick.cpp: {len(tick_chunk)} lines")


if __name__ == "__main__":
    main()