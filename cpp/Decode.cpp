// ── Decode helpers (Phase 4 access layer) ─────────────────────────────────────
// Turns disk-precision block payloads into consumer-ready values: BSVertexDesc
// vertex unpack, geometry extraction, TRI morph scaling, glossiness->roughness.
// Vertex layout derived from the SSE format (vertexSize=(desc&0xF)*4; nibble
// byte-offsets; flags at bit 44) and corroborated against the corpus.
#include <niffer/Niffer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace niffer {

// IEEE half (uint16) -> float.
static float HalfToFloat(uint16_t h) {
    uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) { bits = sign; }
        else {  // subnormal
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0) { mant <<= 1; --exp; }
            mant &= 0x3FF;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000 | (mant << 13);  // inf/nan
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float f; std::memcpy(&f, &bits, 4); return f;
}
static float ReadF32(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }
static float ReadHalf(const uint8_t* p) { uint16_t h; std::memcpy(&h, p, 2); return HalfToFloat(h); }
// Signed byte 0..255 -> [-1, 1].
static float SNorm(uint8_t b) { return static_cast<float>(b) / 127.5f - 1.0f; }

VertexLayout DecodeVertexDesc(uint64_t d) {
    VertexLayout v;
    v.vertexSize   = static_cast<uint32_t>((d & 0xF) * 4);
    v.uvOffset     = static_cast<uint32_t>(((d >> 8)  & 0xF) * 4);
    v.normalOffset = static_cast<uint32_t>(((d >> 16) & 0xF) * 4);
    v.tangentOffset= static_cast<uint32_t>(((d >> 20) & 0xF) * 4);
    v.colorOffset  = static_cast<uint32_t>(((d >> 24) & 0xF) * 4);
    v.skinOffset   = static_cast<uint32_t>(((d >> 28) & 0xF) * 4);
    v.eyeOffset    = static_cast<uint32_t>(((d >> 36) & 0xF) * 4);
    uint32_t vf = static_cast<uint32_t>((d >> 44) & 0xFFFF);
    v.hasVertex  = vf & 0x001;
    v.hasUV      = vf & 0x002;
    v.hasNormal  = vf & 0x008;
    v.hasTangent = vf & 0x010;
    v.hasColor   = vf & 0x020;
    v.hasSkin    = vf & 0x040;
    v.hasEye     = vf & 0x100;
    // Position precision: infer from the first attribute offset (16 => full
    // float3+w, 8 => half3+w); fall back to the FULLPREC flag when position is
    // the only attribute.
    uint32_t first = 0;
    for (uint32_t off : {v.uvOffset, v.normalOffset, v.tangentOffset,
                         v.colorOffset, v.skinOffset, v.eyeOffset})
        if (off > 0 && (first == 0 || off < first)) first = off;
    if (first == 8)       v.fullPrecision = false;
    else if (first >= 16) v.fullPrecision = true;
    else                  v.fullPrecision = (vf & 0x400) != 0;
    return v;
}

// Decode interleaved vertex data (raw) + triangle bytes into a DecodedMesh.
static DecodedMesh DecodeVertexBlock(const std::vector<uint8_t>& vtx, uint64_t desc,
                                     uint16_t numVertices,
                                     const std::vector<uint8_t>* triBytes,
                                     uint16_t numTriangles) {
    DecodedMesh m;
    m.numVertices = numVertices;
    VertexLayout L = DecodeVertexDesc(desc);
    if (L.vertexSize == 0 || vtx.size() < static_cast<size_t>(numVertices) * L.vertexSize)
        return m;

    m.positions.resize(numVertices * 3);
    if (L.hasNormal)  m.normals.resize(numVertices * 3);
    if (L.hasTangent) m.tangents.resize(numVertices * 4);
    if (L.hasUV)      m.uvs.resize(numVertices * 2);
    if (L.hasColor)   m.colors.resize(numVertices * 4);
    if (L.hasSkin) { m.boneWeights.resize(numVertices * 4); m.boneIndices.resize(numVertices * 4); }

    for (uint16_t i = 0; i < numVertices; ++i) {
        const uint8_t* v = vtx.data() + static_cast<size_t>(i) * L.vertexSize;
        if (L.fullPrecision) {
            m.positions[i*3+0] = ReadF32(v + 0);
            m.positions[i*3+1] = ReadF32(v + 4);
            m.positions[i*3+2] = ReadF32(v + 8);
        } else {
            m.positions[i*3+0] = ReadHalf(v + 0);
            m.positions[i*3+1] = ReadHalf(v + 2);
            m.positions[i*3+2] = ReadHalf(v + 4);
        }
        if (L.hasUV) {
            m.uvs[i*2+0] = ReadHalf(v + L.uvOffset);
            m.uvs[i*2+1] = ReadHalf(v + L.uvOffset + 2);
        }
        if (L.hasNormal) {
            m.normals[i*3+0] = SNorm(v[L.normalOffset + 0]);
            m.normals[i*3+1] = SNorm(v[L.normalOffset + 1]);
            m.normals[i*3+2] = SNorm(v[L.normalOffset + 2]);
        }
        if (L.hasTangent) {
            m.tangents[i*4+0] = SNorm(v[L.tangentOffset + 0]);
            m.tangents[i*4+1] = SNorm(v[L.tangentOffset + 1]);
            m.tangents[i*4+2] = SNorm(v[L.tangentOffset + 2]);
            m.tangents[i*4+3] = 1.0f;
        }
        if (L.hasColor)
            std::memcpy(&m.colors[i*4], v + L.colorOffset, 4);
        if (L.hasSkin) {
            for (int w = 0; w < 4; ++w)
                m.boneWeights[i*4+w] = ReadHalf(v + L.skinOffset + w*2);
            for (int b = 0; b < 4; ++b)
                m.boneIndices[i*4+b] = v[L.skinOffset + 8 + b];
        }
    }

    if (triBytes && numTriangles > 0 &&
        triBytes->size() >= static_cast<size_t>(numTriangles) * 6) {
        m.triangles.resize(numTriangles * 3);
        std::memcpy(m.triangles.data(), triBytes->data(),
                    static_cast<size_t>(numTriangles) * 6);
    }
    return m;
}

