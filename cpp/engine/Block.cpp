// ── niffer::Block — the generic block engine ─────────────────────────────────
// Interprets a SchemaSet's tables to decode a block's bytes into a Value tree
// and encode it back. One engine for every block class: nothing in this file
// knows any block by name. Semantics: docs/schema-language.md §7.
#include <niffer/Niffer.h>

#include <charconv>
#include <cstring>

namespace niffer {

using schema::CondOp;
using schema::FieldDef;
using schema::Kind;
using schema::LenKind;
using schema::SchemaSet;
using schema::TypeDef;

namespace {

size_t ScalarSize(Kind k) {
    switch (k) {
    case Kind::U8:  case Kind::I8:  return 1;
    case Kind::U16: case Kind::I16: return 2;
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

// A struct being decoded or encoded: field references (lengths, conditions)
// resolve against this stack, innermost last.
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

    // nullopt when the condition's field can't be read (treated as absent).
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

private:
    const SchemaSet&   set_;
    std::vector<Frame> frames_;
};

// ── Decoder ───────────────────────────────────────────────────────────────────
class Decoder {
public:
    Decoder(const SchemaSet& s, const uint8_t* beg, const uint8_t* end)
        : set_(s), scope_(s), beg_(beg), cur_(beg), end_(end) {}

    size_t Remaining() const { return static_cast<size_t>(end_ - cur_); }
    size_t Offset() const { return static_cast<size_t>(cur_ - beg_); }
    const std::string& Error() const { return err_; }

    // `fieldLen` is the length from a LenKind::Field source (-1 when none).
    bool Decode(const TypeDef& t, Value& v, int64_t fieldLen) {
        switch (t.kind) {
        case Kind::U8: case Kind::U16: case Kind::U32: case Kind::U64:
        case Kind::I8: case Kind::I16: case Kind::I32: case Kind::I64:
        case Kind::F32: case Kind::Ref: case Kind::Ptr: case Kind::StringRef: {
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
                const FieldDef& fd = set_.fields[t.ref + i];
                v.items.emplace_back();
                Value& fv = v.items.back();
                if (!scope_.CondHolds(fd)) { fv.present = false; continue; }
                const int64_t len = fd.lenField != schema::kNoField ? scope_.Length(fd) : -1;
                if (!Decode(set_.types[fd.type], fv, len)) { scope_.Pop(); return false; }
            }
            scope_.Pop();
            return true;
        }
        case Kind::Array: {
            const TypeDef& et = set_.types[t.ref];
            if (t.len == LenKind::Rest) {
                while (Remaining() > 0) {
                    const uint8_t* before = cur_;
                    v.items.emplace_back();
                    if (!Decode(et, v.items.back(), -1)) return false;
                    if (cur_ == before) return Fail("array element consumed no bytes");
                }
                return true;
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
                if (!Decode(et, item, -1)) return false;
            return true;
        }
        }
        return Fail("unknown kind");
    }

private:
    bool Fail(const char* what) {
        if (err_.empty()) err_ = what;
        return false;
    }

    const SchemaSet& set_;
    Scope            scope_;
    const uint8_t*   beg_;
    const uint8_t*   cur_;
    const uint8_t*   end_;
    std::string      err_;
};

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
    }
    return v;
}

class Encoder {
public:
    Encoder(const SchemaSet& s, std::vector<uint8_t>& out) : set_(s), scope_(s), out_(out) {}

