#include "app/accounts.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace zk {

using nlohmann::json;
namespace fs = std::filesystem;

std::vector<std::string> pluginDataRoots(const Paths& p) {
    std::vector<std::string> roots;
    std::error_code ec;
    for (fs::directory_iterator it(p.accountsRoot, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        std::error_code e2;
        if (name.empty() || name.size() > 18 || !it->is_directory(e2) ||
            !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        const int64_t userId = std::stoll(name);
        if (userId > 0) roots.push_back(accountPaths(p, userId).pluginDataRoot);
    }
    return roots;
}

AccountPaths accountPaths(const Paths& p, int64_t userId) {
    AccountPaths a;
    a.root = p.accountsRoot + "/" + std::to_string(userId);
    a.dbPath = a.root + "/zotero.db";
    a.filesDir = a.root + "/files";
    a.previewDir = a.root + "/cache/preview";
    a.creDir = a.root + "/cache/cre";
    a.fullTextDir = a.root + "/cache/fulltext";
    a.stateDir = a.root + "/state";
    a.pluginDataRoot = a.root + "/plugins";
    return a;
}

namespace {

bool hasDuplicates(const std::vector<AccountEntry>& v) {
    for (size_t i = 0; i < v.size(); ++i)
        for (size_t k = i + 1; k < v.size(); ++k)
            if (v[i].userId == v[k].userId) return true;
    return false;
}

}  // namespace

std::optional<AccountsState> readAccounts(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        json j = json::parse(ss.str(), nullptr, false);
        if (!j.is_object() || !j.contains("accounts") || !j["accounts"].is_array()) return std::nullopt;
        AccountsState s;
        s.version = j.value("version", 1);
        s.active = j.value("active", int64_t{0});
        for (const json& a : j["accounts"]) {
            if (!a.is_object() || !a.contains("userId") || !a["userId"].is_number_integer()) return std::nullopt;
            AccountEntry e;
            e.userId = a["userId"].get<int64_t>();
            e.name = a.value("name", std::string{});
            e.order = a.value("order", 0);
            e.lockOnSleep = a.value("lockOnSleep", false);
            if (a.contains("pin") && a["pin"].is_object()) {
                const json& p = a["pin"];
                e.pin = PinHash{p.value("salt", std::string{}), p.value("hash", std::string{}), p.value("iterations", 3),
                                p.value("memoryKiB", 1024)};
            }
            e.itemCount = a.value("itemCount", int64_t{0});
            e.unsentCount = a.value("unsentCount", 0);
            e.syncedWallMs = a.value("syncedWallMs", int64_t{0});
            if (a.contains("backoff") && a["backoff"].is_object()) {
                e.backoff.misses = a["backoff"].value("misses", 0);
                e.backoff.waitUntilMs = a["backoff"].value("waitUntilMs", int64_t{0});
            }
            s.accounts.push_back(std::move(e));
        }
        if (s.accounts.size() > kMaxAccounts || hasDuplicates(s.accounts)) {
            log(LogLevel::Error, "accounts: %s lists too many or duplicate accounts; ignored", file.c_str());
            return std::nullopt;
        }
        return s;
    } catch (const json::exception& ex) {
        log(LogLevel::Error, "accounts: %s is malformed (%s); ignored", file.c_str(), ex.what());
        return std::nullopt;
    }
}

bool writeAccounts(const std::string& file, const AccountsState& s) {
    if (s.accounts.size() > kMaxAccounts) {
        log(LogLevel::Error, "accounts: refusing to write %zu accounts (max %zu)", s.accounts.size(), kMaxAccounts);
        return false;
    }
    if (hasDuplicates(s.accounts)) {
        log(LogLevel::Error, "accounts: refusing to write duplicate user ids");
        return false;
    }
    json list = json::array();
    for (const AccountEntry& e : s.accounts) {
        json pin = nullptr;
        if (e.pin) pin = {{"salt", e.pin->salt}, {"hash", e.pin->hash}, {"iterations", e.pin->iterations},
                          {"memoryKiB", e.pin->memoryKiB}};
        list.push_back({{"userId", e.userId}, {"name", e.name}, {"order", e.order}, {"pin", pin},
                        {"lockOnSleep", e.lockOnSleep},
                        {"backoff", {{"misses", e.backoff.misses}, {"waitUntilMs", e.backoff.waitUntilMs}}},
                        {"itemCount", e.itemCount}, {"unsentCount", e.unsentCount}, {"syncedWallMs", e.syncedWallMs}});
    }
    const std::string text = json{{"version", s.version}, {"active", s.active}, {"accounts", list}}.dump(2) + "\n";
    std::error_code ec;
    fs::create_directories(fs::path(file).parent_path(), ec);
    const std::string tmp = file + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        log(LogLevel::Error, "accounts: can't write %s: %s", tmp.c_str(), std::strerror(errno));
        return false;
    }
    ::fchmod(fd, 0600);
    const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || std::rename(tmp.c_str(), file.c_str()) != 0) {
        log(LogLevel::Error, "accounts: can't save %s: %s", file.c_str(), std::strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::vector<std::string> repairAccounts(const Paths& paths, AccountsState& s) {
    std::vector<std::string> lines;
    auto note = [&](std::string line) {
        log(LogLevel::Warn, "accounts: %s", line.c_str());
        lines.push_back(std::move(line));
    };
    std::vector<AccountEntry> kept;
    for (AccountEntry& e : s.accounts) {
        if (fs::is_directory(accountPaths(paths, e.userId).root)) {
            // Without credentials it stays: its library is there, the app asks for the key (Update key).
            if (!loadCredentials(paths, e.userId)) note("account " + std::to_string(e.userId) + " has no credentials; kept");
            kept.push_back(std::move(e));
            continue;
        }
        note("account " + std::to_string(e.userId) + " dropped from the list: missing data dir");
    }
    s.accounts = std::move(kept);
    if (std::none_of(s.accounts.begin(), s.accounts.end(), [&](const AccountEntry& e) { return e.userId == s.active; }))
        s.active = s.accounts.empty() ? 0 : s.accounts.front().userId;
    auto listed = [&](const std::string& name) {
        return std::any_of(s.accounts.begin(), s.accounts.end(),
                           [&](const AccountEntry& e) { return std::to_string(e.userId) == name; });
    };
    std::error_code ec;
    for (const auto& d : fs::directory_iterator(paths.accountsRoot, ec))
        if (d.is_directory() && d.path().filename().string()[0] != '.' && !listed(d.path().filename().string()))
            note("orphan account dir left in place: " + d.path().filename().string());
    for (const auto& f : fs::directory_iterator(paths.configDir + "/accounts", ec))
        if (f.path().extension() == ".json" && !listed(f.path().stem().string()))
            note("orphan credentials left in place: " + f.path().filename().string());
    return lines;
}

bool addAccount(const Paths& paths, int64_t userId, const std::string& name) {
    AccountsState st = readAccounts(paths.accountsFile()).value_or(AccountsState{});
    if (userId <= 0 || st.accounts.size() >= kMaxAccounts) return false;
    int order = 0;
    for (const auto& e : st.accounts) {
        if (e.userId == userId) return false;
        order = std::max(order, e.order + 1);
    }
    std::error_code ec;
    fs::create_directories(accountPaths(paths, userId).root, ec);
    if (ec) {
        log(LogLevel::Error, "accounts: can't make the folder of %lld: %s", static_cast<long long>(userId), ec.message().c_str());
        return false;
    }
    AccountEntry e;
    e.userId = userId;
    e.name = name;
    e.order = order;
    st.accounts.push_back(std::move(e));
    if (st.active == 0) st.active = userId;
    return writeAccounts(paths.accountsFile(), st);
}

bool removeAccount(const Paths& paths, int64_t userId) {
    auto st = readAccounts(paths.accountsFile());
    if (!st || userId <= 0) return false;
    const std::string root = accountPaths(paths, userId).root;
    const std::string aside = paths.accountsRoot + "/.removing-" + std::to_string(userId);
    std::error_code ec;
    fs::remove_all(aside, ec);  // an earlier removal cut short
    if (fs::exists(root, ec)) {
        fs::rename(root, aside, ec);
        if (ec) {
            log(LogLevel::Error, "accounts: can't put %s aside: %s", root.c_str(), ec.message().c_str());
            return false;
        }
    }
    st->accounts.erase(std::remove_if(st->accounts.begin(), st->accounts.end(), [&](const AccountEntry& e) { return e.userId == userId; }),
                       st->accounts.end());
    if (st->active == userId) {
        std::vector<AccountEntry> byOrder = st->accounts;
        std::stable_sort(byOrder.begin(), byOrder.end(), [](const AccountEntry& a, const AccountEntry& b) { return a.order < b.order; });
        st->active = byOrder.empty() ? 0 : byOrder.front().userId;
    }
    if (!writeAccounts(paths.accountsFile(), *st)) {
        fs::rename(aside, root, ec);  // still listed: its library back where the list says
        return false;
    }
    if (!fs::remove(paths.credentialsFile(userId), ec) && ec)
        log(LogLevel::Warn, "accounts: credentials of %lld not deleted: %s", static_cast<long long>(userId), ec.message().c_str());
    ec.clear();
    fs::remove_all(aside, ec);
    if (ec) log(LogLevel::Warn, "accounts: %s not deleted entirely: %s", aside.c_str(), ec.message().c_str());
    log(LogLevel::Info, "accounts: %lld removed from this Kindle", static_cast<long long>(userId));
    return true;
}

bool keepAccountSummary(const std::string& accountsFile, int64_t userId, int64_t items, int unsent, int64_t syncedWallMs,
                        const std::string& name) {
    auto st = readAccounts(accountsFile);
    if (!st) return false;
    for (auto& e : st->accounts)
        if (e.userId == userId) {
            const bool named = e.name.empty() && !name.empty();
            if (!named && e.itemCount == items && e.unsentCount == unsent && (syncedWallMs <= 0 || e.syncedWallMs == syncedWallMs))
                return true;
            e.itemCount = items;
            e.unsentCount = unsent;
            if (syncedWallMs > 0) e.syncedWallMs = syncedWallMs;
            if (named) e.name = name;
            return writeAccounts(accountsFile, *st);
        }
    return false;
}

AccountTarget accountTarget(const Paths& p, int64_t userId) {
    AccountTarget t;
    const auto st = readAccounts(p.accountsFile());
    if (!st) {  // before the account layout
        if (userId > 0) {
            t.error = "no account list in " + p.configDir + " (--account needs the account layout)";
            return t;
        }
        t.config = loadConfig(p.configDir);
        t.dbPath = p.dbPath;
        if (t.config) t.userId = t.config->userId;
        else t.error = "no config.json in " + p.configDir;
        return t;
    }
    t.userId = userId > 0 ? userId : st->active;
    const bool listed = std::any_of(st->accounts.begin(), st->accounts.end(), [&](const AccountEntry& e) { return e.userId == t.userId; });
    if (t.userId <= 0 || !listed) {
        t.error = t.userId > 0 ? "account " + std::to_string(t.userId) + " isn't on this Kindle" : "no active account";
        return t;
    }
    t.config = loadCredentials(p, t.userId);
    t.dbPath = accountPaths(p, t.userId).dbPath;
    if (!t.config) t.error = "no key for account " + std::to_string(t.userId) + " in " + p.credentialsFile(t.userId);
    return t;
}

}  // namespace zk
