#pragma once
// Settings that belong to the Kindle, not to an account: kept in one JSON object file
// (device.json), so they stay the same whichever account is open. isDeviceKey() says which keys
// those are; everything else is an account's, in its library.

#include <map>
#include <optional>
#include <string>

namespace zk {

class DeviceSettings {
public:
    // An empty `file` keeps the settings in memory only (tests, screenshots).
    explicit DeviceSettings(std::string file);
    std::optional<std::string> get(const std::string& key) const;
    void set(const std::string& key, std::string value);  // written at once, through a temp file and rename

private:
    std::string file_;
    std::map<std::string, std::string> values_;
};

// The device keys: syncIntervalMinutes, flashEvery, landscapeReading, twoColumns, the reader's
// annotationColour / penMode / penWidth, and the update notice's update:* keys.
bool isDeviceKey(const std::string& key);

}  // namespace zk
