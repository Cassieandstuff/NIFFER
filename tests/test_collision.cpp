#include "test_harness.h"

#include <niffer/Niffer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
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
std::vector<uint8_t> RigidBody(int32_t shape, const float t[3], const float q[4]) {
    Bytes r;
    r.i32(shape);
    r.u32(1).u32(0).u8(1).zeros(3).u32(0).u32(0).u32(0x80000000u);   // unknown4..unknown16
    r.u32(0xFFFF0001u).u32(0).u32(1).u32(0).u32(0).u32(0xFFFF0001u);  // unknown28..unknown48
    r.vec4(t[0], t[1], t[2], 0).vec4(q[0], q[1], q[2], q[3]);
    r.zeros(32);                                                      // unknown84, unknown100
    r.zeros(48).zeros(16).f32(0);                                     // inertia, centre of mass, mass
    r.zeros(40).zeros(8).zeros(12);                                   // unknown184..unknown232
    r.u32(0).u16(0);                                                  // constraints, tail
    return r.b;
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

bool Near(float a, float b, float tol = 1e-2f) { return std::fabs(a - b) <= tol; }

}  // namespace

void run_collision_tests() {
    std::printf(" collision\n");

    const float kSin45 = std::sqrt(0.5f);
    const float rbtT[3] = { 1, 0, 0 }, rbtQ[4] = { 0, 0, kSin45, kSin45 };   // 90 degrees about z
    const float rbT[3] = { 5, 5, 5 }, rbQ[4] = { 0, 0, 0, 1 };              // not applied: plain bhkRigidBody

    std::vector<std::pair<std::string, std::vector<uint8_t>>> blocks = {
        { "NiNode", Node(10, 0, 0, 2, 1, { 6 }) },          // 0 root
        { "bhkCollisionObject", CollisionObject(0, 2) },     // 1
        { "bhkRigidBodyT", RigidBody(3, rbtT, rbtQ) },       // 2
        { "bhkListShape", List({ 4, 5 }) },                  // 3
        { "bhkBoxShape", Box(1, 2, 3) },                     // 4
        { "bhkCylinderShape", Cylinder() },                  // 5 (not decoded: skipped)
        { "NiNode", Node(0, 0, 100, 1, 7, {}) },             // 6 child
        { "bhkCollisionObject", CollisionObject(6, 8) },     // 7
        { "bhkRigidBody", RigidBody(9, rbT, rbQ) },          // 8
        { "bhkMoppBvTreeShape", Mopp(10) },                  // 9
        { "bhkCompressedMeshShape", CompressedMesh(11) },    // 10
        { "bhkCompressedMeshShapeData", CompressedMeshData() }, // 11
    };

    NifFile f;
    f.header.versionLine = "Gamebryo File Format, Version 20.2.0.7";
    f.header.version = kNifVersion;
    f.header.endian = 1;
    f.header.userVersion = kUserVersion;
    f.header.bsVersion = kStreamVersion;
    f.header.author = std::string("\0", 1);
    f.header.processScript = std::string("\0", 1);
    f.header.exportScript = std::string("\0", 1);
    std::map<std::string, uint16_t> typeIndex;
    for (auto& [name, raw] : blocks) {
        if (!typeIndex.count(name)) {
            typeIndex[name] = static_cast<uint16_t>(f.header.blockTypes.size());
            f.header.blockTypes.push_back(name);
        }
        f.header.blockTypeIndex.push_back(typeIndex[name]);
        f.header.blockSizes.push_back(0);
        auto u = std::make_unique<UnknownBlock>();
        u->typeName = name;
        u->raw = raw;
        f.blocks.push_back(std::move(u));
    }
    f.header.numBlocks = static_cast<uint32_t>(blocks.size());
    f.roots = { 0 };

    // Every block must decode under the schema, or the test exercises nothing.
    auto nif = Nif::Load(f.Save());
    CHECK(nif.has_value());
    if (nif) CHECK_EQ(nif->RawCount(), 0u);

    const CollisionGeometry g = ExtractCollision(f);
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
}
