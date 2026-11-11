#include "Framing.h"
#include "test_harness.h"

#include <niffer/Niffer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace niffer;

// ExtractCollision over a hand-built file: the layouts are the schema's
// (schema/blocks/collision), the expected numbers are worked out by hand from
// the conventions documented in docs/collision-provenance.md.
namespace {

struct Bytes {
    std::vector<uint8_t> b;
    Bytes& u8(uint8_t v) { b.push_back(v); return *this; }
    Bytes& u16(uint16_t v) { u8(uint8_t(v)); return u8(uint8_t(v >> 8)); }
    Bytes& u32(uint32_t v) { u16(uint16_t(v)); return u16(uint16_t(v >> 16)); }
    Bytes& i32(int32_t v) { return u32(static_cast<uint32_t>(v)); }
    Bytes& f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u32(u); }
    Bytes& vec4(float x, float y, float z, float w) { return f32(x).f32(y).f32(z).f32(w); }
    Bytes& zeros(size_t n) { b.insert(b.end(), n, 0); return *this; }
};

// NiNode: NiObjectNET (name, extra data, controller), NiAVObject (flags,
// translation, rotation, scale, collision object), children, effects.
std::vector<uint8_t> Node(float tx, float ty, float tz, float scale, int32_t collision,
                          const std::vector<int32_t>& children) {
    Bytes n;
    n.u32(0xFFFFFFFFu).u32(0).i32(-1);
    n.u32(0).f32(tx).f32(ty).f32(tz);
    n.f32(1).f32(0).f32(0).f32(0).f32(1).f32(0).f32(0).f32(0).f32(1);
    n.f32(scale).i32(collision);
    n.u32(static_cast<uint32_t>(children.size()));
    for (int32_t c : children) n.i32(c);
    n.u32(0);
    return n.b;
}

std::vector<uint8_t> CollisionObject(int32_t target, int32_t body) {
    Bytes c;
    c.i32(target).u16(1).i32(body);
    return c.b;
}

// bhkRigidBody / bhkRigidBodyT (250 bytes with no constraints).
std::vector<uint8_t> RigidBody(int32_t shape, const float t[3], const float q[4], uint32_t filter = 1,
                               uint8_t motion = 0, float mass = 0, const float com[3] = nullptr) {
    Bytes r;
    r.i32(shape);
    r.u32(filter).u32(0).u8(1).zeros(3).u32(0).u32(0).u32(0x80000000u);  // collisionFilter..unknown16
    r.u32(0xFFFF0001u).u32(0).u32(filter).u32(0).u32(0);                 // unknown28..unknown44
    r.u8(1).u8(0).u16(0xFFFF);                                           // response, pad, callback delay
    r.vec4(t[0], t[1], t[2], 0).vec4(q[0], q[1], q[2], q[3]);
    r.zeros(32);                                                         // linear, angular velocity
    r.vec4(mass, 0, 0, 0).vec4(0, 2 * mass, 0, 0).vec4(0, 0, 3 * mass, 0);   // inertia
    r.vec4(com ? com[0] : 0, com ? com[1] : 0, com ? com[2] : 0, 0).f32(mass);
    r.f32(0.1f).f32(0.05f).f32(1).f32(1);                                // damping, time and gravity factors
    r.f32(0.5f).f32(0).f32(0.4f);                                        // friction, rolling, restitution
    r.f32(104.4f).f32(31.57f).f32(0.15f);                                // max velocities, penetration depth
    r.u8(motion).u8(1).u8(2).u8(4).zeros(4).zeros(12);                   // motion, deactivation, quality, ...
    r.u32(0).u16(0);                                                     // constraints, tail
    return r.b;
}

std::vector<uint8_t> Capsule(float r, float z) {
    Bytes c;
    c.u32(0x5ED4DFD3u).f32(r).zeros(8).f32(0).f32(0).f32(-z).f32(r).f32(0).f32(0).f32(z).f32(r);
    return c.b;
}

// bhkConvexTransformShape: shape, material, radius, two words, then a
// HavokTransform whose three basis vectors are the rotation's columns.
std::vector<uint8_t> ConvexTransform(int32_t shape, float radius, const float cols[3][3], const float t[3]) {
    Bytes c;
    c.i32(shape).u32(0x5ED4DFD3u).f32(radius).u32(0).u32(0);
    for (int i = 0; i < 3; ++i) c.vec4(cols[i][0], cols[i][1], cols[i][2], 0);
    c.vec4(t[0], t[1], t[2], 0);
    return c.b;
}

