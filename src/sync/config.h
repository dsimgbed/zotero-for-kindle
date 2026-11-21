#pragma once
// Account and WebDAV settings. On the Kindle they live in /var/local/zotero/config.json (root
// only, mode 0600); a config.json dropped on /mnt/us/zotero/ over USB is imported once and then
// deleted. Desktop runs read local/credentials.env. Values are never logged.

#include <cstdint>
#include <optional>
#include <string>

namespace zk {

struct Paths;

struct Config {
    int64_t userId = 0;
    std::string apiKey, webdavUrl, webdavUser, webdavPassword;
};

std::optional<Config> loadConfig(const std::string& dir);
// A config file anywhere (the one dropped over USB); nullopt if absent or without userId and apiKey.
std::optional<Config> readConfigFile(const std::string& path);
bool saveConfig(const std::string& dir, const Config&);
// Per account: <configDir>/accounts/<userId>.json, mode 0600.
std::optional<Config> loadCredentials(const Paths&, int64_t userId);
bool saveCredentials(const Paths&, const Config&);
// Valid dropped file -> saved into `dir`, then the dropped file is deleted (the app's only
// deletion, and of its own local file). An invalid file is left in place.
bool importDroppedConfig(const std::string& droppedPath, const std::string& dir);
std::optional<Config> configFromEnvFile(const std::string& path);

}  // namespace zk
