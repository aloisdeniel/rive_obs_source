/*
 * obs_events: see obs_events.h for the contract. This file implements the
 * mapping table, the UI-thread frontend callback, and the graphics-thread
 * drain.
 */

#include "obs_events.h"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct EventDef {
	obs_frontend_event id;
	const char *setting_key;
	const char *label;
	const char *group;
	const char *group_label;
};

// Stable order; setting keys are persisted in scene-collection JSON. The
// plan's "TRANSITION_STARTED" maps to OBS's TRANSITION_CHANGED, which is the
// API name for the same observable event.
const EventDef kEvents[] = {
	{OBS_FRONTEND_EVENT_STREAMING_STARTING, "evt_streaming_starting", "Streaming starting",
	 "streaming", "Streaming"},
	{OBS_FRONTEND_EVENT_STREAMING_STARTED, "evt_streaming_started", "Streaming started",
	 "streaming", "Streaming"},
	{OBS_FRONTEND_EVENT_STREAMING_STOPPING, "evt_streaming_stopping", "Streaming stopping",
	 "streaming", "Streaming"},
	{OBS_FRONTEND_EVENT_STREAMING_STOPPED, "evt_streaming_stopped", "Streaming stopped",
	 "streaming", "Streaming"},
	{OBS_FRONTEND_EVENT_RECORDING_STARTING, "evt_recording_starting", "Recording starting",
	 "recording", "Recording"},
	{OBS_FRONTEND_EVENT_RECORDING_STARTED, "evt_recording_started", "Recording started",
	 "recording", "Recording"},
	{OBS_FRONTEND_EVENT_RECORDING_STOPPING, "evt_recording_stopping", "Recording stopping",
	 "recording", "Recording"},
	{OBS_FRONTEND_EVENT_RECORDING_STOPPED, "evt_recording_stopped", "Recording stopped",
	 "recording", "Recording"},
	{OBS_FRONTEND_EVENT_RECORDING_PAUSED, "evt_recording_paused", "Recording paused",
	 "recording", "Recording"},
	{OBS_FRONTEND_EVENT_RECORDING_UNPAUSED, "evt_recording_unpaused", "Recording unpaused",
	 "recording", "Recording"},
	{OBS_FRONTEND_EVENT_SCENE_CHANGED, "evt_scene_changed", "Scene changed", "scene",
	 "Scenes & transitions"},
	{OBS_FRONTEND_EVENT_TRANSITION_CHANGED, "evt_transition_started", "Transition started",
	 "scene", "Scenes & transitions"},
	{OBS_FRONTEND_EVENT_TRANSITION_STOPPED, "evt_transition_stopped", "Transition stopped",
	 "scene", "Scenes & transitions"},
};
constexpr size_t kEventCount = sizeof(kEvents) / sizeof(kEvents[0]);

size_t event_index_for(obs_frontend_event ev)
{
	for (size_t i = 0; i < kEventCount; ++i) {
		if (kEvents[i].id == ev)
			return i;
	}
	return SIZE_MAX;
}

} // namespace

struct rive_events {
	bool attached = false;

	// Guards mapping, queue, warned. Held for very short critical sections
	// so the UI-thread frontend callback never blocks the graphics thread.
	std::mutex mu;
	std::string mapping[kEventCount];
	std::vector<std::string> queue;
	std::unordered_set<std::string> warned;
};

namespace {

void frontend_cb(obs_frontend_event ev, void *user)
{
	auto *e = static_cast<rive_events *>(user);
	if (!e)
		return;
	const size_t idx = event_index_for(ev);
	if (idx == SIZE_MAX)
		return;

	std::lock_guard<std::mutex> lk(e->mu);
	const std::string &name = e->mapping[idx];
	if (name.empty())
		return;
	e->queue.push_back(name);
}

} // namespace

extern "C" {

rive_events_t *rive_events_create(void)
{
	return new rive_events();
}

void rive_events_destroy(rive_events_t *e)
{
	if (!e)
		return;
	rive_events_detach(e);
	delete e;
}

void rive_events_attach(rive_events_t *e)
{
	if (!e || e->attached)
		return;
	obs_frontend_add_event_callback(frontend_cb, e);
	e->attached = true;
}

void rive_events_detach(rive_events_t *e)
{
	if (!e || !e->attached)
		return;
	obs_frontend_remove_event_callback(frontend_cb, e);
	e->attached = false;
}

void rive_events_update_mapping(rive_events_t *e, obs_data_t *settings)
{
	if (!e || !settings)
		return;
	std::lock_guard<std::mutex> lk(e->mu);
	for (size_t i = 0; i < kEventCount; ++i) {
		const char *v = obs_data_get_string(settings, kEvents[i].setting_key);
		e->mapping[i] = v ? v : "";
	}
}

void rive_events_drain(rive_events_t *e, rive_events_fire_fn fire, void *user_data)
{
	if (!e || !fire)
		return;

	std::vector<std::string> pending;
	{
		std::lock_guard<std::mutex> lk(e->mu);
		pending.swap(e->queue);
	}
	for (auto &name : pending) {
		if (fire(name.c_str(), user_data))
			continue;
		std::lock_guard<std::mutex> lk(e->mu);
		if (e->warned.insert(name).second) {
			obs_log(LOG_WARNING,
				"rive: event mapping refers to trigger '%s', but no input "
				"with that name was found on the active state machine",
				name.c_str());
		}
	}
}

void rive_events_reset_warnings(rive_events_t *e)
{
	if (!e)
		return;
	std::lock_guard<std::mutex> lk(e->mu);
	e->warned.clear();
}

size_t rive_events_count(void)
{
	return kEventCount;
}

const char *rive_events_setting_key(size_t i)
{
	return i < kEventCount ? kEvents[i].setting_key : nullptr;
}

const char *rive_events_label(size_t i)
{
	return i < kEventCount ? kEvents[i].label : nullptr;
}

const char *rive_events_group(size_t i)
{
	return i < kEventCount ? kEvents[i].group : nullptr;
}

const char *rive_events_group_label(size_t i)
{
	return i < kEventCount ? kEvents[i].group_label : nullptr;
}

} // extern "C"
