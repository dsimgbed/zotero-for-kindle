// Grimoire × Zotero bridge: the frozen schema_version 1 IPC of docs/bridge-contract.md.
// Only bibliographic metadata is read from the library; no account/config file is ever opened.

#include "bridge/grimoire_bridge.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "store/library.h"

namespace zk::bridge {
namespace {

using json = nlohmann::json;

constexpr const char* kHandoffName = "handoff.json";
constexpr const char* kReturnName = "return_context.json";
constexpr const char* kIndexName = "library_index.json";
constexpr const char* kInboxName = "inbox";

std::atomic<uint64_t> gNonce{0};

std::string dirOf(const std::string& dir) { return dir.empty() ? bridgeDir() : dir; }

std::string joined(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

int64_t wallSec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

int64_t wallMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string freshNonce() {
    const uint64_t n = ++gNonce;
    const auto ticks = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    char buf[96];
    std::snprintf(buf, sizeof buf, "%x-%llx-%llx", static_cast<unsigned>(::getpid()),
                  static_cast<unsigned long long>(n), static_cast<unsigned long long>(ticks));
    return buf;
}

std::string freshId() { return std::to_string(wallMs()) + "-" + freshNonce(); }

void setWhy(std::string* why, const char* text) {
    if (why) *why = text;
}

// ---- Directory safety: no symlink in any component, create missing ones ----------------------

bool checkComponents(const std::string& path, bool create, std::string* why) {
    std::string cur;
    size_t at = 0;
    if (!path.empty() && path[0] == '/') cur = "/";
    while (at <= path.size()) {
        const size_t next = path.find('/', at);
        const std::string part = path.substr(at, next == std::string::npos ? std::string::npos : next - at);
        at = next == std::string::npos ? path.size() + 1 : next + 1;
        if (part.empty() || part == "." || part == "..") continue;
        cur = cur.empty() || cur == "/" ? cur + part : cur + "/" + part;
        struct stat st{};
        if (::lstat(cur.c_str(), &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                setWhy(why, "a bridge path component is a symlink");
                return false;
            }
            if (create && !S_ISDIR(st.st_mode)) {
                setWhy(why, "a bridge path component is not a directory");
                return false;
            }
        } else if (errno == ENOENT) {
            if (!create) break;  // nothing to check further: the parent will be created
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                setWhy(why, "can't create the bridge directory");
                return false;
            }
            if (::lstat(cur.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
                setWhy(why, "can't create a safe bridge directory");
                return false;
            }
        } else {
            setWhy(why, "can't inspect the bridge directory");
            return false;
        }
    }
    return true;
}

bool ensureDirectory(const std::string& path, std::string* why) {
    if (path.empty()) return true;
    return checkComponents(path, true, why);
}

// ---- Strict UTF-8 and text ------------------------------------------------------------------

bool validUtf8Impl(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            ++i;
            continue;
        }
        size_t n = 0;
        uint32_t cp = 0;
        if (b >= 0xC2 && b <= 0xDF) {
            n = 2;
            cp = b & 0x1Fu;
        } else if (b >= 0xE0 && b <= 0xEF) {
            n = 3;
            cp = b & 0x0Fu;
        } else if (b >= 0xF0 && b <= 0xF4) {
            n = 4;
            cp = b & 0x07u;
        } else {
            return false;
        }
        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            const auto c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        if (cp < 0x80 || (n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += n;
    }
    return true;
}

std::string sanitiseImpl(const std::string& s, size_t maxBytes) {
    std::string out;
    bool gap = false;
    size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        size_t n = 1;
        uint32_t cp = b;
        bool valid = b < 0x80;
        if (b >= 0xC2 && b <= 0xDF) n = 2;
        else if (b >= 0xE0 && b <= 0xEF) n = 3;
        else if (b >= 0xF0 && b <= 0xF4) n = 4;
        if (n > 1) {
            valid = i + n <= s.size();
            cp = b & (n == 2 ? 0x1Fu : n == 3 ? 0x0Fu : 0x07u);
            for (size_t k = 1; valid && k < n; ++k) {
                const auto c = static_cast<unsigned char>(s[i + k]);
                if ((c & 0xC0) != 0x80) valid = false;
                else cp = (cp << 6) | (c & 0x3Fu);
            }
            if (valid && (cp < (n == 2 ? 0x80u : n == 3 ? 0x800u : 0x10000u) || cp > 0x10FFFF ||
                          (cp >= 0xD800 && cp <= 0xDFFF)))
                valid = false;
            if (!valid) n = 1;
        }
        const std::string bytes = valid ? s.substr(i, n) : std::string("\xEF\xBF\xBD", 3);
        if (!valid) cp = 0xFFFD;  // a malformed byte is one U+FFFD
        i += valid ? n : 1;
        // Control characters (including NUL, CR and LF) become one space; invisible format
        // characters are dropped. The result is one line and safe to put in a JSON string.
        if (cp == 0 || cp < 0x20 || cp == 0x7F) {
            gap = true;
            continue;
        }
        if (cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || cp == 0xFEFF) continue;
        std::string add = gap && !out.empty() ? " " + bytes : bytes;
        if (out.size() + add.size() > maxBytes) break;
        out += add;
        gap = false;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool isAlnumDashUnderscore(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// ---- JSON field helpers ---------------------------------------------------------------------

bool schemaOk(const json& j, std::string* why) {
    if (!j.is_object() || !j.contains("schema_version")) {
        setWhy(why, "a bridge document has the wrong schema_version");
        return false;
    }
    const json& v = j["schema_version"];
    const bool ok = v.is_number_unsigned() ? v.get<uint64_t>() == static_cast<uint64_t>(kSchemaVersion)
                    : v.is_number_integer() ? v.get<int64_t>() == static_cast<int64_t>(kSchemaVersion)
                                            : false;
    if (!ok) {
        setWhy(why, "a bridge document has the wrong schema_version");
        return false;
    }
    return true;
}

bool textField(const json& j, const char* key, std::string& out, size_t max, bool required, std::string* why) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        if (required) {
            setWhy(why, "a required bridge field is missing");
            return false;
        }
        out.clear();
        return true;
    }
    if (!it->is_string()) {
        setWhy(why, "a bridge field isn't text");
        return false;
    }
    out = it->get<std::string>();
    if (out.find('\0') != std::string::npos || out.size() > max || !validUtf8(out)) {
        setWhy(why, "a bridge text field is too long or not valid UTF-8");
        return false;
    }
    return true;
}

bool uintField(const json& j, const char* key, uint64_t max, uint64_t& out, bool required, std::string* why) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        if (required) {
            setWhy(why, "a required bridge field is missing");
            return false;
        }
        out = 0;
        return true;
    }
    if (!it->is_number_integer()) {
        setWhy(why, "a bridge number field isn't an integer");
        return false;
    }
    if (it->is_number_unsigned()) {
        out = it->get<uint64_t>();
    } else {
        const int64_t v = it->get<int64_t>();
        if (v < 0) {
            setWhy(why, "a bridge number field is negative");
            return false;
        }
        out = static_cast<uint64_t>(v);
    }
    if (out > max) {
        setWhy(why, "a bridge number field is out of range");
        return false;
    }
    return true;
}

bool intField(const json& j, const char* key, int64_t& out, std::string* why) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        out = 0;
        return true;
    }
    if (!it->is_number_integer() || (it->is_number_unsigned() && it->get<uint64_t>() >
                                                                  static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))) {
        setWhy(why, "a bridge timestamp isn't an integer");
        return false;
    }
    out = it->get<int64_t>();
    return true;
}

