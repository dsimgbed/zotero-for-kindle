#pragma once
// Settings that belong to the Kindle, not to an account: kept in one JSON object file
// (device.json), so they stay the same whichever account is open. isDeviceKey() says which keys
// those are; everything else is an account's, in its library.

#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace zk {

class DeviceSettings {
public:
    // An empty `file` keeps the settings in memory only (tests, screenshots).
    explicit DeviceSettings(std::string file);
    std::optional<std::string> get(const std::string& key) const;
    // Kept in memory and written at once, through a temp file and rename; false when the file
    // couldn't be written (the value still holds in memory). Safe from any thread.
    bool set(const std::string& key, std::string value);
    bool flush();  // writes the file again (true when it is on disk as in memory)

private:
    bool writeLocked();
    std::string file_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> values_;
};

// The device keys: syncIntervalMinutes, flashEvery, landscapeReading, twoColumns, the reader's
// annotationColour / penMode / penWidth, and the update notice's update:* keys.
bool isDeviceKey(const std::string& key);

}  // namespace zk
