"""Source-only checks for the scoped final-screen shader generator.

Run with: python -B -m unittest discover -s tests -p test_generate_screen_fx_spirv.py
Real shader reproduction is checked separately with the generator's --check.
"""

import contextlib
import importlib.util
import io
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "tools/generate_screen_fx_spirv.py"
SPEC = importlib.util.spec_from_file_location("generate_screen_fx_spirv", SCRIPT)
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)

EXPECTED_MATRIX = (
    ("vs_raw", "vs_6_0", "", "k_screen_fx_vs_raw"),
    ("vs_scaled", "vs_6_0", "", "k_screen_fx_vs_scaled"),
    ("ps_opaque", "ps_6_0", "", "k_screen_fx_ps_opaque"),
    ("ps_alpha", "ps_6_0", "", "k_screen_fx_ps_alpha"),
)
OWN_NAMES = {request[3] for request in EXPECTED_MATRIX}


def lookup_row(entry, variant, name, filename="screen_fx.hlsl"):
    return f'    {{"{filename}", "{entry}", "{variant}", {name}, sizeof({name})}},'


def fixture(newline="\n", installed=True):
    """Keep 75 preexisting shaders and the five separate HoM additions."""
    old_names = ["k_photo_fx_vs_raw", "k_photo_fx_vs_scaled"]
    old_names += [f"k_existing_{index:03d}" for index in range(73)]
    hom_matrix = (
        ("vs_hom", "", "k_hom_vs_hom"),
        ("ps_hom", "", "k_hom_ps_hom"),
        ("ps_hom", "HDR=1", "k_hom_ps_hom_HDR_1"),
        ("ps_hom", "SHOWCASE=1", "k_hom_ps_hom_SHOWCASE_1"),
        ("ps_hom", "HDR=1;SHOWCASE=1", "k_hom_ps_hom_HDR_1_SHOWCASE_1"),
    )
    old_names += [name for _, _, name in hom_matrix]
    old_blobs = {name: struct.pack("<II", 0x07230203, index + 1)
                 for index, name in enumerate(old_names)}

    def array(name):
        words = struct.unpack("<II", old_blobs[name])
        return (f"static const uint32_t {name}[] = {{\n"
                f"    0x{words[0]:08x}, 0x{words[1]:08x},\n}};\n")

    prefix = "#pragma once\n#include <cstdint>\n\n// Existing shaders\n"
    prefix += "\n".join(array(name) for name in old_names[:75])
    prefix += "\n// HOM_GENERATED_START\n"
    prefix += "\n".join(array(name) for name in old_names[75:])
    prefix += "// HOM_GENERATED_END\n\n"
    blobs = {name: struct.pack("<II", 0x07230203, index + 101)
             for index, (_, _, _, name) in enumerate(EXPECTED_MATRIX)}
    if installed:
        prefix += GEN.generated_block(blobs, "test compiler") + "\n\n"
    suffix = "struct NativeSpirvBlob {\n    const char* file;\n};\n\n"
    suffix += "static const NativeSpirvBlob kNativeSpirvBlobs[] = {\n"
    if installed:
        suffix += "\n".join(lookup_row(entry, variant, name)
                            for entry, _, variant, name in EXPECTED_MATRIX) + "\n"
    old_rows = [lookup_row("vs_raw", "", old_names[0], "photo_fx.hlsl"),
                lookup_row("vs_scaled", "", old_names[1], "photo_fx.hlsl")]
    old_rows += [lookup_row(f"vs_existing_{index}", "", name, "other.hlsl")
                 for index, name in enumerate(old_names[2:75])]
    old_rows += [lookup_row(entry, variant, name, "scene.hlsl")
                 for entry, variant, name in hom_matrix]
    old_lookup = "\n".join(old_rows)
    suffix += old_lookup + "\n};\n// Footer\n"
    text = (prefix + suffix).replace("\n", newline)
    return text.encode(), blobs, old_blobs, old_lookup.replace("\n", newline)


