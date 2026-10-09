// ── M6 (leaves): interpolators ────────────────────────────────────────────────
// The NiInterpolator leaf blocks — value + keyed-data ref. Layouts confirmed
// against shipped bytes and gated across the corpus. The keyed NiXxxData blocks
// and the controllers that drive these are the rest of M6 (not yet typed).
#include "Registry.h"
#include "Stream.h"

#include <niffer/Niffer.h>

namespace niffer {

void NiFloatInterpolator::Sync(Stream& s) { s.F32(value); s.Ref(data); }
void NiBoolInterpolator::Sync(Stream& s)  { s.U8(value);  s.Ref(data); }
void NiPoint3Interpolator::Sync(Stream& s){ s.Vec3v(value); s.Ref(data); }

void NiTransformInterpolator::Sync(Stream& s) {
    s.Vec3v(translation);
    s.Quatv(rotation);
    s.F32(scale);
    s.Ref(data);
}

void NiBlendInterpolator::Sync(Stream& s) {
    s.U8(flags);
    s.U8(arraySize);
    s.F32(weightThreshold);
}
void NiBlendFloatInterpolator::Sync(Stream& s) { NiBlendInterpolator::Sync(s); s.F32(value); }
void NiBlendBoolInterpolator::Sync(Stream& s)  { NiBlendInterpolator::Sync(s); s.U8(value); }
void NiBoolTimelineInterpolator::Sync(Stream& s) { s.U8(value); s.Ref(data); }

// Shared NiAnimationKeyGroup body: numKeys, then interpolation (only if
// numKeys>0), then the type/channel-dependent keys captured raw. Byte-exact.
static void SyncKeyGroup(Stream& s, uint32_t& numKeys, uint32_t& interpolation,
                         std::vector<uint8_t>& keyData) {
    s.U32(numKeys);
    if (numKeys > 0) s.U32(interpolation);
    size_t n = s.reading() ? s.Remaining() : keyData.size();
    s.RawVector(keyData, n);
}

void NiFloatData::Sync(Stream& s) { SyncKeyGroup(s, numKeys, interpolation, keyData); }
void NiBoolData::Sync(Stream& s)  { SyncKeyGroup(s, numKeys, interpolation, keyData); }
void NiPosData::Sync(Stream& s)   { SyncKeyGroup(s, numKeys, interpolation, keyData); }
void NiColorData::Sync(Stream& s) { SyncKeyGroup(s, numKeys, interpolation, keyData); }

void NiTimeController::Sync(Stream& s) {
    s.Ref(nextController);
    s.U16(flags);
    s.F32(frequency); s.F32(phase); s.F32(startTime); s.F32(stopTime);
    s.Ref(target);
}
void NiSingleInterpController::Sync(Stream& s) {
    NiTimeController::Sync(s);
    s.Ref(interpolator);
}
void BSLightingShaderPropertyFloatController::Sync(Stream& s) { NiSingleInterpController::Sync(s); s.U32(targetVariable); }
void BSLightingShaderPropertyColorController::Sync(Stream& s) { NiSingleInterpController::Sync(s); s.U32(targetVariable); }
void BSEffectShaderPropertyFloatController::Sync(Stream& s)   { NiSingleInterpController::Sync(s); s.U32(targetVariable); }
void BSEffectShaderPropertyColorController::Sync(Stream& s)   { NiSingleInterpController::Sync(s); s.U32(targetVariable); }

void NiControllerManager::Sync(Stream& s) {
    NiTimeController::Sync(s);
    s.U8(cumulative);
    s.RefArray(controllerSequences);
    s.Ref(objectPalette);
}
void NiMultiTargetTransformController::Sync(Stream& s) {
    NiTimeController::Sync(s);
    uint16_t n = static_cast<uint16_t>(extraTargets.size());
    s.U16(n);
    if (s.reading()) extraTargets.resize(n);
    for (auto& r : extraTargets) s.Ref(r);
}

void NiControllerSequence::Sync(Stream& s) {
    s.StringRef(name);
    s.U32(numControlledBlocks);
    size_t n = s.reading() ? s.Remaining() : tail.size();
    s.RawVector(tail, n);
}
void NiTransformData::Sync(Stream& s) {
    s.U32(numRotationKeys);
    size_t n = s.reading() ? s.Remaining() : tail.size();
    s.RawVector(tail, n);
}

void RegisterAnim(NifRegistry& reg) {
    RegisterBlock<NiFloatData>(reg, "NiFloatData");
    RegisterBlock<NiTransformData>(reg, "NiTransformData");
    RegisterBlock<NiTransformController>(reg, "NiTransformController");
    RegisterBlock<NiVisController>(reg, "NiVisController");
    RegisterBlock<BSNiAlphaPropertyTestRefController>(reg, "BSNiAlphaPropertyTestRefController");
    RegisterBlock<NiControllerManager>(reg, "NiControllerManager");
    RegisterBlock<NiControllerSequence>(reg, "NiControllerSequence");
    RegisterBlock<NiMultiTargetTransformController>(reg, "NiMultiTargetTransformController");
    RegisterBlock<BSLightingShaderPropertyFloatController>(reg, "BSLightingShaderPropertyFloatController");
    RegisterBlock<BSLightingShaderPropertyColorController>(reg, "BSLightingShaderPropertyColorController");
    RegisterBlock<BSEffectShaderPropertyFloatController>(reg, "BSEffectShaderPropertyFloatController");
    RegisterBlock<BSEffectShaderPropertyColorController>(reg, "BSEffectShaderPropertyColorController");
    RegisterBlock<NiBoolData>(reg, "NiBoolData");
    RegisterBlock<NiPosData>(reg, "NiPosData");
    RegisterBlock<NiColorData>(reg, "NiColorData");
    RegisterBlock<NiFloatInterpolator>(reg, "NiFloatInterpolator");
    RegisterBlock<NiBoolInterpolator>(reg, "NiBoolInterpolator");
    RegisterBlock<NiPoint3Interpolator>(reg, "NiPoint3Interpolator");
    RegisterBlock<NiTransformInterpolator>(reg, "NiTransformInterpolator");
    RegisterBlock<NiBlendFloatInterpolator>(reg, "NiBlendFloatInterpolator");
    RegisterBlock<NiBlendBoolInterpolator>(reg, "NiBlendBoolInterpolator");
    RegisterBlock<NiBoolTimelineInterpolator>(reg, "NiBoolTimelineInterpolator");
}

}  // namespace niffer
