/*
 * obs_events: per-source bridge from obs-frontend-api events to Rive state
 * machine trigger inputs.
 *
 * Each rive_source owns one rive_events_t. It:
 *   - subscribes to obs_frontend_add_event_callback on attach,
 *   - holds an event -> trigger-name mapping read from obs_data_t settings,
 *   - enqueues a trigger name when a mapped event fires (UI thread),
 *   - is drained from the graphics thread via rive_events_drain, which fires
 *     each trigger through the source-provided callback.
 *
 * The mapping and queue live behind a mutex so the UI-thread callback and
 * the graphics-thread drain never race.
 */

#pragma once

#include <obs.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rive_events rive_events_t;

// Invoked by rive_events_drain for each queued trigger name. Returning false
// signals "no input by that name on the active state machine" so the events
// layer can log a one-shot warning. The callback must only return false when
// a state machine *is* loaded — otherwise the warning will be misleading.
typedef bool (*rive_events_fire_fn)(const char *trigger_name, void *user_data);

rive_events_t *rive_events_create(void);
void rive_events_destroy(rive_events_t *e);

// Subscribe / unsubscribe to obs_frontend_add_event_callback. Both are
// idempotent. Subscription is per-source (not module-wide) per the plan,
// so callbacks never fire before the source exists.
void rive_events_attach(rive_events_t *e);
void rive_events_detach(rive_events_t *e);

// Refresh the event -> trigger mapping from the source's persisted settings.
// Safe to call from any thread; takes the internal mutex briefly.
void rive_events_update_mapping(rive_events_t *e, obs_data_t *settings);

// Drain the pending-trigger queue. For each entry, calls fire(); if fire
// returns false, logs a one-shot warning per (trigger name) per handle.
void rive_events_drain(rive_events_t *e, rive_events_fire_fn fire, void *user_data);

// Clear the "already warned about this trigger name" memo — call when the
// active state machine changes, so a name that's now valid stops warning and
// a name that's still invalid warns once for the new SM.
void rive_events_reset_warnings(rive_events_t *e);

// Property-layout helpers. Iterate 0..rive_events_count() to enumerate all
// supported events in stable order; entries with the same group string are
// meant to share an obs_properties_add_group.
size_t rive_events_count(void);
const char *rive_events_setting_key(size_t i);
const char *rive_events_label(size_t i);
const char *rive_events_group(size_t i);
const char *rive_events_group_label(size_t i);

#ifdef __cplusplus
}
#endif
