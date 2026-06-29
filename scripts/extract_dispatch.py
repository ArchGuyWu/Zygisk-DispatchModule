#!/usr/bin/env python3
"""One-shot extractor used during the 2026-06 dispatch_logic.cpp split.

dispatch_logic.cpp already exists in the tree. Re-running without --force will
refuse to overwrite it.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/main/cpp"
MODULE = CPP / "module.cpp"
DISPATCH = CPP / "dispatch_logic.cpp"

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

PROXY_NAMES = [
    "proxy_report_crime",
    "proxy_register_kill",
    "proxy_set_wanted_level",
    "proxy_generate_damage_event",
    "proxy_event_damage_ctor_c1",
    "proxy_event_damage_ctor_c2",
    "proxy_SetCurrentWeapon",
    "proxy_the_scripts_process",
    "proxy_wanted_update",
    "proxy_add_ped",
    "proxy_generate_one_emergency_car",
    "proxy_script_generate_one_emergency_car",
    "proxy_tell_occupants_leave_car",
]

# Historical line range from the pre-split monolith.
START, END = 670, 5379


def strip_static_proxy(line: str) -> str:
    for name in PROXY_NAMES:
        if re.match(rf"^static .*\b{name}\b", line):
            return line[7:]
    return line


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite dispatch_logic.cpp from module.cpp line ranges",
    )
    args = parser.parse_args()

    if DISPATCH.exists() and not args.force:
        print(
            "Refusing to run: dispatch_logic.cpp already exists. "
            "Use --force only with a monolithic module.cpp backup.",
            file=sys.stderr,
        )
        sys.exit(1)

    lines = MODULE.read_text(encoding="utf-8").splitlines(keepends=True)
    if len(lines) < END:
        print(
            f"Refusing to run: module.cpp has {len(lines)} lines; "
            f"expected at least {END} for historical range.",
            file=sys.stderr,
        )
        sys.exit(1)

    chunk = lines[START - 1 : END]

    out: list[str] = []
    for line in chunk:
        if line.strip() == "void init_ecs_systems();":
            continue
        out.append(strip_static_proxy(line))

    DISPATCH.write_text(COMMON + "\n" + "".join(out), encoding="utf-8")

    remaining = lines[: START - 1] + lines[END:]
    MODULE.write_text("".join(remaining), encoding="utf-8")
    print(f"dispatch_logic.cpp: {len(out)} lines")
    print(f"module.cpp: {len(remaining)} lines")


if __name__ == "__main__":
    main()