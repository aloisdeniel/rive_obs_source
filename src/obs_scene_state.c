/*
 * obs_scene_state: see header.
 *
 * Memory layout: we keep three growable buffers across refreshes — the source
 * array, the audio array, and a string heap holding every name we expose to
 * Rive. We hand Rive raw pointers into the string heap (rive_scene_state's
 * char* fields), so the heap must not be re-allocated while those pointers
 * are live — i.e. the caller has finished consuming the snapshot before the
 * next obs_scene_state_refresh.
 */

#include "obs_scene_state.h"

#include <obs.h>
#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <plugin-support.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct obs_scene_state_collector {
	// Resizable arrays we hand to Rive each tick.
	struct rive_scene_source *sources;
	size_t source_count;
	size_t source_cap;

	struct rive_scene_audio *audios;
	size_t audio_count;
	size_t audio_cap;

	// Bump-allocated string heap used by the const char* fields above.
	// Cleared (length set to 0) at the start of each refresh. Strings
	// stay valid until the next refresh.
	char *str_buf;
	size_t str_len;
	size_t str_cap;

	// Current scene name lives in the same string heap so we don't need
	// a separate allocation. Index into str_buf.
	size_t scene_name_off;
	bool has_scene_name;

	// Mirrors the rive_scene_state we expose. Populated each refresh and
	// returned to the caller; the caller is allowed to keep this pointer
	// valid until the next refresh.
	struct rive_scene_state state;
};

// --- string heap ------------------------------------------------------------

// Reserves `extra` more bytes in the string heap, growing if needed.
static void ensure_str_cap(struct obs_scene_state_collector *c, size_t extra)
{
	if (c->str_len + extra <= c->str_cap)
		return;
	size_t new_cap = c->str_cap ? c->str_cap : 256;
	while (c->str_len + extra > new_cap)
		new_cap *= 2;
	c->str_buf = brealloc(c->str_buf, new_cap);
	c->str_cap = new_cap;
}

// Copies a (possibly NULL) string into the heap and returns its offset, so
// the caller can compute c->str_buf + offset when handing the pointer out.
// We use offsets rather than pointers internally because the heap may grow
// (and move) between successive interns within the same refresh.
static size_t intern_str(struct obs_scene_state_collector *c, const char *s)
{
	if (!s)
		s = "";
	size_t n = strlen(s) + 1;
	ensure_str_cap(c, n);
	size_t off = c->str_len;
	memcpy(c->str_buf + off, s, n);
	c->str_len += n;
	return off;
}

// --- source / audio lists ---------------------------------------------------

// Holds string offsets while we accumulate; the const char* pointers in the
// public structs are filled in once at the end of the refresh (after the heap
// has stopped growing).
struct source_acc {
	size_t name_off;
	float x, y, w, h;
	bool visible;
};

struct audio_acc {
	size_t name_off;
	float volume;
	bool muted;
};

// Parallel arrays of offsets; we promote them to const char* at the end. We
// keep them as separate fields rather than inside rive_scene_source so the
// final pointer assignment is a clean memcpy/loop with stable struct layout.
struct collector_workspace {
	struct source_acc *src;
	size_t src_count;
	size_t src_cap;

	struct audio_acc *aud;
	size_t aud_count;
	size_t aud_cap;
};

static void ws_reset(struct collector_workspace *w)
{
	w->src_count = 0;
	w->aud_count = 0;
}

static void ws_free(struct collector_workspace *w)
{
	bfree(w->src);
	bfree(w->aud);
	memset(w, 0, sizeof(*w));
}

static void ws_push_source(struct collector_workspace *w, const struct source_acc *a)
{
	if (w->src_count == w->src_cap) {
		w->src_cap = w->src_cap ? w->src_cap * 2 : 8;
		w->src = brealloc(w->src, w->src_cap * sizeof(*w->src));
	}
	w->src[w->src_count++] = *a;
}

static void ws_push_audio(struct collector_workspace *w, const struct audio_acc *a)
{
	if (w->aud_count == w->aud_cap) {
		w->aud_cap = w->aud_cap ? w->aud_cap * 2 : 8;
		w->aud = brealloc(w->aud, w->aud_cap * sizeof(*w->aud));
	}
	w->aud[w->aud_count++] = *a;
}

// --- scene item collection --------------------------------------------------

// Compute the axis-aligned bounding box of a scene item by transforming the
// four corners of the unit quad through its box_transform and reducing to
// min/max. This handles rotated/scaled items correctly; for non-rotated items
// it collapses to (pos, pos + scaled-size) as expected.
static void item_aabb(obs_sceneitem_t *item, float *out_x, float *out_y, float *out_w,
		      float *out_h)
{
	struct matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);

	struct vec3 corners[4];
	vec3_set(&corners[0], 0.f, 0.f, 0.f);
	vec3_set(&corners[1], 1.f, 0.f, 0.f);
	vec3_set(&corners[2], 0.f, 1.f, 0.f);
	vec3_set(&corners[3], 1.f, 1.f, 0.f);

	float min_x = 0.f, min_y = 0.f, max_x = 0.f, max_y = 0.f;
	for (int i = 0; i < 4; ++i) {
		struct vec3 v;
		vec3_transform(&v, &corners[i], &m);
		if (i == 0) {
			min_x = max_x = v.x;
			min_y = max_y = v.y;
		} else {
			if (v.x < min_x)
				min_x = v.x;
			if (v.x > max_x)
				max_x = v.x;
			if (v.y < min_y)
				min_y = v.y;
			if (v.y > max_y)
				max_y = v.y;
		}
	}
	*out_x = min_x;
	*out_y = min_y;
	*out_w = max_x - min_x;
	*out_h = max_y - min_y;
}

