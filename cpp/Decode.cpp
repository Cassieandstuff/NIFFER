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

// sign((N x T) . B): +1 for a right-handed frame, -1 for a mirrored one.
float Handedness(const float* n, const float* t, const float* b) {
    const float c[3] = { n[1]*t[2] - n[2]*t[1], n[2]*t[0] - n[0]*t[2], n[0]*t[1] - n[1]*t[0] };
    return c[0]*b[0] + c[1]*b[1] + c[2]*b[2] < 0.f ? -1.0f : 1.0f;
}

// Fills `m` from the BSVertex array at `array` in `b`: `nv` vertices laid out per
// `desc`. Allocates exactly what DecodeVertexBlock does for the same desc.
void SchemaVertices(const Block& b, const RelPath& array, uint64_t desc, uint16_t nv, DecodedMesh& m) {
    m.numVertices = nv;
    const VertexLayout L = DecodeVertexDesc(desc);
    if (L.vertexSize == 0 || b.Count(array) < nv) return;

    m.positions.resize(static_cast<size_t>(nv) * 3);
    if (L.hasNormal)  m.normals.resize(static_cast<size_t>(nv) * 3);
    if (L.hasTangent) m.tangents.resize(static_cast<size_t>(nv) * 4);
    if (L.hasTangent && L.hasNormal) m.bitangents.resize(static_cast<size_t>(nv) * 3);
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
            if (L.hasNormal) {
                // The bitangent rides in the three W slots (docs/tangent-space.md); a
                // BSDynamicTriShape's x is in its dynamic vertex's w (DecodeShapeMesh).
                const Value& pw = e.items[V::kPositionW];
                const float b[3] = { pw.present ? F32Of(pw) : 0.f, SNorm(U8Of(e.items[V::kNormalW])),
                                     SNorm(U8Of(e.items[V::kTangentW])) };
                for (int k = 0; k < 3; ++k) m.bitangents[i*3+k] = b[k];
                m.tangents[i*4+3] = Handedness(&m.normals[i*3], &m.tangents[i*4], b);
            } else {
                m.tangents[i*4+3] = 1.0f;
            }
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

// ── Tangent space ─────────────────────────────────────────────────────────────
namespace {

// [-1, 1] -> the byte SNorm decodes back to the same value.
uint8_t SNormByte(float f) {
    const float x = std::round((std::clamp(f, -1.0f, 1.0f) + 1.0f) * 127.5f);
    return static_cast<uint8_t>(std::clamp(x, 0.0f, 255.0f));
}

void Normalize(float* v) {
    const float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-20f) for (int k = 0; k < 3; ++k) v[k] /= l;
}

// `v` made orthogonal to unit `n` and unit length; false when nothing is left.
bool Orthonormal(float* v, const float* n) {
    const float d = v[0]*n[0] + v[1]*n[1] + v[2]*n[2];
    for (int k = 0; k < 3; ++k) v[k] -= d * n[k];
    const float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l < 1e-12f) return false;
    for (int k = 0; k < 3; ++k) v[k] /= l;
    return true;
}

// The bitangent to write for vertex i: the mesh's, or W (N x T).
void BitangentOf(const DecodedMesh& m, size_t i, float* bt) {
    const float* t = &m.tangents[i*4];
    if (m.bitangents.size() == size_t(m.numVertices) * 3) {
        for (int k = 0; k < 3; ++k) bt[k] = m.bitangents[i*3+k];
        return;
    }
    const float* n = &m.normals[i*3];
    const float c[3] = { n[1]*t[2] - n[2]*t[1], n[2]*t[0] - n[0]*t[2], n[0]*t[1] - n[1]*t[0] };
    for (int k = 0; k < 3; ++k) bt[k] = c[k] * (t[3] < 0.f ? -1.f : 1.f);
}

