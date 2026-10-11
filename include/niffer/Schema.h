#pragma once
// ── Niffer schema engine — the schema-driven block model ─────────────────────
// Behind the umbrella: consumers include <niffer/Niffer.h>, which pulls this in.
// One generic engine interprets a SchemaSet (generated from schema/ by
// tools/schemagen) for every block class; nothing here is per-block code. The
// semantics are specified in docs/schema-language.md.
//
//   auto nif = niffer::NifFile::LoadFile(path);                 // std::expected
//   for (niffer::Block& b : nif->blocks)
//       if (auto node = niffer::view::NiNode::From(b))
//           for (auto child : node->Children()) use(nif->Get(child));
//   nif->Save();                                                // verbatim unless modified
//
// Fidelity: every block keeps its original bytes. Encode emits them verbatim
// unless the block was modified (or `force`, which the real-data gate uses to
// prove every layout re-encodes byte-exactly). A block the schema can't
// reproduce exactly — no descriptor, or decoding didn't consume exactly its
// declared size — stays raw.

#include <niffer/Niffer.h>

#include <array>
#include <bit>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace niffer {

// ── Compiled schema tables ────────────────────────────────────────────────────
// Flat constexpr tables, linked by index, emitted by tools/schemagen:
//   TypeDef   a value layout: scalar, ref, string, bytes, struct (→ FieldDef
//             range), array (→ element TypeDef) or union (→ CaseDef range).
//             Ref/Ptr types carry their allowed targets (→ links range of
//             BlockDef indices).
//   FieldDef  one struct field, with its length source, condition and union
//             selector, each as (structs up, field index) from the field's own
//             struct.
//   CaseDef   one union case: the selector values that pick it (→ whens
//             range) and its TypeDef (none for an empty case).
//   BlockDef  a block class: its parent and its flattened layout (a struct
//             TypeDef: inherited fields first). Sorted by name.
namespace schema {

enum class Kind : uint8_t {
    U8, U16, U32, U64, I8, I16, I32, I64, F32,
    Half,         // IEEE-754 half, carried as raw bits
    Ref,          // i32 block index, -1 = null; owning link
    Ptr,          // i32 block index, -1 = null; non-owning link
    StringRef,    // u32 header string-table index, 0xFFFFFFFF = empty
    SizedString,  // u32 length + raw bytes
    Bytes,        // opaque
    Struct,
    Array,
    Union,        // one of a closed set of layouts, chosen by an earlier field
};

enum class LenKind : uint8_t {
    None,
    Fixed,      // Bytes: `count` bytes; Array: `count` elements
    Rest,       // to the end of the block
    Prefix8,    // Array: element count read from a leading u8 / u16 / u32
    Prefix16,
    Prefix32,
    Field,      // the value of an earlier field (FieldDef::lenUp / lenField)
    Window,     // Array: elements until exactly N bytes are consumed (`count`,
                // or the value of an earlier field)
};

enum class CondOp : uint8_t { None, Eq, Ne, Min, Max, Mask };

inline constexpr uint16_t kNoField = 0xFFFF;

struct TypeDef {
    Kind        kind      = Kind::U8;
    LenKind     len       = LenKind::None;
    uint32_t    count     = 0;   // Fixed Bytes: byte count; Fixed Array: element count
    uint16_t    ref       = 0;   // Array: element TypeDef; Struct: first FieldDef
    uint16_t    refCount  = 0;   // Struct: FieldDef count
    uint16_t    linkFirst = 0;   // Ref/Ptr: allowed target BlockDefs (empty = any)
    uint16_t    linkCount = 0;
    const char* name      = nullptr;  // named struct / block layout, for diagnostics
};

struct FieldDef {
    const char* name      = nullptr;
    uint16_t    type      = 0;
    uint8_t     lenUp     = 0;         // LenKind::Field: structs outward from this field's struct
    uint16_t    lenField  = kNoField;  //   …and the field index there
    CondOp      cond      = CondOp::None;
    uint8_t     condUp    = 0;
    uint16_t    condField = kNoField;
    int64_t     condValue = 0;
    uint8_t     selUp     = 0;         // Union: the selector field…
    uint16_t    selField  = kNoField;
    uint64_t    selMask   = 0;         //   …ANDed with this mask when non-zero
    bool        packed    = false;     // Array: may be stored packed (spec §4.4)
};

struct CaseDef {
    const char* name      = nullptr;
    uint16_t    type      = kNoField;  // kNoField: an empty case (zero bytes)
    uint16_t    whenFirst = 0;         // selector values (whens range)
    uint16_t    whenCount = 0;
    bool        isDefault = false;     // taken when no other case matches
};

struct BlockDef {
    const char* name     = nullptr;
    int16_t     parent   = -1;       // BlockDef index; -1 for the root (NiObject)
    uint16_t    type     = 0;        // the flattened layout (a Struct TypeDef)
    uint16_t    ownFirst = 0;        // index of this class's first own field in the layout
    bool        abstract = false;
};

struct SchemaSet {
    std::span<const TypeDef>  types;
    std::span<const FieldDef> fields;
    std::span<const CaseDef>  cases;
    std::span<const int64_t>  whens;
    std::span<const uint16_t> links;
    std::span<const BlockDef> blocks;    // sorted by name
    const char*               schemaVersion   = nullptr;
    const char*               languageVersion = nullptr;

