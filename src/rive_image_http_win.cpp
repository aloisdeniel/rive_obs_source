/*
 * rive_image_http_win: synchronous HTTP(S) GET implementation for Windows,
 * backed by WinHTTP. Linked against winhttp.lib (see CMakeLists.txt).
 *
 * Called from the ImageCache worker thread, so blocking is fine.
 */

#include "rive_image_cache.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <string>
#include <vector>

namespace rive_obs {

namespace {

// utf-8 → utf-16 for WinHTTP's wide-string API.
std::wstring widen(const std::string &s)
{
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

// RAII guard that closes a WinHTTP handle on scope exit.
struct WinHttpGuard {
	HINTERNET h;
	~WinHttpGuard()
	{
		if (h)
			WinHttpCloseHandle(h);
	}
};

} // namespace

bool image_http_fetch(const std::string &url, std::vector<uint8_t> &out, std::string &err)
{
	std::wstring wurl = widen(url);
	if (wurl.empty()) {
		err = "invalid URL";
		return false;
	}

	URL_COMPONENTS comps{};
	comps.dwStructSize = sizeof(comps);
	comps.dwSchemeLength = (DWORD)-1;
	comps.dwHostNameLength = (DWORD)-1;
	comps.dwUrlPathLength = (DWORD)-1;
	comps.dwExtraInfoLength = (DWORD)-1;
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &comps)) {
		err = "URL parse failed";
		return false;
	}

	std::wstring host(comps.lpszHostName, comps.dwHostNameLength);
	std::wstring path(comps.lpszUrlPath, comps.dwUrlPathLength);
	if (comps.dwExtraInfoLength)
		path.append(comps.lpszExtraInfo, comps.dwExtraInfoLength);

	HINTERNET session = WinHttpOpen(L"rive-obs-source/1.0",
					WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		err = "WinHttpOpen failed";
		return false;
	}
	WinHttpGuard g_session{session};

	// 30s timeouts everywhere — matches the macOS side.
	WinHttpSetTimeouts(session, 30000, 30000, 30000, 30000);

	HINTERNET conn = WinHttpConnect(session, host.c_str(), comps.nPort, 0);
	if (!conn) {
		err = "WinHttpConnect failed";
		return false;
	}
	WinHttpGuard g_conn{conn};

	const DWORD flags = (comps.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
	HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
					   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
	if (!req) {
		err = "WinHttpOpenRequest failed";
		return false;
	}
	WinHttpGuard g_req{req};

	if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
				0, 0)) {
		err = "WinHttpSendRequest failed";
		return false;
	}
	if (!WinHttpReceiveResponse(req, nullptr)) {
		err = "WinHttpReceiveResponse failed";
		return false;
	}

	DWORD status = 0;
	DWORD status_size = sizeof(status);
	WinHttpQueryHeaders(req,
			    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			    WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
			    WINHTTP_NO_HEADER_INDEX);
	if (status < 200 || status >= 300) {
		char buf[64];
		std::snprintf(buf, sizeof(buf), "HTTP %lu", (unsigned long)status);
		err = buf;
		return false;
	}

	for (;;) {
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(req, &avail)) {
			err = "WinHttpQueryDataAvailable failed";
			return false;
		}
		if (avail == 0)
			break;
		size_t old_size = out.size();
		out.resize(old_size + avail);
		DWORD read = 0;
		if (!WinHttpReadData(req, out.data() + old_size, avail, &read)) {
			err = "WinHttpReadData failed";
			return false;
		}
		out.resize(old_size + read);
		if (read == 0)
			break;
	}

	if (out.empty()) {
		err = "empty response";
		return false;
	}
	return true;
}

} // namespace rive_obs
