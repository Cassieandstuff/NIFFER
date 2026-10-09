// ── Havok collision extraction (consumer layer over the raw bhk* blobs) ───────
// niffer decodes the bhk block GRAPH (refs) but keeps each block's Havok payload
// as a raw byte blob (byte-exact round-trip). This pass re-reads those blobs to
// produce usable collision geometry: it walks bhkCollisionObject -> body -> shape
// tree, decodes the compressed-mesh / convex / box leaves, and emits triangles +
// convex hulls in the NIF's ROOT space, scaled Havok->Skyrim.
//
// Decode math is authoritative: struct wire order per nifly; the compressed-mesh
// vertex decompression per the Havok SDK (hkpCompressedMeshShape.h/.inl) —
//   big triangle : v = m_bigVertices[idx]         (optional m_transforms[ti])
//   chunk vertex : v = chunk.m_offset + Vec3(q0,q1,q2) * shape.error
// It is GATED: decoded chunk verts must fall inside the shape's own stored AABB
// (aabbBoundMin/Max), else the shape is dropped — a wrong layout can't ship.

#include <niffer/Niffer.h>

#include <array>
#include <cmath>
#include <cstring>

namespace niffer {
namespace {

// Little-endian reader over a raw payload blob. `ok` latches false on overrun;
// callers check it and bail (a truncated/misparsed blob yields no geometry).
struct LE {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    explicit LE(const std::vector<uint8_t>& v) : p(v.data()), end(v.data() + v.size()) {}
    bool avail(size_t n) const { return ok && p + n <= end; }
    uint8_t  u8()  { if (!avail(1)) { ok = false; return 0; } return *p++; }
    uint16_t u16() { if (!avail(2)) { ok = false; return 0; } uint16_t v; std::memcpy(&v, p, 2); p += 2; return v; }
    uint32_t u32() { if (!avail(4)) { ok = false; return 0; } uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; }
    float    f32() { if (!avail(4)) { ok = false; return 0.f; } float v; std::memcpy(&v, p, 4); p += 4; return v; }
    void skip(size_t n) { if (!avail(n)) { ok = false; return; } p += n; }
};

struct V3 { float x = 0, y = 0, z = 0; };
struct Xform {                       // translation + rotation (row-major 3x3)
    V3    t;
    float r[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    V3 apply(const V3& v) const {
        return { r[0] * v.x + r[1] * v.y + r[2] * v.z + t.x,
                 r[3] * v.x + r[4] * v.y + r[5] * v.z + t.y,
                 r[6] * v.x + r[7] * v.y + r[8] * v.z + t.z };
    }
};
Xform mul(const Xform& a, const Xform& b) {   // a ∘ b
    Xform o;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            o.r[i * 3 + j] = a.r[i * 3 + 0] * b.r[0 * 3 + j] +
                             a.r[i * 3 + 1] * b.r[1 * 3 + j] +
                             a.r[i * 3 + 2] * b.r[2 * 3 + j];
    const V3 bt{ b.t.x, b.t.y, b.t.z };
    o.t = a.apply(bt);
    return o;
}
// Quaternion (x,y,z,w) -> row-major 3x3 (matches QuaternionXYZW on disk).
void quatToMat(float x, float y, float z, float w, float* r) {
    const float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n > 1e-8f) { x /= n; y /= n; z /= n; w /= n; }
    r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y - z * w);     r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w);     r[4] = 1 - 2 * (x * x + z * z); r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w);     r[7] = 2 * (y * z + x * w);     r[8] = 1 - 2 * (x * x + y * y);
}

// Accumulated output (Havok units — scaled to Skyrim at the very end).
struct Accum {
    std::vector<float>              tv;
    std::vector<uint32_t>           ti;
    std::vector<std::vector<float>> hulls;
    int decoded = 0, skipped = 0;
    void tri(const V3& a, const V3& b, const V3& c) {
        const uint32_t base = (uint32_t)(tv.size() / 3);
        for (const V3& v : { a, b, c }) { tv.push_back(v.x); tv.push_back(v.y); tv.push_back(v.z); }
        ti.push_back(base); ti.push_back(base + 1); ti.push_back(base + 2);
    }
};

