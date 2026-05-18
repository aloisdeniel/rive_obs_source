/*
 * rive_obs_source — OBS plugin that renders a Rive (.riv) file as a source.
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "obs_scene_state.h"
#include "rive_file.h"
#include "rive_renderer.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// Defined in rive_probe.cpp; referenced from obs_module_load so the linker
// keeps the rive-runtime archives in this module.
extern void *rive_obs_link_probe(void);

#define RIVE_SOURCE_ID "rive_source"
// Fallback dimensions only used if OBS hasn't initialized video yet at the
// moment defaults are queried; otherwise we match the active canvas size.
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
#define SK_STATUS "status"
#define SK_REFRESH "refresh"
#define SK_DATA_FILE "data_file"

// How often the file watchers stat their respective paths. Cheap, but
// pointless to do on every video_tick — the user is unlikely to overwrite a
// file more than once per second.
#define FILE_WATCH_POLL_SECONDS 1.0f

// Fit / alignment values are persisted as strings so future renderer code can
// map them to rive::Fit / rive::Alignment without an int-vs-enum coupling.
static const char *const FIT_VALUES[] = {"contain", "cover", "fill", "fit-width", "fit-height", "none", "scale-down"};
static const char *const FIT_LABELS[] = {"Contain", "Cover", "Fill", "Fit width", "Fit height", "None", "Scale down"};
#define FIT_COUNT (sizeof(FIT_VALUES) / sizeof(FIT_VALUES[0]))

static const char *const ALIGN_VALUES[] = {"top-left",     "top-center",  "top-right",     "center-left", "center",
					   "center-right", "bottom-left", "bottom-center", "bottom-right"};
static const char *const ALIGN_LABELS[] = {"Top left",     "Top center",  "Top right",     "Center left", "Center",
					   "Center right", "Bottom left", "Bottom center", "Bottom right"};
#define ALIGN_COUNT (sizeof(ALIGN_VALUES) / sizeof(ALIGN_VALUES[0]))

struct rive_source {
	obs_source_t *source;

	// Cached settings. Owned strings (bstrdup) so we don't depend on the
	// obs_data_t outliving the source between update() calls.
	char *path;
	char *artboard;
	char *state_machine;
	char *data_path; // optional JSON file feeding the custom view model
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

	// Per-tick OBS scene snapshot pushed into the renderer's SceneViewModel.
	obs_scene_state_collector_t *scene_state;

	// .riv watcher state. Always polled — there's no reasonable cadence for
	// picking up external file changes other than once per second. Only
	// touched on the graphics thread.
	int64_t file_mtime; // mtime in seconds of the currently loaded file
	float watch_accum;  // seconds accumulated since the last stat()

	// Custom-data JSON watcher state. Same cadence as the .riv watcher.
	int64_t data_mtime;       // mtime in seconds of the currently loaded JSON
	float data_watch_accum;   // seconds accumulated since the last stat()
	obs_data_t *data_payload; // last successfully parsed JSON, or NULL
	char *data_logged_error;  // de-dup memo for parse failures

	// Status surface. Written by the graphics thread (on load success /
	// failure), read by the UI thread (when populating the properties
	// panel). The mutex is held only for short snapshots.
	pthread_mutex_t status_mu;
	char *last_error;  // owned (bstrdup) error string, or NULL
	bool last_load_ok; // true if the most recent load succeeded
	bool ever_loaded;  // true once a file has been successfully loaded
	// Most recently logged error string. Used to suppress repeated
	// identical errors so we don't spam the OBS log when the watcher
	// keeps reading a malformed file.
	char *logged_error;
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
	const char *data_path = obs_data_get_string(settings, SK_DATA_FILE);

	// Detect data-path edits so we can drop the old payload immediately
	// rather than serving stale JSON until the next mtime change.
	const char *prev_dp = ctx->data_path ? ctx->data_path : "";
	const char *new_dp = data_path ? data_path : "";
	const bool data_path_changed = strcmp(prev_dp, new_dp) != 0;

	replace_str(&ctx->path, path);
	replace_str(&ctx->artboard, artboard);
	replace_str(&ctx->state_machine, sm);
	replace_str(&ctx->fit, fit);
	replace_str(&ctx->alignment, align);
	replace_str(&ctx->data_path, data_path);

	long long w = obs_data_get_int(settings, SK_WIDTH);
	long long h = obs_data_get_int(settings, SK_HEIGHT);
	ctx->width = (w > 0) ? (uint32_t)w : RIVE_SOURCE_DEFAULT_WIDTH;
	ctx->height = (h > 0) ? (uint32_t)h : RIVE_SOURCE_DEFAULT_HEIGHT;
	ctx->bg_color = (uint32_t)obs_data_get_int(settings, SK_BG_COLOR);

	// Tell the graphics-thread tick to push these into the renderer on its
	// next pass. We deliberately do not touch ctx->renderer from here — the
	// update callback can fire from non-graphics threads.
	ctx->dirty = true;
	// Force the mtime watcher to re-stat next tick so a freshly chosen file
	// (or one whose path field was edited by hand) updates its baseline.
	ctx->file_mtime = 0;
	ctx->watch_accum = FILE_WATCH_POLL_SECONDS;

	if (data_path_changed) {
		// Drop any cached payload tied to the previous file so the
		// next tick starts fresh against whatever the new path resolves
		// to (or no payload at all when cleared).
		if (ctx->data_payload) {
			obs_data_release(ctx->data_payload);
			ctx->data_payload = NULL;
		}
		bfree(ctx->data_logged_error);
		ctx->data_logged_error = NULL;
		ctx->data_mtime = 0;
		ctx->data_watch_accum = FILE_WATCH_POLL_SECONDS;
	}
}

// ---- status surface --------------------------------------------------------

// Replaces ctx->last_error under the status mutex and sets the load flag. err
// may be NULL to clear the error; pass ok=true only when there is no error.
static void rive_source_set_status(struct rive_source *ctx, bool ok, const char *err)
{
	pthread_mutex_lock(&ctx->status_mu);
	ctx->last_load_ok = ok;
	if (ok) {
		ctx->ever_loaded = true;
		bfree(ctx->last_error);
		ctx->last_error = NULL;
	} else if (err && *err) {
		bfree(ctx->last_error);
		ctx->last_error = bstrdup(err);
	}
	pthread_mutex_unlock(&ctx->status_mu);
}

// Returns a malloc'd copy of the current status string for display in the
// properties panel. Caller must bfree(). path / artboard / sm are passed in to
// avoid taking the source lock in here.
static char *rive_source_format_status(struct rive_source *ctx)
{
	char err_copy[512] = {0};
	bool ok;
	bool ever;
	pthread_mutex_lock(&ctx->status_mu);
	ok = ctx->last_load_ok;
	ever = ctx->ever_loaded;
	if (ctx->last_error)
		snprintf(err_copy, sizeof(err_copy), "%s", ctx->last_error);
	pthread_mutex_unlock(&ctx->status_mu);

	char buf[2048];
	int n = 0;
	if (!ctx->path || !*ctx->path) {
		snprintf(buf, sizeof(buf), "No file selected.");
		return bstrdup(buf);
	}
	if (!ok && err_copy[0]) {
		n = snprintf(buf, sizeof(buf), "Error: %s\nFile: %s", err_copy, ctx->path);
	} else if (ok) {
		n = snprintf(buf, sizeof(buf), "Loaded: %s", ctx->path);
		if (ctx->artboard && *ctx->artboard)
			n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\nArtboard: %s", ctx->artboard);
		if (ctx->state_machine && *ctx->state_machine)
			n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\nState machine: %s", ctx->state_machine);
	} else if (ever) {
		n = snprintf(buf, sizeof(buf), "Pending reload: %s", ctx->path);
	} else {
		n = snprintf(buf, sizeof(buf), "Loading: %s", ctx->path);
	}
	(void)n;
	return bstrdup(buf);
}

// Logs an error if (and only if) the message differs from the last one we
// logged. Stops the OBS log from filling up when the hot-reload watcher keeps
// retrying a broken file.
static void rive_source_log_error_once(struct rive_source *ctx, const char *fmt, const char *msg)
{
	const char *m = msg && *msg ? msg : "unknown";
	if (ctx->logged_error && strcmp(ctx->logged_error, m) == 0)
		return;
	bfree(ctx->logged_error);
	ctx->logged_error = bstrdup(m);
	obs_log(LOG_WARNING, fmt, m);
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
		// gs_texture_create_from_iosurface (called inside renderer create)
		// requires the GS context to be active. video_tick runs on the
		// graphics thread but without it entered.
		obs_enter_graphics();
		ctx->renderer = rive_renderer_create(ctx->width, ctx->height, err, sizeof(err));
		obs_leave_graphics();
		if (!ctx->renderer) {
			obs_log(LOG_ERROR, "rive: renderer create failed: %s", err[0] ? err : "unknown");
			rive_source_set_status(ctx, false, err[0] ? err : "renderer create failed");
			return;
		}
	} else {
		obs_enter_graphics();
		rive_renderer_resize(ctx->renderer, ctx->width, ctx->height);
		obs_leave_graphics();
	}

	// Empty path is the "unloaded" state, not an error. Clear status and
	// short-circuit before touching the renderer.
	if (!ctx->path || !*ctx->path) {
		rive_renderer_set_file(ctx->renderer, "", "", "", NULL, 0);
		rive_source_set_status(ctx, true, NULL);
		bfree(ctx->logged_error);
		ctx->logged_error = NULL;
		ctx->file_mtime = 0;
		return;
	}

	char err[256];
	err[0] = '\0';
	const bool ok = rive_renderer_set_file(ctx->renderer, ctx->path, ctx->artboard ? ctx->artboard : "",
					       ctx->state_machine ? ctx->state_machine : "", err, sizeof(err));
	if (!ok) {
		rive_source_log_error_once(ctx, "rive: set_file failed: %s", err);
		rive_source_set_status(ctx, false, err[0] ? err : "set_file failed");
		return;
	}

	// Successful load. Record the mtime so the watcher has a baseline and
	// clear the dedup memo so future failures get logged again.
	struct stat sb;
	if (stat(ctx->path, &sb) == 0)
		ctx->file_mtime = (int64_t)sb.st_mtime;
	bfree(ctx->logged_error);
	ctx->logged_error = NULL;
	rive_source_set_status(ctx, true, NULL);
}

// Polls the .riv file's mtime. If it changed since the last successful load,
// mark the source dirty so rive_source_sync_renderer reloads it next tick.
// Cheap (one stat()) and rate-limited to FILE_WATCH_POLL_SECONDS. Always on:
// authors expect to export from Rive and see the new file pick up in OBS.
static void rive_source_poll_file_reload(struct rive_source *ctx, float dt)
{
	if (!ctx->path || !*ctx->path)
		return;
	ctx->watch_accum += dt;
	if (ctx->watch_accum < FILE_WATCH_POLL_SECONDS)
		return;
	ctx->watch_accum = 0.f;

	struct stat sb;
	if (stat(ctx->path, &sb) != 0)
		return;
	const int64_t mtime = (int64_t)sb.st_mtime;
	if (ctx->file_mtime == 0) {
		ctx->file_mtime = mtime;
		return;
	}
	if (mtime != ctx->file_mtime) {
		obs_log(LOG_INFO, "rive: file changed on disk, reloading: %s", ctx->path);
		ctx->file_mtime = mtime;
		// Force the renderer to drop its cached load and reread. The
		// renderer short-circuits when (path, artboard, sm) is
		// unchanged, so we briefly clear its synced selection by
		// pushing an empty path before the next sync.
		if (ctx->renderer)
			rive_renderer_set_file(ctx->renderer, "", "", "", NULL, 0);
		ctx->dirty = true;
	}
}

// Polls the user-supplied JSON file's mtime; when it changes (or on first
// load), parses it via the OBS data API and stashes the result in
// ctx->data_payload for the next push into the renderer. Cheap (one stat()
// + one parse on change) and rate-limited like the .riv watcher.
static void rive_source_poll_data_file(struct rive_source *ctx, float dt)
{
	if (!ctx->data_path || !*ctx->data_path)
		return;
	ctx->data_watch_accum += dt;
	if (ctx->data_watch_accum < FILE_WATCH_POLL_SECONDS)
		return;
	ctx->data_watch_accum = 0.f;

	struct stat sb;
	if (stat(ctx->data_path, &sb) != 0) {
		// Log once per distinct error; transient races (file being
		// written by an external tool) are common, so don't spam.
		const char *msg = "stat failed";
		if (!ctx->data_logged_error || strcmp(ctx->data_logged_error, msg) != 0) {
			bfree(ctx->data_logged_error);
			ctx->data_logged_error = bstrdup(msg);
			obs_log(LOG_WARNING, "rive: data file unreadable: %s", ctx->data_path);
		}
		return;
	}
	const int64_t mtime = (int64_t)sb.st_mtime;
	if (mtime == ctx->data_mtime && ctx->data_payload)
		return;

	obs_data_t *parsed = obs_data_create_from_json_file(ctx->data_path);
	if (!parsed) {
		const char *msg = "parse failed";
		if (!ctx->data_logged_error || strcmp(ctx->data_logged_error, msg) != 0) {
			bfree(ctx->data_logged_error);
			ctx->data_logged_error = bstrdup(msg);
			obs_log(LOG_WARNING, "rive: data file is not valid JSON: %s", ctx->data_path);
		}
		return;
	}

	if (ctx->data_payload)
		obs_data_release(ctx->data_payload);
	ctx->data_payload = parsed;
	ctx->data_mtime = mtime;
	bfree(ctx->data_logged_error);
	ctx->data_logged_error = NULL;
}

// ---- lifecycle -------------------------------------------------------------

static void *rive_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct rive_source *ctx = bzalloc(sizeof(struct rive_source));
	ctx->source = source;
	pthread_mutex_init(&ctx->status_mu, NULL);

	ctx->scene_state = obs_scene_state_create();

	rive_source_apply_settings(ctx, settings);

	obs_log(LOG_INFO, "Rive source created (file='%s')", ctx->path ? ctx->path : "");
	return ctx;
}

static void rive_source_destroy(void *data)
{
	struct rive_source *ctx = data;
	if (!ctx)
		return;
	if (ctx->scene_state) {
		obs_scene_state_destroy(ctx->scene_state);
		ctx->scene_state = NULL;
	}
	if (ctx->renderer) {
		obs_enter_graphics();
		rive_renderer_destroy(ctx->renderer);
		obs_leave_graphics();
		ctx->renderer = NULL;
	}
	bfree(ctx->path);
	bfree(ctx->artboard);
	bfree(ctx->state_machine);
	bfree(ctx->data_path);
	bfree(ctx->fit);
	bfree(ctx->alignment);
	bfree(ctx->last_error);
	bfree(ctx->logged_error);
	bfree(ctx->data_logged_error);
	if (ctx->data_payload) {
		obs_data_release(ctx->data_payload);
		ctx->data_payload = NULL;
	}
	pthread_mutex_destroy(&ctx->status_mu);
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

	// Default to the active OBS canvas size so a freshly added source
	// covers the scene without manual tweaking. obs_get_video_info returns
	// false if video isn't initialized yet (rare, but possible during
	// module load) — fall back to the static defaults in that case.
	uint32_t default_width = RIVE_SOURCE_DEFAULT_WIDTH;
	uint32_t default_height = RIVE_SOURCE_DEFAULT_HEIGHT;
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0) {
		default_width = ovi.base_width;
		default_height = ovi.base_height;
	}
	obs_data_set_default_int(settings, SK_WIDTH, default_width);
	obs_data_set_default_int(settings, SK_HEIGHT, default_height);
	obs_data_set_default_string(settings, SK_FIT, "contain");
	obs_data_set_default_string(settings, SK_ALIGNMENT, "center");
	obs_data_set_default_int(settings, SK_BG_COLOR, 0x00000000);
	obs_data_set_default_string(settings, SK_DATA_FILE, "");
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

static void populate_state_machines(obs_property_t *sm_prop, rive_file_t *file, const char *artboard_name)
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

static bool on_file_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
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
			obs_log(LOG_WARNING, "rive: failed to load '%s': %s", path, err[0] ? err : "unknown error");
	}

	populate_artboards(ab_prop, file);

	// If the previously selected artboard isn't in the new file, snap to
	// the first one and reset the SM choice to keep the UI consistent.
	const char *current_ab = obs_data_get_string(settings, SK_ARTBOARD);
	if (file && rive_file_find_artboard(file, current_ab) == SIZE_MAX) {
		const char *first = rive_file_artboard_count(file) > 0 ? rive_file_artboard_name(file, 0) : "";
		obs_data_set_string(settings, SK_ARTBOARD, first ? first : "");
		obs_data_set_string(settings, SK_STATE_MACHINE, "");
	}

	populate_state_machines(sm_prop, file, obs_data_get_string(settings, SK_ARTBOARD));

	rive_file_close(file);
	return true;
}

static bool on_artboard_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
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
		size_t ab_idx = rive_file_find_artboard(file, obs_data_get_string(settings, SK_ARTBOARD));
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

// Refresh button callback. Returning true forces the properties UI to be
// rebuilt — that's how we get the status info field to re-render with the
// most recent error / load state.
static bool on_refresh_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(data);
	return true;
}

static obs_properties_t *rive_source_get_properties(void *data)
{
	struct rive_source *ctx = data;

	obs_properties_t *props = obs_properties_create();

	// Status (read-only, computed from current source state). Placed at
	// the top so users see load errors before scrolling.
	if (ctx) {
		char *status = rive_source_format_status(ctx);
		obs_properties_add_text(props, SK_STATUS, status ? status : "", OBS_TEXT_INFO);
		bfree(status);
		obs_properties_add_button(props, SK_REFRESH, obs_module_text("Refresh"), on_refresh_clicked);
	}

	obs_property_t *p_file = obs_properties_add_path(props, SK_FILE, obs_module_text("File"), OBS_PATH_FILE,
							 "Rive files (*.riv);;All files (*.*)", NULL);
	obs_property_set_modified_callback(p_file, on_file_modified);

	obs_property_t *p_ab = obs_properties_add_list(props, SK_ARTBOARD, obs_module_text("Artboard"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(p_ab, on_artboard_modified);

	obs_properties_add_list(props, SK_STATE_MACHINE, obs_module_text("StateMachine"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);

	obs_properties_add_int(props, SK_WIDTH, obs_module_text("Width"), 1, 8192, 1);
	obs_properties_add_int(props, SK_HEIGHT, obs_module_text("Height"), 1, 8192, 1);

	obs_property_t *p_fit = obs_properties_add_list(props, SK_FIT, obs_module_text("Fit"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	for (size_t i = 0; i < FIT_COUNT; ++i)
		obs_property_list_add_string(p_fit, FIT_LABELS[i], FIT_VALUES[i]);

	obs_property_t *p_align = obs_properties_add_list(props, SK_ALIGNMENT, obs_module_text("Alignment"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (size_t i = 0; i < ALIGN_COUNT; ++i)
		obs_property_list_add_string(p_align, ALIGN_LABELS[i], ALIGN_VALUES[i]);

	obs_properties_add_color_alpha(props, SK_BG_COLOR, obs_module_text("BackgroundColor"));

	// Optional JSON file feeding the user-defined view model. Polled once
	// per second; see rive_custom_data.h for the value-mapping contract.
	obs_properties_add_path(props, SK_DATA_FILE, obs_module_text("DataFile"), OBS_PATH_FILE,
				"JSON files (*.json);;All files (*.*)", NULL);

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

	rive_source_poll_file_reload(ctx, seconds);
	rive_source_poll_data_file(ctx, seconds);
	rive_source_sync_renderer(ctx);

	if (ctx->renderer) {
		// Push the current OBS scene snapshot into the renderer's
		// SceneViewModel (silently no-ops if the .riv doesn't declare
		// one). Done *before* advance so this frame's state machine
		// can react to the new values.
		if (ctx->scene_state) {
			const struct rive_scene_state *state = obs_scene_state_refresh(ctx->scene_state);
			rive_renderer_apply_scene_state(ctx->renderer, state);
		}

		// Re-apply the cached JSON payload every tick. Cheap because
		// every scalar setter checks equality before writing; pushing
		// it again on each frame keeps things consistent if the SM
		// reset or rebound (e.g. via a state change) since the last
		// snapshot.
		if (ctx->data_payload)
			rive_renderer_apply_custom_data(ctx->renderer, ctx->data_payload);

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
			// On macOS, gs_texture_create_from_iosurface produces a
			// GL_TEXTURE_RECTANGLE — sampling it with the standard 2D
			// effect collapses every fragment to texel (0,0) and yields
			// solid clear-color. Use the rect-sampler effect there.
#ifdef __APPLE__
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT_RECT);
#else
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
#endif
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
