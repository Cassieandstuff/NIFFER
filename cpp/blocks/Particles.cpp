// ── M8: particles + long tail ─────────────────────────────────────────────────
// Structural typing to reach zero unknowns: shared leading fields decoded,
// payloads raw (byte-exact). SCT does not render particles. Bases confirmed
// against ashpileghost / bfxsmoke / ashpile blocks.
#include "Registry.h"
#include "Stream.h"

#include <niffer/Niffer.h>

namespace niffer {

static void RawTail(Stream& s, std::vector<uint8_t>& v) {
    size_t n = s.reading() ? s.Remaining() : v.size();
    s.RawVector(v, n);
}

void NifNamedRaw::Sync(Stream& s) { RawTail(s, data); }
void NifNamedObjectNET::Sync(Stream& s) { NiObjectNET::Sync(s); RawTail(s, tail); }
void NifNamedAVObject::Sync(Stream& s) { NiAVObject::Sync(s); RawTail(s, tail); }
void NifNamedTimeController::Sync(Stream& s) { NiTimeController::Sync(s); RawTail(s, tail); }

void NifNamedPSysModifier::Sync(Stream& s) {
    s.StringRef(name);
    s.U32(order);
    s.Ref(target);
    s.U8(active);
    RawTail(s, tail);
}

void NiBlendPoint3Interpolator::Sync(Stream& s) {
    NiBlendInterpolator::Sync(s);
    s.Vec3v(value);
}

void RegisterParticles(NifRegistry& reg) {
    // Particle modifiers + emitters (NiPSysModifier base).
    for (const char* n : {
        "NiPSysDragModifier", "NiPSysSpawnModifier", "BSPSysLODModifier",
        "NiPSysAgeDeathModifier", "NiPSysPositionModifier", "NiPSysBoundUpdateModifier",
        "BSPSysSimpleColorModifier", "BSPSysScaleModifier", "NiPSysGravityModifier",
        "NiPSysRotationModifier", "BSPSysSubTexModifier", "NiPSysBombModifier",
        "BSPSysInheritVelocityModifier", "BSPSysStripUpdateModifier",
        "BSPSysRecycleBoundModifier", "BSWindModifier", "NiPSysColliderManager",
        "NiPSysMeshEmitter", "NiPSysBoxEmitter", "NiPSysCylinderEmitter",
        "NiPSysSphereEmitter", "BSPSysHavokUpdateModifier" })
        RegisterNamed<NifNamedPSysModifier>(reg, n);

    // Particle + misc controllers (NiTimeController base).
    for (const char* n : {
        "NiPSysModifierActiveCtlr", "NiPSysUpdateCtlr", "NiPSysEmitterCtlr",
        "NiPSysEmitterSpeedCtlr", "NiPSysGravityStrengthCtlr",
        "NiPSysEmitterInitialRadiusCtlr", "BSPSysMultiTargetEmitterCtlr",
        "NiPSysEmitterLifeSpanCtlr", "NiPSysEmitterDeclinationCtlr",
        "BSLagBoneController", "NiFloatExtraDataController", "BSFrustumFOVController",
        "BSProceduralLightningController", "NiPSysEmitterPlanarAngleCtlr",
        "NiBSBoneLODController", "NiPSysInitialRotSpeedCtlr" })
        RegisterNamed<NifNamedTimeController>(reg, n);

    // NiAVObject-derived (particle systems, camera, legacy shapes, misc nodes).
    for (const char* n : {
        "NiParticleSystem", "BSStripParticleSystem", "BSMasterParticleSystem",
        "NiCamera", "NiTriShape", "BSLODTriShape", "BSBlastNode", "BSDamageStage",
        "NiMeshParticleSystem" })
        RegisterNamed<NifNamedAVObject>(reg, n);

    // NiObjectNET-derived shader properties (deferred from M3).
    for (const char* n : {
        "BSEffectShaderProperty", "BSSkyShaderProperty", "BSWaterShaderProperty" })
        RegisterNamed<NifNamedObjectNET>(reg, n);

    // Pure raw carriers (data blocks, colliders, bounds, misc).
    for (const char* n : {
        "NiPSysData", "BSStripPSysData", "NiTriShapeData", "NiTriStripsData",
        "NiPSysPlanarCollider", "NiPSysSphericalCollider", "NiDefaultAVObjectPalette",
        "BSMultiBound", "BSMultiBoundOBB", "BSMultiBoundAABB", "BSFurnitureMarkerNode",
        "BSDecalPlacementVectorExtraData", "NiPathInterpolator", "NiLookAtInterpolator",
        "NiMeshPSysData" })
        RegisterNamed<NifNamedRaw>(reg, n);

    RegisterBlock<NiBlendPoint3Interpolator>(reg, "NiBlendPoint3Interpolator");
}

}  // namespace niffer
