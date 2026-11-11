// ── ExtractBodies — one record per Havok rigid body ──────────────────────────
// The same walk and conventions as ExtractCollision (Collision.cpp), keeping
// each body apart and each leaf shape as what it is. Layouts are the schema's
// (schema/blocks/collision); the motion and layer readings are the ones
// bhkRigidBody's schema documents, with their evidence in
// docs/collision-provenance.md.
//
// Not on the round-trip path: read through the generated views.
#include "Collision.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace niffer {
namespace collision {

namespace {

std::array<float, 9> Rows(const M3& m) {
    return { m.m[0][0], m.m[0][1], m.m[0][2], m.m[1][0], m.m[1][1], m.m[1][2], m.m[2][0], m.m[2][1], m.m[2][2] };
}

// Havok's motion types (1 dynamic, 2 sphere inertia, 3 box inertia, 4 keyframed,
// 5 fixed, 6 thin box inertia), then the loader's rule for bodies of no mass
// (CB-SDK bhkRigidBody::LoadBinary).
BodyMotion MotionOf(uint8_t value, float mass, uint8_t layer) {
    BodyMotion m = BodyMotion::Unknown;
    switch (value) {
    case 5: m = BodyMotion::Fixed; break;
    case 4: m = BodyMotion::Keyframed; break;
    case 1:
    case 2:
    case 3:
    case 6: m = BodyMotion::Dynamic; break;
    default: break;
    }
    if (mass == 0.f) {
        if (layer != 2 && layer != 28) m = BodyMotion::Fixed;
        if (layer == 14 || layer == 29) m = BodyMotion::Keyframed;
    }
    return m;
}

class Walker {
public:
    Walker(NifFile& nif, const Options& opt, Body& body) : nif_(nif), opt_(opt), body_(body) {}

    // `toBody` maps the shape's space to body space (Havok units, no scale).
    void Shape(int32_t index, const Xform& toBody, std::vector<int32_t> path) {
        path.push_back(index);
        Block* b = nif_.GetBlock(index);
        if (!b) { Skip("(null)", "missing shape", toBody, path); return; }
        const std::string cls(b->TypeName());
        if (path.size() > 32) { Skip(cls, "shape nesting too deep", toBody, path); return; }
        if (!b->Modeled()) { Skip(cls, "block not modeled", toBody, path); return; }

        if (auto mopp = view::bhkMoppBvTreeShape::From(*b)) { Shape(mopp->Shape().index, toBody, path); return; }
        if (auto list = view::bhkListShape::From(*b)) {
            for (const auto& sub : list->SubShapes()) Shape(sub.index, toBody, path);
            return;
        }
        if (auto cts = view::bhkConvexTransformShape::From(*b)) {
            Shape(cts->Shape().index, Compose(toBody, HavokLocal(cts->Transform(), opt_)), path);
            return;
        }
        if (auto ts = view::bhkTransformShape::From(*b)) {
            Shape(ts->Shape().index, Compose(toBody, HavokLocal(ts->Transform(), opt_)), path);
            return;
        }

        BodyShape s = Leaf(cls, toBody, path);
        if (view::bhkCompressedMeshShape::From(*b)) {
            DecodedCompressedMesh m;
            if (const char* why = DecodeCompressedMesh(nif_, *b, m)) { Skip(cls, why, toBody, path); return; }
            s.kind = BodyShape::Kind::Mesh;
            s.vertices.reserve(m.vertices.size() * 3);
            for (const V3& p : m.vertices) s.vertices.insert(s.vertices.end(), p.begin(), p.end());
            s.triangles         = std::move(m.triangles);
            s.triangleMaterials = std::move(m.triangleMaterials);
        } else if (auto cvs = view::bhkConvexVerticesShape::From(*b)) {
            s.kind     = BodyShape::Kind::ConvexVertices;
            s.material = cvs->Material();
            s.radius   = cvs->Radius();
            for (const auto& v : cvs->Vertices()) {
                const auto p = Read4(v);
                s.vertices.insert(s.vertices.end(), { p[0], p[1], p[2] });
            }
            if (s.vertices.empty()) { Skip(cls, "no points", toBody, path); return; }
        } else if (auto box = view::bhkBoxShape::From(*b)) {
            s.kind        = BodyShape::Kind::Box;
            s.material    = box->Material();
            s.radius      = box->Radius();
            s.halfExtents = Read3(box->HalfExtents());
        } else if (auto sph = view::bhkSphereShape::From(*b)) {
            s.kind     = BodyShape::Kind::Sphere;
            s.material = sph->Material();
            s.radius   = sph->Radius();
        } else if (auto cap = view::bhkCapsuleShape::From(*b)) {
            s.kind     = BodyShape::Kind::Capsule;
            s.material = cap->Material();
            s.radius   = cap->Radius();
            s.point1   = Read3(cap->Point1());
            s.point2   = Read3(cap->Point2());
        } else {
            // bhkCylinderShape (its radius word isn't established), bhkPlaneShape
            // (one sample), bhkNiTriStripsShape (NiTriStripsData is still opaque).
            Skip(cls, "not decoded", toBody, path);
            return;
        }
        if (!Finite(s)) { Skip(cls, "non-finite value", toBody, path); return; }
        body_.shapes.push_back(std::move(s));
    }

private:
    static BodyShape Leaf(const std::string& cls, const Xform& toBody, const std::vector<int32_t>& path) {
        BodyShape s;
        s.className   = cls;
        s.path        = path;
        s.rotation    = Rows(toBody.R);
        s.translation = toBody.t;
        return s;
    }

