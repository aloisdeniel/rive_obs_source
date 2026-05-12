/*
 * rive_renderer: per-source GPU bridge between the Rive runtime and libobs.
 *
 * The plugin owns one of these per OBS source. Internally it picks the right
 * backend at build time:
 *   - macOS: rive_renderer_mac.mm (Metal + IOSurface bridged to a GL texture)
 *   - Windows: rive_renderer_win.cpp (D3D11 shared NT-handle texture)
 *
 * All entry points must be called from the OBS graphics thread (inside
 * video_tick / video_render, or under obs_enter_graphics()).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gs_texture;
typedef struct gs_texture gs_texture_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rive_renderer rive_renderer_t;

// Creates an empty renderer sized width x height. No .riv loaded yet — call
// rive_renderer_set_file() to point it at content.
rive_renderer_t *rive_renderer_create(uint32_t width, uint32_t height, char *err,
				      size_t err_size);

void rive_renderer_destroy(rive_renderer_t *r);

// Resizes the render target. Returns false on GPU resource failure (the
// previous texture is preserved in that case).
bool rive_renderer_resize(rive_renderer_t *r, uint32_t width, uint32_t height);

// Loads / replaces the .riv file and selects an artboard + state machine.
// Empty / null artboard or state machine selects the default for the file.
// Returns false on load failure. After failure, the renderer is left without
// content (rive_renderer_render() will just clear).
bool rive_renderer_set_file(rive_renderer_t *r, const char *path, const char *artboard_name,
			    const char *state_machine_name, char *err, size_t err_size);

// Advances the running state machine by dt seconds. No-op if no SM loaded.
void rive_renderer_advance(rive_renderer_t *r, float dt_seconds);

// Renders one frame.
//   fit_value / alignment_value: strings matching the FIT_VALUES / ALIGN_VALUES
//     in plugin-main.c (e.g. "contain", "center"). NULL falls back to defaults.
//   bg_color: RGBA in OBS's encoding (byte 0 = R, byte 3 = A).
void rive_renderer_render(rive_renderer_t *r, const char *fit_value,
			  const char *alignment_value, uint32_t bg_color);

// Returns the gs_texture_t the source should sample, or NULL when nothing has
// been rendered yet. The renderer retains ownership.
gs_texture_t *rive_renderer_get_texture(rive_renderer_t *r);

// Fires the named trigger input on the active state machine. Returns true if
// an input with that name was found. Returns false if no input matches OR if
// no state machine is loaded — callers that distinguish "missing input" from
// "no SM" should check rive_renderer_has_state_machine() first.
bool rive_renderer_fire_trigger(rive_renderer_t *r, const char *trigger_name);

// True if a state machine instance is currently loaded and ready to receive
// trigger inputs. Used by the source layer to suppress "unknown trigger"
// warnings while the file is still loading or unset.
bool rive_renderer_has_state_machine(rive_renderer_t *r);

#ifdef __cplusplus
}
#endif
