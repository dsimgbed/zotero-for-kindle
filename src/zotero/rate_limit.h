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
    // false from the sleeper: stopping. The request ends (cancelled) instead of going out again.
    void setSleeper(std::function<bool(int seconds)> f) { sleeper_ = std::move(f); }
    // Told whenever a request ends rate-limited (a wait too long to sleep, or still busy after the
    // retries): how long Zotero asked to wait, in seconds.
    void setOnGiveUp(std::function<void(int seconds)> f) { onGiveUp_ = std::move(f); }
    // onWait: told before each wait, with the answer that asked for it. An answer that asks for
    // too long a wait comes back as it is; a Backoff too long for the next request comes back as
    // a 429 with that Retry-After, the request not sent.
    HttpResponse send(Http&, const HttpRequest&, const std::function<void(const HttpResponse&)>& onWait = nullptr);

private:
    std::function<bool(int)> sleeper_;
    std::function<void(int)> onGiveUp_;
    int backoffS_ = 0;
};

}  // namespace zk
