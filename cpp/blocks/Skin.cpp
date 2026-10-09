// ── M4: skinning (legacy NiSkinInstance / NiSkinData / NiSkinPartition path) ──
// The only skin path present in the SSE vanilla corpus (no FO4 BSSkin::Instance).
// Layouts confirmed against actors/character/character assets/childhead.nif
// blocks 4 (BSDismemberSkinInstance), 5 (NiSkinData), 6 (NiSkinPartition).
#include "Registry.h"
#include "Stream.h"

#include <niffer/Niffer.h>

namespace niffer {

void NiSkinInstance::Sync(Stream& s) {
    s.Ref(data);
    s.Ref(skinPartition);
    s.Ref(skeletonRoot);
    s.RefArray(bones);
}

void BSDismemberSkinInstance::Sync(Stream& s) {
    NiSkinInstance::Sync(s);
    uint32_t count = static_cast<uint32_t>(partitions.size());
    s.U32(count);
    if (s.reading()) partitions.resize(count);
    for (auto& p : partitions) { s.U16(p.partFlag); s.U16(p.bodyPart); }
}

void NiSkinData::Sync(Stream& s) {
    s.Transformv(skinTransform);
    uint32_t numBones = static_cast<uint32_t>(bones.size());
    s.U32(numBones);
    s.U8(hasVertexWeights);
    if (s.reading()) bones.resize(numBones);
    for (auto& b : bones) {
        s.Transformv(b.transform);
        s.Vec3v(b.boundingOffset);
        s.F32(b.boundingRadius);
        uint16_t numVerts = static_cast<uint16_t>(b.vertexWeights.size());
        s.U16(numVerts);
        if (hasVertexWeights) {
            if (s.reading()) b.vertexWeights.resize(numVerts);
            for (auto& w : b.vertexWeights) { s.U16(w.index); s.F32(w.weight); }
        }
    }
}

void NiSkinPartition::Sync(Stream& s) {
    s.U32(numPartitions);
    s.U32(dataSize);
    s.U32(vertexSize);
    s.U64(vertexDesc);
    size_t vn = s.reading() ? dataSize : vertexData.size();
    s.RawVector(vertexData, vn);
    size_t pn = s.reading() ? s.Remaining() : partitionData.size();
    s.RawVector(partitionData, pn);
}

void RegisterSkin(NifRegistry& reg) {
    RegisterBlock<NiSkinInstance>(reg, "NiSkinInstance");
    RegisterBlock<BSDismemberSkinInstance>(reg, "BSDismemberSkinInstance");
    RegisterBlock<NiSkinData>(reg, "NiSkinData");
    RegisterBlock<NiSkinPartition>(reg, "NiSkinPartition");
}

}  // namespace niffer
