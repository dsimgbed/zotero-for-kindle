#include "screens/preview.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

extern "C" {
#include <md5.h>
}

namespace zk {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string md5Hex(const std::string& s) {
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, s.data(), static_cast<unsigned long>(s.size()));
    unsigned char out[16];
    MD5_Final(out, &ctx);
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", out[i]);
    return hex;
}

bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// The first bytes, past a UTF-8 BOM and white space, lower-cased.
std::string headOf(std::string_view bytes) {
    if (startsWith(bytes, "\xEF\xBB\xBF")) bytes.remove_prefix(3);
    while (!bytes.empty() && std::isspace(static_cast<unsigned char>(bytes.front()))) bytes.remove_prefix(1);
    return lower(std::string(bytes.substr(0, 256)));
}

bool looksHtml(std::string_view bytes) {
    const std::string h = headOf(bytes);
    if (startsWith(h, "<?xml")) return h.find("<html") != std::string::npos || h.find("xhtml") != std::string::npos;
    return startsWith(h, "<!doctype html") || startsWith(h, "<html") || startsWith(h, "<head") || startsWith(h, "<body") ||
           startsWith(h, "<!--") || startsWith(h, "<meta") || startsWith(h, "<title");
}

bool looksPdf(std::string_view bytes) { return bytes.substr(0, 1024).find("%PDF-") != std::string_view::npos; }

// An EPUB is a zip whose first entry is "mimetype", stored, holding application/epub+zip.
bool looksEpub(std::string_view bytes) {
    return startsWith(bytes, std::string_view("PK\3\4", 4)) && bytes.size() >= 58 &&
           bytes.substr(30, 28) == "mimetypeapplication/epub+zip";
}

const char* extensionOf(PreviewType t) {
    switch (t) {
        case PreviewType::Pdf: return ".pdf";
        case PreviewType::Epub: return ".epub";
        case PreviewType::Html: return ".html";
        case PreviewType::Unknown: break;
    }
    return ".bin";
}

std::string typeName(PreviewType t) {
    switch (t) {
        case PreviewType::Pdf: return "pdf";
        case PreviewType::Epub: return "epub";
        case PreviewType::Html: return "html";
        case PreviewType::Unknown: break;
    }
    return "";
}

PreviewType typeNamed(const std::string& s) {
    if (s == "pdf") return PreviewType::Pdf;
    if (s == "epub") return PreviewType::Epub;
    if (s == "html") return PreviewType::Html;
    return PreviewType::Unknown;
}

std::string escaped(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

// <script>…</script> (and an unclosed one to the end) taken out, any case.
std::string withoutScripts(const std::string& html) {
    const std::string l = lower(html);
    std::string out;
    size_t at = 0;
    for (;;) {
        const size_t open = l.find("<script", at);
        if (open == std::string::npos) break;
        out.append(html, at, open - at);
        const size_t close = l.find("</script", open);
        if (close == std::string::npos) return out;
        const size_t end = l.find('>', close);
        at = end == std::string::npos ? l.size() : end + 1;
    }
    out.append(html, at, std::string::npos);
    return out;
}

}  // namespace

PreviewType sniffPreviewType(const std::string& contentType, std::string_view bytes) {
    std::string ct = lower(contentType.substr(0, contentType.find(';')));
    ct.erase(std::remove_if(ct.begin(), ct.end(), [](unsigned char c) { return std::isspace(c); }), ct.end());
    if (ct == "application/pdf" || ct == "application/x-pdf") return looksHtml(bytes) ? PreviewType::Html : PreviewType::Pdf;
    if (ct == "application/epub+zip") return looksHtml(bytes) ? PreviewType::Html : PreviewType::Epub;
    if (ct == "text/html" || ct == "application/xhtml+xml") return PreviewType::Html;
    if (looksPdf(bytes)) return PreviewType::Pdf;
    if (looksEpub(bytes)) return PreviewType::Epub;
    if (looksHtml(bytes)) return PreviewType::Html;
    return PreviewType::Unknown;
}

std::string previewHtmlPage(const std::string& html, const std::string& title, const std::string& baseUrl) {
    const std::string body = withoutScripts(html);
    std::string head = "<meta charset=\"utf-8\">";
    if (!baseUrl.empty()) head += "<base href=\"" + escaped(baseUrl) + "\">";
    const std::string l = lower(body.substr(0, 4096));
    if (l.find("<html") != std::string::npos) {  // a whole document: our head goes first in its own
        const size_t h = l.find("<head");
        if (h != std::string::npos) {
            const size_t end = body.find('>', h);
            if (end != std::string::npos) return body.substr(0, end + 1) + head + body.substr(end + 1);
        }
        const size_t at = body.find('>', l.find("<html"));
        if (at != std::string::npos) return body.substr(0, at + 1) + "<head>" + head + "</head>" + body.substr(at + 1);
    }
    return "<!DOCTYPE html>\n<html><head>" + head + "<title>" + escaped(title) + "</title></head><body>" + body + "</body></html>\n";
}

