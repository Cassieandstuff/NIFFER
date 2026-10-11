// ── Decompile — NifFile → faithful-mode YAML ─────────────────────────────────
// Walks each block's value tree against its schema type and writes it as YAML
// (docs/yaml-format.md). Generic over the schema: nothing here knows a block
// class by name. Small values (a few scalars) are written in flow style on one
// line; larger ones in block style. Refs and string refs carry a comment naming
// what they point at; comments are ignored by Compile.
#include "Text.h"

#include <niffer-yaml/NifferYaml.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <optional>

namespace niffer::yaml {

namespace {

using schema::Kind;
using schema::SchemaSet;
using schema::TypeDef;

constexpr int    kFlowBudget = 12;    // leaves a field's value may have and still go on one line
constexpr int    kRowBudget  = 32;    // …an array element's (one vertex, key or triangle per line)
constexpr size_t kFlowBytes  = 32;    // longest opaque bytes written inline
constexpr size_t kWrap       = 100;   // flow sequences and hex wrap near this column

bool IsScalar(Kind k) {
    return k != Kind::Struct && k != Kind::Array && k != Kind::Union && k != Kind::Bytes &&
           k != Kind::SizedString;
}
bool IsLink(Kind k) { return k == Kind::Ref || k == Kind::Ptr || k == Kind::StringRef; }

size_t ScalarBytes(Kind k) {
    switch (k) {
    case Kind::U8: case Kind::I8: return 1;
    case Kind::U16: case Kind::I16: case Kind::Half: return 2;
    case Kind::U64: case Kind::I64: return 8;
    default: return 4;
    }
}

bool HasPacked(const Value& v) {
    if (v.packed) return true;
    for (const Value& item : v.items)
        if (HasPacked(item)) return true;
    return false;
}

class Writer {
public:
    explicit Writer(const NifFile& nif) : nif_(nif) {}

    std::string Run() {
        out_ += "# NIFFER YAML, faithful mode (docs/yaml-format.md). Compile with niffer-yaml.\n";
        const SchemaSet& schema = nif_.blocks.empty() || !nif_.blocks[0].Schema() ? schema::Main()
                                                                                  : *nif_.blocks[0].Schema();
        out_ += "niffer: {format: faithful, version: " + std::to_string(kFormatVersion) + ", schema: ";
        out_ += schema.schemaVersion;
        out_ += "}\n";
        Header();
        out_ += nif_.blocks.empty() ? "blocks: []\n" : "blocks:\n";
        for (size_t i = 0; i < nif_.blocks.size(); ++i) BlockEntry(i);
        out_ += "roots: [";
        for (size_t i = 0; i < nif_.roots.size(); ++i) {
            if (i) out_ += ", ";
            out_ += std::to_string(nif_.roots[i]);
        }
        out_ += "]\n";
        if (!nif_.trailing.empty()) {
            out_ += "trailing: ";
            Hex(nif_.trailing.data(), nif_.trailing.size(), 2);
            out_ += '\n';
        }
        return std::move(out_);
    }

private:
    // ── header ───────────────────────────────────────────────────────────────
    void Header() {
        const niffer::Header& h = nif_.header;
        char buf[32];
        out_ += "header:\n  versionLine: ";
        text::AppendString(out_, h.versionLine);
        std::snprintf(buf, sizeof buf, "0x%08X", h.version);
        out_ += "\n  version: ";
        out_ += buf;
        out_ += "\n  endian: " + std::to_string(h.endian);
        out_ += "\n  userVersion: " + std::to_string(h.userVersion);
        out_ += "\n  bsVersion: " + std::to_string(h.bsVersion);
        out_ += "\n  author: ";
        text::AppendString(out_, h.author);
        out_ += "\n  processScript: ";
        text::AppendString(out_, h.processScript);
        out_ += "\n  exportScript: ";
        text::AppendString(out_, h.exportScript);
        out_ += "\n  blockTypes:";
        StringList(h.blockTypes);
        out_ += "  maxStringLength: " + std::to_string(h.maxStringLength) + "\n";
        out_ += "  strings:";
        StringList(h.strings);
        out_ += "  groups: [";
        for (size_t i = 0; i < h.groups.size(); ++i) {
            if (i) out_ += ", ";
            out_ += std::to_string(h.groups[i]);
        }
        out_ += "]\n";
    }

