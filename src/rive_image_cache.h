/*
 * rive_image_cache: resolves URLs / file paths to rive::RenderImage objects so
 * the data binding can populate Rive view-model image properties from string
 * fields in the user's JSON.
 *
 * Supported schemes:
 *   - "https://..." / "http://..."  HTTP GET via the platform's native HTTP
 *                                   client (NSURLSession on macOS, WinHTTP
 *                                   on Windows).
 *   - "file://..."                  Read from the local filesystem. The path
 *                                   may include the optional `localhost`
 *                                   authority (file://localhost/foo == file:///foo).
 *
 * Threading:
 *   - request() and tick() must be called on the OBS graphics thread (same
 *     thread that drives the renderer).
 *   - Fetches run on a single internal worker thread so the graphics thread
 *     never blocks on I/O. Decoded RenderImage objects are produced on the
 *     graphics thread inside tick(), where the Rive RenderContext is safe to
 *     use.
 *
 * Lifetime:
 *   - URLs are cached for the lifetime of the cache (no eviction). The cache
 *     is owned by the renderer and re-created on file reload, which is when
 *     stale URLs naturally clear.
 *   - In-flight fetches that complete after the cache is destroyed are
 *     dropped silently — completion handlers capture a shared_ptr to a
 *     "result queue" that outlives the cache and is cleaned up when the last
 *     callback runs.
 */

#pragma once

#ifdef __cplusplus

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rive/refcnt.hpp>

namespace rive {
class RenderImage;
} // namespace rive

namespace rive_obs {

class ImageCache {
public:
	// Decoder is invoked on the graphics thread inside tick() to turn raw
	// image bytes into a RenderImage. Typically wraps the renderer's
	// RenderContext::decodeImage().
	using Decoder = std::function<rive::rcp<rive::RenderImage>(const uint8_t *, size_t)>;

	explicit ImageCache(Decoder decoder);
	~ImageCache();

	ImageCache(const ImageCache &) = delete;
	ImageCache &operator=(const ImageCache &) = delete;

	// Returns the cached RenderImage for url, or nullptr if it isn't ready
	// yet. The first call for a new URL kicks off a background fetch;
	// subsequent calls return null until the bytes arrive and tick() has
	// decoded them. Subsequent calls for a known URL are O(1) hash lookups.
	rive::RenderImage *request(const std::string &url);

	// Drains completed fetches and decodes them. Cheap when there's
	// nothing to do.
	void tick();

private:
	enum class State { Pending, Loaded, Failed };

	struct Entry {
		State state = State::Pending;
		rive::rcp<rive::RenderImage> image;
	};

	// Worker-thread → graphics-thread handoff. Held via shared_ptr so
	// in-flight fetches can complete after the cache is destroyed.
	struct ResultQueue {
		std::mutex mu;
		struct Item {
			std::string url;
			std::vector<uint8_t> bytes; // empty on failure
			std::string error;          // populated on failure
		};
		std::vector<Item> items;
	};

	void enqueueFetch(const std::string &url);
	static void workerLoop(ImageCache *self);

	Decoder m_decoder;
	std::unordered_map<std::string, Entry> m_entries;

	std::shared_ptr<ResultQueue> m_results;

	// Worker-thread state.
	std::thread m_worker;
	std::mutex m_jobs_mu;
	std::condition_variable m_jobs_cv;
	std::vector<std::string> m_jobs;
	bool m_worker_quit = false;
};

// Implemented per-platform (rive_image_http_mac.mm / rive_image_http_win.cpp).
// Performs a synchronous HTTP(S) GET. Returns true on 2xx with bytes; on
// failure returns false and populates err with a short message.
bool image_http_fetch(const std::string &url, std::vector<uint8_t> &out, std::string &err);

} // namespace rive_obs

#endif // __cplusplus
