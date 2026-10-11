#pragma once
// ── Framing — the file container ─────────────────────────────────────────────
// Header, per-block byte windows, footer roots and trailing bytes; NifFile reads
// and writes files through these. SSE only: any other version is rejected as
// UnsupportedVersion.

#include <niffer/Niffer.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace niffer::detail {

struct BlockSpan {
    size_t   offset = 0;
    uint32_t size   = 0;
};

struct Framing {
    Header                 header;
    std::vector<BlockSpan> blocks;     // index == file block index
    std::vector<int32_t>   roots;
    std::vector<uint8_t>   trailing;
};

std::expected<Framing, NifError> ReadFraming(const uint8_t* data, size_t len);

// Writes `header` (with numBlocks/blockSizes re-measured from `bodies`), the
// bodies, the footer and the trailing bytes.
std::vector<uint8_t> WriteFraming(const Header& header,
                                  const std::vector<std::vector<uint8_t>>& bodies,
                                  const std::vector<int32_t>& roots,
                                  const std::vector<uint8_t>& trailing);

// Reads a whole file; Io error on failure.
std::expected<std::vector<uint8_t>, NifError> ReadWholeFile(const std::string& path);

}  // namespace niffer::detail
