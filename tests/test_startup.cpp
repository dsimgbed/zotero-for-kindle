// The USB drop with accounts (spec: multiple accounts, Migration): a config.json put into
// /mnt/us/zotero/ is the same user's new key, a new account, or one too many — and never written
// into another user's account.
#include <doctest/doctest.h>

#include "app/accounts.h"
#include "app/pin.h"
#include "app/startup.h"
#include "sync/config.h"
#include "test_paths.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace zk;
namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

Config cred(int64_t id, const std::string& key) {
    Config c;
    c.userId = id;
    c.apiKey = key;
    c.webdavUrl = "https://dav/" + std::to_string(id);
    return c;
}

// A Kindle with the accounts `ids` (the first one active), each with its key: /var/local/zotero
// is <root>/var, /mnt/us/zotero is <root>/us.
struct Kindle {
    fs::path root = test::uniqueTempPath("zk-startup-drop");
    Paths p;
    explicit Kindle(std::vector<int64_t> ids) {
        fs::remove_all(root);
        p = defaultPaths();
        p.configDir = (root / "var").string();
        p.droppedConfig = (root / "us/config.json").string();
        p.dbPath = (root / "us/zotero.db").string();
        p.accountsRoot = (root / "us/accounts").string();
        p.deviceSettingsFile = (root / "us/device.json").string();
        for (int64_t id : ids) {
            REQUIRE(saveCredentials(p, cred(id, "key" + std::to_string(id))));
            REQUIRE(addAccount(p, id, "user" + std::to_string(id)));
        }
    }
    ~Kindle() { fs::remove_all(root); }
    void drop(const std::string& text) const { writeFile(p.droppedConfig, text); }
    std::string list() const { return readFile(p.accountsFile()); }
    std::string credentials(int64_t id) const { return readFile(p.credentialsFile(id)); }
};

}  // namespace

TEST_CASE("startup drop: the same user's key waits for Zotero's word; nothing changes before") {
    Kindle k({111, 222});
    const std::string list = k.list(), first = k.credentials(111), second = k.credentials(222);
    const std::string dropped = R"({"userId":222,"apiKey":"new222"})";
    k.drop(dropped);
    const Startup s = startupConfig(k.p);
    CHECK(s.dropped == Startup::Dropped::KeyPending);
    CHECK(s.droppedUser == 222);
    CHECK_FALSE(s.imported);
    CHECK(k.credentials(222) == second);  // not taken on the file's word
    CHECK(k.credentials(111) == first);
    CHECK(k.list() == list);
    CHECK(readFile(k.p.droppedConfig) == dropped);  // left until Zotero says whose it is
    REQUIRE(s.config);
    CHECK(s.config->userId == 111);
    REQUIRE(pendingDroppedKey(k.p));
    CHECK(pendingDroppedKey(k.p)->apiKey == "new222");
}

TEST_CASE("startup drop: the same user's key is kept once Zotero says it is that user's, and only then") {
    Kindle k({111, 222});
    const std::string first = k.credentials(111), second = k.credentials(222);
    const std::string dropped = R"({"userId":222,"apiKey":"new222"})";
    k.drop(dropped);
    std::vector<Config> asked;
    auto says = [&](UserError answer) {
        return [&asked, answer](const Config& c) {
            asked.push_back(c);
            return answer;
        };
    };
    // Offline: nothing changes, the file waits.
    CHECK(applyDroppedKey(k.p, says({ErrorKind::NoWifi, "No Wi-Fi.", "offline"})) == DroppedKey::Offline);
    CHECK(k.credentials(222) == second);
    CHECK(readFile(k.p.droppedConfig) == dropped);
    // Another user's key (or none Zotero knows): refused, the file left, nothing changes.
    CHECK(applyDroppedKey(k.p, says({ErrorKind::ApiKeyRejected, "This API key belongs to another Zotero user.",
                                     "userID mismatch"})) == DroppedKey::Refused);
    CHECK(k.credentials(222) == second);
    CHECK(readFile(k.p.droppedConfig) == dropped);
    // The owner's: kept, under its own user only; the file goes.
    Config kept;
    CHECK(applyDroppedKey(k.p, says({}), &kept) == DroppedKey::Applied);
    CHECK(kept.userId == 222);
    CHECK(loadCredentials(k.p, 222)->apiKey == "new222");
    CHECK(k.credentials(111) == first);
    CHECK_FALSE(fs::exists(k.p.droppedConfig));
    REQUIRE(asked.size() == 3);
    CHECK(asked.back().userId == 222);
    CHECK(asked.back().apiKey == "new222");
    // Nothing dropped: nothing asked.
    CHECK(applyDroppedKey(k.p, says({})) == DroppedKey::None);
    CHECK(asked.size() == 3);
}

