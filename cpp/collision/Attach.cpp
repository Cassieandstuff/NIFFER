// ── AttachConvexCollision — authoring a rigid body from convex hulls ─────────
// Builds the blocks through Block::Create and the schema's field names, and
// inserts them with NifFile's editing API. The values written are the vanilla
// ones bhkRigidBody's, bhkConvexVerticesShape's and bhkListShape's schema docs
// record (clutter/barrel01.nif for a fixed body, clutter/common/broom.nif for a
// dynamic one).
#include <niffer/Niffer.h>

#include <array>
#include <bit>
#include <cmath>

namespace niffer {

namespace {

using V3 = std::array<double, 3>;

V3 Sub(const V3& a, const V3& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
double Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Cross(const V3& a, const V3& b) {
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}

Value F32(float f) {
    Value v;
    v.bits = std::bit_cast<uint32_t>(f);
    return v;
}
Value Vec4(float x, float y, float z, float w) {
    Value v;
    v.items = { F32(x), F32(y), F32(z), F32(w) };
    return v;
}

// Field setters by schema path; every path below is in the schema, so a miss is a bug.
struct Fields {
    Block&       b;
    std::string* error;
    bool Path(std::string_view path, RelPath& p) {
        auto found = b.Find(path);
        if (!found) {
            if (error->empty()) *error = std::string(b.TypeName()) + " has no field " + std::string(path);
            return false;
        }
        p = *found;
        return true;
    }
    template <class T> void Set(std::string_view path, const T& v) {
        RelPath p;
        if (Path(path, p) && !b.SetAt<T>(p, v) && error->empty())
            *error = "can't set " + std::string(b.TypeName()) + "." + std::string(path);
    }
    void Store(std::string_view path, Value v) {
        RelPath p;
        if (Path(path, p) && !b.StoreValue(p, std::move(v)) && error->empty())
            *error = "can't set " + std::string(b.TypeName()) + "." + std::string(path);
    }
};

// One hull in Havok units: its points, the outward planes of its faces (grown
// by `radius`), and its volume integrals.
struct Hull {
    std::vector<V3>                 points;
    std::vector<std::array<double, 4>> planes;
    double volume = 0;
    V3     first{};     // ∫ x dV
    std::array<double, 9> second{};   // ∫ x x^T dV
};

std::expected<Hull, std::string> MakeHull(const ConvexHullInput& in, size_t which, double radius) {
    const std::string name = "hull " + std::to_string(which);
    if (in.points.size() < 12 || in.points.size() % 3) return std::unexpected(name + ": fewer than 4 points");
    if (in.triangles.size() < 12 || in.triangles.size() % 3) return std::unexpected(name + ": fewer than 4 faces");
    Hull h;
    const size_t np = in.points.size() / 3;
    V3 centre{};
    for (size_t i = 0; i < np; ++i) {
        V3 p{ in.points[i * 3] / double(kHavokToSkyrim), in.points[i * 3 + 1] / double(kHavokToSkyrim),
              in.points[i * 3 + 2] / double(kHavokToSkyrim) };
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            return std::unexpected(name + ": a point isn't finite");
        h.points.push_back(p);
        for (int k = 0; k < 3; ++k) centre[k] += p[k] / double(np);
    }
    for (size_t t = 0; t < in.triangles.size(); t += 3) {
        uint32_t i0 = in.triangles[t], i1 = in.triangles[t + 1], i2 = in.triangles[t + 2];
        if (i0 >= np || i1 >= np || i2 >= np) return std::unexpected(name + ": a face index is past the points");
        V3 a = h.points[i0], b = h.points[i1], c = h.points[i2];
        V3 n = Cross(Sub(b, a), Sub(c, a));
        const double len = std::sqrt(Dot(n, n));
        if (len < 1e-12) continue;   // a degenerate face bounds nothing
        // Outward: away from the hull's centre, whichever way the face winds.
        if (Dot(n, Sub(a, centre)) < 0) {
            std::swap(b, c);
            for (double& x : n) x = -x;
        }
        // Volume integrals over the tetrahedron (0, a, b, c).
        const double det = Dot(a, Cross(b, c));
        h.volume += det / 6.0;
        const V3 s{ a[0] + b[0] + c[0], a[1] + b[1] + c[1], a[2] + b[2] + c[2] };
        for (int k = 0; k < 3; ++k) h.first[k] += det / 24.0 * s[k];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k)
                h.second[r * 3 + k] += det / 120.0 * (a[r] * a[k] + b[r] * b[k] + c[r] * c[k] + s[r] * s[k]);

        const V3 u{ n[0] / len, n[1] / len, n[2] / len };
        const double w = -Dot(u, a) - radius;
        bool same = false;
        for (const auto& q : h.planes)
            same = same || (std::abs(q[0] - u[0]) < 1e-5 && std::abs(q[1] - u[1]) < 1e-5 && std::abs(q[2] - u[2]) < 1e-5 &&
                            std::abs(q[3] - w) < 1e-5);
        if (!same) h.planes.push_back({ u[0], u[1], u[2], w });
    }
    if (h.planes.size() < 4 || h.volume <= 0) return std::unexpected(name + ": not a closed solid");
    return h;
}

}  // namespace

std::expected<size_t, std::string> RemoveCollision(NifFile& nif, int32_t node) {
    Block* b = nif.GetBlock(node);
    auto av = b ? view::NiAVObject::From(*b) : std::nullopt;
    if (!av) return std::unexpected("block " + std::to_string(node) + " isn't a scene object");
    const int32_t co = av->CollisionObject().index;
    if (co < 0) return 0;
    auto removed = nif.RemoveTree(co);
    return removed;   // the node's ref to it became null
}

std::expected<AttachedCollision, std::string> AttachConvexCollision(NifFile& nif, int32_t node,
                                                                    std::span<const ConvexHullInput> hulls,
                                                                    const CollisionDesc& desc) {
    {
        Block* b = nif.GetBlock(node);
        if (!b || !view::NiNode::From(*b)) return std::unexpected("block " + std::to_string(node) + " isn't a NiNode");
    }
    if (hulls.empty()) return std::unexpected("no hulls");
    const schema::SchemaSet& set = schema::Main();

    // Validate everything before touching the file.
    std::vector<Hull> made;
    for (size_t i = 0; i < hulls.size(); ++i) {
        auto h = MakeHull(hulls[i], i, desc.radius);
        if (!h) return std::unexpected(h.error());
        made.push_back(std::move(*h));
    }
    const BodyMotion motion = desc.motion != BodyMotion::Unknown ? desc.motion
                              : desc.mass > 0                   ? BodyMotion::Dynamic
                                                                : BodyMotion::Fixed;
    if (motion == BodyMotion::Dynamic && !(desc.mass > 0)) return std::unexpected("a dynamic body needs a mass");
    const int layer = desc.layer >= 0                     ? desc.layer
                      : motion == BodyMotion::Dynamic     ? 4
                      : motion == BodyMotion::Keyframed   ? 2
                                                          : 1;
    if (layer > 127) return std::unexpected("layer " + std::to_string(layer) + " is past the 7-bit layer field");

    AttachedCollision out;
    // Mass properties (uniform density over all hulls).
    double volume = 0;
    V3 first{};
    std::array<double, 9> second{};
    for (const Hull& h : made) {
        volume += h.volume;
        for (int k = 0; k < 3; ++k) first[k] += h.first[k];
        for (int k = 0; k < 9; ++k) second[k] += h.second[k];
    }
    out.volume = static_cast<float>(volume);
    const bool hasMass = desc.mass > 0;
    if (hasMass) {
        const V3 com{ first[0] / volume, first[1] / volume, first[2] / volume };
        const double rho = desc.mass / volume;
        // Second moment about the centre of mass, then I = tr(C) Id - C.
        std::array<double, 9> c{};
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) c[r * 3 + k] = rho * (second[r * 3 + k] - volume * com[r] * com[k]);
        const double tr = c[0] + c[4] + c[8];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k)
                out.inertia[r * 3 + k] = static_cast<float>((r == k ? tr : 0.0) - c[r * 3 + k]);
        for (int k = 0; k < 3; ++k) out.centerOfMass[k] = static_cast<float>(com[k]);
        if (desc.centerOfMass) out.centerOfMass = *desc.centerOfMass;
        if (desc.inertia) out.inertia = *desc.inertia;
    }

    // What a replaced collision object takes with it. It is removed only once
    // the new blocks are built, so a failure leaves the file as it was; the new
    // blocks are numbered for the file after the removal.
    std::vector<int32_t> old;
    if (const int32_t co = view::NiAVObject::From(nif.blocks[static_cast<size_t>(node)])->CollisionObject().index;
        co >= 0 && static_cast<size_t>(co) < nif.blocks.size())
        old = nif.OwnedTree(co);
    int32_t below = 0;
    for (int32_t i : old) below += i < node ? 1 : 0;
    const int32_t newNode = node - below;
    std::string error;
    std::vector<Block> fresh;
    auto create = [&](std::string_view type) -> Block* {
        auto b = Block::Create(set, type);
        if (!b) {
            if (error.empty()) error = "no descriptor for " + std::string(type);
            return nullptr;
        }
        fresh.push_back(std::move(*b));
        return &fresh.back();
    };
    fresh.reserve(made.size() + 3);
    const auto base = static_cast<int32_t>(nif.blocks.size() - old.size());

    // Hulls.
    for (const Hull& h : made) {
        Block* b = create("bhkConvexVerticesShape");
        if (!b) break;
        Fields f{ *b, &error };
        f.Set<uint32_t>("material", desc.material);
        f.Set<float>("radius", desc.radius);
        f.Set<std::vector<uint32_t>>("unknown8", { 0, 0, 0x80000000u, 0, 0, 0x80000000u });
        Value verts, planes;
        for (const V3& p : h.points)
            verts.items.push_back(Vec4(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]), 0.f));
        for (const auto& q : h.planes)
            planes.items.push_back(Vec4(static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2]),
                                        static_cast<float>(q[3])));
        f.Store("vertices", std::move(verts));
        f.Store("planes", std::move(planes));
    }
    int32_t shape = base;
    if (error.empty() && made.size() > 1) {
        if (Block* b = create("bhkListShape")) {
            Fields f{ *b, &error };
            std::vector<NiRef<view::NiObject>> subs;
            for (size_t i = 0; i < made.size(); ++i) subs.push_back(NiRef<view::NiObject>{ { base + static_cast<int32_t>(i) } });
            f.Set("subShapes", subs);
            f.Set<uint32_t>("material", desc.material);
            f.Set<std::vector<uint32_t>>("unknown", { 0, 0, 0x80000000u, 0, 0, 0x80000000u });
            f.Set<std::vector<uint32_t>>("unknownList", std::vector<uint32_t>(made.size(), 0));
            shape = base + static_cast<int32_t>(made.size());
        }
    }
    const auto bodyIndex = base + static_cast<int32_t>(fresh.size());
    if (error.empty()) {
        if (Block* b = create("bhkRigidBody")) {
            Fields f{ *b, &error };
            const uint32_t filter = static_cast<uint32_t>(layer);
            f.Set("shape", NiRef<view::NiObject>{ { shape } });
            f.Set<uint32_t>("collisionFilter", filter);
            f.Set<uint8_t>("unknown12", 1);
            f.Set<std::vector<uint32_t>>("unknown16", { 0, 0, 0x80000000u });
            f.Set<uint32_t>("unknown28", 0xFFFF0001u);
            f.Set<uint32_t>("unknown36", filter);
            f.Set<uint8_t>("collisionResponse", 1);
            f.Set<uint16_t>("contactPointCallbackDelay", 0xFFFF);
            f.Store("rotation", Vec4(0, 0, 0, 1));
            if (hasMass) {
                Value inertia;
                for (int r = 0; r < 3; ++r)
                    inertia.items.push_back(Vec4(out.inertia[r * 3], out.inertia[r * 3 + 1], out.inertia[r * 3 + 2], 0));
                f.Store("inertia", std::move(inertia));
                f.Store("centerOfMass", Vec4(out.centerOfMass[0], out.centerOfMass[1], out.centerOfMass[2], 0));
                f.Set<float>("mass", desc.mass);
            }
            f.Set<float>("linearDamping", desc.linearDamping);
            f.Set<float>("angularDamping", desc.angularDamping);
            f.Set<float>("timeFactor", 1.f);
            f.Set<float>("gravityFactor", 1.f);
            f.Set<float>("friction", desc.friction);
            f.Set<float>("restitution", desc.restitution);
            const bool keyframedNoMass = motion == BodyMotion::Keyframed && !hasMass;
            f.Set<float>("maxLinearVelocity", keyframedNoMass ? 1000002.f : 104.4f);
            f.Set<float>("maxAngularVelocity", motion == BodyMotion::Dynamic ? 31.57f : 1000002.f);
            f.Set<float>("allowedPenetrationDepth", desc.allowedPenetrationDepth);
            // Havok's motion type and collidable quality per motion, as vanilla stores them:
            // dynamic 3 (box inertia) / 4 moving, keyframed 4 / 1, fixed 5 / 0.
            const uint8_t havokMotion = motion == BodyMotion::Dynamic ? 3 : motion == BodyMotion::Keyframed ? 4 : 5;
            const uint8_t quality     = motion == BodyMotion::Dynamic ? 4 : motion == BodyMotion::Keyframed ? 1 : 0;
            f.Set<uint8_t>("motionType", havokMotion);
            f.Set<uint8_t>("enableDeactivation", 1);
            f.Set<uint8_t>("solverDeactivation", motion == BodyMotion::Dynamic ? 2 : 1);
            f.Set<uint8_t>("qualityType", quality);
            f.Set<uint8_t>("numShapeKeysInContactPointProperties", 3);
        }
    }
    const auto objectIndex = base + static_cast<int32_t>(fresh.size());
    if (error.empty()) {
        if (Block* b = create("bhkCollisionObject")) {
            Fields f{ *b, &error };
            f.Set("target", NiRef<view::NiNode>{ { newNode } });
            f.Set<uint16_t>("flags", desc.collisionObjectFlags);
            f.Set("body", NiRef<view::bhkShapeWrapper>{ { bodyIndex } });
        }
    }
    if (!error.empty()) return std::unexpected(error);

    if (!old.empty()) {
        auto removed = nif.RemoveBlocks(old);
        if (!removed) return std::unexpected(removed.error());
        out.removed = *removed;
    }
    for (Block& b : fresh)
        if (auto r = nif.AppendBlock(std::move(b)); !r) return std::unexpected(r.error());
    auto nodeView = view::NiAVObject::From(nif.blocks[static_cast<size_t>(newNode)]);
    if (!nodeView || !nodeView->SetCollisionObject(NiRef<view::bhkCollisionObject>{ { objectIndex } }))
        return std::unexpected("can't set the node's collision object");

    out.collisionObject = objectIndex;
    out.body            = bodyIndex;
    out.shape           = shape;
    for (size_t i = 0; i < made.size(); ++i) out.hulls.push_back(base + static_cast<int32_t>(i));
    return out;
}

}  // namespace niffer
