// ── NifFile — editing the block list ─────────────────────────────────────────
// Insert and remove with every reference renumbered. Generic over the schema:
// refs are found through Block::MapRefs / MapStringRefs, never per block class.
#include <niffer/Niffer.h>

#include <algorithm>
#include <limits>

namespace niffer {

namespace {

// A block (other than one being removed) whose refs a renumbering can't see: raw,
// or holding opaque bytes that could hide an index. "" when there is none.
std::string Blocker(const NifFile& nif, const std::vector<bool>* gone = nullptr) {
    for (size_t i = 0; i < nif.blocks.size(); ++i) {
        if (gone && (*gone)[i]) continue;
        const Block& b = nif.blocks[i];
        const std::string where = "block " + std::to_string(i) + " (" + std::string(b.TypeName()) + ")";
        if (!b.Modeled()) return where + " is raw, so its refs can't be renumbered";
        if (std::string at = b.OpaqueRefs(); !at.empty())
            return where + " has opaque bytes at " + at + " that could hold a ref or string index";
    }
    return {};
}

uint32_t LongestString(const std::vector<std::string>& strings) {
    size_t n = 0;
    for (const std::string& s : strings) n = std::max(n, s.size());
    return static_cast<uint32_t>(n);
}

}  // namespace

std::expected<int32_t, std::string> NifFile::InsertBlock(int32_t index, Block block) {
    const auto n = static_cast<int32_t>(blocks.size());
    if (index < 0 || index > n) return std::unexpected("insert index " + std::to_string(index) + " is outside 0.." + std::to_string(n));
    if (!block.Modeled()) return std::unexpected("the inserted " + std::string(block.TypeName()) + " block isn't modeled");
    if (header.blockTypeIndex.size() != blocks.size())
        return std::unexpected("the header's block-type index doesn't match the block list");
    if (index < n)
        if (std::string blocker = Blocker(*this); !blocker.empty()) return std::unexpected(blocker);
    // Its class in the block-type table.
    const std::string_view type = block.TypeName();
    size_t ti = 0;
    while (ti < header.blockTypes.size() && header.blockTypes[ti] != type) ++ti;
    if (ti == header.blockTypes.size()) {
        if (ti >= std::numeric_limits<uint16_t>::max()) return std::unexpected("the block-type table is full");
        header.blockTypes.emplace_back(type);
    }

    if (index < n) {
        auto shift = [index](int32_t r) { return r >= index ? r + 1 : r; };
        for (Block& b : blocks) b.MapRefs(shift);
        for (int32_t& r : roots) r = shift(r);
    }
    blocks.insert(blocks.begin() + index, std::move(block));
    header.blockTypeIndex.insert(header.blockTypeIndex.begin() + index, static_cast<uint16_t>(ti));
    return index;
}

std::expected<size_t, std::string> NifFile::RemoveBlocks(std::span<const int32_t> indices) {
    const auto n = static_cast<int32_t>(blocks.size());
    std::vector<bool> gone(blocks.size(), false);
    size_t count = 0;
    for (int32_t i : indices) {
        if (i < 0 || i >= n) return std::unexpected("remove index " + std::to_string(i) + " is outside 0.." + std::to_string(n - 1));
        if (!gone[static_cast<size_t>(i)]) ++count;
        gone[static_cast<size_t>(i)] = true;
    }
    if (count == 0) return 0;
    if (header.blockTypeIndex.size() != blocks.size())
        return std::unexpected("the header's block-type index doesn't match the block list");
    if (std::string blocker = Blocker(*this, &gone); !blocker.empty()) return std::unexpected(blocker);

    // Old index -> new index, -1 for a removed block.
    std::vector<int32_t> remap(blocks.size(), -1);
    int32_t next = 0;
    for (size_t i = 0; i < blocks.size(); ++i)
        if (!gone[i]) remap[i] = next++;

    // Strings and types in use before, and by the survivors after.
    const size_t numStrings = header.strings.size();
    std::vector<bool> stringBefore(numStrings, false), stringAfter(numStrings, false);
    std::vector<bool> typeBefore(header.blockTypes.size(), false), typeAfter(header.blockTypes.size(), false);
    for (size_t i = 0; i < blocks.size(); ++i) {
        const uint16_t ti = header.blockTypeIndex[i];
        if (ti < typeBefore.size()) {
            typeBefore[ti] = true;
            if (!gone[i]) typeAfter[ti] = true;
        }
        blocks[i].ForEachStringRef([&](uint32_t s) {
            if (s >= numStrings) return;
            stringBefore[s] = true;
            if (!gone[i]) stringAfter[s] = true;
        });
    }

    // Survivors, with their refs renumbered.
    auto map = [&](int32_t r) { return (r >= 0 && r < n) ? remap[static_cast<size_t>(r)] : r; };
    std::vector<Block>    kept;
    std::vector<uint16_t> keptTypes;
    kept.reserve(blocks.size() - count);
    keptTypes.reserve(blocks.size() - count);
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (gone[i]) continue;
        blocks[i].MapRefs(map);
        kept.push_back(std::move(blocks[i]));
        keptTypes.push_back(header.blockTypeIndex[i]);
    }
    std::vector<int32_t> keptRoots;
    for (int32_t r : roots) {
        if (r >= 0 && r < n && gone[static_cast<size_t>(r)]) continue;
        keptRoots.push_back(map(r));
    }
    blocks = std::move(kept);
    roots  = std::move(keptRoots);

