#include "app/zk_sync_json.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "bridge/grimoire_bridge.h"
#include "docs/engine_factory.h"

namespace zk {
namespace {

using json = nlohmann::json;

void setErr(std::string* err, const std::string& msg) {
    if (err) *err = msg;
}

int64_t wallSec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

double cleanRound2(double v) {
    const double r = std::round(v * 100.0) / 100.0;
    return r == 0.0 ? 0.0 : r;
}

bool isAttachmentKeyFormat(const std::string& k) {
    if (k.size() != 8) return false;
    return std::all_of(k.begin(), k.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
}

bool isStrictInt(const json& v) {
    return !v.is_boolean() && (v.is_number_integer() || v.is_number_unsigned());
}

bool isStrictNumber(const json& v) {
    return !v.is_boolean() && v.is_number();
}

bool readFiniteDouble(const json& v, double& out) {
    if (!isStrictNumber(v)) return false;
    const double d = v.get<double>();
    if (!std::isfinite(d)) return false;
    out = d;
    return true;
}

bool readIntInRange(const json& v, int minVal, int maxVal, int& out) {
    if (!isStrictInt(v)) return false;
    if (v.is_number_unsigned()) {
        const uint64_t u = v.get<uint64_t>();
        if (minVal > 0 && u < static_cast<uint64_t>(minVal)) return false;
        if (u > static_cast<uint64_t>(maxVal)) return false;
        out = static_cast<int>(u);
        return true;
    }
    const int64_t s = v.get<int64_t>();
    if (s < minVal || s > maxVal) return false;
    out = static_cast<int>(s);
    return true;
}

bool normalizeHexColor(const std::string& raw, std::string& out) {
    if (raw.empty()) {
        out = "#000000";
        return true;
    }
    if (raw.size() != 7 || raw[0] != '#') return false;
    std::string norm = "#";
    norm.reserve(7);
    for (size_t i = 1; i < 7; ++i) {
        const auto c = static_cast<unsigned char>(raw[i]);
        if (!std::isxdigit(c)) return false;
        norm.push_back(static_cast<char>(std::tolower(c)));
    }
    out = std::move(norm);
    return true;
}

bool validUtf8NoNul(const std::string& s, size_t maxBytes) {
    return s.size() <= maxBytes && s.find('\0') == std::string::npos && bridge::validUtf8(s);
}

std::string dumpJson(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

struct CanonicalInkPos {
    int pageIndex = 0;
    double width = 0.0;
    std::vector<std::vector<double>> paths;

    bool operator==(const CanonicalInkPos&) const = default;
};

bool parseCanonicalInkPos(const std::string& raw, CanonicalInkPos& out) {
    const json j = json::parse(raw, nullptr, false);
    if (!j.is_object()) return false;
    if (!j.contains("pageIndex") || !readIntInRange(j["pageIndex"], 0, 1'000'000, out.pageIndex)) return false;
    double w = 0.0;
    if (!j.contains("width") || !readFiniteDouble(j["width"], w)) return false;
    out.width = cleanRound2(w);
    if (!j.contains("paths") || !j["paths"].is_array()) return false;
    out.paths.clear();
    for (const auto& stroke : j["paths"]) {
        if (!stroke.is_array() || stroke.size() < 2 || (stroke.size() % 2) != 0) return false;
        std::vector<double> pts;
        pts.reserve(stroke.size());
        for (const auto& coord : stroke) {
            double c = 0.0;
            if (!readFiniteDouble(coord, c)) return false;
            pts.push_back(cleanRound2(c));
        }
        out.paths.push_back(std::move(pts));
    }
    return true;
}

std::string formatCanonicalInkPos(const CanonicalInkPos& p) {
    json jpaths = json::array();
    for (const auto& stroke : p.paths) {
        json js = json::array();
        for (double c : stroke) js.push_back(c);
        jpaths.push_back(std::move(js));
    }
    return json{{"pageIndex", p.pageIndex}, {"paths", std::move(jpaths)}, {"width", p.width}}.dump();
}

std::string computeInkSortIndex(int pageIndex, const std::vector<std::vector<double>>& paths,
                                const std::vector<PdfPageGeometry>* pdfPages) {
    int topInt = 0;
    if (pdfPages && pageIndex >= 0 && static_cast<size_t>(pageIndex) < pdfPages->size() && !paths.empty()) {
        const auto& m = (*pdfPages)[static_cast<size_t>(pageIndex)].userToPage;
        // q_y = b * u_x + d * u_y + f
        const double b = m[1], d = m[3], f = m[5];
        double minTop = std::numeric_limits<double>::infinity();
        for (const auto& stroke : paths) {
            for (size_t i = 0; i + 1 < stroke.size(); i += 2) {
                const double qy = b * stroke[i] + d * stroke[i + 1] + f;
                if (std::isfinite(qy) && qy < minTop) minTop = qy;
            }
        }
        if (std::isfinite(minTop)) {
            const double clamped = std::max(0.0, std::min(99999.0, std::floor(minTop)));
            topInt = static_cast<int>(clamped);
        }
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%05d|000000|%05d", pageIndex, topInt);
    return buf;
}

bool attachmentIsCachedOnDisk(const AttachmentRecord& a) {
    return a.state == AttachmentRecord::State::Local && !a.localPath.empty() && bridge::pathIsRegularFile(a.localPath);
}

int attachmentRank(const AttachmentRecord& a) {
    if (a.contentType == "application/pdf") return attachmentIsCachedOnDisk(a) ? 0 : 1;
    if (a.contentType == "application/epub+zip") return 2;
    if (a.contentType == "text/html") return 3;
    return 4;
}

}  // namespace

bool exportIndexJson(Library& lib, const std::string& outPath, nlohmann::json& summaryOut, std::string* err) {
    if (outPath.empty()) {
        setErr(err, "export-index output path is empty");
        return false;
    }

    std::vector<std::string> keys;
    lib.forEachItemData([&](const std::string& key, const std::string&) { keys.push_back(key); });
    std::sort(keys.begin(), keys.end());

    std::vector<CollectionRecord> rawCols = lib.collections();
    std::sort(rawCols.begin(), rawCols.end(), [](const CollectionRecord& a, const CollectionRecord& b) {
        return a.key < b.key;
    });
    std::map<std::string, std::string> collNames;
    json jCollections = json::array();
    for (const auto& c : rawCols) {
        if (!bridge::validKey(c.key, 32)) continue;
        const std::string cleanName = bridge::sanitiseText(c.name, 256);
        collNames[c.key] = cleanName;
        jCollections.push_back({
            {"key", c.key},
            {"name", cleanName},
            {"parent_key", c.parentKey},
        });
    }

    json jItems = json::array();
    size_t approxBytes = dumpJson(jCollections).size() + 256;
    for (const auto& key : keys) {
        auto item = lib.item(key);
        if (!item || !bridge::validKey(item->key, 32)) continue;
        if (item->itemType == "annotation") continue;

        std::vector<AttachmentRecord> attachments = lib.attachmentsOf(key);
        if (item->itemType == "attachment") {
            if (auto own = lib.attachment(key)) {
                const bool seen = std::any_of(attachments.begin(), attachments.end(),
                                              [&](const AttachmentRecord& a) { return a.key == own->key; });
                if (!seen) attachments.push_back(*own);
            }
        }
        std::sort(attachments.begin(), attachments.end(), [](const AttachmentRecord& a, const AttachmentRecord& b) {
            const int ra = attachmentRank(a), rb = attachmentRank(b);
            return ra != rb ? ra < rb : a.key < b.key;
        });

        bool hasPdf = false, hasEpub = false, hasSnapshot = false;
        bool pdfCached = false;
        std::string pdfLocalPath;
        std::string attachmentKey;
        for (const auto& a : attachments) {
            const bool isPdf = a.contentType == "application/pdf";
            const bool isEpub = a.contentType == "application/epub+zip";
            const bool isSnap = a.contentType == "text/html";
            hasPdf |= isPdf;
            hasEpub |= isEpub;
            hasSnapshot |= isSnap;
            if (attachmentKey.empty() && (isPdf || isEpub || isSnap)) attachmentKey = a.key;
            if (isPdf && !pdfCached && attachmentIsCachedOnDisk(a)) {
                pdfCached = true;
                pdfLocalPath = a.localPath;
            }
        }

        std::string authors;
        for (size_t i = 0; i < item->creators.size() && i < 8; ++i) {
            const auto& c = item->creators[i];
            const std::string name = !c.last.empty() ? c.last : c.name;
            if (name.empty()) continue;
            if (!authors.empty()) authors += ", ";
            authors += name;
        }
        if (item->creators.size() > 8) authors += " et al.";

        const std::string cleanAuthors = bridge::sanitiseText(authors, 256);
        const std::string cleanYear = bridge::sanitiseText(item->year, 256);
        std::string rawCite;
        if (!cleanAuthors.empty() && !cleanYear.empty()) rawCite = cleanAuthors + " " + cleanYear;
        else if (!cleanAuthors.empty()) rawCite = cleanAuthors;
        else rawCite = cleanYear;

        json cs = json::array();
        json ckeys = json::array();
        for (const auto& ck : item->collections) {
            if (!bridge::validKey(ck, 32)) continue;
            auto it = collNames.find(ck);
            ckeys.push_back(ck);
            if (it != collNames.end() && !it->second.empty()) cs.push_back(it->second);
        }

        json tags = json::array();
        for (const auto& t : item->tags) {
            if (const std::string name = bridge::sanitiseText(t, 256); !name.empty()) tags.push_back(name);
        }

        int64_t lastOpened = 0;
        if (auto rs = lib.readingState(item->key)) lastOpened = rs->openedMs;

        json entry = {
            {"item_key", item->key},
            {"attachment_key", attachmentKey},
            {"citation_key", bridge::sanitiseText(rawCite, 256)},
            {"title", bridge::sanitiseText(item->title, 512)},
            {"authors", cleanAuthors},
            {"year", cleanYear},
            {"item_type", bridge::sanitiseText(item->itemType, 256)},
            {"collections", std::move(cs)},
            {"collection_keys", std::move(ckeys)},
            {"tags", std::move(tags)},
            {"has_pdf", hasPdf},
            {"has_epub", hasEpub},
            {"has_snapshot", hasSnapshot},
            {"pdf_cached", pdfCached},
            {"pdf_local_path", pdfLocalPath},
            {"last_opened", lastOpened},
        };

        const size_t entryBytes = dumpJson(entry).size() + 2;
        if (approxBytes + entryBytes > kZkSyncMaxPayloadBytes - 1024) break;
        approxBytes += entryBytes;
        jItems.push_back(std::move(entry));
    }

    const int64_t exportedAt = wallSec();
    const size_t itemCount = jItems.size();
    json doc = {
        {"schema_version", kZkSyncJsonSchemaVersion},
        {"exported_at", exportedAt},
        {"item_count", itemCount},
        {"collections", std::move(jCollections)},
        {"items", std::move(jItems)},
    };
    const std::string serialized = dumpJson(doc);
    if (serialized.size() > kZkSyncMaxPayloadBytes) {
        setErr(err, "exported index exceeds 10 MiB limit");
        return false;
    }
    if (!bridge::writeFileAtomic(outPath, serialized, err)) return false;

    summaryOut = {
        {"schema_version", kZkSyncJsonSchemaVersion},
        {"out_path", outPath},
        {"item_count", itemCount},
        {"exported_at", exportedAt},
    };
    return true;
}

bool pdfInfoJson(Library& lib, const std::string& attachmentKey, const EngineEnv& env, bool fetch,
                 const PdfFetcher& fetcher, nlohmann::json& out, std::string* err) {
    if (!isAttachmentKeyFormat(attachmentKey)) {
        setErr(err, "invalid attachment key");
        return false;
    }
    auto att = lib.attachment(attachmentKey);
    if (!att) {
        setErr(err, "attachment not found: " + attachmentKey);
        return false;
    }
    if (att->contentType != "application/pdf") {
        setErr(err, "attachment is not a PDF: " + attachmentKey);
        return false;
    }

    bool cached = attachmentIsCachedOnDisk(*att);
    bool fetched = false;
    std::string localPath = cached ? att->localPath : std::string();

    if (!cached && fetch) {
        if (!fetcher) {
            setErr(err, "cannot fetch PDF: no Zotero account configured");
            return false;
        }
        std::string dest;
        if (!fetcher(*att, dest, err)) return false;
        if (dest.empty() || !bridge::pathIsRegularFile(dest)) {
            setErr(err, "fetched PDF file is missing or not a regular file");
            return false;
        }
        lib.setFileState(att->key, AttachmentRecord::State::Local, dest);
        cached = true;
        fetched = true;
        localPath = dest;
    }

    json jPages = json::array();
    int pageCount = 0;
    if (cached) {
        std::vector<PdfPageGeometry> pages;
        UserError uerr;
        if (!inspectPdfPages(localPath, env, pages, &uerr)) {
            setErr(err, uerr.message + (uerr.details.empty() ? "" : ": " + uerr.details));
            return false;
        }
        pageCount = static_cast<int>(pages.size());
        for (const auto& pg : pages) {
            jPages.push_back({
                {"page_index", pg.pageIndex},
                {"width_pt", pg.widthPt},
                {"height_pt", pg.heightPt},
                {"rotation", pg.rotation},
                {"page_label", pg.pageLabel},
                {"crop_box", {pg.cropBox[0], pg.cropBox[1], pg.cropBox[2], pg.cropBox[3]}},
                {"user_to_page", {pg.userToPage[0], pg.userToPage[1], pg.userToPage[2],
                                  pg.userToPage[3], pg.userToPage[4], pg.userToPage[5]}},
            });
        }
    }

    out = {
        {"schema_version", kZkSyncJsonSchemaVersion},
        {"attachment_key", att->key},
        {"parent_item_key", att->parentKey},
        {"filename", att->filename},
        {"cached", cached},
        {"fetched", fetched},
        {"local_path", localPath},
        {"page_count", pageCount},
        {"pages", std::move(jPages)},
    };
    return true;
}

bool inkPullJson(Library& lib, const std::string& attachmentKey, bool synced, nlohmann::json& out, std::string* err) {
    if (!isAttachmentKeyFormat(attachmentKey)) {
        setErr(err, "invalid attachment key");
        return false;
    }
    auto att = lib.attachment(attachmentKey);
    if (!att) {
        setErr(err, "attachment not found: " + attachmentKey);
        return false;
    }

    json jInk = json::array();
    json jHighlights = json::array();
    int noteCount = 0, imageCount = 0, textCount = 0;

    for (const auto& a : lib.annotationsFor(attachmentKey)) {
        const json pos = json::parse(a.position, nullptr, false);
        int pageIndex = 0;
        if (pos.is_object() && pos.contains("pageIndex")) readIntInRange(pos["pageIndex"], 0, 1'000'000, pageIndex);

        if (a.type == "ink") {
            double width = 2.0;
            json paths = json::array();
            if (pos.is_object()) {
                double w = 0.0;
                if (pos.contains("width") && readFiniteDouble(pos["width"], w) && w > 0.0) width = cleanRound2(w);
                if (pos.contains("paths") && pos["paths"].is_array()) {
                    for (const auto& stroke : pos["paths"]) {
                        if (!stroke.is_array() || stroke.size() < 2 || (stroke.size() % 2) != 0) continue;
                        json js = json::array();
                        bool ok = true;
                        for (const auto& c : stroke) {
                            double v = 0.0;
                            if (!readFiniteDouble(c, v)) {
                                ok = false;
                                break;
                            }
                            js.push_back(cleanRound2(v));
                        }
                        if (ok) paths.push_back(std::move(js));
                    }
                }
            }
            jInk.push_back({
                {"key", a.key},
                {"version", a.version},
                {"pageIndex", pageIndex},
                {"paths", std::move(paths)},
                {"width", width},
                {"color", a.color},
                {"comment", a.comment},
                {"tags", a.tags},
                {"sortIndex", a.sortIndex},
                {"pageLabel", a.pageLabel},
                {"dirty", a.dirty},
                {"conflict", a.conflict},
                {"dateModified", a.dateModified},
            });
        } else if (a.type == "highlight" || a.type == "underline") {
            json rects = json::array();
            if (pos.is_object() && pos.contains("rects") && pos["rects"].is_array()) {
                for (const auto& r : pos["rects"]) {
                    if (!r.is_array() || r.size() != 4) continue;
                    double c[4];
                    if (readFiniteDouble(r[0], c[0]) && readFiniteDouble(r[1], c[1]) &&
                        readFiniteDouble(r[2], c[2]) && readFiniteDouble(r[3], c[3])) {
                        rects.push_back({cleanRound2(c[0]), cleanRound2(c[1]), cleanRound2(c[2]), cleanRound2(c[3])});
                    }
                }
            }
            jHighlights.push_back({
                {"key", a.key},
                {"version", a.version},
                {"type", a.type},
                {"pageIndex", pageIndex},
                {"pageLabel", a.pageLabel},
                {"sortIndex", a.sortIndex},
                {"color", a.color},
                {"text", a.text},
                {"comment", a.comment},
                {"tags", a.tags},
                {"rects", std::move(rects)},
            });
        } else if (a.type == "note") {
            ++noteCount;
        } else if (a.type == "image") {
            ++imageCount;
        } else if (a.type == "text") {
            ++textCount;
        }
    }

    out = {
        {"schema_version", kZkSyncJsonSchemaVersion},
        {"attachment_key", att->key},
        {"synced", synced},
        {"ink", std::move(jInk)},
        {"highlights", std::move(jHighlights)},
        {"other_counts", {{"note", noteCount}, {"image", imageCount}, {"text", textCount}}},
    };
    return true;
}

bool inkUpsertJson(Library& lib, const std::string& attachmentKey, const std::string& inputJsonPath,
                   const EngineEnv& env, nlohmann::json& out, std::string* err) {
    if (inputJsonPath.empty()) {
        setErr(err, "ink-upsert input path is empty");
        return false;
    }
    std::string raw;
    if (!bridge::readFileSafe(inputJsonPath, kZkSyncMaxPayloadBytes, &raw, err)) return false;
    const json payload = json::parse(raw, nullptr, false);
    if (payload.is_discarded()) {
        setErr(err, "invalid JSON in ink-upsert input file");
        return false;
    }
    return inkUpsertJsonPayload(lib, attachmentKey, payload, env, out, err);
}

bool inkUpsertJsonPayload(Library& lib, const std::string& attachmentKey, const nlohmann::json& payload,
                          const EngineEnv& env, nlohmann::json& out, std::string* err) {
    if (!isAttachmentKeyFormat(attachmentKey)) {
        setErr(err, "invalid attachment key");
        return false;
    }
    auto att = lib.attachment(attachmentKey);
    if (!att) {
        setErr(err, "attachment not found: " + attachmentKey);
        return false;
    }

    // ---- 1. Strict full-payload validation (before any DB mutation) ----
    if (!payload.is_object()) {
        setErr(err, "ink-upsert payload must be a JSON object");
        return false;
    }
    int schemaVer = 0;
    if (!payload.contains("schema_version") || !readIntInRange(payload["schema_version"], 1, 1, schemaVer) ||
        schemaVer != kZkSyncJsonSchemaVersion) {
        setErr(err, "ink-upsert payload has missing or unsupported schema_version");
        return false;
    }
    if (!payload.contains("annotations") || !payload["annotations"].is_array()) {
        setErr(err, "ink-upsert payload must contain an 'annotations' array");
        return false;
    }
    if (payload.contains("delete_keys") && !payload["delete_keys"].is_null() && !payload["delete_keys"].is_array()) {
        setErr(err, "ink-upsert 'delete_keys' must be an array");
        return false;
    }

    struct ParsedEntry {
        size_t index = 0;
        std::string key;
        CanonicalInkPos pos;
        std::string color;
        std::string comment;
        std::vector<std::string> tags;
        std::string pageLabel;
        std::string sortIndex;
        bool sortIndexProvided = false;
        std::string canonicalPosition;
    };

    std::vector<ParsedEntry> entries;
    entries.reserve(payload["annotations"].size());
    std::set<std::string> seenExplicitKeys;
    bool needPdfGeomForSort = false;

    const auto& jAnns = payload["annotations"];
    for (size_t idx = 0; idx < jAnns.size(); ++idx) {
        const auto& item = jAnns[idx];
        const std::string prefix = "annotations[" + std::to_string(idx) + "]: ";
        if (!item.is_object()) {
            setErr(err, prefix + "must be an object");
            return false;
        }
        ParsedEntry e;
        e.index = idx;

        if (item.contains("key") && !item["key"].is_null()) {
            if (!item["key"].is_string()) {
                setErr(err, prefix + "'key' must be a string");
                return false;
            }
            e.key = item["key"].get<std::string>();
            if (!e.key.empty()) {
                if (!isZoteroKey(e.key)) {
                    setErr(err, prefix + "invalid Zotero key '" + e.key + "'");
                    return false;
                }
                if (!seenExplicitKeys.insert(e.key).second) {
                    setErr(err, prefix + "duplicate key '" + e.key + "' in payload");
                    return false;
                }
            }
        }

        if (!item.contains("pageIndex") || !readIntInRange(item["pageIndex"], 0, 1'000'000, e.pos.pageIndex)) {
            setErr(err, prefix + "'pageIndex' must be a non-negative integer");
            return false;
        }

        if (!item.contains("paths") || !item["paths"].is_array()) {
            setErr(err, prefix + "'paths' must be an array");
            return false;
        }
        for (size_t s = 0; s < item["paths"].size(); ++s) {
            const auto& stroke = item["paths"][s];
            if (!stroke.is_array() || stroke.size() < 2 || (stroke.size() % 2) != 0) {
                setErr(err, prefix + "paths[" + std::to_string(s) + "] must be an even-length array of >= 2 numbers");
                return false;
            }
            std::vector<double> pts;
            pts.reserve(stroke.size());
            for (const auto& coord : stroke) {
                double c = 0.0;
                if (!readFiniteDouble(coord, c)) {
                    setErr(err, prefix + "paths[" + std::to_string(s) + "] contains a non-finite or non-numeric coordinate");
                    return false;
                }
                pts.push_back(cleanRound2(c));
            }
            e.pos.paths.push_back(std::move(pts));
        }

        if (!e.pos.paths.empty()) {
            double w = 0.0;
            if (!item.contains("width") || !readFiniteDouble(item["width"], w) || w <= 0.0 || w > 200.0) {
                setErr(err, prefix + "'width' must be a finite number in (0, 200]");
                return false;
            }
            e.pos.width = cleanRound2(w);
            if (!(e.pos.width > 0.0)) {
                setErr(err, prefix + "'width' rounds to 0");
                return false;
            }
        } else if (item.contains("width") && !item["width"].is_null()) {
            double w = 0.0;
            if (!readFiniteDouble(item["width"], w) || w < 0.0 || w > 200.0) {
                setErr(err, prefix + "'width' must be a finite number in [0, 200]");
                return false;
            }
            e.pos.width = cleanRound2(w);
        }

        std::string rawColor;
        if (item.contains("color") && !item["color"].is_null()) {
            if (!item["color"].is_string()) {
                setErr(err, prefix + "'color' must be a string");
                return false;
            }
            rawColor = item["color"].get<std::string>();
        }
        if (!normalizeHexColor(rawColor, e.color)) {
            setErr(err, prefix + "'color' must be #rrggbb");
            return false;
        }

        if (item.contains("comment") && !item["comment"].is_null()) {
            if (!item["comment"].is_string()) {
                setErr(err, prefix + "'comment' must be a string");
                return false;
            }
            e.comment = item["comment"].get<std::string>();
            if (!validUtf8NoNul(e.comment, 16384)) {
                setErr(err, prefix + "'comment' must be valid UTF-8 without NUL and <= 16384 bytes");
                return false;
            }
        }

        if (item.contains("tags") && !item["tags"].is_null()) {
            if (!item["tags"].is_array()) {
                setErr(err, prefix + "'tags' must be an array of strings");
                return false;
            }
            for (const auto& t : item["tags"]) {
                if (!t.is_string()) {
                    setErr(err, prefix + "'tags' elements must be strings");
                    return false;
                }
                const std::string tag = t.get<std::string>();
                if (!validUtf8NoNul(tag, 512)) {
                    setErr(err, prefix + "tag must be valid UTF-8 without NUL and <= 512 bytes");
                    return false;
                }
                e.tags.push_back(tag);
            }
        }

        if (item.contains("pageLabel") && !item["pageLabel"].is_null()) {
            if (!item["pageLabel"].is_string()) {
                setErr(err, prefix + "'pageLabel' must be a string");
                return false;
            }
            e.pageLabel = item["pageLabel"].get<std::string>();
            if (!validUtf8NoNul(e.pageLabel, 128)) {
                setErr(err, prefix + "'pageLabel' must be valid UTF-8 without NUL and <= 128 bytes");
                return false;
            }
        }
        if (e.pageLabel.empty()) e.pageLabel = std::to_string(e.pos.pageIndex + 1);

        if (item.contains("sortIndex") && !item["sortIndex"].is_null()) {
            if (!item["sortIndex"].is_string()) {
                setErr(err, prefix + "'sortIndex' must be a string");
                return false;
            }
            e.sortIndex = item["sortIndex"].get<std::string>();
            if (!validUtf8NoNul(e.sortIndex, 64)) {
                setErr(err, prefix + "'sortIndex' must be valid UTF-8 without NUL and <= 64 bytes");
                return false;
            }
            e.sortIndexProvided = !e.sortIndex.empty();
        }
        if (!e.sortIndexProvided && !e.pos.paths.empty()) needPdfGeomForSort = true;

        entries.push_back(std::move(e));
    }

    std::vector<std::string> deleteKeys;
    if (payload.contains("delete_keys") && payload["delete_keys"].is_array()) {
        for (size_t i = 0; i < payload["delete_keys"].size(); ++i) {
            const auto& k = payload["delete_keys"][i];
            if (!k.is_string()) {
                setErr(err, "delete_keys[" + std::to_string(i) + "]: must be a string");
                return false;
            }
            const std::string ks = k.get<std::string>();
            if (!isZoteroKey(ks)) {
                setErr(err, "delete_keys[" + std::to_string(i) + "]: invalid Zotero key '" + ks + "'");
                return false;
            }
            if (!seenExplicitKeys.insert(ks).second) {
                setErr(err, "delete_keys[" + std::to_string(i) + "]: duplicate or conflicting key '" + ks + "'");
                return false;
            }
            deleteKeys.push_back(ks);
        }
    }

    // ---- 2. Pre-validate DB constraints before opening write transaction ----
    for (const auto& e : entries) {
        if (!e.key.empty()) {
            auto existing = lib.annotation(e.key);
            if (existing) {
                if (existing->parentKey != attachmentKey) {
                    setErr(err, "annotation '" + e.key + "' belongs to a different attachment");
                    return false;
                }
                if (existing->type != "ink") {
                    setErr(err, "annotation '" + e.key + "' is not an ink annotation");
                    return false;
                }
                if (!lib.annotationEditable(*existing)) {
                    setErr(err, "annotation '" + e.key + "' is not editable");
                    return false;
                }
                if (!e.pos.paths.empty() && existing->deletedHere) {
                    setErr(err, "annotation '" + e.key + "' is tombstoned and cannot be updated");
                    return false;
                }
            } else if (!e.pos.paths.empty()) {
                if (lib.objectVersion(e.key) > 0 || lib.libraryOf(e.key).has_value()) {
                    setErr(err, "key '" + e.key + "' collides with an existing library object");
                    return false;
                }
                if (!lib.libraryEditable(att->lib)) {
                    setErr(err, "target library is not editable");
                    return false;
                }
            }
        } else if (!e.pos.paths.empty()) {
            if (!lib.libraryEditable(att->lib)) {
                setErr(err, "target library is not editable");
                return false;
            }
        }
    }

    for (const auto& dk : deleteKeys) {
        auto existing = lib.annotation(dk);
        if (!existing || existing->deletedHere) continue;
        if (existing->parentKey != attachmentKey) {
            setErr(err, "delete_keys: annotation '" + dk + "' belongs to a different attachment");
            return false;
        }
        if (existing->type != "ink") {
            setErr(err, "delete_keys: annotation '" + dk + "' is not an ink annotation");
            return false;
        }
        if (!lib.annotationEditable(*existing)) {
            setErr(err, "delete_keys: annotation '" + dk + "' is not editable");
            return false;
        }
    }

    // ---- 3. Optional cached PDF geometry for sortIndex computation ----
    std::vector<PdfPageGeometry> pdfPages;
    const std::vector<PdfPageGeometry>* pdfPagesPtr = nullptr;
    if (needPdfGeomForSort && att->contentType == "application/pdf" && attachmentIsCachedOnDisk(*att)) {
        UserError ignored;
        if (inspectPdfPages(att->localPath, env, pdfPages, &ignored)) pdfPagesPtr = &pdfPages;
    }
    for (auto& e : entries) {
        if (!e.pos.paths.empty()) {
            e.canonicalPosition = formatCanonicalInkPos(e.pos);
            if (!e.sortIndexProvided) e.sortIndex = computeInkSortIndex(e.pos.pageIndex, e.pos.paths, pdfPagesPtr);
        }
    }

    // ---- 4. Snapshot existing ink annotations for keyless canonical dedup ----
    struct ExistingInk {
        std::string key;
        int64_t version = 0;
        bool dirty = false;
        CanonicalInkPos pos;
        std::string color;
        std::string comment;
        std::vector<std::string> tags;
    };
    std::vector<ExistingInk> existingInks;
    for (const auto& a : lib.annotationsFor(attachmentKey)) {
        if (a.type != "ink" || a.deletedHere) continue;
        CanonicalInkPos p;
        if (!parseCanonicalInkPos(a.position, p)) continue;
        std::string normColor;
        if (!normalizeHexColor(a.color, normColor)) normColor = a.color;
        existingInks.push_back({a.key, a.version, a.dirty, std::move(p), std::move(normColor), a.comment, a.tags});
    }
    // Never let a keyless entry match a row that is explicitly targeted in the same batch.
    std::set<std::string> matchedKeyless = seenExplicitKeys;

    // ---- 5. Apply all mutations atomically ----
    int created = 0, updated = 0, deleted = 0, unchanged = 0;
    json results = json::array();

    auto tx = lib.begin();
    for (const auto& e : entries) {
        if (!e.key.empty()) {
            auto existing = lib.annotation(e.key);
            if (existing) {
                if (e.pos.paths.empty()) {
                    if (existing->deletedHere) {
                        ++unchanged;
                        results.push_back({{"index", e.index}, {"key", e.key}, {"action", "unchanged"},
                                           {"version", existing->version}, {"dirty", existing->dirty}});
                    } else {
                        if (!lib.deleteAnnotation(e.key)) {
                            setErr(err, "failed to delete annotation '" + e.key + "'");
                            return false;
                        }
                        auto after = lib.annotation(e.key);
                        ++deleted;
                        results.push_back({{"index", e.index}, {"key", e.key}, {"action", "deleted"},
                                           {"version", after ? after->version : int64_t{0}},
                                           {"dirty", after ? after->dirty : false}});
                    }
                } else {
                    CanonicalInkPos oldPos;
                    std::string oldColor;
                    const bool posMatches = parseCanonicalInkPos(existing->position, oldPos) && oldPos == e.pos;
                    const bool colorMatches =
                        (normalizeHexColor(existing->color, oldColor) ? oldColor : existing->color) == e.color;
                    const bool identical = posMatches && colorMatches && existing->comment == e.comment &&
                                           existing->tags == e.tags && existing->pageLabel == e.pageLabel &&
                                           existing->sortIndex == e.sortIndex;
                    if (identical) {
                        ++unchanged;
                        results.push_back({{"index", e.index}, {"key", e.key}, {"action", "unchanged"},
                                           {"version", existing->version}, {"dirty", existing->dirty}});
                    } else {
                        AnnotationRecord edited = *existing;
                        edited.position = e.canonicalPosition;
                        edited.color = e.color;
                        edited.comment = e.comment;
                        edited.tags = e.tags;
                        edited.pageLabel = e.pageLabel;
                        edited.sortIndex = e.sortIndex;
                        if (!lib.editAnnotation(edited)) {
                            setErr(err, "failed to update annotation '" + e.key + "'");
                            return false;
                        }
                        auto after = lib.annotation(e.key);
                        ++updated;
                        results.push_back({{"index", e.index}, {"key", e.key}, {"action", "updated"},
                                           {"version", after ? after->version : existing->version},
                                           {"dirty", after ? after->dirty : true}});
                    }
                }
            } else {
                if (e.pos.paths.empty()) {
                    ++unchanged;
                    results.push_back({{"index", e.index}, {"key", e.key}, {"action", "unchanged"},
                                       {"version", 0}, {"dirty", false}});
                } else {
                    AnnotationRecord a;
                    a.key = e.key;
                    a.parentKey = attachmentKey;
                    a.type = "ink";
                    a.position = e.canonicalPosition;
                    a.color = e.color;
                    a.comment = e.comment;
                    a.tags = e.tags;
                    a.pageLabel = e.pageLabel;
                    a.sortIndex = e.sortIndex;
                    const std::string createdKey = lib.createAnnotationWithKey(a);
                    if (createdKey.empty()) {
                        setErr(err, "failed to create annotation with key '" + e.key + "'");
                        return false;
                    }
                    auto after = lib.annotation(createdKey);
                    ++created;
                    results.push_back({{"index", e.index}, {"key", createdKey}, {"action", "created"},
                                       {"version", after ? after->version : int64_t{0}},
                                       {"dirty", after ? after->dirty : true}});
                }
            }
        } else {
            if (e.pos.paths.empty()) {
                ++unchanged;
                results.push_back({{"index", e.index}, {"key", ""}, {"action", "unchanged"},
                                   {"version", 0}, {"dirty", false}});
            } else {
                const ExistingInk* match = nullptr;
                for (const auto& ex : existingInks) {
                    if (matchedKeyless.count(ex.key)) continue;
                    if (ex.pos == e.pos && ex.color == e.color && ex.comment == e.comment && ex.tags == e.tags) {
                        match = &ex;
                        break;
                    }
                }
                if (match) {
                    matchedKeyless.insert(match->key);
                    ++unchanged;
                    results.push_back({{"index", e.index}, {"key", match->key}, {"action", "unchanged"},
                                       {"version", match->version}, {"dirty", match->dirty}});
                } else {
                    AnnotationRecord a;
                    a.parentKey = attachmentKey;
                    a.type = "ink";
                    a.position = e.canonicalPosition;
                    a.color = e.color;
                    a.comment = e.comment;
                    a.tags = e.tags;
                    a.pageLabel = e.pageLabel;
                    a.sortIndex = e.sortIndex;
                    const std::string createdKey = lib.createAnnotation(a);
                    if (createdKey.empty()) {
                        setErr(err, "failed to create annotation");
                        return false;
                    }
                    auto after = lib.annotation(createdKey);
                    ++created;
                    results.push_back({{"index", e.index}, {"key", createdKey}, {"action", "created"},
                                       {"version", after ? after->version : int64_t{0}},
                                       {"dirty", after ? after->dirty : true}});
                }
            }
        }
    }

    for (const auto& dk : deleteKeys) {
        auto existing = lib.annotation(dk);
        if (!existing || existing->deletedHere) {
            ++unchanged;
            results.push_back({{"key", dk}, {"action", "unchanged"},
                               {"version", existing ? existing->version : int64_t{0}},
                               {"dirty", existing ? existing->dirty : false}});
        } else {
            if (!lib.deleteAnnotation(dk)) {
                setErr(err, "failed to delete annotation '" + dk + "'");
                return false;
            }
            auto after = lib.annotation(dk);
            ++deleted;
            results.push_back({{"key", dk}, {"action", "deleted"},
                               {"version", after ? after->version : int64_t{0}},
                               {"dirty", after ? after->dirty : false}});
        }
    }
    tx.commit();

    out = {
        {"schema_version", kZkSyncJsonSchemaVersion},
        {"attachment_key", att->key},
        {"created", created},
        {"updated", updated},
        {"deleted", deleted},
        {"unchanged", unchanged},
        {"results", std::move(results)},
    };
    return true;
}

}  // namespace zk
