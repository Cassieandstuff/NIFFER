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
//   FieldDef  one struct field, with its length source, condition, union
//             selector and computation, each as (structs up, field index)
//             from the field's own struct. A `size` computation sums fields of
//             the same struct (→ sums range of field indices).
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

// A computed field's rule (spec §6.2). Size: the encoded byte size of the
// fields `sums` names (absent ones count 0). Bits: ((source >> shift) & mask)
// * times, the source an earlier integer field.
enum class Compute : uint8_t { None, Size, Bits };

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
    Compute     compute   = Compute::None;
    uint16_t    sumFirst  = 0;         // Size: the summed fields (sums range; same struct)
    uint16_t    sumCount  = 0;
    uint8_t     srcUp     = 0;         // Bits: the source field…
    uint16_t    srcField  = kNoField;
    uint64_t    srcMask   = 0;         //   …shifted right, ANDed with this (0 = all bits)…
    uint8_t     srcShift  = 0;
    uint32_t    srcTimes  = 1;         //   …and multiplied by this
    uint16_t    names     = kNoField;  // `enum:` / `flags:`: an EnumDef (SchemaSet::enums) naming its values or bits
};

struct CaseDef {
    const char* name      = nullptr;
    uint16_t    type      = kNoField;  // kNoField: an empty case (zero bytes)
    uint16_t    whenFirst = 0;         // selector values (whens range)
    uint16_t    whenCount = 0;
    bool        isDefault = false;     // taken when no other case matches
};

// A value of an `enum` descriptor (spec §4.5): its name and its value, or for a
// flags enum the bit's mask.
struct EnumValueDef {
    const char* name  = nullptr;
    uint64_t    value = 0;
};

// An `enum` descriptor: named values of an integer field, or named bits of a
// flags word. `source` says where the names come from (NIFFER's naming rule).
struct EnumDef {
    const char* name   = nullptr;
    bool        flags  = false;
    uint16_t    first  = 0;        // its values (SchemaSet::enumValues range)
    uint16_t    count  = 0;
    const char* source = nullptr;
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
    std::span<const uint16_t> sums;      // Compute::Size field lists
    std::span<const BlockDef> blocks;    // sorted by name
    const char*               schemaVersion   = nullptr;
    const char*               languageVersion = nullptr;
    std::span<const EnumDef>      enums;        // sorted by name
    std::span<const EnumValueDef> enumValues;

    // The names `f` reads by (its `enum:` or `flags:`); nullptr when it has none.
    const EnumDef* NamesOf(const FieldDef& f) const {
        return f.names < enums.size() ? &enums[f.names] : nullptr;
    }
    // `e`'s values (for a flags enum, its bits as masks).
    std::span<const EnumValueDef> ValuesOf(const EnumDef& e) const { return enumValues.subspan(e.first, e.count); }
    // The name of value `v` (a value enum) or of the single bit `v` (a flags enum); "" when unnamed.
    std::string_view NameOf(const EnumDef& e, uint64_t v) const {
        for (const EnumValueDef& d : ValuesOf(e))
            if (d.value == v) return d.name;
        return {};
    }

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
    // Derivable fields: a length field (one that gives the element count or
    // byte size of a later array or bytes field: `count: <field>`,
    // `size: <field>`) and a computed field (`compute:`, spec §6.2).
    // DeriveFields sets every ABSENT derivable field whose condition holds:
    // a length field to the length of the present containers it sizes (which
    // must agree: false, with `error`, when they don't), then a computed field
    // to its computation. OmitDerivable clears (present = false) every
    // derivable field whose value DeriveFields would give back. A text form
    // uses the pair to leave them out. `root` must hold no packed arrays.
    static bool DeriveFields(const schema::SchemaSet& set, std::string_view typeName, Value& root,
                             std::string* error = nullptr);
    static void OmitDerivable(const schema::SchemaSet& set, std::string_view typeName, Value& root);
    // The first computed field (as a path, "partitions/0/vertexSize") whose
    // stored value isn't its computation; "" when every one holds. The
    // real-data gate checks `compute:` claims with it.
    static std::string ComputedMismatch(const schema::SchemaSet& set, std::string_view typeName,
                                        const Value& root);
    // A new block of class `typeName` with every field zero (arrays empty,
    // conditions and unions as zero selectors decide), refs and ptrs null and
    // stringrefs empty. nullopt when the class has no concrete descriptor. Set
    // its fields through the views, then NifFile::InsertBlock it.
    static std::optional<Block> Create(const schema::SchemaSet& set, std::string_view typeName,
                                       LoadMode mode = LoadMode::Packed);

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
    // verbatim. Replacing an array/bytes field also updates its length field,
    // and every computed field is recomputed. A path into a packed array
    // unpacks that array first.
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
    // Replaces every ref/ptr value r with fn(r), in layout order (null refs
    // included: fn(-1)). Marks the block dirty only when a value changed, so an
    // identity map keeps it verbatim. Returns whether anything changed; false
    // for a raw block, whose refs can't be found.
    bool MapRefs(const std::function<int32_t(int32_t index)>& fn);
    // The same for stringref values (0xFFFFFFFF = empty is passed too).
    bool MapStringRefs(const std::function<uint32_t(uint32_t index)>& fn);
    // Removes element `i` of the variable-length array at `p` (its length field
    // follows). False when `p` isn't such an array or `i` is past its end.
    bool EraseElement(const RelPath& p, size_t i);

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

