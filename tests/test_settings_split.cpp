#include <doctest/doctest.h>

#include "app/device_settings.h"
#include "app/settings_store.h"
#include "store/db.h"
#include "store/library.h"
#include "test_paths.h"

#include <filesystem>
#include <fstream>

using namespace zk;
namespace fs = std::filesystem;

namespace {

struct Dir {
    fs::path path = test::uniqueTempPath("zk-settings-split");
    Dir() { fs::remove_all(path); fs::create_directories(path); }
    ~Dir() { fs::remove_all(path); }
    std::string file() const { return (path / "device.json").string(); }
};

}  // namespace

TEST_CASE("settings split: device settings are stored in the file and read back by a new object") {
    Dir d;
    {
        DeviceSettings dev(d.file());
        CHECK_FALSE(dev.get("flashEvery"));
        dev.set("flashEvery", "3");
    }
    DeviceSettings again(d.file());
    CHECK(again.get("flashEvery") == "3");
    CHECK_FALSE(fs::exists(d.file() + ".tmp"));
}

TEST_CASE("settings split: an unreadable device file is an empty one") {
    Dir d;
    std::ofstream(d.file()) << "{not json";
    DeviceSettings dev(d.file());
    CHECK_FALSE(dev.get("flashEvery"));
    dev.set("flashEvery", "2");
    CHECK(DeviceSettings(d.file()).get("flashEvery") == "2");
}

TEST_CASE("settings split: which keys are the device's") {
    for (const char* k : {"syncIntervalMinutes", "flashEvery", "landscapeReading", "twoColumns", "annotationColour", "penMode",
                          "penWidth", "update:available", "update:checkedMs", "update:dismissed", "update:failedMs"})
        CHECK(isDeviceKey(k));
    CHECK_FALSE(isDeviceKey("homeScreen"));
    CHECK_FALSE(isDeviceKey("libraryVersion"));
}

TEST_CASE("settings split: a device key saved with account A is read with account B, homeScreen isn't") {
    Dir d;
    DeviceSettings dev(d.file());
    Db dbA{Db::open(":memory:")}, dbB{Db::open(":memory:")};
    Library a{dbA}, b{dbB};
    AppSettings s = loadSettings(a, dev);
    s.flashEvery = 3;
    s.syncIntervalMinutes = 0;
    s.landscapeReading = true;
    s.twoColumns = 1;
    s.homeScreen = "desk/home";
    saveSettings(a, dev, s);
    const AppSettings seen = loadSettings(b, dev);
    CHECK(seen.flashEvery == 3);
    CHECK(seen.syncIntervalMinutes == 0);
    CHECK(seen.landscapeReading);
    CHECK(seen.twoColumns == 1);
    CHECK(seen.homeScreen.empty());
    CHECK(loadSettings(a, dev).homeScreen == "desk/home");
}

TEST_CASE("settings split: moveDeviceKeys moves device keys from a database into an empty device file") {
    Dir d;
    Db db{Db::open(":memory:")};
    Library lib{db};
    lib.setMetaValue("settings", R"({"syncIntervalMinutes":30,"flashEvery":4,"landscapeReading":true,"twoColumns":0,"homeScreen":"desk/home"})");
    lib.setMetaValue("update:checkedMs", "12345");
    lib.setMetaValue("penWidth", "2");
    DeviceSettings dev(d.file());
    moveDeviceKeys(lib, dev);
    CHECK(dev.get("flashEvery") == "4");
    CHECK(dev.get("update:checkedMs") == "12345");
    CHECK(dev.get("penWidth") == "2");
    const AppSettings s = loadSettings(lib, dev);
    CHECK(s.syncIntervalMinutes == 30);
    CHECK(s.flashEvery == 4);
    CHECK(s.landscapeReading);
    CHECK(s.twoColumns == 0);
    CHECK(s.homeScreen == "desk/home");  // stays in the database
    CHECK(lib.metaValue("update:checkedMs").empty());
    CHECK(lib.metaValue("penWidth").empty());
    CHECK(lib.metaValue("settings").find("flashEvery") == std::string::npos);
    CHECK(lib.metaValue("settings").find("homeScreen") != std::string::npos);
    CHECK(DeviceSettings(d.file()).get("flashEvery") == "4");  // on disk
}

TEST_CASE("settings split: moveDeviceKeys keeps the value already on the device") {
    Dir d;
    Db db{Db::open(":memory:")};
    Library lib{db};
    lib.setMetaValue("settings", R"({"flashEvery":4})");
    DeviceSettings dev(d.file());
    dev.set("flashEvery", "9");
    moveDeviceKeys(lib, dev);
    CHECK(dev.get("flashEvery") == "9");
    CHECK(lib.metaValue("settings").find("flashEvery") == std::string::npos);
}

TEST_CASE("settings split: the update notice's state is on the device, so it survives a library swap") {
    Dir d;
    DeviceSettings dev(d.file());
    dev.set("update:available", "0.3.0");
    Db dbA{Db::open(":memory:")}, dbB{Db::open(":memory:")};
    Library a{dbA}, b{dbB};
    (void)a;
    (void)b;
    CHECK(DeviceSettings(d.file()).get("update:available") == "0.3.0");
    CHECK(b.metaValue("update:available").empty());
}
