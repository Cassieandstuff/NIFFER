// ── M1: core scene graph ──────────────────────────────────────────────────────
// NiObjectNET / NiAVObject / NiNode layout confirmed byte-for-byte against
// clutter/barrel01.nif's 88-byte BSFadeNode (name ref, 1 extra→BSXFlags,
// controller -1; flags u32, translation, 3x3 rotation, scale; collision→block 6;
// 1 child→block 7, 0 effects) and gated by the corpus round-trip — not nif.xml.
#include "Registry.h"
#include "Stream.h"

#include <niffer/Niffer.h>

namespace niffer {

void NiObjectNET::Sync(Stream& s) {
    s.StringRef(name);
    s.RefArray(extraData);
    s.Ref(controller);
}

void NiAVObject::Sync(Stream& s) {
    NiObjectNET::Sync(s);
    s.U32(flags);
    s.AvTransformv(transform);   // Translation, Rotation, Scale (NOT the compound order)
    s.Ref(collisionObject);
}

void NiNode::Sync(Stream& s) {
    NiAVObject::Sync(s);
    s.RefArray(children);
    s.RefArray(effects);
}

void NiBillboardNode::Sync(Stream& s) {
    NiNode::Sync(s);
    s.U16(billboardMode);
}

void BSValueNode::Sync(Stream& s) {
    NiNode::Sync(s);
    s.U32(value);
    s.U8(valueNodeFlags);
}

void BSOrderedNode::Sync(Stream& s) {
    NiNode::Sync(s);
    s.Vec4v(alphaSortBound);
    s.U8(staticBound);
}

void BSMultiBoundNode::Sync(Stream& s) {
    NiNode::Sync(s);
    s.Ref(multiBound);
    s.U32(cullingMode);
}

void NiSwitchNode::Sync(Stream& s) {
    NiNode::Sync(s);
    s.U16(switchFlags);
    s.U32(index);
}

void BSTreeNode::Sync(Stream& s) {
    NiNode::Sync(s);
    s.RefArray(bones1);
    s.RefArray(bones2);
}

// DEFERRED (round-trip-safe as UnknownBlock until their field semantics are
// verified, not merely size-matched — a wrong layout that consumes the right
// byte count still round-trips, so the gate can't catch a mislabel):
//   BSFurnitureMarkerNode (243) — 28 bytes, does NOT derive from NiNode
//     (NiNode min is 84); its small layout needs proper derivation.
//   BSBlastNode (31) — layout unverified.
// Both are niche; typing them semantically is tracked with the M8 long tail.

void RegisterNodes(NifRegistry& reg) {
    RegisterBlock<NiNode>(reg, "NiNode");
    RegisterBlock<BSFadeNode>(reg, "BSFadeNode");
    RegisterBlock<BSLeafAnimNode>(reg, "BSLeafAnimNode");
    RegisterBlock<NiBillboardNode>(reg, "NiBillboardNode");
    RegisterBlock<BSValueNode>(reg, "BSValueNode");
    RegisterBlock<BSOrderedNode>(reg, "BSOrderedNode");
    RegisterBlock<BSMultiBoundNode>(reg, "BSMultiBoundNode");
    RegisterBlock<NiSwitchNode>(reg, "NiSwitchNode");
    RegisterBlock<BSTreeNode>(reg, "BSTreeNode");
}

}  // namespace niffer
