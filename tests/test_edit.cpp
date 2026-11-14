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

// Tangent handedness (N5) and block-list editing (N6) over hand-built files.
// Layouts are the schema's; the tangent convention and its corpus evidence are
// in docs/tangent-space.md, the collision values in bhkRigidBody's schema docs.
namespace {

struct Bytes {
    std::vector<uint8_t> b;
    Bytes& u8(uint8_t v) { b.push_back(v); return *this; }
    Bytes& u16(uint16_t v) { u8(uint8_t(v)); return u8(uint8_t(v >> 8)); }
    Bytes& u32(uint32_t v) { u16(uint16_t(v)); return u16(uint16_t(v >> 16)); }
    Bytes& u64(uint64_t v) { u32(uint32_t(v)); return u32(uint32_t(v >> 32)); }
    Bytes& i32(int32_t v) { return u32(static_cast<uint32_t>(v)); }
    Bytes& f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u32(u); }
    Bytes& vec4(float x, float y, float z, float w) { return f32(x).f32(y).f32(z).f32(w); }
    Bytes& zeros(size_t n) { b.insert(b.end(), n, 0); return *this; }
};

// NiObjectNET + NiAVObject with an identity transform.
void AvObject(Bytes& n, uint32_t name, int32_t collision) {
    n.u32(name).u32(0).i32(-1);
    n.u32(14).f32(0).f32(0).f32(0);
    n.f32(1).f32(0).f32(0).f32(0).f32(1).f32(0).f32(0).f32(0).f32(1);
    n.f32(1).i32(collision);
}

std::vector<uint8_t> Node(uint32_t name, int32_t collision, const std::vector<int32_t>& children,
                          const std::vector<int32_t>& extra = {}) {
    Bytes n;
    n.u32(name).u32(static_cast<uint32_t>(extra.size()));
    for (int32_t e : extra) n.i32(e);
    n.i32(-1);
    n.u32(14).f32(0).f32(0).f32(0);
    n.f32(1).f32(0).f32(0).f32(0).f32(1).f32(0).f32(0).f32(0).f32(1);
    n.f32(1).i32(collision);
    n.u32(static_cast<uint32_t>(children.size()));
    for (int32_t c : children) n.i32(c);
    n.u32(0);
    return n.b;
}

std::vector<uint8_t> XFlags(uint32_t name, uint32_t flags) {
    Bytes x;
    x.u32(name).u32(flags);
    return x.b;
}

// A half float's bits for the few exact values the UVs use.
uint16_t HalfBits(float f) { return Half::FromFloat(f).bits; }

struct Vert {
    float p[3];
    float uv[2];
};

// BSTriShape with full-precision position, uv, normal (+Z) and tangent; the
// tangent bytes and the W slots are left zero for ComputeTangentSpace to fill.
std::vector<uint8_t> TriShape(uint32_t name, const std::vector<Vert>& verts, const std::vector<uint16_t>& tris) {
    Bytes s;
    AvObject(s, name, -1);
    s.f32(0).f32(0).f32(0).f32(10);                       // bound sphere
    s.i32(-1).i32(-1).i32(-1);                            // skin, shader, alpha
    s.u64(0x0001B00000650407ull);                         // 28-byte vertex: pos, uv, normal, tangent
    s.u16(static_cast<uint16_t>(tris.size() / 3)).u16(static_cast<uint16_t>(verts.size()));
    s.u32(static_cast<uint32_t>(verts.size() * 28));
    for (const Vert& v : verts) {
        s.f32(v.p[0]).f32(v.p[1]).f32(v.p[2]).f32(0);
        s.u16(HalfBits(v.uv[0])).u16(HalfBits(v.uv[1]));
        s.u8(128).u8(128).u8(255).u8(0);                  // normal +Z, normalW
        s.u8(0).u8(0).u8(0).u8(0);                        // tangent, tangentW
    }
    for (uint16_t t : tris) s.u16(t);
    s.u32(0);                                             // particleDataSize
    return s.b;
}

std::vector<uint8_t> CollisionObject(int32_t target, int32_t body) {
    Bytes c;
    c.i32(target).u16(0x81).i32(body);
    return c.b;
}

