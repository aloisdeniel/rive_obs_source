/*
 * rive_renderer_mac: Rive Renderer (Metal backend) bridged to libobs via an
 * IOSurface-backed MTLTexture.
 *
 * Pipeline per frame:
 *   1. State machine is advanced by dt.
 *   2. RenderContextMetalImpl::beginFrame() with the current width/height
 *      and clear color.
 *   3. The target's IOSurface-backed MTLTexture is bound to the
 *      RenderTargetMetal.
 *   4. A RiveRenderer draws the artboard into the context, applying the
 *      computeAlignment() matrix for the chosen fit + alignment.
 *   5. RenderContext::flush() encodes Rive's GPU work onto an external
 *      MTLCommandBuffer that we created from our queue.
 *   6. The command buffer is committed and we wait for completion so OBS's GL
 *      side can safely sample the IOSurface as a GL_TEXTURE_RECTANGLE.
 *
 * The libobs side wraps the same IOSurface with gs_texture_create_from_iosurface;
 * that texture is what video_render() ultimately draws.
 */

#include "rive_renderer.h"
#include "rive_scene_view_model.h"
#include "rive_custom_data.h"
#include "rive_image_cache.h"

// Rive headers come first because <obs.h>'s graphics/math-defs.h defines
// EPSILON as a macro that would otherwise clobber rive::math::EPSILON.
#include <rive/file.hpp>
#include <rive/artboard.hpp>
#include <rive/layout.hpp>
#include <rive/animation/state_machine_instance.hpp>
#include <rive/animation/state_machine_input_instance.hpp>
#include <rive/viewmodel/runtime/viewmodel_runtime.hpp>
#include <rive/renderer/render_context.hpp>
#include <rive/renderer/rive_renderer.hpp>
#include <rive/renderer/metal/render_context_metal_impl.h>

#include <obs.h>
#include <plugin-support.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

rive::Fit parse_fit(const char *value)
{
	if (!value)
		return rive::Fit::contain;
	if (std::strcmp(value, "fill") == 0)
		return rive::Fit::fill;
	if (std::strcmp(value, "cover") == 0)
		return rive::Fit::cover;
	if (std::strcmp(value, "fit-width") == 0)
		return rive::Fit::fitWidth;
	if (std::strcmp(value, "fit-height") == 0)
		return rive::Fit::fitHeight;
	if (std::strcmp(value, "none") == 0)
		return rive::Fit::none;
	if (std::strcmp(value, "scale-down") == 0)
		return rive::Fit::scaleDown;
	return rive::Fit::contain;
}

rive::Alignment parse_alignment(const char *value)
{
	if (!value)
		return rive::Alignment::center;
	if (std::strcmp(value, "top-left") == 0)
		return rive::Alignment::topLeft;
	if (std::strcmp(value, "top-center") == 0)
		return rive::Alignment::topCenter;
	if (std::strcmp(value, "top-right") == 0)
		return rive::Alignment::topRight;
	if (std::strcmp(value, "center-left") == 0)
		return rive::Alignment::centerLeft;
	if (std::strcmp(value, "center-right") == 0)
		return rive::Alignment::centerRight;
	if (std::strcmp(value, "bottom-left") == 0)
		return rive::Alignment::bottomLeft;
	if (std::strcmp(value, "bottom-center") == 0)
		return rive::Alignment::bottomCenter;
	if (std::strcmp(value, "bottom-right") == 0)
		return rive::Alignment::bottomRight;
	return rive::Alignment::center;
}

// OBS persists colors as 0xAABBGGRR (byte 0 = R, byte 3 = A). Rive's ColorInt
// is 0xAARRGGBB. Swap the byte order of the RGB channels while leaving alpha
// alone.
rive::ColorInt obs_color_to_rive(uint32_t bg)
{
	const uint32_t r = (bg >> 0) & 0xff;
	const uint32_t g = (bg >> 8) & 0xff;
	const uint32_t b = (bg >> 16) & 0xff;
	const uint32_t a = (bg >> 24) & 0xff;
	return (a << 24) | (r << 16) | (g << 8) | b;
}

void set_err(char *err, size_t err_size, const char *msg)
{
	if (!err || err_size == 0)
		return;
	std::snprintf(err, err_size, "%s", msg);
}

bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
	std::FILE *fp = std::fopen(path.c_str(), "rb");
	if (!fp)
		return false;
	if (std::fseek(fp, 0, SEEK_END) != 0) {
		std::fclose(fp);
		return false;
	}
	long size = std::ftell(fp);
	if (size <= 0) {
		std::fclose(fp);
		return false;
	}
	std::rewind(fp);
	out.resize(static_cast<size_t>(size));
	size_t read = std::fread(out.data(), 1, out.size(), fp);
	std::fclose(fp);
	return read == out.size();
}

} // namespace

