cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

option(SKATE3_STATIC_MSVC_RUNTIME
    "Link Windows MSVC-ABI builds with the static Visual C++ runtime"
    ON)
if(SKATE3_STATIC_MSVC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(skate3 VERSION 2.0.0 LANGUAGES CXX)

if(WIN32)
    enable_language(RC)
endif()
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/Skate3Version.cmake)

skate3_resolve_version(SKATE3_FULL_VERSION
    FLOOR_VERSION ${PROJECT_VERSION}
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
string(REGEX REPLACE "-.*$" "" SKATE3_NUMERIC_VERSION "${SKATE3_FULL_VERSION}")

# Four-part comma-separated form for Windows VERSIONINFO resources.
string(REPLACE "." ";" SKATE3_RC_VERSION_PARTS "${SKATE3_NUMERIC_VERSION}")
list(LENGTH SKATE3_RC_VERSION_PARTS SKATE3_RC_VERSION_PART_COUNT)
while(SKATE3_RC_VERSION_PART_COUNT LESS 4)
    list(APPEND SKATE3_RC_VERSION_PARTS 0)
    list(LENGTH SKATE3_RC_VERSION_PARTS SKATE3_RC_VERSION_PART_COUNT)
endwhile()
list(JOIN SKATE3_RC_VERSION_PARTS "," SKATE3_RC_VERSION)

set(SKATE3_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES SKATE3_OSX_ARCH_COUNT)
    if(SKATE3_OSX_ARCH_COUNT EQUAL 1)
        set(SKATE3_TARGET_PROCESSOR "${CMAKE_OSX_ARCHITECTURES}")
    endif()
endif()

if(WIN32)
    if(SKATE3_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
        set(SKATE3_BUILD_PLATFORM "win-amd64")
    elseif(SKATE3_TARGET_PROCESSOR MATCHES "aarch64|ARM64")
        set(SKATE3_BUILD_PLATFORM "win-arm64")
    else()
        set(SKATE3_BUILD_PLATFORM "win-${SKATE3_TARGET_PROCESSOR}")
    endif()
elseif(APPLE)
    if(SKATE3_TARGET_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(SKATE3_BUILD_PLATFORM "macos-arm64")
    else()
        message(FATAL_ERROR "Unsupported macOS architecture: ${SKATE3_TARGET_PROCESSOR}. macOS builds are ARM64-only.")
    endif()
elseif(UNIX AND NOT APPLE)
    if(SKATE3_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
        set(SKATE3_BUILD_PLATFORM "linux-amd64")
    elseif(SKATE3_TARGET_PROCESSOR MATCHES "aarch64|ARM64")
        set(SKATE3_BUILD_PLATFORM "linux-arm64")
    else()
        set(SKATE3_BUILD_PLATFORM "linux-${SKATE3_TARGET_PROCESSOR}")
    endif()
else()
    set(SKATE3_BUILD_PLATFORM "${CMAKE_SYSTEM_NAME}-${SKATE3_TARGET_PROCESSOR}")
endif()

string(TIMESTAMP SKATE3_BUILD_TIMESTAMP "%Y%m%d_%H%M")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/skate3_version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/skate3_version.h
    @ONLY
)

set(REXSDK_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rexglue-sdk"
    CACHE PATH "Path to the rexglue SDK source tree")
set(SKATE3_GAME_DATA_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/game"
    CACHE PATH "Path to an extracted Skate 3 retail game dump")
set(SKATE3_TITLE_UPDATE_PACKAGE
    ""
    CACHE FILEPATH "Optional Skate 3 title update STFS package used for codegen")
option(SKATE3_ENABLE_X86_64_V3
    "Build x86_64 desktop binaries for x86-64-v3 CPUs (AVX2/BMI2)"
    OFF)

if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    if(SKATE3_ENABLE_X86_64_V3)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            add_compile_options(-march=x86-64-v3)
            set(SNAPPY_REQUIRE_AVX2 ON CACHE BOOL
                "Match Snappy feature detection to the project x86-64-v3 target" FORCE)
            set(SNAPPY_HAVE_SSSE3 1 CACHE INTERNAL
                "x86-64-v3 requires SSSE3; Snappy checks do not see directory compile options" FORCE)
            set(SNAPPY_HAVE_X86_CRC32 1 CACHE INTERNAL
                "x86-64-v3 requires SSE4.2 CRC32; Snappy checks do not see directory compile options" FORCE)
            set(SNAPPY_HAVE_BMI2 1 CACHE INTERNAL
                "x86-64-v3 requires BMI2; Snappy checks do not see directory compile options" FORCE)
        elseif(MSVC)
            add_compile_options(/arch:AVX2)
        endif()
    else()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            add_compile_options(-march=x86-64 -mtune=generic -mssse3)
        endif()
        set(SNAPPY_REQUIRE_AVX OFF CACHE BOOL
            "Do not require AVX for non-AVX x86_64 release builds" FORCE)
        set(SNAPPY_REQUIRE_AVX2 OFF CACHE BOOL
            "Do not require AVX2 for non-AVX x86_64 release builds" FORCE)
        set(SNAPPY_HAVE_SSSE3 0 CACHE INTERNAL
            "Keep Snappy on its scalar path for non-AVX x86_64 release builds" FORCE)
        set(SNAPPY_HAVE_X86_CRC32 0 CACHE INTERNAL
            "Non-AVX x86_64 release builds must not require SSE4.2 CRC32" FORCE)
        set(SNAPPY_HAVE_BMI2 0 CACHE INTERNAL
            "Non-AVX x86_64 release builds must not require BMI2" FORCE)
    endif()
else()
    set(SNAPPY_REQUIRE_AVX OFF CACHE BOOL
        "Do not require AVX for generic release builds" FORCE)
    set(SNAPPY_REQUIRE_AVX2 OFF CACHE BOOL
        "Do not require AVX2 for generic release builds" FORCE)
    set(SNAPPY_HAVE_SSSE3 0 CACHE INTERNAL
        "Generic release builds must not require SSSE3" FORCE)
    set(SNAPPY_HAVE_X86_CRC32 0 CACHE INTERNAL
        "Generic release builds must not require SSE4.2 CRC32" FORCE)
    set(SNAPPY_HAVE_BMI2 0 CACHE INTERNAL
        "Generic release builds must not require BMI2" FORCE)
endif()

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" SKATE3_SOURCE_DIR_CONFIG)
file(TO_CMAKE_PATH "${SKATE3_GAME_DATA_ROOT}" SKATE3_GAME_DATA_ROOT_CONFIG)

set(SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE "${SKATE3_TITLE_UPDATE_PACKAGE}")
if(SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE STREQUAL "")
    set(SKATE3_LOCAL_TITLE_UPDATE_PACKAGE
        "${CMAKE_CURRENT_SOURCE_DIR}/TU_12K2276_000000C000000.00000000000O3")
    if(EXISTS "${SKATE3_LOCAL_TITLE_UPDATE_PACKAGE}")
        set(SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE "${SKATE3_LOCAL_TITLE_UPDATE_PACKAGE}")
    endif()
endif()

set(SKATE3_PATCHED_DEFAULT_XEX_LINE "")
set(SKATE3_PATCHED_EAWEBKIT_XEX_LINE "")
set(SKATE3_CODEGEN_ARGS "")
set(SKATE3_FUNCTION_CONFIG_INCLUDES
    "  \"${SKATE3_SOURCE_DIR_CONFIG}/config/skate3_functions.toml\",")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(NOT SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE STREQUAL "")
    if(NOT EXISTS "${SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE}")
        message(FATAL_ERROR
            "SKATE3_TITLE_UPDATE_PACKAGE does not exist: ${SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE}")
    endif()

    set(SKATE3_TITLE_UPDATE_CODEGEN_ROOT
        "${CMAKE_CURRENT_BINARY_DIR}/title_update_codegen")
    set(SKATE3_TITLE_UPDATE_DEFAULT_XEX
        "${SKATE3_TITLE_UPDATE_CODEGEN_ROOT}/default.xex")
    set(SKATE3_TITLE_UPDATE_DEFAULT_XEXP
        "${SKATE3_TITLE_UPDATE_CODEGEN_ROOT}/default.xexp")
    set(SKATE3_TITLE_UPDATE_EAWEBKIT_XEX
        "${SKATE3_TITLE_UPDATE_CODEGEN_ROOT}/data/webkit/EAWebkit.xex")
    set(SKATE3_TITLE_UPDATE_EAWEBKIT_XEXP
        "${SKATE3_TITLE_UPDATE_CODEGEN_ROOT}/data/webkit/EAWebkit.xexp")

    add_custom_command(
        OUTPUT
            "${SKATE3_TITLE_UPDATE_DEFAULT_XEX}"
            "${SKATE3_TITLE_UPDATE_DEFAULT_XEXP}"
            "${SKATE3_TITLE_UPDATE_EAWEBKIT_XEX}"
            "${SKATE3_TITLE_UPDATE_EAWEBKIT_XEXP}"
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExtractTitleUpdateXexp.py"
            --tu "${SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE}"
            --game-root "${SKATE3_GAME_DATA_ROOT}"
            --out-root "${SKATE3_TITLE_UPDATE_CODEGEN_ROOT}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExtractTitleUpdateXexp.py"
            "${SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE}"
            "${SKATE3_GAME_DATA_ROOT}/default.xex"
            "${SKATE3_GAME_DATA_ROOT}/data/webkit/EAWebkit.xex"
        COMMENT "Staging Skate 3 title update patches for codegen"
        VERBATIM
    )
    add_custom_target(skate3-title-update-codegen-inputs
        DEPENDS
            "${SKATE3_TITLE_UPDATE_DEFAULT_XEX}"
            "${SKATE3_TITLE_UPDATE_DEFAULT_XEXP}"
            "${SKATE3_TITLE_UPDATE_EAWEBKIT_XEX}"
            "${SKATE3_TITLE_UPDATE_EAWEBKIT_XEXP}"
    )

    file(TO_CMAKE_PATH "${SKATE3_TITLE_UPDATE_DEFAULT_XEX}"
        SKATE3_TITLE_UPDATE_DEFAULT_XEX_CONFIG)
    file(TO_CMAKE_PATH "${SKATE3_TITLE_UPDATE_EAWEBKIT_XEX}"
        SKATE3_TITLE_UPDATE_EAWEBKIT_XEX_CONFIG)
    set(SKATE3_PATCHED_DEFAULT_XEX_LINE
        "patched_file_path = \"${SKATE3_TITLE_UPDATE_DEFAULT_XEX_CONFIG}\"")
    set(SKATE3_PATCHED_EAWEBKIT_XEX_LINE
        "patched_file_path = \"${SKATE3_TITLE_UPDATE_EAWEBKIT_XEX_CONFIG}\"")
    set(SKATE3_FUNCTION_CONFIG_INCLUDES
        "  \"${SKATE3_SOURCE_DIR_CONFIG}/config/skate3_tu_functions.toml\",")
    message(STATUS
        "Skate 3 title update codegen enabled: ${SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE}")
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/manifests")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/manifests/skate3.toml.in
    ${CMAKE_CURRENT_BINARY_DIR}/manifests/skate3.toml
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/manifests/eawebkit.toml.in
    ${CMAKE_CURRENT_BINARY_DIR}/manifests/eawebkit.toml
    @ONLY
)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Disable Tracy instrumentation for normal Skate 3 builds" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "Enable lightweight rexglue perf counter instrumentation")

if(WIN32)
    set(REXGLUE_RUNTIME_RC_VERSION "${SKATE3_RC_VERSION}")
    set(REXGLUE_RUNTIME_RC_VERSION_STRING "${SKATE3_FULL_VERSION}")
    set(REXGLUE_RUNTIME_RC_PRODUCT_NAME "Skate3Recomp")
    set(REXGLUE_RUNTIME_RC_COMPANY "Skate3Recomp project")
    set(REXGLUE_RUNTIME_RC_DESCRIPTION "Skate3Recomp guest runtime")
    set(REXGLUE_RUNTIME_RC_COPYRIGHT "(c) Skate3Recomp contributors")
endif()

include(cmake/RexGlue.cmake)
include(cmake/CodegenTargets.cmake)

set(SKATE3_COMMON_SOURCES
    src/exception_compat.cpp
    src/skate3_app_common.cpp
    src/skate3_demo_path.cpp
    src/skate3_draw_distance.cpp
    src/skate3_fov.cpp
    src/skate3_heap_check.cpp
    src/skate3_iso_installer.cpp
    src/native/skate3_native_diagnostics.cpp
    src/native/skate3_native_entity.cpp
    src/native/skate3_native_guest_read.cpp
    src/native/skate3_native_lw.cpp
    src/native/skate3_native_palette.cpp
    src/skate3_native_debug_dialog.cpp
    src/skate3_native_render.cpp
    src/skate3_native_scene.cpp
    src/skate3_native_scene_gpu.cpp
    src/skate3_native_scene_post.cpp
    src/skate3_screenshot.cpp
    src/skate3_shader_disasm.cpp
    src/skate3_title_update_installer.cpp
    src/skate3_user_settings.cpp
    src/skate3_win_icon.cpp
    third_party/rexglue-sdk/thirdparty/crypto/sha256.cpp
)
if(APPLE)
    list(APPEND SKATE3_COMMON_SOURCES src/skate3_iso_installer_macos.mm)
endif()

set(SKATE3_SOURCES
    src/main.cpp
    ${SKATE3_COMMON_SOURCES}
)

if(WIN32)
    add_executable(skate3 WIN32 ${SKATE3_SOURCES})
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/skate3_app.manifest
        ${CMAKE_CURRENT_BINARY_DIR}/skate3_app.manifest
        COPYONLY
    )
    target_link_options(skate3 PRIVATE LINKER:/MANIFESTINPUT:skate3_app.manifest)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/skate3.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/skate3.rc
        @ONLY
    )
    target_sources(skate3 PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/skate3.rc)
