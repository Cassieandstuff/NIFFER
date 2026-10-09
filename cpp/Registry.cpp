#include "Registry.h"

namespace niffer {

NifRegistry& NifRegistry::Instance() {
    static NifRegistry inst;
    return inst;
}

void NifRegistry::Register(std::string_view typeName, Factory f) {
    m_factories.emplace(std::string(typeName), std::move(f));
}

std::unique_ptr<NiObject> NifRegistry::Create(std::string_view typeName) const {
    auto it = m_factories.find(std::string(typeName));
    return it == m_factories.end() ? nullptr : it->second();
}

bool NifRegistry::Has(std::string_view typeName) const {
    return m_factories.contains(std::string(typeName));
}

// Central registration. Explicit calls (not static-init) so no blocks/ TU can
// be dropped by the static linker. Add each new area's registrar here.
void EnsureBlocksRegistered() {
    static bool done = false;
    if (done) return;
    done = true;
    NifRegistry& reg = NifRegistry::Instance();
    RegisterExtra(reg);
    RegisterNodes(reg);
    RegisterGeometry(reg);
    RegisterShaders(reg);
    RegisterSkin(reg);
    RegisterAnim(reg);
    RegisterCollision(reg);
    RegisterParticles(reg);
}

}  // namespace niffer