struct rive_renderer {
	// Output size
	uint32_t width = 0;
	uint32_t height = 0;

	// Metal state
	id<MTLDevice> device = nil;
	id<MTLCommandQueue> queue = nil;

	std::unique_ptr<rive::gpu::RenderContext> renderContext;
	rive::rcp<rive::gpu::RenderTargetMetal> renderTarget;

	// Shared GPU resource bridged to OBS.
	IOSurfaceRef iosurface = nullptr;
	id<MTLTexture> mtlTexture = nil;
	gs_texture_t *gsTexture = nullptr;

	// Loaded Rive content.
	rive::rcp<rive::File> file;
	std::unique_ptr<rive::ArtboardInstance> artboard;
	std::unique_ptr<rive::StateMachineInstance> stateMachine;

	// SceneViewModel binding. Null when the file doesn't declare a view
	// model named "SceneViewModel"; we silently no-op scene-state pushes
	// in that case.
	std::unique_ptr<rive_obs::SceneViewModelBinding> sceneViewModel;

	// Custom-data binding. Null when the file has neither a SceneViewModel
	// nor a default artboard view model — there's nothing to feed.
	std::unique_ptr<rive_obs::CustomDataBinding> customData;

	// Resolves URL/file-path strings on Rive image properties to decoded
	// RenderImages. Held as a member of the renderer (and not the binding)
	// so the cache survives state-machine rebinds and so we can tick it
	// from the right thread.
	std::unique_ptr<rive_obs::ImageCache> imageCache;

	// Cached current selection so we can detect drift between calls.
	std::string loadedPath;
	std::string loadedArtboard;
	std::string loadedStateMachine;
};

