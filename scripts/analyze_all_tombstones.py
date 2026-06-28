import struct
import sys
import re
import os

elf_path = "/root/workspace/libUE4.so" if os.path.exists("/root/workspace/libUE4.so") else "/data/data/com.termux/files/home/Projects/mod-workspace/libUE4.so"

def load_symbols():
    symbols = []
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
    # Binary search
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

def main():
    print("Loading symbols from libUE4.so...")
    symbols = load_symbols()
    print(f"Loaded {len(symbols) if symbols else 0} symbols.")
    
    print("\nProcessing all GTA SA DE tombstones in /data/tombstones/...")
    print("-" * 110)
    print(f"{'Tombstone':<13} | {'Timestamp':<19} | {'Crash PC':<18} | {'Resolved Symbol':<60}")
    print("-" * 110)
    
    for i in range(0, 100):
        path = f"/data/tombstones/tombstone_{i:02d}"
        if not os.path.exists(path):
            path = f"/data/tombstones/tombstone_{i}"
            if not os.path.exists(path):
                continue
        try:
            content = open(path, errors="ignore").read()
            # Verify if it is GTA SA DE
            cmd_m = re.search(r"Cmdline:\s+(.+)", content)
            cmd = cmd_m.group(1).strip() if cmd_m else ""
            if "gtasa" not in cmd:
                continue
                
            ts_m = re.search(r"Timestamp:\s+(.+)", content)
            ts = ts_m.group(1)[:19] if ts_m else "unknown"
            
            pc_m = re.search(r"#00\s+pc\s+([0-9a-fA-F]+)\s+(.+)", content)
            if not pc_m:
                continue
            pc_val = int(pc_m.group(1), 16)
            lib = pc_m.group(2).strip()
            
            resolved = "N/A"
            if "split_config.arm64_v8a.apk" in lib:
                offset_m = re.search(r"offset\s+(0x[0-9a-fA-F]+)", lib)
                if offset_m:
                    offset_val = int(offset_m.group(1), 16)
                    # VirtAddr = (offset_val + pc_val) - 0x1457000 + 0x1000
                    virt_addr = (offset_val + pc_val) - 0x1457000 + 0x1000
                    resolved = lookup_address(symbols, virt_addr)
                else:
                    resolved = f"APK (No Offset)"
            elif "libc.so" in lib:
                abort_m = re.search(r"Abort message:\s+(.+)", content)
                abort = abort_m.group(1).strip() if abort_m else "abort"
                
                # Try to get frame #03 or #04
                frame3_m = re.search(r"#03\s+pc\s+([0-9a-fA-F]+)\s+(.+)", content)
                if frame3_m:
                    f3_pc = int(frame3_m.group(1), 16)
                    f3_lib = frame3_m.group(2).strip()
                    if "split_config.arm64_v8a.apk" in f3_lib:
                        offset_m = re.search(r"offset\s+(0x[0-9a-fA-F]+)", f3_lib)
                        if offset_m:
                            offset_val = int(offset_m.group(1), 16)
                            virt_addr = (offset_val + f3_pc) - 0x1457000 + 0x1000
                            resolved = f"Abort: {abort} | Caller: {lookup_address(symbols, virt_addr)}"
                if resolved == "N/A":
                    resolved = f"Abort: {abort}"
            else:
                # Check if it's our module
                if "zygisk" in lib or "arm64-v8a.so" in lib:
                    resolved = f"Mod: {lib.split('/')[-1]}"
                else:
                    resolved = f"Lib: {lib.split('/')[-1]}"
            
            print(f"tombstone_{i:<2}      | {ts} | {hex(pc_val):<18} | {resolved:<60}")
        except Exception as e:
            print(f"tombstone_{i} error: {e}")

if __name__ == "__main__":
    main()
