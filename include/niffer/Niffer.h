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
// registry, framing, engine tables) lives in the private hpp/ tree and is
// forward-declared here at most.
//
// Round-trip contract: NifFile::Load followed by NifFile::Save reproduces the
// input bytes exactly. Unimplemented block types are carried as UnknownBlock
// raw spans (listed on NifFile::unknownBlocks — consumers report them loudly);
// a typed block whose layout disagrees with the header-declared size is
// demoted to raw and listed on NifFile::layoutMismatches (louder still: it
// means Niffer's layout for that type is wrong). Stored header facts are
// re-emitted verbatim, never recomputed.
//
// Clean-room provenance: layouts derive from published format documentation
// corroborated against shipped game bytes — never from nifly source or the
// GPL nif.xml schema. See README.md "Provenance".

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace niffer {

class Stream;  // internal serialization channel (hpp/Stream.h); one Sync()
               // per block drives BOTH read and write through it.

// ── Format constants ──────────────────────────────────────────────────────────
inline constexpr uint32_t kNifVersion    = 0x14020007;  // 20.2.0.7
inline constexpr uint32_t kUserVersion   = 12;
inline constexpr uint32_t kStreamVersion = 100;         // SSE (BS stream version)

const char* LibraryVersion();

// ── Math PODs (on-disk layout; semantics decoded at the view layer) ──────────
struct Vec2 { float x = 0, y = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };
struct Quat { float w = 1, x = 0, y = 0, z = 0; };  // w-FIRST on disk
struct Mat33 { float m[9] = {1,0,0, 0,1,0, 0,0,1}; };  // 9 floats, disk order
struct NiTransform {
    Mat33 rotation;
    Vec3  translation;
    float scale = 1.f;
};

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

// ── Block base ────────────────────────────────────────────────────────────────
struct NiObject {
    virtual ~NiObject() = default;
    virtual std::string_view TypeName() const = 0;
    virtual void Sync(Stream& s) = 0;  // ONE function, both directions
};

// ── Typed blocks ──────────────────────────────────────────────────────────────
// Consumer-visible block types. Fields mirror on-disk order (explicit-for-
// round-trip); each Sync() (defined in cpp/blocks/*) drives both directions.
// Coverage grows milestone by milestone; until a type is typed it round-trips
// as UnknownBlock.

// NiExtraData — base of the extra-data family. Modern NIF names index the
// header string table (StringRef), not an inline string.
struct NiExtraData : NiObject {
    NiStringRef name;
    std::string_view TypeName() const override { return "NiExtraData"; }
    void Sync(Stream& s) override;
};

// BSXFlags — bit flags on the root node (havok/animation/etc). NiExtraData + u32.
struct BSXFlags final : NiExtraData {
    uint32_t integerData = 0;
    std::string_view TypeName() const override { return "BSXFlags"; }
    void Sync(Stream& s) override;
};

// ── Extra-data family (M5) — all NiExtraData + a small payload ─────────────────
struct NiStringExtraData final : NiExtraData {
    NiStringRef stringData;
    std::string_view TypeName() const override { return "NiStringExtraData"; }
    void Sync(Stream& s) override;
};
struct NiIntegerExtraData final : NiExtraData {
    uint32_t integerData = 0;
    std::string_view TypeName() const override { return "NiIntegerExtraData"; }
    void Sync(Stream& s) override;
};
struct NiFloatExtraData final : NiExtraData {
    float floatData = 0.f;
    std::string_view TypeName() const override { return "NiFloatExtraData"; }
    void Sync(Stream& s) override;
};
struct NiBooleanExtraData final : NiExtraData {
    uint8_t booleanData = 0;
    std::string_view TypeName() const override { return "NiBooleanExtraData"; }
    void Sync(Stream& s) override;
};
struct NiBinaryExtraData final : NiExtraData {
    std::vector<uint8_t> binaryData;   // u32 size + bytes
    std::string_view TypeName() const override { return "NiBinaryExtraData"; }
    void Sync(Stream& s) override;
};
struct NiStringsExtraData final : NiExtraData {
    std::vector<std::string> data;     // u32 count + SizedString[] (inline)
    std::string_view TypeName() const override { return "NiStringsExtraData"; }
    void Sync(Stream& s) override;
};
// Behavior graph path (root-node HKX pointer the editor reads). +StringRef +u8.
struct BSBehaviorGraphExtraData final : NiExtraData {
    NiStringRef behaviorGraphFile;
    uint8_t     controlsBaseSkeleton = 0;
    std::string_view TypeName() const override { return "BSBehaviorGraphExtraData"; }
    void Sync(Stream& s) override;
};
// Inventory display transform: three u16 rotations + zoom.
struct BSInvMarker final : NiExtraData {
    uint16_t rotationX = 0, rotationY = 0, rotationZ = 0;
    float    zoom = 0.f;
    std::string_view TypeName() const override { return "BSInvMarker"; }
    void Sync(Stream& s) override;
};
// Axis-aligned bound: center + dimensions.
struct BSBound final : NiExtraData {
    Vec3 center, dimensions;
    std::string_view TypeName() const override { return "BSBound"; }
    void Sync(Stream& s) override;
};
// Animation text keys: (time, string) pairs.
struct NiTextKeyExtraData final : NiExtraData {
    struct Key { float time = 0.f; NiStringRef value; };
    std::vector<Key> keys;             // u32 count
    std::string_view TypeName() const override { return "NiTextKeyExtraData"; }
    void Sync(Stream& s) override;
};
// Per-bone LOD distances.
struct BSBoneLODExtraData final : NiExtraData {
    struct BoneLOD { uint32_t distance = 0; NiStringRef boneName; };
    std::vector<BoneLOD> boneLODs;     // u32 count
    std::string_view TypeName() const override { return "BSBoneLODExtraData"; }
    void Sync(Stream& s) override;
};

// ── Scene graph (M1) ──────────────────────────────────────────────────────────
// NiObjectNET → NiAVObject → NiNode, layout confirmed from clutter/barrel01.nif
// (BSFadeNode block == 88 bytes). Intermediate types are never registered as
// blocks; only concrete leaf node types are.

// Named, controllable, extra-data-carrying object.
struct NiObjectNET : NiObject {
    NiStringRef                      name;
    std::vector<NiRef<NiExtraData>>  extraData;
    NiRef<NiObject>                  controller;
    std::string_view TypeName() const override { return "NiObjectNET"; }
    void Sync(Stream& s) override;
};

// Transformable scene node: flags, local transform, optional collision object.
struct NiAVObject : NiObjectNET {
    uint32_t        flags = 0;
    NiTransform     transform;
    NiRef<NiObject> collisionObject;  // bhkNiCollisionObject family
    std::string_view TypeName() const override { return "NiAVObject"; }
    void Sync(Stream& s) override;
};

// Grouping node with children and (dynamic-effect) effects.
struct NiNode : NiAVObject {
    std::vector<NiRef<NiAVObject>> children;
    std::vector<NiRef<NiObject>>   effects;  // NiDynamicEffect
    std::string_view TypeName() const override { return "NiNode"; }
    void Sync(Stream& s) override;
};

// Node types whose on-disk layout is identical to NiNode (no added fields);
// they inherit NiNode::Sync and differ only by wire type name / runtime role.
struct BSFadeNode final : NiNode {
    std::string_view TypeName() const override { return "BSFadeNode"; }
};
struct BSLeafAnimNode final : NiNode {
    std::string_view TypeName() const override { return "BSLeafAnimNode"; }
};

// Node variants that add fields after NiNode (each layout gated by the corpus).
struct NiBillboardNode final : NiNode {
    uint16_t billboardMode = 0;
    std::string_view TypeName() const override { return "NiBillboardNode"; }
    void Sync(Stream& s) override;
};
struct BSValueNode final : NiNode {
    uint32_t value = 0;
    uint8_t  valueNodeFlags = 0;
    std::string_view TypeName() const override { return "BSValueNode"; }
    void Sync(Stream& s) override;
};
struct BSOrderedNode final : NiNode {
    Vec4    alphaSortBound;
    uint8_t staticBound = 0;
    std::string_view TypeName() const override { return "BSOrderedNode"; }
    void Sync(Stream& s) override;
};
struct BSMultiBoundNode final : NiNode {
    NiRef<NiObject> multiBound;
    uint32_t        cullingMode = 0;
    std::string_view TypeName() const override { return "BSMultiBoundNode"; }
    void Sync(Stream& s) override;
};
struct NiSwitchNode final : NiNode {
    uint16_t switchFlags = 0;
    uint32_t index = 0;
    std::string_view TypeName() const override { return "NiSwitchNode"; }
    void Sync(Stream& s) override;
};
struct BSTreeNode final : NiNode {
    std::vector<NiRef<NiObject>> bones1;
    std::vector<NiRef<NiObject>> bones2;
    std::string_view TypeName() const override { return "BSTreeNode"; }
    void Sync(Stream& s) override;
};

// ── Geometry (M2) ─────────────────────────────────────────────────────────────
// BSTriShape layout confirmed against clutter/barrel01.nif block 7: NiAVObject +
// bounding sphere + skin/shader/alpha refs + vertexDesc(u64) + numTriangles(u16)
// + numVertices(u16) + dataSize(u32) + [dataSize bytes of interleaved vertex &
// triangle data] + a trailing u32. Vertex/triangle bytes are kept RAW at disk
// precision (byte-exact round-trip); decoding them via the vertexDesc bitfield
// is a separate helper layer — deliberately not guessed on the round-trip path.
struct BSTriShape : NiAVObject {
    Vec3            boundCenter;
    float           boundRadius = 0.f;
    NiRef<NiObject> skin;             // NiSkinInstance / BSSkin::Instance family
    NiRef<NiObject> shaderProperty;   // BSShaderProperty family
    NiRef<NiObject> alphaProperty;    // NiAlphaProperty
    uint64_t        vertexDesc = 0;   // BSVertexDesc bitfield (per-vertex layout)
    uint16_t        numTriangles = 0;
    uint16_t        numVertices = 0;
    uint32_t        dataSize = 0;              // bytes of vertexData + triangles
    std::vector<uint8_t> geometryData;         // raw, length == dataSize
    uint32_t        particleDataSize = 0;      // >0 only for particle/FX shapes
    std::vector<uint8_t> particleData;         // uint16[particleDataSize] when >0
    std::string_view TypeName() const override { return "BSTriShape"; }
    void Sync(Stream& s) override;
};

// BSDynamicTriShape — CPU-writable geometry (compiled NPC facegen heads). Extends
// BSTriShape with a dynamic vertex array (Vector4 per vertex, kept raw). Confirmed
// against actors/character/character assets/childhead.nif block 3 (dataSize=0,
// numVertices=1211, dynamicDataSize=19376=1211*16, data runs to the block end).
struct BSDynamicTriShape final : BSTriShape {
    uint32_t             dynamicDataSize = 0;   // bytes (== numVertices * 16)
    std::vector<uint8_t> dynamicData;           // raw Vector4[numVertices]
    std::string_view TypeName() const override { return "BSDynamicTriShape"; }
    void Sync(Stream& s) override;
};

// ── Shaders / materials (M3) ──────────────────────────────────────────────────
// BSShaderTextureSet — the texture-path list. NiObject + u32 count + SizedString
// per slot (confirmed against barrel01 block 9: 9 slots, 2 named + 7 empty).
struct BSShaderTextureSet final : NiObject {
    std::vector<std::string> textures;
    std::string_view TypeName() const override { return "BSShaderTextureSet"; }
    void Sync(Stream& s) override;
};

// NiAlphaProperty — blend/test state. NiObjectNET + u16 flags + u8 threshold
// (confirmed against akatoshamuletf block 10, size 15). Flag bits decode via
// helpers, kept raw on the round-trip path.
struct NiAlphaProperty final : NiObjectNET {
    uint16_t flags = 0;
    uint8_t  threshold = 0;
    std::string_view TypeName() const override { return "NiAlphaProperty"; }
    void Sync(Stream& s) override;
};

// BSLightingShaderProperty — the main PBR/lighting material block. The 12-byte
// pre-flags header (name + three fixed u32s, observed -1/0/-1 across the corpus —
// a name ref plus controller/extra-data refs that are null in vanilla) is
// followed by a well-understood run: shaderFlags1/2, UV offset+scale, textureSet
// ref, then the shader-type-dependent material scalars (kept raw for now, decoded
// by a later helper). Confirmed against barrel01 block 8 (flags1=0x82400301,
// textureSet -> block 9). The header is modelled as fixed fields (no variable
// extra-data list appears on these blocks); any shader that does carry one
// round-trips safely as a demoted block.
struct BSLightingShaderProperty final : NiObject {
    NiStringRef     name;            // @0
    uint32_t        headerA = 0xFFFFFFFF;   // @4  (ref, null in vanilla)
    uint32_t        headerB = 0;            // @8
    uint32_t        headerC = 0xFFFFFFFF;   // @12 (ref, null in vanilla)
    uint32_t        shaderFlags1 = 0;       // @16 SkyrimShaderPropertyFlags1
    uint32_t        shaderFlags2 = 0;       // @20 SkyrimShaderPropertyFlags2
    Vec2            uvOffset;               // @24
    Vec2            uvScale;                // @32
    NiRef<BSShaderTextureSet> textureSet;   // @40
    std::vector<uint8_t> materialTail;      // type-dependent scalars (raw)

    std::string_view TypeName() const override { return "BSLightingShaderProperty"; }
    void Sync(Stream& s) override;
};

// ── Skinning (M4, legacy path — the only one in the SSE corpus) ───────────────
struct NiSkinData;  // fwd

// NiSkinInstance — binds a shape to a NiSkinData + NiSkinPartition + bone list.
// Confirmed against childhead block 4 (data->5, skinPartition->6, root->0,
// bones[2]).
struct NiSkinInstance : NiObject {
    NiRef<NiSkinData> data;
    NiRef<NiObject>   skinPartition;   // NiSkinPartition
    NiRef<NiObject>   skeletonRoot;    // NiNode (Ptr)
    std::vector<NiRef<NiObject>> bones;  // Ptr array (bone NiNodes)
    std::string_view TypeName() const override { return "NiSkinInstance"; }
    void Sync(Stream& s) override;
};

// BSDismemberSkinInstance — NiSkinInstance + dismemberment body-part partitions
// (each: u16 part flags + u16 body-part id).
struct BSDismemberSkinInstance final : NiSkinInstance {
    struct Partition { uint16_t partFlag = 0; uint16_t bodyPart = 0; };
    std::vector<Partition> partitions;
    std::string_view TypeName() const override { return "BSDismemberSkinInstance"; }
    void Sync(Stream& s) override;
};

// Per-vertex weight within one bone's influence list.
struct BoneVertData { uint16_t index = 0; float weight = 0.f; };

// One bone's skin data: inverse-bind transform, bounding sphere, vertex weights.
struct SkinBoneData {
    NiTransform transform;      // skin-to-bone (inverse bind)
    Vec3        boundingOffset;
    float       boundingRadius = 0.f;
    std::vector<BoneVertData> vertexWeights;  // u16 count
};

// NiSkinData — per-bone inverse-bind transforms + vertex weights. Confirmed
// against childhead block 5 (skin transform 52B, numBones=2, hasVertexWeights u8).
struct NiSkinData final : NiObject {
    NiTransform skinTransform;
    uint8_t     hasVertexWeights = 1;
    std::vector<SkinBoneData> bones;  // u32 count
    std::string_view TypeName() const override { return "NiSkinData"; }
    void Sync(Stream& s) override;
};

// NiSkinPartition (SSE variant) — carries the actual skinned vertex data (this
// is where a skinned shape's vertices live when its BSTriShape.dataSize is 0),
// plus the per-partition strip/bone tables. Header confirmed against
// goldringdiamond block 5 (numPartitions=1, dataSize=6908, vertexSize=44,
// 6908/44 = 157 vertices). The vertex block is exposed; the per-partition tables
// (strips, bone indices, per-partition weights) are kept raw for now — decoded
// by a later helper.
struct NiSkinPartition final : NiObject {
    uint32_t numPartitions = 0;
    uint32_t dataSize = 0;              // bytes of vertexData (== numVertices*vertexSize)
    uint32_t vertexSize = 0;
    uint64_t vertexDesc = 0;            // BSVertexDesc (same layout as BSTriShape)
    std::vector<uint8_t> vertexData;    // raw, length == dataSize
    std::vector<uint8_t> partitionData; // raw per-partition tables (remainder)
    std::string_view TypeName() const override { return "NiSkinPartition"; }
    void Sync(Stream& s) override;
};

// ── Interpolators (M6 leaves) ─────────────────────────────────────────────────
// NiInterpolator-family leaves: a default value (a -FLT_MAX sentinel when driven
// purely by keyed data) plus a ref to the keyed NiXxxData block. Confirmed
// against shipped bytes (glazedcandles / ashpile / fxspiderweb).
struct NiFloatInterpolator final : NiObject {
    float           value = 0.f;
    NiRef<NiObject> data;   // NiFloatData
    std::string_view TypeName() const override { return "NiFloatInterpolator"; }
    void Sync(Stream& s) override;
};
struct NiBoolInterpolator final : NiObject {
    uint8_t         value = 0;
    NiRef<NiObject> data;   // NiBoolData
    std::string_view TypeName() const override { return "NiBoolInterpolator"; }
    void Sync(Stream& s) override;
};
struct NiPoint3Interpolator final : NiObject {
    Vec3            value;
    NiRef<NiObject> data;   // NiPosData
    std::string_view TypeName() const override { return "NiPoint3Interpolator"; }
    void Sync(Stream& s) override;
};
struct NiTransformInterpolator final : NiObject {
    Vec3            translation;
    Quat            rotation;   // w-first on disk
    float           scale = 0.f;
    NiRef<NiObject> data;       // NiTransformData
    std::string_view TypeName() const override { return "NiTransformInterpolator"; }
    void Sync(Stream& s) override;
};

// NiBlendInterpolator base — manager-controlled blend state (flags + arraySize +
// weightThreshold). Confirmed against hotiron / eyeofmagnus (managed form; no
// stored interp-item arrays). A blend interpolator that stores those arrays
// round-trips safely as a demoted block.
struct NiBlendInterpolator : NiObject {
    uint8_t flags = 0;
    uint8_t arraySize = 0;
    float   weightThreshold = 0.f;
    std::string_view TypeName() const override { return "NiBlendInterpolator"; }
    void Sync(Stream& s) override;
};
struct NiBlendFloatInterpolator final : NiBlendInterpolator {
    float value = 0.f;
    std::string_view TypeName() const override { return "NiBlendFloatInterpolator"; }
    void Sync(Stream& s) override;
};
struct NiBlendBoolInterpolator final : NiBlendInterpolator {
    uint8_t value = 0;
    std::string_view TypeName() const override { return "NiBlendBoolInterpolator"; }
    void Sync(Stream& s) override;
};
// Same wire shape as NiBoolInterpolator (u8 value + NiBoolData ref).
struct NiBoolTimelineInterpolator final : NiObject {
    uint8_t         value = 0;
    NiRef<NiObject> data;
    std::string_view TypeName() const override { return "NiBoolTimelineInterpolator"; }
    void Sync(Stream& s) override;
};

// ── Keyed animation data (M6) ─────────────────────────────────────────────────
// NiAnimationKeyGroup blocks: numKeys, then (if numKeys>0) an interpolation key
// type, then the keys. Key size depends on the key type AND the channel
// (float / bool / Vec3 / color), so the keys are kept raw at disk precision and
// decoded by a later helper; numKeys + interpolation are exposed. Confirmed
// against glazedcandles (NiFloatData, QUADRATIC 16B keys), nocturnal (NiBoolData,
// CONST 5B keys), ashpile (NiPosData, QUADRATIC 40B keys).
struct NiFloatData final : NiObject {
    uint32_t numKeys = 0, interpolation = 0;
    std::vector<uint8_t> keyData;
    std::string_view TypeName() const override { return "NiFloatData"; }
    void Sync(Stream& s) override;
};
struct NiBoolData final : NiObject {
    uint32_t numKeys = 0, interpolation = 0;
    std::vector<uint8_t> keyData;
    std::string_view TypeName() const override { return "NiBoolData"; }
    void Sync(Stream& s) override;
};
struct NiPosData final : NiObject {
    uint32_t numKeys = 0, interpolation = 0;
    std::vector<uint8_t> keyData;
    std::string_view TypeName() const override { return "NiPosData"; }
    void Sync(Stream& s) override;
};
struct NiColorData final : NiObject {
    uint32_t numKeys = 0, interpolation = 0;
    std::vector<uint8_t> keyData;
    std::string_view TypeName() const override { return "NiColorData"; }
    void Sync(Stream& s) override;
};

// ── Controllers (M6) ──────────────────────────────────────────────────────────
// NiTimeController base, confirmed against fxspiderweb block 29 (NiTransform
// controller, 30 bytes): nextController + flags(u16) + frequency + phase +
// startTime + stopTime + target.
struct NiTimeController : NiObject {
    NiRef<NiObject> nextController;
    uint16_t flags = 0;
    float    frequency = 1.f, phase = 0.f, startTime = 0.f, stopTime = 0.f;
    NiRef<NiObject> target;   // Ptr
    std::string_view TypeName() const override { return "NiTimeController"; }
    void Sync(Stream& s) override;
};
// NiSingleInterpController adds one interpolator ref.
struct NiSingleInterpController : NiTimeController {
    NiRef<NiObject> interpolator;
    std::string_view TypeName() const override { return "NiSingleInterpController"; }
    void Sync(Stream& s) override;
};
struct NiTransformController final : NiSingleInterpController {
    std::string_view TypeName() const override { return "NiTransformController"; }
};
// Plain NiSingleInterpController leaves (no added fields; size 30 in vanilla).
struct NiVisController final : NiSingleInterpController {
    std::string_view TypeName() const override { return "NiVisController"; }
};
struct BSNiAlphaPropertyTestRefController final : NiSingleInterpController {
    std::string_view TypeName() const override { return "BSNiAlphaPropertyTestRefController"; }
};
// Shader-property controllers: NiSingleInterpController + a target-variable id.
struct BSLightingShaderPropertyFloatController final : NiSingleInterpController {
    uint32_t targetVariable = 0;
    std::string_view TypeName() const override { return "BSLightingShaderPropertyFloatController"; }
    void Sync(Stream& s) override;
};
struct BSLightingShaderPropertyColorController final : NiSingleInterpController {
    uint32_t targetVariable = 0;
    std::string_view TypeName() const override { return "BSLightingShaderPropertyColorController"; }
    void Sync(Stream& s) override;
};
struct BSEffectShaderPropertyFloatController final : NiSingleInterpController {
    uint32_t targetVariable = 0;
    std::string_view TypeName() const override { return "BSEffectShaderPropertyFloatController"; }
    void Sync(Stream& s) override;
};
struct BSEffectShaderPropertyColorController final : NiSingleInterpController {
    uint32_t targetVariable = 0;
    std::string_view TypeName() const override { return "BSEffectShaderPropertyColorController"; }
    void Sync(Stream& s) override;
};
// NiControllerManager: NiTimeController + cumulative(u8) + sequence refs +
// objectPalette ref (shacklewall block 4, 47 bytes).
struct NiControllerManager final : NiTimeController {
    uint8_t cumulative = 0;
    std::vector<NiRef<NiObject>> controllerSequences;
    NiRef<NiObject> objectPalette;
    std::string_view TypeName() const override { return "NiControllerManager"; }
    void Sync(Stream& s) override;
};
// NiMultiTargetTransformController: NiTimeController + u16 count + extra targets.
struct NiMultiTargetTransformController final : NiTimeController {
    std::vector<NiRef<NiObject>> extraTargets;   // u16 count
    std::string_view TypeName() const override { return "NiMultiTargetTransformController"; }
    void Sync(Stream& s) override;
};

// NiControllerSequence and NiTransformData carry variable-length controlled-block
// / keyframe structures whose full decode is deferred; their headers are exposed
// and the remainder kept raw (byte-exact).
struct NiControllerSequence final : NiObject {
    NiStringRef name;
    uint32_t    numControlledBlocks = 0;
    std::vector<uint8_t> tail;   // controlled blocks + sequence params (raw)
    std::string_view TypeName() const override { return "NiControllerSequence"; }
    void Sync(Stream& s) override;
};
struct NiTransformData final : NiObject {
    uint32_t numRotationKeys = 0;
    std::vector<uint8_t> tail;   // rotation/translation/scale key groups (raw)
    std::string_view TypeName() const override { return "NiTransformData"; }
    void Sync(Stream& s) override;
};

// ── Havok collision (M7) ──────────────────────────────────────────────────────
// The SCT consumer does not read collision, so these are typed at the STRUCTURAL
// level: the linking refs that keep the block graph walkable are decoded, and
// the Havok physics / geometry payloads are kept raw (byte-exact, no mislabel
// risk). Layouts confirmed against barrel01, cookingspit, basket, eggs blocks.

// Collision object: target + flags + body ref (barrel01 block 6, 10B).
struct bhkCollisionObject : NiObject {
    NiRef<NiObject> target;    // NiAVObject (Ptr)
    uint16_t        flags = 0;
    NiRef<NiObject> body;      // bhkWorldObject
    std::string_view TypeName() const override { return "bhkCollisionObject"; }
    void Sync(Stream& s) override;
};
struct bhkSPCollisionObject final : bhkCollisionObject {
    std::string_view TypeName() const override { return "bhkSPCollisionObject"; }
};

// A shape/body that wraps a child shape ref, then a raw Havok payload.
struct bhkShapeWrapper : NiObject {
    NiRef<NiObject>      shape;   // wrapped shape / bhkWorldObject shape
    std::vector<uint8_t> data;    // remaining Havok fields (raw)
    void Sync(Stream& s) override;
    std::string_view TypeName() const override { return "bhkShapeWrapper"; }
};
struct bhkRigidBody final : bhkShapeWrapper { std::string_view TypeName() const override { return "bhkRigidBody"; } };
struct bhkRigidBodyT final : bhkShapeWrapper { std::string_view TypeName() const override { return "bhkRigidBodyT"; } };
struct bhkMoppBvTreeShape final : bhkShapeWrapper { std::string_view TypeName() const override { return "bhkMoppBvTreeShape"; } };
struct bhkConvexTransformShape final : bhkShapeWrapper { std::string_view TypeName() const override { return "bhkConvexTransformShape"; } };
struct bhkTransformShape final : bhkShapeWrapper { std::string_view TypeName() const override { return "bhkTransformShape"; } };
struct bhkCompressedMeshShape final : bhkShapeWrapper { std::string_view TypeName() const override { return "bhkCompressedMeshShape"; } };
struct bhkNiTriStripsShape final : bhkShapeWrapper { std::string_view TypeName() const override { return "bhkNiTriStripsShape"; } };

// A convex leaf shape: Havok material + radius, then raw geometry/params.
struct bhkPrimitiveShape : NiObject {
    uint32_t             material = 0;
    float                radius = 0.f;
    std::vector<uint8_t> data;    // dimensions / vertices / params (raw)
    void Sync(Stream& s) override;
    std::string_view TypeName() const override { return "bhkPrimitiveShape"; }
};
struct bhkSphereShape final : bhkPrimitiveShape { std::string_view TypeName() const override { return "bhkSphereShape"; } };
struct bhkBoxShape final : bhkPrimitiveShape { std::string_view TypeName() const override { return "bhkBoxShape"; } };
struct bhkCapsuleShape final : bhkPrimitiveShape { std::string_view TypeName() const override { return "bhkCapsuleShape"; } };
struct bhkCylinderShape final : bhkPrimitiveShape { std::string_view TypeName() const override { return "bhkCylinderShape"; } };
struct bhkConvexVerticesShape final : bhkPrimitiveShape { std::string_view TypeName() const override { return "bhkConvexVerticesShape"; } };

// List shape: sub-shape refs, then material/filter tail (raw).
struct bhkListShape final : NiObject {
    std::vector<NiRef<NiObject>> subShapes;
    std::vector<uint8_t> data;
    std::string_view TypeName() const override { return "bhkListShape"; }
    void Sync(Stream& s) override;
};

// Opaque Havok blobs (geometry data, phantoms, constraints): byte-exact raw.
struct bhkRawBlock : NiObject {
    std::vector<uint8_t> data;
    void Sync(Stream& s) override;
    std::string_view TypeName() const override { return "bhkRawBlock"; }
};
struct bhkCompressedMeshShapeData final : bhkRawBlock { std::string_view TypeName() const override { return "bhkCompressedMeshShapeData"; } };
struct bhkSimpleShapePhantom final : bhkRawBlock { std::string_view TypeName() const override { return "bhkSimpleShapePhantom"; } };
struct bhkPlaneShape final : bhkRawBlock { std::string_view TypeName() const override { return "bhkPlaneShape"; } };
struct bhkBlendCollisionObject final : bhkRawBlock { std::string_view TypeName() const override { return "bhkBlendCollisionObject"; } };
struct bhkLimitedHingeConstraint final : bhkRawBlock { std::string_view TypeName() const override { return "bhkLimitedHingeConstraint"; } };
struct bhkRagdollConstraint final : bhkRawBlock { std::string_view TypeName() const override { return "bhkRagdollConstraint"; } };
struct bhkHingeConstraint final : bhkRawBlock { std::string_view TypeName() const override { return "bhkHingeConstraint"; } };
struct bhkBallAndSocketConstraint final : bhkRawBlock { std::string_view TypeName() const override { return "bhkBallAndSocketConstraint"; } };
struct bhkBallSocketConstraintChain final : bhkRawBlock { std::string_view TypeName() const override { return "bhkBallSocketConstraintChain"; } };
struct bhkStiffSpringConstraint final : bhkRawBlock { std::string_view TypeName() const override { return "bhkStiffSpringConstraint"; } };
struct bhkBreakableConstraint final : bhkRawBlock { std::string_view TypeName() const override { return "bhkBreakableConstraint"; } };

// ── Particles + long tail (M8) ────────────────────────────────────────────────
// Name-carrying structural bases: one C++ type backs many wire names, decoding
// the shared leading fields and keeping the payload raw (byte-exact). SCT does
// not render particles, so full particle decode is out of scope. `wireName` is
// stamped at registration and returned from TypeName().

// Pure raw carrier (data-only blocks: NiPSysData, colliders, multibounds, …).
struct NifNamedRaw : NiObject {
    std::string          wireName;
    std::vector<uint8_t> data;
    std::string_view TypeName() const override { return wireName; }
    void Sync(Stream& s) override;
};
// NiObjectNET + raw (effect/sky/water shader properties).
struct NifNamedObjectNET : NiObjectNET {
    std::string          wireName;
    std::vector<uint8_t> tail;
    std::string_view TypeName() const override { return wireName; }
    void Sync(Stream& s) override;
};
// NiAVObject + raw (NiParticleSystem, NiCamera, legacy NiTriShape, …).
struct NifNamedAVObject : NiAVObject {
    std::string          wireName;
    std::vector<uint8_t> tail;
    std::string_view TypeName() const override { return wireName; }
    void Sync(Stream& s) override;
};
// NiTimeController + raw (the NiPSys*Ctlr controllers + misc controllers).
struct NifNamedTimeController : NiTimeController {
    std::string          wireName;
    std::vector<uint8_t> tail;
    std::string_view TypeName() const override { return wireName; }
    void Sync(Stream& s) override;
};
// NiPSysModifier base (name + order + target + active) + raw. Backs every
// particle modifier and emitter. Confirmed against ashpileghost block 153.
struct NifNamedPSysModifier : NiObject {
    std::string          wireName;
    NiStringRef          name;
    uint32_t             order = 0;
    NiRef<NiObject>      target;   // owning NiParticleSystem
    uint8_t              active = 0;
    std::vector<uint8_t> tail;
    std::string_view TypeName() const override { return wireName; }
    void Sync(Stream& s) override;
};
// NiBlendPoint3Interpolator — NiBlendInterpolator + Vec3 value.
struct NiBlendPoint3Interpolator final : NiBlendInterpolator {
    Vec3 value;
    std::string_view TypeName() const override { return "NiBlendPoint3Interpolator"; }
    void Sync(Stream& s) override;
};

// Raw passthrough for block types Niffer has no typed layout for (and demotion
// target for typed layout mismatches). Byte-exact on round-trip: block order
// and indices are preserved verbatim, so refs inside `raw` stay valid.
struct UnknownBlock final : NiObject {
    std::string          typeName;
    std::vector<uint8_t> raw;
    std::string_view TypeName() const override { return typeName; }
    void Sync(Stream& s) override;
};

// ── File ──────────────────────────────────────────────────────────────────────
struct UnknownBlockInfo {
    uint32_t    blockIndex = 0;
    std::string typeName;
};
struct LayoutMismatch {   // OUR typed layout disagreed with the file — a bug
    uint32_t    blockIndex = 0;
    std::string typeName;
    uint32_t    declaredSize = 0;
    uint32_t    consumedSize = 0;
};

class NifFile {
public:
    Header header;
    std::vector<std::unique_ptr<NiObject>> blocks;  // index == file block index
    std::vector<int32_t> roots;                     // footer root refs
    std::vector<uint8_t> trailing;                  // any bytes past the footer,
                                                    // preserved verbatim (normally empty)

    // Loud-failure surface — consumers must report these (aggregated per file).
    std::vector<UnknownBlockInfo> unknownBlocks;
    std::vector<LayoutMismatch>   layoutMismatches;

    static std::expected<NifFile, NifError> Load(const uint8_t* data, size_t len);
    static std::expected<NifFile, NifError> Load(const std::vector<uint8_t>& bytes);
    static std::expected<NifFile, NifError> LoadFile(const std::string& path);

    // Byte-faithful re-write. Block sizes are re-measured; every other header
    // fact is emitted verbatim.
    std::vector<uint8_t> Save() const;

    NiObject* GetBlock(int32_t index) const;
    template <class T>
    T* Get(NiRef<T> ref) const { return dynamic_cast<T*>(GetBlock(ref.index)); }

    std::string_view String(NiStringRef ref) const;  // "" if empty/invalid
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
// Decode from a shape's own geometry block (BSTriShape family).
DecodedMesh DecodeMesh(const BSTriShape& shape);
// Decode the vertex block of a NiSkinPartition (for skinned shapes with no own
// geometry); triangles come from the shape's partition tables (not yet decoded).
DecodedMesh DecodeMesh(const NiSkinPartition& part);

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
struct CollisionGeometry {
    std::vector<float>              triVerts;     // 3 * N, root space (Skyrim units)
    std::vector<uint32_t>           triIndices;   // 3 per triangle
    std::vector<std::vector<float>> hulls;        // each: 3*M convex points (root space)
    int shapesDecoded = 0;
    int shapesSkipped = 0;                        // out-of-bounds / unsupported
};
inline constexpr float kHavokToSkyrim = 69.99124908447265625f;  // bhk unit -> Skyrim unit
CollisionGeometry ExtractCollision(const NifFile& nif);

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

// The schema engine (Nif, Block, views) — behind this umbrella.
#include <niffer/Schema.h>
