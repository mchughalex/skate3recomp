cmake_minimum_required(VERSION 3.25)

project(skate3 VERSION 1.0.1 LANGUAGES CXX)

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

if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
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
endif()

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" SKATE3_SOURCE_DIR_CONFIG)
file(TO_CMAKE_PATH "${SKATE3_GAME_DATA_ROOT}" SKATE3_GAME_DATA_ROOT_CONFIG)

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

include(cmake/RexGlue.cmake)
include(cmake/CodegenTargets.cmake)

set(SKATE3_COMMON_SOURCES
    src/exception_compat.cpp
    src/skate3_app_common.cpp
    src/skate3_fov.cpp
    src/skate3_iso_installer.cpp
    src/skate3_user_settings.cpp
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
else()
    add_executable(skate3 ${SKATE3_SOURCES})
endif()

target_include_directories(skate3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/include
)
target_compile_definitions(skate3 PRIVATE SKATE3_BUILD_CONFIG="$<CONFIG>")

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
    target_link_libraries(skate3 PRIVATE comdlg32)
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

if(MSVC)
    target_compile_options(skate3 PRIVATE /utf-8)
else()
    target_compile_options(skate3 PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()
