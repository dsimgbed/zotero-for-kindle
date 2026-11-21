#include "core/open_guard.h"

#ifdef ZK_OPEN_GUARD
#include <mutex>

namespace zk::openguard {

namespace {
std::mutex& mu() {
    static std::mutex m;
    return m;
}
std::function<void(const std::string&)>& recorder() {
    static std::function<void(const std::string&)> r;
    return r;
}
}  // namespace

void setRecorder(std::function<void(const std::string&)> r) {
    std::lock_guard<std::mutex> lock(mu());
    recorder() = std::move(r);
}

void note(const std::string& path) {
    std::lock_guard<std::mutex> lock(mu());
    if (recorder()) recorder()(path);
}

}  // namespace zk::openguard
#endif
