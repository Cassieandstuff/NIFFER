// ── niffer::Block — the generic block engine ─────────────────────────────────
// Interprets a SchemaSet's tables to decode a block's bytes into a Value tree
// and encode it back. One engine for every block class: nothing in this file
// knows any block by name. Semantics: docs/schema-language.md §7.
#include <niffer/Niffer.h>

#include <charconv>
#include <cstring>
#include <deque>

namespace niffer {

using schema::CaseDef;
using schema::CondOp;
using schema::FieldDef;
using schema::Kind;
using schema::LenKind;
using schema::SchemaSet;
using schema::TypeDef;

// ── Half ──────────────────────────────────────────────────────────────────────
float Half::ToFloat() const {
    const uint32_t sign = (bits & 0x8000u) << 16;
    uint32_t exp  = (bits >> 10) & 0x1F;
    uint32_t mant = bits & 0x3FF;
    uint32_t out;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {  // subnormal
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0) { mant <<= 1; --exp; }
            mant &= 0x3FF;
            out = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        out = sign | 0x7F800000u | (mant << 13);  // inf / nan
    } else {
        out = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    return std::bit_cast<float>(out);
}

Half Half::FromFloat(float f) {
    const uint32_t x    = std::bit_cast<uint32_t>(f);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t absx = x & 0x7FFFFFFFu;
    if (absx >= 0x7F800000u)   // inf / nan (keep a quiet nan payload bit)
        return Half{ static_cast<uint16_t>(sign | 0x7C00u | (absx > 0x7F800000u ? 0x200u : 0u)) };
    if (absx >= 0x477FF000u)   // rounds past the largest half: inf
        return Half{ static_cast<uint16_t>(sign | 0x7C00u) };
    if (absx < 0x38800000u) {  // subnormal or zero
        const uint32_t shift = 113 - (absx >> 23);
        if (shift > 24) return Half{ static_cast<uint16_t>(sign) };
        const uint32_t mant = (absx & 0x7FFFFFu) | 0x800000u;
        uint32_t h = mant >> (shift + 13);
        const uint32_t rem = mant & ((1u << (shift + 13)) - 1u);
        const uint32_t half = 1u << (shift + 12);
        if (rem > half || (rem == half && (h & 1u))) ++h;
        return Half{ static_cast<uint16_t>(sign | h) };
    }
    uint32_t h = ((absx >> 13) - (112u << 10));
    const uint32_t rem = absx & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
    return Half{ static_cast<uint16_t>(sign | h) };
}

namespace {

size_t ScalarSize(Kind k) {
    switch (k) {
    case Kind::U8:  case Kind::I8:  return 1;
    case Kind::U16: case Kind::I16: case Kind::Half: return 2;
    case Kind::U32: case Kind::I32: case Kind::F32:
    case Kind::Ref: case Kind::Ptr: case Kind::StringRef: return 4;
    case Kind::U64: case Kind::I64: return 8;
    default: return 0;
    }
}

bool IsSigned(Kind k) {
    return k == Kind::I8 || k == Kind::I16 || k == Kind::I32 || k == Kind::I64;
}

// Sign-extends a stored integer to int64 when its kind is signed.
int64_t AsInt(uint64_t bits, Kind k) {
    switch (k) {
    case Kind::I8:  return static_cast<int8_t>(bits);
    case Kind::I16: return static_cast<int16_t>(bits);
    case Kind::I32: return static_cast<int32_t>(bits);
    case Kind::I64: return static_cast<int64_t>(bits);
    default:        return static_cast<int64_t>(bits);
    }
}

bool Holds(CondOp op, uint64_t bits, Kind k, int64_t operand) {
    if (op == CondOp::Mask) return (bits & static_cast<uint64_t>(operand)) != 0;
    if (IsSigned(k)) {
        const int64_t v = AsInt(bits, k);
        switch (op) {
        case CondOp::Eq:  return v == operand;
        case CondOp::Ne:  return v != operand;
        case CondOp::Min: return v >= operand;
        case CondOp::Max: return v <= operand;
        default:          return true;
        }
    }
    const uint64_t v = bits, o = static_cast<uint64_t>(operand);
    switch (op) {
    case CondOp::Eq:  return v == o;
    case CondOp::Ne:  return v != o;
    case CondOp::Min: return v >= o;
    case CondOp::Max: return v <= o;
    default:          return true;
    }
}

// A struct being decoded or encoded: field references (lengths, conditions,
// union selectors) resolve against this stack, innermost last.
struct Frame {
    const TypeDef* type;
    const Value*   value;
};

class Scope {
public:
    explicit Scope(const SchemaSet& s) : set_(s) {}

