#include "screens/item_source.h"

#include "ui/widgets.h"

namespace zk {

std::optional<ItemRow> LibraryItemSource::row(const ItemRef& ref) {
    auto row = lib_.rowFor(ref.key);
    if (row && ref.library && row->lib != *ref.library) return std::nullopt;
    return row;
}

std::optional<OpenTarget> itemToOpen(Library& lib, const ItemRef& ref, const std::optional<std::string>& attachment,
                                     const std::optional<std::string>& annotation, std::string* why) {
    const auto fail = [why](const char* text) -> std::optional<OpenTarget> {
        if (why) *why = text;
        return std::nullopt;
    };
    auto row = lib.rowFor(ref.key);
    if (!row || (ref.library && row->lib != *ref.library)) return fail("Item not found");
    if (annotation) {
        auto a = lib.annotation(*annotation);
        if (!a || a->deletedHere || (attachment && *attachment != a->parentKey)) return fail("Annotation not found");
        // Its file must be one of this item's (a standalone file is its own item).
        auto file = lib.attachment(a->parentKey);
        if (!file || (file->parentKey.empty() ? file->key : file->parentKey) != ref.key) return fail("Annotation not found");
        auto at = lib.rowFor(ref.key, a->parentKey);
        if (!at || !at->exactFile) return fail("Annotation not found");
        return OpenTarget{std::move(*at), std::move(*a)};
    }
    if (attachment) {
        row = lib.rowFor(ref.key, *attachment);
        if (!row || !row->exactFile) return fail("Item not found");
    }
    return OpenTarget{std::move(*row), std::nullopt};
}

std::string LibraryItemSource::cardMeta(const ItemRow& row) { return itemCardMeta(lib_, row, nowMs_()); }

}  // namespace zk