def instruction(opcode, *args):
    return [((len(args) + 1) << 16) | opcode, *args]


def string_words(text):
    data = text.encode() + b"\0"
    data += b"\0" * (-len(data) % 4)
    return list(struct.unpack("<" + "I" * (len(data) // 4), data))


def reflection_fixture(entry="ps_opaque", model=4, resources=(("t0", 1, 0),)):
    words = [0x07230203, 0x00010300, 0, len(resources) + 2, 0]
    words += instruction(15, model, 1, *string_words(entry))
    for resource_id, (name, descriptor_set, binding) in enumerate(resources, 2):
        words += instruction(5, resource_id, *string_words(name))
        words += instruction(71, resource_id, 34, descriptor_set)
        words += instruction(71, resource_id, 33, binding)
    return struct.pack("<" + "I" * len(words), *words)


def append_instruction(data, opcode, *args):
    words = instruction(opcode, *args)
    return data + struct.pack("<" + "I" * len(words), *words)


class GeneratorTest(unittest.TestCase):
    def test_exact_compile_matrix(self):
        self.assertEqual(GEN.MATRIX, EXPECTED_MATRIX)

    def test_absent_block_is_valid_only_without_own_arrays_or_rows(self):
        original, _, old_blobs, _ = fixture(installed=False)
        text = original.decode()
        self.assertEqual(GEN.validate_header(text), old_blobs)
        with self.assertRaises(ValueError):
            GEN.block_bounds(text)
        stray_array = "static const uint32_t k_screen_fx_unknown[] = {\n0x00000000,\n};\n"
        stray_row = lookup_row("ps_opaque", "", "k_existing_000")
        for bad in (text + stray_array, text + stray_row,
                    text + lookup_row("vs_raw", "", "k_screen_fx_vs_raw", "other.hlsl")):
            with self.subTest(suffix=bad[len(text):]):
                with self.assertRaises(ValueError):
                    GEN.validate_header(bad)

    def test_initial_insertion_preserves_all_80_arrays_and_lookup_rows(self):
        for newline in ("\n", "\r\n"):
            with self.subTest(newline=repr(newline)):
                original, blobs, old_blobs, old_lookup = fixture(newline, installed=False)
                updated, count = GEN.replace_block(original, blobs, "test compiler")
                text = updated.decode()
                self.assertEqual(count, 80)
                arrays = GEN.validate_header(text)
                self.assertEqual({name: arrays[name] for name in old_blobs}, old_blobs)
                self.assertEqual({name: arrays[name] for name in OWN_NAMES}, blobs)
                self.assertEqual(len(arrays), 84)
                self.assertIn(old_lookup, text)
                self.assertEqual(text.count("// HOM_GENERATED_START"), 1)
                start, end = GEN.block_bounds(text)
                self.assertLess(end, text.index("struct NativeSpirvBlob"))
                self.assertLess(start, end)
                table_start = text.index("static const NativeSpirvBlob kNativeSpirvBlobs[] = {")
                own_rows = newline.join(lookup_row(entry, variant, name)
                                        for entry, _, variant, name in EXPECTED_MATRIX)
                self.assertLess(table_start, text.index(own_rows))
                self.assertLess(text.index(own_rows), text.index(old_lookup))
                if newline == "\r\n":
                    self.assertNotIn(b"\n", updated.replace(b"\r\n", b""))

    def test_initial_insertion_requires_unique_struct_and_lookup_table(self):
        original, blobs, _, _ = fixture(installed=False)
        text = original.decode()
        anchors = ("struct NativeSpirvBlob {", "static const NativeSpirvBlob kNativeSpirvBlobs[] = {")
        for anchor in anchors:
            for bad in (text.replace(anchor, "// Missing insertion anchor"), text + "\n" + anchor):
                with self.subTest(anchor=anchor, duplicate=bad.endswith(anchor)):
                    with self.assertRaises(ValueError):
                        GEN.replace_block(bad.encode(), blobs, "test compiler")

    def test_bytewise_noop_and_newline_preservation(self):
        for newline in ("\n", "\r\n"):
            with self.subTest(newline=repr(newline)):
                original, blobs, _, _ = fixture(newline)
                updated, count = GEN.replace_block(original, blobs, "test compiler")
                self.assertEqual(updated, original)
                self.assertEqual(count, 80)

    def test_subsequent_update_changes_only_own_block(self):
        original, blobs, old_blobs, _ = fixture()
        blobs["k_screen_fx_ps_alpha"] = struct.pack("<II", 0x12345678, 0x87654321)
        updated, count = GEN.replace_block(original, blobs, "new compiler")
        before, after = original.decode(), updated.decode()
        old_start, old_end = GEN.block_bounds(before)
        new_start, new_end = GEN.block_bounds(after)
        self.assertEqual(before[:old_start], after[:new_start])
        self.assertEqual(before[old_end:], after[new_end:])
        self.assertEqual(count, 80)
        arrays = GEN.parse_arrays(after)
        self.assertEqual({name: arrays[name] for name in old_blobs}, old_blobs)

    def test_shared_photo_fx_entry_names_are_preserved(self):
        original, _, _, _ = fixture()
        text = original.decode()
        self.assertIn(lookup_row("vs_raw", "", "k_photo_fx_vs_raw", "photo_fx.hlsl"), text)
        self.assertIn(lookup_row("vs_scaled", "", "k_photo_fx_vs_scaled", "photo_fx.hlsl"), text)
        self.assertEqual(len(GEN.validate_header(text)), 84)

    def test_missing_duplicate_and_reversed_markers_fail(self):
        original, _, _, _ = fixture()
        text = original.decode()
        bad_headers = (text.replace(GEN.START, "// Missing start"),
                       text.replace(GEN.END, "// Missing end"),
                       text + "\n" + GEN.START + "\n",
                       text + "\n" + GEN.END + "\n",
                       text.replace(GEN.START, "// SWAP").replace(GEN.END, GEN.START)
                       .replace("// SWAP", GEN.END))
        for bad in bad_headers:
            with self.subTest(tail=bad[-70:]):
                with self.assertRaises(ValueError):
                    GEN.validate_header(bad)

    def test_duplicate_missing_and_unknown_arrays_fail(self):
        original, _, _, _ = fixture()
        text = original.decode()
        missing = text.replace("static const uint32_t k_screen_fx_vs_raw[]",
                               "static const uint32_t k_unowned_vs_raw[]")
        extras = ("static const uint32_t k_existing_000[] = {\n0x00000000,\n};\n",
                  "static const uint32_t k_screen_fx_ps_alpha[] = {\n0x00000000,\n};\n",
                  "static const uint32_t k_screen_fx_unknown[] = {\n0x00000000,\n};\n")
        for bad in (missing, *(text + extra for extra in extras)):
            with self.assertRaises(ValueError):
                GEN.validate_header(bad)

    def test_missing_duplicate_and_mismatched_lookup_rows_fail(self):
        original, _, _, _ = fixture()
        text = original.decode()
        row = lookup_row("vs_raw", "", "k_screen_fx_vs_raw")
        bad_rows = ("", row + "\n" + row,
                    row.replace('"screen_fx.hlsl"', '"other.hlsl"'),
                    row.replace('"vs_raw"', '"vs_unknown"'),
                    row.replace('"", k_screen_fx', '"HDR=1", k_screen_fx'),
                    row.replace("sizeof(k_screen_fx_vs_raw)", "sizeof(k_screen_fx_vs_scaled)"))
        for replacement in bad_rows:
            with self.subTest(row=replacement):
                with self.assertRaises(ValueError):
                    GEN.validate_header(text.replace(row, replacement))

    def test_reflection_accepts_both_stages_and_frozen_named_bindings(self):
        resources = (("Consts", 0, 0), ("s_lin", 0, 1))
        resources += tuple((f"t{index}", 1, index) for index in range(3))
        for entry, profile, _, _ in EXPECTED_MATRIX:
            with self.subTest(entry=entry):
                model = 0 if profile.startswith("vs_") else 4
                GEN.validate_spirv(reflection_fixture(entry, model, resources), entry, profile)

    def test_reflection_rejects_wrong_entry_stage_and_bindings(self):
        bad_blobs = (reflection_fixture(entry="ps_unknown"), reflection_fixture(model=0),
                     reflection_fixture(resources=(("t0", 1, 1),)),
                     reflection_fixture(resources=(("t0", 0, 0),)),
                     reflection_fixture(resources=(("unknown", 1, 0),)))
        for bad in bad_blobs:
            with self.assertRaises(ValueError):
                GEN.validate_spirv(bad, "ps_opaque", "ps_6_0")
        with self.assertRaises(ValueError):
            GEN.validate_spirv(reflection_fixture(entry="vs_raw", model=4), "vs_raw", "vs_6_0")

    def test_duplicate_reflection_decorations_and_entry_points_fail(self):
        original = reflection_fixture()
        extra_instructions = ((71, 2, 33, 0), (71, 2, 34, 1),
                              (5, 2, *string_words("t0")),
                              (15, 4, 1, *string_words("ps_opaque")))
        for extra in extra_instructions:
            with self.assertRaises(ValueError):
                GEN.validate_spirv(append_instruction(original, *extra), "ps_opaque", "ps_6_0")

    def test_malformed_reflection_fails(self):
        original = reflection_fixture()
        bad_magic = struct.pack("<I", 0) + original[4:]
        bad_version = original[:4] + struct.pack("<I", 0x00010400) + original[8:]
        zero_instruction = original + struct.pack("<I", 71)
        incomplete = original[:-4 * len(instruction(71, 2, 33, 0))]
        for bad in (original[:16], original[:-1], original[:-4], bad_magic,
                    bad_version, zero_instruction, incomplete):
            with self.assertRaises(ValueError):
                GEN.validate_spirv(bad, "ps_opaque", "ps_6_0")

    def test_check_never_writes_existing_or_initial_header(self):
        for installed, different in ((True, False), (True, True), (False, False)):
            with self.subTest(installed=installed, different=different):
                original, blobs, _, _ = fixture(installed=installed)
                if different:
                    blobs["k_screen_fx_ps_alpha"] = struct.pack("<I", 0x12345678)
                with tempfile.TemporaryDirectory() as directory:
                    header = Path(directory) / "header.h"
                    header.write_bytes(original)
                    with patch.object(GEN, "compile_blobs", return_value=blobs), \
                            patch.object(GEN.subprocess, "check_output", return_value="test compiler\n"), \
                            patch.object(GEN, "write_atomic") as write, \
                            contextlib.redirect_stdout(io.StringIO()), \
                            contextlib.redirect_stderr(io.StringIO()):
                        result = GEN.main(["--dxc", "unused", "--header", str(header), "--check"])
                    self.assertEqual(result, 1 if different or not installed else 0)
                    write.assert_not_called()
                    self.assertEqual(header.read_bytes(), original)

    def test_concurrent_header_edit_is_preserved_for_write_and_check(self):
        for check in (False, True):
            with self.subTest(check=check):
                original, blobs, _, _ = fixture()
                with tempfile.TemporaryDirectory() as directory:
                    header = Path(directory) / "header.h"
                    header.write_bytes(original)
                    edited = original + b"// Concurrent edit\n"

                    def compile_and_edit(*_):
                        header.write_bytes(edited)
                        return blobs

                    argv = ["--dxc", "unused", "--header", str(header)]
                    if check:
                        argv.append("--check")
                    with patch.object(GEN, "compile_blobs", side_effect=compile_and_edit), \
                            patch.object(GEN.subprocess, "check_output", return_value="test compiler\n"), \
                            patch.object(GEN, "write_atomic") as write:
                        with self.assertRaises(ValueError):
                            GEN.main(argv)
                    write.assert_not_called()
                    self.assertEqual(header.read_bytes(), edited)


if __name__ == "__main__":
    unittest.main()
