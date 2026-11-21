// The USB drop with accounts (spec: multiple accounts, Migration): a config.json put into
// /mnt/us/zotero/ is the same user's new key, a new account, or one too many — and never written
// into another user's account.
#include <doctest/doctest.h>

#include "app/accounts.h"
#include "app/pin.h"
#include "app/startup.h"
#include "app/migrate_accounts.h"
#include "sync/config.h"
#include "test_paths.h"

#include <filesystem>
#include <sqlite3.h>
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

// ---- the startup repair (final review): removals cut short, an unreadable list, config.json ----

TEST_CASE("startup repair: a power cut mid-removal — the account isn't listed, .removing-* is gone, the others intact") {
    Kindle k({111, 222, 333});
    writeFile(fs::path(accountPaths(k.p, 111).filesDir) / "K1/book.pdf", "pdf of 111");
    writeFile(fs::path(accountPaths(k.p, 333).filesDir) / "K3/book.pdf", "pdf of 333");
    writeFile(fs::path(accountPaths(k.p, 222).filesDir) / "K2/book.pdf", "pdf of 222");
    REQUIRE(saveConfig(k.p.configDir, cred(222, "key222")));  // a first Setup's copy of 222's key
    // removeAccount's first step (the folder put aside), then the power goes: the list still names 222.
    AccountsState st = *readAccounts(k.p.accountsFile());
    st.active = 222;
    REQUIRE(writeAccounts(k.p.accountsFile(), st));
    const fs::path aside = fs::path(k.p.accountsRoot) / ".removing-222";
    fs::rename(accountPaths(k.p, 222).root, aside);
    // A removal of 333's from long ago whose last step (the delete) was cut, 333 since added again.
    writeFile(fs::path(k.p.accountsRoot) / ".removing-333/files/old.pdf", "old");
    const std::string credentials111 = k.credentials(111), credentials333 = k.credentials(333);

    Paths p = k.p;
    const auto active = openAccountLayout(p);  // the next start
    REQUIRE(active);
    const auto now = readAccounts(k.p.accountsFile());
    REQUIRE(now);
    REQUIRE(now->accounts.size() == 2);
    CHECK(now->accounts[0].userId == 111);
    CHECK(now->accounts[1].userId == 333);
    CHECK(now->active == 111);
    CHECK(*active == 111);
    CHECK_FALSE(fs::exists(aside));
    CHECK_FALSE(fs::exists(fs::path(k.p.accountsRoot) / ".removing-333"));
    CHECK_FALSE(fs::exists(accountPaths(k.p, 222).root));       // never made again, empty
    CHECK_FALSE(fs::exists(k.p.credentialsFile(222)));          // the rest of the removal
    CHECK_FALSE(fs::exists(k.p.configFile()));                  // 222's key nowhere
    CHECK(k.credentials(111) == credentials111);
    CHECK(k.credentials(333) == credentials333);                // 333 listed: its key stays
    CHECK(readFile(fs::path(accountPaths(k.p, 111).filesDir) / "K1/book.pdf") == "pdf of 111");
    CHECK(readFile(fs::path(accountPaths(k.p, 333).filesDir) / "K3/book.pdf") == "pdf of 333");
    // Run again: nothing more changes.
    const std::string list = k.list();
    Paths again = k.p;
    CHECK(openAccountLayout(again) == std::optional<int64_t>(111));
    CHECK(k.list() == list);
}

