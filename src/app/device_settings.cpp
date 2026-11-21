#include "app/device_settings.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unistd.h>

namespace zk {

namespace fs = std::filesystem;

bool isDeviceKey(const std::string& key) {
    static const std::set<std::string> keys = {
        "syncIntervalMinutes", "flashEvery",     "landscapeReading", "twoColumns",       "annotationColour",
        "penMode",             "penWidth",       "update:available", "update:checkedMs", "update:dismissed",
        "update:failedMs"};
    return keys.count(key) > 0;
}

DeviceSettings::DeviceSettings(std::string file) : file_(std::move(file)) {
    if (file_.empty()) return;
    std::ifstream in(file_, std::ios::binary);
    if (!in) return;
    std::stringstream text;
    text << in.rdbuf();
    const auto j = nlohmann::json::parse(text.str(), nullptr, false);
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.value().is_string()) values_[it.key()] = it.value().get<std::string>();
}

std::optional<std::string> DeviceSettings::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

bool DeviceSettings::set(const std::string& key, std::string value) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[key] = std::move(value);
    return writeLocked();
}

bool DeviceSettings::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    return writeLocked();
}

bool DeviceSettings::writeLocked() {
    if (file_.empty()) return true;
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : values_) j[k] = v;
    const std::string text = j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    const std::string tmp = file_ + ".tmp";
    std::error_code ec;
    if (const fs::path dir = fs::path(file_).parent_path(); !dir.empty()) fs::create_directories(dir, ec);
    // Synced before the rename: a power cut right after must not leave an empty file in its place.
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const bool ok = fd >= 0 && ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && ::fsync(fd) == 0;
    if (fd >= 0) ::close(fd);
    if (!ok) {
        log(LogLevel::Warn, "settings: can't write %s: %s", tmp.c_str(), std::strerror(errno));
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, file_, ec);
    if (ec) {
        log(LogLevel::Warn, "settings: can't keep %s: %s", file_.c_str(), ec.message().c_str());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace zk