bool keyField(const json& j, const char* key, std::string& out, size_t max, bool required, std::string* why) {
    if (!textField(j, key, out, max, required, why)) return false;
    if (out.empty()) return true;  // an absent optional key
    if (!validKey(out, max)) {
        setWhy(why, "a bridge key has invalid characters");
        return false;
    }
    return true;
}

bool locatorField(const json& j, std::string& out, std::string* why) {
    if (!textField(j, "locator", out, 512, false, why)) return false;
    if (!validLocator(out)) {
        setWhy(why, "a bridge locator has control characters");
        return false;
    }
    return true;
}

bool parseReturnObject(const json& j, ReturnContext& ctx, std::string* why) {
    if (!j.is_object()) {
        setWhy(why, "a bridge return block isn't an object");
        return false;
    }
    if (!textField(j, "notebook_id", ctx.notebookId, 63, false, why)) return false;
    if (!ctx.notebookId.empty() && !validNotebookId(ctx.notebookId)) {
        setWhy(why, "a bridge notebook id has invalid characters");
        return false;
    }
    uint64_t id = 0, index = 0;
    if (!uintField(j, "page_id", std::numeric_limits<uint32_t>::max(), id, false, why) ||
        !uintField(j, "page_index", std::numeric_limits<uint32_t>::max(), index, false, why)) return false;
    ctx.pageId = static_cast<uint32_t>(id);
    ctx.pageIndex = static_cast<uint32_t>(index);
    return true;
}

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

