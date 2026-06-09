if(NOT DEFINED SKATE3_SOURCE_DIR)
  message(FATAL_ERROR "SKATE3_SOURCE_DIR is required")
endif()

set(_skate3_recomp_39 "${SKATE3_SOURCE_DIR}/generated/skate3_recomp.39.cpp")
if(NOT EXISTS "${_skate3_recomp_39}")
  message(FATAL_ERROR "Expected generated file not found: ${_skate3_recomp_39}")
endif()

file(READ "${_skate3_recomp_39}" _contents)

if(NOT _contents MATCHES "#include \"skate3_ultrawide_guest\\.h\"")
  string(REPLACE
    "#include \"skate3_init.h\"\n"
    "#include \"skate3_init.h\"\n#include \"skate3_ultrawide_guest.h\"\n"
    _contents
    "${_contents}")
endif()

set(_frustum_call
"	// bl 0x827f2330
	ctx.lr = 0x827F2AE8;
	sub_827F2330(ctx, base);")

set(_frustum_patch
"	// bl 0x827f2330
	ctx.lr = 0x827F2AE8;
	Skate3UltrawideGameFrustumPatchScope skate3_ultrawide_game_frustum_patch_scope(
		ctx, base, ctx.r4.u32);
	sub_827F2330(ctx, base);")

if(_contents MATCHES "Skate3UltrawideGameFrustumPatchScope")
  message(STATUS "Skate 3 generated frustum patch already applied")
elseif(_contents MATCHES "sub_827F2330\\(ctx, base\\);")
  string(REPLACE "${_frustum_call}" "${_frustum_patch}" _patched "${_contents}")
  if(_patched STREQUAL _contents)
    message(FATAL_ERROR "Failed to apply Skate 3 generated frustum patch; expected call-site context changed")
  endif()
  set(_contents "${_patched}")
else()
  message(FATAL_ERROR "Failed to apply Skate 3 generated frustum patch; call to sub_827F2330 not found")
endif()

file(WRITE "${_skate3_recomp_39}" "${_contents}")
message(STATUS "Applied Skate 3 generated frustum patch")

set(_skate3_recomp_38 "${SKATE3_SOURCE_DIR}/generated/skate3_recomp.38.cpp")
if(NOT EXISTS "${_skate3_recomp_38}")
  message(FATAL_ERROR "Expected generated file not found: ${_skate3_recomp_38}")
endif()

file(READ "${_skate3_recomp_38}" _contents)

if(NOT _contents MATCHES "#include \"skate3_fov\\.h\"")
  string(REPLACE
    "#include \"skate3_init.h\"\n"
    "#include \"skate3_init.h\"\n#include \"skate3_fov.h\"\n"
    _contents
    "${_contents}")
endif()

set(_projection_fov_site
"DEFINE_REX_FUNC(sub_827DE960) {
	REX_FUNC_PROLOGUE();
	PPCRegister temp{};
	uint32_t ea{};")

set(_projection_fov_patch
"DEFINE_REX_FUNC(sub_827DE960) {
	REX_FUNC_PROLOGUE();
	PPCRegister temp{};
	uint32_t ea{};
	ctx.f1.f64 = double(Skate3MaybeOverrideProjectionFovRadians(float(ctx.f1.f64)));")

if(_contents MATCHES "Skate3MaybeOverrideProjectionFovRadians")
  message(STATUS "Skate 3 generated projection FOV patch already applied")
else()
  string(REPLACE "${_projection_fov_site}" "${_projection_fov_patch}" _patched "${_contents}")
  if(_patched STREQUAL _contents)
    message(FATAL_ERROR "Failed to apply Skate 3 generated projection FOV patch; expected function prologue changed")
  endif()
  set(_contents "${_patched}")
endif()

file(WRITE "${_skate3_recomp_38}" "${_contents}")
message(STATUS "Applied Skate 3 generated projection FOV patch")