    // ── Editing the block list ──────────────────────────────────────────────
    // Refs are file-native indices, so inserting or removing a block renumbers
    // every later block: each ref, ptr and footer root is fixed up, and the
    // header's block-type table follows. A block whose value doesn't change
    // stays verbatim, so an unedited file still saves byte-for-byte, and
    // removing what was inserted gives back the original bytes.
    //
    // Fix-ups need every block's refs, so editing refuses (with a reason) while
    // the file holds a raw block that would need renumbering; appending
    // renumbers nothing and is always allowed.

    // Inserts `block` (which must be modeled) at `index` (0..blocks.size()):
    // every ref, ptr and root at or past `index` moves up one. Its class joins
    // header.blockTypes (at the end) if it isn't there. The inserted block's own
    // refs are taken as already in the new numbering. Returns `index`.
    std::expected<int32_t, std::string> InsertBlock(int32_t index, Block block);
    std::expected<int32_t, std::string> AppendBlock(Block block) {
        return InsertBlock(static_cast<int32_t>(blocks.size()), std::move(block));
    }
    // Removes the blocks at `indices` (any order, duplicates ignored). Every ref
    // or ptr to a removed block becomes null (-1), in a list as well (list
    // lengths never change; Block::EraseElement drops an entry); roots that
    // were removed leave the footer. A block type, or a string, that only the
    // removed blocks used leaves its table (later stringrefs renumbered); types
    // and strings unused before stay. Returns the number removed.
    std::expected<size_t, std::string> RemoveBlocks(std::span<const int32_t> indices);
    std::expected<size_t, std::string> RemoveBlock(int32_t index) { return RemoveBlocks({ &index, 1 }); }
    // `index` plus every block it owns: reached from it through refs (not
    // ptrs), and owned by nothing outside that set. Those are removed together
    // (RemoveBlocks). Returns the removed indices in ascending order.
    std::vector<int32_t> OwnedTree(int32_t index) const;
    std::expected<size_t, std::string> RemoveTree(int32_t index);
    // The index of `s` in the header string table, appended if absent
    // (maxStringLength grows to cover it).
    NiStringRef AddString(std::string_view s);
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
// One entry per rigid body reached from a collision object (phantoms excluded),
// in block order; see Body in Niffer.h.
std::vector<Body> ExtractBodies(NifFile& nif);

// ── Tangent space (docs/tangent-space.md) ────────────────────────────────────
// DecodedMesh's tangent frame is the NIF's, by its field names: `tangents` xyz is
// the vertex's tangent field (it follows +dP/dV), `bitangents` the vector the
// three W slots carry (it follows +dP/dU), and the tangent's W is the
// handedness sign((N x T) . B): -1 where the UVs are mirrored relative to the
// usual frame. A renderer that wants T along +U and B = W' (N x T) along +V
// takes T' = bitangent and W' = -W.
//
// Recomputes the tangent frame of `mesh` from its positions, normals, UVs and
// triangles, in that convention (area-weighted UV gradients, each made
// orthogonal to the normal). Needs normals and UVs; fills `tangents` and
// `bitangents`.
void ComputeTangentSpace(DecodedMesh& mesh);
// Writes `mesh`'s tangents and bitangents (or, without bitangents, W (N x T))
// into the vertices DecodeShapeMesh(nif, shape) reads them from: the shape's
// own, or its skin partition's. Values that come back the same leave the block
// verbatim. False when the counts don't match or there is no tangent to write.
bool EncodeTangentSpace(NifFile& nif, Block& shape, const DecodedMesh& mesh);

// ── Authoring collision (types in Niffer.h) ──────────────────────────────────
// Gives NiNode `node` a rigid body made of `hulls`: bhkConvexVerticesShape per
// hull (under a bhkListShape when there are several), bhkRigidBody and
// bhkCollisionObject, appended in that order (dependencies first, as vanilla
// files order them), with the node's collisionObject pointed at it. A collision
// object the node already has is removed first, with everything only it owns.
std::expected<AttachedCollision, std::string> AttachConvexCollision(NifFile& nif, int32_t node,
                                                                    std::span<const ConvexHullInput> hulls,
                                                                    const CollisionDesc& desc = {});
// Removes `node`'s collision object and every block only it owns; its
// collisionObject becomes null. Returns the number of blocks removed (0 when it
// had none).
std::expected<size_t, std::string> RemoveCollision(NifFile& nif, int32_t node);

}  // namespace niffer
