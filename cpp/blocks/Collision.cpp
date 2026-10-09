// ── M7: Havok collision (bhk*) ────────────────────────────────────────────────
// Typed at the structural level — linking refs decoded (graph stays walkable),
// Havok physics/geometry payloads kept raw (byte-exact). The SCT consumer does
// not read collision, so full physics decode is intentionally out of scope.
// Leading layouts confirmed against barrel01 / cookingspit / basket / eggs.
#include "Registry.h"
#include "Stream.h"

#include <niffer/Niffer.h>

namespace niffer {

void bhkCollisionObject::Sync(Stream& s) {
    s.Ref(target);
    s.U16(flags);
    s.Ref(body);
}

void bhkShapeWrapper::Sync(Stream& s) {
    s.Ref(shape);
    size_t n = s.reading() ? s.Remaining() : data.size();
    s.RawVector(data, n);
}

void bhkPrimitiveShape::Sync(Stream& s) {
    s.U32(material);
    s.F32(radius);
    size_t n = s.reading() ? s.Remaining() : data.size();
    s.RawVector(data, n);
}

void bhkListShape::Sync(Stream& s) {
    s.RefArray(subShapes);
    size_t n = s.reading() ? s.Remaining() : data.size();
    s.RawVector(data, n);
}

void bhkRawBlock::Sync(Stream& s) {
    size_t n = s.reading() ? s.Remaining() : data.size();
    s.RawVector(data, n);
}

void RegisterCollision(NifRegistry& reg) {
    RegisterBlock<bhkCollisionObject>(reg, "bhkCollisionObject");
    RegisterBlock<bhkSPCollisionObject>(reg, "bhkSPCollisionObject");

    RegisterBlock<bhkRigidBody>(reg, "bhkRigidBody");
    RegisterBlock<bhkRigidBodyT>(reg, "bhkRigidBodyT");
    RegisterBlock<bhkMoppBvTreeShape>(reg, "bhkMoppBvTreeShape");
    RegisterBlock<bhkConvexTransformShape>(reg, "bhkConvexTransformShape");
    RegisterBlock<bhkTransformShape>(reg, "bhkTransformShape");
    RegisterBlock<bhkCompressedMeshShape>(reg, "bhkCompressedMeshShape");
    RegisterBlock<bhkNiTriStripsShape>(reg, "bhkNiTriStripsShape");

    RegisterBlock<bhkSphereShape>(reg, "bhkSphereShape");
    RegisterBlock<bhkBoxShape>(reg, "bhkBoxShape");
    RegisterBlock<bhkCapsuleShape>(reg, "bhkCapsuleShape");
    RegisterBlock<bhkCylinderShape>(reg, "bhkCylinderShape");
    RegisterBlock<bhkConvexVerticesShape>(reg, "bhkConvexVerticesShape");

    RegisterBlock<bhkListShape>(reg, "bhkListShape");

    RegisterBlock<bhkCompressedMeshShapeData>(reg, "bhkCompressedMeshShapeData");
    RegisterBlock<bhkSimpleShapePhantom>(reg, "bhkSimpleShapePhantom");
    RegisterBlock<bhkPlaneShape>(reg, "bhkPlaneShape");
    RegisterBlock<bhkBlendCollisionObject>(reg, "bhkBlendCollisionObject");
    RegisterBlock<bhkLimitedHingeConstraint>(reg, "bhkLimitedHingeConstraint");
    RegisterBlock<bhkRagdollConstraint>(reg, "bhkRagdollConstraint");
    RegisterBlock<bhkHingeConstraint>(reg, "bhkHingeConstraint");
    RegisterBlock<bhkBallAndSocketConstraint>(reg, "bhkBallAndSocketConstraint");
    RegisterBlock<bhkBallSocketConstraintChain>(reg, "bhkBallSocketConstraintChain");
    RegisterBlock<bhkStiffSpringConstraint>(reg, "bhkStiffSpringConstraint");
    RegisterBlock<bhkBreakableConstraint>(reg, "bhkBreakableConstraint");
}

}  // namespace niffer