TEST_CASE("startup drop: a key dropped for a PIN-locked account can't reset its PIN while unproved") {
    Kindle k({111});
    AccountPins pins(k.p.accountsFile(), [] { return int64_t{1'000'000}; }, [] { return int64_t{0}; });
    REQUIRE(pins.setPin(111, "1234"));
    k.drop(R"({"userId":111,"apiKey":"anything"})");
    const Startup s = startupConfig(k.p);
    CHECK(s.dropped == Startup::Dropped::KeyPending);
    CHECK(applyDroppedKey(k.p, [](const Config&) { return UserError{ErrorKind::NoWifi, "No Wi-Fi.", "offline"}; }) ==
          DroppedKey::Offline);
    CHECK(applyDroppedKey(k.p, [](const Config&) {
              return UserError{ErrorKind::ApiKeyRejected, "This API key is invalid.", "HTTP 403"};
          }) == DroppedKey::Refused);
    // reset-pin.txt with that key: not this account's key, so no new PIN.
    const auto stored = loadCredentials(k.p, 111);
    REQUIRE(stored);
    CHECK(stored->apiKey == "key111");
    CHECK(pins.tryKey(111, *stored, "anything") != AccountPins::Result::Ok);
    CHECK(pins.entry(111)->pin);
}

TEST_CASE("startup drop: a new user is added and listed; the open account stays open") {
    Kindle k({111, 222});
    const std::string first = k.credentials(111), second = k.credentials(222);
    k.drop(R"({"userId":333,"apiKey":"key333"})");
    const Startup s = startupConfig(k.p);
    CHECK(s.dropped == Startup::Dropped::Added);
    CHECK(s.droppedUser == 333);
    CHECK_FALSE(s.imported);
    const auto st = readAccounts(k.p.accountsFile());
    REQUIRE(st);
    REQUIRE(st->accounts.size() == 3);
    CHECK(st->accounts.back().userId == 333);
    CHECK(st->active == 111);
    CHECK(fs::is_directory(accountPaths(k.p, 333).root));
    CHECK(loadCredentials(k.p, 333)->apiKey == "key333");
    CHECK(k.credentials(111) == first);
    CHECK(k.credentials(222) == second);
    CHECK_FALSE(fs::exists(k.p.droppedConfig));
    CHECK_FALSE(fs::exists(k.p.configFile()));
    REQUIRE(s.config);
    CHECK(s.config->userId == 111);
}

TEST_CASE("startup drop: a new user with no account to open is added and opened") {
    Kindle k({111});
    fs::remove(k.p.credentialsFile(111));  // its key is gone: nothing can be opened
    k.drop(R"({"userId":333,"apiKey":"key333"})");
    const Startup s = startupConfig(k.p);
    CHECK(s.dropped == Startup::Dropped::Added);
    REQUIRE(s.config);
    CHECK(s.config->userId == 333);
    CHECK(readAccounts(k.p.accountsFile())->active == 333);
}

TEST_CASE("startup drop: a fourth user is refused, logged, and the file left in place") {
    Kindle k({111, 222, 333});
    const std::string list = k.list();
    const std::string dropped = R"({"userId":444,"apiKey":"key444"})";
    k.drop(dropped);
    const Startup s = startupConfig(k.p);
    CHECK(s.dropped == Startup::Dropped::Refused);
    CHECK(s.droppedUser == 444);
    CHECK(readFile(k.p.droppedConfig) == dropped);
    CHECK(k.list() == list);
    CHECK_FALSE(fs::exists(k.p.credentialsFile(444)));
    CHECK_FALSE(fs::exists(accountPaths(k.p, 444).root));
    CHECK_FALSE(fs::exists(k.p.configFile()));
    REQUIRE(s.config);
    CHECK(s.config->userId == 111);
    // Still there at the next start: refused again, nothing changed.
    CHECK(startupConfig(k.p).dropped == Startup::Dropped::Refused);
    CHECK(k.list() == list);
}

TEST_CASE("startup drop: a file without a user or key is left in place and changes nothing") {
    Kindle k({111});
    const std::string list = k.list();
    k.drop(R"({"apiKey":"no user"})");
    const Startup s = startupConfig(k.p);
    CHECK(s.dropped == Startup::Dropped::Invalid);
    CHECK(fs::exists(k.p.droppedConfig));
    CHECK(k.list() == list);
    REQUIRE(s.config);
    CHECK(s.config->userId == 111);
}

TEST_CASE("startup drop: with no account on the Kindle, Setup confirms the dropped one") {
    for (const bool listEmpty : {false, true}) {
        Kindle k({});
        if (listEmpty) REQUIRE(writeAccounts(k.p.accountsFile(), AccountsState{}));  // the last account was removed
        k.drop(R"({"userId":333,"apiKey":"key333"})");
        const Startup s = startupConfig(k.p);
        CHECK(s.dropped == Startup::Dropped::FirstAccount);
        CHECK(s.imported);
        REQUIRE(s.config);
        CHECK(s.config->userId == 333);
        CHECK_FALSE(fs::exists(k.p.credentialsFile(333)));  // identity first: nothing made before Setup's check
        CHECK_FALSE(fs::exists(accountPaths(k.p, 333).root));
    }
}