bool writeJson(const std::string& dir, const char* name, const json& j, std::string* why) {
    return writeFileAtomic(joined(dirOf(dir), name), dump(j), why);
}

}  // namespace

bool validUtf8(const std::string& s) { return validUtf8Impl(s); }
std::string sanitiseText(const std::string& s, size_t maxBytes) { return sanitiseImpl(s, maxBytes); }

std::string bridgeDir() {
    const char* v = std::getenv("GZ_BRIDGE_DIR");
    return v && *v ? std::string(v) : std::string(kDefaultDir);
}

bool bridgeActive() {
    const char* v = std::getenv("GZ_BRIDGE_DIR");
    if (v && *v) return true;
    struct stat st{};
    return ::lstat(kDefaultDir, &st) == 0 && !S_ISLNK(st.st_mode);
}

bool validKey(const std::string& s, size_t max) {
    if (s.empty() || s.size() > max) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return isAlnumDashUnderscore(c); });
}

bool validNotebookId(const std::string& s) { return validKey(s, 63); }

bool validNonceOrId(const std::string& s) { return validKey(s, 64); }

bool validPage(uint32_t page) { return page == 0 || (page >= 1 && page <= 1'000'000); }

bool validBoundedText(const std::string& s, size_t maxBytes) {
    return s.size() <= maxBytes && s.find('\0') == std::string::npos && validUtf8(s);
}

bool validLocator(const std::string& s) {
    if (!validBoundedText(s, 512)) return false;
    return std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20; });
}

bool pathIsRegularFile(const std::string& path) {
    struct stat st{};
    return ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode);
}

bool writeFileAtomic(const std::string& path, const std::string& content, std::string* why) {
    const size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash);
    if (!ensureDirectory(dir, why)) return false;
    const std::string final = slash == std::string::npos ? path : path.substr(slash + 1);
    if (final.empty()) {
        setWhy(why, "no file name for the bridge document");
        return false;
    }
    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0 && (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode))) {
        setWhy(why, "the bridge document is not a plain file");
        return false;
    }
    const std::string tmp = path + ".tmp." + std::to_string(::getpid()) + "." + freshNonce();
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        setWhy(why, "can't create a temporary bridge document");
        return false;
    }
    size_t written = 0;
    bool ok = true;
    while (written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            ok = false;
            break;
        }
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        ::unlink(tmp.c_str());
        setWhy(why, "can't write the bridge document");
        return false;
    }
    if (::lstat(path.c_str(), &st) == 0 && (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode))) {
        ::unlink(tmp.c_str());
        setWhy(why, "the bridge document became a non-file");
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        setWhy(why, "can't replace the bridge document");
        return false;
    }
    // Best effort: FAT/FUSE on the Kindle can refuse a directory fsync.
    const int dfd = dir.empty() ? -1 : ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

bool readFileSafe(const std::string& path, size_t maxBytes, std::string* out, std::string* why) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        setWhy(why, "missing");
        return false;
    }
    if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) {
        setWhy(why, "a bridge document is not a plain file");
        return false;
    }
    if (static_cast<uint64_t>(st.st_size) > maxBytes) {
        setWhy(why, "a bridge document is too large");
        return false;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        setWhy(why, "can't open a bridge document");
        return false;
    }
    struct stat opened{};
    if (::fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode) || static_cast<uint64_t>(opened.st_size) > maxBytes) {
        ::close(fd);
        setWhy(why, "a bridge document is not a plain file within the size cap");
        return false;
    }
    std::string text;
    text.reserve(static_cast<size_t>(opened.st_size));
    char buf[4096];
    bool ok = true;
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            ok = false;
            break;
        }
        if (text.size() + static_cast<size_t>(n) > maxBytes) {
            ok = false;
            break;
        }
        text.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    if (!ok || !validUtf8(text)) {
        setWhy(why, "a bridge document is unreadable or not valid UTF-8");
        return false;
    }
    if (out) *out = std::move(text);
    return true;
}

