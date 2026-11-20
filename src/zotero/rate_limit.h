#pragma once
// Zotero's rate limits, for every request to api.zotero.org (the API, and the file downloads'
// /file redirects): a Backoff header delays the next request; a 429 (or a 503 with Retry-After)
// is waited out and tried again, up to three times, waiting once for the longer of its
// Retry-After and Backoff. A wait over kMaxRateWaitS isn't slept: the sync holds the Kindle
// awake meanwhile. The request ends rate-limited instead, and the next sync tries again.

#include "net/http.h"

#include <functional>

namespace zk {

constexpr int kMaxRateWaitS = 60;

class RateLimiter {
public:
    RateLimiter();
    void setSleeper(std::function<void(int seconds)> f) { sleeper_ = std::move(f); }
    // onWait: told before each wait, with the answer that asked for it. An answer that asks for
    // too long a wait comes back as it is; a Backoff too long for the next request comes back as
    // a 429 with that Retry-After, the request not sent.
    HttpResponse send(Http&, const HttpRequest&, const std::function<void(const HttpResponse&)>& onWait = nullptr);

private:
    std::function<void(int)> sleeper_;
    int backoffS_ = 0;
};

}  // namespace zk
