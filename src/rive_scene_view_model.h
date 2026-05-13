/*
 * rive_scene_view_model: bridges the active OBS scene's state into a Rive
 * view-model instance bound to the loaded state machine.
 *
 * Convention (the .riv author is expected to declare these by name):
 *   - View model "SceneViewModel" (bound as the SM's root data context)
 *       string  name              -- current scene name
 *       number  width             -- canvas width in pixels
 *       number  height            -- canvas height in pixels
 *       boolean streaming         -- true while streaming
 *       boolean recording         -- true while recording
 *       boolean recordingPaused   -- true while recording is paused
 *       number  sourceCount       -- number of scene items
 *       number  audioCount        -- number of audio inputs
 *       list    sources           -- items of an inner VM with the fields below
 *       list    audioSources      -- items of an inner VM with the fields below
 *
 *   - View model used as items of "sources" (any name, picked up from the
 *     list property at runtime):
 *       string  name              -- source name
 *       number  x, y              -- top-left of the source AABB in canvas px
 *       number  width, height     -- AABB size in canvas px
 *       boolean visible           -- visibility flag of the scene item
 *
 *   - View model used as items of "audioSources":
 *       string  name              -- audio source name
 *       number  volume            -- linear 0..1 (matches obs_source_get_volume)
 *       boolean muted             -- mute flag
 *
 * Any property the author omits is silently skipped — the binding is forgiving
 * so authors can adopt a subset.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rive_scene_source {
	const char *name;
	float x;
	float y;
	float width;
	float height;
	bool visible;
};

struct rive_scene_audio {
	const char *name;
	float volume;
	bool muted;
};

struct rive_scene_state {
	const char *scene_name;
	float canvas_width;
	float canvas_height;
	bool streaming;
	bool recording;
	bool recording_paused;

	const struct rive_scene_source *sources;
	size_t source_count;

	const struct rive_scene_audio *audio_sources;
	size_t audio_count;
};

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <rive/refcnt.hpp>

namespace rive {
class File;
class StateMachineInstance;
class ViewModelInstanceRuntime;
class ViewModelInstanceListRuntime;
} // namespace rive

namespace rive_obs {

// The conventional view-model name we look for in the loaded file.
inline constexpr const char *kSceneViewModelName = "SceneViewModel";

// Per-source-instance view-model binding. Lifetime is tied to a loaded
// (rive::File, rive::StateMachineInstance) pair. Re-create whenever the file
// or state machine identity changes.
class SceneViewModelBinding {
public:
	// Looks up a view model called "SceneViewModel" in file. If absent, returns
	// nullptr — the renderer should treat this as "the file isn't using a
	// scene view model" and skip scene-state propagation.
	//
	// If the view model exists but the state machine is null, the binding is
	// created without binding to a state machine (useful for files that draw
	// a single artboard with no SM). The state machine can be re-supplied
	// later via rebind().
	static std::unique_ptr<SceneViewModelBinding> tryCreate(rive::File *file,
								rive::StateMachineInstance *sm);

	~SceneViewModelBinding();

	// Pushes one frame of OBS scene state into the bound view-model instance.
	// Cheap when nothing has changed (every value setter checks equality).
	void apply(const rive_scene_state &state);

	// Exposes the bound SceneViewModel root so the custom-data binding can
	// share it (rather than instantiating its own VM and competing for the
	// SM's data context). Stays valid for the binding's lifetime.
	rive::ViewModelInstanceRuntime *root() const { return m_root.get(); }

private:
	SceneViewModelBinding(rive::File *file,
			      rive::rcp<rive::ViewModelInstanceRuntime> root);

	// Resize a list of inner view-model instances to match desired_count by
	// adding/removing items. Returns the list, or nullptr if the list
	// property is missing.
	rive::ViewModelInstanceListRuntime *resizeList(const char *property_name,
						       const std::string &item_vm_name,
						       size_t desired_count);

	rive::File *m_file;
	rive::rcp<rive::ViewModelInstanceRuntime> m_root;

	// Cached "inner view model name" per list. Looked up the first time we
	// need to add an item to that list, by inspecting an existing one if
	// present, otherwise by trying conventional names ("SourceViewModel",
	// "AudioSourceViewModel"). Empty string means "give up adding items to
	// this list" so we don't retry on every frame.
	std::string m_sourcesItemVmName;
	std::string m_audioItemVmName;

	// VM names we already tried to look up via viewModelByName() and got
	// back null. Used to suppress per-frame warnings when a list's
	// fallback inner-VM name doesn't exist in the file.
	std::unordered_set<std::string> m_unknownItemVmNames;
};

} // namespace rive_obs

#endif // __cplusplus