    void StringList(const std::vector<std::string>& list) {
        if (list.empty()) {
            out_ += " []\n";
            return;
        }
        out_ += '\n';
        for (size_t i = 0; i < list.size(); ++i) {
            out_ += "    - ";
            text::AppendString(out_, list[i]);
            out_ += "  # " + std::to_string(i) + "\n";
        }
    }

    // ── blocks ───────────────────────────────────────────────────────────────
    void BlockEntry(size_t i) {
        const Block& b = nif_.blocks[i];
        out_ += "  - ";
        out_ += b.TypeName();
        out_ += ':';
        if (!b.Modeled()) {
            out_ += ' ';
            Hex(b.Original().data(), b.Original().size(), 6);
            out_ += "  # " + std::to_string(i) + ", raw: ";
            out_ += b.Refusal().empty() ? std::string("no descriptor") : b.Refusal();
            out_ += '\n';
            return;
        }
        const SchemaSet& s = *b.Schema();
        // Packed arrays hold encoded elements: write from a materialized copy.
        std::optional<Block> materialized;
        const Value* root = b.Root();
        if (HasPacked(*root)) {
            materialized = Block::Load(s, b.TypeName(), b.Encode(), LoadMode::Materialized);
            root = materialized->Root();
        }
        set_ = &s;
        const TypeDef& t = s.types[b.Def()->type];
        if (!AnyPresent(t, *root)) {
            out_ += " {}  # " + std::to_string(i) + "\n";
            return;
        }
        out_ += "  # " + std::to_string(i) + "\n";
        Fields(t, *root, 6, false);
    }

    bool AnyPresent(const TypeDef& t, const Value& v) const {
        for (uint16_t f = 0; f < t.refCount && f < v.items.size(); ++f)
            if (v.items[f].present) return true;
        return false;
    }

    // A struct's present fields as `key: value` lines at `indent`. With `dash`,
    // the first line starts with "- " two columns left of `indent` (a sequence
    // element that is a mapping).
    void Fields(const TypeDef& t, const Value& v, int indent, bool dash) {
        bool first = true;
        for (uint16_t f = 0; f < t.refCount && f < v.items.size(); ++f) {
            if (!v.items[f].present) continue;
            const auto& fd = set_->fields[t.ref + f];
            Prefix(indent, dash && first);
            first = false;
            Entry(fd.name, set_->types[fd.type], v.items[f], indent);
        }
    }

    void Prefix(int indent, bool dash) {
        if (dash) {
            out_.append(static_cast<size_t>(indent - 2), ' ');
            out_ += "- ";
        } else {
            out_.append(static_cast<size_t>(indent), ' ');
        }
    }

    // `key: value` (the prefix already written), the value in whichever style fits.
    void Entry(std::string_view key, const TypeDef& t, const Value& v, int indent) {
        out_ += key;
        out_ += ':';
        if (IsScalar(t.kind)) {
            out_ += ' ';
            Scalar(t, v);
            Comment(t, v);
            out_ += '\n';
            return;
        }
        if (t.kind == Kind::Bytes || t.kind == Kind::SizedString) {
            out_ += ' ';
            if (t.kind == Kind::Bytes) Hex(v.bytes.data(), v.bytes.size(), indent + 2);
            else text::AppendString(out_, v.str);
            out_ += '\n';
            return;
        }
        if (t.kind == Kind::Array && IsScalar(set_->types[t.ref].kind)) {
            ScalarArray(set_->types[t.ref], v, indent);
            return;
        }
        int budget = kFlowBudget;
        if (Small(t, v, budget)) {
            out_ += ' ';
            Flow(t, v);
            out_ += '\n';
            return;
        }
        out_ += '\n';
        Nested(t, v, indent + 2);
    }

    // A struct, array or union value in block style, starting on a fresh line.
    void Nested(const TypeDef& t, const Value& v, int indent) {
        switch (t.kind) {
        case Kind::Struct:
            Fields(t, v, indent, false);
            return;
        case Kind::Union:
            Prefix(indent, false);
            CaseEntry(t, v, indent);
            return;
        case Kind::Array:
            for (const Value& item : v.items) Element(set_->types[t.ref], item, indent + 2);
            return;
        default:
            return;
        }
    }

