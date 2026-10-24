if(NOT DEFINED SKATE3_SOURCE_DIR)
  message(FATAL_ERROR "SKATE3_SOURCE_DIR is required")
endif()

if(NOT DEFINED SKATE3_GENERATED_ROOT)
  set(SKATE3_GENERATED_ROOT "${SKATE3_SOURCE_DIR}/generated")
endif()

file(GLOB _skate3_recomp_files
  LIST_DIRECTORIES false
  "${SKATE3_GENERATED_ROOT}/skate3_recomp.*.cpp")
if(NOT _skate3_recomp_files)
  message(FATAL_ERROR "No generated Skate 3 recompilation files found in ${SKATE3_GENERATED_ROOT}")
endif()

function(_skate3_add_include _contents_var _include)
  set(_contents "${${_contents_var}}")
  if(NOT _contents MATCHES "#include \"${_include}\"")
    string(REPLACE
      "#include \"skate3_init.h\"\n"
      "#include \"skate3_init.h\"\n#include \"${_include}\"\n"
      _contents
      "${_contents}")
  endif()
  set(${_contents_var} "${_contents}" PARENT_SCOPE)
endfunction()

set(_frustum_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "Skate3UltrawideGameFrustumPatchScope")
    set(_frustum_patched TRUE)
    break()
  endif()
  string(FIND "${_contents}" "ctx.r6.u64 = REX_LOAD_U32(ctx.r4.u32 + 5260);" _frustum_anchor)
  if(_frustum_anchor EQUAL -1)
    continue()
  endif()

  string(SUBSTRING "${_contents}" ${_frustum_anchor} 12000 _frustum_window)
  string(REGEX MATCH
    "\t// bl 0x[0-9a-fA-F]+\n\tctx\\.lr = 0x[0-9A-F]+;\n\tsub_[0-9A-F]+\\(ctx, base\\);"
    _frustum_call
    "${_frustum_window}")
  if(_frustum_call STREQUAL "")
    message(FATAL_ERROR "Failed to apply Skate 3 generated frustum patch; call near frustum anchor not found in ${_file}")
  endif()

  string(REGEX REPLACE
    "(\tctx\\.lr = 0x[0-9A-F]+;\n)"
    "\\1\tSkate3UltrawideGameFrustumPatchScope skate3_ultrawide_game_frustum_patch_scope(\n\t\tctx, base, ctx.r4.u32);\n"
    _frustum_patch
    "${_frustum_call}")
  string(REPLACE "${_frustum_call}" "${_frustum_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_ultrawide_guest.h")
  file(WRITE "${_file}" "${_contents}")
  set(_frustum_patched TRUE)
  message(STATUS "Applied Skate 3 generated frustum patch in ${_file}")
  break()
endforeach()
if(NOT _frustum_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 generated frustum patch; frustum anchor not found")
endif()

set(_fov_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "Skate3MaybeOverrideProjectionFovRadians")
    set(_fov_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "ctx\\.f27\\.f64 = ctx\\.f1\\.f64;")
    continue()
  endif()
  if(NOT _contents MATCHES "ctx\\.f4\\.f64 = double\\(float\\(ctx\\.f1\\.f64 \\* ctx\\.f0\\.f64\\)\\);")
    continue()
  endif()

  set(_projection_fov_site "ctx.f27.f64 = ctx.f1.f64;")
  set(_projection_fov_patch
"ctx.f1.f64 = double(Skate3MaybeOverrideProjectionFovRadians(float(ctx.f1.f64)));
	ctx.f27.f64 = ctx.f1.f64;")
  string(REPLACE "${_projection_fov_site}" "${_projection_fov_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_fov.h")
  file(WRITE "${_file}" "${_contents}")
  set(_fov_patched TRUE)
  message(STATUS "Applied Skate 3 generated projection FOV patch in ${_file}")
  break()
endforeach()
if(NOT _fov_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 generated projection FOV patch; projection FOV anchor not found")
endif()

set(_demo_path_movie_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "ShouldForceIntroMovieComplete")
    set(_demo_path_movie_patched TRUE)
    break()
  endif()

  set(_demo_path_movie_site
"	// bl 0x825d60c8
	ctx.lr = 0x825E05A0;
	sub_825D60C8(ctx, base);")
  if(NOT _contents MATCHES "ctx\\.lr = 0x825E05A0;")
    continue()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_825E0510\\)")
    continue()
  endif()
  string(FIND "${_contents}" "${_demo_path_movie_site}" _demo_path_movie_anchor)
  if(_demo_path_movie_anchor EQUAL -1)
    continue()
  endif()

  set(_demo_path_movie_patch
"	if (skate3::demo_path::ShouldForceIntroMovieComplete()) {
		ctx.r3.u64 = 0;
	} else {
		// bl 0x825d60c8
		ctx.lr = 0x825E05A0;
		sub_825D60C8(ctx, base);
	}")
  string(REPLACE "${_demo_path_movie_site}" "${_demo_path_movie_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_demo_path.h")
  file(WRITE "${_file}" "${_contents}")
  set(_demo_path_movie_patched TRUE)
  message(STATUS "Applied Skate 3 demo path intro movie patch in ${_file}")
  break()
endforeach()
if(NOT _demo_path_movie_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 demo path intro movie patch; FEMoviePlayer::Update anchor not found")
endif()
