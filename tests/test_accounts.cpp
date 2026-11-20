#include <doctest/doctest.h>

#include "app/accounts.h"
#include "test_paths.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>

using namespace zk;
namespace fs = std::filesystem;

namespace {

struct Root {
    fs::path path = test::uniqueTempPath("zk-accounts");
    Root() { fs::remove_all(path); fs::create_directories(path); }
    ~Root() { fs::remove_all(path); }
    Paths paths() const {
        Paths p = defaultPaths();
        p.configDir = (path / "var").string();
        p.accountsRoot = (path / "accounts").string();
        return p;
    }
};

AccountEntry entry(int64_t id, int order, bool pin) {
    AccountEntry e;
    e.userId = id;
    e.name = "user" + std::to_string(id);
    e.order = order;
    if (pin) e.pin = PinHash{"c2FsdA==", "aGFzaA==", 3, 1};
    e.lockOnSleep = pin;
    if (pin) e.backoff = {6, 123456789};
    return e;
}

Config cred(int64_t id) {
    Config c;
    c.userId = id;
    c.apiKey = "key" + std::to_string(id);
    c.webdavUrl = "https://dav/";
    return c;
}

}  // namespace

TEST_CASE("accounts: every account path is under <accountsRoot>/<userId>") {
    Paths p = defaultPaths();
    p.accountsRoot = "/x/accounts";
    const AccountPaths a = accountPaths(p, 42);
    const std::string prefix = "/x/accounts/42/";
    for (const std::string& s : {a.dbPath, a.filesDir, a.previewDir, a.creDir, a.fullTextDir, a.stateDir,
                                 a.pluginDataRoot, a.pluginStorage("x"), a.pluginData("x")})
        CHECK(s.rfind(prefix, 0) == 0);
    CHECK(a.root == "/x/accounts/42");
    CHECK(a.dbPath == prefix + "zotero.db");
    CHECK(a.previewDir == prefix + "cache/preview");
    CHECK(a.pluginStorage("x") == prefix + "plugins/x/storage.db");
    CHECK(a.pluginData("x") == prefix + "plugins/x/data");
    CHECK(accountPaths(p, 43).root != a.root);
}

TEST_CASE("accounts: new Paths fields and files") {
    Paths p = defaultPaths();
    CHECK(!p.accountsRoot.empty());
    CHECK(!p.deviceSettingsFile.empty());
    CHECK(p.accountsFile() == p.configDir + "/accounts.json");
    CHECK(p.credentialsFile(7) == p.configDir + "/accounts/7.json");
}

