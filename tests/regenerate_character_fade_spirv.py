"""Regenerate only scene ps_main's four variants; requires DXC 1.9.2602.17.

python tests/regenerate_character_fade_spirv.py --dxc /path/to/dxc [--check]
All other blobs, register bindings and lookup rows are preserved.
"""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BINDINGS = {
    "b0": (0, 0), "t2": (0, 1), "b1": (0, 2), "b2": (0, 3),
    "s0": (0, 4), "s1": (0, 5), "t0": (1, 0), "t1": (2, 0),
    "t3": (3, 0), "t4": (4, 0), "t5": (4, 1), "t6": (5, 0),
    "t7": (5, 1), "t8": (6, 0), "t9": (6, 1), "t10": (6, 2),
}
VARIANTS = {
    "k_scene_ps_main": (),
    "k_scene_ps_main_HDR_1": ("HDR=1",),
    "k_scene_ps_main_SHOWCASE_1": ("SHOWCASE=1",),
    "k_scene_ps_main_HDR_1_SHOWCASE_1": ("HDR=1", "SHOWCASE=1"),
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    source = ROOT / "src/native/shaders/scene.hlsl"
    header = ROOT / "src/native/shaders/spirv/skate3_native_shaders_spirv.h"
    original = header.read_bytes()
    updated = original
    with tempfile.TemporaryDirectory() as directory:
        for name, defines in VARIANTS.items():
            output = Path(directory) / (name + ".spv")
            command = [args.dxc, str(source), "-E", "ps_main", "-T", "ps_6_0",
                       "-spirv", "-fspv-target-env=vulkan1.1", "-O3", "-Fo", str(output)]
            for register, (descriptor_set, binding) in BINDINGS.items():
                command += ["-fvk-bind-register", register, "0", str(binding), str(descriptor_set)]
            for define in defines:
                command += ["-D", define]
            subprocess.run(command, check=True)
            data = output.read_bytes()
            if len(data) < 20 or len(data) % 4 or data[:4] != b"\x03\x02\x23\x07":
                raise ValueError("Invalid SPIR-V: " + name)
            words = struct.unpack("<" + "I" * (len(data) // 4), data)
            pattern = rb"// scene.hlsl : ps_main[^\r\n]*\r?\nstatic const uint32_t " + name.encode() + rb"\[\] = \{.*?\n\};"
            matches = list(re.finditer(pattern, updated, re.S))
            if len(matches) != 1:
                raise ValueError("Expected one array: " + name)
            match = matches[0]
            newline = b"\r\n" if b"\r\n" in match[0] else b"\n"
            variant = " [" + ";".join(defines) + "]" if defines else ""
            lines = [f"// scene.hlsl : ps_main{variant} ({len(data)} bytes; DXC 1.9.2602.17)",
                     "static const uint32_t " + name + "[] = {"]
            for start in range(0, len(words), 8):
                lines.append("    " + ", ".join(f"0x{word:08x}" for word in words[start:start+8]) + ",")
            lines.append("};")
            updated = updated[:match.start()] + newline.join(line.encode() for line in lines) + updated[match.end():]
    if header.read_bytes() != original:
        raise ValueError("Header changed during compilation")
    if args.check:
        if updated != original:
            raise SystemExit("Regeneration differs; header left unchanged")
    elif updated != original:
        header.write_bytes(updated)
    print("PASS: four scene pixel shaders compiled; other arrays and lookup rows preserved")

if __name__ == "__main__":
    main()
