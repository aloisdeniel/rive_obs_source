/*
 * obs_scene_state: gathers the current OBS scene's state and pushes it into
 * a rive_renderer's bound SceneViewModel.
 *
 * Runs from the source's video_tick (graphics thread). libobs's source
 * enumeration and per-source getters are documented as thread-safe (each
 * source holds its own ref-count and per-source locks), and the OBS pattern
 * used by other plugins (e.g. obs-studio's own move-source filter) is to read
 * scene-item state from the graphics-tick callback, so we follow suit.
 *
 * The struct & lists are reused across ticks; the layer owns the backing
 * memory and the strings it points at.
 */

#pragma once

#include "rive_scene_view_model.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct obs_scene_state_collector obs_scene_state_collector_t;

obs_scene_state_collector_t *obs_scene_state_create(void);
void obs_scene_state_destroy(obs_scene_state_collector_t *c);

// Refreshes the collector by querying obs-frontend + libobs and rebuilding the
// scene/audio source lists. Returns a pointer to the collector's internal
// rive_scene_state; valid until the next call to obs_scene_state_refresh or
// obs_scene_state_destroy.
const struct rive_scene_state *obs_scene_state_refresh(obs_scene_state_collector_t *c);

#ifdef __cplusplus
}
#endif
