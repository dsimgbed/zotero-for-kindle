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
                        {"backoff", {{"misses", e.backoff.misses}, {"waitUntilMs", e.backoff.waitUntilMs}}}});
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
        const bool creds = loadCredentials(paths, e.userId).has_value();
        const bool dir = fs::is_directory(accountPaths(paths, e.userId).root);
        if (creds && dir) {
            kept.push_back(std::move(e));
            continue;
        }
        note("account " + std::to_string(e.userId) + " dropped from the list: missing " +
             (!creds && !dir ? "credentials and data dir" : !creds ? "credentials" : "data dir"));
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
        if (d.is_directory() && !listed(d.path().filename().string()))
            note("orphan account dir left in place: " + d.path().filename().string());
    for (const auto& f : fs::directory_iterator(paths.configDir + "/accounts", ec))
        if (f.path().extension() == ".json" && !listed(f.path().stem().string()))
            note("orphan credentials left in place: " + f.path().filename().string());
    return lines;
}

}  // namespace zk