TEST_CASE("startup repair: an unreadable accounts.json is kept aside, and the list made again from the folders, each opening only with its key") {
    Kindle k({111, 222});
    REQUIRE(saveCredentials(k.p, cred(222, "key222")));
    AccountPins pins(k.p.accountsFile(), [] { return int64_t{1'000'000}; }, [] { return int64_t{0}; });
    REQUIRE(pins.setPin(111, "1234"));
    {  // 222's library knows its username
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open(accountPaths(k.p, 222).dbPath.c_str(), &db) == SQLITE_OK);
        sqlite3_exec(db, "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT); INSERT INTO meta VALUES ('zotero.userName','anna');",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    fs::create_directories(fs::path(k.p.accountsRoot) / ".leftovers");  // not an account
    writeFile(k.p.accountsFile(), "{\"version\":1,\"accounts\":[{\"userId\":111,");  // torn by hand, or a bad card
    // Nothing may write a new list over it meanwhile (it would drop the others and their PINs).
    CHECK_FALSE(addAccount(k.p, 444, "x"));
    CHECK(readFile(k.p.accountsFile()) == "{\"version\":1,\"accounts\":[{\"userId\":111,");

    Paths p = k.p;
    REQUIRE(openAccountLayout(p));
    const auto st = readAccounts(k.p.accountsFile());
    REQUIRE(st);
    REQUIRE(st->accounts.size() == 2);
    CHECK(st->accounts[0].userId == 111);
    CHECK(st->accounts[1].userId == 222);
    CHECK(st->accounts[1].name == "anna");
    CHECK(st->active == 111);
    for (const AccountEntry& e : st->accounts) {
        CHECK(e.needsKey);  // a PIN it may have had isn't known: its key is asked instead
        CHECK_FALSE(e.pin);
        CHECK(asksBeforeOpening(e));
    }
    bool keptAside = false;
    for (const auto& f : fs::directory_iterator(k.p.configDir))
        if (f.path().filename().string().rfind("accounts.json.bad-", 0) == 0)
            keptAside = readFile(f.path()) == "{\"version\":1,\"accounts\":[{\"userId\":111,";
    CHECK(keptAside);
    // A PIN opens nothing; the account's key does, and from then on it needs none.
    CHECK(pins.tryPin(111, "1234") != AccountPins::Result::Ok);
    CHECK(pins.tryPin(111, "0000") != AccountPins::Result::Ok);
    CHECK(pins.tryKey(111, *loadCredentials(k.p, 111), "wrong") == AccountPins::Result::Wrong);
    CHECK(pins.tryKey(111, *loadCredentials(k.p, 111), "key111") == AccountPins::Result::Ok);
    CHECK_FALSE(pins.entry(111)->needsKey);
    CHECK(pins.entry(222)->needsKey);
}

TEST_CASE("startup: with an account list, config.json is nobody's key; a first Setup writes it only on a fresh install") {
    Kindle fresh(std::vector<int64_t>{});
    CHECK(firstSetupKeepsConfigFile(fresh.p));
    REQUIRE(saveConfig(fresh.p.configDir, cred(555, "key555")));
    CHECK(startupConfig(fresh.p).config->userId == 555);  // before accounts: the first account's key

    Kindle k({111});
    CHECK_FALSE(firstSetupKeepsConfigFile(k.p));
    REQUIRE(removeAccount(k.p, 111));  // every account removed: the list stays, empty
    REQUIRE(saveConfig(k.p.configDir, cred(111, "key111")));  // a stale copy (from before this fix)
    const Startup s = startupConfig(k.p);
    CHECK_FALSE(s.config);  // the next Setup is empty
    CHECK_FALSE(firstSetupKeepsConfigFile(k.p));
}

TEST_CASE("startup drop: Zotero's rate limit or a server error on a dropped key is 'later', never 'not this account's'") {
    Kindle k({111, 222});
    const std::string dropped = R"({"userId":222,"apiKey":"new222"})";
    k.drop(dropped);
    const std::string second = k.credentials(222);
    for (const UserError& e : {UserError{ErrorKind::RateLimited, "Zotero asks to wait.", "HTTP 429"},
                               UserError{ErrorKind::ServerError, "Zotero had a problem (503).", "HTTP 503"}}) {
        CHECK(applyDroppedKey(k.p, [&](const Config&) { return e; }) == DroppedKey::Later);
        CHECK(k.credentials(222) == second);
        CHECK(readFile(k.p.droppedConfig) == dropped);
    }
    CHECK(applyDroppedKey(k.p, [](const Config&) { return UserError{}; }) == DroppedKey::Applied);
}
