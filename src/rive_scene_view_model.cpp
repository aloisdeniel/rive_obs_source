/*
 * rive_scene_view_model: see header for the contract.
 *
 * This module is platform-agnostic — it only talks to the Rive runtime's
 * view-model API and never touches the GPU. It is compiled into both the mac
 * and win renderer modules.
 */

#include "rive_scene_view_model.h"

// Rive headers first (see comment in rive_renderer_mac.mm).
#include <rive/file.hpp>
#include <rive/animation/state_machine_instance.hpp>
#include <rive/viewmodel/runtime/viewmodel_runtime.hpp>
#include <rive/viewmodel/runtime/viewmodel_instance_runtime.hpp>
#include <rive/viewmodel/runtime/viewmodel_instance_list_runtime.hpp>
#include <rive/viewmodel/runtime/viewmodel_instance_string_runtime.hpp>
#include <rive/viewmodel/runtime/viewmodel_instance_number_runtime.hpp>
#include <rive/viewmodel/runtime/viewmodel_instance_boolean_runtime.hpp>
#include <rive/viewmodel/viewmodel_instance.hpp>
#include <rive/viewmodel/viewmodel_instance_viewmodel.hpp>
#include <rive/viewmodel/viewmodel.hpp>
#include <rive/data_bind/data_values/data_type.hpp>

#include <obs.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rive_obs {

