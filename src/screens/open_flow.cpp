#include "screens/open_flow.h"

#include "bridge/grimoire_bridge.h"
#include "store/library.h"
#include "screens/book.h"
#include "screens/file_choice.h"
#include "screens/reader.h"

namespace zk {

std::string OpenFlow::statusFor(const std::string& key) const {
    auto it = pending_.find(key);
    if (it != pending_.end()) return it->second.status;
    for (const auto& [k, p] : pending_)
        if (!key.empty() && p.listKey == key) return p.status;
    return {};
}

void OpenFlow::openNow(const ItemRow& row, Nav& nav, const std::optional<AnnotationRecord>& at,
                       const std::optional<BridgeOpenTarget>& target) {
    nav.library().markOpened(row.key, row.attachmentKey, wallMs_());
    if (readerFactory_)
        if (auto reader = readerFactory_(row, nav)) {
            if (auto* r = dynamic_cast<ReaderScreen*>(reader.get())) {
                if (at) r->jumpToAnnotation(*at);
                if (target) r->jumpToBridgeTarget(static_cast<int>(target->page), target->locator);
            }
            nav.push(std::move(reader));
            return;
        }
    nav.push(std::make_unique<BookScreen>(row));
}

namespace {
void tell(std::vector<OpenFlow::Done>& done, const std::string& error) {
    auto waiting = std::move(done);
    done.clear();
    for (auto& d : waiting)
        if (d) d(error);
}
}  // namespace

void OpenFlow::reset() {
    auto old = std::move(pending_);
    pending_.clear();
    for (auto& [k, p] : old) tell(p.done, "Cancelled");
}

void OpenFlow::open(const ItemRow& row, Nav& nav, std::optional<AnnotationRecord> at, Done done) {
    if (row.files > 1 && !row.exactFile) {
        auto files = nav.library().fileChoices(row.key);
        if (files.size() > 1) {
            const std::string listKey = row.attachmentKey;
            nav.push(std::make_unique<FileChoiceScreen>(
                row, std::move(files), const_cast<Screen*>(nav.top()),
                [this, at, listKey, done](const ItemRow& r, Nav& n) { start(r, n, at, listKey, done); },
                [done] {
                    if (done) done("Cancelled");
                }));
            return;
        }
    }
    start(row, nav, std::move(at), row.attachmentKey, std::move(done));
}

void OpenFlow::openAt(const ItemRow& row, Nav& nav, BridgeOpenTarget target, Done done) {
    start(row, nav, std::nullopt, row.attachmentKey, std::move(done), std::move(target));
}

void OpenFlow::start(const ItemRow& row, Nav& nav, std::optional<AnnotationRecord> at, const std::string& listKey,
                     Done done, std::optional<BridgeOpenTarget> target) {
    if (row.attachmentKey.empty()) {
        bool linked = false;
        for (const auto& a : nav.library().attachmentsOf(row.key))
            linked |= a.linkMode == "linked_file" &&
                      isReadableType(a.contentType);
        const std::string why = linked ? "This file is only on your computer." : "No EPUB, PDF or saved web page for this item.";
        nav.toast(why);
        if (done) done(why);
        return;
    }
    auto it = pending_.find(row.attachmentKey);
    if (it != pending_.end() && it->second.running) {  // already downloading: it joins that one
        if (target) it->second.bridgeTarget = std::move(target);
        if (done) it->second.done.push_back(std::move(done));
        return;
    }
    if (row.fileState == AttachmentRecord::State::Local) {
        pending_.erase(row.attachmentKey);
        openNow(row, nav, at, target);
        if (done) done("");
        nav.sync().openBook(row.key, row.attachmentKey);  // refreshes its highlights
        return;
    }
    if (it != pending_.end()) tell(it->second.done, "Cancelled");  // a retry replaces the failed one
    Pending p{row, nav.top() ? nav.top()->serial() : 0, nav.sync().busy() ? "Waiting for sync to finish…" : "Downloading…", true, std::move(at), std::move(target), listKey, {}};
    if (done) p.done.push_back(std::move(done));
    pending_[row.attachmentKey] = std::move(p);
    nav.sync().openBook(row.key, row.attachmentKey);
}

void OpenFlow::onSyncEvent(const SyncEvent& e, Nav& nav) {
    auto it = pending_.find(e.attachmentKey);
    if (it == pending_.end()) return;
    Pending& p = it->second;
    if (e.kind == SyncEvent::Kind::DownloadProgress) {
        if (e.error) p.status = e.error->message;  // waiting, and why (Zotero asked to)
        else p.status = e.total > 0 ? "Downloading " + std::to_string(100 * e.done / e.total) + "%" : "Downloading…";
    } else if (e.kind == SyncEvent::Kind::DownloadFinished) {
        // The failed one came down since without a tap (a kept collection's): no surprise book.
        // If that failed too, its error replaces the progress the row showed meanwhile.
        if (!p.running && !e.error) {
            pending_.erase(it);
            return;
        }
        if (e.error) p.status = e.error->message + (e.error->details.empty() ? "" : " (" + e.error->details + ")");
        if (!p.running) return;
        p.running = false;
        if (e.error) {
            tell(p.done, e.error->message);
            return;
        }
        const ItemRow row = p.row;
        const auto at = p.at;
        const bool stillThere = p.from && nav.top() && nav.top()->serial() == p.from;
        auto done = std::move(p.done);
        pending_.erase(it);
        if (stillThere) {
            ItemRow fresh = nav.library().rowFor(row.key, row.attachmentKey).value_or(row);
            openNow(fresh, nav, at, p.bridgeTarget);
            tell(done, "");
        } else {
            tell(done, "Cancelled");
        }
    }
}

bool openZoteroHandoff(const bridge::HandoffRequest& handoff, Library& lib, OpenFlow& flow, Nav& nav,
                       OpenFlow::Done done) {
    if (handoff.target != "zotero" || !bridge::validKey(handoff.itemKey, 32)) return false;
    auto row = lib.rowFor(handoff.itemKey);
    if (!row) return false;
    if (!handoff.attachmentKey.empty()) {
        auto chosen = lib.rowFor(handoff.itemKey, handoff.attachmentKey);
        // rowFor(item, unknown attachment) still returns the item's default row: insist on a match.
        if (!chosen || chosen->attachmentKey != handoff.attachmentKey) return false;
        row = chosen;
    } else if (handoff.page > 0) {
        // A page number is a PDF notion: prefer the PDF when the item has several files.
        for (const auto& f : lib.fileChoices(handoff.itemKey)) {
            if (f.attachment.contentType != "application/pdf") continue;
            auto chosen = lib.rowFor(handoff.itemKey, f.attachment.key);
            if (chosen && chosen->attachmentKey == f.attachment.key) row = chosen;
            break;
        }
    }
    row->exactFile = true;  // the handoff has already chosen the file; never ask "Open which file?"
    BridgeOpenTarget target{handoff.itemKey, row->attachmentKey, handoff.locator, handoff.page};
    flow.openAt(*row, nav, std::move(target), std::move(done));
    return true;
}

}  // namespace zk