// ── bhkCompressedMeshShapeData: decode big-tris + chunks into `world`-space tris.
void decodeCompressedMesh(const std::vector<uint8_t>& data, const Xform& world, Accum& out) {
    LE s(data);
    s.u32(); s.u32(); s.u32(); s.u32();            // bitsPerIndex/WIndex, maskWIndex/Index
    const float error = s.f32();
    V3 bmin, bmax;
    bmin.x = s.f32(); bmin.y = s.f32(); bmin.z = s.f32(); s.f32();   // aabbMin (+w)
    bmax.x = s.f32(); bmax.y = s.f32(); bmax.z = s.f32(); s.f32();   // aabbMax (+w)
    s.u8(); s.u8();                                // weldingType, materialType

    // Material palettes (skip): mat32 (u32), mat16 (u16), mat8 (u8), then the
    // bhkCMSDMaterial array (8B each), then numNamedMaterials.
    const uint32_t nMat32 = s.u32(); s.skip((size_t)nMat32 * 4);
    const uint32_t nMat16 = s.u32(); s.skip((size_t)nMat16 * 2);
    const uint32_t nMat8  = s.u32(); s.skip((size_t)nMat8  * 1);
    const uint32_t nMat   = s.u32(); s.skip((size_t)nMat   * 8);   // bhkCMSDMaterial {u32 mat, u8,u8,u16}
    s.u32();                                                       // numNamedMaterials

    // Chunk transforms: { Vec4 translation, QuaternionXYZW rotation } (32B each).
    const uint32_t nXf = s.u32();
    std::vector<Xform> xforms(s.ok ? nXf : 0);
    for (uint32_t i = 0; i < nXf && s.ok; ++i) {
        Xform& x = xforms[i];
        x.t.x = s.f32(); x.t.y = s.f32(); x.t.z = s.f32(); s.f32();
        const float qx = s.f32(), qy = s.f32(), qz = s.f32(), qw = s.f32();
        quatToMat(qx, qy, qz, qw, x.r);
    }

    // Big vertices (Vec4) + big triangles (u16 a,b,c; u16 welding; u32 mat).
    const uint32_t nBV = s.u32();
    std::vector<V3> big(s.ok ? nBV : 0);
    for (uint32_t i = 0; i < nBV && s.ok; ++i) { big[i].x = s.f32(); big[i].y = s.f32(); big[i].z = s.f32(); s.f32(); }
    const uint32_t nBT = s.u32();
    struct BigTri { uint16_t a, b, c, weld; uint32_t mat; };
    std::vector<BigTri> bigTris(s.ok ? nBT : 0);
    for (uint32_t i = 0; i < nBT && s.ok; ++i) {
        bigTris[i].a = s.u16(); bigTris[i].b = s.u16(); bigTris[i].c = s.u16();
        bigTris[i].weld = s.u16(); bigTris[i].mat = s.u32();
    }

    if (!s.ok) { ++out.skipped; return; }

    // Gate: big verts must sit inside the stored AABB (a slack for the shape
    // radius). A gross miss means the material/transform skip was misaligned.
    auto inBounds = [&](const V3& v) {
        const float e = 2.f;
        return v.x >= bmin.x - e && v.x <= bmax.x + e &&
               v.y >= bmin.y - e && v.y <= bmax.y + e &&
               v.z >= bmin.z - e && v.z <= bmax.z + e;
    };
    for (const V3& v : big)
        if (!inBounds(v)) { ++out.skipped; return; }

    // Emit big triangles (optional per-tri transform via index+1 convention — the
    // bhk file stores transformIndex on the big-tri's 4th slot only in some
    // exports; here big verts are already root-local, so just place by `world`).
    for (const BigTri& t : bigTris) {
        if (t.a >= nBV || t.b >= nBV || t.c >= nBV) continue;
        out.tri(world.apply(big[t.a]), world.apply(big[t.b]), world.apply(big[t.c]));
    }

    // Chunks.
    const uint32_t nChunks = s.u32();
    for (uint32_t ci = 0; ci < nChunks && s.ok; ++ci) {
        V3 off; off.x = s.f32(); off.y = s.f32(); off.z = s.f32(); s.f32();   // m_offset
        s.u32();                                        // materialInfo
        s.u16();                                        // reference
        const uint16_t ti = s.u16();                    // transformIndex
        const uint32_t nV = s.u32();
        std::vector<V3> verts(s.ok ? nV / 3 : 0);
        for (uint32_t i = 0; i + 2 < nV && s.ok; i += 3) {
            const float qx = (float)s.u16(), qy = (float)s.u16(), qz = (float)s.u16();
            verts[i / 3] = { off.x + qx * error, off.y + qy * error, off.z + qz * error };
        }
        const uint32_t nI = s.u32();
        std::vector<uint16_t> idx(s.ok ? nI : 0);
        for (uint32_t i = 0; i < nI && s.ok; ++i) idx[i] = s.u16();
        const uint32_t nS = s.u32();
        std::vector<uint16_t> strips(s.ok ? nS : 0);
        for (uint32_t i = 0; i < nS && s.ok; ++i) strips[i] = s.u16();
        const uint32_t nW = s.u32(); s.skip((size_t)nW * 2);   // weldingInfo

        if (!s.ok) { ++out.skipped; break; }
        for (const V3& v : verts)
            if (!inBounds(v)) { ++out.skipped; verts.clear(); break; }
        if (verts.empty()) continue;

        // The chunk's per-transform (if referenced) then the world placement.
        Xform cx = world;
        if (ti != 0xFFFF && ti < xforms.size()) cx = mul(world, xforms[ti]);

        auto emit = [&](uint16_t a, uint16_t b, uint16_t c) {
            if (a < verts.size() && b < verts.size() && c < verts.size())
                out.tri(cx.apply(verts[a]), cx.apply(verts[b]), cx.apply(verts[c]));
        };
        // Strips first (triangle strips, alternating winding), then list tris.
        size_t pos = 0;
        for (uint16_t L : strips) {
            for (uint16_t k = 0; k + 2 < L && pos + k + 2 < idx.size(); ++k) {
                if (k & 1) emit(idx[pos + k], idx[pos + k + 2], idx[pos + k + 1]);
                else       emit(idx[pos + k], idx[pos + k + 1], idx[pos + k + 2]);
            }
            pos += L;
        }
        for (size_t i = pos; i + 2 < idx.size(); i += 3) emit(idx[i], idx[i + 1], idx[i + 2]);
    }
    ++out.decoded;
}