TEST_CASE("accounts: the list round-trips three entries with PINs and backoff") {
    Root r;
    const std::string f = (r.path / "accounts.json").string();
    AccountsState s;
    s.active = 2;
    s.accounts = {entry(1, 0, true), entry(2, 1, false), entry(3, 2, true)};
    REQUIRE(writeAccounts(f, s));
    auto back = readAccounts(f);
    REQUIRE(back);
    CHECK(back->version == 1);
    CHECK(back->active == 2);
    REQUIRE(back->accounts.size() == 3);
    CHECK(back->accounts[0].name == "user1");
    REQUIRE(back->accounts[0].pin);
    CHECK(back->accounts[0].pin->salt == "c2FsdA==");
    CHECK(back->accounts[0].pin->hash == "aGFzaA==");
    CHECK(back->accounts[0].pin->iterations == 3);
    CHECK(back->accounts[0].lockOnSleep);
    CHECK(back->accounts[0].backoff.misses == 6);
    CHECK(back->accounts[0].backoff.waitUntilMs == 123456789);
    CHECK(!back->accounts[1].pin);
    CHECK(back->accounts[2].order == 2);
    struct stat st{};
    REQUIRE(::stat(f.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
}

TEST_CASE("accounts: a fourth account is refused and nothing is written") {
    Root r;
    const std::string f = (r.path / "accounts.json").string();
    AccountsState s;
    s.accounts = {entry(1, 0, false), entry(2, 1, false), entry(3, 2, false), entry(4, 3, false)};
    CHECK_FALSE(writeAccounts(f, s));
    CHECK_FALSE(fs::exists(f));
}

TEST_CASE("accounts: a torn temp file leaves the old list readable") {
    Root r;
    const std::string f = (r.path / "accounts.json").string();
    AccountsState s;
    s.active = 1;
    s.accounts = {entry(1, 0, false)};
    REQUIRE(writeAccounts(f, s));
    std::ofstream(f + ".tmp") << "{\"version\":1,\"active\":9,\"acc";
    auto back = readAccounts(f);
    REQUIRE(back);
    CHECK(back->active == 1);
    CHECK_FALSE(readAccounts((r.path / "none.json").string()));
}

TEST_CASE("accounts: credentials round-trip per account, mode 0600") {
    Root r;
    Paths p = r.paths();
    REQUIRE(saveCredentials(p, cred(11)));
    REQUIRE(saveCredentials(p, cred(12)));
    auto c = loadCredentials(p, 11);
    REQUIRE(c);
    CHECK(c->apiKey == "key11");
    CHECK(c->webdavUrl == "https://dav/");
    CHECK(loadCredentials(p, 12)->apiKey == "key12");
    CHECK_FALSE(loadCredentials(p, 13));
    struct stat st{};
    REQUIRE(::stat(p.credentialsFile(11).c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
}

TEST_CASE("accounts: repair drops an entry without its dir and keeps an orphan dir") {
    Root r;
    Paths p = r.paths();
    for (int64_t id : {1, 2, 3}) saveCredentials(p, cred(id));
    fs::create_directories(accountPaths(p, 1).root);
    fs::create_directories(accountPaths(p, 3).root);  // 2 has credentials but no dir
    fs::create_directories(accountPaths(p, 99).root);  // orphan
    AccountsState s;
    s.active = 2;
    s.accounts = {entry(1, 0, false), entry(2, 1, false), entry(3, 2, false), };
    auto log = repairAccounts(p, s);
    REQUIRE(s.accounts.size() == 2);
    CHECK(s.accounts[0].userId == 1);
    CHECK(s.accounts[1].userId == 3);
    CHECK(s.active == 1);
    CHECK_FALSE(log.empty());
    CHECK(fs::exists(accountPaths(p, 99).root));
    CHECK(fs::exists(p.credentialsFile(2)));  // left alone

    AccountsState t;
    t.active = 5;
    t.accounts = {entry(5, 0, false)};  // no credentials, no dir
    repairAccounts(p, t);
    CHECK(t.accounts.empty());
    CHECK(t.active == 0);

    // A library without its key (migrated from an install with no config.json) stays listed: the
    // app asks for the key (Update key) instead of hiding the library.
    fs::create_directories(accountPaths(p, 7).root);
    AccountsState u;
    u.active = 7;
    u.accounts = {entry(7, 0, false)};
    repairAccounts(p, u);
    REQUIRE(u.accounts.size() == 1);
    CHECK(u.active == 7);
}

TEST_CASE("accounts: wrong-typed or malformed files read as nothing and never throw") {
    Root r;
    const std::string f = (r.path / "accounts.json").string();
    for (const char* body : {"{\"accounts\":[{\"userId\":1,\"name\":5}]}",
                             "{\"accounts\":[{\"userId\":1,\"pin\":{\"iterations\":\"x\"}}]}",
                             "{\"active\":\"a\",\"accounts\":[]}",
                             "{\"accounts\":[{\"userId\":1,\"backoff\":{\"misses\":\"m\"}}]}",
                             "{\"accounts\":[{\"userId\":1,\"lockOnSleep\":3}]}",
                             "{\"accounts\": [", "[]", ""}) {
        std::ofstream(f, std::ios::trunc) << body;
        CHECK_NOTHROW(readAccounts(f));
        CHECK_FALSE(readAccounts(f));
    }
}

TEST_CASE("accounts: duplicate user ids are refused on read and on write") {
    Root r;
    const std::string f = (r.path / "accounts.json").string();
    AccountsState s;
    s.accounts = {entry(1, 0, false), entry(1, 1, false)};
    CHECK_FALSE(writeAccounts(f, s));
    CHECK_FALSE(fs::exists(f));
    std::ofstream(f) << "{\"accounts\":[{\"userId\":1},{\"userId\":1}]}";
    CHECK_FALSE(readAccounts(f));
}

TEST_CASE("accounts: a PIN hash defaults to 1 MiB of Argon2i memory") {
    PinHash h;
    CHECK(h.memoryKiB == 1024);
    CHECK(h.iterations == 3);
    Root r;
    const std::string f = (r.path / "accounts.json").string();
    std::ofstream(f) << "{\"accounts\":[{\"userId\":1,\"pin\":{\"salt\":\"s\",\"hash\":\"h\"}}]}";
    auto back = readAccounts(f);
    REQUIRE(back);
    CHECK(back->accounts[0].pin->memoryKiB == 1024);
}

TEST_CASE("accounts: credentials under another id's name are rejected; orphan credentials are logged") {
    Root r;
    Paths p = r.paths();
    REQUIRE(saveCredentials(p, cred(21)));
    fs::copy_file(p.credentialsFile(21), p.credentialsFile(22));
    CHECK_FALSE(loadCredentials(p, 22));
    fs::create_directories(accountPaths(p, 5).root);
    saveCredentials(p, cred(5));
    AccountsState s;
    s.active = 5;
    s.accounts = {entry(5, 0, false)};
    auto log = repairAccounts(p, s);
    bool orphan = false;
    for (auto& l : log) orphan = orphan || l.find("orphan credentials") != std::string::npos;
    CHECK(orphan);
}

TEST_CASE("accounts: every account's plugin data root, listed or not; dot-dirs and files aren't accounts") {
    Root r;
    Paths p = r.paths();
    CHECK(pluginDataRoots(p).empty());  // no accounts folder yet
    fs::create_directories(p.accountsRoot + "/111/plugins");
    fs::create_directories(p.accountsRoot + "/222");  // no plugin data yet: still its root
    fs::create_directories(p.accountsRoot + "/.leftovers/1");
    fs::create_directories(p.accountsRoot + "/notanid");
    std::ofstream(p.accountsRoot + "/333") << "a file";
    auto roots = pluginDataRoots(p);
    std::sort(roots.begin(), roots.end());
    CHECK(roots == std::vector<std::string>{accountPaths(p, 111).pluginDataRoot, accountPaths(p, 222).pluginDataRoot});
}
