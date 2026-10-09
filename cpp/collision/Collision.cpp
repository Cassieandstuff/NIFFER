// ── ExtractCollision — Havok collision geometry through the schema ───────────
// Clean-room: every layout read here is a schema descriptor under
// schema/blocks/collision/, measured on the SSE corpus; every convention
// (quantized-vertex decode, strip order, transform conventions, Havok scale) is
// the one the corpus measurement selected. Evidence: docs/collision-provenance.md.
//
// Not on the round-trip path: the NifFile is serialized and re-read as a schema
// Nif, and geometry is read through the generated views.
#include "Collision.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>

namespace niffer {
namespace collision {

V3 Apply(const Xform& x, const V3& p) {
    V3 o{};
    for (int r = 0; r < 3; ++r)
        o[r] = x.s * (x.R.m[r][0] * p[0] + x.R.m[r][1] * p[1] + x.R.m[r][2] * p[2]) + x.t[r];
    return o;
}

static M3 Mul(const M3& a, const M3& b) {
    M3 o;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
    return o;
}

Xform Compose(const Xform& parent, const Xform& child) {
    Xform o;
    o.R = Mul(parent.R, child.R);
    o.s = parent.s * child.s;
    o.t = Apply(parent, child.t);
    return o;
}

namespace {

using view::Vec4;

std::array<float, 4> Read4(const Vec4& v) { return { v.X(), v.Y(), v.Z(), v.W() }; }
V3 Read3(const view::Vec3& v) { return { v.X(), v.Y(), v.Z() }; }

bool Finite(const V3& p) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }

// NiAVObject's transform (AvTransform: translation, Matrix33, scale).
Xform NodeLocal(const view::NiAVObject& o, const Options& opt) {
    Xform x;
    const auto tr = o.Transform();
    x.t = Read3(tr.Translation());
    x.s = tr.Scale();
    const std::vector<float> m = tr.Rotation().M();
    if (m.size() == 9)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                x.R.m[r][c] = opt.niRowMajor ? m[static_cast<size_t>(3 * r + c)] : m[static_cast<size_t>(3 * c + r)];
    return x;
}

// HavokTransform: three orthonormal basis Vec4s + a translation Vec4.
Xform HavokLocal(const view::HavokTransform& h, const Options& opt) {
    Xform x;
    const auto basis = h.Rotation();
    if (basis.size() == 3) {
        for (int i = 0; i < 3; ++i) {
            const auto v = Read4(basis[static_cast<size_t>(i)]);
            for (int k = 0; k < 3; ++k) {
                if (opt.havokColumns) x.R.m[k][i] = v[static_cast<size_t>(k)];
                else                  x.R.m[i][k] = v[static_cast<size_t>(k)];
            }
        }
    }
    const auto t = Read4(h.Translation());
    x.t = { t[0], t[1], t[2] };
    return x;
}

// Unit quaternion (x, y, z, w) -> rotation matrix.
M3 QuatMatrix(float qx, float qy, float qz, float qw) {
    M3 o;
    o.m[0][0] = 1 - 2 * (qy * qy + qz * qz); o.m[0][1] = 2 * (qx * qy - qz * qw);     o.m[0][2] = 2 * (qx * qz + qy * qw);
    o.m[1][0] = 2 * (qx * qy + qz * qw);     o.m[1][1] = 1 - 2 * (qx * qx + qz * qz); o.m[1][2] = 2 * (qy * qz - qx * qw);
    o.m[2][0] = 2 * (qx * qz - qy * qw);     o.m[2][1] = 2 * (qy * qz + qx * qw);     o.m[2][2] = 1 - 2 * (qx * qx + qy * qy);
    return o;
}

// 26 unit directions (faces, edges and corners of a cube), for sphere-like hulls.
const std::vector<V3>& SphereDirs() {
    static const std::vector<V3> dirs = [] {
        std::vector<V3> d;
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z) {
                    if (!x && !y && !z) continue;
                    const float n = std::sqrt(static_cast<float>(x * x + y * y + z * z));
                    d.push_back({ static_cast<float>(x) / n, static_cast<float>(y) / n, static_cast<float>(z) / n });
                }
        return d;
    }();
    return dirs;
}

class Extractor {
public:
    Extractor(Nif& nif, const Options& opt, Diagnostics* diag) : nif_(nif), opt_(opt), diag_(diag) {}