    // nullptr if no block of that name.
    const BlockDef* Find(std::string_view name) const {
        size_t lo = 0, hi = blocks.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (std::string_view(blocks[mid].name) < name) lo = mid + 1;
            else hi = mid;
        }
        return (lo < blocks.size() && name == blocks[lo].name) ? &blocks[lo] : nullptr;
    }
    uint16_t IndexOf(const BlockDef* b) const { return static_cast<uint16_t>(b - blocks.data()); }
    // True when block `derived` is `base` or inherits from it.
    bool IsA(uint16_t derived, uint16_t base) const {
        for (int32_t b = derived; b >= 0; b = blocks[static_cast<size_t>(b)].parent)
            if (static_cast<uint16_t>(b) == base) return true;
        return false;
    }
};

// The schema generated from schema/ (hpp/generated/Tables.h).
const SchemaSet& Main();

}  // namespace schema

// ── Values ────────────────────────────────────────────────────────────────────
// An IEEE-754 half (`half` in the schema), kept as its raw bits so every pattern
// round-trips; convert at the edge.
struct Half {
    uint16_t bits = 0;
    float       ToFloat() const;
    static Half FromFloat(float f);   // round to nearest even
    friend bool operator==(Half, Half) = default;
};

// A decoded value. Integers, f32 bits, block refs and string refs live in `bits`
// (floats as raw bits, so NaN payloads survive); strings in `str`; opaque bytes
// in `bytes`; struct fields and array elements in `items`. A union holds the
// chosen case's index in `bits` and its value in `items[0]` (no items for an
// empty case). A conditional field whose condition was false is
// `present == false`.
//
// A PACKED array (a `packed: true` field whose elements all have the same size,
// loaded in LoadMode::Packed) keeps its elements as their encoded bytes in
// `bytes`, with the element count in the low 32 bits of `bits` and the element
// size in the high 32; `items` is empty. Read it through Block::Visit / GetAt /
// Count (which decode just the elements a path reaches) — never through `items`.
struct Value {
    uint64_t             bits    = 0;
    std::string          str;
    std::vector<uint8_t> bytes;
    std::vector<Value>   items;
    bool                 present = true;
    bool                 packed  = false;
    friend bool operator==(const Value&, const Value&) = default;
};

// How Block::Load stores `packed: true` arrays. Packed keeps large arrays
// (vertices, triangles, keys) as bytes and decodes elements on access; every
// element is still validated at load. Materialized decodes everything into the
// value tree (the real-data gate uses it, so a forced re-encode proves every
// element's layout).
enum class LoadMode : uint8_t { Packed, Materialized };

// A path into a block's value: struct field indices, with an element index after
// each array field and a case index after each union field
// ("bones/2/transform" = { bones, 2, transform }).
struct RelPath {
    std::array<uint16_t, 8> idx{};
    uint8_t depth = 0;

    RelPath Child(uint16_t i) const {
        RelPath p = *this;
        if (p.depth < p.idx.size()) p.idx[p.depth++] = i;
        return p;
    }
};

// Generated handles (include/niffer/generated/Fields.h). `block` is the class
// that declares the field (a BlockDef index); the handle works on any block that
// is-a that class, because inherited fields keep their positions. `T` is the
// value type the schema gives the field, so a wrong name or type fails to compile.
template <class T>
struct FieldRef {
    uint16_t block = 0;
    RelPath  path;
};
struct StructRef {
    uint16_t block = 0;
    RelPath  path;
};

