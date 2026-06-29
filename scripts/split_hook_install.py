#!/usr/bin/env python3
"""Extract hook installation thread from module.cpp into hook_install.cpp."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/main/cpp"
MONOLITH_BACKUP = CPP / "module.cpp.monolith.bak"

COMMON = '''\
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
#include <cinttypes>
#include <sys/mman.h>
#include <functional>

#include "shadowhook.h"
#include "third_party/xdl/xdl.h"

#include "log.hpp"
#include "game_config.hpp"
#include "game_types.hpp"
#include "pointer_sanitizer.hpp"
#include "mod_shared.hpp"
'''

# 1-based inclusive line range: pure-virtual patching + hook_thread_func
HOOK_INSTALL_RANGE = (627, 1234)


def read_lines() -> list[str]:
    if MONOLITH_BACKUP.exists():
        return MONOLITH_BACKUP.read_text(encoding="utf-8").splitlines(keepends=True)
    src = CPP / "module.cpp"
    if not src.exists():
        print("No module.cpp or backup found.", file=sys.stderr)
        sys.exit(1)
    return src.read_text(encoding="utf-8").splitlines(keepends=True)


def main() -> None:
    if (CPP / "hook_install.cpp").exists():
        print("Refusing to run: hook_install.cpp already exists.", file=sys.stderr)
        sys.exit(1)

    src = CPP / "module.cpp"
    src.rename(MONOLITH_BACKUP)

    lines = read_lines()
    start, end = HOOK_INSTALL_RANGE
    if end > len(lines):
        print(f"Range exceeds file length ({len(lines)})", file=sys.stderr)
        sys.exit(1)

    hook_chunk = lines[start - 1 : end]
    module_chunk = lines[: start - 1] + lines[end:]

    (CPP / "hook_install.cpp").write_text(
        COMMON + "\n" + "".join(hook_chunk), encoding="utf-8"
    )
    (CPP / "module.cpp").write_text("".join(module_chunk), encoding="utf-8")

    MONOLITH_BACKUP.unlink()
    print(f"  hook_install.cpp: {len(hook_chunk)} lines")
    print(f"  module.cpp: {len(module_chunk)} lines")
    print("Split complete")


if __name__ == "__main__":
    main()