// Writes the bitangents' x into a BSDynamicTriShape's dynamic vertices (Vec4 w).
bool EncodeDynamicW(Block& b, const DecodedMesh& m) {
    const RelPath array = RelPath{}.Child(view::BSDynamicTriShape::kDynamicData);
    Value arr;
    if (!b.Visit(array, [&](const schema::TypeDef&, const Value& v) { arr = v; })) return false;
    if (arr.items.size() < m.numVertices) return false;
    bool changed = false;
    for (size_t i = 0; i < m.numVertices; ++i) {
        float bt[3];
        BitangentOf(m, i, bt);
        Value& w = arr.items[i].items[3];
        const uint64_t bits = std::bit_cast<uint32_t>(bt[0]);
        if (w.bits != bits) { w.bits = bits; changed = true; }
    }
    return !changed || b.StoreValue(array, std::move(arr));
}

// Writes the frame into the BSVertex array at `array` in `b` (nv vertices).
bool EncodeVertices(Block& b, const RelPath& array, const DecodedMesh& m) {
    const size_t nv = m.numVertices;
    bool ok = true, changed = false;
    Value arr;
    if (!b.Visit(array, [&](const schema::TypeDef&, const Value& v) { arr = v; })) return false;
    if (arr.items.size() < nv) return false;
    using V = view::BSVertex;
    for (size_t i = 0; i < nv && ok; ++i) {
        Value& e = arr.items[i];
        if (!e.items[V::kTangent].present || !e.items[V::kNormal].present) {
            ok = false;
            break;
        }
        const float* t = &m.tangents[i*4];
        float bt[3];
        BitangentOf(m, i, bt);
        auto put = [&](Value& v, uint64_t bits) {
            if (v.bits != bits) { v.bits = bits; changed = true; }
        };
        for (int k = 0; k < 3; ++k) put(e.items[V::kTangent].items[k], SNormByte(t[k]));
        if (e.items[V::kPositionW].present) put(e.items[V::kPositionW], std::bit_cast<uint32_t>(bt[0]));
        put(e.items[V::kNormalW], SNormByte(bt[1]));
        put(e.items[V::kTangentW], SNormByte(bt[2]));
    }
    if (!ok) return false;
    return !changed || b.StoreValue(array, std::move(arr));
}

}  // namespace

void ComputeTangentSpace(DecodedMesh& m) {
    const size_t nv = m.numVertices;
    if (m.positions.size() != nv * 3 || m.normals.size() != nv * 3 || m.uvs.size() != nv * 2) return;
    std::vector<double> su(nv * 3, 0.0), sv(nv * 3, 0.0);
    for (size_t t = 0; t + 2 < m.triangles.size(); t += 3) {
        const size_t i[3] = { m.triangles[t], m.triangles[t + 1], m.triangles[t + 2] };
        if (i[0] >= nv || i[1] >= nv || i[2] >= nv) continue;
        double e1[3], e2[3];
        for (int k = 0; k < 3; ++k) {
            e1[k] = double(m.positions[i[1]*3+k]) - m.positions[i[0]*3+k];
            e2[k] = double(m.positions[i[2]*3+k]) - m.positions[i[0]*3+k];
        }
        const double du1 = double(m.uvs[i[1]*2]) - m.uvs[i[0]*2], dv1 = double(m.uvs[i[1]*2+1]) - m.uvs[i[0]*2+1];
        const double du2 = double(m.uvs[i[2]*2]) - m.uvs[i[0]*2], dv2 = double(m.uvs[i[2]*2+1]) - m.uvs[i[0]*2+1];
        const double det = du1 * dv2 - du2 * dv1;
        if (std::abs(det) < 1e-20) continue;
        // dP/dU and dP/dV times |det|: each triangle weighs by its UV area.
        const double s = det > 0 ? 1.0 : -1.0;
        for (size_t v : i)
            for (int k = 0; k < 3; ++k) {
                su[v*3+k] += (e1[k] * dv2 - e2[k] * dv1) * s;
                sv[v*3+k] += (e2[k] * du1 - e1[k] * du2) * s;
            }
    }
    m.tangents.assign(nv * 4, 0.f);
    m.bitangents.assign(nv * 3, 0.f);
    for (size_t v = 0; v < nv; ++v) {
        float n[3] = { m.normals[v*3], m.normals[v*3+1], m.normals[v*3+2] };
        Normalize(n);
        // The NIF's tangent follows +dP/dV, its bitangent +dP/dU.
        float t[3] = { float(sv[v*3]), float(sv[v*3+1]), float(sv[v*3+2]) };
        float b[3] = { float(su[v*3]), float(su[v*3+1]), float(su[v*3+2]) };
        const bool tOk = Orthonormal(t, n), bOk = Orthonormal(b, n);
        if (!tOk && !bOk) {   // no UV gradient: any frame square to the normal
            const float a[3] = { std::abs(n[0]) < 0.9f ? 1.f : 0.f, std::abs(n[0]) < 0.9f ? 0.f : 1.f, 0.f };
            for (int k = 0; k < 3; ++k) t[k] = a[k];
            Orthonormal(t, n);
        }
        if (!tOk || !bOk) {   // one missing: complete the frame from the other
            const float* have = tOk || !bOk ? t : b;
            float* fill = tOk || !bOk ? b : t;
            const float c[3] = { n[1]*have[2] - n[2]*have[1], n[2]*have[0] - n[0]*have[2], n[0]*have[1] - n[1]*have[0] };
            const float sign = have == t ? 1.f : -1.f;   // B = N x T, T = B x N
            for (int k = 0; k < 3; ++k) fill[k] = c[k] * sign;
        }
        for (int k = 0; k < 3; ++k) { m.tangents[v*4+k] = t[k]; m.bitangents[v*3+k] = b[k]; }
        m.tangents[v*4+3] = Handedness(n, t, b);
    }
}