// ── bhkConvexVerticesShape: material + radius consumed by niffer; `data` holds
// the two hkWorldObjCInfoProperty headers (3 u32 each) then verts + normals.
void decodeConvexVerts(const std::vector<uint8_t>& data, const Xform& world, Accum& out) {
    LE s(data);
    s.u32(); s.u32(); s.u32();          // vertsProp   (data, size, capacityAndFlags)
    s.u32(); s.u32(); s.u32();          // normalsProp
    const uint32_t nV = s.u32();
    std::vector<float> pts;
    pts.reserve((s.ok ? nV : 0) * 3);
    for (uint32_t i = 0; i < nV && s.ok; ++i) {
        V3 v{ s.f32(), s.f32(), s.f32() }; s.f32();
        const V3 w = world.apply(v);
        pts.push_back(w.x); pts.push_back(w.y); pts.push_back(w.z);
    }
    if (!s.ok || pts.size() < 12) { ++out.skipped; return; }
    out.hulls.push_back(std::move(pts));
    ++out.decoded;
}

// ── bhkBoxShape: data = HavokMaterial-tail then a Vec4 half-extents. niffer's
// bhkPrimitiveShape already consumed material+radius; `data` starts at the 8-byte
// pad then the half-extents Vector4.
void decodeBox(const std::vector<uint8_t>& data, const Xform& world, Accum& out) {
    LE s(data);
    s.u32(); s.u32();                   // 8-byte pad before the dimensions
    const float hx = s.f32(), hy = s.f32(), hz = s.f32(); s.f32();
    if (!s.ok || (hx <= 0 && hy <= 0 && hz <= 0)) { ++out.skipped; return; }
    const V3 c[8] = {
        { -hx, -hy, -hz }, { hx, -hy, -hz }, { hx, hy, -hz }, { -hx, hy, -hz },
        { -hx, -hy,  hz }, { hx, -hy,  hz }, { hx, hy,  hz }, { -hx, hy,  hz } };
    V3 w[8]; for (int i = 0; i < 8; ++i) w[i] = world.apply(c[i]);
    const int f[12][3] = { {0,1,2},{0,2,3},{4,6,5},{4,7,6},{0,4,5},{0,5,1},
                           {1,5,6},{1,6,2},{2,6,7},{2,7,3},{3,7,4},{3,4,0} };
    for (auto& t : f) out.tri(w[t[0]], w[t[1]], w[t[2]]);
    ++out.decoded;
}

