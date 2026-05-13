/*
 * rive_image_http_mac: synchronous HTTP(S) GET implementation for macOS,
 * backed by NSURLSession.
 *
 * Called from the ImageCache worker thread, so blocking is fine. We use a
 * dispatch_semaphore to turn NSURLSession's async API into a sync call.
 */

#include "rive_image_cache.h"

#import <Foundation/Foundation.h>

namespace rive_obs {

bool image_http_fetch(const std::string &url, std::vector<uint8_t> &out, std::string &err)
{
	@autoreleasepool {
		NSString *url_str = [NSString stringWithUTF8String:url.c_str()];
		NSURL *nsurl = url_str ? [NSURL URLWithString:url_str] : nil;
		if (!nsurl) {
			err = "invalid URL";
			return false;
		}

		dispatch_semaphore_t sema = dispatch_semaphore_create(0);
		__block NSData *data_out = nil;
		__block NSString *err_out = nil;
		__block NSInteger status = 0;
		__block bool got_response = false;

		NSURLRequest *req = [NSURLRequest requestWithURL:nsurl
						     cachePolicy:NSURLRequestUseProtocolCachePolicy
						 timeoutInterval:30.0];
		NSURLSessionDataTask *task =
			[[NSURLSession sharedSession] dataTaskWithRequest:req
							completionHandler:^(NSData *d,
									    NSURLResponse *r,
									    NSError *e) {
				if (e) {
					err_out = [e localizedDescription];
				} else {
					got_response = true;
					data_out = d;
					if ([r isKindOfClass:[NSHTTPURLResponse class]])
						status = ((NSHTTPURLResponse *)r).statusCode;
					else
						status = 200;
				}
				dispatch_semaphore_signal(sema);
			}];
		[task resume];
		dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);

		if (!got_response) {
			err = err_out ? [err_out UTF8String] : "request failed";
			return false;
		}
		if (status < 200 || status >= 300) {
			char buf[64];
			std::snprintf(buf, sizeof(buf), "HTTP %ld", (long)status);
			err = buf;
			return false;
		}
		if (!data_out || data_out.length == 0) {
			err = "empty response";
			return false;
		}

		const uint8_t *p = (const uint8_t *)data_out.bytes;
		out.assign(p, p + data_out.length);
		return true;
	}
}

} // namespace rive_obs