    void Skip(const std::string& cls, const char* why, const Xform& toBody, const std::vector<int32_t>& path) {
        BodyShape s = Leaf(cls, toBody, path);
        s.skipReason = why;
        body_.shapes.push_back(std::move(s));
        ++body_.shapesSkipped;
    }

    static bool Finite(const BodyShape& s) {
        auto ok = [](float v) { return std::isfinite(v); };
        for (float v : s.vertices) if (!ok(v)) return false;
        for (float v : s.halfExtents) if (!ok(v)) return false;
        for (float v : s.point1) if (!ok(v)) return false;
        for (float v : s.point2) if (!ok(v)) return false;
        return ok(s.radius);
    }

    NifFile&       nif_;
    const Options& opt_;
    Body&          body_;
};

}  // namespace

std::vector<Body> Bodies(NifFile& nif, const Options& opt) {
    std::vector<Body> out;
    const WorldTransforms wt = ComputeWorldTransforms(nif, opt);
    for (size_t i = 0; i < nif.blocks.size(); ++i) {
        auto co = view::bhkCollisionObject::From(nif.blocks[i]);
        if (!co) continue;
        Block* bb = nif.GetBlock(co->Body().index);
        if (!bb) continue;
        auto rb = view::bhkRigidBody::From(*bb);
        if (!rb) continue;   // phantoms are trigger volumes, not bodies

        Body body;
        body.collisionObject      = static_cast<int32_t>(i);
        body.collisionObjectClass = std::string(nif.blocks[i].TypeName());
        body.collisionObjectFlags = co->Flags();
        body.body                 = co->Body().index;
        body.bodyClass            = std::string(bb->TypeName());
        body.targetNode           = co->Target().index;
        if (Block* tb = nif.GetBlock(body.targetNode))
            if (auto net = view::NiObjectNET::From(*tb)) body.targetName = std::string(nif.String(net->Name()));

        // Placement: the target node in root space, Havok -> Skyrim units, and for
        // bhkRigidBodyT its own translation and rotation (the measured conventions).
        Xform node;
        const int32_t target = body.targetNode;
        if (opt.nodeTransform && target >= 0 && static_cast<size_t>(target) < wt.valid.size() &&
            wt.valid[static_cast<size_t>(target)])
            node = wt.world[static_cast<size_t>(target)];
        Xform scale;
        scale.s     = kHavokToSkyrim;
        Xform place = Compose(node, scale);
        const bool isT = view::bhkRigidBodyT::From(*bb).has_value();
        if (opt.bodyTransform == 2 || (opt.bodyTransform == 1 && isT)) {
            const auto q = Read4(rb->Rotation());
            const auto t = Read4(rb->Translation());
            Xform bx;
            bx.R  = opt.quatConjugate ? QuatMatrix(-q[0], -q[1], -q[2], q[3]) : QuatMatrix(q[0], q[1], q[2], q[3]);
            bx.t  = { t[0], t[1], t[2] };
            place = Compose(place, bx);
        }
        body.rotation    = Rows(place.R);
        body.translation = place.t;
        body.scale       = place.s;

        body.collisionFilter = rb->CollisionFilter();
        body.layer           = static_cast<uint8_t>(body.collisionFilter & 0x7Fu);
        body.motionType      = rb->MotionType();
        body.mass            = rb->Mass();
        body.motion          = MotionOf(body.motionType, body.mass, body.layer);
        body.qualityType     = rb->QualityType();
        body.linearDamping   = rb->LinearDamping();
        body.angularDamping  = rb->AngularDamping();
        body.friction        = rb->Friction();
        body.restitution     = rb->Restitution();
        body.gravityFactor   = rb->GravityFactor();
        body.maxLinearVelocity       = std::min(rb->MaxLinearVelocity(), 100.f);   // the loader's clamp
        body.maxAngularVelocity      = rb->MaxAngularVelocity();
        body.allowedPenetrationDepth = rb->AllowedPenetrationDepth();
        const auto rows      = rb->Inertia();
        for (size_t r = 0; r < rows.size() && r < 3; ++r) {
            const auto v = Read4(rows[r]);
            for (size_t c = 0; c < 3; ++c) body.inertia[3 * r + c] = v[c];
        }
        const auto com = Read4(rb->CenterOfMass());
        body.centerOfMass = { com[0], com[1], com[2] };

        Walker(nif, opt, body).Shape(rb->Shape().index, Xform{}, {});
        out.push_back(std::move(body));
    }
    return out;
}

}  // namespace collision

std::vector<Body> ExtractBodies(NifFile& nif) {
    return collision::Bodies(nif, collision::Options{});
}

}  // namespace niffer