    void Push(const TypeDef* t, const Value* v) { frames_.push_back({t, v}); }
    void Pop() { frames_.pop_back(); }

    // The referenced field's value and kind; nullptr when unavailable.
    const Value* Lookup(uint8_t up, uint16_t field, Kind& kind) const {
        if (up >= frames_.size()) return nullptr;
        const Frame& f = frames_[frames_.size() - 1 - up];
        if (field >= f.type->refCount || field >= f.value->items.size()) return nullptr;
        kind = set_.types[set_.fields[f.type->ref + field].type].kind;
        return &f.value->items[field];
    }

    // An unreadable condition field counts as false (the field is absent).
    bool CondHolds(const FieldDef& fd) const {
        if (fd.cond == CondOp::None) return true;
        Kind k{};
        const Value* v = Lookup(fd.condUp, fd.condField, k);
        return v && v->present && Holds(fd.cond, v->bits, k, fd.condValue);
    }

    // -1 when the length field can't be read.
    int64_t Length(const FieldDef& fd) const {
        Kind k{};
        const Value* v = Lookup(fd.lenUp, fd.lenField, k);
        if (!v || !v->present) return -1;
        return AsInt(v->bits, k);
    }

    // The union case the selector picks: a matching case, else the default,
    // else -1.
    int SelectCase(const TypeDef& u, const FieldDef& fd) const {
        Kind k{};
        const Value* v = Lookup(fd.selUp, fd.selField, k);
        if (!v || !v->present) return -1;
        const bool masked = fd.selMask != 0;
        const uint64_t bits = masked ? (v->bits & fd.selMask) : v->bits;
        int dflt = -1;
        for (uint16_t c = 0; c < u.refCount; ++c) {
            const CaseDef& cd = set_.cases[u.ref + c];
            if (cd.isDefault) { dflt = c; continue; }
            for (uint16_t w = 0; w < cd.whenCount; ++w) {
                const int64_t when = set_.whens[cd.whenFirst + w];
                const bool hit = (!masked && IsSigned(k)) ? AsInt(bits, k) == when
                                                          : bits == static_cast<uint64_t>(when);
                if (hit) return c;
            }
        }
        return dflt;
    }

private:
    const SchemaSet&   set_;
    std::vector<Frame> frames_;
};

// ── Decoder ───────────────────────────────────────────────────────────────────
class Decoder {
public:
    Decoder(const SchemaSet& s, const uint8_t* beg, const uint8_t* end,
            LoadMode mode = LoadMode::Materialized)
        : set_(s), scope_(s), beg_(beg), cur_(beg), end_(end), mode_(mode) {}

    // Context for decoding a value below the block root (packed elements).
    void PushFrame(const TypeDef* t, const Value* v) { scope_.Push(t, v); }

    size_t Remaining() const { return static_cast<size_t>(end_ - cur_); }
    size_t Offset() const { return static_cast<size_t>(cur_ - beg_); }
    const std::string& Error() const { return err_; }

