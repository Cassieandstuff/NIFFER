#pragma once
// ── Niffer — the public API ───────────────────────────────────────────────────
// First-party, clean-room NIF library for Skyrim Special Edition. Scope is SSE
// parity ONLY: NIF version 20.2.0.7 (0x14020007), user version 12, BS stream
// version 100. No Fallout 4, Legendary Edition, or other-game paths.
//
// SINGLE PUBLIC HEADER (CommonLib `Skyrim.h` model): consumers write
// `#include <niffer/Niffer.h>` and nothing else. The only other files under
// include/niffer/ sit behind this umbrella: Schema.h (the schema engine) and
// generated/ (its generated handles and views). Internal machinery (Stream,
// framing, engine tables) lives in the private hpp/ tree and is forward-declared
// here at most.
//
// Block layouts are a schema (schema/, compiled into the tables one engine
// interprets); NifFile, Block and the generated views are in Schema.h, included
// at the end of this file.
//
// Round-trip contract: NifFile::Load followed by NifFile::Save reproduces the
// input bytes exactly. A block with no descriptor, or one whose decode doesn't
// consume exactly its declared size, stays raw (Block::Modeled() is false;
// Block::Refusal() says why) and is re-emitted verbatim. Stored header facts are
// re-emitted verbatim, never recomputed.
//
// Clean-room provenance: layouts derive from published format documentation
// corroborated against shipped game bytes — never from nifly source or the
// GPL nif.xml schema. See README.md "Provenance".

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace niffer {

class Stream;  // internal serialization channel (hpp/Stream.h); the header's
               // one Sync() drives both read and write through it.

// ── Format constants ──────────────────────────────────────────────────────────
inline constexpr uint32_t kNifVersion    = 0x14020007;  // 20.2.0.7
inline constexpr uint32_t kUserVersion   = 12;
inline constexpr uint32_t kStreamVersion = 100;         // SSE (BS stream version)

const char* LibraryVersion();

// ── References ────────────────────────────────────────────────────────────────
// Block refs are FILE-NATIVE indices (never reindexed); -1 = null.
struct NiRefBase {
    int32_t index = -1;
    bool IsNull() const { return index < 0; }
};
template <class T>
struct NiRef : NiRefBase {};

// Index into the header string table; 0xFFFFFFFF = empty.
struct NiStringRef {
    uint32_t index = 0xFFFFFFFFu;
    bool IsEmpty() const { return index == 0xFFFFFFFFu; }
};

// ── Errors ────────────────────────────────────────────────────────────────────
enum class NifErrorKind : uint8_t {
    Malformed,           // structural damage / truncation / bad counts
    UnsupportedVersion,  // not SSE 20.2.0.7 / user 12 / bs 100 (e.g. LE files)
    Io,                  // file could not be read
};
struct NifError {
    NifErrorKind kind = NifErrorKind::Malformed;
    size_t       offset = 0;       // byte offset of first failure
    int32_t      blockIndex = -1;  // block being parsed, -1 = header/footer
    std::string  message;
};

// ── Header ────────────────────────────────────────────────────────────────────
// All fields stored verbatim as read and re-emitted verbatim on save
// ("explicit-for-round-trip") — counts/maxStringLength are never recomputed
// except where Save() must reflect the live block list.
struct Header {
    std::string versionLine;            // without the trailing '\n'
    uint32_t version     = kNifVersion;
    uint8_t  endian      = 1;
    uint32_t userVersion = kUserVersion;
    uint32_t numBlocks   = 0;
    uint32_t bsVersion   = kStreamVersion;
    std::string author, processScript, exportScript;  // export ShortStrings
    std::vector<std::string> blockTypes;   // block-type string table
    std::vector<uint16_t> blockTypeIndex;  // per block (raw u16, kept verbatim)
    std::vector<uint32_t> blockSizes;      // per block
    uint32_t maxStringLength = 0;          // stored, not recomputed
    std::vector<std::string> strings;      // global string table
    std::vector<uint32_t> groups;

    void Sync(Stream& s);  // both directions; SSE whitelist enforced on read
    std::string_view TypeNameOf(uint32_t blockIndex) const;  // "" if invalid
};

// ── Decode helpers (Phase 4 access layer) ─────────────────────────────────────
// Niffer stores block payloads at disk precision; these turn the raw vertex /
// skin / material / morph bytes into consumer-ready values. Dependency-free
// (flat float/int arrays, no glm) so the leaf library stays clean.

// Decoded BSVertexDesc: attribute presence + byte offsets within one vertex.
// vertexSize = (desc & 0xF) * 4; attribute offsets are nibbles * 4; the flags
// byte is at bit 44. Position precision is inferred from the first attribute
// offset (16 => full float3, 8 => half3), falling back to the FULLPREC flag.
struct VertexLayout {
    uint32_t vertexSize = 0;
    bool hasVertex = false, hasUV = false, hasNormal = false, hasTangent = false;
    bool hasColor = false, hasSkin = false, hasEye = false, fullPrecision = true;
    uint32_t uvOffset = 0, normalOffset = 0, tangentOffset = 0;
    uint32_t colorOffset = 0, skinOffset = 0, eyeOffset = 0;
};
VertexLayout DecodeVertexDesc(uint64_t desc);

// Decoded geometry from a BSTriShape / BSDynamicTriShape (or, for a skinned
// shape whose own dataSize is 0, from its NiSkinPartition). Flat arrays: 3 floats
// per position/normal, 4 per tangent, 2 per uv, 4 bytes per color, 4 bone
// weights + 4 bone indices per vertex, 3 indices per triangle.
struct DecodedMesh {
    uint32_t numVertices = 0;
    std::vector<float>    positions;   // 3 * numVertices
    std::vector<float>    normals;     // 3 * numVertices (empty if absent)
    std::vector<float>    tangents;    // 4 * numVertices
    std::vector<float>    uvs;         // 2 * numVertices
    std::vector<uint8_t>  colors;      // 4 * numVertices (RGBA bytes)
    std::vector<float>    boneWeights; // 4 * numVertices
    std::vector<uint16_t> boneIndices; // 4 * numVertices
    std::vector<uint16_t> triangles;   // 3 * numTriangles
};
// DecodeMesh / DecodeShapeMesh, which fill one from a shape, are declared at the
// end of Schema.h.

// Blinn-Phong specular exponent (glossiness) -> GGX roughness, for consumers
// that want a PBR roughness from a legacy shader.
float GlossinessToRoughness(float glossiness);

// ── Havok collision extraction ────────────────────────────────────────────────
// World-collision geometry for a NIF, in the NIF's own ROOT space and already
// scaled from Havok units to Skyrim/NIF units (kHavokToSkyrim). Concave triangle
// geometry goes in triVerts/triIndices; convex leaves go in `hulls` (one point
// set each), so a physics backend can choose a triangle-mesh or convex-hull
// shape. Empty when the NIF has no collision. Shapes that can't be decoded with
// confidence are skipped and counted in shapesSkipped (garbage collision is
// worse than none).
//
// Read through the schema (schema/blocks/collision): every bhkRigidBody /
// bhkRigidBodyT reached from a bhkCollisionObject, placed at the collision
// object's target node. Phantom bodies (trigger volumes) are not collision and
// are ignored. Decoded: compressed meshes (triangles), convex vertex sets,
// boxes, spheres and capsules (hulls; convex shapes grown by their radius).
// Skipped: cylinders, planes and NiTriStrips-backed shapes. The conventions and
// their corpus evidence: docs/collision-provenance.md.
struct CollisionGeometry {
    std::vector<float>              triVerts;     // 3 * N, root space (Skyrim units)
    std::vector<uint32_t>           triIndices;   // 3 per triangle
    std::vector<std::vector<float>> hulls;        // each: 3*M convex points (root space)
    int shapesDecoded = 0;
    int shapesSkipped = 0;                        // out-of-bounds / unsupported
};
inline constexpr float kHavokToSkyrim = 69.99124908447265625f;  // bhk unit -> Skyrim unit
// ExtractCollision(NifFile&) is declared at the end of Schema.h.

// FaceGen TRI (FRTRI003) — facial morph / performance-capture shape keys ────
// A second first-party format under Niffer (not a NIF). Byte-exact round-trip:
// reserved header bytes and geometry are preserved verbatim, and morph deltas
// are kept as raw int16 (the lossy int16*baseDiff->float scaling that the old
// sct-assets TriDocument did is a decode helper here, never on the round-trip
// path). Header layout confirmed against the FRTRI003 spec + vanilla corpus.
struct TriMorph {
    std::string          name;     // len-prefixed raw bytes (includes trailing null)
    float                baseDiff = 0.f;   // int16 delta -> float scale
    std::vector<uint8_t> deltas;   // raw int16[3] per vertex (vertexNum*6 bytes)
};

struct TriFile {
    int32_t vertexNum = 0, faceNum = 0, uvVertexNum = 0;
    int32_t morphNum = 0, addMorphNum = 0, addVertexNum = 0;
    std::vector<uint8_t> reservedA;   // header bytes 0x10..0x1B (12), verbatim
    std::vector<uint8_t> reservedB;   // header bytes 0x2C..0x3F (20), verbatim
    std::vector<uint8_t> baseVertices;    // vertexNum * 12 (raw float3)
    std::vector<uint8_t> faceIndices;     // faceNum * 12 (raw uint3)
    std::vector<uint8_t> uvCoords;        // uvVertexNum * 8 (raw float2)
    std::vector<uint8_t> uvFaceIndices;   // faceNum * 12 (raw uint3)
    std::vector<TriMorph> diffMorphs;     // expression / phoneme morphs (typed)
    // Modifier-morph section (addMorphNum entries) + any trailing bytes, kept
    // raw: its per-morph layout differs from the diff morphs and is not yet
    // decoded. Byte-exact; a decode helper can split it later.
    std::vector<uint8_t>  modMorphData;

    static std::expected<TriFile, NifError> Load(const uint8_t* data, size_t len);
    static std::expected<TriFile, NifError> Load(const std::vector<uint8_t>& bytes);
    static std::expected<TriFile, NifError> LoadFile(const std::string& path);
    std::vector<uint8_t> Save() const;
};

// Decode a diff morph's raw int16[3] deltas into float displacements
// (delta * baseDiff), 3 floats per vertex. This is the scaling the old lossy
// TriDocument did inline — here it is an explicit, opt-in decode step.
std::vector<float> DecodeMorphDeltas(const TriMorph& morph);

// Decode the base geometry (raw float3) into a flat float buffer, 3 per vertex
// (size == vertexNum*3). The neutral shape the diff morphs displace from.
std::vector<float> DecodeBaseVertices(const TriFile& tri);

// Blend-shape evaluation: base + Σ (diff-morph delta * weight), matched to each
// morph BY NAME against `weights`. Returns a flat float buffer (3 per vertex),
// always a full copy of `base`. `base` is caller-supplied (3 per vertex) so a
// consumer can blend onto the mesh's own base positions — the FaceGeom dynamic
// vertices correspond 1:1 with the TRI base — rather than the TRI's neutral
// geometry; pass DecodeBaseVertices(tri) to blend from the TRI's own base. A
// weight of 0 (or an unmatched name) contributes nothing. Only diffMorphs are
// searched; the modifier-morph section (modMorphData) is not yet decoded.
// Shared here (not in an app) so any consumer can drive TRI morphs.
std::vector<float> ApplyMorphs(const std::vector<float>& base, const TriFile& tri,
                               const std::map<std::string, float>& weights);

}  // namespace niffer

// The schema engine (NifFile, Block, views) — behind this umbrella.
#include <niffer/Schema.h>
