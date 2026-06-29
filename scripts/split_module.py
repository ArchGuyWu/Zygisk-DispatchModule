#!/usr/bin/env python3
"""One-shot splitter used during the 2026-06 module.cpp refactor.

The tree is already split:
  module.cpp          - globals, helpers, hook_thread_func(), Zygisk entry
  dispatch_logic.cpp  - crime/dispatch proxies and tick logic
  hooks_stability.cpp - stability hook proxies
  ecs_systems.cpp     - init_ecs_systems()

Re-running this script will corrupt the repo. Pass --force only if you have
restored a monolithic module.cpp backup and intentionally want to re-split.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP_DIR = ROOT / "src/main/cpp"
MODULE = CPP_DIR / "module.cpp"

# Historical 1-based line ranges from the pre-split monolith (module.cpp.bak era).
RANGES = {
    "hooks_stability.cpp": [(5241, 5788), (6082, 6338)],
    "ecs_systems.cpp": [(6940, 7435)],
}

COMMON_INCLUDES = '''\
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

#include "shadowhook.h"
#include "third_party/xdl/xdl.h"

#include "log.hpp"
#include "game_config.hpp"
#include "game_types.hpp"
#include "pointer_sanitizer.hpp"
#include "mod_shared.hpp"
#include "ecs_engine.hpp"
'''


def line_set(ranges: list[tuple[int, int]]) -> set[int]:
    s: set[int] = set()
    for a, b in ranges:
        s.update(range(a, b + 1))
    return s


def strip_static(line: str) -> str:
    if line.startswith("static inline "):
        return line[14:]
    if line.startswith("static "):
        return line[7:]
    return line


def already_split() -> bool:
    if not MODULE.exists():
        return False
    if (CPP_DIR / "dispatch_logic.cpp").exists():
        return True
    return len(MODULE.read_text(encoding="utf-8").splitlines()) < 3000


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--force",
        action="store_true",
        help="Run the historical splitter even though the tree is already split",
    )
    args = parser.parse_args()

    if already_split() and not args.force:
        print(
            "Refusing to run: module.cpp is already split. "
            "See docs/MODULE_LAYOUT.md. Use --force only with a monolithic backup.",
            file=sys.stderr,
        )
        sys.exit(1)

    lines = MODULE.read_text(encoding="utf-8").splitlines(keepends=True)
    extract_lines = line_set(sum(RANGES.values(), []))

    extracted: dict[str, list[str]] = {name: [] for name in RANGES}
    for name, ranges in RANGES.items():
        for a, b in ranges:
            extracted[name].extend(lines[a - 1 : b])

    remaining: list[str] = []
    for i, line in enumerate(lines, start=1):
        if i not in extract_lines:
            remaining.append(line)

    text = "".join(remaining)

    def demote_static_globals(content: str) -> str:
        out = []
        for line in content.splitlines(keepends=True):
            if re.match(r"^static (fn_|void\*|void\*\*|std::|FMalloc|constexpr)", line):
                out.append(strip_static(line))
            else:
                out.append(line)
        return "".join(out)

    text = demote_static_globals(text)

    for fname, chunks in extracted.items():
        fixed = []
        for line in chunks:
            if re.match(r"^static ", line):
                fixed.append(strip_static(line))
            else:
                fixed.append(line)
        content = COMMON_INCLUDES + "\n" + "".join(fixed)
        (CPP_DIR / fname).write_text(content, encoding="utf-8")

    MODULE.write_text(text, encoding="utf-8")
    print("Split complete (historical ranges):")
    for fname in RANGES:
        print(f"  {fname}: {(CPP_DIR / fname).stat().st_size} bytes")
    print(f"  module.cpp: {MODULE.stat().st_size} bytes")
    print("Note: hook installation remains in module.cpp::hook_thread_func().")


if __name__ == "__main__":
    main()