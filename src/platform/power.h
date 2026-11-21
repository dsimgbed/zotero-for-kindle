#pragma once
// Keeps the Kindle from going to sleep while the app does background work (sync, downloads),
// and tells whether the app's storage is still there.

#include "platform/lipc.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace zk {

class KeepAwake {
public:
    explicit KeepAwake(Lipc& lipc) : lipc_(lipc) {}
    void acquire();  // first holder sets preventScreenSaver 1
    void release();  // last holder sets it back to 0; extra releases are ignored

private:
    Lipc& lipc_;
    std::mutex mutex_;
    int holders_ = 0;
};

struct KeepAwakeScope {
    explicit KeepAwakeScope(KeepAwake& k) : keep(k) { keep.acquire(); }
    ~KeepAwakeScope() { keep.release(); }
    KeepAwakeScope(const KeepAwakeScope&) = delete;
    KeepAwakeScope& operator=(const KeepAwakeScope&) = delete;
    KeepAwake& keep;
};

// The Kindle is in its screensaver or going to sleep (powerd's state isn't "active"), as opposed
// to a system dialog or the keyboard over the app. Desktop (no powerd answer): false.
bool deviceAsleep(Lipc&);
// How long the device has been suspended since it started (Linux: CLOCK_BOOTTIME minus
// CLOCK_MONOTONIC, which stops while suspended; elsewhere 0): it grows only across a suspend.
int64_t suspendedMs();

// The directory exists, is writable and its filesystem answers (false once /mnt/us is gone).
bool storageAvailable(const std::string& dir);

}  // namespace zk
