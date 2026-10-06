#!/usr/bin/env python3
"""Regenerate only the four final screen effects SPIR-V blobs in the native header.

Run from any directory:
  python tools/generate_screen_fx_spirv.py --dxc /path/to/dxc --check
  python tools/generate_screen_fx_spirv.py --dxc /path/to/dxc

DXC 1.9.2602.17 (21d28f727) reproduces the checked-in screen effects blobs. Other
compiler versions may generate different valid bytecode; --check reports
that difference without writing. This tool never regenerates other shaders.
Its first run adds four lookup rows; subsequent runs preserve the whole lookup
table. Only Python's standard library is needed.
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
DEFAULT_SOURCE = REPO_ROOT / "src/native/shaders/screen_fx.hlsl"
DEFAULT_HEADER = REPO_ROOT / "src/native/shaders/spirv/skate3_native_shaders_spirv.h"
START = "// SCREEN_FX_GENERATED_START"
END = "// SCREEN_FX_GENERATED_END"

# register -> (descriptor set, binding). These match the native RHI layout.
BINDINGS = {
    "b0": (0, 0), "s0": (0, 1),
    **{f"t{index}": (1, index) for index in range(3)},
}
RESOURCE_BINDINGS = {
    "Consts": (0, 0), "s_lin": (0, 1),
    **{f"t{index}": (1, index) for index in range(3)},
}
# entry, shader model, canonical runtime macro signature, C++ array name.
MATRIX = (
    ("vs_raw", "vs_6_0", "", "k_screen_fx_vs_raw"),
    ("vs_scaled", "vs_6_0", "", "k_screen_fx_vs_scaled"),
    ("ps_opaque", "ps_6_0", "", "k_screen_fx_ps_opaque"),
    ("ps_alpha", "ps_6_0", "", "k_screen_fx_ps_alpha"),
)
SCREEN_FX_NAMES = {request[3] for request in MATRIX}
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
        raise ValueError("Expected one complete screen effects generated block")
    return starts[0].start(), ends[0].end()


def validate_header(text):
    arrays = parse_arrays(text)
    rows = LOOKUP_PATTERN.findall(text)
    expected = [("screen_fx.hlsl", entry, variant, name, name)
                for entry, _, variant, name in MATRIX]
    # Entry names such as vs_raw also belong to photo_fx.hlsl. Scope by file
    # and array name rather than treating shared entry names as duplicates.
    actual = [row for row in rows if row[0] == "screen_fx.hlsl" or row[3] in SCREEN_FX_NAMES]
    own_arrays = {name for name in arrays if name.startswith("k_screen_fx_")}
    if START not in text and END not in text:
        if own_arrays or actual:
            raise ValueError("Screen-effects arrays/rows require their generated block")
        return arrays
    start, end = block_bounds(text)
    if own_arrays != SCREEN_FX_NAMES or set(parse_arrays(text[start:end])) != SCREEN_FX_NAMES:
        raise ValueError("Screen-effects block must contain exactly the five supported arrays")
    if actual != expected:
        raise ValueError("Screen-effects lookup rows must match the compile matrix exactly")
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
            raise ValueError("Resource violates frozen screen-effects binding plan: " + str(name))


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
             "// -spirv -fspv-target-env=vulkan1.1 -O3; explicit frozen screen-effects register bindings."]
    for entry, _, variant, name in MATRIX:
        data = blobs[name]
        words = struct.unpack("<" + "I" * (len(data) // 4), data)
        lines.extend(["", f"// screen_fx.hlsl : {entry} [{variant}] ({len(data)} bytes)",
                      f"static const uint32_t {name}[] = {{"])
        for index in range(0, len(words), 8):
            lines.append("    " + ", ".join(f"0x{word:08x}" for word in words[index:index + 8]) + ",")
        lines.append("};")
    lines.append(END)
    return "\n".join(lines)


def replace_block(original, blobs, compiler):
    text = original.decode("utf-8")
    before = validate_header(text)
    before_rows = LOOKUP_PATTERN.findall(text)
    if START in text or END in text:
        start, end = block_bounds(text)
        newline = "\r\n" if "\r\n" in text[start:end] else "\n"
        block = generated_block(blobs, compiler).replace("\n", newline)
        if text[end - 1:end] == "\r":
            block += "\r"
        updated_text = text[:start] + block + text[end:]
    else:
        # First insertion is additive. Keep all old arrays and lookup rows
        # byte-for-byte; later runs replace only this tool's marked block.
        struct_anchor = "struct NativeSpirvBlob {"
        table_anchor = "static const NativeSpirvBlob kNativeSpirvBlobs[] = {"
        if text.count(struct_anchor) != 1 or text.count(table_anchor) != 1:
            raise ValueError("Expected one NativeSpirvBlob schema and lookup table")
        newline = "\r\n" if "\r\n" in text else "\n"
        block = generated_block(blobs, compiler).replace("\n", newline)
        insertion = text.index(struct_anchor)
        updated_text = text[:insertion] + block + newline * 2 + text[insertion:]
        insertion = updated_text.index(table_anchor) + len(table_anchor)
        rows = [f'    {{"screen_fx.hlsl", "{entry}", "{variant}", {name}, sizeof({name})}},'
                for entry, _, variant, name in MATRIX]
        updated_text = (updated_text[:insertion] + newline + newline.join(rows) +
                        updated_text[insertion:])
    after = validate_header(updated_text)
    untouched = {name: data for name, data in before.items() if name not in SCREEN_FX_NAMES}
    if untouched != {name: data for name, data in after.items() if name not in SCREEN_FX_NAMES}:
        raise ValueError("An existing non-screen-effects SPIR-V blob changed")
    old_rows = [row for row in before_rows if row[0] != "screen_fx.hlsl" and row[3] not in SCREEN_FX_NAMES]
    new_rows = [row for row in LOOKUP_PATTERN.findall(updated_text)
                if row[0] != "screen_fx.hlsl" and row[3] not in SCREEN_FX_NAMES]
    if old_rows != new_rows:
        raise ValueError("An existing shader lookup row changed")
    if {name: after[name] for name in SCREEN_FX_NAMES} != blobs:
        raise ValueError("Embedded screen-effects arrays differ from compiled SPIR-V")
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
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE, help="HLSL source (default: repo screen_fx.hlsl)")
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER, help="Native SPIR-V header (default: repo header)")
    parser.add_argument("--check", action="store_true", help="Compile and compare without writing; exit 1 if regeneration differs")
    options = parser.parse_args(argv)
    dxc, source, header = options.dxc.resolve(), options.source.resolve(), options.header.resolve()
    original = header.read_bytes()
    validate_header(original.decode("utf-8"))
    compiler = subprocess.check_output([str(dxc), "--version"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix="screen-fx-spirv-") as directory:
        blobs = compile_blobs(dxc, source, Path(directory))
    updated, preserved_count = replace_block(original, blobs, compiler)
    changed = updated != original
    if header.read_bytes() != original:
        raise ValueError("Header changed during compilation; refusing to overwrite or check stale contents")
    if not options.check and changed:
        write_atomic(header, updated)
    print(f"{'DIFF' if options.check and changed else 'PASS'}: four screen effects blobs compiled; "
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
