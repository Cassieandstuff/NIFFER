// ── FaceGen TRI (FRTRI003) — byte-exact round-trip ────────────────────────────
// Header (64 bytes): "FRTRI003", vertexNum, faceNum, [12 reserved], uvVertexNum,
// morphNum, addMorphNum, addVertexNum, [20 reserved]. Then geometry (base verts,
// faces, and — when uvVertexNum>0 — UV coords + UV faces), then diff morphs and
// modifier morphs (each: int32 name length incl. null, name bytes, float
// baseDiff, vertexNum * int16[3] deltas). Uses the same direction-flagged Stream
// as the NIF path, so read and write derive from one layout.
#include "Stream.h"

#include <niffer/Niffer.h>

#include <cstring>
#include <fstream>

namespace niffer {

static std::unexpected<NifError> TriErr(NifErrorKind kind, size_t off, std::string msg) {
    return std::unexpected(NifError{kind, off, -1, std::move(msg)});
}

// One morph: len-prefixed raw name (incl. null), baseDiff, raw int16[3] deltas.
static void SyncMorph(Stream& s, TriMorph& m, int32_t vertexNum) {
    s.SizedString(m.name);   // int32 len + len bytes (byte-exact, keeps the null)
    s.F32(m.baseDiff);
    size_t n = s.reading() ? static_cast<size_t>(vertexNum) * 6 : m.deltas.size();
    s.RawVector(m.deltas, n);
}

static void SyncTri(Stream& s, TriFile& t) {
    // magic
    uint8_t magic[8] = {'F','R','T','R','I','0','0','3'};
    s.Bytes(std::span<uint8_t>(magic, 8));

    s.I32(t.vertexNum);
    s.I32(t.faceNum);
    if (s.reading()) t.reservedA.resize(12);
    s.RawVector(t.reservedA, 12);
    s.I32(t.uvVertexNum);
    s.I32(t.morphNum);
    s.I32(t.addMorphNum);
    s.I32(t.addVertexNum);
    if (s.reading()) t.reservedB.resize(20);
    s.RawVector(t.reservedB, 20);

    // geometry (raw, sized by the header counts)
    auto sized = [&](std::vector<uint8_t>& v, size_t bytes) {
        if (s.reading()) v.resize(bytes);
        s.RawVector(v, bytes);
    };
    sized(t.baseVertices, static_cast<size_t>(t.vertexNum) * 12);
    sized(t.faceIndices,  static_cast<size_t>(t.faceNum) * 12);
    if (t.uvVertexNum > 0) {
        sized(t.uvCoords,      static_cast<size_t>(t.uvVertexNum) * 8);
        sized(t.uvFaceIndices, static_cast<size_t>(t.faceNum) * 12);
    }

    // diff (expression) morphs — name + baseDiff + vertexNum * int16[3].
    // Guarded so a quirky header (morphNum set with no/short morph payload, or
    // an eyes/addVertexNum variant whose diff layout differs) never over-reads:
    // read only full, sane-looking morphs; whatever doesn't fit stays raw below.
    // Byte-exact either way — Save writes the parsed diffMorphs then modMorphData.
    if (s.reading()) {
        const size_t deltaBytes = static_cast<size_t>(t.vertexNum) * 6;
        for (int i = 0; i < t.morphNum; ++i) {
            int32_t nameLen = s.PeekI32();
            size_t needed = 4 + static_cast<size_t>(nameLen) + 4 + deltaBytes;
            if (nameLen <= 0 || nameLen > 4096 || s.Remaining() < needed) break;
            TriMorph m;
            SyncMorph(s, m, t.vertexNum);
            if (!s.ok) break;
            t.diffMorphs.push_back(std::move(m));
        }
    } else {
        for (auto& m : t.diffMorphs) SyncMorph(s, m, t.vertexNum);
    }

    // Remaining diff morphs (if we bailed), the modifier-morph section, and any
    // trailing bytes — kept raw (byte-exact).
    size_t mn = s.reading() ? s.Remaining() : t.modMorphData.size();
    s.RawVector(t.modMorphData, mn);
}

std::expected<TriFile, NifError> TriFile::Load(const uint8_t* data, size_t len) {
    if (len < 64 || std::memcmp(data, "FRTRI003", 8) != 0)
        return TriErr(NifErrorKind::UnsupportedVersion, 0, "not a FRTRI003 .tri file");
    TriFile t;
    Stream s = Stream::Reader(data, data + len);
    SyncTri(s, t);
    if (!s.ok)
        return TriErr(NifErrorKind::Malformed, s.errorOffset, "tri: " + s.error);
    return t;
}
std::expected<TriFile, NifError> TriFile::Load(const std::vector<uint8_t>& b) {
    return Load(b.data(), b.size());
}
std::expected<TriFile, NifError> TriFile::LoadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return TriErr(NifErrorKind::Io, 0, "cannot open: " + path);
    std::streamsize sz = in.tellg();
    in.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0 && !in.read(reinterpret_cast<char*>(buf.data()), sz))
        return TriErr(NifErrorKind::Io, 0, "read failed: " + path);
    return Load(buf);
}
std::vector<uint8_t> TriFile::Save() const {
    std::vector<uint8_t> out;
    Stream s = Stream::Writer(out);
    TriFile& self = const_cast<TriFile&>(*this);   // write mode reads from members
    SyncTri(s, self);
    return out;
}

}  // namespace niffer
