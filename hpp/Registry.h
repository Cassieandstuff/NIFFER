#pragma once
// ── Registry — block-type name → factory ──────────────────────────────────────
// Model: havok-core's HavokRegistry. A typed block registers its on-disk type
// name; Create() returns a fresh instance or nullptr when the type has no typed
// layout yet (the read loop then falls back to UnknownBlock). Registration is
// EXPLICIT (not static-init), because a static library drops any TU whose only
// effect is a static registrar. Each blocks/<Area>.cpp exposes a
// Register<Area>(NifRegistry&) that the central EnsureBlocksRegistered() calls
// once; Load() invokes EnsureBlocksRegistered() first.

#include <niffer/Niffer.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace niffer {

class NifRegistry {
public:
    using Factory = std::function<std::unique_ptr<NiObject>()>;

    static NifRegistry& Instance();

    void Register(std::string_view typeName, Factory f);
    // nullptr if no typed layout is registered for this name.
    std::unique_ptr<NiObject> Create(std::string_view typeName) const;
    bool Has(std::string_view typeName) const;

private:
    std::unordered_map<std::string, Factory> m_factories;
};

// Convenience: register typed block T under its wire name.
template <class T>
inline void RegisterBlock(NifRegistry& reg, std::string_view name) {
    reg.Register(name, [] { return std::unique_ptr<NiObject>(new T()); });
}

// Register a name-carrying structural type T (T must have a `std::string
// wireName` member and return it from TypeName()). One C++ type can thus back
// many wire names while keeping each block's real type identity in the graph.
template <class T>
inline void RegisterNamed(NifRegistry& reg, std::string_view name) {
    std::string n(name);
    reg.Register(name, [n] {
        auto* p = new T();
        p->wireName = n;
        return std::unique_ptr<NiObject>(p);
    });
}

// Per-area registrars (defined in cpp/blocks/<Area>.cpp).
void RegisterExtra(NifRegistry& reg);
void RegisterNodes(NifRegistry& reg);
void RegisterGeometry(NifRegistry& reg);
void RegisterShaders(NifRegistry& reg);
void RegisterSkin(NifRegistry& reg);
void RegisterAnim(NifRegistry& reg);
void RegisterCollision(NifRegistry& reg);
void RegisterParticles(NifRegistry& reg);

// Registers every typed block exactly once. Called by NifFile::Load.
void EnsureBlocksRegistered();

}  // namespace niffer
