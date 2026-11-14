#pragma once
// Previews (zk.reader.open, 0.9): documents a plugin opens in the reader without adding them to the
// library. What a download is (sniffPreviewType), the page an `html` document becomes
// (previewHtmlPage), and the cache they are kept in, cache/preview/: the last kMaxEntries
// documents and at most kMaxBytes, the oldest (least recently read) going first. Each keeps its
// reading place while it is cached. Nothing here touches the library or syncs.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zk {

enum class PreviewType { Unknown, Pdf, Epub, Html };

// What a downloaded document is: Content-Type first (a PDF or EPUB as declared, unless the bytes
// are plainly an HTML page — a landing page or a sign-in wall), else its first bytes.
PreviewType sniffPreviewType(const std::string& contentType, std::string_view firstBytes);

// An `html` document as a page the reader opens like a web snapshot: UTF-8, its title, a <base>
// for its relative links, no scripts. (The reader fetches nothing: images other than data: URLs
// don't show.)
std::string previewHtmlPage(const std::string& html, const std::string& title, const std::string& baseUrl);

struct PreviewEntry {
    std::string key, file, title;
    PreviewType type = PreviewType::Unknown;
    uint64_t bytes = 0;
    int64_t usedMs = 0;    // added or last opened
    std::string position;  // the reader's place (DocPosition::value); "" none yet
};

class PreviewCache {
public:
    static constexpr size_t kMaxEntries = 10;
    static constexpr uint64_t kMaxBytes = 100ull << 20;
    static constexpr uint64_t kMaxDownloadBytes = 50ull << 20;  // one document (the download's cap)

    // dir: <cache>/preview (made when needed). clock: wall ms (the order documents go in).
    PreviewCache(std::string dir, std::function<int64_t()> clock);

    static std::string keyForUrl(const std::string& url);
    static std::string keyForHtml(const std::string& html);

    // The cached document (its file still there), marked as just read; nullopt if none.
    std::optional<PreviewEntry> find(const std::string& key);
    // Moves `file` in as `key` (replacing an older one), then lets the oldest go until the limits
    // hold — never the one just added. nullopt (and *err) if it couldn't be stored.
    std::optional<PreviewEntry> add(const std::string& key, const std::string& file, PreviewType, const std::string& title,
                                    std::string* err);
    void savePosition(const std::string& key, const std::string& position);
    // Where a download for `key` is written before add() takes it.
    std::string partPath(const std::string& key);
    std::vector<PreviewEntry> entries() const { return entries_; }
    const std::string& dir() const { return dir_; }

private:
    void load();
    void save() const;
    void evict(const std::string& keep);
    std::string dir_;
    std::function<int64_t()> clock_;
    std::vector<PreviewEntry> entries_;
};

}  // namespace zk
