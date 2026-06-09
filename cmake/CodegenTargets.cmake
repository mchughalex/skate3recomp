add_custom_target(generate-skate3
    COMMAND $<TARGET_FILE:rex::rexglue> codegen
            "${CMAKE_CURRENT_BINARY_DIR}/manifests/skate3.toml"
    COMMAND "${CMAKE_COMMAND}"
            "-DSKATE3_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplySkate3CodegenPatches.cmake"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating recompiled code for default.xex"
    VERBATIM
)

add_custom_target(generate-eawebkit
    COMMAND $<TARGET_FILE:rex::rexglue> codegen
            "${CMAKE_CURRENT_BINARY_DIR}/manifests/eawebkit.toml"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating recompiled code for EAWebkit.xex"
    VERBATIM
)

add_dependencies(generate-skate3 generate-eawebkit)

add_custom_target(generate-all
    DEPENDS generate-skate3 generate-eawebkit
)

add_custom_target(skate3_codegen DEPENDS generate-skate3)
add_custom_target(eawebkit_codegen DEPENDS generate-eawebkit)
add_custom_target(all_codegen DEPENDS generate-all)