    // `fieldLen` is the length from a LenKind::Field source (-1 when none);
    // `fd` is the field being decoded (unions read their selector through it).
    bool Decode(const TypeDef& t, Value& v, int64_t fieldLen, const FieldDef* fd) {
        switch (t.kind) {
        case Kind::U8: case Kind::U16: case Kind::U32: case Kind::U64:
        case Kind::I8: case Kind::I16: case Kind::I32: case Kind::I64:
        case Kind::F32: case Kind::Half: case Kind::Ref: case Kind::Ptr: case Kind::StringRef: {
            const size_t n = ScalarSize(t.kind);
            if (Remaining() < n) return Fail("scalar past end of block");
            uint64_t bits = 0;
            std::memcpy(&bits, cur_, n);
            cur_ += n;
            v.bits = bits;
            return true;
        }
        case Kind::SizedString: {
            if (Remaining() < 4) return Fail("string length past end of block");
            uint32_t n = 0;
            std::memcpy(&n, cur_, 4);
            cur_ += 4;
            if (Remaining() < n) return Fail("string past end of block");
            v.str.assign(reinterpret_cast<const char*>(cur_), n);
            cur_ += n;
            return true;
        }
        case Kind::Bytes: {
            size_t n = 0;
            switch (t.len) {
            case LenKind::Fixed: n = t.count; break;
            case LenKind::Rest:  n = Remaining(); break;
            case LenKind::Field:
                if (fieldLen < 0) return Fail("bytes: invalid size field");
                n = static_cast<size_t>(fieldLen);
                break;
            default: return Fail("bytes: unsupported length");
            }
            if (Remaining() < n) return Fail("bytes past end of block");
            v.bytes.assign(cur_, cur_ + n);
            cur_ += n;
            return true;
        }
        case Kind::Struct: {
            scope_.Push(&t, &v);
            v.items.reserve(t.refCount);
            for (uint16_t i = 0; i < t.refCount; ++i) {
                const FieldDef& f = set_.fields[t.ref + i];
                v.items.emplace_back();
                Value& fv = v.items.back();
                if (!scope_.CondHolds(f)) { fv.present = false; continue; }
                const int64_t len = f.lenField != schema::kNoField ? scope_.Length(f) : -1;
                const TypeDef& ft = set_.types[f.type];
                const bool ok = (f.packed && mode_ == LoadMode::Packed && ft.kind == Kind::Array)
                                    ? DecodePacked(ft, fv, len)
                                    : Decode(ft, fv, len, &f);
                if (!ok) { scope_.Pop(); return false; }
            }
            scope_.Pop();
            return true;
        }
        case Kind::Union: {
            const int c = fd ? scope_.SelectCase(t, *fd) : -1;
            if (c < 0) return Fail("union: no case for the selector's value");
            v.bits = static_cast<uint64_t>(c);
            const CaseDef& cd = set_.cases[t.ref + static_cast<uint16_t>(c)];
            if (cd.type == schema::kNoField) return true;   // empty case
            v.items.emplace_back();
            return Decode(set_.types[cd.type], v.items.back(), -1, nullptr);
        }
        case Kind::Array:
            return DecodeArray(t, v, fieldLen);
        }
        return Fail("unknown kind");
    }

private:
    // A `packed: true` array: every element is decoded (validated) into a
    // scratch value; if all have the same size the array keeps just its bytes.
    // Otherwise it is decoded again, materialized.
    bool DecodePacked(const TypeDef& t, Value& v, int64_t fieldLen) {
        const uint8_t* savedCur = cur_;
        const uint8_t* savedEnd = end_;
        uint64_t count = 0;
        if (!ArrayBounds(t, fieldLen, count)) { end_ = savedEnd; return false; }
        const uint8_t* start = cur_;
        const TypeDef& et = set_.types[t.ref];
        const bool open = t.len == LenKind::Rest || t.len == LenKind::Window;
        uint64_t n = 0;
        size_t elemSize = 0;
        bool uniform = true;
        while (open ? Remaining() > 0 : n < count) {
            const uint8_t* before = cur_;
            Value scratch;
            if (!Decode(et, scratch, -1, nullptr)) { end_ = savedEnd; return false; }
            const size_t sz = static_cast<size_t>(cur_ - before);
            if (sz == 0) { end_ = savedEnd; return Fail("array element consumed no bytes"); }
            if (n == 0) elemSize = sz;
            else if (sz != elemSize) uniform = false;
            ++n;
        }
        const uint8_t* stop = cur_;
        end_ = savedEnd;
        if (!uniform) {            // elements differ in size: keep them materialized
            cur_ = savedCur;
            return DecodeArray(t, v, fieldLen);
        }
        v.packed = true;
        v.bits = n | (static_cast<uint64_t>(elemSize) << 32);
        v.bytes.assign(start, stop);
        return true;
    }

