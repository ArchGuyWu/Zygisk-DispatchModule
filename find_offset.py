import zipfile
import sys

apk_path = "/data/app/~~Hnr3_03VrBCUPE_GwkzmiA==/com.rockstargames.gtasa.de-WOYhjG8xHfPjsz8XmFchng==/split_config.arm64_v8a.apk"
offsets = [0x5598000, 0x54ec000, 0x54ae000]

print(f"Opening {apk_path}...")
try:
    with open(apk_path, "rb") as f:
        zz = zipfile.ZipFile(f)
        for info in zz.infolist():
            for opt in offsets:
                diff = opt - info.header_offset
                if abs(diff) < 128 * 1024:
                    print(f"Offset 0x{opt:x} is close to Entry: {info.filename}")
                    print(f"  Header offset: {info.header_offset} (0x{info.header_offset:x})")
                    print(f"  Difference: {diff}")
except Exception as e:
    print("Error:", e)
