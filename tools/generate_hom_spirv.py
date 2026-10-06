#!/usr/bin/env python3
"""Regenerate only the five Hall of Meat SPIR-V blobs in the native header.

Run from any directory:
  python tools/generate_hom_spirv.py --dxc /path/to/dxc --check
  python tools/generate_hom_spirv.py --dxc /path/to/dxc

DXC 1.9.2602.17 (21d28f727) reproduces the checked-in HoM blobs. Other
compiler versions may generate different valid bytecode; --check reports
that difference without writing. This tool never regenerates other shaders
or changes the shader lookup table. Only Python's standard library is needed.
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = REPO_ROOT / "src/native/shaders/scene.hlsl"
DEFAULT_HEADER = REPO_ROOT / "src/native/shaders/spirv/skate3_native_shaders_spirv.h"
START = "// HOM_GENERATED_START"
END = "// HOM_GENERATED_END"

# register -> (descriptor set, binding). These match the native RHI layout.
BINDINGS = {
    "b0": (0, 0), "t2": (0, 1), "b1": (0, 2), "b2": (0, 3),
    "s0": (0, 4), "s1": (0, 5), "t0": (1, 0), "t1": (2, 0),
    "t3": (3, 0), "t4": (4, 0), "t5": (4, 1), "t6": (5, 0),
    "t7": (5, 1), "t8": (6, 0), "t9": (6, 1), "t10": (6, 2),
}
RESOURCE_BINDINGS = dict(zip(
    ("C", "bones", "S", "CH", "smp", "smp_clamp", "diffuse", "lightmap",
     "macro", "decal_art", "normal_map", "env_cube", "shadow_atlas",
     "detail_map", "spec2_map", "static_sun"),
    BINDINGS.values(),
))
# entry, shader model, canonical runtime macro signature, C++ array name.
MATRIX = (
    ("vs_hom", "vs_6_0", "", "k_hom_vs_hom"),
    ("ps_hom", "ps_6_0", "", "k_hom_ps_hom"),
    ("ps_hom", "ps_6_0", "HDR=1", "k_hom_ps_hom_HDR_1"),
    ("ps_hom", "ps_6_0", "SHOWCASE=1", "k_hom_ps_hom_SHOWCASE_1"),
    ("ps_hom", "ps_6_0", "HDR=1;SHOWCASE=1", "k_hom_ps_hom_HDR_1_SHOWCASE_1"),
)
HOM_NAMES = {request[3] for request in MATRIX}
ARRAY_PATTERN = re.compile(
    r"static const uint32_t (\w+)\[\] = \{(.*?)\n\};", re.S
)
LOOKUP_PATTERN = re.compile(
    r'\{"([^"\n]+)", "([^"\n]+)", "([^"\n]*)", '
    r'(\w+), sizeof\((\w+)\)\}'
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def parse_arrays(text):
    result = {}
    for match in ARRAY_PATTERN.finditer(text):
        name, body = match.groups()
        if name in result:
            raise ValueError("Duplicate SPIR-V array: " + name)
        words = [int(value, 16) for value in re.findall(r"0x[0-9a-fA-F]+", body)]
        result[name] = struct.pack("<" + "I" * len(words), *words)
    return result


def block_bounds(text):
    starts = list(re.finditer(r"(?m)^" + re.escape(START) + r"\r?$", text))
    ends = list(re.finditer(r"(?m)^" + re.escape(END) + r"\r?$", text))
    if len(starts) != 1 or len(ends) != 1 or starts[0].start() >= ends[0].start():
        raise ValueError("Expected one complete HoM generated block")
    return starts[0].start(), ends[0].end()


def validate_header(text):
    start, end = block_bounds(text)
    arrays = parse_arrays(text)
    if set(parse_arrays(text[start:end])) != HOM_NAMES:
        raise ValueError("HoM block must contain exactly the five supported arrays")
    if any(name.startswith("k_hom_") and name not in HOM_NAMES for name in arrays):
        raise ValueError("Unexpected HoM array outside the supported compile matrix")
    rows = LOOKUP_PATTERN.findall(text)
    expected = [("scene.hlsl", entry, variant, name, name)
                for entry, _, variant, name in MATRIX]
    actual = [row for row in rows if row[3] in HOM_NAMES or row[1] in ("vs_hom", "ps_hom")]
    if actual != expected:
        raise ValueError("HoM lookup rows must match the runtime compile matrix exactly")
    return arrays


def spirv_string(words):
    data = struct.pack("<" + "I" * len(words), *words)
    if b"\0" not in data:
        raise ValueError("Unterminated SPIR-V string")
    return data.split(b"\0", 1)[0].decode("utf-8")


def validate_spirv(data, entry, profile):
    if len(data) < 20 or len(data) % 4:
        raise ValueError("Invalid SPIR-V byte length")
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    if words[0] != 0x07230203 or not 0x00010000 <= words[1] <= 0x00010300:
        raise ValueError("Expected Vulkan 1.1 compatible SPIR-V")
    names, sets, bindings, entries = {}, {}, {}, []
    offset = 5
    while offset < len(words):
        count, opcode = words[offset] >> 16, words[offset] & 0xFFFF
        if not count or offset + count > len(words):
            raise ValueError("Malformed SPIR-V instruction stream")
        args = words[offset + 1:offset + count]
        if opcode == 5:  # OpName
            if len(args) < 2:
                raise ValueError("Malformed OpName")
            if args[0] in names:
                raise ValueError("Duplicate OpName")
            names[args[0]] = spirv_string(args[1:])
        elif opcode == 15:  # OpEntryPoint
            if len(args) < 3:
                raise ValueError("Malformed OpEntryPoint")
            entries.append((args[0], spirv_string(args[2:])))
        elif opcode == 71:  # OpDecorate
            if len(args) < 2:
                raise ValueError("Malformed OpDecorate")
            if args[1] in (33, 34):
                if len(args) != 3:
                    raise ValueError("Malformed descriptor decoration")
                values = bindings if args[1] == 33 else sets
                if args[0] in values:
                    raise ValueError("Duplicate descriptor decoration")
                values[args[0]] = args[2]
        offset += count
    expected_model = 0 if profile.startswith("vs_") else 4
    if entries != [(expected_model, entry)]:
        raise ValueError("Unexpected SPIR-V entry point for " + entry)
    if sets.keys() != bindings.keys():
        raise ValueError("Descriptor set/binding decorations are incomplete")
    for resource_id, descriptor_set in sets.items():
        name = names.get(resource_id)
        pair = (descriptor_set, bindings[resource_id])
        if name not in RESOURCE_BINDINGS or RESOURCE_BINDINGS[name] != pair:
            raise ValueError("Resource violates frozen scene binding plan: " + str(name))


def compile_blobs(dxc, source, directory):
    result = {}
    for entry, profile, variant, name in MATRIX:
        output = directory / (name + ".spv")
        args = [str(dxc), str(source), "-E", entry, "-T", profile,
                "-spirv", "-fspv-target-env=vulkan1.1", "-O3", "-Fo", str(output)]
        for register, (descriptor_set, binding) in BINDINGS.items():
            args.extend(["-fvk-bind-register", register, "0", str(binding), str(descriptor_set)])
        for define in filter(None, variant.split(";")):
            args.extend(["-D", define])
        subprocess.run(args, check=True)
        data = output.read_bytes()
        validate_spirv(data, entry, profile)
        result[name] = data
    return result


def generated_block(blobs, compiler):
    if "\n" in compiler or "\r" in compiler:
        raise ValueError("Expected a one-line DXC version")
    lines = [START, "// Additions compiled with " + compiler,
             "// -spirv -fspv-target-env=vulkan1.1 -O3; explicit frozen scene register bindings."]
    for entry, _, variant, name in MATRIX:
        data = blobs[name]
        words = struct.unpack("<" + "I" * (len(data) // 4), data)
        lines.extend(["", f"// scene.hlsl : {entry} [{variant}] ({len(data)} bytes)",
                      f"static const uint32_t {name}[] = {{"])
        for index in range(0, len(words), 8):
            lines.append("    " + ", ".join(f"0x{word:08x}" for word in words[index:index + 8]) + ",")
        lines.append("};")
    lines.append(END)
    return "\n".join(lines)


def replace_block(original, blobs, compiler):
    text = original.decode("utf-8")
    before = validate_header(text)
    start, end = block_bounds(text)
    # Preserve every byte outside the generated block, including lookup rows
    # and line endings. Use the block's existing newline convention inside it.
    newline = "\r\n" if "\r\n" in text[start:end] else "\n"
    block = generated_block(blobs, compiler).replace("\n", newline)
    if text[end - 1:end] == "\r":
        block += "\r"  # The end marker match excludes its terminating LF.
    updated_text = text[:start] + block + text[end:]
    after = validate_header(updated_text)
    untouched = {name: data for name, data in before.items() if name not in HOM_NAMES}
    if untouched != {name: data for name, data in after.items() if name not in HOM_NAMES}:
        raise ValueError("An existing non-HoM SPIR-V blob changed")
    if {name: after[name] for name in HOM_NAMES} != blobs:
        raise ValueError("Embedded HoM arrays differ from compiled SPIR-V")
    return updated_text.encode("utf-8"), len(untouched)


def write_atomic(path, data):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".",
                                         suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, path.stat().st_mode)
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dxc", required=True, type=Path, help="Path to the DXC executable")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE, help="HLSL source (default: repo scene.hlsl)")
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER, help="Native SPIR-V header (default: repo header)")
    parser.add_argument("--check", action="store_true", help="Compile and compare without writing; exit 1 if regeneration differs")
    options = parser.parse_args(argv)
    dxc, source, header = options.dxc.resolve(), options.source.resolve(), options.header.resolve()
    original = header.read_bytes()
    validate_header(original.decode("utf-8"))
    compiler = subprocess.check_output([str(dxc), "--version"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix="hom-spirv-") as directory:
        blobs = compile_blobs(dxc, source, Path(directory))
    updated, preserved_count = replace_block(original, blobs, compiler)
    changed = updated != original
    if header.read_bytes() != original:
        raise ValueError("Header changed during compilation; refusing to overwrite or check stale contents")
    if not options.check and changed:
        write_atomic(header, updated)
    print(f"{'DIFF' if options.check and changed else 'PASS'}: five HoM blobs compiled; "
          f"{preserved_count} existing blobs and lookup table preserved")
    print("header SHA256: " + digest(updated))
    for _, _, variant, name in MATRIX:
        print(f"{name}: {len(blobs[name])} bytes, SHA256 {digest(blobs[name])}")
    if options.check and changed:
        print("Regeneration differs; --check left the header unchanged.", file=sys.stderr)
        return 1
    if not changed:
        print("Header is byte-for-byte current; no write performed.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("error: " + str(error), file=sys.stderr)
        sys.exit(2)
