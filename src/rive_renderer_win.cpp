/*
 * rive_renderer_win: Rive Renderer (D3D11 backend) bridged to libobs via a
 * shared NT-handle ID3D11Texture2D protected by an IDXGIKeyedMutex.
 *
 * The plugin owns its own D3D11 device dedicated to Rive's rendering — sharing
 * libobs's device would require deep coordination of buffer-state and command
 * ordering, so we keep them separate and synchronize through the keyed mutex.
 *
 * Synchronization protocol (same-key, acts as a regular mutex with cross-
 * device coherence):
 *   - Renderer: AcquireSync(0) -> draw + flush -> ReleaseSync(0).
 *   - libobs: gs_texture_acquire_sync(tex, 0) -> sample -> gs_texture_release_sync(tex, 0).
 * Either side blocks the other while it owns the mutex.
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
#include <rive/renderer/render_context.hpp>
#include <rive/renderer/rive_renderer.hpp>
#include <rive/renderer/d3d11/render_context_d3d_impl.hpp>
#include <rive/renderer/d3d/d3d.hpp>

#include <obs.h>
#include <plugin-support.h>

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

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

// OBS persists colors as 0xAABBGGRR; Rive's ColorInt is 0xAARRGGBB.
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
	uint32_t width = 0;
	uint32_t height = 0;

	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;

	std::unique_ptr<rive::gpu::RenderContext> renderContext;
	rive::rcp<rive::gpu::RenderTargetD3D> renderTarget;

	ComPtr<ID3D11Texture2D> sharedTexture;
	ComPtr<IDXGIKeyedMutex> sharedMutex;
	HANDLE sharedNtHandle = nullptr;
	gs_texture_t *gsTexture = nullptr;

	rive::rcp<rive::File> file;
	std::unique_ptr<rive::ArtboardInstance> artboard;
	std::unique_ptr<rive::StateMachineInstance> stateMachine;

	// SceneViewModel binding (null when the .riv has no such VM). See
	// rive_renderer_mac.mm for the rationale; the windows backend treats it
	// identically.
	std::unique_ptr<rive_obs::SceneViewModelBinding> sceneViewModel;

	// Custom-data binding (null when neither SceneViewModel nor an artboard
	// default VM is present).
	std::unique_ptr<rive_obs::CustomDataBinding> customData;

	// URL → RenderImage cache shared with customData. Lives on the
	// renderer because decoding needs the renderer's RenderContext.
	std::unique_ptr<rive_obs::ImageCache> imageCache;

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
	r->renderTarget.reset();
	r->sharedMutex.Reset();
	r->sharedTexture.Reset();
	if (r->sharedNtHandle) {
		CloseHandle(r->sharedNtHandle);
		r->sharedNtHandle = nullptr;
	}
}

bool allocate_target_resources(rive_renderer *r, char *err, size_t err_size)
{
	if (r->width == 0 || r->height == 0) {
		set_err(err, err_size, "renderer width/height not set");
		return false;
	}

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = r->width;
	desc.Height = r->height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	ComPtr<ID3D11Texture2D> tex;
	HRESULT hr = r->device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
	if (FAILED(hr)) {
		set_err(err, err_size, "CreateTexture2D failed");
		return false;
	}

	ComPtr<IDXGIResource1> dxgiRes;
	hr = tex.As(&dxgiRes);
	if (FAILED(hr) || !dxgiRes) {
		set_err(err, err_size, "QueryInterface(IDXGIResource1) failed");
		return false;
	}

	HANDLE handle = nullptr;
	hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
					 &handle);
	if (FAILED(hr) || !handle) {
		set_err(err, err_size, "CreateSharedHandle failed");
		return false;
	}

	ComPtr<IDXGIKeyedMutex> mutex;
	hr = tex.As(&mutex);
	if (FAILED(hr) || !mutex) {
		CloseHandle(handle);
		set_err(err, err_size, "QueryInterface(IDXGIKeyedMutex) failed");
		return false;
	}

	// gs_texture_open_nt_shared takes the handle's low 32 bits — that's how
	// libobs's existing capture plugins thread NT handles through this API.
	const uint32_t obs_handle = (uint32_t)(uintptr_t)handle;
	gs_texture_t *gs_tex = gs_texture_open_nt_shared(obs_handle);
	if (!gs_tex) {
		CloseHandle(handle);
		set_err(err, err_size, "gs_texture_open_nt_shared failed");
		return false;
	}

	auto *impl = r->renderContext->static_impl_cast<rive::gpu::RenderContextD3DImpl>();
	auto rt = impl->makeRenderTarget(r->width, r->height);
	if (!rt) {
		gs_texture_destroy(gs_tex);
		CloseHandle(handle);
		set_err(err, err_size, "makeRenderTarget failed");
		return false;
	}

	r->sharedTexture = std::move(tex);
	r->sharedMutex = std::move(mutex);
	r->sharedNtHandle = handle;
	r->gsTexture = gs_tex;
	r->renderTarget = std::move(rt);
	return true;
}

void clear_content(rive_renderer *r)
{
	// Custom-data first — it may share the SceneViewModel root.
	r->customData.reset();
	r->sceneViewModel.reset();
	r->stateMachine.reset();
	r->artboard.reset();
	r->file.reset();
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
	rive::rcp<rive::File> file = rive::File::import(rive::Span<const uint8_t>(bytes.data(), bytes.size()),
							r->renderContext.get(), &result);
	if (!file || result != rive::ImportResult::success) {
		set_err(err, err_size,
			result == rive::ImportResult::unsupportedVersion ? "unsupported Rive file version"
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
	if (!sm && !sm_name.empty()) {
		obs_log(LOG_WARNING, "rive: state machine '%s' not found on artboard '%s'", sm_name.c_str(),
			artboard_name.empty() ? "(default)" : artboard_name.c_str());
	}

	r->file = std::move(file);
	r->artboard = std::move(ab);
	r->stateMachine = std::move(sm);
	r->loadedPath = path;
	r->loadedArtboard = artboard_name;
	r->loadedStateMachine = sm_name;

	r->sceneViewModel = rive_obs::SceneViewModelBinding::tryCreate(r->file.get(), r->stateMachine.get());

	rive::gpu::RenderContext *rc = r->renderContext.get();
	r->imageCache = std::make_unique<rive_obs::ImageCache>(
		[rc](const uint8_t *data, size_t size) -> rive::rcp<rive::RenderImage> {
			if (!rc || !data || size == 0)
				return nullptr;
			return rc->decodeImage(rive::Span<const uint8_t>(data, size));
		});

	r->customData = rive_obs::CustomDataBinding::tryCreate(r->file.get(), r->artboard.get(), r->stateMachine.get(),
							       r->sceneViewModel ? r->sceneViewModel->root() : nullptr,
							       r->imageCache.get());

	return true;
}

} // namespace

extern "C" {

rive_renderer_t *rive_renderer_create(uint32_t width, uint32_t height, char *err, size_t err_size)
{
	auto *r = new rive_renderer();
	r->width = width;
	r->height = height;

	UINT creationFlags = 0;
#ifndef NDEBUG
	creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1};
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, featureLevels,
				       std::size(featureLevels), D3D11_SDK_VERSION, r->device.GetAddressOf(), nullptr,
				       r->context.GetAddressOf());
	if (FAILED(hr) || !r->device || !r->context) {
		set_err(err, err_size, "D3D11CreateDevice failed");
		delete r;
		return nullptr;
	}

	rive::gpu::D3DContextOptions options;
	// Inspect the GPU vendor so the renderer's pipeline manager can take the
	// Intel codepaths where applicable.
	ComPtr<IDXGIDevice> dxgiDevice;
	if (SUCCEEDED(r->device.As(&dxgiDevice))) {
		ComPtr<IDXGIAdapter> adapter;
		if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) && adapter) {
			DXGI_ADAPTER_DESC desc{};
			if (SUCCEEDED(adapter->GetDesc(&desc))) {
				options.isIntel = desc.VendorId == 0x163C || desc.VendorId == 0x8086 ||
						  desc.VendorId == 0x8087;
			}
		}
	}

	r->renderContext = rive::gpu::RenderContextD3DImpl::MakeContext(r->device, r->context, options);
	if (!r->renderContext) {
		set_err(err, err_size, "RenderContextD3DImpl::MakeContext failed");
		delete r;
		return nullptr;
	}

	if (!allocate_target_resources(r, err, err_size)) {
		delete r;
		return nullptr;
	}
	return r;
}

void rive_renderer_destroy(rive_renderer_t *r)
{
	if (!r)
		return;
	clear_content(r);
	release_target_resources(r);
	r->renderContext.reset();
	r->context.Reset();
	r->device.Reset();
	delete r;
}

bool rive_renderer_resize(rive_renderer_t *r, uint32_t width, uint32_t height)
{
	if (!r || width == 0 || height == 0)
		return false;
	if (width == r->width && height == r->height && r->gsTexture)
		return true;

	release_target_resources(r);
	r->width = width;
	r->height = height;
	char err[256] = {0};
	if (!allocate_target_resources(r, err, sizeof(err))) {
		obs_log(LOG_ERROR, "rive: resize to %ux%u failed: %s", width, height, err[0] ? err : "unknown");
		return false;
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
		clear_content(r);
		return true;
	}

	if (p == r->loadedPath && a == r->loadedArtboard && s == r->loadedStateMachine && r->file)
		return true;

	bool ok = load_content(r, p, a, s, err, err_size);
	if (!ok)
		clear_content(r);
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

void rive_renderer_render(rive_renderer_t *r, const char *fit_value, const char *alignment_value, uint32_t bg_color)
{
	if (!r || !r->renderTarget || !r->sharedTexture || !r->sharedMutex)
		return;

	// Same-key keyed-mutex protocol: both producer (this) and consumer (OBS,
	// in video_render) Acquire(0)/Release(0) so the mutex behaves like a
	// regular cross-device mutex.
	HRESULT hr = r->sharedMutex->AcquireSync(0, INFINITE);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "rive: AcquireSync failed (%08lX)", hr);
		return;
	}

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
			       rive::AABB(0, 0, r->width, r->height), r->artboard->bounds());
		r->artboard->draw(&renderer);
		renderer.restore();
	}

	r->renderTarget->setTargetTexture(r->sharedTexture);

	rive::gpu::RenderContext::FlushResources flush;
	flush.renderTarget = r->renderTarget.get();
	r->renderContext->flush(flush);

	// Force Rive's queued GPU work to be submitted before we release the
	// mutex, so the OBS-side acquire actually sees the rendered pixels.
	r->context->Flush();

	r->renderTarget->setTargetTexture(nullptr);

	hr = r->sharedMutex->ReleaseSync(0);
	if (FAILED(hr))
		obs_log(LOG_ERROR, "rive: ReleaseSync failed (%08lX)", hr);
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
	if (r->imageCache)
		r->imageCache->tick();
	if (r->customData)
		r->customData->apply(data);
}

} // extern "C"