// bhkRigidBody as barrel01 stores it (fixed, layer 1), on `shape`.
std::vector<uint8_t> FixedBody(int32_t shape) {
    Bytes r;
    r.i32(shape);
    r.u32(1).u32(0).u8(1).zeros(3).u32(0).u32(0).u32(0x80000000u);
    r.u32(0xFFFF0001u).u32(0).u32(1).u32(0).u32(0);
    r.u8(1).u8(0).u16(0xFFFF);
    r.vec4(0, 0, 0, 0).vec4(0, 0, 0, 1);
    r.zeros(32).zeros(48).vec4(0, 0, 0, 0).f32(0);
    r.f32(0.1f).f32(0.05f).f32(1).f32(1).f32(0.5f).f32(0).f32(0.4f);
    r.f32(104.4f).f32(31.57f).f32(0.1f);
    r.u8(5).u8(1).u8(1).u8(0).u8(0).u8(0).u8(3).u8(0).zeros(12);
    r.u32(0).u16(0);
    return r.b;
}

std::vector<uint8_t> Box(float hx, float hy, float hz) {
    Bytes b;
    b.u32(500811281u).f32(0.05f).zeros(8).f32(hx).f32(hy).f32(hz).f32(hz);
    return b.b;
}

using BlockList = std::vector<std::pair<std::string, std::vector<uint8_t>>>;

std::optional<NifFile> Build(const BlockList& blocks, const std::vector<std::string>& strings, int32_t root = 0) {
    Header h;
    h.versionLine = "Gamebryo File Format, Version 20.2.0.7";
    h.author = std::string("\0", 1);
    h.processScript = std::string("\0", 1);
    h.exportScript = std::string("\0", 1);
    h.strings = strings;
    for (const std::string& s : strings) h.maxStringLength = std::max(h.maxStringLength, static_cast<uint32_t>(s.size()));
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
    auto nif = NifFile::Load(detail::WriteFraming(h, bodies, { root }, {}));
    if (!nif) return std::nullopt;
    return std::move(*nif);
}

bool Near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

// A box hull (corner bits: x 1, y 2, z 4), Skyrim units, faces wound inconsistently on purpose.
ConvexHullInput BoxHull(float x0, float x1, float y0, float y1, float z0, float z1) {
    ConvexHullInput h;
    for (int i = 0; i < 8; ++i) {
        h.points.push_back(i & 1 ? x1 : x0);
        h.points.push_back(i & 2 ? y1 : y0);
        h.points.push_back(i & 4 ? z1 : z0);
    }
    h.triangles = { 0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                    2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 3, 7, 1, 7, 5 };   // the last face winds inward
    return h;
}

// A barrel01-like file: BSFadeNode root (named), BSXFlags, a fixed box body, one BSTriShape.
BlockList BarrelLike() {
    return {
        { "BSFadeNode", Node(0, 4, { 5 }, { 1 }) },          // 0 root "Barrel01", collision 4
        { "BSXFlags", XFlags(1, 2) },                        // 1 "BSX"
        { "bhkBoxShape", Box(0.4f, 0.4f, 0.6f) },            // 2
        { "bhkRigidBody", FixedBody(2) },                    // 3
        { "bhkCollisionObject", CollisionObject(0, 3) },     // 4
        { "BSTriShape", TriShape(2, { { { 0, 0, 0 }, { 0, 1 } }, { { 1, 0, 0 }, { 1, 1 } }, { { 0, 1, 0 }, { 0, 0 } } },
                                 { 0, 1, 2 }) },             // 5 "Barrel01:0"
    };
}
const std::vector<std::string> kBarrelStrings = { "Barrel01", "BSX", "Barrel01:0" };

}  // namespace