namespace {

// Convenience helpers. Each tolerates a missing property — the .riv author may
// have only declared a subset of the fields documented in the header.
void set_string_if_present(rive::ViewModelInstanceRuntime *vm, const char *name,
			   const char *value)
{
	if (!vm)
		return;
	auto *p = vm->propertyString(name);
	if (!p)
		return;
	std::string desired = value ? value : "";
	if (p->value() != desired)
		p->value(desired);
}

void set_number_if_present(rive::ViewModelInstanceRuntime *vm, const char *name, float value)
{
	if (!vm)
		return;
	auto *p = vm->propertyNumber(name);
	if (!p)
		return;
	// Skip the write when the value hasn't materially changed so we don't
	// flag the property dirty 60 times a second for static scenes.
	if (std::fabs(p->value() - value) <= 1e-3f)
		return;
	p->value(value);
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

// Returns the view-model name used by items of the named list, by:
//   1. Inspecting an existing item, if the list has at least one.
//   2. Otherwise falling back to the supplied conventional name (which may
//      not exist in the file; the caller treats that as "give up").
// The "inspecting an existing item" branch is what lets the author choose any
// inner VM name they like — we just need *one* sample to learn it.
std::string detect_list_item_vm_name(rive::ViewModelInstanceListRuntime *list,
				     const char *fallback)
{
	if (list && list->size() > 0) {
		auto first = list->instanceAt(0);
		if (first)
			return first->viewModelName();
	}
	return fallback ? fallback : "";
}

} // namespace

std::unique_ptr<SceneViewModelBinding>
SceneViewModelBinding::tryCreate(rive::File *file, rive::StateMachineInstance *sm)
{
	if (!file)
		return nullptr;
	rive::ViewModelRuntime *vm = file->viewModelByName(kSceneViewModelName);
	if (!vm) {
		// The file simply doesn't use a SceneViewModel — that's fine,
		// the renderer will skip scene-state propagation entirely.
		return nullptr;
	}

	// Prefer the file's default instance (the editor's "Default" instance)
	// so the author can configure starting values; fall back to a fresh
	// blank instance if the file has no instances at all.
	rive::rcp<rive::ViewModelInstanceRuntime> root = vm->createDefaultInstance();
	if (!root)
		root = vm->createInstance();
	if (!root)
		return nullptr;

	if (sm)
		sm->bindViewModelInstance(root->instance());

	// std::make_unique can't call a private constructor.
	return std::unique_ptr<SceneViewModelBinding>(new SceneViewModelBinding(file, std::move(root)));
}

SceneViewModelBinding::SceneViewModelBinding(rive::File *file,
					     rive::rcp<rive::ViewModelInstanceRuntime> root)
	: m_file(file), m_root(std::move(root))
{
}

SceneViewModelBinding::~SceneViewModelBinding() = default;

rive::ViewModelInstanceListRuntime *
SceneViewModelBinding::resizeList(const char *property_name, const std::string &item_vm_name,
				  size_t desired_count)
{
	if (!m_root)
		return nullptr;
	auto *list = m_root->propertyList(property_name);
	if (!list)
		return nullptr;

	const size_t current = list->size();

	// Shrink first — cheap and lets the add path below assume we only ever
	// append. removeInstanceAt(int) takes a signed index so we step down.
	for (size_t i = current; i > desired_count; --i) {
		list->removeInstanceAt(static_cast<int>(i - 1));
	}

	if (desired_count > list->size()) {
		// Need to add items. If we don't know an item VM name yet,
		// the list is empty and the caller didn't supply one — bail
		// silently (the author probably declared a list but didn't
		// configure its item type yet, or declared it without an inner
		// VM, both of which we tolerate).
		if (item_vm_name.empty())
			return list;
		// Suppress per-frame warning spam: once we've established that
		// a name doesn't resolve, return the list silently on every
		// subsequent call. The .riv would have to be re-imported (which
		// rebuilds the binding) for the lookup to be re-tried.
		if (m_unknownItemVmNames.count(item_vm_name))
			return list;
		rive::ViewModelRuntime *item_vm = m_file->viewModelByName(item_vm_name);
		if (!item_vm) {
			obs_log(LOG_WARNING,
				"rive: SceneViewModel list '%s' references view model "
				"'%s' but no such view model exists in the file",
				property_name, item_vm_name.c_str());
			m_unknownItemVmNames.insert(item_vm_name);
			return list;
		}
		while (list->size() < desired_count) {
			auto inst = item_vm->createInstance();
			if (!inst)
				break;
			list->addInstance(inst.get());
		}
	}
	return list;
}

void SceneViewModelBinding::apply(const rive_scene_state &state)
{
	if (!m_root)
		return;

	set_string_if_present(m_root.get(), "name", state.scene_name);
	set_number_if_present(m_root.get(), "width", state.canvas_width);
	set_number_if_present(m_root.get(), "height", state.canvas_height);
	set_boolean_if_present(m_root.get(), "streaming", state.streaming);
	set_boolean_if_present(m_root.get(), "recording", state.recording);
	set_boolean_if_present(m_root.get(), "recordingPaused", state.recording_paused);
	set_number_if_present(m_root.get(), "sourceCount",
			      static_cast<float>(state.source_count));
	set_number_if_present(m_root.get(), "audioCount",
			      static_cast<float>(state.audio_count));

	// Sources list -----------------------------------------------------
	{
		// Look up (and cache) the inner VM name from whatever the file
		// already has in the list. If empty, try a conventional name.
		auto *list_for_detect = m_root->propertyList("sources");
		if (m_sourcesItemVmName.empty()) {
			m_sourcesItemVmName =
				detect_list_item_vm_name(list_for_detect, "SourceViewModel");
		}

		auto *list = resizeList("sources", m_sourcesItemVmName, state.source_count);
		if (list) {
			const size_t n = std::min(list->size(), state.source_count);
			for (size_t i = 0; i < n; ++i) {
				auto item = list->instanceAt(static_cast<int>(i));
				if (!item)
					continue;
				const auto &s = state.sources[i];
				set_string_if_present(item.get(), "name", s.name);
				set_number_if_present(item.get(), "x", s.x);
				set_number_if_present(item.get(), "y", s.y);
				set_number_if_present(item.get(), "width", s.width);
				set_number_if_present(item.get(), "height", s.height);
				set_boolean_if_present(item.get(), "visible", s.visible);
			}
		}
	}

	// Audio sources list -----------------------------------------------
	{
		auto *list_for_detect = m_root->propertyList("audioSources");
		if (m_audioItemVmName.empty()) {
			m_audioItemVmName = detect_list_item_vm_name(list_for_detect,
								     "AudioSourceViewModel");
		}

		auto *list = resizeList("audioSources", m_audioItemVmName, state.audio_count);
		if (list) {
			const size_t n = std::min(list->size(), state.audio_count);
			for (size_t i = 0; i < n; ++i) {
				auto item = list->instanceAt(static_cast<int>(i));
				if (!item)
					continue;
				const auto &a = state.audio_sources[i];
				set_string_if_present(item.get(), "name", a.name);
				set_number_if_present(item.get(), "volume", a.volume);
				set_boolean_if_present(item.get(), "muted", a.muted);
			}
		}
	}
}

} // namespace rive_obs