struct enum_sources_ctx {
	struct obs_scene_state_collector *c;
	struct collector_workspace *ws;
};

static bool enum_scene_item_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	(void)scene;
	struct enum_sources_ctx *ctx = param;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;

	struct source_acc a = {0};
	a.name_off = intern_str(ctx->c, obs_source_get_name(src));
	item_aabb(item, &a.x, &a.y, &a.w, &a.h);
	a.visible = obs_sceneitem_visible(item);
	ws_push_source(ctx->ws, &a);

	return true; // continue
}

// --- audio source collection ------------------------------------------------

struct enum_audio_ctx {
	struct obs_scene_state_collector *c;
	struct collector_workspace *ws;
};

static bool enum_audio_source_cb(void *param, obs_source_t *source)
{
	struct enum_audio_ctx *ctx = param;
	if (!source)
		return true;

	const uint32_t flags = obs_source_get_output_flags(source);
	// Only collect sources that actually produce audio. The audio-mixer
	// dock in OBS uses the same filter.
	if ((flags & OBS_SOURCE_AUDIO) == 0)
		return true;
	// Skip scenes/transitions, which can carry the AUDIO flag transitively
	// — those don't appear in the audio mixer and would just duplicate the
	// inputs that already do.
	const enum obs_source_type type = obs_source_get_type(source);
	if (type != OBS_SOURCE_TYPE_INPUT)
		return true;

	struct audio_acc a = {0};
	a.name_off = intern_str(ctx->c, obs_source_get_name(source));
	a.volume = obs_source_get_volume(source);
	a.muted = obs_source_muted(source);
	ws_push_audio(ctx->ws, &a);

	return true;
}

// --- lifecycle / refresh ----------------------------------------------------

obs_scene_state_collector_t *obs_scene_state_create(void)
{
	struct obs_scene_state_collector *c = bzalloc(sizeof(*c));
	return c;
}

void obs_scene_state_destroy(obs_scene_state_collector_t *c)
{
	if (!c)
		return;
	bfree(c->sources);
	bfree(c->audios);
	bfree(c->str_buf);
	bfree(c);
}

static void ensure_sources_cap(struct obs_scene_state_collector *c, size_t n)
{
	if (n <= c->source_cap)
		return;
	c->source_cap = c->source_cap ? c->source_cap : 8;
	while (n > c->source_cap)
		c->source_cap *= 2;
	c->sources = brealloc(c->sources, c->source_cap * sizeof(*c->sources));
}

static void ensure_audios_cap(struct obs_scene_state_collector *c, size_t n)
{
	if (n <= c->audio_cap)
		return;
	c->audio_cap = c->audio_cap ? c->audio_cap : 8;
	while (n > c->audio_cap)
		c->audio_cap *= 2;
	c->audios = brealloc(c->audios, c->audio_cap * sizeof(*c->audios));
}

const struct rive_scene_state *obs_scene_state_refresh(obs_scene_state_collector_t *c)
{
	// Reset string heap and output arrays. We keep their allocations so
	// steady-state refreshes don't churn the allocator.
	c->str_len = 0;
	c->source_count = 0;
	c->audio_count = 0;
	c->has_scene_name = false;
	c->scene_name_off = 0;

	struct collector_workspace ws = {0};

	// 1. Scene name + scene items.
	obs_source_t *scene_src = obs_frontend_get_current_scene();
	if (scene_src) {
		c->scene_name_off = intern_str(c, obs_source_get_name(scene_src));
		c->has_scene_name = true;

		obs_scene_t *scene = obs_scene_from_source(scene_src);
		if (!scene)
			scene = obs_group_from_source(scene_src);
		if (scene) {
			struct enum_sources_ctx ctx = {c, &ws};
			obs_scene_enum_items(scene, enum_scene_item_cb, &ctx);
		}
		obs_source_release(scene_src);
	}

	// 2. Audio inputs (global, not scene-scoped).
	{
		struct enum_audio_ctx ctx = {c, &ws};
		obs_enum_sources(enum_audio_source_cb, &ctx);
	}

	// 3. Promote offsets to const char* now that the string heap won't
	// grow again this refresh.
	ensure_sources_cap(c, ws.src_count);
	for (size_t i = 0; i < ws.src_count; ++i) {
		const struct source_acc *a = &ws.src[i];
		c->sources[i].name = c->str_buf + a->name_off;
		c->sources[i].x = a->x;
		c->sources[i].y = a->y;
		c->sources[i].width = a->w;
		c->sources[i].height = a->h;
		c->sources[i].visible = a->visible;
	}
	c->source_count = ws.src_count;

	ensure_audios_cap(c, ws.aud_count);
	for (size_t i = 0; i < ws.aud_count; ++i) {
		const struct audio_acc *a = &ws.aud[i];
		c->audios[i].name = c->str_buf + a->name_off;
		c->audios[i].volume = a->volume;
		c->audios[i].muted = a->muted;
	}
	c->audio_count = ws.aud_count;

	ws_free(&ws);

	// 4. Top-level scalars.
	struct obs_video_info ovi = {0};
	const bool have_video = obs_get_video_info(&ovi);

	c->state.scene_name = c->has_scene_name ? (c->str_buf + c->scene_name_off) : "";
	c->state.canvas_width = have_video ? (float)ovi.base_width : 0.f;
	c->state.canvas_height = have_video ? (float)ovi.base_height : 0.f;
	c->state.streaming = obs_frontend_streaming_active();
	c->state.recording = obs_frontend_recording_active();
	c->state.recording_paused = obs_frontend_recording_paused();
	c->state.sources = c->sources;
	c->state.source_count = c->source_count;
	c->state.audio_sources = c->audios;
	c->state.audio_count = c->audio_count;

	return &c->state;
}