    void CaseEntry(const TypeDef& t, const Value& v, int indent) {
        const auto& cd = set_->cases[t.ref + v.bits];
        if (v.items.empty()) {
            out_ += cd.name;
            out_ += ": ~\n";
            return;
        }
        Entry(cd.name, set_->types[cd.type], v.items[0], indent);
    }

    // One sequence element at `indent` (its "- " sits two columns left).
    void Element(const TypeDef& t, const Value& v, int indent) {
        int budget = kRowBudget;
        if (IsScalar(t.kind) || t.kind == Kind::SizedString || Small(t, v, budget)) {
            Prefix(indent, true);
            Flow(t, v);
            Comment(t, v);
            out_ += '\n';
            return;
        }
        switch (t.kind) {
        case Kind::Struct:
            if (AnyPresent(t, v)) {
                Fields(t, v, indent, true);
            } else {
                Prefix(indent, true);
                out_ += "{}\n";
            }
            return;
        case Kind::Union:
            Prefix(indent, true);
            CaseEntry(t, v, indent);
            return;
        case Kind::Bytes:
            Prefix(indent, true);
            Hex(v.bytes.data(), v.bytes.size(), indent);
            out_ += '\n';
            return;
        default:   // an array of arrays
            Prefix(indent, true);
            out_ += '\n';
            Nested(t, v, indent);
            return;
        }
    }

    // An array of scalars: refs and string refs one per line (each with its
    // comment), everything else as a wrapped flow sequence.
    void ScalarArray(const TypeDef& et, const Value& v, int indent) {
        if (v.items.empty()) {
            out_ += " []\n";
            return;
        }
        if (IsLink(et.kind)) {
            out_ += '\n';
            for (const Value& item : v.items) Element(et, item, indent + 4);
            return;
        }
        out_ += " [";
        size_t lineStart = out_.rfind('\n') + 1;
        for (size_t i = 0; i < v.items.size(); ++i) {
            if (i) {
                out_ += ',';
                if (out_.size() - lineStart > kWrap) {
                    out_ += '\n';
                    lineStart = out_.size();
                    out_.append(static_cast<size_t>(indent + 2), ' ');
                } else {
                    out_ += ' ';
                }
            }
            Scalar(et, v.items[i]);
        }
        out_ += "]\n";
    }

    // Whether `v` fits on one line: at most `budget` leaves, short bytes, and
    // no arrays of refs or strings (those are written one per line).
    bool Small(const TypeDef& t, const Value& v, int& budget) const {
        if (!v.present) return true;
        switch (t.kind) {
        case Kind::Struct:
            for (uint16_t f = 0; f < t.refCount && f < v.items.size(); ++f)
                if (!Small(set_->types[set_->fields[t.ref + f].type], v.items[f], budget)) return false;
            return budget >= 0;
        case Kind::Array: {
            const TypeDef& et = set_->types[t.ref];
            // Refs (with their comments) and strings go one per line.
            if ((IsLink(et.kind) || et.kind == Kind::SizedString) && !v.items.empty()) return false;
            for (const Value& item : v.items)
                if (!Small(et, item, budget)) return false;
            return budget >= 0;
        }
        case Kind::Union:
            if (v.items.empty()) return --budget >= 0;
            return Small(set_->types[set_->cases[t.ref + v.bits].type], v.items[0], budget);
        case Kind::Bytes:
            budget -= 1 + static_cast<int>(v.bytes.size() / 8);
            return v.bytes.size() <= kFlowBytes && budget >= 0;
        case Kind::SizedString:
            budget -= 1 + static_cast<int>(v.str.size() / 16);
            return budget >= 0;
        default:
            return --budget >= 0;
        }
    }

