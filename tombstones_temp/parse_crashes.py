import struct
import re
import os
import sys

elf_path = "/data/data/com.termux/files/home/Projects/mod-workspace/libUE4.so"

def load_symbols():
    symbols = []
    if not os.path.exists(elf_path):
        print(f"libUE4.so not found at {elf_path}")
        return None
    try:
        with open(elf_path, "rb") as f:
            header = f.read(64)
            if not header.startswith(b"\x7fELF"):
                return None
            e_shoff = struct.unpack("<Q", header[40:48])[0]
            e_shentsize = struct.unpack("<H", header[58:60])[0]
            e_shnum = struct.unpack("<H", header[60:62])[0]
            e_shstrndx = struct.unpack("<H", header[62:64])[0]

            f.seek(e_shoff)
            sh_bytes = f.read(e_shnum * e_shentsize)
            sections = []
            for i in range(e_shnum):
                offset = i * e_shentsize
                sh = sh_bytes[offset:offset+e_shentsize]
                if len(sh) < 64:
                    continue
                sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack("<IIQQQQIIQQ", sh[:64])
                sections.append({
                    'index': i, 'name_idx': sh_name, 'type': sh_type, 'offset': sh_offset,
                    'size': sh_size, 'link': sh_link, 'entsize': sh_entsize, 'addr': sh_addr
                })

            shstrtab_sec = sections[e_shstrndx]
            f.seek(shstrtab_sec['offset'])
            shstrtab_data = f.read(shstrtab_sec['size'])

            def get_str(data, idx):
                end = data.find(b'\x00', idx)
                return data[idx:end].decode('utf-8', errors='ignore') if end != -1 else ""

            for sec in sections:
                sec['name'] = get_str(shstrtab_data, sec['name_idx'])

            sym_sec = None
            str_sec = None
            for sec in sections:
                if sec['name'] == ".symtab" or (sec['name'] == ".dynsym" and not sym_sec):
                    sym_sec = sec
                if sec['name'] == ".strtab" or (sec['name'] == ".dynstr" and not str_sec):
                    str_sec = sec

            if not sym_sec or not str_sec:
                for sec in sections:
                    if sec['type'] == 11:
                        sym_sec = sec
                    if sec['type'] == 3 and sec['name'] == ".dynstr":
                        str_sec = sec

            if not sym_sec or not str_sec:
                return None

            f.seek(str_sec['offset'])
            str_data = f.read(str_sec['size'])

            f.seek(sym_sec['offset'])
            sym_entry_size = sym_sec['entsize']
            num_symbols = sym_sec['size'] // sym_entry_size

            for i in range(num_symbols):
                sym_data = f.read(sym_entry_size)
                if len(sym_data) < sym_entry_size:
                    break
                st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack("<IBBHQQ", sym_data)
                if st_value == 0:
                    continue
                name = get_str(str_data, st_name)
                symbols.append((st_value, st_size, name))
            symbols.sort(key=lambda x: x[0])
    except Exception as e:
        print("Error loading symbols:", e)
    return symbols

def lookup_address(symbols, target_addr):
    if not symbols:
        return "Unknown (No Symbols)"
    low = 0
    high = len(symbols) - 1
    closest_idx = -1
    
    while low <= high:
        mid = (low + high) // 2
        val, size, name = symbols[mid]
        if val <= target_addr:
            closest_idx = mid
            low = mid + 1
        else:
            high = mid - 1
            
    if closest_idx != -1:
        val, size, name = symbols[closest_idx]
        if size > 0 and val <= target_addr < val + size:
            return f"{name} (Exact)"
        else:
            return f"{name} + 0x{target_addr - val:x}"
    return "Unknown"

def find_base_addr_from_map(content):
    pattern = r"([0-9a-fA-F]+)-([0-9a-fA-F]+)\s+(r[-xwp]{3})\s+([0-9a-fA-F]+)\s+\S+\s+\S+\s+(/data/app/\S+split_config\.arm64_v8a\.apk)"
    matches = re.findall(pattern, content)
    for start, end, perm, offset, path in matches:
        start_val = int(start, 16)
        end_val = int(end, 16)
        size = end_val - start_val
        if size == 0x4146000 and 'r--' in perm:
            return start_val
    return None