// ── Block ─────────────────────────────────────────────────────────────────────
class Block {
public:
    // Never fails: a block with no concrete descriptor, or one whose decode was
    // refused, is raw (Modeled() == false) and keeps its bytes.
    static Block Load(const schema::SchemaSet& set, std::string_view typeName,
                      std::span<const uint8_t> bytes, LoadMode mode = LoadMode::Packed);
    // A block of class `typeName` built from a value tree (the layout's root
    // struct, with no packed arrays): `root` is encoded, and the bytes decoded
    // again. The block is returned, as if loaded from those bytes, only when the
    // decode gives back exactly `root`; a tree that contradicts itself (a count
    // field that doesn't match its array, a field present where its condition is
    // false, a union case its selector doesn't pick) is refused, with the first
    // differing path in `error`.
    static std::optional<Block> Build(const schema::SchemaSet& set, std::string_view typeName,
                                      const Value& root, LoadMode mode = LoadMode::Packed,
                                      std::string* error = nullptr);

    std::string_view          TypeName() const { return typeName_; }
    bool                      Modeled() const { return def_ != nullptr; }
    // Why a block that has a descriptor stayed raw ("" when modeled or no descriptor).
    const std::string&        Refusal() const { return refusal_; }
    // The descriptor's class, or nullptr when raw.
    const schema::BlockDef*   Def() const { return def_; }
    const schema::SchemaSet*  Schema() const { return set_; }
    bool                      IsA(uint16_t blockIndex) const;
    bool                      IsA(std::string_view blockName) const;
    std::span<const uint8_t>  Original() const { return original_; }
    bool                      Dirty() const { return dirty_; }

    // Emits the original bytes unless the block is dirty or `force` (modeled
    // blocks only) re-encodes it from its value.
    void                 Encode(std::vector<uint8_t>& out, bool force = false) const;
    std::vector<uint8_t> Encode(bool force = false) const;

    // ── Typed access through generated handles ───────────────────────────────
    template <class T> std::optional<T> Get(const FieldRef<T>& f) const;
    template <class T> bool Set(const FieldRef<T>& f, const T& v);

    // ── Path access (tools/UIs): "transform/translation/x", "bones/0/transform"
    std::optional<RelPath> Find(std::string_view path) const;
    const Value*           Root() const { return Modeled() ? &value_ : nullptr; }
    // The value at `p` in the stored tree; nullptr when the path doesn't exist,
    // the field is absent, or the path enters a packed array (use Visit).
    const Value*           GetValue(const RelPath& p) const;
    // Calls fn(type, value) with the value at `p`, decoding elements of packed
    // arrays on the way (a path ending ON a packed array gets it unpacked).
    // Returns false (fn not called) when the path doesn't exist or is absent.
    bool                   Visit(const RelPath& p,
                                 const std::function<void(const schema::TypeDef&, const Value&)>& fn) const;
    bool                   Has(const RelPath& p) const;
    // Element count of the array at `p` (packed or not); 0 when absent.
    size_t                 Count(const RelPath& p) const;
    // The selected case of the union at `p`; -1 when absent.
    int                    CaseAt(const RelPath& p) const;
    // Calls fn(index, elementType, element) for every element of the array at
    // `p`, decoding each element exactly once (packed or not), in order. For
    // bulk readers (vertices, triangles, keys): index an element's `items`
    // with the generated views' field-index constants (view::BSVertex::kPosition).
    // Returns false when `p` isn't a present array.
    bool                   ForEachElement(const RelPath& p,
                                          const std::function<void(size_t index, const schema::TypeDef& type,
                                                                   const Value& element)>& fn) const;
    // The schema type at `p` (whether or not the value is present).
    const schema::TypeDef* TypeAt(const RelPath& p) const;
    // Replaces the value at `p`. A no-op when unchanged, so untouched blocks stay
    // verbatim. Replacing an array/bytes field also updates its length field. A
    // path into a packed array unpacks that array first.
    // Returns false when the block isn't modeled or `p` doesn't exist.
    bool                   StoreValue(const RelPath& p, Value v);

    template <class T> std::optional<T> GetAt(const RelPath& p) const;
    template <class T> std::vector<T>   GetElements(const RelPath& arrayPath) const;
    template <class T> bool             SetAt(const RelPath& p, const T& v);

    // ── References ──────────────────────────────────────────────────────────
    // Every ref/ptr value in layout order, with its declared targets (BlockDef
    // indices; empty = any).
    void ForEachRef(const std::function<void(int32_t index, bool isPtr,
                                             std::span<const uint16_t> to)>& fn) const;
    // Every stringref value in layout order.
    void ForEachStringRef(const std::function<void(uint32_t index)>& fn) const;

