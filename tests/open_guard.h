#pragma once
// The isolation checks' file-open guard (spec: multiple accounts, Isolation checks), over the
// hook in src/core/open_guard.h: while an OpenGuard lives, every account file the app opens
// (Db::open, the files sweep, downloads, the caches, Home's kept tree) is recorded when it is under
// `accountsRoot`; outside() lists those that weren't under the active account's folder.
// Also: the files this process holds open under a folder (macOS F_GETPATH, Linux /proc/self/fd),
// and the process's thread count.

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/param.h>
#endif

#include "core/open_guard.h"

namespace zk::test {

inline std::string absolutePath(const std::string& p) {
    std::error_code ec;
    auto a = std::filesystem::absolute(p, ec).lexically_normal().string();
    while (a.size() > 1 && a.back() == '/') a.pop_back();
    return a;
}

// `path` is `dir` or inside it (lexically, both made absolute).
inline bool isUnder(const std::string& path, const std::string& dir) {
    const std::string p = absolutePath(path), d = absolutePath(dir);
    return p == d || (p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == '/');
}

class OpenGuard {
public:
    explicit OpenGuard(std::string accountsRoot) : root_(std::move(accountsRoot)) {
        zk::openguard::setRecorder([this](const std::string& path) {
            if (!isUnder(path, root_)) return;
            std::lock_guard<std::mutex> lock(mu_);
            opens_.push_back(path);
            if (active_.empty() || !isUnder(path, active_)) outside_.push_back(path);
        });
    }
    ~OpenGuard() { zk::openguard::setRecorder({}); }
    OpenGuard(const OpenGuard&) = delete;
    OpenGuard& operator=(const OpenGuard&) = delete;

    // The active account's folder from now on (opens are checked against it); clears the records.
    void setActive(std::string accountRoot) {
        std::lock_guard<std::mutex> lock(mu_);
        active_ = std::move(accountRoot);
        opens_.clear();
        outside_.clear();
    }
    std::vector<std::string> opens() const {
        std::lock_guard<std::mutex> lock(mu_);
        return opens_;
    }
    std::vector<std::string> outside() const {
        std::lock_guard<std::mutex> lock(mu_);
        return outside_;
    }

private:
    std::string root_, active_;
    mutable std::mutex mu_;
    std::vector<std::string> opens_, outside_;
};

// The files this process has open under `dir` now (each descriptor's path).
inline std::vector<std::string> openFilesUnder(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    const std::string real = std::filesystem::weakly_canonical(dir, ec).string();
    for (int fd = 0; fd < 4096; ++fd) {
        std::string path;
#if defined(__APPLE__)
        char buf[MAXPATHLEN];
        if (fcntl(fd, F_GETPATH, buf) == -1) continue;
        path = buf;
#else
        char buf[4096];
        const ssize_t n = readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), buf, sizeof buf - 1);
        if (n <= 0) continue;
        path.assign(buf, static_cast<size_t>(n));
#endif
        if (isUnder(path, real) || isUnder(path, dir)) out.push_back(path);
    }
    return out;
}

// Threads of this process now (macOS: the Mach task's; Linux: /proc/self/task).
inline size_t threadCount() {
#if defined(__APPLE__)
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) return 0;
    for (mach_msg_type_number_t i = 0; i < n; ++i) mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), n * sizeof(thread_act_t));
    return n;
#else
    size_t n = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/proc/self/task", ec), end; !ec && it != end; it.increment(ec)) ++n;
    return n;
#endif
}

}  // namespace zk::test
