#pragma once
// ── Collision extraction internals ───────────────────────────────────────────
// Private: ExtractCollision (Niffer.h) is the public entry point. This header
// exposes the schema-path implementation, its conventions and its diagnostics
// to niffer-tool, whose `collision-validate` command measures them against the
// corpus (docs/collision-provenance.md).

#include <niffer/Niffer.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace niffer::collision {

using V3 = std::array<float, 3>;

struct M3 {
    float m[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
};

// p' = R * (s * p) + t
struct Xform {
    M3    R;
    V3    t{ 0, 0, 0 };
    float s = 1.f;
};

V3    Apply(const Xform& x, const V3& p);
Xform Compose(const Xform& parent, const Xform& child);   // parent ∘ child

// The conventions the extractor applies. The defaults are the ones the corpus
// measurement selected; niffer-tool's `collision-validate --sweep` re-measures
// the alternatives.
struct Options {
    bool niRowMajor     = true;   // NiAVObject Matrix33: R[r][c] = m[3r + c]
    bool havokColumns   = true;   // HavokTransform basis vectors are R's columns (else its rows)
    bool quatConjugate  = false;  // bhkRigidBody rotation applied as q (false) or q* (true)
    int  bodyTransform  = 1;      // 0: never, 1: bhkRigidBodyT only, 2: every bhkRigidBody
    bool nodeTransform  = true;   // place the body at its collision object's node (else root)
    bool expandByRadius = true;   // grow box/convex hull points outward by the shape radius
    int  cylinderRadius = 0;      // bhkCylinderShape: 0 skip; 28 or 48: the word used as its radius
};

struct Diagnostics {
    std::map<std::string, int> decoded;            // leaf shape class -> count
    std::map<std::string, int> skipped;            // "class: reason" -> count
    int  phantomsIgnored    = 0;
    int  cmsVerticesChecked = 0;                   // chunk + big vertices tested against the data bounds
    int  cmsVerticesOutside = 0;
    int  cmsMaterialsOutOfRange = 0;              // chunk / big-triangle material indices past the table
    int  moppChecked        = 0;                   // MOPP over compressed mesh: data bounds inside the MOPP grid
    int  moppOutside        = 0;
};

// Root-space transform of every NiAVObject reachable from the footer roots
// through NiNode children (index = block index; `valid` false elsewhere).
struct WorldTransforms {
    std::vector<Xform> world;
    std::vector<bool>  valid;
};
WorldTransforms ComputeWorldTransforms(NifFile& nif, const Options& opt);

// Shared by ExtractCollision and ExtractBodies: the conventions above, as
// functions (Options picks the measured alternative).
std::array<float, 4> Read4(const view::Vec4& v);
V3    Read3(const view::Vec3& v);
Xform NodeLocal(const view::NiAVObject& o, const Options& opt);      // NiAVObject transform
Xform HavokLocal(const view::HavokTransform& h, const Options& opt); // bhk(Convex)TransformShape
M3    QuatMatrix(float qx, float qy, float qz, float qw);            // unit quaternion (x, y, z, w)

// A bhkCompressedMeshShape's triangles in its own space (Havok units): chunks
// (strips, then a list) and then the big triangles; one material per triangle
// (the data's material-table entry). Returns nullptr on success, else why the
// shape was refused.
struct DecodedCompressedMesh {
    std::vector<V3>       vertices;
    std::vector<uint32_t> triangles;          // 3 per triangle, into vertices
    std::vector<uint32_t> triangleMaterials;  // one per triangle
    int  verticesChecked = 0;
    int  verticesOutside = 0;
    bool boundsChecked   = false;             // every vertex was decoded and tested
    int  materialsOutOfRange = 0;              // material indices past the table (read as material 0)
};
const char* DecodeCompressedMesh(NifFile& nif, Block& shapeBlock, DecodedCompressedMesh& out);

CollisionGeometry Extract(NifFile& nif, const Options& opt, Diagnostics* diag = nullptr);

// ExtractBodies under the given conventions (cpp/collision/Bodies.cpp).
std::vector<Body> Bodies(NifFile& nif, const Options& opt);

}  // namespace niffer::collision