std::optional<HandoffRequest> parseHandoff(const std::string& text, std::string* why) {
    const json j = json::parse(text, nullptr, false);
    if (!schemaOk(j, why)) return std::nullopt;
    HandoffRequest h;
    if (!textField(j, "nonce", h.nonce, 64, true, why) || !validNonceOrId(h.nonce) ||
        !textField(j, "source", h.source, 32, false, why) ||
        !textField(j, "target", h.target, 16, true, why) ||
        !textField(j, "action", h.action, 32, true, why) ||
        !intField(j, "created_at", h.createdAt, why))
        return std::nullopt;
    if (h.target != "zotero" && h.target != "grimoire") {
        setWhy(why, "a bridge handoff has an unknown target");
        return std::nullopt;
    }
    if (h.action != (h.target == "zotero" ? "open_item" : "return_essay")) {
        setWhy(why, "a bridge handoff's action doesn't match its target");
        return std::nullopt;
    }
    if (j.contains("zotero")) {
        const json& z = j["zotero"];
        if (!z.is_object()) {
            setWhy(why, "a bridge handoff's zotero block isn't an object");
            return std::nullopt;
        }
        if (!keyField(z, "item_key", h.itemKey, 32, true, why) ||
            !keyField(z, "attachment_key", h.attachmentKey, 32, false, why) ||
            !locatorField(z, h.locator, why)) return std::nullopt;
        uint64_t page = 0;
        if (!uintField(z, "page", 1'000'000, page, false, why)) return std::nullopt;
        h.page = static_cast<uint32_t>(page);
    }
    if (h.target == "zotero" && !validKey(h.itemKey, 32)) {
        setWhy(why, "a zotero handoff has no valid item key");
        return std::nullopt;
    }
    if (j.contains("grimoire")) {
        const json& g = j["grimoire"];
        if (!g.is_object()) {
            setWhy(why, "a bridge handoff's grimoire block isn't an object");
            return std::nullopt;
        }
        if (!textField(g, "notebook_id", h.notebookId, 63, false, why) ||
            (!h.notebookId.empty() && !validNotebookId(h.notebookId))) {
            setWhy(why, "a bridge handoff has an invalid notebook id");
            return std::nullopt;
        }
        uint64_t id = 0, index = 0;
        if (!uintField(g, "page_id", std::numeric_limits<uint32_t>::max(), id, false, why) ||
            !uintField(g, "page_index", std::numeric_limits<uint32_t>::max(), index, false, why)) return std::nullopt;
        h.pageId = static_cast<uint32_t>(id);
        h.pageIndex = static_cast<uint32_t>(index);
    }
    // A return_to block is persisted only if it is valid and non-empty; a bad or empty one must not poison the handoff.
    if (j.contains("return_to") && j["return_to"].is_object()) {
        std::string ignored;
        ReturnContext ctx;
        if (parseReturnObject(j["return_to"], ctx, &ignored) && !ctx.notebookId.empty()) {
            h.returnTo = ctx;
            h.hasReturnTo = true;
        }
    }
    return h;
}

std::optional<ReturnContext> parseReturnContext(const std::string& text, std::string* why) {
    const json j = json::parse(text, nullptr, false);
    if (!schemaOk(j, why)) return std::nullopt;
    ReturnContext ctx;
    if (!parseReturnObject(j, ctx, why)) return std::nullopt;
    if (ctx.notebookId.empty()) {
        setWhy(why, "a bridge return context has an empty notebook id");
        return std::nullopt;
    }
    int64_t updated = 0;
    if (!intField(j, "updated_at", updated, why)) return std::nullopt;
    return ctx;
}

