// ── Decode helpers (Phase 4 access layer) ─────────────────────────────────────
// Turns disk-precision block payloads into consumer-ready values: BSVertexDesc
// vertex unpack, geometry extraction, TRI morph scaling, glossiness->roughness.
// Vertex layout derived from the SSE format (vertexSize=(desc&0xF)*4; nibble
// byte-offsets; flags at bit 44) and corroborated against the corpus.
#include <niffer/Niffer.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>

namespace niffer {

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

// ── Schema path ───────────────────────────────────────────────────────────────
// Geometry through the generated views: vertices are BSVertex values (their
// layout already resolved from vertexDesc by the schema), read one element at a
// time with Block::ForEachElement and indexed with the views' field-index
// constants. Before the typed path was deleted, `niffer-tool decode-compare`
// held these bit-identical to its byte decoders on all 83,178 corpus shapes.
namespace {

float F32Of(const Value& v) { return std::bit_cast<float>(static_cast<uint32_t>(v.bits)); }
float HalfOf(const Value& v) { return Half{ static_cast<uint16_t>(v.bits) }.ToFloat(); }
uint8_t U8Of(const Value& v) { return static_cast<uint8_t>(v.bits); }

// Fills `m` from the BSVertex array at `array` in `b`: `nv` vertices laid out per
// `desc`. Allocates exactly what DecodeVertexBlock does for the same desc.
void SchemaVertices(const Block& b, const RelPath& array, uint64_t desc, uint16_t nv, DecodedMesh& m) {
    m.numVertices = nv;
    const VertexLayout L = DecodeVertexDesc(desc);
    if (L.vertexSize == 0 || b.Count(array) < nv) return;

    m.positions.resize(static_cast<size_t>(nv) * 3);
    if (L.hasNormal)  m.normals.resize(static_cast<size_t>(nv) * 3);
    if (L.hasTangent) m.tangents.resize(static_cast<size_t>(nv) * 4);
    if (L.hasUV)      m.uvs.resize(static_cast<size_t>(nv) * 2);
    if (L.hasColor)   m.colors.resize(static_cast<size_t>(nv) * 4);
    if (L.hasSkin) {
        m.boneWeights.resize(static_cast<size_t>(nv) * 4);
        m.boneIndices.resize(static_cast<size_t>(nv) * 4);
    }

    using V = view::BSVertex;
    b.ForEachElement(array, [&](size_t i, const schema::TypeDef&, const Value& e) {
        if (i >= nv) return;
        if (const Value& p = e.items[V::kPosition]; p.present)
            for (int k = 0; k < 3; ++k) m.positions[i*3+k] = F32Of(p.items[k]);
        if (const Value& uv = e.items[V::kUv]; uv.present && L.hasUV) {
            m.uvs[i*2+0] = HalfOf(uv.items[view::TexCoord::kU]);
            m.uvs[i*2+1] = HalfOf(uv.items[view::TexCoord::kV]);
        }
        if (const Value& n = e.items[V::kNormal]; n.present && L.hasNormal)
            for (int k = 0; k < 3; ++k) m.normals[i*3+k] = SNorm(U8Of(n.items[k]));
        if (const Value& t = e.items[V::kTangent]; t.present && L.hasTangent) {
            for (int k = 0; k < 3; ++k) m.tangents[i*4+k] = SNorm(U8Of(t.items[k]));
            m.tangents[i*4+3] = 1.0f;
        }
        if (const Value& c = e.items[V::kColor]; c.present && L.hasColor)
            for (int k = 0; k < 4; ++k) m.colors[i*4+k] = U8Of(c.items[k]);
        if (const Value& w = e.items[V::kBoneWeights]; w.present && L.hasSkin) {
            const Value& bi = e.items[V::kBoneIndices];
            for (int k = 0; k < 4; ++k) {
                m.boneWeights[i*4+k] = HalfOf(w.items[k]);
                m.boneIndices[i*4+k] = U8Of(bi.items[k]);
            }
        }
    });
}

// Appends the triangles of the Triangle array at `array` (all of them, or only
// those whose indices are below `limit` when it is non-zero).
void SchemaTriangles(const Block& b, const RelPath& array, uint32_t limit, std::vector<uint16_t>& out) {
    using T = view::Triangle;
    b.ForEachElement(array, [&](size_t, const schema::TypeDef&, const Value& t) {
        const auto a = static_cast<uint16_t>(t.items[T::kA].bits);
        const auto c1 = static_cast<uint16_t>(t.items[T::kB].bits);
        const auto c2 = static_cast<uint16_t>(t.items[T::kC].bits);
        if (limit && (a >= limit || c1 >= limit || c2 >= limit)) return;
        out.push_back(a); out.push_back(c1); out.push_back(c2);
    });
}

}  // namespace

DecodedMesh DecodeMesh(const view::BSTriShape& s) {
    DecodedMesh m;
    const RelPath root{};
    SchemaVertices(s.block(), root.Child(view::BSTriShape::kVertices), s.VertexDesc(), s.NumVertices(), m);
    if (!m.positions.empty())
        SchemaTriangles(s.block(), root.Child(view::BSTriShape::kTriangles), 0, m.triangles);
    return m;
}

DecodedMesh DecodeMesh(const view::NiSkinPartition& p) {
    // The vertex count comes from the byte size, and only
    // triangles whose indices fall inside the vertex array are kept.
    const uint32_t vs = p.VertexSize();
    const auto nv = static_cast<uint16_t>(vs ? p.DataSize() / vs : 0);
    DecodedMesh m;
    const RelPath root{};
    SchemaVertices(p.block(), root.Child(view::NiSkinPartition::kVertexData), p.VertexDesc(), nv, m);
    const RelPath parts = root.Child(view::NiSkinPartition::kPartitions);
    const size_t n = p.block().Count(parts);
    for (size_t i = 0; i < n; ++i)
        SchemaTriangles(p.block(), parts.Child(static_cast<uint16_t>(i)).Child(view::SkinPartition::kTrueTriangles),
                        nv, m.triangles);
    return m;
}

DecodedMesh DecodeShapeMesh(NifFile& nif, Block& shapeBlock) {
    auto shape = view::BSTriShape::From(shapeBlock);
    if (!shape) return {};
    std::optional<view::NiSkinPartition> part;
    if (auto skin = nif.Get(shape->Skin()))
        part = nif.Get(skin->SkinPartition());

    if (auto dyn = view::BSDynamicTriShape::From(shapeBlock)) {
        // FaceGen heads: attributes and triangles in the partition, positions in
        // the shape's dynamic vertices (one Vec4 each).
        DecodedMesh m = part ? DecodeMesh(*part) : DecodedMesh{};
        const size_t nv = m.numVertices;
        m.positions.assign(nv * 3, 0.f);
        shapeBlock.ForEachElement(RelPath{}.Child(view::BSDynamicTriShape::kDynamicData),
                                  [&](size_t i, const schema::TypeDef&, const Value& v) {
            if (i >= nv) return;
            for (int k = 0; k < 3; ++k) m.positions[i*3+k] = F32Of(v.items[k]);
        });
        return m;
    }
    DecodedMesh m = DecodeMesh(*shape);
    if (m.numVertices == 0 && part) m = DecodeMesh(*part);
    return m;
}

}  // namespace niffer