    // T <-> Value given the TypeDef the value lives in.
    template <class T> static T     FromValue(const Value& v, const schema::TypeDef& t, const schema::SchemaSet& s);
    template <class T> static Value ToValue(const T& x, const schema::TypeDef& t, const schema::SchemaSet& s);

private:
    // `structs` (optional) receives the structs enclosing the value, outermost
    // first: the context a packed element at `p` decodes against.
    const Value* Locate(const RelPath& p, std::deque<Value>& temps, const schema::TypeDef*& t,
                        bool unpackEnd,
                        std::vector<std::pair<const schema::TypeDef*, const Value*>>* structs = nullptr) const;

    const schema::SchemaSet* set_ = nullptr;
    const schema::BlockDef*  def_ = nullptr;
    std::string              typeName_;
    std::vector<uint8_t>     original_;
    Value                    value_;
    std::string              refusal_;
    bool                     dirty_ = false;

    friend struct BlockAccess;
};

// ── NifFile — the container ──────────────────────────────────────────────────
// Header, blocks, footer roots and trailing bytes (SSE only). Each block body is
// a Block interpreted by the schema.
class NifFile {
public:
    Header                header;
    std::vector<Block>    blocks;     // index == file block index
    std::vector<int32_t>  roots;      // footer root refs
    std::vector<uint8_t>  trailing;   // bytes past the footer, verbatim

    static std::expected<NifFile, NifError> Load(const uint8_t* data, size_t len,
                                                 const schema::SchemaSet& set = schema::Main(),
                                                 LoadMode mode = LoadMode::Packed);
    static std::expected<NifFile, NifError> Load(const std::vector<uint8_t>& bytes,
                                                 const schema::SchemaSet& set = schema::Main(),
                                                 LoadMode mode = LoadMode::Packed);
    static std::expected<NifFile, NifError> LoadFile(const std::string& path,
                                                     const schema::SchemaSet& set = schema::Main(),
                                                     LoadMode mode = LoadMode::Packed);

    // Block sizes are re-measured; every other header fact is emitted verbatim.
    // `force` re-encodes every modeled block from its value (the real-data gate).
    std::vector<uint8_t> Save(bool force = false) const;

    Block*           GetBlock(int32_t index);
    const Block*     GetBlock(int32_t index) const;
    std::string_view String(NiStringRef ref) const;  // "" if empty/invalid
    size_t           RawCount() const;

    // Resolves a typed ref to its view; nullopt if null, out of range, raw, or
    // not of the view's class.
    template <class V> std::optional<V> Get(NiRef<V> ref) {
        Block* b = GetBlock(ref.index);
        return b ? V::From(*b) : std::nullopt;
    }
};

// ── Value conversions ────────────────────────────────────────────────────────
namespace schema_detail {
template <class T> struct IsVector : std::false_type {};
template <class T> struct IsVector<std::vector<T>> : std::true_type { using Elem = T; };

inline uint64_t MaskFor(schema::Kind k) {
    switch (k) {
    case schema::Kind::U8:  case schema::Kind::I8:  return 0xFFull;
    case schema::Kind::U16: case schema::Kind::I16: case schema::Kind::Half: return 0xFFFFull;
    case schema::Kind::U64: case schema::Kind::I64: return ~0ull;
    default:                                        return 0xFFFFFFFFull;
    }
}
}  // namespace schema_detail

template <class T>
T Block::FromValue(const Value& v, const schema::TypeDef& t, const schema::SchemaSet& s) {
    if constexpr (std::is_same_v<T, Value>) {
        return v;
    } else if constexpr (std::is_same_v<T, float>) {
        return std::bit_cast<float>(static_cast<uint32_t>(v.bits));
    } else if constexpr (std::is_same_v<T, Half>) {
        return Half{ static_cast<uint16_t>(v.bits) };
    } else if constexpr (std::is_integral_v<T>) {
        return static_cast<T>(v.bits);
    } else if constexpr (std::is_base_of_v<NiRefBase, T>) {
        T r;
        r.index = static_cast<int32_t>(static_cast<uint32_t>(v.bits));
        return r;
    } else if constexpr (std::is_same_v<T, NiStringRef>) {
        return NiStringRef{ static_cast<uint32_t>(v.bits) };
    } else if constexpr (std::is_same_v<T, std::string>) {
        return v.str;
    } else if constexpr (schema_detail::IsVector<T>::value) {
        using E = typename schema_detail::IsVector<T>::Elem;
        T out;
        if (t.kind == schema::Kind::Bytes) {
            if constexpr (std::is_same_v<E, uint8_t>) out = v.bytes;
            return out;
        }
        const schema::TypeDef& et = s.types[t.ref];
        out.reserve(v.items.size());
        for (const Value& item : v.items) out.push_back(FromValue<E>(item, et, s));
        return out;
    } else {
        static_assert(sizeof(T) == 0, "niffer::Block: unsupported value type");
    }
}

