#include "zotero/rate_limit.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace zk {

namespace {

constexpr int kMaxRateLimitRetries = 3;

int headerInt(const HttpResponse& r, const char* name, int fallback = 0) {
    auto it = r.headers.find(name);
    if (it == r.headers.end()) return fallback;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

RateLimiter::RateLimiter()
    : sleeper_([](int s) {
          std::this_thread::sleep_for(std::chrono::seconds(s));
          return true;
      }) {}

HttpResponse RateLimiter::send(Http& http, const HttpRequest& req, const std::function<void(const HttpResponse&)>& onWait) {
    auto stopped = [] {
        HttpResponse r;
        r.curlCode = 42;  // CURLE_ABORTED_BY_CALLBACK, as a cancelled transfer
        r.curlError = "stopped while waiting for Zotero";
        return r;
    };
    auto giveUp = [&](HttpResponse r, int wait) {
        if (onGiveUp_) onGiveUp_(wait);
        return r;
    };
    for (int attempt = 0;; ++attempt) {
        if (backoffS_ > kMaxRateWaitS) {
            HttpResponse busy;
            busy.status = 429;
            busy.headers["retry-after"] = std::to_string(backoffS_);
            const int wait = backoffS_;
            backoffS_ = 0;
            return giveUp(busy, wait);
        }
        if (backoffS_ > 0) {
            const int wait = backoffS_;
            backoffS_ = 0;
            if (!sleeper_(wait)) return stopped();
        }
        HttpResponse r = http.send(req);
        backoffS_ = headerInt(r, "backoff");
        const bool busy = r.curlCode == 0 && (r.status == 429 || (r.status == 503 && r.headers.count("retry-after")));
        if (!busy) return r;
        const int wait = std::max(headerInt(r, "retry-after", 60), backoffS_);
        backoffS_ = 0;  // waited out with the retry, not again before it
        if (wait > kMaxRateWaitS || attempt >= kMaxRateLimitRetries) return giveUp(r, wait);
        if (onWait) onWait(r);
        if (!sleeper_(wait)) return stopped();
    }
}

}  // namespace zk
