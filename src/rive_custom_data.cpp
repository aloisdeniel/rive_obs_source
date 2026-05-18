/*
 * rive_custom_data: see header for the contract.
 *
 * Walks an obs_data_t (obs's JSON DOM) against a target view-model instance
 * recursively. Strings/numbers/booleans become scalar setters; objects recurse
 * into nested-VM properties; arrays manage list size and recurse per item.
 *
 * Two reserved JSON keys are stripped before writing:
 *   - "type": resolves view-model types (root, list items)
 *   - "id":   carries object identity across snapshots so VM instances are
 *            preserved (and animations / internal state survive) when the
 *            same id reappears, and replaced/removed when ids change.
 *
 * Like the scene binding, every lookup is forgiving — JSON keys with no match
 * on the VM are silently skipped so authors only declare the fields they care
 * about. We don't even know whether mismatches are typos or intentional.
 */

#include "rive_custom_data.h"
#include "rive_image_cache.h"
#include "rive_scene_view_model.h"

// Rive headers first (see comment in rive_renderer_mac.mm).
#include <rive/file.hpp>
#include <rive/artboard.hpp>
#include <rive/animation/state_machine_instance.hpp>
#include <rive/viewmodel/runtime/viewmodel_runtime.hpp>
#include <rive/viewmodel/runtime/viewmodel_instance_runtime.hpp>
#include <rive/viewmodel/runtime/viewmodel_instance_list_runtime.hpp>
#include <rive/viewmodel/runtime/viewmodel_instance_string_runtime.hpp>
#include <rive/viewmodel/runtime/viewmodel_instance_number_runtime.hpp>
#include <rive/viewmodel/runtime/viewmodel_instance_boolean_runtime.hpp>
#include <rive/viewmodel/runtime/viewmodel_instance_color_runtime.hpp>
#include <rive/viewmodel/runtime/viewmodel_instance_asset_image_runtime.hpp>

#include <obs.h>
#include <obs-data.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace rive_obs {

