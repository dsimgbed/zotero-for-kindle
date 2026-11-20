#include "app/settings_store.h"

#include <nlohmann/json.hpp>

namespace zk {

namespace {

// Device-key meta entries of the library itself (outside the "settings" JSON).
const char* const kMetaDeviceKeys[] = {"annotationColour", "penMode",          "penWidth",        "update:available",
                                       "update:checkedMs", "update:dismissed", "update:failedMs"};

int intValue(const DeviceSettings& d, const char* key, int def) {
    const auto v = d.get(key);
    if (!v || v->empty()) return def;
    try {
        size_t pos = 0;
        const int n = std::stoi(*v, &pos);
        return pos == v->size() ? n : def;
    } catch (...) {
        return def;
    }
}

}  // namespace

AppSettings loadSettings(Library& lib, const DeviceSettings& dev) {
    AppSettings s;
    s.syncIntervalMinutes = intValue(dev, "syncIntervalMinutes", s.syncIntervalMinutes);
    s.flashEvery = intValue(dev, "flashEvery", s.flashEvery);
    s.twoColumns = intValue(dev, "twoColumns", s.twoColumns);
    if (const auto v = dev.get("landscapeReading")) s.landscapeReading = *v == "1";
    auto j = nlohmann::json::parse(lib.metaValue("settings"), nullptr, false);
    if (j.is_object() && j.contains("homeScreen") && j["homeScreen"].is_string()) s.homeScreen = j["homeScreen"].get<std::string>();
    return s;
}

void saveSettings(Library& lib, DeviceSettings& dev, const AppSettings& s) {
    dev.set("syncIntervalMinutes", std::to_string(s.syncIntervalMinutes));
    dev.set("flashEvery", std::to_string(s.flashEvery));
    dev.set("landscapeReading", s.landscapeReading ? "1" : "0");
    dev.set("twoColumns", std::to_string(s.twoColumns));
    // Other keys of the account's JSON (a device key left from before the split included) stay as they are.
    auto j = nlohmann::json::parse(lib.metaValue("settings"), nullptr, false);
    if (!j.is_object()) j = nlohmann::json::object();
    j["homeScreen"] = s.homeScreen;
    lib.setMetaValue("settings", j.dump());
}

void moveDeviceKeys(Library& lib, DeviceSettings& dev) {
    auto j = nlohmann::json::parse(lib.metaValue("settings"), nullptr, false);
    if (j.is_object()) {
        bool changed = false;
        for (auto it = j.begin(); it != j.end();) {
            if (!isDeviceKey(it.key())) {
                ++it;
                continue;
            }
            if (!dev.get(it.key())) {
                if (it.value().is_boolean()) dev.set(it.key(), it.value().get<bool>() ? "1" : "0");
                else if (it.value().is_number_integer()) dev.set(it.key(), std::to_string(it.value().get<long long>()));
                else if (it.value().is_string()) dev.set(it.key(), it.value().get<std::string>());
            }
            it = j.erase(it);
            changed = true;
        }
        if (changed) lib.setMetaValue("settings", j.dump());
    }
    for (const char* key : kMetaDeviceKeys) {
        const std::string v = lib.metaValue(key);
        if (v.empty()) continue;
        if (!dev.get(key)) dev.set(key, v);
        lib.setMetaValue(key, "");
    }
}

}  // namespace zk
