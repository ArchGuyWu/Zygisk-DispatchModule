import zipfile

apk_path = "/data/app/~~Hnr3_03VrBCUPE_GwkzmiA==/com.rockstargames.gtasa.de-WOYhjG8xHfPjsz8XmFchng==/split_config.arm64_v8a.apk"

print(f"Opening {apk_path}...")
try:
    with open(apk_path, "rb") as f:
        zz = zipfile.ZipFile(f)
        print(f"{'Filename':<50} {'Offset (Hex)':<15} {'Size':<10}")
        print("-" * 80)
        # Sort by offset
        entries = sorted(zz.infolist(), key=lambda x: x.header_offset)
        for info in entries:
            if info.filename.endswith(".so") or "lib" in info.filename:
                offset_str = f"0x{info.header_offset:x}"
                print(f"{info.filename:<50} {offset_str:<15} {info.file_size:<10}")
except Exception as e:
    print("Error:", e)