// ---- The cache ----------------------------------------------------------------------------------

PreviewCache::PreviewCache(std::string dir, std::function<int64_t()> clock) : dir_(std::move(dir)), clock_(std::move(clock)) { load(); }

std::string PreviewCache::keyForUrl(const std::string& url) { return "u" + md5Hex(url); }
std::string PreviewCache::keyForHtml(const std::string& html) { return "h" + md5Hex(html); }

std::string PreviewCache::partPath(const std::string& key) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    return (fs::path(dir_) / (key + ".part")).string();
}

void PreviewCache::load() {
    entries_.clear();
    std::ifstream in(fs::path(dir_) / "index.json");
    if (!in) return;
    const json j = json::parse(in, nullptr, false);
    if (!j.is_array()) return;
    std::error_code ec;
    for (const auto& e : j) {
        if (!e.is_object()) continue;
        PreviewEntry p;
        p.key = e.value("key", "");
        p.file = e.value("file", "");
        p.title = e.value("title", "");
        p.type = typeNamed(e.value("type", ""));
        p.bytes = e.value("bytes", uint64_t{0});
        p.usedMs = e.value("used", int64_t{0});
        p.position = e.value("position", "");
        // Only files of our own folder, still there.
        if (p.key.empty() || p.type == PreviewType::Unknown || fs::path(p.file).parent_path() != fs::path(dir_) ||
            !fs::exists(p.file, ec))
            continue;
        entries_.push_back(std::move(p));
    }
}

void PreviewCache::save() const {
    json j = json::array();
    for (const auto& e : entries_)
        j.push_back({{"key", e.key}, {"file", e.file}, {"title", e.title}, {"type", typeName(e.type)}, {"bytes", e.bytes},
                     {"used", e.usedMs}, {"position", e.position}});
    std::error_code ec;
    fs::create_directories(dir_, ec);
    const fs::path tmp = fs::path(dir_) / "index.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump();
        if (!out) return;
    }
    fs::rename(tmp, fs::path(dir_) / "index.json", ec);
}

std::optional<PreviewEntry> PreviewCache::find(const std::string& key) {
    std::error_code ec;
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->key != key) continue;
        if (!fs::exists(it->file, ec)) {  // gone from under us: not cached
            entries_.erase(it);
            save();
            return std::nullopt;
        }
        it->usedMs = std::max(clock_(), it->usedMs);
        save();
        return *it;
    }
    return std::nullopt;
}

std::optional<PreviewEntry> PreviewCache::add(const std::string& key, const std::string& file, PreviewType type,
                                              const std::string& title, std::string* err) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    for (auto it = entries_.begin(); it != entries_.end();)  // an older copy goes
        if (it->key == key) {
            fs::remove(it->file, ec);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    PreviewEntry e;
    e.key = key;
    e.file = (fs::path(dir_) / (key + extensionOf(type))).string();
    e.title = title;
    e.type = type;
    e.usedMs = clock_();
    fs::rename(file, e.file, ec);
    if (ec) {  // another file system: copy, then remove
        ec.clear();
        fs::copy_file(file, e.file, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::remove(file, ec), ec.clear();
    }
    if (ec) {
        if (err) *err = "Couldn't keep the document: " + ec.message();
        return std::nullopt;
    }
    e.bytes = fs::file_size(e.file, ec);
    entries_.push_back(e);
    evict(key);
    save();
    return e;
}

void PreviewCache::evict(const std::string& keep) {
    std::stable_sort(entries_.begin(), entries_.end(), [&](const PreviewEntry& a, const PreviewEntry& b) {
        if ((a.key == keep) != (b.key == keep)) return a.key == keep;
        return a.usedMs > b.usedMs;
    });
    std::error_code ec;
    uint64_t total = 0;
    size_t n = 0;
    std::vector<PreviewEntry> kept;
    for (auto& e : entries_) {  // the one just added, then newest first
        const bool fits = n < kMaxEntries && total + e.bytes <= kMaxBytes;
        if (e.key == keep || fits) {
            total += e.bytes;
            ++n;
            kept.push_back(std::move(e));
        } else {
            fs::remove(e.file, ec);
        }
    }
    entries_ = std::move(kept);
    // Files no entry names (a download cut short, an index lost) go too.
    std::set<std::string> named;
    for (const auto& e : entries_) named.insert(fs::path(e.file).filename().string());
    for (const auto& f : fs::directory_iterator(dir_, ec)) {
        const std::string name = f.path().filename().string();
        if (name == "index.json" || name.ends_with(".part") || named.count(name)) continue;
        fs::remove(f.path(), ec);
    }
}

void PreviewCache::savePosition(const std::string& key, const std::string& position) {
    for (auto& e : entries_)
        if (e.key == key && e.position != position) {
            e.position = position;
            save();
        }
}

}  // namespace zk
