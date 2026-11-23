#pragma once
// What tapping an item does (spec §6.2): open it if the file is here; otherwise download it
// with progress shown in its row, and open it when done if the reader is still on that screen.
// Errors stay in the row ("message (details)"); tapping again retries. An item with more than one
// readable file asks which first ("Open which file?"), unless the row names its file (exactFile);
// the chosen file's progress shows in the item's row all the same.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "screens/screen.h"

namespace zk {

class Library;

namespace bridge {
struct HandoffRequest;
}

// A Grimoire → Zotero handoff's open request. `page` is 1-based (0: no jump); `locator` is an
// optional position hint. Neither ever comes from a config or account file.
struct BridgeOpenTarget {
    std::string itemKey, attachmentKey, locator;
    uint32_t page = 0;
};

class OpenFlow {
public:
    explicit OpenFlow(std::function<int64_t()> wallMs) : wallMs_(std::move(wallMs)) {}

    // `done` (optional) hears the outcome, once: "" when the reader (or the Book screen) shows,
    // else why not ("Cancelled": the user left before the file came, backed out of "Open which
    // file?", or the flow was reset; the failed download's message; "No EPUB, PDF or saved web page…").
    // A failed download stays in the row for a retry; a retry's own `done` is what hears the next
    // outcome. Opening what is already downloading joins that download: both hear its outcome.
    using Done = std::function<void(const std::string& error)>;
    void open(const ItemRow&, Nav&, std::optional<AnnotationRecord> at = std::nullopt, Done done = {});  // at: show this highlight
    // Opens the already-resolved row without asking "Open which file?", carrying a Grimoire page/locator
    // to the reader (also when the file still has to download).
    void openAt(const ItemRow&, Nav&, BridgeOpenTarget target, Done done = {});
    void onSyncEvent(const SyncEvent&, Nav&);  // from App::setEventHook
    // By the file downloading, or by the row's file of an item whose other file is.
    std::string statusFor(const std::string& attachmentKey) const;
    void reset();
    // Opens a downloaded book in the reader; returns nullptr when it can't (then the Book screen).
    using ReaderFactory = std::function<std::unique_ptr<Screen>(const ItemRow&, Nav&)>;
    void setReaderFactory(ReaderFactory f) { readerFactory_ = std::move(f); }  // a new sync service: forget downloads the old one had
    StatusFn statusFn() const {
        return [this](const std::string& k) { return statusFor(k); };
    }

private:
    struct Pending {
        ItemRow row;
        uint64_t from = 0;  // the screen it was tapped on (Screen::serial; 0: none)
        std::string status;
        bool running = false;  // false: it failed (its error shows until a retry)
        std::optional<AnnotationRecord> at;
        std::optional<BridgeOpenTarget> bridgeTarget;  // a page/locator to apply in the reader
        std::string listKey;  // the attachment key of the row it was tapped on
        std::vector<Done> done;  // who waits for the outcome
    };
    void start(const ItemRow&, Nav&, std::optional<AnnotationRecord> at, const std::string& listKey, Done done,
               std::optional<BridgeOpenTarget> target = std::nullopt);
    void openNow(const ItemRow&, Nav&, const std::optional<AnnotationRecord>& at = std::nullopt,
                 const std::optional<BridgeOpenTarget>& target = std::nullopt);

    std::function<int64_t()> wallMs_;
    ReaderFactory readerFactory_;
    std::map<std::string, Pending> pending_;  // by attachment key
};

// Resolves a validated handoff against `lib` and opens it with OpenFlow::openAt. False when the
// item (or the explicitly named attachment) isn't in the library; nothing is opened then.
bool openZoteroHandoff(const bridge::HandoffRequest& handoff, Library& lib, OpenFlow& flow, Nav& nav,
                       OpenFlow::Done done = {});

}  // namespace zk
