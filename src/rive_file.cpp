#include "rive_file.h"

#include <rive/file.hpp>
#include <rive/artboard.hpp>
#include <utils/no_op_factory.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// One NoOpFactory is enough for introspection across all loaded files; it has
// no state and Rive only invokes it during File::import for the introspection
// path we use here.
rive::NoOpFactory &shared_no_op_factory()
{
	static rive::NoOpFactory f;
	return f;
}

void set_err(char *err, size_t err_size, const char *msg)
{
	if (!err || err_size == 0)
		return;
	std::snprintf(err, err_size, "%s", msg);
}

} // namespace

struct ArtboardInfo {
	std::string name;
	uint32_t width;
	uint32_t height;
	std::vector<std::string> state_machine_names;
};

struct rive_file {
	rive::rcp<rive::File> file;
	std::vector<ArtboardInfo> artboards;
};

rive_file_t *rive_file_open(const char *path, char *err, size_t err_size)
{
	if (!path || !*path) {
		set_err(err, err_size, "empty path");
		return nullptr;
	}

	std::FILE *fp = std::fopen(path, "rb");
	if (!fp) {
		set_err(err, err_size, "could not open file");
		return nullptr;
	}

	if (std::fseek(fp, 0, SEEK_END) != 0) {
		std::fclose(fp);
		set_err(err, err_size, "fseek failed");
		return nullptr;
	}
	long size = std::ftell(fp);
	if (size <= 0) {
		std::fclose(fp);
		set_err(err, err_size, "empty or unreadable file");
		return nullptr;
	}
	std::rewind(fp);

	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	size_t read = std::fread(bytes.data(), 1, bytes.size(), fp);
	std::fclose(fp);
	if (read != bytes.size()) {
		set_err(err, err_size, "short read while loading file");
		return nullptr;
	}

	rive::ImportResult result = rive::ImportResult::malformed;
	auto file = rive::File::import(rive::Span<const uint8_t>(bytes.data(), bytes.size()), &shared_no_op_factory(),
				       &result);
	if (!file || result != rive::ImportResult::success) {
		switch (result) {
		case rive::ImportResult::unsupportedVersion:
			set_err(err, err_size, "unsupported Rive file version");
			break;
		case rive::ImportResult::malformed:
		default:
			set_err(err, err_size, "malformed Rive file");
			break;
		}
		return nullptr;
	}

	auto handle = std::make_unique<rive_file>();
	handle->file = std::move(file);

	const size_t ab_count = handle->file->artboardCount();
	handle->artboards.reserve(ab_count);
	for (size_t i = 0; i < ab_count; ++i) {
		ArtboardInfo info;
		info.name = handle->file->artboardNameAt(i);

		// artboardAt returns a fully-instanced ArtboardInstance. We only
		// need it to read intrinsic dimensions and the SM list; it gets
		// destroyed at the end of this loop iteration.
		auto instance = handle->file->artboardAt(i);
		if (instance) {
			float w = instance->width();
			float h = instance->height();
			info.width = w > 0.f ? static_cast<uint32_t>(w) : 0u;
			info.height = h > 0.f ? static_cast<uint32_t>(h) : 0u;

			const size_t sm_count = instance->stateMachineCount();
			info.state_machine_names.reserve(sm_count);
			for (size_t s = 0; s < sm_count; ++s) {
				info.state_machine_names.push_back(instance->stateMachineNameAt(s));
			}
		}
		handle->artboards.push_back(std::move(info));
	}

	return handle.release();
}

void rive_file_close(rive_file_t *file)
{
	delete file;
}

size_t rive_file_artboard_count(const rive_file_t *file)
{
	return file ? file->artboards.size() : 0;
}

const char *rive_file_artboard_name(const rive_file_t *file, size_t artboard_idx)
{
	if (!file || artboard_idx >= file->artboards.size())
		return nullptr;
	return file->artboards[artboard_idx].name.c_str();
}

bool rive_file_artboard_size(const rive_file_t *file, size_t artboard_idx, uint32_t *out_w, uint32_t *out_h)
{
	if (!file || artboard_idx >= file->artboards.size())
		return false;
	const auto &ab = file->artboards[artboard_idx];
	if (out_w)
		*out_w = ab.width;
	if (out_h)
		*out_h = ab.height;
	return ab.width > 0 && ab.height > 0;
}

size_t rive_file_state_machine_count(const rive_file_t *file, size_t artboard_idx)
{
	if (!file || artboard_idx >= file->artboards.size())
		return 0;
	return file->artboards[artboard_idx].state_machine_names.size();
}

const char *rive_file_state_machine_name(const rive_file_t *file, size_t artboard_idx, size_t i)
{
	if (!file || artboard_idx >= file->artboards.size())
		return nullptr;
	const auto &sms = file->artboards[artboard_idx].state_machine_names;
	if (i >= sms.size())
		return nullptr;
	return sms[i].c_str();
}

size_t rive_file_find_artboard(const rive_file_t *file, const char *name)
{
	if (!file || !name)
		return SIZE_MAX;
	for (size_t i = 0; i < file->artboards.size(); ++i) {
		if (file->artboards[i].name == name)
			return i;
	}
	return SIZE_MAX;
}