DecodedMesh DecodeMesh(const BSTriShape& s) {
    // geometryData holds numVertices*vertexSize vertex bytes, then triangles.
    VertexLayout L = DecodeVertexDesc(s.vertexDesc);
    size_t vtxBytes = static_cast<size_t>(s.numVertices) * L.vertexSize;
    std::vector<uint8_t> vtx, tri;
    if (s.geometryData.size() >= vtxBytes) {
        vtx.assign(s.geometryData.begin(), s.geometryData.begin() + vtxBytes);
        tri.assign(s.geometryData.begin() + vtxBytes, s.geometryData.end());
    }
    return DecodeVertexBlock(vtx, s.vertexDesc, s.numVertices, &tri, s.numTriangles);
}

// Extract global-indexed triangles from an SSE NiSkinPartition's partition
// tables. Per partition, after the variable-length skin fields, SSE stores
// unkShort(2) + a per-partition BSVertexDesc(8) + trueTriangles (numTriangles ×
// uint16[3]) that index the GLOBAL vertex array directly. Layout reverse-
// engineered + validated (every vanilla character mesh parses with zero residual
// and all indices in range). Fully bounds-checked: malformed data stops early.
static void DecodeSkinPartitionTriangles(const NiSkinPartition& p, uint16_t numVerts,
                                         std::vector<uint16_t>& out) {
    const uint8_t* cur = p.partitionData.data();
    const uint8_t* end = cur + p.partitionData.size();
    auto u16 = [&](uint16_t& v) { if (cur + 2 > end) { cur = end; v = 0; return; }
                                  std::memcpy(&v, cur, 2); cur += 2; };
    auto u8  = [&]() -> uint8_t { if (cur >= end) return 0; return *cur++; };
    auto skip = [&](size_t n) { cur = (static_cast<size_t>(end - cur) < n) ? end : cur + n; };

    for (uint32_t pi = 0; pi < p.numPartitions && cur < end; ++pi) {
        uint16_t nV, nT, nB, nS, nWPV;
        u16(nV); u16(nT); u16(nB); u16(nS); u16(nWPV);
        skip(static_cast<size_t>(nB) * 2);                 // bones
        if (u8()) skip(static_cast<size_t>(nV) * 2);       // hasVertexMap + vertexMap
        if (u8()) skip(static_cast<size_t>(nV) * nWPV * 4);// hasVertexWeights + float weights
        skip(static_cast<size_t>(nS) * 2);                 // strip lengths
        uint8_t hasFaces = u8();
        if (hasFaces && nS == 0) skip(static_cast<size_t>(nT) * 6);  // mapped triangles
        if (u8()) skip(static_cast<size_t>(nV) * nWPV);    // hasBoneIndices + indices
        skip(2);                                           // unkShort
        skip(8);                                           // per-partition BSVertexDesc
        for (uint16_t t = 0; t < nT; ++t) {                // trueTriangles (global)
            uint16_t a, b, c; u16(a); u16(b); u16(c);
            if (a < numVerts && b < numVerts && c < numVerts) {
                out.push_back(a); out.push_back(b); out.push_back(c);
            }
        }
    }
}

DecodedMesh DecodeMesh(const NiSkinPartition& p) {
    // Skinned character meshes keep ALL geometry here: vertices in vertexData,
    // triangles in the partition tables (global-indexed trueTriangles).
    uint16_t nv = p.vertexSize ? static_cast<uint16_t>(p.dataSize / p.vertexSize) : 0;
    DecodedMesh m = DecodeVertexBlock(p.vertexData, p.vertexDesc, nv, nullptr, 0);
    DecodeSkinPartitionTriangles(p, nv, m.triangles);
    return m;
}

float GlossinessToRoughness(float glossiness) {
    // Blinn-Phong exponent -> GGX roughness (matches the old NifGlossinessToRoughness).
    if (glossiness <= 0.f) return 1.f;
    return std::sqrt(2.0f / (glossiness + 2.0f));
}

std::vector<float> DecodeMorphDeltas(const TriMorph& morph) {
    size_t n = morph.deltas.size() / 2;   // int16 count
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        int16_t d; std::memcpy(&d, morph.deltas.data() + i * 2, 2);
        out[i] = static_cast<float>(d) * morph.baseDiff;
    }
    return out;
}

std::vector<float> DecodeBaseVertices(const TriFile& tri) {
    size_t n = tri.baseVertices.size() / 4;   // float count (vertexNum*3)
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i)
        std::memcpy(&out[i], tri.baseVertices.data() + i * 4, 4);
    return out;
}

std::vector<float> ApplyMorphs(const std::vector<float>& base, const TriFile& tri,
                               const std::map<std::string, float>& weights) {
    std::vector<float> out = base;
    if (weights.empty()) return out;
    for (const TriMorph& m : tri.diffMorphs) {
        auto it = weights.find(m.name);
        if (it == weights.end() || it->second == 0.f) continue;
        const float w = it->second;
        const std::vector<float> d = DecodeMorphDeltas(m);   // 3 per vertex
        const size_t n = std::min(d.size(), out.size());
        for (size_t i = 0; i < n; ++i)
            out[i] += d[i] * w;
    }
    return out;
}

}  // namespace niffer