std::optional<CitationPayload> parseCitation(const std::string& text, std::string* why) {
    const json j = json::parse(text, nullptr, false);
    if (!schemaOk(j, why)) return std::nullopt;
    CitationPayload c;
    if (!textField(j, "id", c.id, 64, true, why) || !validNonceOrId(c.id) ||
        !keyField(j, "item_key", c.itemKey, 32, true, why) ||
        !keyField(j, "attachment_key", c.attachmentKey, 32, false, why) ||
        !textField(j, "short_cite", c.shortCite, 256, false, why) ||
        !textField(j, "title", c.title, 512, false, why) ||
        !textField(j, "authors", c.authors, 256, false, why) ||
        !textField(j, "year", c.year, 256, false, why) ||
        !textField(j, "quote", c.quote, 4096, false, why) ||
        !textField(j, "note", c.note, 4096, false, why) ||
        !locatorField(j, c.locator, why) || !intField(j, "created_at", c.createdAt, why))
        return std::nullopt;
    uint64_t page = 0;
    if (!uintField(j, "page", 1'000'000, page, false, why)) return std::nullopt;
    c.page = static_cast<uint32_t>(page);
    return c;
}

bool writeHandoff(const HandoffRequest& h, const std::string& dir, std::string* why) {
    if (h.target != "zotero" && h.target != "grimoire") {
        setWhy(why, "a bridge handoff has an unknown target");
        return false;
    }
    const std::string source = h.source.empty() ? "zotero" : h.source;
    if (source != "grimoire" && source != "zotero") {
        setWhy(why, "a bridge handoff has an unknown source");
        return false;
    }
    const std::string action = h.action.empty() ? (h.target == "zotero" ? "open_item" : "return_essay") : h.action;
    if (action != (h.target == "zotero" ? "open_item" : "return_essay")) {
        setWhy(why, "a bridge handoff's action doesn't match its target");
        return false;
    }
    if (h.target == "zotero" && !validKey(h.itemKey, 32)) {
        setWhy(why, "a zotero handoff has no valid item key");
        return false;
    }
    if (!h.attachmentKey.empty() && !validKey(h.attachmentKey, 32)) {
        setWhy(why, "a bridge handoff has an invalid attachment key");
        return false;
    }
    if (!validPage(h.page) || !validLocator(h.locator) || (!h.notebookId.empty() && !validNotebookId(h.notebookId)))
        return false;
    if (h.hasReturnTo && !h.returnTo.notebookId.empty() && !validNotebookId(h.returnTo.notebookId)) {
        setWhy(why, "a bridge handoff has an invalid return_to notebook id");
        return false;
    }
    const std::string nonce = h.nonce.empty() ? freshId() : h.nonce;
    if (!validNonceOrId(nonce)) {
        setWhy(why, "a bridge handoff has an invalid nonce");
        return false;
    }
    json j;
    j["schema_version"] = kSchemaVersion;
    j["nonce"] = nonce;
    j["created_at"] = h.createdAt ? h.createdAt : wallSec();
    j["source"] = source;
    j["target"] = h.target;
    j["action"] = action;
    if (h.target == "zotero") {
        json z;
        z["item_key"] = h.itemKey;
        z["attachment_key"] = h.attachmentKey;
        z["page"] = h.page;
        z["locator"] = sanitiseText(h.locator, 512);
        j["zotero"] = z;
    } else {
        json g;
        g["notebook_id"] = sanitiseText(h.notebookId, 63);
        g["page_id"] = h.pageId;
        g["page_index"] = h.pageIndex;
        j["grimoire"] = g;
    }
    if (h.hasReturnTo && !h.returnTo.notebookId.empty()) {
        json r;
        r["target"] = "grimoire";
        r["notebook_id"] = sanitiseText(h.returnTo.notebookId, 63);
        r["page_id"] = h.returnTo.pageId;
        r["page_index"] = h.returnTo.pageIndex;
        j["return_to"] = r;
    }
    return writeJson(dir, kHandoffName, j, why);
}