    // Reads the array's length: consumes a count prefix, and for a byte window
    // narrows end_ (the caller restores it). `count` is unused for open arrays.
    bool ArrayBounds(const TypeDef& t, int64_t fieldLen, uint64_t& count) {
        switch (t.len) {
        case LenKind::Rest: return true;
        case LenKind::Window: {
            const int64_t n = fieldLen >= 0 ? fieldLen : static_cast<int64_t>(t.count);
            if (n < 0 || static_cast<uint64_t>(n) > Remaining())
                return Fail("array: byte window exceeds remaining bytes");
            end_ = cur_ + n;
            return true;
        }
        case LenKind::Fixed: count = t.count; break;
        case LenKind::Prefix8:
        case LenKind::Prefix16:
        case LenKind::Prefix32: {
            const size_t w = t.len == LenKind::Prefix8 ? 1 : t.len == LenKind::Prefix16 ? 2 : 4;
            if (Remaining() < w) return Fail("array count past end of block");
            std::memcpy(&count, cur_, w);
            cur_ += w;
            break;
        }
        case LenKind::Field:
            if (fieldLen < 0) return Fail("array: invalid count field");
            count = static_cast<uint64_t>(fieldLen);
            break;
        default: return Fail("array: unsupported length");
        }
        // Every element takes at least one byte in practice; a bogus count
        // (a mis-parsed layout) must refuse, not allocate gigabytes.
        if (count > Remaining()) return Fail("array count exceeds remaining bytes");
        return true;
    }

    bool DecodeArray(const TypeDef& t, Value& v, int64_t fieldLen) {
        const TypeDef& et = set_.types[t.ref];
        if (t.len == LenKind::Rest || t.len == LenKind::Window) {
            const uint8_t* savedEnd = end_;
            if (t.len == LenKind::Window) {
                const int64_t n = fieldLen >= 0 ? fieldLen : static_cast<int64_t>(t.count);
                if (n < 0 || static_cast<uint64_t>(n) > Remaining())
                    return Fail("array: byte window exceeds remaining bytes");
                end_ = cur_ + n;
            }
            bool ok = true;
            while (ok && Remaining() > 0) {
                const uint8_t* before = cur_;
                v.items.emplace_back();
                ok = Decode(et, v.items.back(), -1, nullptr);
                if (ok && cur_ == before) ok = Fail("array element consumed no bytes");
            }
            end_ = savedEnd;
            return ok;
        }
        uint64_t count = 0;
        switch (t.len) {
        case LenKind::Fixed: count = t.count; break;
        case LenKind::Prefix8:
        case LenKind::Prefix16:
        case LenKind::Prefix32: {
            const size_t w = t.len == LenKind::Prefix8 ? 1 : t.len == LenKind::Prefix16 ? 2 : 4;
            if (Remaining() < w) return Fail("array count past end of block");
            std::memcpy(&count, cur_, w);
            cur_ += w;
            break;
        }
        case LenKind::Field:
            if (fieldLen < 0) return Fail("array: invalid count field");
            count = static_cast<uint64_t>(fieldLen);
            break;
        default: return Fail("array: unsupported length");
        }
        // Every element takes at least one byte in practice; a bogus count
        // (a mis-parsed layout) must refuse, not allocate gigabytes.
        if (count > Remaining()) return Fail("array count exceeds remaining bytes");
        v.items.resize(static_cast<size_t>(count));
        for (Value& item : v.items)
            if (!Decode(et, item, -1, nullptr)) return false;
        return true;
    }

    bool Fail(const char* what) {
        if (err_.empty()) err_ = what;
        return false;
    }

