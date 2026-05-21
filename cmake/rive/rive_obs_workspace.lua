-- Minimal premake5 entry point for the rive_obs_source plugin.
--
-- Pulls in only the rive-runtime static libraries we link against:
--   rive (core), rive_pls_renderer, rive_decoders, plus their transitive deps
--   (rive_harfbuzz, rive_sheenbidi, rive_yoga, miniaudio, libpng, libjpeg,
--    libwebp, zlib, luau_vm — the real Luau VM, since the build enables
--    --with_rive_scripting).
--
-- Skips path_fiddle / webgpu_player demo apps and glfw3.
--
-- Invoked via:
--   cd <rive-runtime>
--   build/build_rive.sh release --file=<abs path to this file> [arch + feature flags]
-- so the invocation CWD is rive-runtime root. Premake5's --file= changes its
-- internal CWD to this script's directory, so we use _WORKING_DIR (premake's
-- record of the original invocation directory) to recover rive root.
--
-- On Windows the build is driven through build_rive.ps1, which only resolves
-- `sh build_rive.sh` when CWD is rive-runtime's build/ dir. premake5 therefore
-- inherits _WORKING_DIR = <rive root>/build rather than <rive root>. Detect
-- that by checking for premake5_v2.lua and walk up one level if needed.

local rive_root = _WORKING_DIR
if not os.isfile(rive_root .. '/premake5_v2.lua') then
    rive_root = path.getabsolute(rive_root .. '/..')
end

-- rive core + transitive deps (harfbuzz, sheenbidi, miniaudio, yoga, luau_vm).
-- premake5_v2.lua does `path.getabsolute('dependencies/')`, so CWD = rive root.
os.chdir(rive_root)
dofile(rive_root .. '/premake5_v2.lua')

-- rive_decoders + libpng/libjpeg/libwebp/zlib. The decoders lua does
-- `path.getabsolute('../')` to locate rive root, so CWD must be `decoders/`.
os.chdir(rive_root .. '/decoders')
dofile(rive_root .. '/decoders/premake5_v2.lua')

-- rive_pls_renderer (Rive Renderer). Its lua does `path.getabsolute('..')`
-- relative to CWD, so CWD must be `renderer/`.
os.chdir(rive_root .. '/renderer')
dofile(rive_root .. '/renderer/premake5_pls_renderer.lua')

-- Restore CWD for any later premake actions.
os.chdir(rive_root)
