#pragma once
// User settings. The device's (sync interval, refresh, landscape, columns) are in device.json
// (app/device_settings.h); the account's (the rest) are in its library database (meta key
// "settings", JSON).

#include <string>

#include "app/device_settings.h"
#include "store/library.h"

namespace zk {

struct AppSettings {
    int syncIntervalMinutes = 10;  // 0 = off
    int flashEvery = 6;            // page turns between full refreshes (used by the reader, Plan 4)
    bool landscapeReading = false;  // turn the screen while a book is open (off by default)
    int twoColumns = -1;            // EPUB in landscape: -1 by screen (on when the page is ≥ 2000 px wide), 0 off, 1 on
    std::string homeScreen;         // Home's body (Settings → Home): "" built-in, else "<pluginId>/<screenId>"
};

AppSettings loadSettings(Library&, const DeviceSettings&);
void saveSettings(Library&, DeviceSettings&, const AppSettings&);

// Copies the device keys found in the library's meta and "settings" JSON into `device` (a value
// already there wins), then removes them from the library. For a library from before the split.
void moveDeviceKeys(Library&, DeviceSettings&);

}  // namespace zk