    void Encode(const TypeDef& t, const Value& v) {
        switch (t.kind) {
        case Kind::U8: case Kind::U16: case Kind::U32: case Kind::U64:
        case Kind::I8: case Kind::I16: case Kind::I32: case Kind::I64:
        case Kind::F32: case Kind::Ref: case Kind::Ptr: case Kind::StringRef:
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
                const FieldDef& fd = set_.fields[t.ref + i];
                if (!scope_.CondHolds(fd)) continue;
                const TypeDef& ft = set_.types[fd.type];
                if (i < v.items.size() && v.items[i].present) Encode(ft, v.items[i]);
                else Encode(ft, ZeroValue(set_, ft));
            }
            scope_.Pop();
            return;
        case Kind::Array: {
            const TypeDef& et = set_.types[t.ref];
            switch (t.len) {
            case LenKind::Prefix8:  Put(v.items.size(), 1); break;
            case LenKind::Prefix16: Put(v.items.size(), 2); break;
            case LenKind::Prefix32: Put(v.items.size(), 4); break;
            default: break;
            }
            if (t.len == LenKind::Fixed) {
                for (uint32_t i = 0; i < t.count; ++i)
                    Encode(et, i < v.items.size() ? v.items[i] : ZeroValue(set_, et));
            } else {
                for (const Value& item : v.items) Encode(et, item);
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
template <class Fn>
void WalkRefs(const SchemaSet& s, const TypeDef& t, const Value& v, Fn& fn) {
    if (!v.present) return;
    switch (t.kind) {
    case Kind::Ref:
    case Kind::Ptr:
        fn(static_cast<int32_t>(static_cast<uint32_t>(v.bits)), t.kind == Kind::Ptr,
           s.links.subspan(t.linkFirst, t.linkCount));
        return;
    case Kind::Struct:
        for (uint16_t i = 0; i < t.refCount && i < v.items.size(); ++i)
            WalkRefs(s, s.types[s.fields[t.ref + i].type], v.items[i], fn);
        return;
    case Kind::Array:
        for (const Value& item : v.items) WalkRefs(s, s.types[t.ref], item, fn);
        return;
    default:
        return;
    }
}

template <class Fn>
void WalkStringRefs(const SchemaSet& s, const TypeDef& t, const Value& v, Fn& fn) {
    if (!v.present) return;
    switch (t.kind) {
    case Kind::StringRef:
        fn(static_cast<uint32_t>(v.bits));
        return;
    case Kind::Struct:
        for (uint16_t i = 0; i < t.refCount && i < v.items.size(); ++i)
            WalkStringRefs(s, s.types[s.fields[t.ref + i].type], v.items[i], fn);
        return;
    case Kind::Array:
        for (const Value& item : v.items) WalkStringRefs(s, s.types[t.ref], item, fn);
        return;
    default:
        return;
    }
}

}  // namespace

// ── Block ─────────────────────────────────────────────────────────────────────
Block Block::Load(const SchemaSet& set, std::string_view typeName,
                  std::span<const uint8_t> bytes) {
    Block b;
    b.set_ = &set;
    b.typeName_.assign(typeName);
    b.original_.assign(bytes.begin(), bytes.end());

    const schema::BlockDef* def = set.Find(typeName);
    if (!def || def->abstract) return b;   // raw: no descriptor

    const uint8_t* beg = b.original_.data();
    Decoder dec(set, beg, beg + b.original_.size());
    Value v;
    if (!dec.Decode(set.types[def->type], v, -1)) {
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
    enc.Encode(set_->types[def_->type], value_);
}

std::vector<uint8_t> Block::Encode(bool force) const {
    std::vector<uint8_t> out;
    out.reserve(original_.size());
    Encode(out, force);
    return out;
}

const TypeDef* Block::TypeAt(const RelPath& p) const {
    if (!def_) return nullptr;
    const TypeDef* t = &set_->types[def_->type];
    for (uint8_t d = 0; d < p.depth; ++d) {
        const uint16_t i = p.idx[d];
        if (t->kind == Kind::Struct) {
            if (i >= t->refCount) return nullptr;
            t = &set_->types[set_->fields[t->ref + i].type];
        } else if (t->kind == Kind::Array) {
            t = &set_->types[t->ref];
        } else {
            return nullptr;
        }
    }
    return t;
}

const Value* Block::GetValue(const RelPath& p) const {
    if (!def_) return nullptr;
    const TypeDef* t = &set_->types[def_->type];
    const Value*   v = &value_;
    for (uint8_t d = 0; d < p.depth; ++d) {
        const uint16_t i = p.idx[d];
        if (t->kind == Kind::Struct) {
            if (i >= t->refCount || i >= v->items.size()) return nullptr;
            t = &set_->types[set_->fields[t->ref + i].type];
        } else if (t->kind == Kind::Array) {
            if (i >= v->items.size()) return nullptr;
            t = &set_->types[t->ref];
        } else {
            return nullptr;
        }
        v = &v->items[i];
        if (!v->present) return nullptr;
    }
    return v;
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
        const uint16_t i = p.idx[d];
        if (t->kind == Kind::Struct) {
            if (i >= t->refCount || i >= v->items.size()) return false;
            structs.push_back({t, v});
            fd = &set_->fields[t->ref + i];
            t  = &set_->types[fd->type];
        } else if (t->kind == Kind::Array) {
            if (i >= v->items.size()) return false;
            fd = nullptr;
            t  = &set_->types[t->ref];
        } else {
            return false;
        }
        v = &v->items[i];
    }
    if (*v == nv) return true;   // no-op: stays verbatim
    *v = std::move(nv);
    dirty_ = true;

    // A replaced array / byte run keeps its length field in step (§6).
    if (fd && fd->lenField != schema::kNoField && fd->lenUp < structs.size()) {
        StructStep& owner = structs[structs.size() - 1 - fd->lenUp];
        if (fd->lenField < owner.value->items.size()) {
            const Kind lk = set_->types[set_->fields[owner.type->ref + fd->lenField].type].kind;
            const uint64_t n = t->kind == Kind::Bytes ? v->bytes.size() : v->items.size();
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
        if (t->kind == Kind::Struct) {
            uint16_t i = 0;
            for (; i < t->refCount; ++i)
                if (seg == set_->fields[t->ref + i].name) break;
            if (i == t->refCount) return std::nullopt;
            p = p.Child(i);
            t = &set_->types[set_->fields[t->ref + i].type];
        } else if (t->kind == Kind::Array) {
            uint16_t i = 0;
            auto [ptr, ec] = std::from_chars(seg.data(), seg.data() + seg.size(), i);
            if (ec != std::errc{} || ptr != seg.data() + seg.size()) return std::nullopt;
            p = p.Child(i);
            t = &set_->types[t->ref];
        } else {
            return std::nullopt;
        }
    }
    return p;
}

void Block::ForEachRef(const std::function<void(int32_t, bool, std::span<const uint16_t>)>& fn) const {
    if (!def_) return;
    WalkRefs(*set_, set_->types[def_->type], value_, fn);
}

void Block::ForEachStringRef(const std::function<void(uint32_t)>& fn) const {
    if (!def_) return;
    WalkStringRefs(*set_, set_->types[def_->type], value_, fn);
}

}  // namespace niffer
