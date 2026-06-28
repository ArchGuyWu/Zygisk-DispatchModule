import struct
import sys

import os
elf_path = "/root/workspace/libUE4.so" if os.path.exists("/root/workspace/libUE4.so") else "/data/data/com.termux/files/home/Projects/mod-workspace/libUE4.so"

def lookup_address(target_addr):
    print(f"Looking up address 0x{target_addr:x} in {elf_path}...")
    try:
        with open(elf_path, "rb") as f:
            header = f.read(64)
            if not header.startswith(b"\x7fELF"):
                print("Not a valid ELF file")
                return

            # Read ELF64 header info
            # e_shoff (section header table offset): 40 bytes in, 8 bytes
            # e_shentsize: 58 bytes in, 2 bytes
            # e_shnum: 60 bytes in, 2 bytes
            # e_shstrndx: 62 bytes in, 2 bytes
            e_shoff = struct.unpack_Q(header[40:48])[0]
            e_shentsize = struct.unpack_H(header[58:60])[0]
            e_shnum = struct.unpack_H(header[60:62])[0]
            e_shstrndx = struct.unpack_H(header[62:64])[0]

            # We need to find .symtab or .dynsym and their corresponding string tables (.strtab or .dynstr)
            # Let's read section headers
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
                    'index': i,
                    'name_idx': sh_name,
                    'type': sh_type,
                    'offset': sh_offset,
                    'size': sh_size,
                    'link': sh_link,
                    'entsize': sh_entsize,
                    'addr': sh_addr
                })

            # Get section names using shstrtab
            shstrtab_sec = sections[e_shstrndx]
            f.seek(shstrtab_sec['offset'])
            shstrtab_data = f.read(shstrtab_sec['size'])

            def get_str(data, idx):
                end = data.find(b'\x00', idx)
                if end != -1:
                    return data[idx:end].decode('utf-8', errors='ignore')
                return ""

            for sec in sections:
                sec['name'] = get_str(shstrtab_data, sec['name_idx'])

            # Find .symtab / .strtab or .dynsym / .dynstr
            sym_sec = None
            str_sec = None
            for sec in sections:
                if sec['name'] == ".symtab" or (sec['name'] == ".dynsym" and not sym_sec):
                    sym_sec = sec
                if sec['name'] == ".strtab" or (sec['name'] == ".dynstr" and not str_sec):
                    str_sec = sec

            if not sym_sec or not str_sec:
                print("Symbol table or string table not found")
                # Let's try .dynsym if .symtab is stripped
                for sec in sections:
                    if sec['type'] == 11: # SHT_DYNSYM
                        sym_sec = sec
                    if sec['type'] == 3 and sec['name'] == ".dynstr": # SHT_STRTAB
                        str_sec = sec

            if not sym_sec or not str_sec:
                print("Failed to locate symbol tables")
                return

            print(f"Using symbol table: {sym_sec['name']} (type={sym_sec['type']}, size={sym_sec['size']})")
            print(f"Using string table: {str_sec['name']} (size={str_sec['size']})")

            # Read string table
            f.seek(str_sec['offset'])
            str_data = f.read(str_sec['size'])

            # Read symbols
            f.seek(sym_sec['offset'])
            sym_entry_size = sym_sec['entsize']
            num_symbols = sym_sec['size'] // sym_entry_size

            closest_sym = None
            closest_diff = float('inf')

            # We want to find the symbol whose value is <= target_addr and closest to it
            for i in range(num_symbols):
                sym_data = f.read(sym_entry_size)
                if len(sym_data) < sym_entry_size:
                    break
                # ELF64 Symbol:
                # st_name (4B), st_info (1B), st_other (1B), st_shndx (2B), st_value (8B), st_size (8B)
                st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack("<IBBHQQ", sym_data)
                if st_value == 0:
                    continue
                
                # Check if target_addr falls within this symbol's range
                if st_size > 0:
                    if st_value <= target_addr < st_value + st_size:
                        name = get_str(str_data, st_name)
                        print(f"Exact match: {name} (0x{st_value:x} - 0x{st_value+st_size:x})")
                        return
                
                diff = target_addr - st_value
                if 0 <= diff < closest_diff:
                    closest_diff = diff
                    closest_sym = (st_name, st_value, st_size)

            if closest_sym:
                name = get_str(str_data, closest_sym[0])
                print(f"Closest symbol: {name} at 0x{closest_sym[1]:x} (diff: +0x{closest_diff:x}, size: {closest_sym[2]})")
            else:
                print("No symbol found")

    except Exception as e:
        print("Error:", e)

# Helper struct unpackers
struct.unpack_Q = lambda b: struct.unpack("<Q", b)
struct.unpack_H = lambda b: struct.unpack("<H", b)

if __name__ == "__main__":
    # Addresses to look up
    # 0xb1498 (tombstone_32 crash PC)
    # 0x114ddc8 (tombstone_11 crash PC)
    # 0x2389c (tombstone_13 crash PC)
    lookup_address(0xb1498)
    lookup_address(0x114ddc8)
    lookup_address(0x2389c)
