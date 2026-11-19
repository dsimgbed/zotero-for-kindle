#include "screens/item_source.h"

#include "ui/widgets.h"

namespace zk {

std::optional<ItemRow> LibraryItemSource::row(const ItemRef& ref) {
    auto row = lib_.rowFor(ref.key);
    if (row && ref.library && row->lib != *ref.library) return std::nullopt;
    return row;
}

std::string LibraryItemSource::cardMeta(const ItemRow& row) { return itemCardMeta(lib_, row, nowMs_()); }

}  // namespace zk
