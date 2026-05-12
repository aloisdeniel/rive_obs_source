/*
 * rive_obs_source — OBS plugin that renders a Rive (.riv) file as a source.
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rive_file.h"
#include "rive_renderer.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// Defined in rive_probe.cpp; referenced from obs_module_load so the linker
// keeps the rive-runtime archives in this module.
extern void *rive_obs_link_probe(void);

#define RIVE_SOURCE_ID "rive_source"
#define RIVE_SOURCE_DEFAULT_WIDTH 640u
#define RIVE_SOURCE_DEFAULT_HEIGHT 360u

// Setting keys. Kept stable across versions because they're persisted into
// scene-collection JSON.
#define SK_FILE "file"
#define SK_ARTBOARD "artboard"
#define SK_STATE_MACHINE "state_machine"
#define SK_WIDTH "width"
#define SK_HEIGHT "height"
#define SK_FIT "fit"
#define SK_ALIGNMENT "alignment"
#define SK_BG_COLOR "bg_color"

// Fit / alignment values are persisted as strings so future renderer code can
// map them to rive::Fit / rive::Alignment without an int-vs-enum coupling.
static const char *const FIT_VALUES[] = {"contain",   "cover",      "fill",      "fit-width",
					 "fit-height", "none",       "scale-down"};
static const char *const FIT_LABELS[] = {"Contain",   "Cover",      "Fill",      "Fit width",
					 "Fit height", "None",       "Scale down"};
#define FIT_COUNT (sizeof(FIT_VALUES) / sizeof(FIT_VALUES[0]))

static const char *const ALIGN_VALUES[] = {"top-left",   "top-center",   "top-right",
					   "center-left", "center",       "center-right",
					   "bottom-left", "bottom-center", "bottom-right"};
static const char *const ALIGN_LABELS[] = {"Top left",   "Top center",   "Top right",
					   "Center left", "Center",       "Center right",
					   "Bottom left", "Bottom center", "Bottom right"};
#define ALIGN_COUNT (sizeof(ALIGN_VALUES) / sizeof(ALIGN_VALUES[0]))

struct rive_source {
	obs_source_t *source;

	// Cached settings. Owned strings (bstrdup) so we don't depend on the
	// obs_data_t outliving the source between update() calls.
	char *path;
	char *artboard;
	char *state_machine;
	uint32_t width;
	uint32_t height;
	char *fit;
	char *alignment;
	uint32_t bg_color; // RGBA, premultiplied later in render

	// GPU renderer (created lazily on the graphics thread). Owned.
	rive_renderer_t *renderer;
	// True when settings have changed and the renderer needs to refresh its
	// file / size on the next graphics-thread tick.
	bool dirty;
};

static const char *rive_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("RiveSource");
}

// ---- settings helpers ------------------------------------------------------

static void replace_str(char **slot, const char *value)
{
	bfree(*slot);
	*slot = value ? bstrdup(value) : NULL;
}

static void rive_source_apply_settings(struct rive_source *ctx, obs_data_t *settings)
{
	const char *path = obs_data_get_string(settings, SK_FILE);
	const char *artboard = obs_data_get_string(settings, SK_ARTBOARD);
	const char *sm = obs_data_get_string(settings, SK_STATE_MACHINE);
	const char *fit = obs_data_get_string(settings, SK_FIT);
	const char *align = obs_data_get_string(settings, SK_ALIGNMENT);

	replace_str(&ctx->path, path);
	replace_str(&ctx->artboard, artboard);
	replace_str(&ctx->state_machine, sm);
	replace_str(&ctx->fit, fit);
	replace_str(&ctx->alignment, align);

	long long w = obs_data_get_int(settings, SK_WIDTH);
	long long h = obs_data_get_int(settings, SK_HEIGHT);
	ctx->width = (w > 0) ? (uint32_t)w : RIVE_SOURCE_DEFAULT_WIDTH;
	ctx->height = (h > 0) ? (uint32_t)h : RIVE_SOURCE_DEFAULT_HEIGHT;
	ctx->bg_color = (uint32_t)obs_data_get_int(settings, SK_BG_COLOR);

	// Tell the graphics-thread tick to push these into the renderer on its
	// next pass. We deliberately do not touch ctx->renderer from here — the
	// update callback can fire from non-graphics threads.
	ctx->dirty = true;
}

// Pushes the source's cached settings into the renderer. Must run on the OBS
// graphics thread (it touches GPU resources).
static void rive_source_sync_renderer(struct rive_source *ctx)
{
	if (!ctx->dirty)
		return;
	ctx->dirty = false;

	if (!ctx->renderer) {
		char err[256];
		err[0] = '\0';
		ctx->renderer = rive_renderer_create(ctx->width, ctx->height, err, sizeof(err));
		if (!ctx->renderer) {
			obs_log(LOG_ERROR, "rive: renderer create failed: %s",
				err[0] ? err : "unknown");
			return;
		}
	} else {
		rive_renderer_resize(ctx->renderer, ctx->width, ctx->height);
	}

	char err[256];
	err[0] = '\0';
	if (!rive_renderer_set_file(ctx->renderer, ctx->path ? ctx->path : "",
				    ctx->artboard ? ctx->artboard : "",
				    ctx->state_machine ? ctx->state_machine : "", err,
				    sizeof(err))) {
		obs_log(LOG_WARNING, "rive: set_file failed: %s", err[0] ? err : "unknown");
	}
}

// ---- lifecycle -------------------------------------------------------------

static void *rive_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct rive_source *ctx = bzalloc(sizeof(struct rive_source));
	ctx->source = source;
	rive_source_apply_settings(ctx, settings);
	obs_log(LOG_INFO, "Rive source created (file='%s')", ctx->path ? ctx->path : "");
	return ctx;
}

static void rive_source_destroy(void *data)
{
	struct rive_source *ctx = data;
	if (!ctx)
		return;
	if (ctx->renderer) {
		obs_enter_graphics();
		rive_renderer_destroy(ctx->renderer);
		obs_leave_graphics();
		ctx->renderer = NULL;
	}
	bfree(ctx->path);
	bfree(ctx->artboard);
	bfree(ctx->state_machine);
	bfree(ctx->fit);
	bfree(ctx->alignment);
	obs_log(LOG_INFO, "Rive source destroyed");
	bfree(ctx);
}

static void rive_source_update(void *data, obs_data_t *settings)
{
	struct rive_source *ctx = data;
	if (!ctx)
		return;
	rive_source_apply_settings(ctx, settings);
}

static void rive_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SK_FILE, "");
	obs_data_set_default_string(settings, SK_ARTBOARD, "");
	obs_data_set_default_string(settings, SK_STATE_MACHINE, "");
	obs_data_set_default_int(settings, SK_WIDTH, RIVE_SOURCE_DEFAULT_WIDTH);
	obs_data_set_default_int(settings, SK_HEIGHT, RIVE_SOURCE_DEFAULT_HEIGHT);
	obs_data_set_default_string(settings, SK_FIT, "contain");
	obs_data_set_default_string(settings, SK_ALIGNMENT, "center");
	obs_data_set_default_int(settings, SK_BG_COLOR, 0x00000000);
}

static uint32_t rive_source_get_width(void *data)
{
	struct rive_source *ctx = data;
	return ctx ? ctx->width : RIVE_SOURCE_DEFAULT_WIDTH;
}

static uint32_t rive_source_get_height(void *data)
{
	struct rive_source *ctx = data;
	return ctx ? ctx->height : RIVE_SOURCE_DEFAULT_HEIGHT;
}

// ---- properties ------------------------------------------------------------

static void populate_state_machines(obs_property_t *sm_prop, rive_file_t *file,
				    const char *artboard_name)
{
	obs_property_list_clear(sm_prop);
	if (!file)
		return;

	size_t ab_idx = rive_file_find_artboard(file, artboard_name);
	if (ab_idx == SIZE_MAX) {
		// Fall back to the first artboard so the SM list isn't empty
		// just because the artboard string hasn't been set yet.
		if (rive_file_artboard_count(file) == 0)
			return;
		ab_idx = 0;
	}

	const size_t count = rive_file_state_machine_count(file, ab_idx);
	for (size_t i = 0; i < count; ++i) {
		const char *name = rive_file_state_machine_name(file, ab_idx, i);
		if (name)
			obs_property_list_add_string(sm_prop, name, name);
	}
}

static void populate_artboards(obs_property_t *ab_prop, rive_file_t *file)
{
	obs_property_list_clear(ab_prop);
	if (!file)
		return;
	const size_t count = rive_file_artboard_count(file);
	for (size_t i = 0; i < count; ++i) {
		const char *name = rive_file_artboard_name(file, i);
		if (name)
			obs_property_list_add_string(ab_prop, name, name);
	}
}

static bool on_file_modified(obs_properties_t *props, obs_property_t *property,
			     obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const char *path = obs_data_get_string(settings, SK_FILE);
	obs_property_t *ab_prop = obs_properties_get(props, SK_ARTBOARD);
	obs_property_t *sm_prop = obs_properties_get(props, SK_STATE_MACHINE);

	rive_file_t *file = NULL;
	if (path && *path) {
		char err[256];
		err[0] = '\0';
		file = rive_file_open(path, err, sizeof(err));
		if (!file)
			obs_log(LOG_WARNING, "rive: failed to load '%s': %s", path,
				err[0] ? err : "unknown error");
	}

	populate_artboards(ab_prop, file);

	// If the previously selected artboard isn't in the new file, snap to
	// the first one and reset the SM choice to keep the UI consistent.
	const char *current_ab = obs_data_get_string(settings, SK_ARTBOARD);
	if (file && rive_file_find_artboard(file, current_ab) == SIZE_MAX) {
		const char *first =
			rive_file_artboard_count(file) > 0 ? rive_file_artboard_name(file, 0) : "";
		obs_data_set_string(settings, SK_ARTBOARD, first ? first : "");
		obs_data_set_string(settings, SK_STATE_MACHINE, "");
	}

	populate_state_machines(sm_prop, file, obs_data_get_string(settings, SK_ARTBOARD));

	rive_file_close(file);
	return true;
}

static bool on_artboard_modified(obs_properties_t *props, obs_property_t *property,
				 obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const char *path = obs_data_get_string(settings, SK_FILE);
	obs_property_t *sm_prop = obs_properties_get(props, SK_STATE_MACHINE);

	rive_file_t *file = NULL;
	if (path && *path) {
		file = rive_file_open(path, NULL, 0);
	}

	populate_state_machines(sm_prop, file, obs_data_get_string(settings, SK_ARTBOARD));

	// Clear the SM if it's no longer valid for the new artboard.
	const char *current_sm = obs_data_get_string(settings, SK_STATE_MACHINE);
	if (current_sm && *current_sm && file) {
		size_t ab_idx =
			rive_file_find_artboard(file, obs_data_get_string(settings, SK_ARTBOARD));
		if (ab_idx == SIZE_MAX)
			ab_idx = 0;
		bool found = false;
		const size_t count = rive_file_state_machine_count(file, ab_idx);
		for (size_t i = 0; i < count; ++i) {
			const char *n = rive_file_state_machine_name(file, ab_idx, i);
			if (n && strcmp(n, current_sm) == 0) {
				found = true;
				break;
			}
		}
		if (!found)
			obs_data_set_string(settings, SK_STATE_MACHINE, "");
	}

	rive_file_close(file);
	return true;
}

static obs_properties_t *rive_source_get_properties(void *data)
{
	struct rive_source *ctx = data;

	obs_properties_t *props = obs_properties_create();

	obs_property_t *p_file =
		obs_properties_add_path(props, SK_FILE, obs_module_text("File"), OBS_PATH_FILE,
					"Rive files (*.riv);;All files (*.*)", NULL);
	obs_property_set_modified_callback(p_file, on_file_modified);

	obs_property_t *p_ab = obs_properties_add_list(props, SK_ARTBOARD,
						       obs_module_text("Artboard"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(p_ab, on_artboard_modified);

	obs_properties_add_list(props, SK_STATE_MACHINE, obs_module_text("StateMachine"),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_properties_add_int(props, SK_WIDTH, obs_module_text("Width"), 1, 8192, 1);
	obs_properties_add_int(props, SK_HEIGHT, obs_module_text("Height"), 1, 8192, 1);

	obs_property_t *p_fit = obs_properties_add_list(props, SK_FIT, obs_module_text("Fit"),
							OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	for (size_t i = 0; i < FIT_COUNT; ++i)
		obs_property_list_add_string(p_fit, FIT_LABELS[i], FIT_VALUES[i]);

	obs_property_t *p_align = obs_properties_add_list(props, SK_ALIGNMENT,
							  obs_module_text("Alignment"),
							  OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_STRING);
	for (size_t i = 0; i < ALIGN_COUNT; ++i)
		obs_property_list_add_string(p_align, ALIGN_LABELS[i], ALIGN_VALUES[i]);

	obs_properties_add_color_alpha(props, SK_BG_COLOR, obs_module_text("BackgroundColor"));

	// Pre-populate the dropdowns based on the source's current settings so
	// the panel shows real choices the first time it opens.
	if (ctx && ctx->source) {
		obs_data_t *settings = obs_source_get_settings(ctx->source);
		if (settings) {
			on_file_modified(props, p_file, settings);
			obs_data_release(settings);
		}
	}

	return props;
}

// ---- render ----------------------------------------------------------------

static void rive_source_video_tick(void *data, float seconds)
{
	struct rive_source *ctx = data;
	if (!ctx)
		return;

	rive_source_sync_renderer(ctx);

	if (ctx->renderer) {
		rive_renderer_advance(ctx->renderer, seconds);
		rive_renderer_render(ctx->renderer, ctx->fit, ctx->alignment, ctx->bg_color);
	}
}

static void rive_source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct rive_source *ctx = data;
	if (!ctx)
		return;

	gs_texture_t *tex = ctx->renderer ? rive_renderer_get_texture(ctx->renderer) : NULL;

	if (tex) {
#ifdef _WIN32
		// On Windows the renderer's texture is a shared NT-handle protected
		// by a keyed mutex (same-key protocol). Acquire it before sampling
		// so writes from the renderer's D3D11 device are visible here.
		if (gs_texture_acquire_sync(tex, 0, 1000) != 0) {
			obs_log(LOG_WARNING, "rive: keyed mutex acquire timed out");
		} else {
#endif
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *image = gs_effect_get_param_by_name(def, "image");
			gs_effect_set_texture(image, tex);
			while (gs_effect_loop(def, "Draw")) {
				gs_draw_sprite(tex, 0, ctx->width, ctx->height);
			}
#ifdef _WIN32
			gs_texture_release_sync(tex, 0);
		}
#endif
		return;
	}

	// Renderer not ready yet — paint the configured background so the
	// source still has a visible footprint at its declared size.
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");

	struct vec4 color;
	const uint32_t c = ctx->bg_color;
	const float r = (float)((c >> 0) & 0xff) / 255.0f;
	const float g = (float)((c >> 8) & 0xff) / 255.0f;
	const float b = (float)((c >> 16) & 0xff) / 255.0f;
	const float a = (float)((c >> 24) & 0xff) / 255.0f;
	vec4_set(&color, r * a, g * a, b * a, a);
	gs_effect_set_vec4(color_param, &color);

	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(NULL, 0, ctx->width, ctx->height);
	}
}

static struct obs_source_info rive_source_info = {
	.id = RIVE_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.icon_type = OBS_ICON_TYPE_IMAGE,
	.get_name = rive_source_get_name,
	.create = rive_source_create,
	.destroy = rive_source_destroy,
	.update = rive_source_update,
	.get_defaults = rive_source_get_defaults,
	.get_properties = rive_source_get_properties,
	.get_width = rive_source_get_width,
	.get_height = rive_source_get_height,
	.video_tick = rive_source_video_tick,
	.video_render = rive_source_render,
};

bool obs_module_load(void)
{
	obs_register_source(&rive_source_info);
	// Force-keep the rive-runtime archives at link time.
	(void)rive_obs_link_probe();
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
