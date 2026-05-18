# Build & link rive-runtime as a set of static libraries vendored under
# `deps/rive-runtime`. rive-runtime uses premake5; we drive its own
# `build/build_rive.sh` (which self-bootstraps premake5 and dispatches to
# make/msbuild) from CMake.
#
# Exposes a single INTERFACE target, `rive::runtime`, that the plugin links
# against. It pulls in: rive (core), rive_pls_renderer, rive_decoders, and the
# vendored deps rive_harfbuzz, rive_sheenbidi, rive_yoga, libpng/libjpeg/libwebp/
# zlib, plus the per-platform GPU frameworks (Metal on macOS, D3D11 on Windows).

include_guard(GLOBAL)

set(RIVE_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/deps/rive-runtime")

if(NOT EXISTS "${RIVE_RUNTIME_DIR}/premake5_v2.lua")
  message(
    FATAL_ERROR
    "rive-runtime submodule is missing or empty at ${RIVE_RUNTIME_DIR}. "
    "Run: git submodule update --init --recursive"
  )
endif()

# All rive build artifacts land here, outside the submodule.
set(RIVE_BUILD_DIR "${CMAKE_BINARY_DIR}/rive-runtime-build")
set(RIVE_OUT_REL "rive-out")
set(RIVE_OUT_DIR "${RIVE_BUILD_DIR}/${RIVE_OUT_REL}")
set(RIVE_DEPS_CACHE "${RIVE_BUILD_DIR}/deps-cache")

# premake5 + make/msbuild produce platform-specific filenames.
# POSIX (gmake2): lib<project>.a in RIVE_OUT_DIR.
# Windows (vs2022): <project>.lib in RIVE_OUT_DIR (msbuild Premake config).
if(WIN32)
  set(_rive_lib_prefix "")
  set(_rive_lib_suffix ".lib")
else()
  set(_rive_lib_prefix "lib")
  set(_rive_lib_suffix ".a")
endif()

# Library files produced by the premake build, in link order
# (dependents first). Ordering matters for ld on Linux/macOS.
set(
  _rive_lib_names
  rive_pls_renderer
  rive
  rive_decoders
  rive_harfbuzz
  rive_sheenbidi
  rive_yoga
  libpng
  libjpeg
  libwebp
  zlib
  luau_vm
)

set(_rive_lib_files "")
foreach(_lib IN LISTS _rive_lib_names)
  list(APPEND _rive_lib_files "${RIVE_OUT_DIR}/${_rive_lib_prefix}${_lib}${_rive_lib_suffix}")
endforeach()

# Args we pass to build_rive.sh. The `release` token selects --config=release;
# the rest are forwarded to premake5 verbatim.
set(
  _rive_build_args
  "release"
  "--file=${CMAKE_SOURCE_DIR}/cmake/rive/rive_obs_workspace.lua"
  "--with_rive_text"
  "--with_rive_layout"
  "--with-pic"
)

if(APPLE)
  # OBS plugins on macOS ship as universal binaries (arm64 + x86_64).
  list(APPEND _rive_build_args "--arch=universal")
elseif(WIN32)
  list(APPEND _rive_build_args "--arch=x64")
endif()

# Custom command that drives the rive build. We redirect outputs outside the
# submodule via the RIVE_OUT env var, and pin the dependency download cache
# under our build dir via DEPENDENCIES. We invoke build_rive.sh from rive's
# root (its scripts assume CWD = rive root for path resolution) but pass a
# relative --out= so artifacts land in RIVE_OUT_DIR.
file(MAKE_DIRECTORY "${RIVE_BUILD_DIR}")
file(MAKE_DIRECTORY "${RIVE_DEPS_CACHE}")

# Compute path from rive-runtime root to our RIVE_OUT_DIR for the relative
# --out= flag (premake's rive_build_config.lua does
# `_WORKING_DIR .. '/' .. _OPTIONS['out']`, so an absolute out path produces
# a malformed concatenation; a relative path works cleanly).
file(RELATIVE_PATH _rive_out_rel "${RIVE_RUNTIME_DIR}" "${RIVE_OUT_DIR}")

# build_rive.sh refuses to build into an existing $RIVE_OUT that lacks a
# `.rive_premake_args` stamp file. CMake (Xcode, Ninja, MSBuild) pre-creates
# the OUTPUT parent dirs, so we run a tiny cmake -P script before delegating
# to clear any stale empty output dir.
set(_rive_clean_script "${RIVE_BUILD_DIR}/clean_stale_rive_out.cmake")
file(
  WRITE "${_rive_clean_script}"
  "if(EXISTS \"\${RIVE_OUT}\" AND NOT EXISTS \"\${RIVE_OUT}/.rive_premake_args\")
  message(STATUS \"Removing stale rive output dir: \${RIVE_OUT}\")
  file(REMOVE_RECURSE \"\${RIVE_OUT}\")
endif()
"
)

if(WIN32)
  set(_rive_build_script "${RIVE_RUNTIME_DIR}/build/build_rive.ps1")
  add_custom_command(
    OUTPUT ${_rive_lib_files}
    COMMAND ${CMAKE_COMMAND} -DRIVE_OUT=${RIVE_OUT_DIR} -P "${_rive_clean_script}"
    COMMAND
      ${CMAKE_COMMAND} -E env "RIVE_OUT=${_rive_out_rel}" "DEPENDENCIES=${RIVE_DEPS_CACHE}" powershell.exe
      -ExecutionPolicy Bypass -File "${_rive_build_script}" ${_rive_build_args}
    WORKING_DIRECTORY "${RIVE_RUNTIME_DIR}"
    COMMENT "Building rive-runtime static libraries (premake5 + msbuild)"
    VERBATIM
    USES_TERMINAL
  )
else()
  set(_rive_build_script "${RIVE_RUNTIME_DIR}/build/build_rive.sh")
  add_custom_command(
    OUTPUT ${_rive_lib_files}
    COMMAND ${CMAKE_COMMAND} -DRIVE_OUT=${RIVE_OUT_DIR} -P "${_rive_clean_script}"
    COMMAND
      ${CMAKE_COMMAND} -E env "RIVE_OUT=${_rive_out_rel}" "DEPENDENCIES=${RIVE_DEPS_CACHE}" bash "${_rive_build_script}"
      ${_rive_build_args}
    WORKING_DIRECTORY "${RIVE_RUNTIME_DIR}"
    COMMENT "Building rive-runtime static libraries (premake5 + make)"
    VERBATIM
    USES_TERMINAL
  )
endif()

add_custom_target(rive_runtime_build DEPENDS ${_rive_lib_files})

# IMPORTED targets for each static lib.
foreach(_lib IN LISTS _rive_lib_names)
  set(_target "rive::${_lib}")
  add_library(${_target} STATIC IMPORTED GLOBAL)
  set_target_properties(
    ${_target}
    PROPERTIES IMPORTED_LOCATION "${RIVE_OUT_DIR}/${_rive_lib_prefix}${_lib}${_rive_lib_suffix}"
  )
  add_dependencies(${_target} rive_runtime_build)
endforeach()

# INTERFACE umbrella target. Consumers link this; CMake's transitive linking
# will pull in every rive::* static lib plus the platform system libs.
add_library(rive::runtime INTERFACE IMPORTED GLOBAL)

target_include_directories(
  rive::runtime
  INTERFACE "${RIVE_RUNTIME_DIR}/include" "${RIVE_RUNTIME_DIR}/renderer/include" "${RIVE_RUNTIME_DIR}/decoders/include"
)

# Static-lib link order matters on ld (dependents first). Renderer pulls in
# rive core; rive core pulls in harfbuzz/sheenbidi/yoga.
target_link_libraries(
  rive::runtime
  INTERFACE
    rive::rive_pls_renderer
    rive::rive
    rive::rive_decoders
    rive::rive_harfbuzz
    rive::rive_sheenbidi
    rive::rive_yoga
    rive::libpng
    rive::libjpeg
    rive::libwebp
    rive::zlib
    rive::luau_vm
)

if(APPLE)
  # Metal backend pulls in these Apple frameworks. CoreText is used by rive's
  # harfbuzz font backend on Apple platforms; ImageIO is used by rive_decoders
  # for PNG/JPEG decode via CGImageSource.
  target_link_libraries(
    rive::runtime
    INTERFACE
      "-framework Foundation"
      "-framework Metal"
      "-framework QuartzCore"
      "-framework IOSurface"
      "-framework CoreGraphics"
      "-framework CoreText"
      "-framework CoreFoundation"
      "-framework ImageIO"
  )
elseif(WIN32)
  target_link_libraries(rive::runtime INTERFACE d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib)
endif()