def analyze_tombstone(filepath, symbols):
    try:
        with open(filepath, "r", errors="ignore") as f:
            content = f.read()
            
        cmdline_m = re.search(r"Cmdline:\s+(\S+)", content)
        if not cmdline_m:
            return None
        cmdline = cmdline_m.group(1)
        if "gtasa" not in cmdline:
            return None
            
        ts_m = re.search(r"Timestamp:\s+(.+)", content)
        ts = ts_m.group(1).strip() if ts_m else "Unknown"
        
        signal_m = re.search(r"signal\s+(\d+)\s+\((.+?)\),\s+code\s+(-?\d+)\s+\((.+?)\),\s+fault addr\s+(\S+)", content)
        sig_info = ""
        if signal_m:
            sig_info = f"Signal {signal_m.group(1)} ({signal_m.group(2)}), FaultAddr {signal_m.group(5)}"
            
        cause_m = re.search(r"Cause:\s+(.+)", content)
        cause = cause_m.group(1).strip() if cause_m else ""
        
        abort_m = re.search(r"Abort message:\s+(.+)", content)
        abort = abort_m.group(1).strip() if abort_m else ""
        
        # Parse registers to find absolute PC
        pc_reg_m = re.search(r"\bpc\s+([0-9a-fA-F]+)\b", content)
        absolute_pc = int(pc_reg_m.group(1), 16) if pc_reg_m else None
        
        # Parse backtrace
        backtrace_lines = []
        in_backtrace = False
        for line in content.splitlines():
            if "backtrace:" in line:
                in_backtrace = True
                continue
            if in_backtrace:
                if line.strip() == "" or (not line.startswith(" ") and "   " not in line and "#" not in line):
                    if len(backtrace_lines) > 0:
                        break
                    continue
                if "#" in line:
                    backtrace_lines.append(line.strip())
                    
        parsed_frames = []
        base_addr = None
        
        for frame in backtrace_lines:
            m = re.search(r"#(\d+)\s+pc\s+([0-9a-fA-F]+)\s+(\S+)(?:\s+\(offset\s+(0x[0-9a-fA-F]+)\))?", frame)
            if m:
                frame_num = int(m.group(1))
                pc_val = int(m.group(2), 16)
                lib_path = m.group(3)
                offset_str = m.group(4)
                
                resolved_sym = "N/A"
                elf_vaddr = None
                
                if "split_config.arm64_v8a.apk" in lib_path:
                    if offset_str:
                        offset_val = int(offset_str, 16)
                        elf_vaddr = pc_val + offset_val - 0x3000
                    else:
                        if base_addr is None:
                            base_addr = find_base_addr_from_map(content)
                        if base_addr and absolute_pc:
                            elf_vaddr = absolute_pc - base_addr
                    
                    if elf_vaddr is not None:
                        resolved_sym = lookup_address(symbols, elf_vaddr)
                    else:
                        resolved_sym = "APK (Could not resolve ELF_vaddr)"
                elif "zygisk" in lib_path or "arm64-v8a.so" in lib_path or "dispatch" in lib_path:
                    resolved_sym = f"Mod: {os.path.basename(lib_path)} + {hex(pc_val)}"
                else:
                    resolved_sym = f"Lib: {os.path.basename(lib_path)} + {hex(pc_val)}"
                    
                parsed_frames.append({
                    'num': frame_num,
                    'pc': pc_val,
                    'lib': os.path.basename(lib_path),
                    'offset': offset_str,
                    'elf_vaddr': elf_vaddr,
                    'symbol': resolved_sym
                })
                
        return {
            'file': os.path.basename(filepath),
            'timestamp': ts,
            'signal': sig_info,
            'cause': cause,
            'abort': abort,
            'frames': parsed_frames
        }
    except Exception as e:
        print(f"Error analyzing {filepath}: {e}")
        return None

def main():
    print("Loading symbols...")
    symbols = load_symbols()
    print(f"Loaded {len(symbols) if symbols else 0} symbols.")
    
    tombstone_dir = "/data/data/com.termux/files/home/Projects/mod-workspace/tombstones_temp"
    files = [os.path.join(tombstone_dir, f) for f in os.listdir(tombstone_dir) if re.match(r"tombstone_\d+$", f)]
    files.sort()
    
    gta_crashes = []
    for f in files:
        res = analyze_tombstone(f, symbols)
        if res:
            gta_crashes.append(res)
            
    print(f"\nFound {len(gta_crashes)} GTA SA DE crashes.")
    for crash in gta_crashes:
        print("=" * 80)
        print(f"File: {crash['file']} | Time: {crash['timestamp']}")
        print(f"Signal: {crash['signal']}")
        if crash['cause']:
            print(f"Cause: {crash['cause']}")
        if crash['abort']:
            print(f"Abort Message: {crash['abort']}")
        print("Backtrace:")
        for frame in crash['frames'][:6]:
            vaddr_str = f" | ELF_vaddr: {hex(frame['elf_vaddr'])}" if frame['elf_vaddr'] is not None else ""
            print(f"  #{frame['num']:02d} pc {hex(frame['pc'])}  {frame['lib']}{vaddr_str} -> {frame['symbol']}")
            
if __name__ == "__main__":
    main()
