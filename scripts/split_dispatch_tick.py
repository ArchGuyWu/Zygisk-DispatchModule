#!/usr/bin/env python3
"""Split dispatch_tick.cpp into focused translation units."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/main/cpp"
MONOLITH_BACKUP = CPP / "dispatch_tick.cpp.monolith.bak"

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

# 1-based inclusive line ranges from dispatch_tick.cpp monolith.
SPLIT = {
    "dispatch_tick.cpp": [
        (36, 97),    # cleanup + dispatch_spawn_emergency_car
        (575, 1478), # civilian avoidance + on_main_thread_tick + proxy_the_scripts_process
    ],
    "dispatch_emergency.cpp": [(98, 573)],
    "dispatch_spawn_hooks.cpp": [(1480, 1692)],
}

DEMOTE_STATIC = {
    "dispatch_emergency.cpp": [r"^static void emergency_vehicles_tick\b"],
}


def read_lines() -> list[str]:
    if MONOLITH_BACKUP.exists():
        return MONOLITH_BACKUP.read_text(encoding="utf-8").splitlines(keepends=True)
    src = CPP / "dispatch_tick.cpp"
    if not src.exists():
        print("No dispatch_tick.cpp or backup found.", file=sys.stderr)
        sys.exit(1)
    return src.read_text(encoding="utf-8").splitlines(keepends=True)


def extract_ranges(lines: list[str], ranges: list[tuple[int, int]]) -> list[str]:
    out: list[str] = []
    for start, end in ranges:
        out.extend(lines[start - 1 : end])
    return out


def demote_static(fname: str, chunk: list[str]) -> list[str]:
    patterns = DEMOTE_STATIC.get(fname, [])
    if not patterns:
        return chunk
    out: list[str] = []
    for line in chunk:
        replaced = line
        for pat in patterns:
            replaced = re.sub(pat, lambda m: m.group(0).replace("static ", "", 1), replaced)
        out.append(replaced)
    return out


def main() -> None:
    if (CPP / "dispatch_emergency.cpp").exists():
        print("Refusing to run: dispatch_tick already split.", file=sys.stderr)
        sys.exit(1)

    src = CPP / "dispatch_tick.cpp"
    src.rename(MONOLITH_BACKUP)

    lines = read_lines()
    total = len(lines)

    for fname, ranges in SPLIT.items():
        for _, end in ranges:
            if end > total:
                print(f"Range for {fname} exceeds file length ({total})", file=sys.stderr)
                sys.exit(1)
        chunk = extract_ranges(lines, ranges)
        chunk = demote_static(fname, chunk)
        (CPP / fname).write_text(COMMON + "\n\n" + "".join(chunk), encoding="utf-8")
        print(f"  {fname}: {len(chunk)} lines")

    MONOLITH_BACKUP.unlink()
    print("Split complete")


if __name__ == "__main__":
    main()