template <class T>
Value Block::ToValue(const T& x, const schema::TypeDef& t, const schema::SchemaSet& s) {
    Value v;
    if constexpr (std::is_same_v<T, Value>) {
        v = x;
    } else if constexpr (std::is_same_v<T, float>) {
        v.bits = std::bit_cast<uint32_t>(x);
    } else if constexpr (std::is_same_v<T, Half>) {
        v.bits = x.bits;
    } else if constexpr (std::is_integral_v<T>) {
        v.bits = static_cast<uint64_t>(x) & schema_detail::MaskFor(t.kind);
    } else if constexpr (std::is_base_of_v<NiRefBase, T>) {
        v.bits = static_cast<uint32_t>(x.index);
    } else if constexpr (std::is_same_v<T, NiStringRef>) {
        v.bits = x.index;
    } else if constexpr (std::is_same_v<T, std::string>) {
        v.str = x;
    } else if constexpr (schema_detail::IsVector<T>::value) {
        using E = typename schema_detail::IsVector<T>::Elem;
        if (t.kind == schema::Kind::Bytes) {
            if constexpr (std::is_same_v<E, uint8_t>) v.bytes = x;
            return v;
        }
        const schema::TypeDef& et = s.types[t.ref];
        v.items.reserve(x.size());
        for (const E& e : x) v.items.push_back(ToValue<E>(e, et, s));
    } else {
        static_assert(sizeof(T) == 0, "niffer::Block: unsupported value type");
    }
    return v;
}

template <class T>
std::optional<T> Block::GetAt(const RelPath& p) const {
    std::optional<T> out;
    Visit(p, [&](const schema::TypeDef& t, const Value& v) { out = FromValue<T>(v, t, *set_); });
    return out;
}

template <class T>
std::vector<T> Block::GetElements(const RelPath& arrayPath) const {
    std::vector<T> out;
    Visit(arrayPath, [&](const schema::TypeDef& t, const Value& v) {
        if (t.kind != schema::Kind::Array) return;
        const schema::TypeDef& et = set_->types[t.ref];
        out.reserve(v.items.size());
        for (const Value& item : v.items) out.push_back(FromValue<T>(item, et, *set_));
    });
    return out;
}

template <class T>
bool Block::SetAt(const RelPath& p, const T& x) {
    const schema::TypeDef* t = TypeAt(p);
    if (!t) return false;
    return StoreValue(p, ToValue<T>(x, *t, *set_));
}

template <class T>
std::optional<T> Block::Get(const FieldRef<T>& f) const {
    if (!IsA(f.block)) return std::nullopt;
    return GetAt<T>(f.path);
}

template <class T>
bool Block::Set(const FieldRef<T>& f, const T& v) {
    if (!IsA(f.block)) return false;
    return SetAt<T>(f.path, v);
}

}  // namespace niffer

#include <niffer/generated/Fields.h>
#include <niffer/generated/Views.h>

// ── Decode helpers over NifFile ──────────────────────────────────────────────
// Geometry and collision (DecodedMesh and CollisionGeometry are in Niffer.h).
// They read through the generated views and Block::ForEachElement, so packed
// vertex arrays are decoded one element at a time and never materialized. Not
// on the round-trip path.
namespace niffer {

// A shape's own vertices and triangles. numVertices is the shape's; the
// attribute arrays stay empty when it stores no geometry (dataSize 0: skinned).
DecodedMesh DecodeMesh(const view::BSTriShape& shape);
// A skin partition's vertices, and the triangles of all its partitions (which
// index the partition's whole vertex array).
DecodedMesh DecodeMesh(const view::NiSkinPartition& partition);
// A shape's renderable geometry wherever it lives: the shape's own; its skin
// partition's when the shape stores none; for BSDynamicTriShape (FaceGen
// heads), attributes and triangles from the partition with positions from the
// shape's dynamic vertices. Empty when `shape` isn't a BSTriShape.
DecodedMesh DecodeShapeMesh(NifFile& nif, Block& shape);
// Collision geometry; see CollisionGeometry in Niffer.h.
CollisionGeometry ExtractCollision(NifFile& nif);

}  // namespace niffer