std::vector<uint8_t> Box(float hx, float hy, float hz) {
    Bytes b;
    b.u32(0x12345678u).f32(0).zeros(8).f32(hx).f32(hy).f32(hz).f32(hz);
    return b.b;
}

std::vector<uint8_t> Cylinder() {
    Bytes c;
    c.u32(0x12345678u).f32(0.05f).zeros(8).f32(0).f32(0).f32(0).f32(0.2f).f32(0).f32(0).f32(1).f32(0.2f).f32(0.15f).zeros(12);
    return c.b;
}

std::vector<uint8_t> List(const std::vector<int32_t>& subs) {
    Bytes l;
    l.u32(static_cast<uint32_t>(subs.size()));
    for (int32_t s : subs) l.i32(s);
    l.u32(0x12345678u).u32(0).u32(0).u32(0x80000000u).u32(0).u32(0).u32(0x80000000u);
    l.u32(static_cast<uint32_t>(subs.size()));
    for (size_t i = 0; i < subs.size(); ++i) l.u32(0);
    return l.b;
}

std::vector<uint8_t> Mopp(int32_t shape) {
    Bytes m;
    m.i32(shape).u32(0).u32(0).u32(0).f32(1).u32(2).f32(-1.005f).f32(-0.005f).f32(-0.005f).f32(1000.f).u8(1).u8(0x28).u8(0);
    return m.b;
}

std::vector<uint8_t> CompressedMesh(int32_t data) {
    Bytes c;
    c.u32(0).u32(0).f32(0.005f).f32(0).vec4(1, 1, 1, 0).f32(0.005f).vec4(1, 1, 1, 0).i32(data);
    return c.b;
}

// One chunk: a quad as a single strip (3, 0, 2, 1) over quantized vertices.
std::vector<uint8_t> CompressedMeshData() {
    Bytes d;
    d.u32(17).u32(18).u32(0x3FFFF).u32(0x1FFFF).f32(0.001f);
    d.vec4(-1.f, 0, 0, 0).vec4(0.371f, 0, 3.543f, 0);                // bounds
    d.u8(0).u8(1).u32(0).u32(0).u32(0);
    d.u32(1).u32(0x84E226A3u).u32(1);                                   // materials
    d.u32(0);                                                           // unknownCount
    d.u32(1).vec4(0, 0, 0, 1).vec4(0, 0, 0, 1);                         // one identity transform
    d.u32(0).u32(0);                                                    // big vertices, big triangles
    d.u32(1);                                                           // chunks
    d.vec4(-1.f, 0, 0, 0).u32(0).u16(0xFFFF).u16(0);                   // offset, material 0, transform 0
    d.u32(12).u16(1371).u16(0).u16(0).u16(1371).u16(0).u16(3543).u16(0).u16(0).u16(3543).u16(0).u16(0).u16(0);
    d.u32(4).u16(3).u16(0).u16(2).u16(1);
    d.u32(1).u16(4);
    d.u32(4).u16(0).u16(0).u16(0).u16(0);
    d.u32(0);
    return d.b;
}

using BlockList = std::vector<std::pair<std::string, std::vector<uint8_t>>>;

// A file with these blocks and block 0 as its root.
std::optional<NifFile> Build(const BlockList& blocks) {
    Header h;
    h.versionLine = "Gamebryo File Format, Version 20.2.0.7";
    h.author = std::string("\0", 1);
    h.processScript = std::string("\0", 1);
    h.exportScript = std::string("\0", 1);
    std::map<std::string, uint16_t> typeIndex;
    std::vector<std::vector<uint8_t>> bodies;
    for (const auto& [name, raw] : blocks) {
        if (!typeIndex.count(name)) {
            typeIndex[name] = static_cast<uint16_t>(h.blockTypes.size());
            h.blockTypes.push_back(name);
        }
        h.blockTypeIndex.push_back(typeIndex[name]);
        bodies.push_back(raw);
    }
    h.numBlocks = static_cast<uint32_t>(blocks.size());
    auto nif = NifFile::Load(detail::WriteFraming(h, bodies, { 0 }, {}));
    if (!nif) return std::nullopt;
    return std::move(*nif);
}

bool Near(float a, float b, float tol = 1e-2f) { return std::fabs(a - b) <= tol; }

}  // namespace

