// ── Extra-data blocks (part of M5; BSXFlags landed early as the typed-pipeline
// proof) ──────────────────────────────────────────────────────────────────────
// Layouts derived from shipped SSE bytes (BSXFlags block size == 8 in
// clutter/barrel01.nif: NiExtraData name StringRef u32 + integerData u32) and
// gated by the corpus round-trip — not from nif.xml.
#include "Registry.h"
#include "Stream.h"

#include <niffer/Niffer.h>

namespace niffer {

void NiExtraData::Sync(Stream& s) {
    s.StringRef(name);
}

void BSXFlags::Sync(Stream& s) {
    NiExtraData::Sync(s);
    s.U32(integerData);
}

void NiStringExtraData::Sync(Stream& s) { NiExtraData::Sync(s); s.StringRef(stringData); }
void NiIntegerExtraData::Sync(Stream& s) { NiExtraData::Sync(s); s.U32(integerData); }
void NiFloatExtraData::Sync(Stream& s) { NiExtraData::Sync(s); s.F32(floatData); }
void NiBooleanExtraData::Sync(Stream& s) { NiExtraData::Sync(s); s.U8(booleanData); }

void NiBinaryExtraData::Sync(Stream& s) {
    NiExtraData::Sync(s);
    uint32_t n = static_cast<uint32_t>(binaryData.size());
    s.U32(n);
    size_t bytes = s.reading() ? n : binaryData.size();
    s.RawVector(binaryData, bytes);
}

void NiStringsExtraData::Sync(Stream& s) {
    NiExtraData::Sync(s);
    uint32_t n = static_cast<uint32_t>(data.size());
    s.U32(n);
    if (s.reading()) data.resize(n);
    for (auto& str : data) s.SizedString(str);
}

void BSBehaviorGraphExtraData::Sync(Stream& s) {
    NiExtraData::Sync(s);
    s.StringRef(behaviorGraphFile);
    s.U8(controlsBaseSkeleton);
}

void BSInvMarker::Sync(Stream& s) {
    NiExtraData::Sync(s);
    s.U16(rotationX); s.U16(rotationY); s.U16(rotationZ);
    s.F32(zoom);
}

void BSBound::Sync(Stream& s) {
    NiExtraData::Sync(s);
    s.Vec3v(center);
    s.Vec3v(dimensions);
}

void NiTextKeyExtraData::Sync(Stream& s) {
    NiExtraData::Sync(s);
    uint32_t n = static_cast<uint32_t>(keys.size());
    s.U32(n);
    if (s.reading()) keys.resize(n);
    for (auto& k : keys) { s.F32(k.time); s.StringRef(k.value); }
}

void BSBoneLODExtraData::Sync(Stream& s) {
    NiExtraData::Sync(s);
    uint32_t n = static_cast<uint32_t>(boneLODs.size());
    s.U32(n);
    if (s.reading()) boneLODs.resize(n);
    for (auto& b : boneLODs) { s.U32(b.distance); s.StringRef(b.boneName); }
}

void RegisterExtra(NifRegistry& reg) {
    RegisterBlock<BSXFlags>(reg, "BSXFlags");
    RegisterBlock<NiStringExtraData>(reg, "NiStringExtraData");
    RegisterBlock<NiIntegerExtraData>(reg, "NiIntegerExtraData");
    RegisterBlock<NiFloatExtraData>(reg, "NiFloatExtraData");
    RegisterBlock<NiBooleanExtraData>(reg, "NiBooleanExtraData");
    RegisterBlock<NiBinaryExtraData>(reg, "NiBinaryExtraData");
    RegisterBlock<NiStringsExtraData>(reg, "NiStringsExtraData");
    RegisterBlock<BSBehaviorGraphExtraData>(reg, "BSBehaviorGraphExtraData");
    RegisterBlock<BSInvMarker>(reg, "BSInvMarker");
    RegisterBlock<BSBound>(reg, "BSBound");
    RegisterBlock<NiTextKeyExtraData>(reg, "NiTextKeyExtraData");
    RegisterBlock<BSBoneLODExtraData>(reg, "BSBoneLODExtraData");
    // DEFERRED (safe UnknownBlock): BSDecalPlacementVectorExtraData (61) — its
    // per-block points/normals arrays are ambiguous at the observed count of 1;
    // needs a multi-vector example to pin points-then-normals vs interleaved.
}

}  // namespace niffer