namespace {

// Same equality-check pattern as rive_scene_view_model.cpp so we don't dirty
// the property graph on every push when the data hasn't changed.
void set_string_if_present(rive::ViewModelInstanceRuntime *vm, const char *name, const char *value)
{
	if (!vm)
		return;
	auto *p = vm->propertyString(name);
	if (!p)
		return;
	std::string desired = value ? value : "";
	if (p->value() != desired) {
		p->value(desired);
		obs_log(LOG_INFO, "rive: set string '%s'.'%s' = '%s' (read-back: '%s', vm=%p, prop=%p)",
			vm->viewModelName().c_str(), name, desired.c_str(), p->value().c_str(), (void *)vm, (void *)p);
		FILE *f = std::fopen("/tmp/rive-bind.log", "a");
		if (f) {
			std::fprintf(f, "set string '%s' vm=%p underlying=%p value=%s\n", name, (void *)vm,
				     (void *)vm->instance().get(), desired.c_str());
			std::fclose(f);
		}
	}
}

void set_number_if_present(rive::ViewModelInstanceRuntime *vm, const char *name, double value)
{
	if (!vm)
		return;
	auto *p = vm->propertyNumber(name);
	if (!p)
		return;
	const float v = static_cast<float>(value);
	if (std::fabs(p->value() - v) <= 1e-3f)
		return;
	p->value(v);
}

void set_boolean_if_present(rive::ViewModelInstanceRuntime *vm, const char *name, bool value)
{
	if (!vm)
		return;
	auto *p = vm->propertyBoolean(name);
	if (!p)
		return;
	if (p->value() != value)
		p->value(value);
}

// String going to an image-typed property: resolve via the cache (kicks off a
// fetch the first time we see a URL) and assign once it's loaded. Empty
// string clears the property. No-op when the property isn't an image, when
// no cache is available, or when the image hasn't finished loading yet — the
// next apply() will pick it up.
void set_image_if_present(rive::ViewModelInstanceRuntime *vm, const char *name, const char *url, ImageCache *cache)
{
	if (!vm)
		return;
	auto *p = vm->propertyImage(name);
	if (!p)
		return;
	if (!url || !*url) {
		p->value(nullptr);
		return;
	}
	if (!cache)
		return;
	rive::RenderImage *img = cache->request(url);
	if (img) {
		p->value(img);
		obs_log(LOG_INFO, "rive: set image '%s'.'%s' = '%s'", vm->viewModelName().c_str(), name, url);
	}
}

// Parses a "#RRGGBB" or "#AARRGGBB" hex color string into a packed
// 0xAARRGGBB int (the layout Rive's color runtime expects). The leading
// '#' is optional. 6-digit strings are treated as fully opaque. Returns
// false (and leaves `out` untouched) on any malformed input so callers
// can treat non-color strings as a no-op.
bool parse_hex_color(const char *str, uint32_t &out)
{
	if (!str)
		return false;
	if (*str == '#')
		++str;
	const std::size_t len = std::strlen(str);
	if (len != 6 && len != 8)
		return false;

	uint32_t acc = 0;
	for (std::size_t i = 0; i < len; ++i) {
		const char c = str[i];
		uint32_t nibble;
		if (c >= '0' && c <= '9')
			nibble = static_cast<uint32_t>(c - '0');
		else if (c >= 'a' && c <= 'f')
			nibble = static_cast<uint32_t>(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			nibble = static_cast<uint32_t>(c - 'A' + 10);
		else
			return false;
		acc = (acc << 4) | nibble;
	}

	// 6-digit form carries no alpha; force fully opaque.
	if (len == 6)
		acc |= 0xFF000000u;
	out = acc;
	return true;
}

// String going to a color-typed property. No-op when the property isn't
// a color or when the string isn't a valid #RRGGBB / #AARRGGBB value, so
// it's safe to try alongside the other string setters.
void set_color_if_present(rive::ViewModelInstanceRuntime *vm, const char *name, const char *value)
{
	if (!vm)
		return;
	auto *p = vm->propertyColor(name);
	if (!p)
		return;
	uint32_t argb;
	if (!parse_hex_color(value, argb))
		return;
	const int v = static_cast<int>(argb);
	if (p->value() != v) {
		p->value(v);
		obs_log(LOG_INFO, "rive: set color '%s'.'%s' = '%s' (0x%08X)", vm->viewModelName().c_str(), name,
			value ? value : "", argb);
	}
}

// Reads obj[key] as a string, or "" if absent / wrong type.
std::string read_string_field(obs_data_t *obj, const char *key)
{
	if (!obj || !key)
		return "";
	std::string out;
	obs_data_item_t *it = obs_data_item_byname(obj, key);
	if (it) {
		if (obs_data_item_gettype(it) == OBS_DATA_STRING) {
			const char *s = obs_data_item_get_string(it);
			if (s)
				out = s;
		}
		obs_data_item_release(&it);
	}
	return out;
}

// Convenience wrappers so the call sites read like the contract.
std::string read_type_field(obs_data_t *obj)
{
	return read_string_field(obj, kTypeKey);
}
std::string read_id_field(obs_data_t *obj)
{
	return read_string_field(obj, kIdKey);
}

// True when name is one of our reserved metadata keys (skip when applying).
bool is_reserved_key(const char *name)
{
	return name && (std::strcmp(name, kTypeKey) == 0 || std::strcmp(name, kIdKey) == 0);
}

// Returns the view-model name to use for items of `list`. Prefers JSON's
// "type" on the first array element (lets empty lists grow without an
// editor-provided sample); falls back to inspecting an existing item.
std::string resolve_list_item_vm_name(rive::ViewModelInstanceListRuntime *list, obs_data_array_t *arr)
{
	if (arr && obs_data_array_count(arr) > 0) {
		obs_data_t *first = obs_data_array_item(arr, 0);
		std::string from_json = read_type_field(first);
		obs_data_release(first);
		if (!from_json.empty())
			return from_json;
	}
	if (list && list->size() > 0) {
		auto first = list->instanceAt(0);
		if (first)
			return first->viewModelName();
	}
	return "";
}

} // namespace

std::unique_ptr<CustomDataBinding> CustomDataBinding::tryCreate(rive::File *file, rive::Artboard *artboard,
								rive::StateMachineInstance *sm,
								rive::ViewModelInstanceRuntime *scene_root,
								ImageCache *images)
{
	if (!file)
		return nullptr;
	return std::unique_ptr<CustomDataBinding>(new CustomDataBinding(file, artboard, sm, scene_root, images));
}

CustomDataBinding::CustomDataBinding(rive::File *file, rive::Artboard *artboard, rive::StateMachineInstance *sm,
				     rive::ViewModelInstanceRuntime *scene_root, ImageCache *images)
	: m_file(file),
	  m_artboard(artboard),
	  m_sm(sm),
	  m_images(images),
	  m_rootResolved(scene_root != nullptr),
	  m_root(scene_root)
{
}

CustomDataBinding::~CustomDataBinding() = default;

void CustomDataBinding::resolveRoot(obs_data_t *data)
{
	if (m_rootResolved)
		return;
	// One-shot: even if we fail to find a VM we won't retry every frame.
	// The user can fix the JSON / file and reload to re-trigger resolution.
	m_rootResolved = true;

	rive::ViewModelRuntime *vm_runtime = nullptr;

	const std::string typed = read_type_field(data);
	if (!typed.empty()) {
		vm_runtime = m_file->viewModelByName(typed);
		if (!vm_runtime) {
			obs_log(LOG_WARNING,
				"rive: custom data 'type' = '%s' but no view model with "
				"that name exists in the file",
				typed.c_str());
		}
	}

	// Fall back to whatever VM the artboard was authored against.
	if (!vm_runtime && m_artboard)
		vm_runtime = m_file->defaultArtboardViewModel(m_artboard);

	if (!vm_runtime)
		return;

	rive::rcp<rive::ViewModelInstanceRuntime> root = vm_runtime->createDefaultInstance();
	if (!root)
		root = vm_runtime->createInstance();
	if (!root)
		return;

	if (m_sm)
		m_sm->bindViewModelInstance(root->instance());

	m_ownedRoot = std::move(root);
	m_root = m_ownedRoot.get();
	m_rootId = read_id_field(data);
}

void CustomDataBinding::apply(obs_data_t *data)
{
	if (!data)
		return;
	if (!m_root)
		resolveRoot(data);
	if (!m_root)
		return;

	// First-apply routing decision: if SceneViewModel owns the SM root and
	// the JSON's top-level "type" names a different VM, look for a
	// nested-VM property of the root with that type and route the JSON
	// into it. Lets users keep a flat JSON layout instead of wrapping it.
	if (!m_routingResolved) {
		m_routingResolved = true;
		m_jsonTarget = m_root;
		const std::string requested = read_type_field(data);
		if (m_ownedRoot == nullptr && !requested.empty() && requested != m_root->viewModelName()) {
			const auto props = m_root->properties();
			for (const auto &pd : props) {
				if (pd.type != rive::DataType::viewModel)
					continue;
				auto nested = m_root->propertyViewModel(pd.name);
				if (nested && nested->viewModelName() == requested) {
					m_jsonTarget = nested.get();
					obs_log(LOG_INFO,
						"rive: custom data routed into '%s.%s' "
						"(type '%s')",
						m_root->viewModelName().c_str(), pd.name.c_str(), requested.c_str());
					break;
				}
			}
			if (m_jsonTarget == m_root) {
				obs_log(LOG_WARNING,
					"rive: custom data 'type' = '%s' but '%s' has "
					"no nested view-model property of that type — "
					"JSON keys will only match properties of '%s' "
					"itself",
					requested.c_str(), m_root->viewModelName().c_str(),
					m_root->viewModelName().c_str());
			}
		} else {
			obs_log(LOG_INFO, "rive: custom data target = '%s'", m_root->viewModelName().c_str());
		}

		// One-shot dump of the routed target's properties so we can see at
		// a glance whether the JSON keys have any chance of matching.
		auto dump_props = [](const char *label, rive::ViewModelInstanceRuntime *vm) {
			if (!vm)
				return;
			const auto props = vm->properties();
			for (const auto &pd : props) {
				const char *t = "?";
				switch (pd.type) {
				case rive::DataType::string:
					t = "string";
					break;
				case rive::DataType::number:
					t = "number";
					break;
				case rive::DataType::boolean:
					t = "boolean";
					break;
				case rive::DataType::color:
					t = "color";
					break;
				case rive::DataType::list:
					t = "list";
					break;
				case rive::DataType::enumType:
					t = "enum";
					break;
				case rive::DataType::trigger:
					t = "trigger";
					break;
				case rive::DataType::viewModel:
					t = "viewModel";
					break;
				case rive::DataType::assetImage:
					t = "image";
					break;
				case rive::DataType::artboard:
					t = "artboard";
					break;
				default:
					break;
				}
				obs_log(LOG_INFO, "rive: %s '%s'.'%s' : %s", label, vm->viewModelName().c_str(),
					pd.name.c_str(), t);
			}
		};
		dump_props("root prop", m_root);
		if (m_jsonTarget && m_jsonTarget != m_root)
			dump_props("target prop", m_jsonTarget);
	}

	// Detect a root-id change. Only meaningful when we own the root —
	// SceneViewModel's root is owned by the scene binding and we don't
	// touch its identity.
	if (m_ownedRoot) {
		const std::string new_id = read_id_field(data);
		if (!new_id.empty() && !m_rootId.empty() && new_id != m_rootId) {
			std::string vm_type = read_type_field(data);
			if (vm_type.empty())
				vm_type = m_ownedRoot->viewModelName();
			rive::ViewModelRuntime *vm_runtime = m_file->viewModelByName(vm_type);
			if (vm_runtime) {
				rive::rcp<rive::ViewModelInstanceRuntime> fresh = vm_runtime->createInstance();
				if (fresh) {
					forgetState(m_ownedRoot.get());
					if (m_sm)
						m_sm->bindViewModelInstance(fresh->instance());
					m_ownedRoot = std::move(fresh);
					m_root = m_ownedRoot.get();
				}
			}
		}
		m_rootId = new_id;
		// When we own the root and just replaced it, m_jsonTarget needs
		// to track the new root pointer.
		if (m_jsonTarget != m_root)
			m_jsonTarget = m_root;
	}

	applyObject(m_jsonTarget ? m_jsonTarget : m_root, data);
}

void CustomDataBinding::forgetState(rive::ViewModelInstanceRuntime *vm)
{
	if (!vm)
		return;
	auto it = m_state.find(vm);
	if (it == m_state.end())
		return;

	// Recursively clean up any children we tracked under vm. If we don't,
	// orphaned entries linger and a future allocator-recycled VM* could
	// inherit stale state. nested[].second and list_items items are the
	// only VM pointers we ever stash, so walking them is sufficient.
	for (auto &n : it->second.nested)
		forgetState(n.second.second);
	for (auto &lp : it->second.list_items) {
		for (auto &kv : lp.second)
			forgetState(kv.second.get());
	}

	m_state.erase(it);
}

void CustomDataBinding::applyObject(rive::ViewModelInstanceRuntime *vm, obs_data_t *obj)
{
	if (!vm || !obj)
		return;

	VMState &state = m_state[vm];

	for (obs_data_item_t *it = obs_data_first(obj); it; obs_data_item_next(&it)) {
		const char *name = obs_data_item_get_name(it);
		if (!name || !*name)
			continue;
		if (is_reserved_key(name))
			continue;
		const enum obs_data_type type = obs_data_item_gettype(it);

		switch (type) {
		case OBS_DATA_STRING: {
			const char *s = obs_data_item_get_string(it);
			// Try string first; if the property is actually an
			// image, propertyString returned null and we route
			// through the image cache instead. Both setters are
			// no-ops when the property name doesn't match their
			// type, so trying both is harmless.
			set_string_if_present(vm, name, s);
			set_image_if_present(vm, name, s, m_images);
			set_color_if_present(vm, name, s);
			break;
		}

		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(it) == OBS_DATA_NUM_INT) {
				set_number_if_present(vm, name, static_cast<double>(obs_data_item_get_int(it)));
			} else {
				set_number_if_present(vm, name, obs_data_item_get_double(it));
			}
			break;

		case OBS_DATA_BOOLEAN:
			set_boolean_if_present(vm, name, obs_data_item_get_bool(it));
			break;

		case OBS_DATA_OBJECT: {
			obs_data_t *child = obs_data_item_get_obj(it);
			rive::rcp<rive::ViewModelInstanceRuntime> nested = vm->propertyViewModel(name);

			if (nested && child) {
				const std::string new_id = read_id_field(child);
				auto &slot = state.nested[name]; // {prev_id, prev_ptr}

				// Same-id (or no id at all) → mutate in place.
				// Otherwise call replaceViewModel so any state
				// machine logic on the nested VM observes a
				// clean instance.
				if (!new_id.empty() && !slot.first.empty() && new_id != slot.first) {
					std::string vm_type = read_type_field(child);
					if (vm_type.empty())
						vm_type = nested->viewModelName();
					rive::ViewModelRuntime *vm_runtime = m_file->viewModelByName(vm_type);
					if (vm_runtime) {
						rive::rcp<rive::ViewModelInstanceRuntime> fresh =
							vm_runtime->createInstance();
						if (fresh && vm->replaceViewModel(name, fresh.get())) {
							forgetState(nested.get());
							nested = fresh;
						}
					}
				}

				slot.first = new_id;
				slot.second = nested.get();

				applyObject(nested.get(), child);
			}
			obs_data_release(child);
			break;
		}

		case OBS_DATA_ARRAY: {
			rive::ViewModelInstanceListRuntime *list = vm->propertyList(name);
			obs_data_array_t *arr = obs_data_item_get_array(it);
			if (list && arr) {
				const size_t desired_count = obs_data_array_count(arr);
				auto &id_to_inst = state.list_items[name];

				// --- Phase 1: snapshot existing list items
				// and partition them into id-keyed (matched in
				// the cache) and "no id" (used for positional
				// fallback so files without ids work as before).
				std::vector<rive::rcp<rive::ViewModelInstanceRuntime>> existing_no_id;
				existing_no_id.reserve(list->size());

				// Reverse lookup: instance pointer → tracked id.
				// Used to identify which existing items were
				// previously cached by id and which weren't.
				std::unordered_map<rive::ViewModelInstanceRuntime *, std::string> ptr_to_id;
				for (auto &kv : id_to_inst)
					ptr_to_id.emplace(kv.second.get(), kv.first);

				for (size_t i = 0; i < list->size(); ++i) {
					auto x = list->instanceAt(static_cast<int>(i));
					if (!x)
						continue;
					if (ptr_to_id.find(x.get()) == ptr_to_id.end())
						existing_no_id.push_back(x);
				}

				// --- Phase 2: build the desired sequence,
				// reusing matched-id and (positionally)
				// no-id items. id_to_inst is mutated as we go;
				// what's left at the end represents items that
				// dropped out of the snapshot.
				std::unordered_map<std::string, rive::rcp<rive::ViewModelInstanceRuntime>>
					remaining_by_id = id_to_inst;
				std::vector<rive::rcp<rive::ViewModelInstanceRuntime>> desired_items;
				desired_items.reserve(desired_count);
				size_t no_id_cursor = 0;

				for (size_t i = 0; i < desired_count; ++i) {
					obs_data_t *elem = obs_data_array_item(arr, i);
					std::string id = read_id_field(elem);
					rive::rcp<rive::ViewModelInstanceRuntime> item;

					if (!id.empty()) {
						auto eit = remaining_by_id.find(id);
						if (eit != remaining_by_id.end()) {
							item = eit->second;
							remaining_by_id.erase(eit);
						}
					} else if (no_id_cursor < existing_no_id.size()) {
						item = existing_no_id[no_id_cursor++];
					}

					if (!item) {
						// Need a fresh instance.
						std::string item_vm = read_type_field(elem);
						if (item_vm.empty())
							item_vm = resolve_list_item_vm_name(list, arr);
						if (item_vm.empty()) {
							obs_log(LOG_WARNING,
								"rive: custom data list "
								"'%s' can't be grown — add "
								"a 'type' field to its "
								"items, or declare at "
								"least one example item "
								"in the editor",
								name);
						} else {
							rive::ViewModelRuntime *iv = m_file->viewModelByName(item_vm);
							if (!iv) {
								obs_log(LOG_WARNING,
									"rive: custom data "
									"list '%s' references "
									"view model '%s' but "
									"no such view model "
									"exists in the file",
									name, item_vm.c_str());
							} else {
								item = iv->createInstance();
							}
						}
					}

					if (item)
						desired_items.push_back(item);
					obs_data_release(elem);
				}

				// --- Phase 3: drop tracked state for items
				// that fell out of the snapshot. Their rcps
				// die with id_to_inst's reset below.
				for (auto &kv : remaining_by_id)
					forgetState(kv.second.get());

				// Update the id-keyed cache with the new mix of
				// items (only those that carry an id).
				id_to_inst.clear();
				for (size_t i = 0; i < desired_items.size(); ++i) {
					obs_data_t *elem = obs_data_array_item(arr, i);
					std::string id = read_id_field(elem);
					obs_data_release(elem);
					if (!id.empty())
						id_to_inst[id] = desired_items[i];
				}

				// --- Phase 4: rewrite the list only if the
				// desired sequence differs from what's already
				// there. Skipping the rewrite when nothing
				// changed keeps the list (and its m_itemsMap
				// cache) from being dirtied every frame for
				// static data. The underlying VM instances
				// survive across the rewrite because we hold
				// them via desired_items.
				bool sequence_changed = list->size() != desired_items.size();
				if (!sequence_changed) {
					for (size_t i = 0; i < desired_items.size(); ++i) {
						auto cur = list->instanceAt(static_cast<int>(i));
						if (cur.get() != desired_items[i].get()) {
							sequence_changed = true;
							break;
						}
					}
				}
				if (sequence_changed) {
					list->removeAllInstances();
					for (auto &item : desired_items)
						list->addInstance(item.get());
				}

				// --- Phase 5: recurse into each item to push
				// its scalar / nested values through.
				for (size_t i = 0; i < desired_items.size(); ++i) {
					obs_data_t *elem = obs_data_array_item(arr, i);
					if (elem)
						applyObject(desired_items[i].get(), elem);
					obs_data_release(elem);
				}
			}
			obs_data_array_release(arr);
			break;
		}

		case OBS_DATA_NULL:
		default:
			// Nothing useful to do with null — treat it as "leave
			// the property at whatever value it currently holds."
			break;
		}
	}
}

} // namespace rive_obs
