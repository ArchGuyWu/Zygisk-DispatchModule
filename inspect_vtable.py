import struct

elf_path = "/data/data/com.termux/files/home/Projects/mod-workspace/libUE4.so"

def inspect_vtable():
    print(f"Inspecting CTask vtable in {elf_path}...")
    try:
        with open(elf_path, "rb") as f:
            header = f.read(64)
            # Read section headers to find .dynsym and .dynstr
            e_shoff = struct.unpack("<Q", header[40:48])[0]
            e_shentsize = struct.unpack("<H", header[58:60])[0]
            e_shnum = struct.unpack("<H", header[60:62])[0]
            
            f.seek(e_shoff)
            sh_bytes = f.read(e_shnum * e_shentsize)
            
            sections = []
            for i in range(e_shnum):
                offset = i * e_shentsize
                sh = sh_bytes[offset:offset+e_shentsize]
                sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack("<IIQQQQIIQQ", sh[:64])
                sections.append({
                    'name_idx': sh_name,
                    'type': sh_type,
                    'offset': sh_offset,
                    'size': sh_size,
                    'link': sh_link,
                    'addr': sh_addr
                })
                
            # Find .shstrtab to get section names
            shstrtab_sec = sections[struct.unpack("<H", header[62:64])[0]]
            f.seek(shstrtab_sec['offset'])
            shstrtab_data = f.read(shstrtab_sec['size'])
            
            def get_str(data, idx):
                end = data.find(b'\x00', idx)
                return data[idx:end].decode('utf-8', errors='ignore') if end != -1 else ""
                
            for sec in sections:
                sec['name'] = get_str(shstrtab_data, sec['name_idx'])
                
            # Find .dynsym and .dynstr
            dynsym_sec = next(sec for sec in sections if sec['name'] == ".dynsym")
            dynstr_sec = next(sec for sec in sections if sec['name'] == ".dynstr")
            
            f.seek(dynstr_sec['offset'])
            dynstr_data = f.read(dynstr_sec['size'])
            
            # Search for _ZTV5CTask symbol
            f.seek(dynsym_sec['offset'])
            sym_size = 24 # ELF64 symbol size
            num_syms = dynsym_sec['size'] // sym_size
            
            ztv_task_addr = None
            for i in range(num_syms):
                sym_data = f.read(sym_size)
                st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack("<IBBHQQ", sym_data)
                name = get_str(dynstr_data, st_name)
                if name == "_ZTV5CTask":
                    ztv_task_addr = st_value
                    print(f"Found _ZTV5CTask at virtual address: 0x{st_value:x}")
                    break
                    
            if not ztv_task_addr:
                print("Symbol _ZTV5CTask not found in .dynsym")
                return
                
            # Now we need to find which section contains this virtual address.
            # In libUE4.so, the virtual addresses correspond to the offsets in the file (since load bias is 0).
            # Let's read the vtable entries at ztv_task_addr.
            # A vtable contains 64-bit function pointers.
            # Let's print the 10 entries before the symbol address, and 20 entries after.
            print("\nMemory around _ZTV5CTask:")
            f.seek(ztv_task_addr - 32)
            for offset_from_sym in range(-32, 80, 8):
                val = struct.unpack("<Q", f.read(8))[0]
                label = ""
                if offset_from_sym == 0:
                    label = " <-- _ZTV5CTask points here"
                elif offset_from_sym == -16:
                    label = " <-- offset-to-top"
                elif offset_from_sym == -8:
                    label = " <-- RTTI descriptor pointer"
                print(f"  Offset {offset_from_sym:+3d}: 0x{val:016x}{label}")
                
    except Exception as e:
        print("Error:", e)

if __name__ == "__main__":
    inspect_vtable()
