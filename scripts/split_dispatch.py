#!/usr/bin/env python3
"""Split dispatch_logic.cpp into focused translation units."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/main/cpp"
MONOLITH_BACKUP = CPP / "dispatch_logic.cpp.monolith.bak"

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

# 1-based inclusive line ranges from the pre-split monolith.
SPLIT = {
    "dispatch_logic.cpp": (1, 406),
    "dispatch_hooks.cpp": (407, 911),
    "dispatch_cop_response.cpp": (912, 3097),
    "dispatch_tick.cpp": (3098, None),
}

DEMOTE_STATIC = {
    "dispatch_cop_response.cpp": [r"^static void make_cops_attack_criminal\b"],
    "dispatch_tick.cpp": [r"^static void cleanup_single_case_vehicles\b"],
}


def read_lines() -> list[str]:
    if MONOLITH_BACKUP.exists():
        return MONOLITH_BACKUP.read_text(encoding="utf-8").splitlines(keepends=True)
    src = CPP / "dispatch_logic.cpp"
    if not src.exists():
        print("No dispatch_logic.cpp or backup found.", file=sys.stderr)
        sys.exit(1)
    return src.read_text(encoding="utf-8").splitlines(keepends=True)


def extract_range(lines: list[str], start: int, end: int | None) -> list[str]:
    return lines[start - 1 : (end if end is None else end)]


def scrub_dispatch_logic(chunk: list[str]) -> list[str]:
    out: list[str] = []
    skip_forward = {
        "static void cleanup_single_case_vehicles",
        "void make_cops_attack_criminal_immediate",
        "bool is_specific_criminal_armed_with_firearm",
        "void make_single_cop_attack_criminal",
        "void update_cops_targeting_criminal_event_driven",
        "void update_primary_criminal_by_threat",
        "static void make_cops_attack_criminal",
    }
    for line in chunk:
        stripped = line.strip()
        if any(stripped.startswith(prefix) for prefix in skip_forward):
            continue
        out.append(line)
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
    if (CPP / "dispatch_hooks.cpp").exists() and not (CPP / "dispatch_logic.cpp").exists():
        print(
            "Partial split detected (hooks exist, logic missing). Re-run after restoring backup.",
            file=sys.stderr,
        )
        sys.exit(1)

    if (CPP / "dispatch_hooks.cpp").exists():
        print("Refusing to run: dispatch already split.", file=sys.stderr)
        sys.exit(1)

    src = CPP / "dispatch_logic.cpp"
    if src.exists():
        src.rename(MONOLITH_BACKUP)

    lines = read_lines()
    total = len(lines)

    for fname, (start, end) in SPLIT.items():
        if end is not None and end > total:
            print(f"Range for {fname} exceeds file length ({total})", file=sys.stderr)
            sys.exit(1)
        chunk = extract_range(lines, start, end)
        if fname == "dispatch_logic.cpp":
            chunk = scrub_dispatch_logic(chunk)
        chunk = demote_static(fname, chunk)
        (CPP / fname).write_text(COMMON + "\n" + "".join(chunk), encoding="utf-8")
        print(f"  {fname}: {len(chunk)} lines")

    if MONOLITH_BACKUP.exists():
        MONOLITH_BACKUP.unlink()
    print("Split complete")


if __name__ == "__main__":
    main()