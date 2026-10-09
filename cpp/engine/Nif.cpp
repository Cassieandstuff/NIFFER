// ── niffer::Nif — the schema-path container ──────────────────────────────────
// File framing (header, block windows, footer) is shared with NifFile through
// Framing.h; each block body is a niffer::Block interpreted by the schema.
#include "Framing.h"

#include <niffer/Niffer.h>

namespace niffer {

std::expected<Nif, NifError> Nif::Load(const uint8_t* data, size_t len,
                                       const schema::SchemaSet& set) {
    auto framing = detail::ReadFraming(data, len);
    if (!framing) return std::unexpected(framing.error());

    Nif f;
    f.header   = std::move(framing->header);
    f.roots    = std::move(framing->roots);
    f.trailing = std::move(framing->trailing);
    f.blocks.reserve(framing->blocks.size());
    for (size_t i = 0; i < framing->blocks.size(); ++i) {
        const detail::BlockSpan span = framing->blocks[i];
        f.blocks.push_back(Block::Load(set, f.header.TypeNameOf(static_cast<uint32_t>(i)),
                                       std::span<const uint8_t>(data + span.offset, span.size)));
    }
    return f;
}

std::expected<Nif, NifError> Nif::Load(const std::vector<uint8_t>& bytes,
                                       const schema::SchemaSet& set) {
    return Load(bytes.data(), bytes.size(), set);
}

std::expected<Nif, NifError> Nif::LoadFile(const std::string& path,
                                           const schema::SchemaSet& set) {
    auto bytes = detail::ReadWholeFile(path);
    if (!bytes) return std::unexpected(bytes.error());
    return Load(*bytes, set);
}

std::vector<uint8_t> Nif::Save(bool force) const {
    std::vector<std::vector<uint8_t>> bodies(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) blocks[i].Encode(bodies[i], force);
    return detail::WriteFraming(header, bodies, roots, trailing);
}

Block* Nif::GetBlock(int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= blocks.size()) return nullptr;
    return &blocks[static_cast<size_t>(index)];
}

const Block* Nif::GetBlock(int32_t index) const {
    if (index < 0 || static_cast<size_t>(index) >= blocks.size()) return nullptr;
    return &blocks[static_cast<size_t>(index)];
}

std::string_view Nif::String(NiStringRef ref) const {
    if (ref.IsEmpty() || ref.index >= header.strings.size()) return {};
    return header.strings[ref.index];
}

size_t Nif::RawCount() const {
    size_t n = 0;
    for (const Block& b : blocks) n += b.Modeled() ? 0 : 1;
    return n;
}

}  // namespace niffer
