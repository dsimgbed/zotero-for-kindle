#pragma once
// Temp paths that belong to this test process: two test binaries running at once (a parallel
// `meson test`, two terminals) must not read, write or delete each other's files.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace zk::test {

// "/tmp/<name>-<pid>"; the crengine document caches go through here, removed when the process ends.
inline std::string creCacheDir(const std::string& suffix = "") {
    static const std::string base = "/tmp/zk-cre-cache-" + std::to_string(::getpid());
    static const bool cleanup = [] {
        std::atexit([] {
            std::error_code ec;
            for (const char* s : {"", "-fast", "-plugin-lib"}) std::filesystem::remove_all(base + s, ec);
        });
        return true;
    }();
    (void)cleanup;
    return base + suffix;
}

// A fresh path in the temp dir for this process: "<tmp>/<stem>-<pid><ext>".
inline std::filesystem::path uniqueTempPath(const std::string& stem, const std::string& ext = "") {
    return std::filesystem::temp_directory_path() / (stem + "-" + std::to_string(::getpid()) + ext);
}

}  // namespace zk::test
