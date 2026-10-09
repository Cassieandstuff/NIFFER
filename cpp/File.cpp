#include "Registry.h"
#include "Stream.h"

#include <niffer/Niffer.h>

#include <cstdio>
#include <fstream>

namespace niffer {

const char* LibraryVersion() { return "0.1.0"; }

// ── UnknownBlock ──────────────────────────────────────────────────────────────
// Read: consumes exactly its windowed size (Remaining) into raw. Write: re-emits
// raw verbatim. Block order/indices are preserved, so refs inside raw stay valid.
void UnknownBlock::Sync(Stream& s) {
    size_t n = s.reading() ? s.Remaining() : raw.size();
    s.RawVector(raw, n);
}

// ── NifFile ───────────────────────────────────────────────────────────────────
NiObject* NifFile::GetBlock(int32_t index) const {
    if (index < 0 || static_cast<size_t>(index) >= blocks.size()) return nullptr;
    return blocks[static_cast<size_t>(index)].get();
}

std::string_view NifFile::String(NiStringRef ref) const {
    if (ref.IsEmpty() || ref.index >= header.strings.size()) return {};
    return header.strings[ref.index];
}

static std::unexpected<NifError> Err(NifErrorKind kind, size_t off,
                                     int32_t block, std::string msg) {
    return std::unexpected(NifError{kind, off, block, std::move(msg)});
}

std::expected<NifFile, NifError> NifFile::Load(const uint8_t* data, size_t len) {
    EnsureBlocksRegistered();
    NifFile f;

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

    // Blocks.
    size_t off = hdr.Position();
    const uint32_t n = f.header.numBlocks;
    f.blocks.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t size = f.header.blockSizes[i];
        if (off + size > len)
            return Err(NifErrorKind::Malformed, off, static_cast<int32_t>(i),
                       "block extends past end of file");
        const uint8_t* bs = data + off;
        const uint8_t* be = bs + size;
        std::string_view typeName = f.header.TypeNameOf(i);

        std::unique_ptr<NiObject> obj = NifRegistry::Instance().Create(typeName);
        if (obj) {
            Stream sub = Stream::Reader(bs, be);
            obj->Sync(sub);
            if (!sub.ok || sub.Position() != size) {
                // Our typed layout disagreed with the file: demote to raw and
                // record it LOUDLY (a Niffer layout bug, not just an unknown).
                f.layoutMismatches.push_back(LayoutMismatch{
                    i, std::string(typeName), size,
                    static_cast<uint32_t>(sub.Position())});
                auto raw = std::make_unique<UnknownBlock>();
                raw->typeName.assign(typeName);
                raw->raw.assign(bs, be);
                obj = std::move(raw);
            }
        } else {
            auto raw = std::make_unique<UnknownBlock>();
            raw->typeName.assign(typeName);
            raw->raw.assign(bs, be);
            obj = std::move(raw);
            f.unknownBlocks.push_back(UnknownBlockInfo{i, std::string(typeName)});
        }
        f.blocks.push_back(std::move(obj));
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

std::expected<NifFile, NifError> NifFile::Load(const std::vector<uint8_t>& bytes) {
    return Load(bytes.data(), bytes.size());
}

std::expected<NifFile, NifError> NifFile::LoadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return Err(NifErrorKind::Io, 0, -1, "cannot open: " + path);
    std::streamsize sz = in.tellg();
    in.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0 && !in.read(reinterpret_cast<char*>(buf.data()), sz))
        return Err(NifErrorKind::Io, 0, -1, "read failed: " + path);
    return Load(buf);
}

std::vector<uint8_t> NifFile::Save() const {
    // Serialize each block body first so its exact size is known before the
    // header (which carries the block-size array) is emitted.
    std::vector<std::vector<uint8_t>> bodies(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        Stream bw = Stream::Writer(bodies[i]);
        blocks[i]->Sync(bw);
    }

    // Reflect the live block list into the header; every other header fact is
    // emitted verbatim (explicit-for-round-trip).
    Header h = header;
    h.numBlocks = static_cast<uint32_t>(blocks.size());
    h.blockSizes.resize(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i)
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

}  // namespace niffer
