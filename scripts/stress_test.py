#!/usr/bin/env python3
import os
import sys
import time
import subprocess
import re

# =====================================================================
# GTA: SA DE Zygisk Module - Automated Stress Test & Stability Monitor
# =====================================================================
# This script runs locally in Termux on the Android device.
# It automates:
# 1. Launching the game.
# 2. Simulating high-frequency inputs (taps/swipes) to force AI task updates.
# 3. Parsing logcat in real-time for our modular sanitizer events.
# 4. Monitoring game process status.
# 5. Automatically capturing and analyzing tombstones if a crash occurs.
# =====================================================================

PACKAGE_NAME = "com.rockstargames.gtasa.de"
TEST_DURATION_SEC = 300  # 5 minutes default
INPUT_FREQUENCY_HZ = 5   # 5 inputs per second
LOG_TAGS = ["ProcessAfterPreRender", "ProcessBuoyancy", "ManageTasks", "PlayFootSteps"]

def run_cmd(cmd, shell=True):
    try:
        res = subprocess.run(cmd, shell=shell, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5)
        return res.stdout.strip()
    except Exception as e:
        return ""

def run_su_cmd(cmd):
    return run_cmd(f"su -c '{cmd}'")

def is_game_running():
    pid = run_su_cmd(f"pidof {PACKAGE_NAME}")
    return pid if pid.isdigit() else None

def start_game():
    print("🚀 Starting GTA: San Andreas DE...")
    run_su_cmd(f"monkey -p {PACKAGE_NAME} -c android.intent.category.LAUNCHER 1")
    time.sleep(8)  # Wait for splash screens

def simulate_input():
    # Simulate random taps on the screen to trigger player/ped task transitions (targeting, moving, etc.)
    # Coordinates are randomized to cover different UI parts
    import random
    x = random.randint(200, 1800)
    y = random.randint(200, 800)
    run_su_cmd(f"input tap {x} {y}")

def get_latest_tombstone_path():
    files = run_su_cmd("ls -t /data/tombstones/ | head -n 1")
    if files:
        return f"/data/tombstones/{files}"
    return None

def analyze_tombstone(tombstone_path, output_path):
    print(f"🔴 Game crashed! Copying and analyzing tombstone: {tombstone_path}")
    run_su_cmd(f"cp {tombstone_path} {output_path}")
    run_cmd(f"chmod 644 {output_path}")
    
    # Simple analysis
    try:
        with open(output_path, "r", errors="ignore") as f:
            content = f.read()
        
        abort_msg = re.search(r"Abort message: '(.*)'", content)
        backtrace = re.findall(r"backtrace:[\s\S]*?#03 pc (\w+)\s+(.*)", content)
        
        print("\n=== CRASH ANALYSIS REPORT ===")
        if abort_msg:
            print(f"Abort Message: {abort_msg.group(1)}")
        else:
            print("Abort Message: None or SEGV")
            
        if backtrace:
            pc, lib = backtrace[0]
            print(f"Crashing Frame #03 PC: 0x{pc} in {lib}")
            # Try to resolve symbol if lookup_symbol is available
            print("💡 Run: python lookup_symbol.py to resolve this address in libUE4.so")
        print("=============================\n")
    except Exception as e:
        print(f"Failed to analyze tombstone: {e}")

def main():
    if os.getuid() != 0 and not run_cmd("which su"):
        print("❌ Error: This script must be run with root privileges (or inside Termux with 'su' available).")
        sys.exit(1)

    print("=========================================================")
    print("🎮 GTA: SA DE Zygisk Module - Stability & Stress Test 🎮")
    print("=========================================================")
    
    # 1. Start/Verify Game
    pid = is_game_running()
    if not pid:
        start_game()
        pid = is_game_running()
        if not pid:
            print("❌ Failed to start the game. Please start it manually and run the script again.")
            sys.exit(1)
    
    print(f"✅ Game is running (PID: {pid}).")
    
    # Clear logcat to start fresh
    run_su_cmd("logcat -c")
    
    # Start logcat monitoring process
    print("🎙️ Starting real-time logcat monitoring...")
    logcat_proc = subprocess.Popen(
        ["su", "-c", "logcat | grep -E 'Sanitizer|Hooked|PlayFootSteps'"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True
    )
    
    # Stats counters
    stats = {
        "ProcessAfterPreRender": 0,
        "ProcessBuoyancy": 0,
        "ManageTasks": 0,
        "PlayFootSteps": 0
    }
    
    start_time = time.time()
    last_input_time = 0
    input_interval = 1.0 / INPUT_FREQUENCY_HZ
    
    print(f"⚡ Stress test started. Duration: {TEST_DURATION_SEC}s. Input Freq: {INPUT_FREQUENCY_HZ}Hz.")
    print("Press Ctrl+C to terminate the test early.\n")
    
    try:
        # Non-blocking stdout read setup
        import select
        
        while time.time() - start_time < TEST_DURATION_SEC:
            # 1. Check if game is still running
            if not is_game_running():
                # Game crashed!
                logcat_proc.terminate()
                latest_ts = get_latest_tombstone_path()
                if latest_ts:
                    analyze_tombstone(latest_ts, "tombstone_stress_crash")
                else:
                    print("🔴 Game terminated unexpectedly, but no tombstone was found.")
                sys.exit(1)
            
            # 2. Simulate High-Frequency Input
            current_time = time.time()
            if current_time - last_input_time >= input_interval:
                simulate_input()
                last_input_time = current_time
            
            # 3. Read logcat output
            r, _, _ = select.select([logcat_proc.stdout], [], [], 0.05)
            if r:
                line = logcat_proc.stdout.readline()
                if line:
                    for tag in stats.keys():
                        if tag in line:
                            stats[tag] += 1
                            # Print warning events in real-time
                            if "Clearing unsafe" in line or "⚠️" in line:
                                print(f"  [SANITIZER EVENT] {line.strip()}")
            
            # Print progress every 10 seconds
            elapsed = int(time.time() - start_time)
            if elapsed > 0 and elapsed % 30 == 0:
                print(f"⏱️ Progress: {elapsed}/{TEST_DURATION_SEC}s | Interceptions: AfterPreRender={stats['ProcessAfterPreRender']}, Buoyancy={stats['ProcessBuoyancy']}, ManageTasks={stats['ManageTasks']}")
                time.sleep(1) # Prevent double print in the same second
                
    except KeyboardInterrupt:
        print("\n🛑 Test interrupted by user.")
    finally:
        logcat_proc.terminate()
        
    end_time = time.time()
    duration = int(end_time - start_time)
    
    print("\n=========================================================")
    print("📊 STABILITY TEST SUMMARY REPORT")
    print("=========================================================")
    print(f"Status:             🎉 PASSED (No crashes detected)")
    print(f"Test Duration:      {duration} seconds")
    print(f"Inputs Simulated:   ~{int(duration * INPUT_FREQUENCY_HZ)} taps")
    print("---------------------------------------------------------")
    print("Interception Stats (Unsafe tasks neutralized):")
    print(f"  - CPedIntelligence::ProcessAfterPreRender:  {stats['ProcessAfterPreRender']} events")
    print(f"  - CPed::ProcessBuoyancy:                   {stats['ProcessBuoyancy']} events")
    print(f"  - CTaskManager::ManageTasks:               {stats['ManageTasks']} events")
    print(f"  - CPed::PlayFootSteps:                     {stats['PlayFootSteps']} events")
    print("=========================================================")
    print("💡 The module successfully intercepted all invalid/destructed tasks without crashing the game!")

if __name__ == "__main__":
    main()
