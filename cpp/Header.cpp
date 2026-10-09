#include "Stream.h"

#include <niffer/Niffer.h>

namespace niffer {

// Header layout derived empirically from shipped SSE bytes (barrel01.nif et al.)
// and corroborated across the vanilla corpus by the round-trip gate — NOT from
// nif.xml. Order:
//   version line '\n'-terminated, u32 version, u8 endian, u32 userVersion,
//   u32 numBlocks, u32 bsVersion, three ShortStrings (author/process/export),
//   u16 numBlockTypes, SizedString[numBlockTypes], u16 blockTypeIndex[numBlocks],
//   u32 blockSizes[numBlocks], u32 numStrings, u32 maxStringLength,
//   SizedString[numStrings], u32 numGroups, u32 groups[numGroups].
void Header::Sync(Stream& s) {
    s.Line(versionLine, 0x0A);
    s.U32(version);
    s.U8(endian);
    s.U32(userVersion);
    s.U32(numBlocks);
    s.U32(bsVersion);

    s.ShortString(author);
    s.ShortString(processScript);
    s.ShortString(exportScript);

    // Block-type table.
    uint16_t numBlockTypes = static_cast<uint16_t>(blockTypes.size());
    s.U16(numBlockTypes);
    if (s.reading()) blockTypes.resize(numBlockTypes);
    for (auto& t : blockTypes) s.SizedString(t);

    // Per-block type index + size (length == numBlocks).
    if (s.reading()) blockTypeIndex.resize(numBlocks);
    for (auto& ti : blockTypeIndex) s.U16(ti);

    if (s.reading()) blockSizes.resize(numBlocks);
    for (auto& bs : blockSizes) s.U32(bs);

    // Global string table.
    uint32_t numStrings = static_cast<uint32_t>(strings.size());
    s.U32(numStrings);
    s.U32(maxStringLength);
    if (s.reading()) strings.resize(numStrings);
    for (auto& str : strings) s.SizedString(str);

    // Groups.
    uint32_t numGroups = static_cast<uint32_t>(groups.size());
    s.U32(numGroups);
    if (s.reading()) groups.resize(numGroups);
    for (auto& g : groups) s.U32(g);
}

std::string_view Header::TypeNameOf(uint32_t blockIndex) const {
    if (blockIndex >= blockTypeIndex.size()) return {};
    uint16_t ti = blockTypeIndex[blockIndex];
    if (ti >= blockTypes.size()) return {};
    return blockTypes[ti];
}

}  // namespace niffer
