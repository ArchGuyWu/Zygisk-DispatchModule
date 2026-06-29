#!/usr/bin/env python3
"""Replace inflated Gemini-style comments with factual wording."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/main/cpp"

FILES = [
    CPP / "module.cpp",
    CPP / "dispatch_logic.cpp",
    CPP / "hooks_stability.cpp",
    CPP / "ecs_systems.cpp",
    CPP / "ecs_engine.hpp",
    CPP / "include" / "mod_shared.hpp",
]

SECTION_MAP = {
    "📡 [并案机制 (Case Consolidation)]：合并同一区域的多人犯罪事件": "Case consolidation: merge crimes in the same area",
    "📡 [trueDispatch Spawn Helper]：自定义调度车辆的生成包装器，保障正确的防拦截标识": "Custom dispatch vehicle spawn wrapper",
    "🚑🚒 [Emergency Vehicle Escaper]：救护车与消防车的高级物理脱困、导航与避障机制": "Emergency vehicle unstuck / navigation helpers",
    "🚗💨 [Civilian Avoidance Field]：给平民车辆施加“恐惧场”，让其智能避让紧急调度车辆": "Civilian vehicle avoidance near emergency units",
    "👮‍♂️ [Wanted-System Spawning Protection]": "Wanted-system spawn interception",
    "🚑🚒 [Emergency Workaround]：移动端救护车与消防车因超长视距生成即秒删 Bug 的修复": "Emergency vehicle spawn distance workaround (mobile draw distance)",
    "📡 [临时寻路规避区]：用于记录中途意外（如警车卡死塞车等）临时关闭的路网，以供后续按时开启": "Temporary road closure records",
}

INLINE_REPLACEMENTS = [
    (r"彻底剥夺免责状态", "取消玩家协助警方的免责状态"),
    (r"彻底排除该点，确保绝对不在玩家视野中凭空出现", "降低该点在玩家视野内刷出的概率"),
    (r"100% 解决崩溃", "避免该路径上的崩溃"),
    (r"100% 内存池复用和零分配", "thread_local 向量复用，减少堆分配"),
    (r"100% 安全稳定", "不依赖结构体偏移"),
    (r"或彻底超时", "或超时过久"),
    (r"终极打破物理死锁", "尝试传送到更近位置以解除卡住"),
    (r"且完美解决 lambda 回调内 push_back 导致的 vector 迭代器失效 crash 风险", "并在回调中避免迭代器失效"),
    (r"确保 100% mission compatibility", "避免影响任务脚本生成的警车"),
    (r"彻底杜绝堆损坏", "使用引擎原生分配器"),
    (r"彻底移除直接向应用根目录写入的尝试，规避权限和越权审计警报", "仅写入应用 code_cache 目录"),
    (r"做到完全无痕 \(Stealth & Clean\)", "写入后删除伴生库文件"),
    (r"杜绝野指针崩溃", "降低野指针风险"),
    (r"极其鲁棒的", ""),
    (r"强力扣除 500 分", "扣除 500 分"),
    (r"【彻底修复说明】", "说明："),
    (r"【后续彻底重构要求】", "后续若重新启用："),
    (r"100% 零成本对接与无缝兼容", "直接持有引擎对象指针"),
]

LOG_EMOJI_PREFIX = re.compile(
    r'LOG[IWED]\("(?:📡|🚫|🚓|🚑|🎯|👮|⚡️|✅|❌|ℹ️|⚠️|🚒|🚗)'
)


def clean_section_headers(text: str) -> str:
    for old, new in SECTION_MAP.items():
        text = text.replace(f"// {old}", f"// {new}")
    # Generic: strip leading emoji from // lines
    text = re.sub(
        r"^//\s*[\U0001F300-\U0001FAFF\u2600-\u27BF]+\s*\[([^\]]+)\][^\n]*",
        r"// \1",
        text,
        flags=re.M,
    )
    return text


def clean_inline(text: str) -> str:
    for pat, repl in INLINE_REPLACEMENTS:
        text = re.sub(pat, repl, text)
    return text


def clean_file_header_module(text: str) -> str:
    old = """/**
 * Zygisk + ShadowHook: GTA SA DE 警方干预系统 Mod
 *
 * 完整功能列表：
 *   1. 检测 NPC 犯罪行为，区分冷兵器/枪械
 *   2. 非枪械：初始仅 1 单位响应；枪械或有人牺牲：附近全体响应
 *   3. 附近无警力时，冷兵器 30 秒 / 枪械 15 秒 后调度接警车
 *   4. 若犯罪 NPC 被提前消灭或自然刷出警车则取消调度
 *   5. 接警车鸣笛抵达犯罪坐标附近，副驾驶下车停留、驾驶员留在车内
 *   6. 10 秒无事后副驾驶上车离开
 *   7. 玩家协助警方击毙犯罪 NPC 且仅瞄准该 NPC 射击时免遭通缉
 *   8. 每有警察牺牲，分别延迟 10/8/6 秒增派一辆警车，最多 3 辆
 *
 * 技术栈：Zygisk API v4 + ShadowHook (UNIQUE mode)
 *
 * 所有使用的符号均通过 nm -D libUE4.so 验证为导出符号。
 */"""
    new = """/**
 * GTA SA DE 警力派发 Zygisk 模块
 *
 * 入口：module.cpp（全局符号、辅助函数、Hook 安装、Zygisk）
 * 派发：dispatch_logic.cpp | 稳定性 Hook：hooks_stability.cpp | ECS：ecs_systems.cpp
 *
 * 符号经 nm -D libUE4.so 核对；运行时通过 xdl_sym 解析。
 */"""
    return text.replace(old, new)


def clean_ecs_engine(text: str) -> str:
    return text.replace(
        "// 这保证了与游戏引擎原本对象树的 100% 零成本对接与无缝兼容。",
        "// 直接保存引擎对象指针，无额外序列化层。",
    )


def clean_sanitize_block(text: str) -> str:
    old = """    // 说明：
    // 禁用盲目扫描内存的“净化器”。
    // 盲目扫描（按8字节步长解引用）会把非指针数据（如 float 数组、CVector 坐标、计时器等）误判为“已被析构清空的 C++ 对象”并置为 nullptr。
    // 例如：BoneNode_c::Limit(float) 在限制骨骼角度时会调用 BoneNode_c::GetLimits(int, float*, float*)。
    // 如果任务对象中包含指向此类限制值 float 数组的指针，且数组前两个 float 恰好为 0.0f（前8字节为0），
    // 盲扫就会将该指针强行置为 nullptr，导致 GetLimits 写入时发生空指针解引用闪退（fault addr 0x10）。
    //
    // 后续若重新启用：
    // 若未来需要重新启用此净化器以防止其他未挂钩子处的虚函数闪退，必须通过逆向分析（如使用 IDA/r2）
    // 找出 CTask 各个子类（如 CTaskComplexPartner 等）中存放子任务或 Ped 指针的【精确偏移量】（Offsets），
    // 并仅针对这些特定偏移量进行安全校验与置空，严禁进行盲目全内存扫描。"""
    new = """    // Blind memory scan disabled: 8-byte stepping can mistake floats/CVector fields
    // for stale pointers and null them out, causing fault-at-0x10 crashes.
    // Re-enable only with per-task-class offsets from reverse engineering."""
    return text.replace(old, new)


def main() -> None:
    for path in FILES:
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        text = clean_section_headers(text)
        text = clean_inline(text)
        if path.name == "module.cpp":
            text = clean_file_header_module(text)
        if path.name == "ecs_engine.hpp":
            text = clean_ecs_engine(text)
        if path.name == "hooks_stability.cpp":
            text = clean_sanitize_block(text)
        path.write_text(text, encoding="utf-8")
        print("cleaned", path.relative_to(ROOT))


if __name__ == "__main__":
    main()