void run_edit_tests() {
    std::printf(" edit\n");

    // ── N5: handedness on a mirrored-UV mesh ────────────────────────────────
    // Two triangles in the z = 0 plane (normal +Z). Triangle A maps U to +X and
    // V to -Y (V runs down the image): the usual frame, W = +1. Triangle B is
    // A's UVs mirrored in U: W = -1. The NIF's tangent follows +dP/dV and its
    // bitangent +dP/dU.
    {
        const std::vector<Vert> verts = {
            { { 0, 0, 0 }, { 0, 1 } }, { { 1, 0, 0 }, { 1, 1 } }, { { 0, 1, 0 }, { 0, 0 } },   // A
            { { 2, 0, 0 }, { 1, 1 } }, { { 3, 0, 0 }, { 0, 1 } }, { { 2, 1, 0 }, { 1, 0 } },   // B (mirrored)
        };
        auto nif = Build({ { "NiNode", Node(0xFFFFFFFFu, -1, { 1 }) },
                           { "BSTriShape", TriShape(0xFFFFFFFFu, verts, { 0, 1, 2, 3, 4, 5 }) } }, {});
        CHECK(nif.has_value());
        if (nif) {
            const std::vector<uint8_t> blank = nif->Save();
            DecodedMesh m = DecodeShapeMesh(*nif, nif->blocks[1]);
            CHECK_EQ(m.numVertices, 6u);
            CHECK_EQ(m.bitangents.size(), 18u);
            ComputeTangentSpace(m);
            CHECK_EQ(m.tangents.size(), 24u);
            if (m.tangents.size() == 24 && m.bitangents.size() == 18) {
                for (int v = 0; v < 3; ++v) {
                    CHECK_EQ(m.tangents[v * 4 + 3], 1.f);                 // A: usual
                    CHECK(Near(m.tangents[v * 4 + 1], -1.f));             // T along +dP/dV = -Y
                    CHECK(Near(m.bitangents[v * 3 + 0], 1.f));            // B along +dP/dU = +X
                }
                for (int v = 3; v < 6; ++v) {
                    CHECK_EQ(m.tangents[v * 4 + 3], -1.f);                // B: mirrored
                    CHECK(Near(m.tangents[v * 4 + 1], -1.f));
                    CHECK(Near(m.bitangents[v * 3 + 0], -1.f));           // +dP/dU = -X
                }
            }
            // Encode, save, reload: the stored frame decodes to the same handedness.
            CHECK(EncodeTangentSpace(*nif, nif->blocks[1], m));
            const std::vector<uint8_t> encoded = nif->Save();
            CHECK(encoded != blank);
            CHECK_EQ(encoded.size(), blank.size());
            auto back = NifFile::Load(encoded);
            CHECK(back.has_value());
            if (back) {
                const DecodedMesh d = DecodeShapeMesh(*back, back->blocks[1]);
                CHECK_EQ(d.tangents.size(), 24u);
                if (d.tangents.size() == 24 && d.bitangents.size() == 18)
                    for (int v = 0; v < 6; ++v) {
                        CHECK_EQ(d.tangents[v * 4 + 3], v < 3 ? 1.f : -1.f);
                        for (int k = 0; k < 3; ++k) {
                            CHECK(Near(d.tangents[v * 4 + k], m.tangents[v * 4 + k], 0.01f));
                            CHECK(Near(d.bitangents[v * 3 + k], m.bitangents[v * 3 + k], 0.01f));
                        }
                    }
                // The decoded frame encodes back to the same bytes: the block stays verbatim.
                CHECK(EncodeTangentSpace(*back, back->blocks[1], d));
                CHECK(!back->blocks[1].Dirty());
                CHECK(back->Save() == encoded);
                // Without bitangents, B = W (N x T) keeps the handedness.
                DecodedMesh noB = d;
                noB.bitangents.clear();
                CHECK(EncodeTangentSpace(*back, back->blocks[1], noB));
                auto again = NifFile::Load(back->Save());
                CHECK(again.has_value());
                if (again) {
                    const DecodedMesh e = DecodeShapeMesh(*again, again->blocks[1]);
                    if (e.tangents.size() == 24)
                        for (int v = 0; v < 6; ++v) CHECK_EQ(e.tangents[v * 4 + 3], v < 3 ? 1.f : -1.f);
                }
            }
        }
    }

    // ── N6: Block::Create ────────────────────────────────────────────────────
    {
        auto b = Block::Create(schema::Main(), "bhkCollisionObject");
        CHECK(b.has_value());
        if (b) {
            auto co = view::bhkCollisionObject::From(*b);
            CHECK(co && co->Target().IsNull() && co->Body().IsNull());
            CHECK_EQ(b->Encode().size(), 10u);
        }
        CHECK(!Block::Create(schema::Main(), "NiAVObject").has_value());   // abstract
        CHECK(!Block::Create(schema::Main(), "NoSuchBlock").has_value());
        auto node = Block::Create(schema::Main(), "BSFadeNode");
        CHECK(node && view::NiObjectNET::From(*node)->Name().IsEmpty());
    }

    // ── N6: insert then remove gives back the original bytes ────────────────
    {
        auto nif = Build(BarrelLike(), kBarrelStrings);
        CHECK(nif.has_value());
        if (nif) {
            CHECK_EQ(nif->RawCount(), 0u);
            const std::vector<uint8_t> original = nif->Save();
            const Header header = nif->header;
            for (int32_t at : { 0, 2, 3, 6 }) {
                auto shape = Block::Create(schema::Main(), "bhkConvexVerticesShape");
                CHECK(shape.has_value());
                if (!shape) break;
                auto in = nif->InsertBlock(at, std::move(*shape));
                CHECK(in.has_value() && *in == at);
                CHECK_EQ(nif->blocks.size(), 7u);
                // Every ref still reaches the same block.
                const int32_t root = at <= 0 ? 1 : 0;
                CHECK_EQ(nif->roots[0], root);
                auto fade = view::NiNode::From(nif->blocks[static_cast<size_t>(root)]);
                CHECK(fade.has_value());
                if (fade) {
                    const Block* co = nif->GetBlock(fade->CollisionObject().index);
                    CHECK(co && co->TypeName() == "bhkCollisionObject");
                    auto children = fade->Children();
                    CHECK(children.size() == 1 && nif->GetBlock(children[0].index) &&
                          nif->GetBlock(children[0].index)->TypeName() == "BSTriShape");
                    auto target = co ? view::bhkCollisionObject::From(*const_cast<Block*>(co)) : std::nullopt;
                    CHECK(target && target->Target().index == root);
                }
                CHECK(std::find(nif->header.blockTypes.begin(), nif->header.blockTypes.end(), "bhkConvexVerticesShape") !=
                      nif->header.blockTypes.end());
                auto out = nif->RemoveBlock(at);
                CHECK(out.has_value() && *out == 1u);
                CHECK(nif->Save() == original);
                CHECK(nif->header.blockTypes == header.blockTypes);
            }
            // A named block: its string joins the table, and leaves with it.
            auto node = Block::Create(schema::Main(), "NiNode");
            CHECK(node.has_value());
            if (node) {
                const NiStringRef name = nif->AddString("A much longer node name than any other");
                CHECK_EQ(name.index, 3u);
                CHECK_EQ(nif->header.maxStringLength, 38u);
                CHECK(view::NiObjectNET::From(*node)->SetName(name));
                CHECK(nif->InsertBlock(1, std::move(*node)).has_value());
                CHECK(nif->AddString("BSX").index == 1u);   // an existing string is reused
                auto reloaded = NifFile::Load(nif->Save());
                CHECK(reloaded.has_value() && reloaded->RawCount() == 0 && reloaded->blocks.size() == 7);
                CHECK(nif->RemoveBlock(1).has_value());
                CHECK(nif->header.strings == kBarrelStrings);
                CHECK_EQ(nif->header.maxStringLength, header.maxStringLength);
                CHECK(nif->Save() == original);
            }
            CHECK(!nif->InsertBlock(9, *Block::Create(schema::Main(), "BSXFlags")).has_value());
            CHECK(!nif->RemoveBlock(-1).has_value());
        }
    }

    // ── N6: remove nulls refs, prunes types and strings; OwnedTree; EraseElement ──
    {
        auto nif = Build(BarrelLike(), kBarrelStrings);
        CHECK(nif.has_value());
        if (nif) {
            // The collision tree: object, body, box; not the node it points back at.
            CHECK(nif->OwnedTree(4) == std::vector<int32_t>({ 2, 3, 4 }));
            // A box shared with another body is not the first body's to remove.
            auto shared = nif->blocks[3];
            CHECK(nif->AppendBlock(std::move(shared)).has_value());   // block 6: a second body on box 2
            CHECK(nif->OwnedTree(4) == std::vector<int32_t>({ 3, 4 }));
            CHECK(nif->RemoveBlock(6).has_value());

            auto removed = RemoveCollision(*nif, 0);
            CHECK(removed.has_value() && *removed == 3u);
            CHECK_EQ(nif->blocks.size(), 3u);
            auto fade = view::NiNode::From(nif->blocks[0]);
            CHECK(fade && fade->CollisionObject().IsNull());
            CHECK(fade && fade->Children().size() == 1 && fade->Children()[0].index == 2);
            CHECK(nif->header.blockTypes == std::vector<std::string>({ "BSFadeNode", "BSXFlags", "BSTriShape" }));
            CHECK(nif->header.strings == kBarrelStrings);   // the survivors still use all three
            auto back = NifFile::Load(nif->Save());
            CHECK(back.has_value() && back->RawCount() == 0);
            CHECK(RemoveCollision(*nif, 0).value_or(9) == 0u);   // nothing left to remove

            // Removing the shape: the node's child entry becomes null, then is erased.
            CHECK(nif->RemoveBlock(2).has_value());
            CHECK(fade && view::NiNode::From(nif->blocks[0])->Children().size() == 1 &&
                  view::NiNode::From(nif->blocks[0])->Children()[0].IsNull());
            CHECK(nif->header.strings == std::vector<std::string>({ "Barrel01", "BSX" }));
            CHECK_EQ(nif->header.maxStringLength, 8u);
            const RelPath children = *nif->blocks[0].Find("children");
            CHECK(nif->blocks[0].EraseElement(children, 0));
            CHECK(view::NiNode::From(nif->blocks[0])->Children().empty());
            CHECK(!nif->blocks[0].EraseElement(children, 0));
            auto small = NifFile::Load(nif->Save());
            CHECK(small.has_value() && small->RawCount() == 0 && small->blocks.size() == 2);
        }
    }

    // ── N6: convex-hull collision on barrel01-like data, re-read ────────────
    {
        auto nif = Build(BarrelLike(), kBarrelStrings);
        CHECK(nif.has_value());
        if (nif) {
            // Two boxes in Skyrim units: 20 x 30 x 40 and 10 x 10 x 10 above it.
            const ConvexHullInput hulls[2] = { BoxHull(-10, 10, -15, 15, 0, 40), BoxHull(-5, 5, -5, 5, 40, 50) };
            CollisionDesc desc;
            desc.mass = 12;
            auto att = AttachConvexCollision(*nif, 0, hulls, desc);
            CHECK(att.has_value());
            if (att) {
                CHECK_EQ(att->removed, 3u);   // the old box body
                // Old tree gone (3 blocks), then hulls, list, body, object appended.
                CHECK_EQ(nif->blocks.size(), 8u);
                CHECK(att->hulls == std::vector<int32_t>({ 3, 4 }));
                CHECK_EQ(att->shape, 5);
                CHECK_EQ(att->body, 6);
                CHECK_EQ(att->collisionObject, 7);
                const float h = kHavokToSkyrim;
                const double v1 = 20.0 * 30 * 40 / (h * h * h), v2 = 1000.0 / (h * h * h);
                CHECK(Near(att->volume, static_cast<float>(v1 + v2), 1e-4f));
                const double comZ = (v1 * 20 + v2 * 45) / (v1 + v2) / h;
                CHECK(Near(att->centerOfMass[0], 0, 1e-5f) && Near(att->centerOfMass[1], 0, 1e-5f));
                CHECK(Near(att->centerOfMass[2], static_cast<float>(comZ), 1e-4f));
                // Inertia about the centre of mass, by parallel axes from each box's own.
                const double m1 = 12 * v1 / (v1 + v2), m2 = 12 * v2 / (v1 + v2);
                const double a = 20 / h, b = 30 / h, c = 40 / h, s = 10 / h;
                const double d1 = 20 / h - comZ, d2 = 45 / h - comZ;
                const double ixx = m1 * (b * b + c * c) / 12 + m1 * d1 * d1 + m2 * (2 * s * s) / 12 + m2 * d2 * d2;
                const double izz = m1 * (a * a + b * b) / 12 + m2 * (2 * s * s) / 12;
                CHECK(Near(att->inertia[0], static_cast<float>(ixx), 1e-4f));
                CHECK(Near(att->inertia[8], static_cast<float>(izz), 1e-4f));
                CHECK(Near(att->inertia[1], 0, 1e-5f) && Near(att->inertia[5], 0, 1e-5f));
            }
            const std::vector<uint8_t> saved = nif->Save();
            auto back = NifFile::Load(saved);
            CHECK(back.has_value());
            if (back) {
                CHECK_EQ(back->RawCount(), 0u);
                CHECK(back->Save() == saved);
                CHECK(std::find(back->header.blockTypes.begin(), back->header.blockTypes.end(), "bhkBoxShape") ==
                      back->header.blockTypes.end());
                const std::vector<Body> bodies = ExtractBodies(*back);
                CHECK_EQ(bodies.size(), 1u);
                if (bodies.size() == 1) {
                    const Body& b = bodies[0];
                    CHECK_EQ(b.collisionObject, 7);
                    CHECK_EQ(b.targetNode, 0);
                    CHECK_EQ(b.bodyClass, std::string("bhkRigidBody"));
                    CHECK(b.motion == BodyMotion::Dynamic);
                    CHECK_EQ(b.motionType, 3u);
                    CHECK_EQ(b.layer, 4u);
                    CHECK_EQ(b.mass, 12.f);
                    CHECK_EQ(b.friction, 0.5f);
                    CHECK_EQ(b.shapes.size(), 2u);
                    for (const BodyShape& s : b.shapes) {
                        CHECK(s.kind == BodyShape::Kind::ConvexVertices);
                        CHECK_EQ(s.vertices.size(), 24u);
                        CHECK_EQ(s.material, 500811281u);
                        CHECK_EQ(s.radius, 0.05f);
                    }
                    if (b.shapes.size() == 2) {
                        CHECK(Near(b.shapes[0].vertices[0], -10 / kHavokToSkyrim, 1e-6f));
                        CHECK(b.shapes[0].path == std::vector<int32_t>({ 5, 3 }));
                    }
                }
                // Planes: 6 per box (faces merged), outward, offset by the radius.
                auto cvs = view::bhkConvexVerticesShape::From(back->blocks[3]);
                CHECK(cvs.has_value());
                if (cvs) {
                    auto planes = cvs->Planes();
                    CHECK_EQ(planes.size(), 6u);
                    for (auto& p : planes) {
                        const float n[3] = { p.X(), p.Y(), p.Z() };
                        float worst = -1e30f;   // the largest n.v + w over the vertices
                        for (auto& v : cvs->Vertices()) worst = std::max(worst, n[0] * v.X() + n[1] * v.Y() + n[2] * v.Z() + p.W());
                        CHECK(Near(worst, -0.05f, 1e-5f));
                    }
                }
                const CollisionGeometry g = ExtractCollision(*back);
                CHECK_EQ(g.hulls.size(), 2u);
            }

            // Attaching again replaces it; a fixed single hull has no list.
            auto again = AttachConvexCollision(*nif, 0, std::span<const ConvexHullInput>(hulls, 1));
            CHECK(again.has_value());
            if (again) {
                CHECK_EQ(again->removed, 5u);
                CHECK_EQ(again->shape, again->hulls[0]);
                CHECK_EQ(nif->blocks.size(), 6u);
                auto re = NifFile::Load(nif->Save());
                CHECK(re.has_value());
                if (re) {
                    const std::vector<Body> bodies = ExtractBodies(*re);
                    CHECK(bodies.size() == 1 && bodies[0].motion == BodyMotion::Fixed && bodies[0].layer == 1 &&
                          bodies[0].motionType == 5 && bodies[0].mass == 0.f && bodies[0].shapes.size() == 1);
                }
            }
            // Refusals leave the file alone.
            const std::vector<uint8_t> before = nif->Save();
            ConvexHullInput flat = BoxHull(0, 1, 0, 1, 0, 0);
            CHECK(!AttachConvexCollision(*nif, 0, std::span<const ConvexHullInput>(&flat, 1)).has_value());
            CHECK(!AttachConvexCollision(*nif, 1, std::span<const ConvexHullInput>(hulls, 1)).has_value());   // BSXFlags
            CollisionDesc noMass;
            noMass.motion = BodyMotion::Dynamic;
            CHECK(!AttachConvexCollision(*nif, 0, std::span<const ConvexHullInput>(hulls, 1), noMass).has_value());
            CHECK(nif->Save() == before);
        }
    }

    // ── N6: opaque bytes that could hide an index refuse renumbering ─────────
    {
        // BSMultiBoundAABB keeps its data as unclaimed opaque bytes; MOPP byte code claims `refs: none`.
        Bytes aabb;
        aabb.f32(0).f32(0).f32(0).f32(1).f32(1).f32(1);
        Bytes mopp;
        mopp.i32(-1).u32(0).u32(0).u32(0).f32(1).u32(2).f32(0).f32(0).f32(0).f32(1).u8(1).u8(0x28).u8(0);
        auto nif = Build({ { "BSFadeNode", Node(0, -1, {}) }, { "bhkMoppBvTreeShape", mopp.b }, { "BSMultiBoundAABB", aabb.b } },
                         { "Root" });
        CHECK(nif.has_value());
        if (nif) {
            CHECK_EQ(nif->RawCount(), 0u);
            CHECK(nif->blocks[1].OpaqueRefs().empty());
            CHECK_EQ(nif->blocks[2].OpaqueRefs(), std::string("data"));
            const std::vector<uint8_t> before = nif->Save();
            auto in = nif->InsertBlock(0, *Block::Create(schema::Main(), "BSXFlags"));
            CHECK(!in.has_value() && in.error().find("opaque bytes at data") != std::string::npos);
            CHECK(!nif->RemoveBlock(1).has_value());
            CHECK(nif->Save() == before);
            // Removing the block that holds them is fine, and so is appending.
            CHECK(nif->RemoveBlock(2).has_value());
            CHECK(nif->AppendBlock(*Block::Create(schema::Main(), "BSXFlags")).has_value());
            CHECK(nif->InsertBlock(0, *Block::Create(schema::Main(), "BSXFlags")).has_value());   // MOPP alone doesn't refuse
        }
    }

    // ── N6: a collision tree stored before its node ──────────────────────────
    {
        const BlockList blocks = {
            { "bhkBoxShape", Box(1, 1, 1) },                     // 0
            { "bhkRigidBody", FixedBody(0) },                    // 1
            { "bhkCollisionObject", CollisionObject(3, 1) },     // 2
            { "BSFadeNode", Node(0, 2, {}) },                    // 3 root
        };
        auto nif = Build(blocks, { "Root" }, 3);
        CHECK(nif.has_value());
        if (nif) {
            const ConvexHullInput hull = BoxHull(-1, 1, -1, 1, -1, 1);
            auto att = AttachConvexCollision(*nif, 3, std::span<const ConvexHullInput>(&hull, 1));
            CHECK(att.has_value());
            if (att) {
                CHECK_EQ(nif->roots[0], 0);   // the node moved down past the removed tree
                auto node = view::NiNode::From(nif->blocks[0]);
                CHECK(node && node->CollisionObject().index == att->collisionObject);
                auto co = view::bhkCollisionObject::From(nif->blocks[static_cast<size_t>(att->collisionObject)]);
                CHECK(co && co->Target().index == 0);
                auto back = NifFile::Load(nif->Save());
                CHECK(back.has_value() && ExtractBodies(*back).size() == 1 && ExtractBodies(*back)[0].targetNode == 0);
            }
        }
    }
}