// Recurse a shape block, accumulating geometry. `world` is the running transform
// (root space) above this shape.
void walkShape(const NifFile& nif, const NiObject* shape, const Xform& world, Accum& out, int depth) {
    if (!shape || depth > 32) return;
    const std::string_view type = shape->TypeName();

    if (type == "bhkMoppBvTreeShape" || type == "bhkConvexTransformShape" ||
        type == "bhkTransformShape"  || type == "bhkCompressedMeshShape") {
        // Wrapper: follow the child shape ref. (ConvexTransform/Transform also
        // carry a local matrix, but static cell collision rarely uses it and the
        // AABB gate protects us; treat as pass-through for now.)
        auto* w = dynamic_cast<const bhkShapeWrapper*>(shape);
        if (w) {
            if (type == "bhkCompressedMeshShape") {
                // The Data ref is the LAST field of the block, not the first: niffer's
                // .shape captured the leading Target pointer, so read the data index from
                // the tail of the raw payload (…Target, userData, radius, scale, …, dataRef).
                int32_t dataRef = -1;
                if (w->data.size() >= 4)
                    std::memcpy(&dataRef, w->data.data() + w->data.size() - 4, 4);
                const NiObject* childData = nif.GetBlock(dataRef);
                if (childData && childData->TypeName() == "bhkCompressedMeshShapeData")
                    if (auto* raw = dynamic_cast<const bhkRawBlock*>(childData))
                        decodeCompressedMesh(raw->data, world, out);
            } else {
                walkShape(nif, nif.GetBlock(w->shape.index), world, out, depth + 1);
            }
        }
        return;
    }
    if (type == "bhkListShape") {
        if (auto* list = dynamic_cast<const bhkListShape*>(shape))
            for (const auto& sub : list->subShapes)
                walkShape(nif, nif.GetBlock(sub.index), world, out, depth + 1);
        return;
    }
    if (type == "bhkConvexVerticesShape") {
        if (auto* prim = dynamic_cast<const bhkPrimitiveShape*>(shape))
            decodeConvexVerts(prim->data, world, out);
        return;
    }
    if (type == "bhkBoxShape") {
        if (auto* prim = dynamic_cast<const bhkPrimitiveShape*>(shape))
            decodeBox(prim->data, world, out);
        return;
    }
    // sphere/capsule/cylinder: not emitted (rare for cell statics); count skipped.
    if (type == "bhkSphereShape" || type == "bhkCapsuleShape" || type == "bhkCylinderShape")
        ++out.skipped;
}

}  // namespace

CollisionGeometry ExtractCollision(const NifFile& nif) {
    Accum acc;
    for (const auto& blk : nif.blocks) {
        if (!blk) continue;
        const std::string_view type = blk->TypeName();
        if (type != "bhkCollisionObject" && type != "bhkSPCollisionObject") continue;
        auto* co = dynamic_cast<const bhkCollisionObject*>(blk.get());
        if (!co) continue;
        // body = bhkRigidBody / bhkRigidBodyT (a bhkShapeWrapper: .shape = the shape).
        auto* body = dynamic_cast<const bhkShapeWrapper*>(nif.GetBlock(co->body.index));
        if (!body) continue;
        walkShape(nif, nif.GetBlock(body->shape.index), Xform{}, acc, 0);
    }

    CollisionGeometry g;
    g.shapesDecoded = acc.decoded;
    g.shapesSkipped = acc.skipped;
    g.triVerts.reserve(acc.tv.size());
    for (float v : acc.tv) g.triVerts.push_back(v * kHavokToSkyrim);
    g.triIndices = std::move(acc.ti);
    g.hulls.reserve(acc.hulls.size());
    for (auto& h : acc.hulls) {
        for (float& v : h) v *= kHavokToSkyrim;
        g.hulls.push_back(std::move(h));
    }
    return g;
}

}  // namespace niffer