namespace {

void release_target_resources(rive_renderer *r)
{
	if (r->gsTexture) {
		gs_texture_destroy(r->gsTexture);
		r->gsTexture = nullptr;
	}
	r->mtlTexture = nil;
	r->renderTarget.reset();
	if (r->iosurface) {
		CFRelease(r->iosurface);
		r->iosurface = nullptr;
	}
}

bool allocate_target_resources(rive_renderer *r, char *err, size_t err_size)
{
	if (r->width == 0 || r->height == 0) {
		set_err(err, err_size, "renderer width/height not set");
		return false;
	}

	// 'BGRA' four-char code matches kCVPixelFormatType_32BGRA and is what
	// libobs's gs_texture_create_from_iosurface accepts on macOS.
	const uint32_t bgra = ('B' << 24) | ('G' << 16) | ('R' << 8) | 'A';
	NSDictionary *props = @{
		(NSString *)kIOSurfaceWidth : @(r->width),
		(NSString *)kIOSurfaceHeight : @(r->height),
		(NSString *)kIOSurfaceBytesPerElement : @4,
		(NSString *)kIOSurfacePixelFormat : @(bgra),
	};

	IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
	if (!surface) {
		set_err(err, err_size, "IOSurfaceCreate failed");
		return false;
	}

	MTLTextureDescriptor *desc =
		[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
								   width:r->width
								  height:r->height
							       mipmapped:NO];
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModeShared;

	id<MTLTexture> tex = [r->device newTextureWithDescriptor:desc iosurface:surface plane:0];
	if (!tex) {
		CFRelease(surface);
		set_err(err, err_size, "newTextureWithDescriptor:iosurface failed");
		return false;
	}

	gs_texture_t *gs_tex = gs_texture_create_from_iosurface(surface);
	if (!gs_tex) {
		CFRelease(surface);
		set_err(err, err_size, "gs_texture_create_from_iosurface failed");
		return false;
	}

	auto *impl = r->renderContext->static_impl_cast<rive::gpu::RenderContextMetalImpl>();
	auto rt = impl->makeRenderTarget(MTLPixelFormatBGRA8Unorm, r->width, r->height);
	if (!rt) {
		gs_texture_destroy(gs_tex);
		CFRelease(surface);
		set_err(err, err_size, "makeRenderTarget failed");
		return false;
	}

	r->iosurface = surface;
	r->mtlTexture = tex;
	r->gsTexture = gs_tex;
	r->renderTarget = std::move(rt);
	return true;
}

void clear_content(rive_renderer *r)
{
	// The bindings hold rcps on runtime instances derived from the file;
	// drop them before the file goes away. Drop the custom-data binding
	// first since it may share the SceneViewModel root.
	r->customData.reset();
	r->sceneViewModel.reset();
	r->stateMachine.reset();
	r->artboard.reset();
	r->file.reset();
	// The image cache holds RenderImages produced via the previous file's
	// RenderContext-decoded bytes; new content gets a fresh cache so we
	// don't try to mix images across reloads.
	r->imageCache.reset();
	r->loadedPath.clear();
	r->loadedArtboard.clear();
	r->loadedStateMachine.clear();
}

bool load_content(rive_renderer *r, const std::string &path, const std::string &artboard_name,
		  const std::string &sm_name, char *err, size_t err_size)
{
	std::vector<uint8_t> bytes;
	if (!read_file(path, bytes)) {
		set_err(err, err_size, "could not read .riv file");
		return false;
	}

	rive::ImportResult result = rive::ImportResult::malformed;
	rive::rcp<rive::File> file =
		rive::File::import(rive::Span<const uint8_t>(bytes.data(), bytes.size()),
				   r->renderContext.get(), &result);
	if (!file || result != rive::ImportResult::success) {
		set_err(err, err_size,
			result == rive::ImportResult::unsupportedVersion
				? "unsupported Rive file version"
				: "malformed Rive file");
		return false;
	}

	std::unique_ptr<rive::ArtboardInstance> ab;
	if (!artboard_name.empty()) {
		ab = file->artboardNamed(artboard_name);
		if (!ab) {
			set_err(err, err_size, "artboard not found");
			return false;
		}
	} else {
		ab = file->artboardDefault();
		if (!ab) {
			set_err(err, err_size, "file has no artboards");
			return false;
		}
	}

	std::unique_ptr<rive::StateMachineInstance> sm;
	if (!sm_name.empty()) {
		sm = ab->stateMachineNamed(sm_name);
	} else {
		sm = ab->defaultStateMachine();
		if (!sm && ab->stateMachineCount() > 0)
			sm = ab->stateMachineAt(0);
	}
	// A missing state machine is non-fatal: we'll still render the artboard
	// statically. Log it so the user can fix the dropdown.
	if (!sm && !sm_name.empty()) {
		obs_log(LOG_WARNING, "rive: state machine '%s' not found on artboard '%s'",
			sm_name.c_str(),
			artboard_name.empty() ? "(default)" : artboard_name.c_str());
	}

	r->file = std::move(file);
	r->artboard = std::move(ab);
	r->stateMachine = std::move(sm);
	r->loadedPath = path;
	r->loadedArtboard = artboard_name;
	r->loadedStateMachine = sm_name;

	// Log the artboard's primary VM so we can tell whether it's
	// SceneViewModel-based or uses a different VM as its data context.
	if (auto *defaultVm = r->file->defaultArtboardViewModel(r->artboard.get())) {
		obs_log(LOG_INFO, "rive: artboard primary view model = '%s'",
			defaultVm->name().c_str());
	} else {
		obs_log(LOG_INFO, "rive: artboard has no primary view model");
	}

	// Optional: bind the SceneViewModel to the SM. tryCreate returns null
	// when the file has no view model by that name, which is the normal
	// case for .riv files that don't opt into the scene-state contract.
	r->sceneViewModel =
		rive_obs::SceneViewModelBinding::tryCreate(r->file.get(), r->stateMachine.get());

	// Image cache: decode raw bytes via this file's RenderContext, which
	// is what allocates the matching GPU textures.
	rive::gpu::RenderContext *rc = r->renderContext.get();
	r->imageCache = std::make_unique<rive_obs::ImageCache>(
		[rc](const uint8_t *data, size_t size) -> rive::rcp<rive::RenderImage> {
			if (!rc || !data || size == 0)
				return nullptr;
			return rc->decodeImage(rive::Span<const uint8_t>(data, size));
		});

	// Custom-data binding sits alongside (or replaces) the scene binding —
	// see rive_custom_data.h for how it picks its target VM.
	r->customData = rive_obs::CustomDataBinding::tryCreate(
		r->file.get(), r->artboard.get(), r->stateMachine.get(),
		r->sceneViewModel ? r->sceneViewModel->root() : nullptr,
		r->imageCache.get());

	return true;
}

} // namespace