    // `v` in flow style.
    void Flow(const TypeDef& t, const Value& v) {
        switch (t.kind) {
        case Kind::Struct: {
            out_ += '{';
            bool first = true;
            for (uint16_t f = 0; f < t.refCount && f < v.items.size(); ++f) {
                if (!v.items[f].present) continue;
                if (!first) out_ += ", ";
                first = false;
                const auto& fd = set_->fields[t.ref + f];
                out_ += fd.name;
                out_ += ": ";
                Flow(set_->types[fd.type], v.items[f]);
            }
            out_ += '}';
            return;
        }
        case Kind::Array:
            out_ += '[';
            for (size_t i = 0; i < v.items.size(); ++i) {
                if (i) out_ += ", ";
                Flow(set_->types[t.ref], v.items[i]);
            }
            out_ += ']';
            return;
        case Kind::Union: {
            const auto& cd = set_->cases[t.ref + v.bits];
            out_ += '{';
            out_ += cd.name;
            out_ += ": ";
            if (v.items.empty()) out_ += '~';
            else Flow(set_->types[cd.type], v.items[0]);
            out_ += '}';
            return;
        }
        case Kind::Bytes:
            Hex(v.bytes.data(), v.bytes.size(), -1);
            return;
        case Kind::SizedString:
            text::AppendString(out_, v.str);
            return;
        default:
            Scalar(t, v);
            return;
        }
    }

    void Scalar(const TypeDef& t, const Value& v) {
        char buf[32];
        const size_t n = ScalarBytes(t.kind);
        const uint64_t mask = n == 8 ? ~0ull : ((1ull << (8 * n)) - 1);
        const uint64_t bits = v.bits & mask;
        switch (t.kind) {
        case Kind::F32: {
            const float f = std::bit_cast<float>(static_cast<uint32_t>(bits));
            if (std::isnan(f)) {
                std::snprintf(buf, sizeof buf, "!bits 0x%08X", static_cast<uint32_t>(bits));
                out_ += buf;
            } else {
                text::AppendFloat(out_, f);
            }
            return;
        }
        case Kind::Half: {
            const float f = Half{ static_cast<uint16_t>(bits) }.ToFloat();
            if (std::isnan(f)) {
                std::snprintf(buf, sizeof buf, "!bits 0x%04X", static_cast<unsigned>(bits));
                out_ += buf;
            } else {
                text::AppendFloat(out_, f);
            }
            return;
        }
        case Kind::I8: case Kind::I16: case Kind::I32: case Kind::I64:
        case Kind::Ref: case Kind::Ptr: case Kind::StringRef: {
            // Sign-extend: refs are i32 (-1 = null), and an empty string ref
            // (0xFFFFFFFF) reads as -1 too.
            const int shift = 64 - static_cast<int>(8 * n);
            const auto x = static_cast<int64_t>(bits << shift) >> shift;
            out_ += std::to_string(x);
            return;
        }
        default:
            out_ += std::to_string(bits);
            return;
        }
    }

    // `  # <target class>` for a ref, `  # "<string>"` for a string ref.
    void Comment(const TypeDef& t, const Value& v) {
        if (t.kind == Kind::Ref || t.kind == Kind::Ptr) {
            const Block* target = nif_.GetBlock(static_cast<int32_t>(static_cast<uint32_t>(v.bits)));
            if (target) {
                out_ += "  # ";
                out_ += target->TypeName();
            }
        } else if (t.kind == Kind::StringRef) {
            const auto idx = static_cast<uint32_t>(v.bits);
            if (idx < nif_.header.strings.size()) {
                std::string_view s = nif_.header.strings[idx];
                std::string shown;
                text::AppendString(shown, s.substr(0, 60));
                out_ += "  # " + shown;
                if (s.size() > 60) out_ += "...";
            }
        }
    }

    // Bytes as a quoted hex string. With `indent` >= 0, long runs wrap with
    // escaped line breaks, continuing at `indent`.
    void Hex(const uint8_t* p, size_t n, int indent) {
        const size_t perLine = kWrap / 2;
        out_ += '"';
        for (size_t i = 0; i < n; i += perLine) {
            if (i && indent >= 0) {
                out_ += "\\\n";
                out_.append(static_cast<size_t>(indent), ' ');
            }
            text::AppendHex(out_, p + i, std::min(perLine, n - i));
        }
        out_ += '"';
    }

    const NifFile&   nif_;
    const SchemaSet* set_ = nullptr;
    std::string      out_;
};

}  // namespace

std::string Decompile(const NifFile& nif) { return Writer(nif).Run(); }

}  // namespace niffer::yaml
