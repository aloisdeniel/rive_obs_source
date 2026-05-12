/*
 * rive_file: thin C wrapper around rive::File for the OBS plugin.
 *
 * Loads a .riv file and lets the rest of the (C) plugin enumerate the
 * artboards and state machines it contains so the properties UI can populate
 * its dropdowns. Rendering is intentionally out of scope here — this layer
 * uses rive::NoOpFactory and never touches the GPU.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rive_file rive_file_t;

rive_file_t *rive_file_open(const char *path, char *err, size_t err_size);
void rive_file_close(rive_file_t *file);

size_t rive_file_artboard_count(const rive_file_t *file);
const char *rive_file_artboard_name(const rive_file_t *file, size_t artboard_idx);
bool rive_file_artboard_size(const rive_file_t *file, size_t artboard_idx, uint32_t *out_w,
			     uint32_t *out_h);

size_t rive_file_state_machine_count(const rive_file_t *file, size_t artboard_idx);
const char *rive_file_state_machine_name(const rive_file_t *file, size_t artboard_idx, size_t i);

// Returns SIZE_MAX when the named artboard isn't present.
size_t rive_file_find_artboard(const rive_file_t *file, const char *name);

#ifdef __cplusplus
}
#endif