    CollisionGeometry Run();

private:
    // `toRoot` maps body-space Havok units to root-space Skyrim units.
    void Shape(int32_t index, const Xform& toRoot, int depth);
    void CompressedMesh(Block& shapeBlock, const Xform& toRoot);
    void Hull(const std::vector<V3>& pts, const Xform& toRoot, const char* cls);
    void Skip(const std::string& cls, const char* why) {
        ++out_.shapesSkipped;
        if (diag_) ++diag_->skipped[cls + ": " + why];
    }
    void Decoded(const std::string& cls) {
        ++out_.shapesDecoded;
        if (diag_) ++diag_->decoded[cls];
    }

    Nif&              nif_;
    const Options&    opt_;
    Diagnostics*      diag_;
    CollisionGeometry out_;
};

void Extractor::Hull(const std::vector<V3>& pts, const Xform& toRoot, const char* cls) {
    std::vector<float> h;
    h.reserve(pts.size() * 3);
    for (const V3& p : pts) {
        if (!Finite(p)) { Skip(cls, "non-finite point"); return; }
        const V3 q = Apply(toRoot, p);
        h.insert(h.end(), q.begin(), q.end());
    }
    if (h.empty()) { Skip(cls, "no points"); return; }
    out_.hulls.push_back(std::move(h));
    Decoded(cls);
}

void Extractor::CompressedMesh(Block& shapeBlock, const Xform& toRoot) {
    const char* cls = "bhkCompressedMeshShape";
    auto cms = view::bhkCompressedMeshShape::From(shapeBlock);
    Block* db = nif_.GetBlock(cms->Data().index);
    std::optional<view::bhkCompressedMeshShapeData> data;
    if (db) data = view::bhkCompressedMeshShapeData::From(*db);
    if (!data) { Skip(cls, "no data block"); return; }

    const float q = data->Quantization();
    const auto lo = Read4(data->BoundsMin());
    const auto hi = Read4(data->BoundsMax());
    // A vertex may sit at most this far outside the stored bounds (measured
    // worst case over the corpus: 6.1e-5, under one quantization step).
    const float tol = 2.f * std::max(q, 1e-4f);
    auto inBounds = [&](const V3& p) {
        for (int k = 0; k < 3; ++k)
            if (!(p[static_cast<size_t>(k)] >= lo[static_cast<size_t>(k)] - tol &&
                  p[static_cast<size_t>(k)] <= hi[static_cast<size_t>(k)] + tol)) return false;
        return true;
    };

    // Identity check for the transform table (every entry is the identity in the
    // corpus; anything else isn't understood, so such chunks are refused).
    std::vector<bool> identity;
    for (const auto& t : data->Transforms()) {
        const auto tr = Read4(t.Translation());
        const auto rq = Read4(t.Rotation());
        identity.push_back(tr[0] == 0.f && tr[1] == 0.f && tr[2] == 0.f &&
                           rq[0] == 0.f && rq[1] == 0.f && rq[2] == 0.f && std::fabs(rq[3] - 1.f) < 1e-5f);
    }

    std::vector<V3>       verts;
    std::vector<uint32_t> tris;
    int checked = 0, outside = 0;

    for (const auto& chunk : data->Chunks()) {
        const auto off = Read4(chunk.Offset());
        const uint16_t ti = chunk.TransformIndex();
        if (ti != 0xFFFF && (ti >= identity.size() || !identity[ti])) {
            Skip(cls, "chunk with a non-identity transform");
            return;
        }
        const std::vector<uint16_t> qv = chunk.Vertices();
        const std::vector<uint16_t> idx = chunk.Indices();
        const std::vector<uint16_t> strips = chunk.StripLengths();
        if (qv.size() % 3 != 0) { Skip(cls, "vertex count not a multiple of 3"); return; }
        const size_t nv = qv.size() / 3;
        const auto first = static_cast<uint32_t>(verts.size());
        for (size_t v = 0; v < nv; ++v) {
            V3 p{ off[0] + static_cast<float>(qv[3 * v + 0]) * q,
                  off[1] + static_cast<float>(qv[3 * v + 1]) * q,
                  off[2] + static_cast<float>(qv[3 * v + 2]) * q };
            ++checked;
            if (!inBounds(p)) ++outside;
            verts.push_back(p);
        }
        for (uint16_t i : idx)
            if (i >= nv) { Skip(cls, "chunk index out of range"); return; }
        // Strips first (alternating winding), then a plain triangle list.
        size_t pos = 0;
        for (uint16_t len : strips) {
            if (pos + len > idx.size()) { Skip(cls, "strip lengths exceed the index count"); return; }
            for (size_t k = 0; k + 2 < len; ++k) {
                uint32_t a = idx[pos + k], b = idx[pos + k + 1], c = idx[pos + k + 2];
                if (k & 1) std::swap(a, b);
                if (a == b || b == c || a == c) continue;
                tris.insert(tris.end(), { first + a, first + b, first + c });
            }
            pos += len;
        }
        if ((idx.size() - pos) % 3 != 0) { Skip(cls, "triangle list not a multiple of 3"); return; }
        for (; pos + 2 < idx.size(); pos += 3)
            tris.insert(tris.end(), { first + idx[pos], first + idx[pos + 1], first + idx[pos + 2] });
    }

    // Big (unquantized) vertices and their triangles.
    const auto firstBig = static_cast<uint32_t>(verts.size());
    const auto big = data->BigVertices();
    for (const auto& v : big) {
        const auto p4 = Read4(v);
        V3 p{ p4[0], p4[1], p4[2] };
        ++checked;
        if (!inBounds(p)) ++outside;
        verts.push_back(p);
    }
    for (const auto& t : data->BigTriangles()) {
        const auto tri = t.Triangle();
        const uint16_t a = tri.A(), b = tri.B(), c = tri.C();
        if (a >= big.size() || b >= big.size() || c >= big.size()) { Skip(cls, "big triangle index out of range"); return; }
        tris.insert(tris.end(), { firstBig + a, firstBig + b, firstBig + c });
    }

    if (diag_) { diag_->cmsVerticesChecked += checked; diag_->cmsVerticesOutside += outside; }
    if (outside) { Skip(cls, "vertices outside the stored bounds"); return; }
    if (tris.empty()) { Skip(cls, "no triangles"); return; }

    const auto baseIndex = static_cast<uint32_t>(out_.triVerts.size() / 3);
    out_.triVerts.reserve(out_.triVerts.size() + verts.size() * 3);
    for (const V3& p : verts) {
        const V3 r = Apply(toRoot, p);
        out_.triVerts.insert(out_.triVerts.end(), r.begin(), r.end());
    }
    out_.triIndices.reserve(out_.triIndices.size() + tris.size());
    for (uint32_t i : tris) out_.triIndices.push_back(baseIndex + i);
    Decoded(cls);
}

void Extractor::Shape(int32_t index, const Xform& toRoot, int depth) {
    Block* b = nif_.GetBlock(index);
    if (!b) { Skip("(null)", "missing shape"); return; }
    const std::string cls(b->TypeName());
    if (depth > 32) { Skip(cls, "shape nesting too deep"); return; }
    if (!b->Modeled()) { Skip(cls, "block not modeled"); return; }

    if (auto mopp = view::bhkMoppBvTreeShape::From(*b)) {
        const int32_t child = mopp->Shape().index;
        // Consistency: a compressed mesh's bounds must lie inside the MOPP grid.
        if (diag_) {
            if (Block* cb = nif_.GetBlock(child)) {
                if (auto cms = view::bhkCompressedMeshShape::From(*cb)) {
                    if (Block* db = nif_.GetBlock(cms->Data().index)) {
                        if (auto data = view::bhkCompressedMeshShapeData::From(*db)) {
                            const V3 o = Read3(mopp->Origin());
                            const float ext = mopp->Scale() > 0 ? 254.f * 65536.f / mopp->Scale() : 0.f;
                            const auto lo = Read4(data->BoundsMin()), hi = Read4(data->BoundsMax());
                            bool in = true;
                            for (int k = 0; k < 3; ++k) {
                                const float slack = 1e-3f * std::max(1.f, ext);
                                in = in && lo[static_cast<size_t>(k)] >= o[static_cast<size_t>(k)] - slack &&
                                     hi[static_cast<size_t>(k)] <= o[static_cast<size_t>(k)] + ext + slack;
                            }
                            ++diag_->moppChecked;
                            if (!in) ++diag_->moppOutside;
                        }
                    }
                }
            }
        }
        Shape(child, toRoot, depth + 1);
        return;
    }
    if (auto list = view::bhkListShape::From(*b)) {
        for (const auto& sub : list->SubShapes()) Shape(sub.index, toRoot, depth + 1);
        return;
    }
    if (auto cts = view::bhkConvexTransformShape::From(*b)) {
        Shape(cts->Shape().index, Compose(toRoot, HavokLocal(cts->Transform(), opt_)), depth + 1);
        return;
    }
    if (auto ts = view::bhkTransformShape::From(*b)) {
        Shape(ts->Shape().index, Compose(toRoot, HavokLocal(ts->Transform(), opt_)), depth + 1);
        return;
    }
    if (view::bhkCompressedMeshShape::From(*b)) {
        CompressedMesh(*b, toRoot);
        return;
    }
    if (auto cvs = view::bhkConvexVerticesShape::From(*b)) {
        // The shape is the vertices' hull grown by the radius (the render-bounds
        // sweep selects the grown form): each vertex is offset along the 14
        // axis and corner directions, whose hull approximates that sum.
        const float r = opt_.expandByRadius ? cvs->Radius() : 0.f;
        std::vector<V3> pts;
        for (const auto& v : cvs->Vertices()) {
            const auto p = Read4(v);
            if (r <= 0) { pts.push_back({ p[0], p[1], p[2] }); continue; }
            for (const V3& d : SphereDirs())
                if ((d[0] == 0) + (d[1] == 0) + (d[2] == 0) != 1)   // faces and corners, not edges
                    pts.push_back({ p[0] + r * d[0], p[1] + r * d[1], p[2] + r * d[2] });
        }
        Hull(pts, toRoot, "bhkConvexVerticesShape");
        return;
    }
    if (auto box = view::bhkBoxShape::From(*b)) {
        V3 h = Read3(box->HalfExtents());
        if (opt_.expandByRadius) for (float& e : h) e += box->Radius();
        std::vector<V3> pts;
        for (int c = 0; c < 8; ++c)
            pts.push_back({ (c & 1) ? h[0] : -h[0], (c & 2) ? h[1] : -h[1], (c & 4) ? h[2] : -h[2] });
        Hull(pts, toRoot, "bhkBoxShape");
        return;
    }
    if (auto sph = view::bhkSphereShape::From(*b)) {
        const float r = sph->Radius();
        std::vector<V3> pts;
        for (const V3& d : SphereDirs()) pts.push_back({ d[0] * r, d[1] * r, d[2] * r });
        Hull(pts, toRoot, "bhkSphereShape");
        return;
    }
    if (auto cap = view::bhkCapsuleShape::From(*b)) {
        const float r = cap->Radius();
        std::vector<V3> pts;
        for (const V3& e : { Read3(cap->Point1()), Read3(cap->Point2()) })
            for (const V3& d : SphereDirs()) pts.push_back({ e[0] + d[0] * r, e[1] + d[1] * r, e[2] + d[2] * r });
        Hull(pts, toRoot, "bhkCapsuleShape");
        return;
    }
    if (auto cyl = view::bhkCylinderShape::From(*b); cyl && opt_.cylinderRadius != 0) {
        const float core = opt_.cylinderRadius == 28 ? cyl->Unknown28() : cyl->Unknown48();
        const float r = opt_.expandByRadius ? cyl->Radius() : 0.f;
        const V3 a = Read3(cyl->Point1()), c = Read3(cyl->Point2());
        V3 axis{ c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        const float len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (!(len > 0)) { Skip(cls, "degenerate axis"); return; }
        for (float& e : axis) e /= len;
        // Two unit vectors perpendicular to the axis.
        V3 u = std::fabs(axis[0]) < 0.9f ? V3{ 1, 0, 0 } : V3{ 0, 1, 0 };
        const float d = u[0] * axis[0] + u[1] * axis[1] + u[2] * axis[2];
        for (int k = 0; k < 3; ++k) u[static_cast<size_t>(k)] -= d * axis[static_cast<size_t>(k)];
        const float un = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        for (float& e : u) e /= un;
        const V3 w{ axis[1] * u[2] - axis[2] * u[1], axis[2] * u[0] - axis[0] * u[2], axis[0] * u[1] - axis[1] * u[0] };
        std::vector<V3> pts;
        const float R = core + r;
        for (int end = 0; end < 2; ++end) {
            const V3& e = end ? c : a;
            const float sgn = end ? 1.f : -1.f;
            for (int i = 0; i < 16; ++i) {
                const float ang = 6.2831853f * static_cast<float>(i) / 16.f;
                const float cu = std::cos(ang) * R, sw = std::sin(ang) * R;
                pts.push_back({ e[0] + cu * u[0] + sw * w[0] + sgn * r * axis[0],
                                e[1] + cu * u[1] + sw * w[1] + sgn * r * axis[1],
                                e[2] + cu * u[2] + sw * w[2] + sgn * r * axis[2] });
            }
        }
        Hull(pts, toRoot, "bhkCylinderShape");
        return;
    }
    // bhkCylinderShape (its radius word isn't established), bhkPlaneShape (one
    // sample), bhkNiTriStripsShape (NiTriStripsData is still opaque) and anything
    // else: not decoded with confidence.
    Skip(cls, "not decoded");
}

CollisionGeometry Extractor::Run() {
    const WorldTransforms wt = ComputeWorldTransforms(nif_, opt_);
    for (size_t i = 0; i < nif_.blocks.size(); ++i) {
        auto co = view::bhkCollisionObject::From(nif_.blocks[i]);
        if (!co) continue;
        Block* body = nif_.GetBlock(co->Body().index);
        if (!body) continue;
        auto rb = view::bhkRigidBody::From(*body);
        if (!rb) {
            if (diag_ && view::bhkSimpleShapePhantom::From(*body)) ++diag_->phantomsIgnored;
            continue;   // phantoms are trigger volumes, not world collision
        }

        // Node placement: the collision object's target node, in root space.
        Xform node;
        const int32_t target = co->Target().index;
        if (opt_.nodeTransform && target >= 0 && static_cast<size_t>(target) < wt.valid.size() &&
            wt.valid[static_cast<size_t>(target)])
            node = wt.world[static_cast<size_t>(target)];

        // Havok units -> Skyrim units.
        Xform scale;
        scale.s = kHavokToSkyrim;
        Xform toRoot = Compose(node, scale);

        const bool isT = view::bhkRigidBodyT::From(*body).has_value();
        if (opt_.bodyTransform == 2 || (opt_.bodyTransform == 1 && isT)) {
            const auto q = Read4(rb->Rotation());
            const auto t = Read4(rb->Translation());
            Xform bx;
            bx.R = opt_.quatConjugate ? QuatMatrix(-q[0], -q[1], -q[2], q[3]) : QuatMatrix(q[0], q[1], q[2], q[3]);
            bx.t = { t[0], t[1], t[2] };
            toRoot = Compose(toRoot, bx);
        }
        Shape(rb->Shape().index, toRoot, 0);
    }
    return std::move(out_);
}

}  // namespace

WorldTransforms ComputeWorldTransforms(Nif& nif, const Options& opt) {
    WorldTransforms wt;
    wt.world.resize(nif.blocks.size());
    wt.valid.assign(nif.blocks.size(), false);
    std::function<void(int32_t, const Xform&, int)> walk = [&](int32_t idx, const Xform& parent, int depth) {
        if (idx < 0 || static_cast<size_t>(idx) >= nif.blocks.size() || depth > 256) return;
        const auto i = static_cast<size_t>(idx);
        if (wt.valid[i]) return;
        auto av = view::NiAVObject::From(nif.blocks[i]);
        if (!av) return;
        const Xform w = Compose(parent, NodeLocal(*av, opt));
        wt.world[i] = w;
        wt.valid[i] = true;
        if (auto node = view::NiNode::From(nif.blocks[i]))
            for (const auto& c : node->Children()) walk(c.index, w, depth + 1);
    };
    for (int32_t r : nif.roots) walk(r, Xform{}, 0);
    return wt;
}

CollisionGeometry Extract(Nif& nif, const Options& opt, Diagnostics* diag) {
    return Extractor(nif, opt, diag).Run();
}

}  // namespace collision

CollisionGeometry ExtractCollision(const NifFile& nif) {
    auto loaded = Nif::Load(nif.Save());
    if (!loaded) return {};
    return collision::Extract(*loaded, collision::Options{});
}

}  // namespace niffer