extern "C" {

rive_renderer_t *rive_renderer_create(uint32_t width, uint32_t height, char *err, size_t err_size)
{
	@autoreleasepool {
		auto *r = new rive_renderer();
		r->width = width;
		r->height = height;

		r->device = MTLCreateSystemDefaultDevice();
		if (!r->device) {
			set_err(err, err_size, "no Metal device available");
			delete r;
			return nullptr;
		}
		r->queue = [r->device newCommandQueue];
		if (!r->queue) {
			set_err(err, err_size, "could not create Metal command queue");
			delete r;
			return nullptr;
		}

		r->renderContext = rive::gpu::RenderContextMetalImpl::MakeContext(r->device);
		if (!r->renderContext) {
			set_err(err, err_size, "RenderContextMetalImpl::MakeContext failed");
			delete r;
			return nullptr;
		}

		if (!allocate_target_resources(r, err, err_size)) {
			delete r;
			return nullptr;
		}
		return r;
	}
}

void rive_renderer_destroy(rive_renderer_t *r)
{
	if (!r)
		return;
	@autoreleasepool {
		clear_content(r);
		release_target_resources(r);
		r->renderContext.reset();
		r->queue = nil;
		r->device = nil;
		delete r;
	}
}

bool rive_renderer_resize(rive_renderer_t *r, uint32_t width, uint32_t height)
{
	if (!r || width == 0 || height == 0)
		return false;
	if (width == r->width && height == r->height && r->gsTexture)
		return true;

	@autoreleasepool {
		release_target_resources(r);
		r->width = width;
		r->height = height;
		char err[256] = {0};
		if (!allocate_target_resources(r, err, sizeof(err))) {
			obs_log(LOG_ERROR, "rive: resize to %ux%u failed: %s", width, height,
				err[0] ? err : "unknown");
			return false;
		}
	}
	return true;
}

bool rive_renderer_set_file(rive_renderer_t *r, const char *path, const char *artboard_name,
			    const char *state_machine_name, char *err, size_t err_size)
{
	if (!r)
		return false;

	std::string p = path ? path : "";
	std::string a = artboard_name ? artboard_name : "";
	std::string s = state_machine_name ? state_machine_name : "";

	if (p.empty()) {
		@autoreleasepool {
			clear_content(r);
		}
		return true;
	}

	// Short-circuit if the selection hasn't changed. Reading the same .riv
	// from disk every settings tick would create unnecessary GPU churn.
	if (p == r->loadedPath && a == r->loadedArtboard && s == r->loadedStateMachine && r->file)
		return true;

	bool ok = false;
	@autoreleasepool {
		ok = load_content(r, p, a, s, err, err_size);
		if (!ok)
			clear_content(r);
	}
	return ok;
}

void rive_renderer_advance(rive_renderer_t *r, float dt_seconds)
{
	if (!r || !r->stateMachine)
		return;
	if (dt_seconds < 0.f)
		dt_seconds = 0.f;
	r->stateMachine->advanceAndApply(dt_seconds);
}

void rive_renderer_render(rive_renderer_t *r, const char *fit_value, const char *alignment_value,
			  uint32_t bg_color)
{
	if (!r || !r->renderTarget || !r->mtlTexture)
		return;

	@autoreleasepool {
		rive::gpu::RenderContext::FrameDescriptor frame;
		frame.renderTargetWidth = r->width;
		frame.renderTargetHeight = r->height;
		frame.loadAction = rive::gpu::LoadAction::clear;
		frame.clearColor = obs_color_to_rive(bg_color);

		r->renderContext->beginFrame(frame);

		if (r->artboard) {
			rive::RiveRenderer renderer(r->renderContext.get());
			renderer.save();
			renderer.align(parse_fit(fit_value), parse_alignment(alignment_value),
				       rive::AABB(0, 0, r->width, r->height),
				       r->artboard->bounds());
			r->artboard->draw(&renderer);
			renderer.restore();
		}

		r->renderTarget->setTargetTexture(r->mtlTexture);

		id<MTLCommandBuffer> cb = [r->queue commandBuffer];
		rive::gpu::RenderContext::FlushResources flush;
		flush.renderTarget = r->renderTarget.get();
		flush.externalCommandBuffer = (__bridge void *)cb;
		r->renderContext->flush(flush);

		[cb commit];
		// Block until the IOSurface contents are safe to be read by GL,
		// which is how the libobs/macOS render path samples it. We can
		// switch to fence-based synchronization later if this becomes a
		// frame-time bottleneck.
		[cb waitUntilCompleted];

		r->renderTarget->setTargetTexture(nil);
	}
}

gs_texture_t *rive_renderer_get_texture(rive_renderer_t *r)
{
	return r ? r->gsTexture : nullptr;
}

bool rive_renderer_fire_trigger(rive_renderer_t *r, const char *trigger_name)
{
	if (!r || !r->stateMachine || !trigger_name || !*trigger_name)
		return false;
	rive::SMITrigger *trig = r->stateMachine->getTrigger(trigger_name);
	if (!trig)
		return false;
	trig->fire();
	return true;
}

bool rive_renderer_has_state_machine(rive_renderer_t *r)
{
	return r && r->stateMachine != nullptr;
}

void rive_renderer_apply_scene_state(rive_renderer_t *r, const struct rive_scene_state *state)
{
	if (!r || !state || !r->sceneViewModel)
		return;
	r->sceneViewModel->apply(*state);
}

void rive_renderer_apply_custom_data(rive_renderer_t *r, struct obs_data *data)
{
	if (!r)
		return;
	// Drain any image fetches that finished since the last tick so the
	// binding sees freshly-loaded RenderImages this frame.
	if (r->imageCache)
		r->imageCache->tick();
	if (r->customData)
		r->customData->apply(data);
}

} // extern "C"
