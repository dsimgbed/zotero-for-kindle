#include "core/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace zk {

namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::string g_rotatePath;  // guarded by g_mutex
long long g_rotateMax = 0;
std::atomic<LogLevel> g_level{std::getenv("ZK_LOG_DEBUG") ? LogLevel::Debug : LogLevel::Info};

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

bool sameFile(int fd, const std::string& path) {
    struct stat a, b;
    return fstat(fd, &a) == 0 && stat(path.c_str(), &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// Under g_mutex. Only while stderr is the file at g_rotatePath: on the desktop it is a terminal,
// and after a rotation by someone else it is not ours to move again.
void rotateIfBig() {
    struct stat st;
    if (g_rotatePath.empty() || fstat(STDERR_FILENO, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= g_rotateMax) return;
    if (!sameFile(STDERR_FILENO, g_rotatePath)) return;
    const bool outToo = sameFile(STDOUT_FILENO, g_rotatePath);  // the launcher sends both there
    if (rename(g_rotatePath.c_str(), (g_rotatePath + ".1").c_str()) != 0) return;
    const int fd = open(g_rotatePath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;  // the lines go on into zotero.log.1
    fflush(stderr);
    dup2(fd, STDERR_FILENO);
    if (outToo) {
        fflush(stdout);
        dup2(fd, STDOUT_FILENO);
    }
    close(fd);
}

}  // namespace

void setLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    if (!path.empty()) g_file = fopen(path.c_str(), "a");
}

void setLogRotation(const std::string& path, long long maxBytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_rotatePath = path;
    g_rotateMax = maxBytes;
}

void setLogLevel(LogLevel minimum) { g_level = minimum; }

LogLevel logLevel() { return g_level; }

void log(LogLevel level, const char* fmt, ...) {
    if (level < g_level.load()) return;
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);

    timeval tv;
    gettimeofday(&tv, nullptr);
    tm local;
    localtime_r(&tv.tv_sec, &local);
    char stamp[16];
    snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
             static_cast<int>(tv.tv_usec / 1000));

    std::lock_guard<std::mutex> lock(g_mutex);
    fprintf(stderr, "%s %s %s\n", stamp, levelName(level), message);
    rotateIfBig();
    if (g_file) {
        fprintf(g_file, "%s %s %s\n", stamp, levelName(level), message);
        fflush(g_file);
    }
}

}  // namespace zk
