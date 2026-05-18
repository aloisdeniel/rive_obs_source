/*
 * rive_custom_data: pushes user-supplied JSON into a Rive view model so authors
 * can data-bind arbitrary runtime values (clock, currently-played song, chat
 * messages, ...) into their .riv without touching the plugin source.
 *
 * The plugin watches a JSON file the user points it at; when the file changes,
 * the parsed obs_data_t is handed to apply() which walks it recursively against
 * a target view-model instance. The mapping is structural — no per-property
 * configuration:
 *
 *   { "title": "Hello",        ->   string  title;
 *     "level": 42,             ->   number  level;
 *     "live":  true,           ->   boolean live;
 *     "now":   {               ->   <NestedVM> now;
 *        "time": "12:34:56" }
 *     "chat": [                ->   list<ChatVM> chat;
 *        { "user": "alice",
 *          "text": "hi" } ] }
 *
 * Two reserved JSON keys carry binding metadata; both are stripped before the
 * remaining fields are written to the VM:
 *
 *   "type": names the view-model type for this object. Used to (a) resolve
 *           the root VM when the artboard has no default, (b) name the inner
 *           VM type for list items so empty lists can grow without an
 *           editor-provided example item, (c) self-document on nested-VM
 *           properties (informational — the parent schema already pins the
 *           type).
 *
 *   "id":   a stable identifier for this object. When the same id reappears
 *           in a later snapshot the plugin keeps the existing VM instance
 *           and just rewrites its properties, so animations, transitions,
 *           and any internal state on that instance survive. When ids
 *           change or disappear, the corresponding instances are recreated
 *           or removed:
 *             - Root: a different id replaces the root instance and rebinds
 *               the SM. (Heavy — most authors won't use this.)
 *             - Nested VM: a different id calls replaceViewModel() to swap
 *               the property's instance for a fresh one.
 *             - List: items are diffed by id. Matched items keep their
 *               instance; new ids spawn new instances; absent ids are
 *               removed. Items without an id fall back to position-based
 *               matching (so files that don't use ids work as before).
 *
 *   { "type": "MyDataVM", "id": "root",   // root identity
 *     "title": "Hello",
 *     "now": { "type": "Now", "id": "now-1",
 *              "time": "..." },
 *     "chat": [ { "type": "ChatMessage", "id": "msg-42",
 *                 "user": "alice", "text": "hi" } ] }
 *
 * Neither key is written to the VM (Rive properties literally named "type"
 * or "id" can't be set via JSON; rename them).
 *
 * Discovery (the "target view model" the JSON applies against):
 *   - If the file declares "SceneViewModel", the scene binding owns the SM
 *     root. Custom data is applied against that same root — usually via a
 *     nested-VM property the author added, e.g. JSON { "data": { ... } }.
 *   - Otherwise the root VM is resolved from JSON "type" (preferred) or, as
 *     a fallback, the artboard's default VM. We instantiate it, bind it to
 *     the SM, and apply the JSON against its properties.
 *
 * Like the scene binding, every lookup is forgiving — JSON keys with no match
 * are silently skipped so authors can declare any subset of fields.
 */

#pragma once

#ifdef __cplusplus

#include <memory>
#include <string>
#include <unordered_map>

#include <rive/refcnt.hpp>

struct obs_data;
typedef struct obs_data obs_data_t;

namespace rive {
class File;
class Artboard;
class StateMachineInstance;
class ViewModelInstanceRuntime;
} // namespace rive

namespace rive_obs {

class ImageCache;

// JSON keys reserved for binding metadata. Never written to the VM.
inline constexpr const char *kTypeKey = "type";
inline constexpr const char *kIdKey = "id";

class CustomDataBinding {
public:
	// Builds a binding. Two cases:
	//   - scene_root != null: feed JSON into the SceneViewModel root the
	//     scene binding already owns.
	//   - scene_root == null: root resolution is deferred to the first
	//     apply() so we can read the JSON's "type" field. If "type" is
	//     missing, we fall back to the artboard's default VM. Either way
	//     the resolved VM is bound to sm.
	// images may be null — in that case string values targeting image
	// properties are silently skipped.
	// Returns nullptr only if file is null.
	static std::unique_ptr<CustomDataBinding> tryCreate(rive::File *file, rive::Artboard *artboard,
							    rive::StateMachineInstance *sm,
							    rive::ViewModelInstanceRuntime *scene_root,
							    ImageCache *images);