void run_collision_tests() {
    std::printf(" collision\n");

    const float kSin45 = std::sqrt(0.5f);
    const float rbtT[3] = { 1, 0, 0 }, rbtQ[4] = { 0, 0, kSin45, kSin45 };   // 90 degrees about z
    const float rbT[3] = { 5, 5, 5 }, rbQ[4] = { 0, 0, 0, 1 };              // not applied: plain bhkRigidBody

    const BlockList blocks = {
        { "NiNode", Node(10, 0, 0, 2, 1, { 6 }) },          // 0 root
        { "bhkCollisionObject", CollisionObject(0, 2) },     // 1
        { "bhkRigidBodyT", RigidBody(3, rbtT, rbtQ, 0x8004u, 3, 2.5f) },   // 2: layer 4, dynamic
        { "bhkListShape", List({ 4, 5 }) },                  // 3
        { "bhkBoxShape", Box(1, 2, 3) },                     // 4
        { "bhkCylinderShape", Cylinder() },                  // 5 (not decoded: skipped)
        { "NiNode", Node(0, 0, 100, 1, 7, {}) },             // 6 child
        { "bhkCollisionObject", CollisionObject(6, 8) },     // 7
        { "bhkRigidBody", RigidBody(9, rbT, rbQ, 1, 5) },    // 8: layer 1, fixed
        { "bhkMoppBvTreeShape", Mopp(10) },                  // 9
        { "bhkCompressedMeshShape", CompressedMesh(11) },    // 10
        { "bhkCompressedMeshShapeData", CompressedMeshData() }, // 11
    };

    // Every block must decode under the schema, or the test exercises nothing.
    auto nif = Build(blocks);
    CHECK(nif.has_value());
    if (!nif) return;
    CHECK_EQ(nif->RawCount(), 0u);

    const CollisionGeometry g = ExtractCollision(*nif);
    CHECK_EQ(g.shapesDecoded, 2);
    CHECK_EQ(g.shapesSkipped, 1);   // the cylinder

    // The box: corners (+-1, +-2, +-3), rotated 90 degrees about z, moved by (1, 0, 0)
    // in Havok units, then scaled by kHavokToSkyrim and placed by the root node
    // (scale 2, translation (10, 0, 0)).
    CHECK_EQ(g.hulls.size(), 1u);
    if (g.hulls.size() == 1) {
        const auto& h = g.hulls[0];
        CHECK_EQ(h.size(), 24u);
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        for (size_t i = 0; i + 2 < h.size(); i += 3)
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], h[i + k]); hi[k] = std::max(hi[k], h[i + k]); }
        const float s = 2.f * kHavokToSkyrim;
        CHECK(Near(lo[0], -1.f * s + 10.f) && Near(hi[0], 3.f * s + 10.f));
        CHECK(Near(lo[1], -1.f * s) && Near(hi[1], 1.f * s));
        CHECK(Near(lo[2], -3.f * s) && Near(hi[2], 3.f * s));
    }

    // The compressed mesh: 4 vertices at offset + q * 0.001, two triangles from
    // the strip (3, 0, 2) and (2, 0, 1); the plain bhkRigidBody's translation is
    // not applied; the child node puts it at (10, 0, 200) with scale 2.
    CHECK_EQ(g.triVerts.size(), 12u);
    CHECK_EQ(g.triIndices.size(), 6u);
    if (g.triVerts.size() == 12 && g.triIndices.size() == 6) {
        const float s = 2.f * kHavokToSkyrim;
        CHECK(Near(g.triVerts[0], 0.371f * s + 10.f) && Near(g.triVerts[1], 0.f) && Near(g.triVerts[2], 200.f));
        CHECK(Near(g.triVerts[5], 3.543f * s + 200.f));
        const std::vector<uint32_t> want = { 3, 0, 2, 2, 0, 1 };
        CHECK(g.triIndices == want);
    }

    // ExtractBodies over the same file: one record per body, primitives kept.
    const std::vector<Body> bodies = ExtractBodies(*nif);
    CHECK_EQ(bodies.size(), 2u);
    if (bodies.size() == 2) {
        const float s = 2.f * kHavokToSkyrim;

        // The bhkRigidBodyT: placed by the root node, scaled to Skyrim units, then
        // its own 90-degree turn about z and its (1, 0, 0) move.
        const Body& a = bodies[0];
        CHECK_EQ(a.collisionObject, 1);
        CHECK_EQ(a.body, 2);
        CHECK_EQ(a.targetNode, 0);
        CHECK(a.bodyClass == "bhkRigidBodyT");
        CHECK_EQ(a.collisionFilter, 0x8004u);
        CHECK_EQ(a.layer, 4);
        CHECK_EQ(a.motionType, 3);
        CHECK(a.motion == BodyMotion::Dynamic);
        CHECK_EQ(a.qualityType, 4);
        CHECK(Near(a.friction, 0.5f, 1e-6f) && Near(a.restitution, 0.4f, 1e-6f));
        CHECK(Near(a.linearDamping, 0.1f, 1e-6f) && Near(a.angularDamping, 0.05f, 1e-6f));
        CHECK(Near(a.gravityFactor, 1.f, 0.f));
        CHECK(Near(a.maxLinearVelocity, 100.f, 0.f));   // 104.4 as stored, clamped as the loader does
        CHECK(Near(a.maxAngularVelocity, 31.57f, 1e-4f) && Near(a.allowedPenetrationDepth, 0.15f, 1e-6f));
        CHECK(Near(a.mass, 2.5f, 1e-6f));
        CHECK(Near(a.inertia[0], 2.5f, 1e-6f) && Near(a.inertia[4], 5.f, 1e-6f) && Near(a.inertia[8], 7.5f, 1e-6f));
        CHECK(Near(a.scale, s, 1e-3f));
        CHECK(Near(a.translation[0], 10.f + s) && Near(a.translation[1], 0.f) && Near(a.translation[2], 0.f));
        const std::array<float, 9> rz{ 0, -1, 0, 1, 0, 0, 0, 0, 1 };
        bool rotOk = true;
        for (size_t k = 0; k < 9; ++k) rotOk = rotOk && Near(a.rotation[k], rz[k], 1e-5f);
        CHECK(rotOk);
        CHECK_EQ(a.shapes.size(), 2u);
        CHECK_EQ(a.shapesSkipped, 1);
        if (a.shapes.size() == 2) {
            const BodyShape& box = a.shapes[0];
            CHECK(box.kind == BodyShape::Kind::Box);
            CHECK(box.className == "bhkBoxShape");
            CHECK((box.path == std::vector<int32_t>{ 3, 4 }));
            CHECK_EQ(box.material, 0x12345678u);
            CHECK(Near(box.halfExtents[0], 1.f) && Near(box.halfExtents[1], 2.f) && Near(box.halfExtents[2], 3.f));
            const BodyShape& cyl = a.shapes[1];
            CHECK(cyl.kind == BodyShape::Kind::Skipped);
            CHECK(cyl.className == "bhkCylinderShape");
            CHECK(!cyl.skipReason.empty());
        }

        // The plain bhkRigidBody: its own translation is not applied; the child
        // node puts it at (10, 0, 200). The mesh keeps its triangles and materials.
        const Body& b = bodies[1];
        CHECK_EQ(b.targetNode, 6);
        CHECK_EQ(b.layer, 1);
        CHECK(b.motion == BodyMotion::Fixed);
        CHECK(Near(b.mass, 0.f, 0.f));
        CHECK(Near(b.translation[0], 10.f) && Near(b.translation[1], 0.f) && Near(b.translation[2], 200.f));
        CHECK_EQ(b.shapes.size(), 1u);
        CHECK_EQ(b.shapesSkipped, 0);
        if (b.shapes.size() == 1) {
            const BodyShape& m = b.shapes[0];
            CHECK(m.kind == BodyShape::Kind::Mesh);
            CHECK((m.path == std::vector<int32_t>{ 9, 10 }));
            CHECK_EQ(m.vertices.size(), 12u);
            CHECK((m.triangles == std::vector<uint32_t>{ 3, 0, 2, 2, 0, 1 }));
            CHECK((m.triangleMaterials == std::vector<uint32_t>{ 0x84E226A3u, 0x84E226A3u }));
            if (m.vertices.size() == 12) CHECK(Near(m.vertices[0], 0.371f, 1e-4f) && Near(m.vertices[5], 3.543f, 1e-4f));
        }

        // Bodies and ExtractCollision agree: the box's corners, placed through the
        // shape and body transforms, span the hull's bounds.
        if (!g.hulls.empty() && !a.shapes.empty()) {
            const BodyShape& box = a.shapes[0];
            float lo = 1e9f, hi = -1e9f;
            for (int c = 0; c < 8; ++c) {
                const float p[3] = { (c & 1) ? box.halfExtents[0] : -box.halfExtents[0],
                                     (c & 2) ? box.halfExtents[1] : -box.halfExtents[1],
                                     (c & 4) ? box.halfExtents[2] : -box.halfExtents[2] };
                float x = a.translation[0];
                for (int k = 0; k < 3; ++k) x += a.scale * a.rotation[k] * p[k];
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
            CHECK(Near(lo, -1.f * s + 10.f) && Near(hi, 3.f * s + 10.f));
        }
    }

    // A keyframed body with a capsule under a convex transform (90 degrees about
    // x, then up by 1): the leaf keeps its own points and carries the transform.
    {
        const float none[3] = { 0, 0, 0 }, identity[4] = { 0, 0, 0, 1 };
        const float cols[3][3] = { { 1, 0, 0 }, { 0, 0, 1 }, { 0, -1, 0 } };
        const float up[3] = { 0, 0, 1 };
        const float com[3] = { 0, 0.25f, 1 };
        const BlockList kb = {
            { "NiNode", Node(0, 0, 0, 1, 1, {}) },                            // 0
            { "bhkCollisionObject", CollisionObject(0, 2) },                   // 1
            { "bhkRigidBody", RigidBody(3, none, identity, 2, 4, 0, com) },    // 2: layer 2, keyframed
            { "bhkConvexTransformShape", ConvexTransform(4, 0.5f, cols, up) }, // 3
            { "bhkCapsuleShape", Capsule(0.5f, 1.f) },                         // 4
        };
        auto kn = Build(kb);
        CHECK(kn.has_value());
        if (kn) {
            CHECK_EQ(kn->RawCount(), 0u);
            const std::vector<Body> kbodies = ExtractBodies(*kn);
            CHECK_EQ(kbodies.size(), 1u);
            if (kbodies.size() == 1 && kbodies[0].shapes.size() == 1) {
                const Body& k = kbodies[0];
                CHECK(k.motion == BodyMotion::Keyframed);
                CHECK_EQ(k.layer, 2);
                CHECK(Near(k.centerOfMass[1], 0.25f, 1e-6f) && Near(k.centerOfMass[2], 1.f, 1e-6f));
                const BodyShape& c = k.shapes[0];
                CHECK(c.kind == BodyShape::Kind::Capsule);
                CHECK((c.path == std::vector<int32_t>{ 3, 4 }));
                CHECK_EQ(c.material, 0x5ED4DFD3u);
                CHECK(Near(c.radius, 0.5f, 1e-6f));
                CHECK(Near(c.point1[2], -1.f, 1e-6f) && Near(c.point2[2], 1.f, 1e-6f));
                const std::array<float, 9> rx{ 1, 0, 0, 0, 0, -1, 0, 1, 0 };
                bool ok = true;
                for (size_t i = 0; i < 9; ++i) ok = ok && Near(c.rotation[i], rx[i], 1e-6f);
                CHECK(ok);
                CHECK(Near(c.translation[2], 1.f, 1e-6f));
            }
        }
    }

    // The loader's rule for bodies of no mass: fixed unless the layer is 2 or 28,
    // keyframed in layers 14 and 29, whatever the stored motion type.
    {
        struct Case { uint32_t layer; uint8_t motion; float mass; BodyMotion want; };
        const Case cases[] = {
            { 2, 4, 0.f, BodyMotion::Keyframed },   // animated static keeps its keyframed motion
            { 1, 4, 0.f, BodyMotion::Fixed },       // a static layer is made fixed
            { 14, 5, 0.f, BodyMotion::Keyframed },  // traps are keyframed
            { 4, 2, 3.f, BodyMotion::Dynamic },     // a body with mass keeps its own
        };
        const float none[3] = { 0, 0, 0 }, identity[4] = { 0, 0, 0, 1 };
        for (const Case& c : cases) {
            const BlockList cb = {
                { "NiNode", Node(0, 0, 0, 1, 1, {}) },
                { "bhkCollisionObject", CollisionObject(0, 2) },
                { "bhkRigidBody", RigidBody(3, none, identity, c.layer, c.motion, c.mass) },
                { "bhkBoxShape", Box(1, 1, 1) },
            };
            auto cn = Build(cb);
            CHECK(cn.has_value());
            if (!cn) continue;
            const std::vector<Body> cbodies = ExtractBodies(*cn);
            CHECK_EQ(cbodies.size(), 1u);
            if (cbodies.size() == 1) CHECK(cbodies[0].motion == c.want);
        }
    }
}
