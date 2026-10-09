// ── M2: geometry ──────────────────────────────────────────────────────────────
// BSTriShape confirmed byte-for-byte against clutter/barrel01.nif block 7 and
// gated across the corpus. Vertex/triangle data is stored raw (disk precision);
// the BSVertexDesc-driven attribute decode is a separate helper, not guessed
// here — a right-sized wrong layout would still round-trip and mislabel data.
#include "Registry.h"
#include "Stream.h"

#include <niffer/Niffer.h>

namespace niffer {

void BSTriShape::Sync(Stream& s) {
    NiAVObject::Sync(s);
    s.Vec3v(boundCenter);
    s.F32(boundRadius);
    s.Ref(skin);
    s.Ref(shaderProperty);
    s.Ref(alphaProperty);
    s.U64(vertexDesc);
    s.U16(numTriangles);
    s.U16(numVertices);
    s.U32(dataSize);
    size_t n = s.reading() ? dataSize : geometryData.size();
    s.RawVector(geometryData, n);
    s.U32(particleDataSize);
    // Particle/FX BSTriShapes carry particleDataSize>0 followed by
    // uint16[particleDataSize] (2 bytes each). Normal shapes have
    // particleDataSize==0 and read nothing here.
    size_t pn = s.reading() ? static_cast<size_t>(particleDataSize) * 2
                            : particleData.size();
    s.RawVector(particleData, pn);
}

void BSDynamicTriShape::Sync(Stream& s) {
    BSTriShape::Sync(s);
    s.U32(dynamicDataSize);
    size_t n = s.reading() ? dynamicDataSize : dynamicData.size();
    s.RawVector(dynamicData, n);
}

void RegisterGeometry(NifRegistry& reg) {
    RegisterBlock<BSTriShape>(reg, "BSTriShape");
    RegisterBlock<BSDynamicTriShape>(reg, "BSDynamicTriShape");
}

}  // namespace niffer