	~CustomDataBinding();

	// Pushes one snapshot of JSON data into the bound view-model. data may
	// be null (treated as no-op). Cheap when nothing has changed — every
	// scalar setter checks equality before writing.
	void apply(obs_data_t *data);

private:
	// Per-VM-instance bookkeeping accumulated as we walk children. Lets us
	// diff lists by "id" across snapshots and detect when a nested-VM
	// property's id changed (so we replace it instead of mutating in
	// place). Keyed by the raw VM pointer; entries are pruned when we
	// drop or replace the corresponding instance ourselves.
	struct VMState {
		// nested-VM property name → (last-seen "id", current instance
		// pointer). Pointer is non-owning — Rive keeps the rcp via
		// the parent VM.
		std::unordered_map<std::string, std::pair<std::string, rive::ViewModelInstanceRuntime *>> nested;
		// list property name → (id → rcp of currently-listed item).
		// We hold rcps so items survive the removeAllInstances() call
		// during list reconciliation.
		std::unordered_map<std::string,
				   std::unordered_map<std::string, rive::rcp<rive::ViewModelInstanceRuntime>>>
			list_items;
	};

	CustomDataBinding(rive::File *file, rive::Artboard *artboard, rive::StateMachineInstance *sm,
			  rive::ViewModelInstanceRuntime *scene_root, ImageCache *images);

	// First-apply-only when no SceneViewModel: pick a VM by JSON "type"
	// (or the artboard default as a fallback), instantiate it, bind it
	// to the SM, and stash it as the JSON root.
	void resolveRoot(obs_data_t *data);

	// Recursive walk: for each key in obj (skipping reserved keys), find
	// a matching property on vm and dispatch by type.
	void applyObject(rive::ViewModelInstanceRuntime *vm, obs_data_t *obj);

	// Drop tracked state for vm and any descendants we tracked under it.
	// Called when an instance is replaced or removed from a list.
	void forgetState(rive::ViewModelInstanceRuntime *vm);

	rive::File *m_file;
	rive::Artboard *m_artboard;       // for default-VM fallback during resolveRoot
	rive::StateMachineInstance *m_sm; // bound during resolveRoot, and re-bound on root id change
	ImageCache *m_images;             // shared with the renderer; null when image loading is disabled
	bool m_rootResolved;              // resolveRoot ran (succeeded or gave up)
	bool m_routingResolved = false;   // first-apply routing decision computed

	// Last-seen id of the root, or "" if untracked. A change here triggers
	// root replacement (only meaningful when we own the root).
	std::string m_rootId;

	// In the "no SceneViewModel" path we own the freshly created instance
	// so it stays alive for the lifetime of the binding. In the shared
	// path it's null (the scene binding owns it). Replaced when the root
	// id changes.
	rive::rcp<rive::ViewModelInstanceRuntime> m_ownedRoot;
	// May be null until the first apply() resolves it. Apply no-ops while
	// null.
	rive::ViewModelInstanceRuntime *m_root;
	// Where the JSON is actually applied. Equals m_root by default, but
	// when the JSON's top-level "type" names a nested-VM property of the
	// SceneViewModel root, m_jsonTarget is set to that nested instance so
	// the user can use a flat JSON layout instead of wrapping it.
	rive::ViewModelInstanceRuntime *m_jsonTarget = nullptr;

	std::unordered_map<rive::ViewModelInstanceRuntime *, VMState> m_state;
};

} // namespace rive_obs

#endif // __cplusplus