bool writeReturnContext(const ReturnContext& ctx, const std::string& dir, std::string* why) {
    if (ctx.notebookId.empty() || !validNotebookId(ctx.notebookId)) {
        setWhy(why, "a bridge return context has an invalid or empty notebook id");
        return false;
    }
    json j;
    j["schema_version"] = kSchemaVersion;
    j["updated_at"] = wallSec();
    j["notebook_id"] = ctx.notebookId;
    j["page_id"] = ctx.pageId;
    j["page_index"] = ctx.pageIndex;
    return writeJson(dir, kReturnName, j, why);
}

std::optional<ReturnContext> readReturnContext(const std::string& dir, std::string* why) {
    std::string text;
    if (!readFileSafe(joined(dirOf(dir), kReturnName), kMaxJsonBytes, &text, why)) return std::nullopt;
    return parseReturnContext(text, why);
}

bool writeReturnToGrimoireHandoff(const ReturnContext& ctx, const std::string& dir, std::string* why) {
    HandoffRequest h;
    h.source = "zotero";
    h.target = "grimoire";
    h.action = "return_essay";
    h.notebookId = ctx.notebookId;
    h.pageId = ctx.pageId;
    h.pageIndex = ctx.pageIndex;
    if (!ctx.notebookId.empty()) {
        if (!validNotebookId(ctx.notebookId)) {
            setWhy(why, "a bridge return handoff has an invalid notebook id");
            return false;
        }
        h.returnTo = ctx;
        h.hasReturnTo = true;
    }
    return writeHandoff(h, dir, why);
}

std::optional<HandoffRequest> consumeZoteroHandoff(const std::string& dir, std::string* why) {
    const std::string d = dirOf(dir);
    const std::string path = joined(d, kHandoffName);
    std::string text;
    if (!readFileSafe(path, kMaxJsonBytes, &text, why)) {
        if (why && *why == "missing") return std::nullopt;  // a normal start without a handoff
        ::unlink(path.c_str());
        setWhy(why, "discarded an unreadable bridge handoff");
        return std::nullopt;
    }
    auto h = parseHandoff(text, why);
    if (!h) {
        ::unlink(path.c_str());
        setWhy(why, "discarded an invalid bridge handoff");
        return std::nullopt;
    }
    if (h->target != "zotero") return h;  // another app's handoff: leave it for its consumer
    ::unlink(path.c_str());
    if (h->hasReturnTo) writeReturnContext(h->returnTo, d, nullptr);  // best effort
    return h;
}

namespace {

bool filenameId(const std::string& id) {
    const size_t dash = id.find('-');
    return dash > 0 && dash < id.size() - 1 &&
           std::all_of(id.begin(), id.begin() + static_cast<long>(dash),
                       [](unsigned char c) { return std::isdigit(c); }) &&
           validNonceOrId(id.substr(dash + 1));
}

}  // namespace

bool queueCitation(const CitationPayload& c, const std::string& dir, std::string* idOut, std::string* why) {
    CitationPayload out = c;
    if (!filenameId(out.id)) out.id = freshId();
    if (out.createdAt == 0) {
        const size_t dash = out.id.find('-');
        out.createdAt = dash == std::string::npos ? wallSec()
                                                  : std::strtoll(out.id.substr(0, dash).c_str(), nullptr, 10) / 1000;
    }
    if (!validKey(out.itemKey, 32) || (!out.attachmentKey.empty() && !validKey(out.attachmentKey, 32)) ||
        !validNonceOrId(out.id) || !validPage(out.page) || !validLocator(out.locator) ||
        !validBoundedText(out.shortCite, 256) || !validBoundedText(out.title, 512) ||
        !validBoundedText(out.authors, 256) || !validBoundedText(out.year, 256) ||
        !validBoundedText(out.quote, 4096) || !validBoundedText(out.note, 4096)) {
        setWhy(why, "a citation has an invalid field");
        return false;
    }
    json j;
    j["schema_version"] = kSchemaVersion;
    j["id"] = out.id;
    j["created_at"] = out.createdAt;
    j["item_key"] = out.itemKey;
    j["attachment_key"] = out.attachmentKey;
    j["short_cite"] = out.shortCite;
    j["title"] = out.title;
    j["authors"] = out.authors;
    j["year"] = out.year;
    j["page"] = out.page;
    j["locator"] = out.locator;
    j["quote"] = out.quote;
    j["note"] = out.note;
    const std::string d = joined(dirOf(dir), kInboxName);
    if (!writeFileAtomic(joined(d, out.id + ".json"), dump(j), why)) return false;
    if (idOut) *idOut = out.id;
    return true;
}

