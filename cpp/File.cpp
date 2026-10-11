// ── File framing — header, block windows, footer (hpp/Framing.h) ─────────────
#include "Framing.h"
#include "Stream.h"

#include <niffer/Niffer.h>

#include <cstdio>
#include <fstream>

namespace niffer {

const char* LibraryVersion() { return "0.3.0"; }

static std::unexpected<NifError> Err(NifErrorKind kind, size_t off,
                                     int32_t block, std::string msg) {
    return std::unexpected(NifError{kind, off, block, std::move(msg)});
}

// ── Framing ───────────────────────────────────────────────────────────────────
namespace detail {

std::expected<Framing, NifError> ReadFraming(const uint8_t* data, size_t len) {
    Framing f;

    // Header.
    Stream hdr = Stream::Reader(data, data + len);
    f.header.Sync(hdr);
    if (!hdr.ok)
        return Err(NifErrorKind::Malformed, hdr.errorOffset, -1,
                   "header: " + hdr.error);

    if (f.header.version != kNifVersion || f.header.endian != 1 ||
        f.header.userVersion != kUserVersion || f.header.bsVersion != kStreamVersion) {
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      "unsupported: version=0x%08X endian=%u user=%u bs=%u (SSE only)",
                      f.header.version, f.header.endian, f.header.userVersion,
                      f.header.bsVersion);
        return Err(NifErrorKind::UnsupportedVersion, 0, -1, buf);
    }

    // Block windows.
    size_t off = hdr.Position();
    const uint32_t n = f.header.numBlocks;
    f.blocks.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t size = f.header.blockSizes[i];
        if (off + size > len)
            return Err(NifErrorKind::Malformed, off, static_cast<int32_t>(i),
                       "block extends past end of file");
        f.blocks.push_back(BlockSpan{off, size});
        off += size;
    }

    // Footer: num roots + root refs.
    Stream foot = Stream::Reader(data + off, data + len);
    uint32_t numRoots = 0;
    foot.U32(numRoots);
    if (!foot.ok)
        return Err(NifErrorKind::Malformed, off, -1, "footer: " + foot.error);
    f.roots.resize(numRoots);
    for (auto& r : f.roots) { int32_t v = 0; foot.I32(v); r = v; }
    if (!foot.ok)
        return Err(NifErrorKind::Malformed, off + foot.errorOffset, -1,
                   "footer roots: " + foot.error);

    // Any bytes past the footer are preserved verbatim (normally none).
    size_t footerEnd = off + foot.Position();
    if (footerEnd < len) f.trailing.assign(data + footerEnd, data + len);

    return f;
}

std::vector<uint8_t> WriteFraming(const Header& header,
                                  const std::vector<std::vector<uint8_t>>& bodies,
                                  const std::vector<int32_t>& roots,
                                  const std::vector<uint8_t>& trailing) {
    // Reflect the live block list into the header; every other header fact is
    // emitted verbatim (explicit-for-round-trip).
    Header h = header;
    h.numBlocks = static_cast<uint32_t>(bodies.size());
    h.blockSizes.resize(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i)
        h.blockSizes[i] = static_cast<uint32_t>(bodies[i].size());

    std::vector<uint8_t> out;
    Stream w = Stream::Writer(out);
    h.Sync(w);
    for (auto& body : bodies) out.insert(out.end(), body.begin(), body.end());

    // Footer.
    uint32_t numRoots = static_cast<uint32_t>(roots.size());
    w = Stream::Writer(out);  // continue appending to the same buffer
    w.U32(numRoots);
    for (int32_t r : roots) { int32_t v = r; w.I32(v); }

    out.insert(out.end(), trailing.begin(), trailing.end());
    return out;
}

std::expected<std::vector<uint8_t>, NifError> ReadWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return Err(NifErrorKind::Io, 0, -1, "cannot open: " + path);
    std::streamsize sz = in.tellg();
    in.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0 && !in.read(reinterpret_cast<char*>(buf.data()), sz))
        return Err(NifErrorKind::Io, 0, -1, "read failed: " + path);
    return buf;
}

}  // namespace detail

}  // namespace niffer
