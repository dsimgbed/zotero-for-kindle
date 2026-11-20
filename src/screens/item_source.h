#pragma once
// The library behind an `items` block (screens/plugin_blocks.h): refs become rows, the way the
// app's own lists read them.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "screens/plugin_blocks.h"
#include "screens/screen.h"

namespace zk {

class LibraryItemSource : public ItemSource {
public:
    LibraryItemSource(Library& lib, StatusFn status, std::function<int64_t()> nowMs)
        : lib_(lib), status_(std::move(status)), nowMs_(std::move(nowMs)) {}

    // The item's row (rowFor: not in the trash, its library shown); with `library` set, only if it is in that one.
    std::optional<ItemRow> row(const ItemRef&) override;
    std::string cardMeta(const ItemRow&) override;
    std::string status(const std::string& attachmentKey) override { return status_ ? status_(attachmentKey) : ""; }

private:
    Library& lib_;
    StatusFn status_;
    std::function<int64_t()> nowMs_;
};

// What zk.library.open opens: the item's row (the same rule as an items block: the key, in that
// library if one is given); with `attachment`, that file's row (a file the item hasn't: not found);
// with `annotation`, the row of the file it is in and the annotation itself, for OpenFlow's `at`. An
// annotation that is unknown, deleted, another item's or not in `attachment` is "Annotation not
// found" (the item is checked first: "Item not found"). nullopt and *why when it can't be opened.
struct OpenTarget {
    ItemRow row;
    std::optional<AnnotationRecord> at;
};
std::optional<OpenTarget> itemToOpen(Library&, const ItemRef&, const std::optional<std::string>& attachment,
                                     const std::optional<std::string>& annotation, std::string* why);

}  // namespace zk