bool EncodeTangentSpace(NifFile& nif, Block& shapeBlock, const DecodedMesh& m) {
    auto shape = view::BSTriShape::From(shapeBlock);
    if (!shape) return false;
    const size_t nv = m.numVertices;
    if (!nv || m.tangents.size() != nv * 4 || m.normals.size() != nv * 3) return false;
    if (!m.bitangents.empty() && m.bitangents.size() != nv * 3) return false;
    const bool dynamic = view::BSDynamicTriShape::From(shapeBlock).has_value();
    // Where DecodeShapeMesh reads attributes from: the shape's own vertices, or
    // (skinned with none, or a BSDynamicTriShape) its skin partition's.
    if (!dynamic && shape->NumVertices() == nv && shapeBlock.Count(RelPath{}.Child(view::BSTriShape::kVertices)) >= nv)
        return EncodeVertices(shapeBlock, RelPath{}.Child(view::BSTriShape::kVertices), m);
    auto skin = nif.Get(shape->Skin());
    if (!skin) return false;
    auto part = nif.Get(skin->SkinPartition());
    if (!part) return false;
    // Both arrays checked before either is written.
    const RelPath vertexData = RelPath{}.Child(view::NiSkinPartition::kVertexData);
    if (part->block().Count(vertexData) < nv) return false;
    if (dynamic && shapeBlock.Count(RelPath{}.Child(view::BSDynamicTriShape::kDynamicData)) < nv) return false;
    if (!EncodeVertices(part->block(), vertexData, m)) return false;
    return !dynamic || EncodeDynamicW(shapeBlock, m);
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
        const bool frame = m.bitangents.size() == nv * 3 && m.tangents.size() == nv * 4 && m.normals.size() == nv * 3;
        shapeBlock.ForEachElement(RelPath{}.Child(view::BSDynamicTriShape::kDynamicData),
                                  [&](size_t i, const schema::TypeDef&, const Value& v) {
            if (i >= nv) return;
            for (int k = 0; k < 3; ++k) m.positions[i*3+k] = F32Of(v.items[k]);
            if (frame) {   // the bitangent's x
                m.bitangents[i*3] = F32Of(v.items[3]);
                m.tangents[i*4+3] = Handedness(&m.normals[i*3], &m.tangents[i*4], &m.bitangents[i*3]);
            }
        });
        return m;
    }
    DecodedMesh m = DecodeMesh(*shape);
    if (m.numVertices == 0 && part) m = DecodeMesh(*part);
    return m;
}

}  // namespace niffer
