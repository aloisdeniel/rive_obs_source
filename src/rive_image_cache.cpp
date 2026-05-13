/*
 * rive_image_cache: see header.
 *
 * The worker thread pops a URL, performs the I/O (file read or HTTP GET),
 * and pushes the result to a shared queue that tick() drains on the graphics
 * thread. Decoding is done inside tick() because the Rive RenderContext is
 * only safe to use from there.
 */

#include "rive_image_cache.h"

#include <rive/renderer.hpp>
#include <rive/span.hpp>

#include <obs.h>
#include <plugin-support.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

namespace rive_obs {

namespace {

bool is_http_scheme(const std::string &url)
{
	return url.compare(0, 8, "https://") == 0 || url.compare(0, 7, "http://") == 0;
}

// Strips "file://" (and the optional "localhost" authority) and returns the
// local filesystem path. Empty on malformed input.
std::string file_url_to_path(const std::string &url)
{
	constexpr const char *prefix = "file://";
	if (url.compare(0, 7, prefix) != 0)
		return "";
	std::string rest = url.substr(7);
	// Tolerate "file://localhost/..." per RFC 8089.
	constexpr const char *localhost = "localhost";
	if (rest.compare(0, 9, localhost) == 0 && (rest.size() == 9 || rest[9] == '/'))
		rest.erase(0, 9);
	if (rest.empty())
		return "";
	return rest;
}

bool read_file(const std::string &path, std::vector<uint8_t> &out, std::string &err)
{
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		err = "could not open file";
		return false;
	}
	std::streamoff size = f.tellg();
	if (size < 0) {
		err = "could not stat file";
		return false;
	}
	out.resize(static_cast<size_t>(size));
	f.seekg(0, std::ios::beg);
	if (!f.read(reinterpret_cast<char *>(out.data()), size)) {
		err = "could not read file";
		return false;
	}
	return true;
}

} // namespace

ImageCache::ImageCache(Decoder decoder)
	: m_decoder(std::move(decoder)), m_results(std::make_shared<ResultQueue>())
{
	m_worker = std::thread(&ImageCache::workerLoop, this);
}

ImageCache::~ImageCache()
{
	{
		std::lock_guard<std::mutex> lk(m_jobs_mu);
		m_worker_quit = true;
	}
	m_jobs_cv.notify_all();
	if (m_worker.joinable())
		m_worker.join();
	// Any HTTP fetches still in flight (not on our worker — those finish
	// before join() returns) keep m_results alive via shared_ptr until
	// their completion handlers run; results pushed there after we're gone
	// are silently discarded when ResultQueue's last reference drops.
}

rive::RenderImage *ImageCache::request(const std::string &url)
{
	if (url.empty())
		return nullptr;

	auto it = m_entries.find(url);
	if (it == m_entries.end()) {
		// First time we see this URL — create the entry as Pending and
		// queue the fetch. Returning null tells the caller "not ready
		// yet"; a future tick() will populate it.
		m_entries.emplace(url, Entry{State::Pending, nullptr});
		obs_log(LOG_INFO, "rive: image fetching '%s'", url.c_str());
		enqueueFetch(url);
		return nullptr;
	}
	if (it->second.state == State::Loaded)
		return it->second.image.get();
	return nullptr;
}

void ImageCache::tick()
{
	std::vector<ResultQueue::Item> drained;
	{
		std::lock_guard<std::mutex> lk(m_results->mu);
		drained.swap(m_results->items);
	}
	for (auto &r : drained) {
		auto it = m_entries.find(r.url);
		if (it == m_entries.end())
			continue; // shouldn't happen, but tolerate

		if (!r.error.empty()) {
			it->second.state = State::Failed;
			obs_log(LOG_WARNING, "rive: image fetch failed for '%s': %s",
				r.url.c_str(), r.error.c_str());
			continue;
		}

		rive::rcp<rive::RenderImage> img;
		if (m_decoder)
			img = m_decoder(r.bytes.data(), r.bytes.size());
		if (!img) {
			it->second.state = State::Failed;
			obs_log(LOG_WARNING,
				"rive: image decode failed for '%s' (%zu bytes)",
				r.url.c_str(), r.bytes.size());
			continue;
		}
		it->second.state = State::Loaded;
		it->second.image = std::move(img);
		obs_log(LOG_INFO, "rive: image loaded '%s' (%zu bytes)", r.url.c_str(),
			r.bytes.size());
	}
}

void ImageCache::enqueueFetch(const std::string &url)
{
	{
		std::lock_guard<std::mutex> lk(m_jobs_mu);
		m_jobs.push_back(url);
	}
	m_jobs_cv.notify_one();
}

void ImageCache::workerLoop(ImageCache *self)
{
	auto results = self->m_results;
	for (;;) {
		std::string url;
		{
			std::unique_lock<std::mutex> lk(self->m_jobs_mu);
			self->m_jobs_cv.wait(lk, [self] {
				return self->m_worker_quit || !self->m_jobs.empty();
			});
			if (self->m_worker_quit && self->m_jobs.empty())
				return;
			url = std::move(self->m_jobs.back());
			self->m_jobs.pop_back();
		}

		std::vector<uint8_t> bytes;
		std::string err;
		bool ok = false;

		if (is_http_scheme(url)) {
			ok = image_http_fetch(url, bytes, err);
		} else if (url.compare(0, 7, "file://") == 0) {
			std::string path = file_url_to_path(url);
			if (path.empty())
				err = "malformed file:// URL";
			else
				ok = read_file(path, bytes, err);
		} else {
			err = "unsupported scheme (use http://, https://, or file://)";
		}

		ResultQueue::Item item;
		item.url = std::move(url);
		if (ok)
			item.bytes = std::move(bytes);
		else
			item.error = err.empty() ? "unknown error" : err;

		{
			std::lock_guard<std::mutex> lk(results->mu);
			results->items.push_back(std::move(item));
		}
	}
}

} // namespace rive_obs
