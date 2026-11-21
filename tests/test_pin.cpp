#include <doctest/doctest.h>

#include "app/pin.h"
#include "test_paths.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace zk;
namespace fs = std::filesystem;

TEST_CASE("pin: format is 4 to 6 ASCII digits") {
    CHECK_FALSE(validPinFormat(""));
    CHECK_FALSE(validPinFormat("123"));
    CHECK(validPinFormat("1234"));
    CHECK(validPinFormat("123456"));
    CHECK_FALSE(validPinFormat("1234567"));
    CHECK_FALSE(validPinFormat("12a4"));
    CHECK_FALSE(validPinFormat("12 4"));
}

TEST_CASE("pin: hash then check") {
    auto t0 = std::chrono::steady_clock::now();
    auto hopt = hashPin("4821");
    REQUIRE(hopt);
    PinHash h = *hopt;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("hashPin took " << ms << " ms");
    CHECK(h.iterations == 3);
    CHECK(h.memoryKiB == 1024);
    CHECK_FALSE(h.salt.empty());
    CHECK_FALSE(h.hash.empty());
    CHECK(checkPin(h, "4821"));
    CHECK_FALSE(checkPin(h, "4822"));
    CHECK_FALSE(checkPin(h, ""));
    CHECK_FALSE(checkPin(h, "48210"));
}

TEST_CASE("pin: the same PIN hashes differently each time (salt)") {
    auto ao = hashPin("1234"), bo = hashPin("1234");
    REQUIRE((ao && bo));
    PinHash a = *ao, b = *bo;
    CHECK(a.salt != b.salt);
    CHECK(a.hash != b.hash);
    CHECK(checkPin(a, "1234"));
    CHECK(checkPin(b, "1234"));
}

TEST_CASE("pin: a malformed stored hash never matches") {
    PinHash h = *hashPin("1234");
    PinHash bad = h;
    bad.hash = "!!!";
    CHECK_FALSE(checkPin(bad, "1234"));
    bad = h;
    bad.salt = "";
    CHECK_FALSE(checkPin(bad, "1234"));
}

TEST_CASE("pin: backoff is 5 free misses, then 30 s doubling to 30 min") {
    PinBackoff b;
    const int64_t now = 1'000'000;
    for (int i = 1; i <= 5; i++) {
        recordMiss(b, now);
        CHECK(pinWaitMs(b, now) == 0);
    }
    recordMiss(b, now);
    CHECK(b.misses == 6);
    CHECK(pinWaitMs(b, now) == 30'000);
    CHECK(pinWaitMs(b, now + 10'000) == 20'000);
    CHECK(pinWaitMs(b, now + 30'000) == 0);
    CHECK(pinWaitMs(b, now + 99'000) == 0);
    recordMiss(b, now);
    CHECK(pinWaitMs(b, now) == 60'000);
    recordMiss(b, now);
    CHECK(pinWaitMs(b, now) == 120'000);
    for (int i = 0; i < 20; i++) recordMiss(b, now);
    CHECK(pinWaitMs(b, now) == 1'800'000);
    // A clock moved back since the wait was stored never means more than 30 min.
    CHECK(pinWaitMs(b, now - 10'000'000'000) == 1'800'000);
    recordSuccess(b);
    CHECK(b.misses == 0);
    CHECK(b.waitUntilMs == 0);
    CHECK(pinWaitMs(b, now) == 0);
}

TEST_CASE("pin: the reset key matches trimmed, constant-time") {
    Config c;
    c.apiKey = "abc-123";
    CHECK(keyMatches(c, "abc-123"));
    CHECK(keyMatches(c, "  abc-123\r\n"));
    CHECK_FALSE(keyMatches(c, "abc-124"));
    CHECK_FALSE(keyMatches(c, "abc-12"));
    CHECK_FALSE(keyMatches(c, ""));
    Config none;
    CHECK_FALSE(keyMatches(none, ""));
    CHECK_FALSE(keyMatches(none, "anything"));
}

TEST_CASE("pin: takeResetKey reads, trims and always deletes the drop file") {
    fs::path f = test::uniqueTempPath("zk-reset-pin", ".txt");
    fs::remove(f);
    CHECK_FALSE(takeResetKey(f.string()));
    {
        std::ofstream(f) << "  key-1\n";
    }
    auto k = takeResetKey(f.string());
    REQUIRE(k);
    CHECK(*k == "key-1");
    CHECK_FALSE(fs::exists(f));
    {
        std::ofstream(f) << " \n\n";
    }
    CHECK_FALSE(takeResetKey(f.string()));
    CHECK_FALSE(fs::exists(f));
}

TEST_CASE("pin: takeResetKey deletes a file it cannot read as text, and caps the read") {
    fs::path d = test::uniqueTempPath("zk-reset-dir");
    fs::remove_all(d);
    fs::create_directories(d);
    CHECK_FALSE(takeResetKey(d.string()));  // a directory: not readable as a file
    CHECK_FALSE(fs::exists(d));

    fs::path f = test::uniqueTempPath("zk-reset-big", ".txt");
    {
        std::ofstream(f) << std::string(100000, 'k');
    }
    auto k = takeResetKey(f.string());
    REQUIRE(k);
    CHECK(k->size() == 4096);
    CHECK_FALSE(fs::exists(f));
}
