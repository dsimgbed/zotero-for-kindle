#include "platform/power.h"

#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

namespace zk {

void KeepAwake::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (holders_++ == 0) lipc_.set("com.lab126.powerd", "preventScreenSaver", "1");
}

void KeepAwake::release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (holders_ == 0) return;
    if (--holders_ == 0) lipc_.set("com.lab126.powerd", "preventScreenSaver", "0");
}

bool deviceAsleep(Lipc& lipc) {
    const std::string state = lipc.get("com.lab126.powerd", "state");
    return state == "screenSaver" || state == "readyToSuspend" || state == "suspended";
}

int64_t suspendedMs() {
#ifdef CLOCK_BOOTTIME
    timespec boot{}, mono{};
    if (clock_gettime(CLOCK_BOOTTIME, &boot) != 0 || clock_gettime(CLOCK_MONOTONIC, &mono) != 0) return 0;
    return (int64_t(boot.tv_sec) - mono.tv_sec) * 1000 + (int64_t(boot.tv_nsec) - mono.tv_nsec) / 1'000'000;
#else
    return 0;
#endif
}

bool storageAvailable(const std::string& dir) {
    struct statvfs st {};
    return ::access(dir.c_str(), W_OK) == 0 && ::statvfs(dir.c_str(), &st) == 0;
}

}  // namespace zk