    const SchemaSet& set_;
    Scope            scope_;
    const uint8_t*   beg_;
    const uint8_t*   cur_;
    const uint8_t*   end_;
    LoadMode         mode_;
    std::string      err_;
};

// Decodes element `i` of the packed array `arr` (of type `t`) into `out`, with
// the enclosing structs as context.
bool DecodePackedElement(const SchemaSet& s, const TypeDef& t, const Value& arr, size_t i,
                         const std::vector<Frame>& frames, Value& out) {
    const size_t size = static_cast<size_t>(arr.bits >> 32);
    if (size == 0 || (i + 1) * size > arr.bytes.size()) return false;
    const uint8_t* beg = arr.bytes.data() + i * size;
    Decoder dec(s, beg, beg + size);
    for (const Frame& f : frames) dec.PushFrame(f.type, f.value);
    return dec.Decode(s.types[t.ref], out, -1, nullptr) && dec.Remaining() == 0;
}

// A packed array's elements, materialized.
bool UnpackArray(const SchemaSet& s, const TypeDef& t, const Value& arr,
                 const std::vector<Frame>& frames, Value& out) {
    const size_t count = static_cast<size_t>(arr.bits & 0xFFFFFFFFu);
    out = Value{};
    out.items.resize(count);
    for (size_t i = 0; i < count; ++i)
        if (!DecodePackedElement(s, t, arr, i, frames, out.items[i])) return false;
    return true;
}

// ── Encoder ───────────────────────────────────────────────────────────────────
Value ZeroValue(const SchemaSet& s, const TypeDef& t) {
    Value v;
    if (t.kind == Kind::Struct) {
        v.items.reserve(t.refCount);
        for (uint16_t i = 0; i < t.refCount; ++i)
            v.items.push_back(ZeroValue(s, s.types[s.fields[t.ref + i].type]));
    } else if (t.kind == Kind::Array && t.len == LenKind::Fixed) {
        v.items.assign(t.count, ZeroValue(s, s.types[t.ref]));
    } else if (t.kind == Kind::Bytes && t.len == LenKind::Fixed) {
        v.bytes.assign(t.count, 0);
    } else if (t.kind == Kind::Union) {
        v.bits = 0xFFFF;   // no case yet: the encoder writes the selected case's zero
    }
    return v;
}

class Encoder {
public:
    Encoder(const SchemaSet& s, std::vector<uint8_t>& out) : set_(s), scope_(s), out_(out) {}

    // Context for encoding a value below the block root (derived window sizes).
    void PushFrame(const TypeDef* t, const Value* v) { scope_.Push(t, v); }

    void Encode(const TypeDef& t, const Value& v, const FieldDef* fd) {
        switch (t.kind) {
        case Kind::U8: case Kind::U16: case Kind::U32: case Kind::U64:
        case Kind::I8: case Kind::I16: case Kind::I32: case Kind::I64:
        case Kind::F32: case Kind::Half: case Kind::Ref: case Kind::Ptr: case Kind::StringRef:
            Put(v.bits, ScalarSize(t.kind));
            return;
        case Kind::SizedString:
            Put(v.str.size(), 4);
            out_.insert(out_.end(), v.str.begin(), v.str.end());
            return;
        case Kind::Bytes:
            if (t.len == LenKind::Fixed) {
                for (uint32_t i = 0; i < t.count; ++i)
                    out_.push_back(i < v.bytes.size() ? v.bytes[i] : 0);
            } else {
                out_.insert(out_.end(), v.bytes.begin(), v.bytes.end());
            }
            return;
        case Kind::Struct:
            scope_.Push(&t, &v);
            for (uint16_t i = 0; i < t.refCount; ++i) {
                const FieldDef& f = set_.fields[t.ref + i];
                if (!scope_.CondHolds(f)) continue;
                const TypeDef& ft = set_.types[f.type];
                if (i < v.items.size() && v.items[i].present) Encode(ft, v.items[i], &f);
                else Encode(ft, ZeroValue(set_, ft), &f);
            }
            scope_.Pop();
            return;
        case Kind::Union: {
            // The selector decides; a value of another case is written as the
            // selected case's zero value.
            const int c = fd ? scope_.SelectCase(t, *fd) : -1;
            if (c < 0) return;
            const CaseDef& cd = set_.cases[t.ref + static_cast<uint16_t>(c)];
            if (cd.type == schema::kNoField) return;
            const TypeDef& ct = set_.types[cd.type];
            if (v.bits == static_cast<uint64_t>(c) && !v.items.empty()) Encode(ct, v.items[0], nullptr);
            else Encode(ct, ZeroValue(set_, ct), nullptr);
            return;
        }
        case Kind::Array: {
            const TypeDef& et = set_.types[t.ref];
            const size_t n = v.packed ? static_cast<size_t>(v.bits & 0xFFFFFFFFu) : v.items.size();
            switch (t.len) {
            case LenKind::Prefix8:  Put(n, 1); break;
            case LenKind::Prefix16: Put(n, 2); break;
            case LenKind::Prefix32: Put(n, 4); break;
            default: break;
            }
            if (v.packed) {   // the elements' own encoded bytes
                out_.insert(out_.end(), v.bytes.begin(), v.bytes.end());
                return;
            }
            if (t.len == LenKind::Fixed) {
                for (uint32_t i = 0; i < t.count; ++i)
                    Encode(et, i < v.items.size() ? v.items[i] : ZeroValue(set_, et), nullptr);
            } else {
                for (const Value& item : v.items) Encode(et, item, nullptr);
            }
            return;
        }
        }
    }

private:
    void Put(uint64_t bits, size_t n) {
        const auto* p = reinterpret_cast<const uint8_t*>(&bits);
        out_.insert(out_.end(), p, p + n);
    }

