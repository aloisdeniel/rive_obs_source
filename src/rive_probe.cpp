// Link probe: pulls a non-inline rive symbol so the linker is forced to resolve
// it against librive.a (and the rest of the rive::runtime archive set).
// Touches the renderer header too so renderer symbols are reached at link time.

#include <rive/file.hpp>
#include <rive/renderer/render_context.hpp>

#include <cstdint>

// Take the address of a non-inline rive function so the symbol survives
// dead-code elimination. Returns void* to keep this exportable to C.
extern "C" void *rive_obs_link_probe(void)
{
	using ImportFn = rive::rcp<rive::File> (*)(rive::Span<const uint8_t>,
	                                           rive::Factory *,
	                                           rive::ImportResult *,
	                                           rive::rcp<rive::FileAssetLoader>,
	                                           rive::ScriptingVM *);
	ImportFn fn = &rive::File::import;
	return reinterpret_cast<void *>(fn);
}
