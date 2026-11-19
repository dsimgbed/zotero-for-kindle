#pragma once
// The library behind an `items` block (screens/plugin_blocks.h): refs become rows, the way the
// app's own lists read them.

#include <cstdint>
#include <functional>

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

}  // namespace zk