    // Block types only the removed blocks used.
    std::vector<uint16_t> typeRemap(header.blockTypes.size());
    std::vector<std::string> types;
    for (size_t t = 0; t < header.blockTypes.size(); ++t) {
        typeRemap[t] = static_cast<uint16_t>(types.size());
        if (typeBefore[t] && !typeAfter[t]) continue;
        types.push_back(std::move(header.blockTypes[t]));
    }
    header.blockTypes = std::move(types);
    for (uint16_t& ti : keptTypes)
        if (ti < typeRemap.size()) ti = typeRemap[ti];
    header.blockTypeIndex = std::move(keptTypes);

    // Strings only the removed blocks used.
    bool dropString = false;
    for (size_t s = 0; s < numStrings; ++s) dropString = dropString || (stringBefore[s] && !stringAfter[s]);
    if (dropString) {
        const bool exactMax = header.maxStringLength == LongestString(header.strings);
        std::vector<uint32_t> stringRemap(numStrings);
        std::vector<std::string> strings;
        for (size_t s = 0; s < numStrings; ++s) {
            stringRemap[s] = static_cast<uint32_t>(strings.size());
            if (stringBefore[s] && !stringAfter[s]) continue;
            strings.push_back(std::move(header.strings[s]));
        }
        header.strings = std::move(strings);
        for (Block& b : blocks)
            b.MapStringRefs([&](uint32_t s) { return s < numStrings ? stringRemap[s] : s; });
        if (exactMax) header.maxStringLength = LongestString(header.strings);
    }
    return count;
}

std::vector<int32_t> NifFile::OwnedTree(int32_t index) const {
    if (index < 0 || static_cast<size_t>(index) >= blocks.size()) return {};
    const size_t n = blocks.size();
    // Everything reachable from `index` through owning refs.
    std::vector<bool> in(n, false);
    std::vector<int32_t> stack{ index };
    in[static_cast<size_t>(index)] = true;
    while (!stack.empty()) {
        const int32_t b = stack.back();
        stack.pop_back();
        blocks[static_cast<size_t>(b)].ForEachRef([&](int32_t r, bool isPtr, std::span<const uint16_t>) {
            if (isPtr || r < 0 || static_cast<size_t>(r) >= n || in[static_cast<size_t>(r)]) return;
            in[static_cast<size_t>(r)] = true;
            stack.push_back(r);
        });
    }
    // Drop, until nothing changes, any block (but the root) that something
    // outside the set owns, and what is reachable only through it.
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<bool> ownedOutside(n, false);
        for (size_t i = 0; i < n; ++i) {
            if (in[i]) continue;
            blocks[i].ForEachRef([&](int32_t r, bool isPtr, std::span<const uint16_t>) {
                if (!isPtr && r >= 0 && static_cast<size_t>(r) < n) ownedOutside[static_cast<size_t>(r)] = true;
            });
        }
        // Re-walk from the root, not entering blocks owned from outside.
        std::vector<bool> reach(n, false);
        reach[static_cast<size_t>(index)] = true;
        stack.assign(1, index);
        while (!stack.empty()) {
            const int32_t b = stack.back();
            stack.pop_back();
            blocks[static_cast<size_t>(b)].ForEachRef([&](int32_t r, bool isPtr, std::span<const uint16_t>) {
                if (isPtr || r < 0 || static_cast<size_t>(r) >= n) return;
                const auto u = static_cast<size_t>(r);
                if (reach[u] || !in[u] || ownedOutside[u]) return;
                reach[u] = true;
                stack.push_back(r);
            });
        }
        for (size_t i = 0; i < n; ++i)
            if (in[i] && !reach[i]) { in[i] = false; changed = true; }
    }
    std::vector<int32_t> out;
    for (size_t i = 0; i < n; ++i)
        if (in[i]) out.push_back(static_cast<int32_t>(i));
    return out;
}

std::expected<size_t, std::string> NifFile::RemoveTree(int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= blocks.size())
        return std::unexpected("remove index " + std::to_string(index) + " is outside the block list");
    const std::vector<int32_t> tree = OwnedTree(index);
    return RemoveBlocks(tree);
}

NiStringRef NifFile::AddString(std::string_view s) {
    for (size_t i = 0; i < header.strings.size(); ++i)
        if (header.strings[i] == s) return NiStringRef{ static_cast<uint32_t>(i) };
    header.strings.emplace_back(s);
    header.maxStringLength = std::max(header.maxStringLength, static_cast<uint32_t>(s.size()));
    return NiStringRef{ static_cast<uint32_t>(header.strings.size() - 1) };
}

}  // namespace niffer
