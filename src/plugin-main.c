/*
 * rive_obs_source — OBS plugin that renders a Rive (.riv) file as a source.
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

#define RIVE_SOURCE_ID "rive_source"
#define RIVE_SOURCE_DEFAULT_WIDTH 640u
#define RIVE_SOURCE_DEFAULT_HEIGHT 360u

struct rive_source {
	obs_source_t *source;
	uint32_t width;
	uint32_t height;
};

static const char *rive_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("RiveSource");
}

static void *rive_source_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct rive_source *ctx = bzalloc(sizeof(struct rive_source));
	ctx->source = source;
	ctx->width = RIVE_SOURCE_DEFAULT_WIDTH;
	ctx->height = RIVE_SOURCE_DEFAULT_HEIGHT;
	obs_log(LOG_INFO, "Rive source created");
	return ctx;
}

static void rive_source_destroy(void *data)
{
	struct rive_source *ctx = data;
	if (!ctx)
		return;
	obs_log(LOG_INFO, "Rive source destroyed");
	bfree(ctx);
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

static void rive_source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct rive_source *ctx = data;
	if (!ctx)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");

	struct vec4 color;
	vec4_set(&color, 0.10f, 0.12f, 0.16f, 1.0f);
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
	.get_width = rive_source_get_width,
	.get_height = rive_source_get_height,
	.video_render = rive_source_render,
};

bool obs_module_load(void)
{
	obs_register_source(&rive_source_info);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