    const SchemaSet&      set_;
    Scope                 scope_;
    std::vector<uint8_t>& out_;
};

// ── Tree walks ────────────────────────────────────────────────────────────────
// Calls fn(type, value) for every present leaf, descending through structs,
// arrays and the selected case of each union.
template <class Fn>
void WalkLeaves(const SchemaSet& s, const TypeDef& t, const Value& v, Fn& fn) {
    if (!v.present) return;
    switch (t.kind) {
    case Kind::Struct:
        for (uint16_t i = 0; i < t.refCount && i < v.items.size(); ++i)
            WalkLeaves(s, s.types[s.fields[t.ref + i].type], v.items[i], fn);
        return;
    case Kind::Array:   // packed arrays hold no refs or strings (schemagen enforces it)
        for (const Value& item : v.items) WalkLeaves(s, s.types[t.ref], item, fn);
        return;
    case Kind::Union:
        if (v.bits < t.refCount && !v.items.empty()) {
            const CaseDef& cd = s.cases[t.ref + v.bits];
            if (cd.type != schema::kNoField) WalkLeaves(s, s.types[cd.type], v.items[0], fn);
        }
        return;
    default:
        fn(t, v);
        return;
    }
}

}  // namespace

// ── Block ─────────────────────────────────────────────────────────────────────
Block Block::Load(const SchemaSet& set, std::string_view typeName,
                  std::span<const uint8_t> bytes, LoadMode mode) {
    Block b;
    b.set_ = &set;
    b.typeName_.assign(typeName);
    b.original_.assign(bytes.begin(), bytes.end());

    const schema::BlockDef* def = set.Find(typeName);
    if (!def || def->abstract) return b;   // raw: no descriptor

    const uint8_t* beg = b.original_.data();
    Decoder dec(set, beg, beg + b.original_.size(), mode);
    Value v;
    if (!dec.Decode(set.types[def->type], v, -1, nullptr)) {
        b.refusal_ = dec.Error() + " (at byte " + std::to_string(dec.Offset()) + " of " +
                     std::to_string(b.original_.size()) + ")";
        return b;
    }
    if (dec.Remaining() != 0) {
        b.refusal_ = "layout consumed " + std::to_string(dec.Offset()) + " of " +
                     std::to_string(b.original_.size()) + " bytes";
        return b;
    }
    b.def_   = def;
    b.value_ = std::move(v);
    return b;
}

bool Block::IsA(uint16_t blockIndex) const {
    return def_ && set_->IsA(set_->IndexOf(def_), blockIndex);
}

bool Block::IsA(std::string_view blockName) const {
    const schema::BlockDef* base = set_ ? set_->Find(blockName) : nullptr;
    return base && IsA(set_->IndexOf(base));
}

void Block::Encode(std::vector<uint8_t>& out, bool force) const {
    if (!def_ || (!dirty_ && !force)) {
        out.insert(out.end(), original_.begin(), original_.end());
        return;
    }
    Encoder enc(*set_, out);
    enc.Encode(set_->types[def_->type], value_, nullptr);
}

std::vector<uint8_t> Block::Encode(bool force) const {
    std::vector<uint8_t> out;
    out.reserve(original_.size());
    Encode(out, force);
    return out;
}

// One step down a path: from (t, v) through index i. `v` may be null (type-only
// walks). Returns false when the step doesn't exist; sets `fd` when the step is
// a struct field.
namespace {
template <class V>
bool Step(const SchemaSet& s, const TypeDef*& t, V*& v, uint16_t i, const FieldDef*& fd) {
    fd = nullptr;
    switch (t->kind) {
    case Kind::Struct:
        if (i >= t->refCount || (v && i >= v->items.size())) return false;
        fd = &s.fields[t->ref + i];
        t  = &s.types[fd->type];
        if (v) v = &v->items[i];
        return true;
    case Kind::Array:
        if (v && (v->packed || i >= v->items.size())) return false;
        t = &s.types[t->ref];
        if (v) v = &v->items[i];
        return true;
    case Kind::Union: {
        if (i >= t->refCount) return false;
        if (v && v->bits != i) return false;          // that case isn't selected
        const CaseDef& cd = s.cases[t->ref + i];
        if (cd.type == schema::kNoField) return true; // empty case: stays on the union
        t = &s.types[cd.type];
        if (v) {
            if (v->items.empty()) return false;
            v = &v->items[0];
        }
        return true;
    }
    default:
        return false;
    }
}
}  // namespace

const TypeDef* Block::TypeAt(const RelPath& p) const {
    if (!def_) return nullptr;
    const TypeDef* t = &set_->types[def_->type];
    const Value*   v = nullptr;
    const FieldDef* fd = nullptr;
    for (uint8_t d = 0; d < p.depth; ++d)
        if (!Step(*set_, t, v, p.idx[d], fd)) return nullptr;
    return t;
}

const Value* Block::GetValue(const RelPath& p) const {
    if (!def_) return nullptr;
    const TypeDef* t = &set_->types[def_->type];
    const Value*   v = &value_;
    const FieldDef* fd = nullptr;
    for (uint8_t d = 0; d < p.depth; ++d) {
        if (!Step(*set_, t, v, p.idx[d], fd)) return nullptr;
        if (!v->present) return nullptr;
    }
    return v;
}

// Walks `p` like GetValue, but decodes the elements of packed arrays it passes
// through (into `temps`, which owns them). With `unpackEnd`, a path ending on a
// packed array gets that array materialized too.
const Value* Block::Locate(const RelPath& p, std::deque<Value>& temps, const TypeDef*& t,
                           bool unpackEnd) const {
    if (!def_) return nullptr;
    t = &set_->types[def_->type];
    const Value* v = &value_;
    std::vector<Frame> frames;
    for (uint8_t d = 0; d < p.depth; ++d) {
        if (t->kind == Kind::Struct) frames.push_back({t, v});
        if (t->kind == Kind::Array && v->packed) {
            const size_t i = p.idx[d];
            if (i >= (v->bits & 0xFFFFFFFFu)) return nullptr;
            Value& elem = temps.emplace_back();
            if (!DecodePackedElement(*set_, *t, *v, i, frames, elem)) return nullptr;
            t = &set_->types[t->ref];
            v = &elem;
            continue;
        }
        const FieldDef* fd = nullptr;
        if (!Step(*set_, t, v, p.idx[d], fd)) return nullptr;
        if (!v->present) return nullptr;
    }
    if (unpackEnd && t->kind == Kind::Array && v->packed) {
        Value& all = temps.emplace_back();
        if (!UnpackArray(*set_, *t, *v, frames, all)) return nullptr;
        v = &all;
    }
    return v;
}

bool Block::Visit(const RelPath& p,
                  const std::function<void(const TypeDef&, const Value&)>& fn) const {
    std::deque<Value> temps;
    const TypeDef* t = nullptr;
    const Value* v = Locate(p, temps, t, true);
    if (!v) return false;
    fn(*t, *v);
    return true;
}

bool Block::Has(const RelPath& p) const {
    std::deque<Value> temps;
    const TypeDef* t = nullptr;
    return Locate(p, temps, t, false) != nullptr;
}

size_t Block::Count(const RelPath& p) const {
    std::deque<Value> temps;
    const TypeDef* t = nullptr;
    const Value* v = Locate(p, temps, t, false);
    if (!v || t->kind != Kind::Array) return 0;
    return v->packed ? static_cast<size_t>(v->bits & 0xFFFFFFFFu) : v->items.size();
}

int Block::CaseAt(const RelPath& p) const {
    std::deque<Value> temps;
    const TypeDef* t = nullptr;
    const Value* v = Locate(p, temps, t, false);
    if (!v || t->kind != Kind::Union) return -1;
    return static_cast<int>(v->bits);
}

bool Block::StoreValue(const RelPath& p, Value nv) {
    if (!def_) return false;
    // Walk to the target, remembering the structs on the way (for the length
    // field) and the FieldDef the target is (when its last step is a field).
    struct StructStep { const TypeDef* type; Value* value; };
    std::vector<StructStep> structs;
    const TypeDef*  t  = &set_->types[def_->type];
    Value*          v  = &value_;
    const FieldDef* fd = nullptr;
    for (uint8_t d = 0; d < p.depth; ++d) {
        if (t->kind == Kind::Struct) structs.push_back({t, v});
        if (t->kind == Kind::Array && v->packed) {   // editing inside: unpack it first
            std::vector<Frame> frames;
            for (const StructStep& st : structs) frames.push_back({st.type, st.value});
            Value unpacked;
            if (!UnpackArray(*set_, *t, *v, frames, unpacked)) return false;
            *v = std::move(unpacked);
        }
        if (!Step(*set_, t, v, p.idx[d], fd)) return false;
    }
    if (t->kind == Kind::Union && nv.bits != v->bits) return false;  // the selector chooses
    if (*v == nv) return true;   // no-op: stays verbatim
    *v = std::move(nv);
    v->present = true;
    dirty_ = true;

    // A replaced array / byte run keeps its length field in step (§6).
    if (fd && fd->lenField != schema::kNoField && fd->lenUp < structs.size()) {
        StructStep& owner = structs[structs.size() - 1 - fd->lenUp];
        if (fd->lenField < owner.value->items.size()) {
            uint64_t n = 0;
            if (t->kind == Kind::Bytes) {
                n = v->bytes.size();
            } else if (t->len == LenKind::Window) {
                // A byte window's size is the encoded size of its elements.
                std::vector<uint8_t> tmp;
                Encoder enc(*set_, tmp);
                for (const StructStep& st : structs) enc.PushFrame(st.type, st.value);
                enc.Encode(*t, *v, fd);
                n = tmp.size();
            } else {
                n = v->items.size();
            }
            const Kind lk = set_->types[set_->fields[owner.type->ref + fd->lenField].type].kind;
            Value& lenValue = owner.value->items[fd->lenField];
            lenValue.bits = n & schema_detail::MaskFor(lk);
            lenValue.present = true;
        }
    }
    return true;
}

std::optional<RelPath> Block::Find(std::string_view path) const {
    if (!def_) return std::nullopt;
    RelPath p;
    const TypeDef* t = &set_->types[def_->type];
    while (!path.empty()) {
        const size_t slash = path.find('/');
        const std::string_view seg = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
        if (p.depth >= p.idx.size()) return std::nullopt;
        uint16_t i = 0;
        if (t->kind == Kind::Struct) {
            for (; i < t->refCount; ++i)
                if (seg == set_->fields[t->ref + i].name) break;
            if (i == t->refCount) return std::nullopt;
        } else if (t->kind == Kind::Union) {
            for (; i < t->refCount; ++i)
                if (seg == set_->cases[t->ref + i].name) break;
            if (i == t->refCount) return std::nullopt;
        } else if (t->kind == Kind::Array) {
            auto [ptr, ec] = std::from_chars(seg.data(), seg.data() + seg.size(), i);
            if (ec != std::errc{} || ptr != seg.data() + seg.size()) return std::nullopt;
        } else {
            return std::nullopt;
        }
        const Value* none = nullptr;
        const FieldDef* fd = nullptr;
        if (!Step(*set_, t, none, i, fd)) return std::nullopt;
        p = p.Child(i);
    }
    return p;
}

void Block::ForEachRef(const std::function<void(int32_t, bool, std::span<const uint16_t>)>& fn) const {
    if (!def_) return;
    auto leaf = [&](const TypeDef& t, const Value& v) {
        if (t.kind == Kind::Ref || t.kind == Kind::Ptr)
            fn(static_cast<int32_t>(static_cast<uint32_t>(v.bits)), t.kind == Kind::Ptr,
               set_->links.subspan(t.linkFirst, t.linkCount));
    };
    WalkLeaves(*set_, set_->types[def_->type], value_, leaf);
}

void Block::ForEachStringRef(const std::function<void(uint32_t)>& fn) const {
    if (!def_) return;
    auto leaf = [&](const TypeDef& t, const Value& v) {
        if (t.kind == Kind::StringRef) fn(static_cast<uint32_t>(v.bits));
    };
    WalkLeaves(*set_, set_->types[def_->type], value_, leaf);
}

}  // namespace niffer
