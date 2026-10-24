"""Exercise production codegen and source-list wiring without game files.

Run with Python 3.11+, CMake 3.25+ and a C++ compiler:
python3 tests/generated_root_test.py
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
MARKERS = "// Skate3UltrawideGameFrustumPatchScope\n// Skate3MaybeOverrideProjectionFovRadians\n// ShouldForceIntroMovieComplete\n"


def production_wiring():
    cmake = (ROOT / "CMakeLists.txt").read_text()
    start = cmake.index("set(SKATE3_GENERATED_ROOT\n")
    definition = cmake[start:cmake.index(")", start) + 1]
    normalization = "\n".join(
        line for line in cmake.splitlines()
        if line.startswith("file(TO_CMAKE_PATH ")
        and any(name in line for name in (
            "SKATE3_SOURCE_DIR_CONFIG", "SKATE3_GAME_DATA_ROOT_CONFIG",
            "SKATE3_GENERATED_ROOT_CONFIG"))
    )
    start = cmake.index('if(EXISTS "${SKATE3_GENERATED_ROOT}/sources.cmake")')
    sources = cmake[start:cmake.index("target_link_libraries(skate3", start)]
    return definition, normalization, sources


class GeneratedRootTest(unittest.TestCase):
    def exercise_generate_and_build(self, external_root):
        with tempfile.TemporaryDirectory(prefix="skate3 codegen ") as directory:
            base = pathlib.Path(directory)
            source = base / "source"
            source.mkdir()
            output = (base / "external directory/compiled assets"
                      if external_root else source / "generated")
            build = base / "build"
            (source / "cmake").mkdir()
            shutil.copy2(ROOT / "cmake/ApplySkate3CodegenPatches.cmake", source / "cmake")
            wrong = source / "generated/skate3_recomp.old.cpp"
            if external_root:
                wrong.parent.mkdir()
                wrong.write_text("// Old output; no patch anchors.\n")
            (source / "codegen").write_text(textwrap.dedent(f'''\
                import pathlib, sys, tomllib
                config = tomllib.loads(pathlib.Path(sys.argv[1]).read_text())
                output = pathlib.Path(config["entrypoint"]["out_directory_path"])
                output.mkdir(parents=True, exist_ok=True)
                name = config["project"]["name"]
                if name == "skate3":
                    filename = "skate3_recomp.fixture.cpp"
                    (output / filename).write_text({MARKERS!r} + "int generated_skate3() {{ return 1; }}\\n")
                    (output / "skate3_init.h").write_text("int generated_skate3();\\nint generated_eawebkit();\\n")
                else:
                    filename = "eawebkit.fixture.cpp"
                    (output / filename).write_text("int generated_eawebkit() {{ return 2; }}\\n")
                (output / "sources.cmake").write_text('set(GENERATED_SOURCES "${{CMAKE_CURRENT_LIST_DIR}}/' + filename + '")\\n')
            '''))
            # Run the production declaration, normalization and source-list
            # blocks so omissions in the top-level wiring cannot be hidden by
            # supplying equivalent definitions in this fixture.
            definition, normalization, sources = production_wiring()
            include = next(line for line in (ROOT / "src/skate3_app_common.h").read_text().splitlines()
                           if line.startswith("#include") and "skate3_init.h" in line)
            (source / "fixture.cpp").write_text(
                include + "\nint main() { return generated_skate3() + generated_eawebkit() - 3; }\n")
            project = f'''\
cmake_minimum_required(VERSION 3.25)
project(GeneratedRootFixture LANGUAGES CXX)
add_executable(rex::rexglue IMPORTED GLOBAL)
set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION [==[{sys.executable}]==])
{definition}
set(SKATE3_GAME_DATA_ROOT "${{CMAKE_CURRENT_SOURCE_DIR}}/unused-game")
{normalization}
file(MAKE_DIRECTORY "${{CMAKE_CURRENT_BINARY_DIR}}/manifests")
configure_file([==[{(ROOT / 'manifests/skate3.toml.in').as_posix()}]==]
               "${{CMAKE_CURRENT_BINARY_DIR}}/manifests/skate3.toml" @ONLY)
configure_file([==[{(ROOT / 'manifests/eawebkit.toml.in').as_posix()}]==]
               "${{CMAKE_CURRENT_BINARY_DIR}}/manifests/eawebkit.toml" @ONLY)
include([==[{(ROOT / 'cmake/CodegenTargets.cmake').as_posix()}]==])
add_executable(skate3 fixture.cpp)
{sources}
'''
            (source / "CMakeLists.txt").write_text(project)
            configure = ["cmake", "-S", str(source), "-B", str(build)]
            if external_root:
                configure.append(f"-DSKATE3_GENERATED_ROOT={output}")
            subprocess.run(configure, check=True)
            subprocess.run(["cmake", "--build", str(build), "--target", "generate-all"], check=True)
            self.assertTrue((output / "skate3_recomp.fixture.cpp").read_text().startswith(MARKERS))
            self.assertTrue((output / "eawebkit/eawebkit.fixture.cpp").is_file())
            if external_root:
                self.assertEqual(wrong.read_text(), "// Old output; no patch anchors.\n")
            # Reconfigure as the real build requires, then compile and link both
            # generated modules and a consumer of the generated header.
            subprocess.run(configure, check=True)
            subprocess.run(["cmake", "--build", str(build), "--target", "skate3", "--parallel", "2"], check=True)
            # Repeated generation exercises the helper's already-patched path.
            subprocess.run(["cmake", "--build", str(build), "--target", "generate-all"], check=True)

    def test_external_root_through_generate_and_build(self):
        self.exercise_generate_and_build(external_root=True)

    def test_default_root_through_generate_and_build(self):
        self.exercise_generate_and_build(external_root=False)

    def test_patch_helper_keeps_legacy_default(self):
        with tempfile.TemporaryDirectory(prefix="skate3 default ") as directory:
            source = pathlib.Path(directory)
            generated = source / "generated"
            generated.mkdir()
            fixture = generated / "skate3_recomp.fixture.cpp"
            fixture.write_text(MARKERS)
            subprocess.run(["cmake", f"-DSKATE3_SOURCE_DIR={source}", "-P",
                            str(ROOT / "cmake/ApplySkate3CodegenPatches.cmake")], check=True)
            self.assertEqual(fixture.read_text(), MARKERS)

    def test_empty_external_root_error_names_that_root(self):
        with tempfile.TemporaryDirectory(prefix="skate3 missing ") as directory:
            result = subprocess.run(
                ["cmake", f"-DSKATE3_SOURCE_DIR={ROOT}",
                 f"-DSKATE3_GENERATED_ROOT={directory}", "-P",
                 str(ROOT / "cmake/ApplySkate3CodegenPatches.cmake")],
                capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("No generated Skate 3 recompilation files found", result.stderr)
            self.assertIn(directory, result.stderr)


if __name__ == "__main__":
    unittest.main()
