#pragma once

#include <string>

namespace zk {

enum class LogLevel { Debug, Info, Warn, Error };

// Writes "HH:MM:SS.mmm LEVEL message" to stderr and, if set, to the log file. Lines below the
// minimum level are dropped: Info by default, Debug when ZK_LOG_DEBUG is set in the environment.
void log(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Appends to `path` from now on; an empty path stops file logging.
void setLogFile(const std::string& path);

// The launcher sends the app's stderr to log/zotero.log and caps it only at a start. While stderr is
// that file (`path`), a log line that finds it over `maxBytes` moves it to <path>.1 (replacing the
// old one) and goes on in a new file, as the launcher does. An empty path stops it.
void setLogRotation(const std::string& path, long long maxBytes);

void setLogLevel(LogLevel minimum);
LogLevel logLevel();

}  // namespace zk