else()
    add_executable(skate3 ${SKATE3_SOURCES})
endif()

target_include_directories(skate3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${REXSDK_DIR}/thirdparty/xxHash
)

set(SKATE3_NATIVE_SHADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/scene.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/shadow_blur.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/resolve.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/blur.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/outline.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/overlay2d.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/spline.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/photo_fx.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/ssao.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/hdr.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/ssr.hlsl
)
# Shared .hlsli headers, inlined into the embedded sources by
# EmbedShaders.cmake: the embed must rerun when any of them changes.
file(GLOB SKATE3_NATIVE_SHADER_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/*.hlsli)
set(SKATE3_NATIVE_SHADER_HEADER
    ${CMAKE_CURRENT_BINARY_DIR}/include/skate3_native_shaders.h)
add_custom_command(
    OUTPUT ${SKATE3_NATIVE_SHADER_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DSHADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders
        -DOUT=${SKATE3_NATIVE_SHADER_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
    DEPENDS ${SKATE3_NATIVE_SHADER_SOURCES}
        ${SKATE3_NATIVE_SHADER_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
    COMMENT "Embedding native renderer shaders"
    VERBATIM)
target_sources(skate3 PRIVATE ${SKATE3_NATIVE_SHADER_HEADER})
target_compile_definitions(skate3 PRIVATE SKATE3_BUILD_CONFIG="$<CONFIG>")
if(NOT SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE STREQUAL "")
    target_compile_definitions(skate3 PRIVATE SKATE3_HAS_TITLE_UPDATE=1)
else()
    target_compile_definitions(skate3 PRIVATE SKATE3_HAS_TITLE_UPDATE=0)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/sources.cmake")
    include(generated/sources.cmake)
    target_sources(skate3 PRIVATE ${GENERATED_SOURCES})
    target_include_directories(skate3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated)
else()
    message(STATUS "generated/sources.cmake not found; run the generate-all target before building skate3")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/eawebkit/sources.cmake")
    set(_SKATE3_SAVED_GENERATED_SOURCES ${GENERATED_SOURCES})
    include(generated/eawebkit/sources.cmake)
    target_sources(skate3 PRIVATE ${GENERATED_SOURCES})
    target_include_directories(skate3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated/eawebkit)
    set(GENERATED_SOURCES ${_SKATE3_SAVED_GENERATED_SOURCES})
    unset(_SKATE3_SAVED_GENERATED_SOURCES)
else()
    message(STATUS "generated/eawebkit/sources.cmake not found; run the generate-all target before building skate3")
endif()

target_link_libraries(skate3 PRIVATE rex::runtime)
rexglue_configure_target(skate3)

if(APPLE)
    target_link_options(skate3 PRIVATE -Wl,-stack_size,0x4000000)
    target_link_libraries(skate3 PRIVATE "-framework Cocoa")
endif()

if(WIN32)
    target_link_libraries(skate3 PRIVATE comdlg32 winhttp d3dcompiler gdiplus)
endif()

add_custom_target(skate3_stage_runtime_library
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:skate3>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:rexruntime>"
        "$<TARGET_FILE_DIR:skate3>"
    DEPENDS rexruntime
    VERBATIM
)
add_dependencies(skate3 skate3_stage_runtime_library)

if(NOT SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE STREQUAL "")
    add_custom_target(skate3_stage_title_update_patches
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:skate3>/game/data/webkit"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SKATE3_TITLE_UPDATE_DEFAULT_XEXP}"
            "$<TARGET_FILE_DIR:skate3>/game/default.xexp"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SKATE3_TITLE_UPDATE_EAWEBKIT_XEXP}"
            "$<TARGET_FILE_DIR:skate3>/game/data/webkit/EAWebkit.xexp"
        DEPENDS skate3-title-update-codegen-inputs
        VERBATIM
    )
    add_dependencies(skate3 skate3_stage_title_update_patches)
endif()

if(MSVC)
    target_compile_options(skate3 PRIVATE /utf-8)
else()
    target_compile_options(skate3 PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()