bool exportLibraryIndex(Library& lib, const std::string& dir, std::string* why, size_t* countOut) {
    std::vector<std::string> keys;
    lib.forEachItemData([&](const std::string& key, const std::string&) { keys.push_back(key); });
    std::sort(keys.begin(), keys.end());
    std::map<std::string, std::string> collections;
    for (const auto& c : lib.collections()) collections[c.key] = c.name;

    json items = json::array();
    for (const auto& key : keys) {
        auto item = lib.item(key);
        if (!item || !validKey(item->key, 32)) continue;
        const auto attachments = lib.attachmentsOf(key);
        bool hasPdf = false, hasEpub = false, hasSnapshot = false;
        std::string attachmentKey;
        for (const auto& a : attachments) {
            hasPdf |= a.contentType == "application/pdf";
            hasEpub |= a.contentType == "application/epub+zip";
            hasSnapshot |= a.contentType == "text/html";
            if (attachmentKey.empty() && (a.contentType == "application/pdf" || a.contentType == "application/epub+zip" ||
                                          a.contentType == "text/html"))
                attachmentKey = a.key;
        }
        if (item->itemType == "attachment") {
            auto own = lib.attachment(key);
            if (own) {
                hasPdf |= own->contentType == "application/pdf";
                hasEpub |= own->contentType == "application/epub+zip";
                hasSnapshot |= own->contentType == "text/html";
                if (attachmentKey.empty() && (hasPdf || hasEpub || hasSnapshot)) attachmentKey = key;
            }
        }
        std::string authors;
        for (size_t i = 0; i < item->creators.size() && i < 8; ++i) {
            const auto& c = item->creators[i];
            const std::string name = !c.last.empty() ? c.last : c.name;
            if (!authors.empty()) authors += ", ";
            authors += name;
        }
        if (item->creators.size() > 8) authors += " et al.";
        std::string citationKey = sanitiseText(authors + " " + item->year, 256);
        json e;
        e["item_key"] = item->key;
        e["attachment_key"] = attachmentKey;
        e["citation_key"] = citationKey;
        e["title"] = sanitiseText(item->title, 512);
        e["authors"] = sanitiseText(authors, 256);
        e["year"] = sanitiseText(item->year, 256);
        e["item_type"] = sanitiseText(item->itemType, 256);
        json cs = json::array();
        for (const auto& k : item->collections) {
            auto it = collections.find(k);
            if (it != collections.end())
                if (const std::string name = sanitiseText(it->second, 256); !name.empty()) cs.push_back(name);
        }
        e["collections"] = cs;
        json tags = json::array();
        for (const auto& t : item->tags)
            if (const std::string name = sanitiseText(t, 256); !name.empty()) tags.push_back(name);
        e["tags"] = tags;
        e["has_pdf"] = hasPdf;
        e["has_epub"] = hasEpub;
        e["has_snapshot"] = hasSnapshot;
        items.push_back(std::move(e));
        // The reader's cap is 10 MiB: never write more than can be consumed.
        if (items.size() % 64 == 0 && dump(items).size() > kMaxIndexBytes / 2) break;
    }
    const size_t count = items.size();
    json j;
    j["schema_version"] = kSchemaVersion;
    j["exported_at"] = wallSec();
    j["item_count"] = count;
    j["items"] = std::move(items);
    if (!writeJson(dir, kIndexName, j, why)) return false;
    if (countOut) *countOut = count;
    return true;
}

bool exportLibraryIndexIfAbsent(Library& lib, const std::string& dir, std::string* why) {
    const std::string path = joined(dirOf(dir), kIndexName);
    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            setWhy(why, "the library index is a symlink");
            return false;
        }
        if (S_ISREG(st.st_mode)) return true;  // an existing index: leave it alone
        setWhy(why, "the library index isn't a plain file");
        return false;
    }
    return exportLibraryIndex(lib, dir, why);
}

}  // namespace zk::bridge
