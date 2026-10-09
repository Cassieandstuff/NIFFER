// ── M3: shaders / materials ───────────────────────────────────────────────────
// Layouts confirmed against shipped bytes and gated across the corpus.
//   BSShaderTextureSet: barrel01 block 9 (u32 count + SizedString per slot).
//   NiAlphaProperty:    akatoshamuletf block 10 (NiObjectNET + u16 flags + u8).
#include "Registry.h"
#include "Stream.h"

#include <niffer/Niffer.h>

namespace niffer {

void BSShaderTextureSet::Sync(Stream& s) {
    uint32_t count = static_cast<uint32_t>(textures.size());
    s.U32(count);
    if (s.reading()) textures.resize(count);
    for (auto& t : textures) s.SizedString(t);
}

void NiAlphaProperty::Sync(Stream& s) {
    NiObjectNET::Sync(s);
    s.U16(flags);
    s.U8(threshold);
}

void BSLightingShaderProperty::Sync(Stream& s) {
    s.StringRef(name);
    s.U32(headerA);
    s.U32(headerB);
    s.U32(headerC);
    s.U32(shaderFlags1);
    s.U32(shaderFlags2);
    s.Vec2v(uvOffset);
    s.Vec2v(uvScale);
    s.Ref(textureSet);
    size_t n = s.reading() ? s.Remaining() : materialTail.size();
    s.RawVector(materialTail, n);
}

void RegisterShaders(NifRegistry& reg) {
    RegisterBlock<BSShaderTextureSet>(reg, "BSShaderTextureSet");
    RegisterBlock<NiAlphaProperty>(reg, "NiAlphaProperty");
    RegisterBlock<BSLightingShaderProperty>(reg, "BSLightingShaderProperty");
}

}  // namespace niffer
