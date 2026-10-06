"""Source-only regression checks for the scoped HoM shader generator.

Run with: python -B -m unittest discover -s tests -p test_generate_hom_spirv.py
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


SCRIPT = Path(__file__).resolve().parents[1] / "tools/generate_hom_spirv.py"
SPEC = importlib.util.spec_from_file_location("generate_hom_spirv", SCRIPT)
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)


def fixture(newline="\n"):
    blobs = {name: struct.pack("<I", index + 1)
             for index, (_, _, _, name) in enumerate(GEN.MATRIX)}
    prefix = "// Existing shader\nstatic const uint32_t k_existing[] = {\n    0x07230203,\n};\n\n"
    rows = [f'    {{"scene.hlsl", "{entry}", "{variant}", {name}, sizeof({name})}},'
            for entry, _, variant, name in GEN.MATRIX]
    suffix = '\n\nstatic const NativeSpirvBlob kNativeSpirvBlobs[] = {\n' + "\n".join(rows)
    suffix += '\n    {"other.hlsl", "vs_main", "", k_existing, sizeof(k_existing)},\n};\n// Footer\n'
    text = prefix + GEN.generated_block(blobs, "test compiler") + suffix
    return text.replace("\n", newline).encode(), blobs


def instruction(opcode, *args):
    return [((len(args) + 1) << 16) | opcode, *args]


def string_words(text):
    data = text.encode() + b"\0"
    data += b"\0" * (-len(data) % 4)
    return list(struct.unpack("<" + "I" * (len(data) // 4), data))


def reflection_fixture(name="diffuse", descriptor_set=1, binding=0, model=4):
    words = [0x07230203, 0x00010300, 0, 4, 0]
    words += instruction(15, model, 1, *string_words("ps_hom"))
    words += instruction(5, 2, *string_words(name))
    words += instruction(71, 2, 34, descriptor_set)
    words += instruction(71, 2, 33, binding)
    return struct.pack("<" + "I" * len(words), *words)


class GeneratorTest(unittest.TestCase):
    def test_bytewise_noop_and_newline_preservation(self):
        for newline in ("\n", "\r\n"):
            with self.subTest(newline=repr(newline)):
                original, blobs = fixture(newline)
                updated, count = GEN.replace_block(original, blobs, "test compiler")
                self.assertEqual(updated, original)
                self.assertEqual(count, 1)

    def test_only_generated_block_changes(self):
        original, blobs = fixture()
        blobs["k_hom_ps_hom"] = struct.pack("<II", 0x12345678, 0x87654321)
        updated, _ = GEN.replace_block(original, blobs, "new compiler")
        before, after = original.decode(), updated.decode()
        old_start, old_end = GEN.block_bounds(before)
        new_start, new_end = GEN.block_bounds(after)
        self.assertEqual(before[:old_start], after[:new_start])
        self.assertEqual(before[old_end:], after[new_end:])
        self.assertEqual(GEN.parse_arrays(before)["k_existing"],
                         GEN.parse_arrays(after)["k_existing"])

    def test_missing_marker_and_duplicate_array_fail(self):
        original, _ = fixture()
        text = original.decode()
        for bad in (text.replace(GEN.START, "// missing"),
                    text + "static const uint32_t k_existing[] = {\n0x00000000,\n};\n"):
            with self.subTest(header=bad[-70:]):
                with self.assertRaises(ValueError):
                    GEN.validate_header(bad)

    def test_missing_and_duplicate_lookup_rows_fail(self):
        original, _ = fixture()
        text = original.decode()
        row = '    {"scene.hlsl", "vs_hom", "", k_hom_vs_hom, sizeof(k_hom_vs_hom)},'
        for bad in (text.replace(row, ""), text.replace(row, row + "\n" + row)):
            with self.assertRaises(ValueError):
                GEN.validate_header(bad)

    def test_reflection_matches_entry_and_named_binding(self):
        GEN.validate_spirv(reflection_fixture(), "ps_hom", "ps_6_0")
        for bad in (reflection_fixture(binding=1), reflection_fixture(name="unknown"),
                    reflection_fixture(model=0), reflection_fixture()[:-4]):
            with self.assertRaises(ValueError):
                GEN.validate_spirv(bad, "ps_hom", "ps_6_0")

    def test_duplicate_reflection_decorations_fail(self):
        for extra in (instruction(71, 2, 33, 0), instruction(5, 2, *string_words("diffuse"))):
            bad = reflection_fixture() + struct.pack("<" + "I" * len(extra), *extra)
            with self.assertRaises(ValueError):
                GEN.validate_spirv(bad, "ps_hom", "ps_6_0")

    def test_check_difference_never_writes(self):
        original, blobs = fixture()
        blobs["k_hom_ps_hom"] = struct.pack("<I", 0x12345678)
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "header.h"
            header.write_bytes(original)
            with patch.object(GEN, "compile_blobs", return_value=blobs), \
                    patch.object(GEN.subprocess, "check_output", return_value="test compiler\n"), \
                    contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                result = GEN.main(["--dxc", "unused", "--header", str(header), "--check"])
            self.assertEqual(result, 1)
            self.assertEqual(header.read_bytes(), original)

    def test_concurrent_header_edit_is_preserved(self):
        original, blobs = fixture()
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "header.h"
            header.write_bytes(original)
            edited = original + b"// Concurrent edit\n"

            def compile_and_edit(*_):
                header.write_bytes(edited)
                return blobs

            with patch.object(GEN, "compile_blobs", side_effect=compile_and_edit), \
                    patch.object(GEN.subprocess, "check_output", return_value="test compiler\n"):
                with self.assertRaises(ValueError):
                    GEN.main(["--dxc", "unused", "--header", str(header)])
            self.assertEqual(header.read_bytes(), edited)


if __name__ == "__main__":
    unittest.main()
