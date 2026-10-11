// ── Compile — YAML (faithful or authoring mode) → NifFile ────────────────────
// Parses the text with rapidyaml, then reads each block's mapping against its
// schema type into a value tree and hands it to Block::Build, which encodes it
// and accepts it only if decoding gives the same tree back. Authoring mode
// first resolves ids to block indices, interns strings into a fresh string
// table, and has the engine derive the length fields left out
// (Block::DeriveLengths). Generic over the schema: nothing here knows a block
// class by name. Every error carries the line of the YAML node it is about.
#include "Text.h"

#include <niffer-yaml/NifferYaml.h>

#ifndef _RYML_SINGLE_HEADER_AMALGAMATED_HPP_
#include <rapidyaml.hpp>
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace niffer::yaml {

namespace {

using schema::Kind;
using schema::SchemaSet;
using schema::TypeDef;
using ryml::id_type;

// A semantic error at a YAML node; turned into an Error with a line number.
struct Failure {
    id_type     node;
    std::string message;
};

class Reader {
public:
    Reader(const ryml::Tree& tree, const SchemaSet& set, LoadMode mode)
        : tree_(tree), set_(set), mode_(mode) {}

    NifFile Run() {
        const id_type root = tree_.root_id();
        if (!tree_.is_map(root)) throw Failure{ root, "expected a mapping at the top level" };
        Format(Child(root, "niffer"));
        NifFile f;
        if (authoring_) {
            AuthoringHeader(root, f.header);
            AuthoringBlocks(Child(root, "blocks"), f);
        } else {
            Header(Child(root, "header"), f.header);
            Blocks(Child(root, "blocks"), f);
        }
        for (id_type r = First(Seq(Child(root, "roots"))); r != ryml::NONE; r = tree_.next_sibling(r))
            f.roots.push_back(authoring_ ? Link(r) : static_cast<int32_t>(Signed(r, 32)));
        const id_type trailing = tree_.find_child(root, ryml::csubstr("trailing", 8));
        if (trailing != ryml::NONE) f.trailing = Hex(trailing);
        Known(root, { "niffer", "header", "blocks", "roots", "trailing" });
        return f;
    }

private:
    // ── structure ────────────────────────────────────────────────────────────
    id_type Child(id_type map, std::string_view key) const {
        const id_type c = tree_.find_child(map, ryml::csubstr(key.data(), key.size()));
        if (c == ryml::NONE) throw Failure{ map, "missing '" + std::string(key) + "'" };
        return c;
    }
    id_type Seq(id_type n) const {
        if (!tree_.is_seq(n)) throw Failure{ n, "expected a sequence" };
        return n;
    }
    id_type Map(id_type n) const {
        if (!tree_.is_map(n)) throw Failure{ n, "expected a mapping" };
        return n;
    }
    id_type First(id_type n) const { return tree_.first_child(n); }

    // Rejects keys of `map` that aren't in `allowed` (a typo would otherwise be
    // silently dropped).
    void Known(id_type map, std::initializer_list<std::string_view> allowed) const {
        for (id_type c = First(map); c != ryml::NONE; c = tree_.next_sibling(c)) {
            const std::string_view k = Key(c);
            bool ok = false;
            for (std::string_view a : allowed) ok = ok || a == k;
            if (!ok) throw Failure{ c, "unknown key '" + std::string(k) + "'" };
        }
    }

    std::string_view Key(id_type n) const {
        const ryml::csubstr k = tree_.key(n);
        return { k.str, k.len };
    }
    std::string_view Text(id_type n) const {
        if (!tree_.has_val(n) || tree_.is_map(n) || tree_.is_seq(n)) throw Failure{ n, "expected a scalar" };
        const ryml::csubstr v = tree_.val(n);
        return { v.str, v.len };
    }
    std::string_view Tag(id_type n) const {
        if (!tree_.has_val_tag(n)) return {};
        const ryml::csubstr t = tree_.val_tag(n);
        return { t.str, t.len };
    }

    // ── scalars ──────────────────────────────────────────────────────────────
    uint64_t Unsigned(id_type n, int bits) const {
        const auto v = text::ParseInt(Text(n));
        if (!v || (v->negative && v->magnitude != 0)) throw Failure{ n, "expected an unsigned integer" };
        if (bits < 64 && v->magnitude >> bits) throw Failure{ n, "out of range for u" + std::to_string(bits) };
        return v->magnitude;
    }
    int64_t Signed(id_type n, int bits) const {
        const auto v = text::ParseInt(Text(n));
        if (!v) throw Failure{ n, "expected an integer" };
        const uint64_t limit = 1ull << (bits - 1);   // |min|
        if (v->negative ? v->magnitude > limit : v->magnitude >= limit)
            throw Failure{ n, "out of range for i" + std::to_string(bits) };
        return v->negative ? static_cast<int64_t>(0 - v->magnitude) : static_cast<int64_t>(v->magnitude);
    }
    // `!bits 0x…` (an exact bit pattern), else a decimal.
    std::optional<uint64_t> Bits(id_type n, int bits) const {
        if (Tag(n) != "!bits") return std::nullopt;
        return Unsigned(n, bits);
    }
    std::string String(id_type n) const {
        if (Tag(n) == "!hex") {
            const auto b = Hex(n);
            return { b.begin(), b.end() };
        }
        const std::string_view t = Text(n);
        return { t.begin(), t.end() };
    }
    std::vector<uint8_t> Hex(id_type n) const {
        auto b = text::ParseHex(Text(n));
        if (!b) throw Failure{ n, "expected hex bytes (two digits per byte)" };
        return std::move(*b);
    }

    // ── format + header ──────────────────────────────────────────────────────
    void Format(id_type n) {
        Map(n);
        const std::string_view format = Text(Child(n, "format"));
        if (format != "faithful" && format != "authoring")
            throw Failure{ Child(n, "format"), "format is 'faithful' or 'authoring'" };
        authoring_ = format == "authoring";
        if (Unsigned(Child(n, "version"), 32) != static_cast<uint64_t>(kFormatVersion))
            throw Failure{ Child(n, "version"), "unsupported format version (this build reads " +
                                                    std::to_string(kFormatVersion) + ")" };
    }

    void Header(id_type n, niffer::Header& h) const {
        Map(n);
        h.versionLine = String(Child(n, "versionLine"));
        h.version     = static_cast<uint32_t>(Unsigned(Child(n, "version"), 32));
        h.endian      = static_cast<uint8_t>(Unsigned(Child(n, "endian"), 8));
        h.userVersion = static_cast<uint32_t>(Unsigned(Child(n, "userVersion"), 32));
        h.bsVersion   = static_cast<uint32_t>(Unsigned(Child(n, "bsVersion"), 32));
        h.author        = String(Child(n, "author"));
        h.processScript = String(Child(n, "processScript"));
        h.exportScript  = String(Child(n, "exportScript"));
        for (id_type c = First(Seq(Child(n, "blockTypes"))); c != ryml::NONE; c = tree_.next_sibling(c))
            h.blockTypes.push_back(String(c));
        h.maxStringLength = static_cast<uint32_t>(Unsigned(Child(n, "maxStringLength"), 32));
        for (id_type c = First(Seq(Child(n, "strings"))); c != ryml::NONE; c = tree_.next_sibling(c))
            h.strings.push_back(String(c));
        for (id_type c = First(Seq(Child(n, "groups"))); c != ryml::NONE; c = tree_.next_sibling(c))
            h.groups.push_back(static_cast<uint32_t>(Unsigned(c, 32)));
        Known(n, { "versionLine", "version", "endian", "userVersion", "bsVersion", "author", "processScript",
                   "exportScript", "blockTypes", "maxStringLength", "strings", "groups" });
    }

    // ── authoring: header, blocks, ids, strings ──────────────────────────────
    // Only the free-form facts are written; the rest is SSE's or derived once
    // the blocks are read (FinishHeader).
    void AuthoringHeader(id_type root, niffer::Header& h) const {
        h.versionLine = "Gamebryo File Format, Version 20.2.0.7";
        h.version     = kNifVersion;
        h.endian      = 1;
        h.userVersion = kUserVersion;
        h.bsVersion   = kStreamVersion;
        h.author = h.processScript = h.exportScript = std::string("\0", 1);
        const id_type n = tree_.find_child(root, ryml::csubstr("header", 6));
        if (n == ryml::NONE) return;
        Map(n);
        auto shortString = [&](std::string_view key, std::string& out) {
            const id_type c = tree_.find_child(n, ryml::csubstr(key.data(), key.size()));
            if (c == ryml::NONE) return;
            if (Tag(c) == "!verbatim") {
                out = std::string(Text(c));
                return;
            }
            out = String(c);
            out += '\0';   // ShortStrings end with a NUL in every vanilla file
        };
        shortString("author", h.author);
        shortString("processScript", h.processScript);
        shortString("exportScript", h.exportScript);
        const id_type groups = tree_.find_child(n, ryml::csubstr("groups", 6));
        if (groups != ryml::NONE)
            for (id_type c = First(Seq(groups)); c != ryml::NONE; c = tree_.next_sibling(c))
                h.groups.push_back(static_cast<uint32_t>(Unsigned(c, 32)));
        Known(n, { "author", "processScript", "exportScript", "groups" });
    }

    // `blocks` is a mapping from each block's id to its class (the tag) and its
    // fields; file order is the order written.
    void AuthoringBlocks(id_type n, NifFile& f) {
        if (tree_.is_seq(n) && tree_.num_children(n) == 0) return;   // blocks: []
        Map(n);
        for (id_type e = First(n); e != ryml::NONE; e = tree_.next_sibling(e)) {
            const std::string id(Key(e));
            if (!ids_.emplace(id, static_cast<int32_t>(ids_.size())).second)
                throw Failure{ e, "id '" + id + "' is used twice" };
        }
        niffer::Header& h = f.header;
        for (id_type e = First(n); e != ryml::NONE; e = tree_.next_sibling(e)) {
            if (!tree_.has_val_tag(e)) throw Failure{ e, "a block is tagged with its class: id: !Class" };
            std::string type(Tag(e));
            type.erase(0, type.find_first_not_of('!'));

            auto ti = std::find(h.blockTypes.begin(), h.blockTypes.end(), type);
            if (ti == h.blockTypes.end()) ti = h.blockTypes.insert(h.blockTypes.end(), type);
            h.blockTypeIndex.push_back(static_cast<uint16_t>(ti - h.blockTypes.begin()));

            if (!tree_.is_map(e)) {   // raw: the block's bytes
                f.blocks.push_back(Block::Load(set_, type, Hex(e), mode_));
            } else {
                const schema::BlockDef* def = set_.Find(type);
                if (!def || def->abstract)
                    throw Failure{ e, "no descriptor for '" + type + "': write the block as raw hex" };
                Value root = Parse(set_.types[def->type], e);
                std::string why;
                if (!Block::DeriveLengths(set_, type, root, &why)) throw Failure{ e, type + ": " + why };
                auto b = Block::Build(set_, type, root, mode_, &why);
                if (!b) throw Failure{ e, type + ": " + why };
                f.blocks.push_back(std::move(*b));
            }
            h.blockSizes.push_back(static_cast<uint32_t>(f.blocks.back().Original().size()));
        }
        h.numBlocks = static_cast<uint32_t>(f.blocks.size());
        h.strings = strings_;
        h.maxStringLength = 0;
        for (const std::string& str : h.strings)
            h.maxStringLength = std::max(h.maxStringLength, static_cast<uint32_t>(str.size()));
    }

    // A ref: an id, or ~ for null.
    int32_t Link(id_type n) const {
        if (tree_.has_val(n) && !tree_.is_map(n) && !tree_.is_seq(n) && tree_.val_is_null(n)) return -1;
        const std::string id = String(n);
        const auto it = ids_.find(id);
        if (it == ids_.end()) throw Failure{ n, "no block has the id '" + id + "'" };
        return it->second;
    }

    // A string ref: the string (interned in first-use order), or ~ for none.
    uint32_t Intern(id_type n) const {
        if (tree_.has_val(n) && !tree_.is_map(n) && !tree_.is_seq(n) && tree_.val_is_null(n)) return 0xFFFFFFFFu;
        std::string str = String(n);
        const auto [it, added] = stringIndex_.emplace(str, static_cast<uint32_t>(strings_.size()));
        if (added) strings_.push_back(std::move(str));
        return it->second;
    }

    // ── blocks ───────────────────────────────────────────────────────────────
    void Blocks(id_type n, NifFile& f) const {
        Seq(n);
        niffer::Header& h = f.header;
        for (id_type e = First(n); e != ryml::NONE; e = tree_.next_sibling(e)) {
            Map(e);
            if (tree_.num_children(e) != 1)
                throw Failure{ e, "a block is a mapping with one key, its class" };
            const id_type body = First(e);
            const std::string type(Key(body));

            // The header's type index: the class's slot in blockTypes.
            size_t ti = 0;
            while (ti < h.blockTypes.size() && h.blockTypes[ti] != type) ++ti;
            if (ti == h.blockTypes.size())
                throw Failure{ body, "class '" + type + "' is not in header.blockTypes" };
            h.blockTypeIndex.push_back(static_cast<uint16_t>(ti));

            if (!tree_.is_map(body)) {   // raw: the block's bytes
                const auto bytes = Hex(body);
                f.blocks.push_back(Block::Load(set_, type, bytes, mode_));
            } else {
                const schema::BlockDef* def = set_.Find(type);
                if (!def || def->abstract)
                    throw Failure{ body, "no descriptor for '" + type + "': write the block as raw hex" };
                const Value root = Parse(set_.types[def->type], body);
                std::string why;
                auto b = Block::Build(set_, type, root, mode_, &why);
                if (!b) throw Failure{ body, type + ": " + why };
                f.blocks.push_back(std::move(*b));
            }
            h.blockSizes.push_back(static_cast<uint32_t>(f.blocks.back().Original().size()));
        }
        h.numBlocks = static_cast<uint32_t>(f.blocks.size());
    }

    // A value of type `t` from node `n`.
    Value Parse(const TypeDef& t, id_type n) const {
        Value v;
        switch (t.kind) {
        case Kind::U8:  v.bits = Unsigned(n, 8); break;
        case Kind::U16: v.bits = Unsigned(n, 16); break;
        case Kind::U32: v.bits = Unsigned(n, 32); break;
        case Kind::U64: v.bits = Unsigned(n, 64); break;
        case Kind::I8:  v.bits = static_cast<uint8_t>(Signed(n, 8)); break;
        case Kind::I16: v.bits = static_cast<uint16_t>(Signed(n, 16)); break;
        case Kind::Ref: case Kind::Ptr:
            if (authoring_) {
                v.bits = static_cast<uint32_t>(Link(n));
                break;
            }
            v.bits = static_cast<uint32_t>(Signed(n, 32));
            break;
        case Kind::I32:
            v.bits = static_cast<uint32_t>(Signed(n, 32));
            break;
        case Kind::I64: v.bits = static_cast<uint64_t>(Signed(n, 64)); break;
        case Kind::StringRef: {
            if (authoring_) {
                v.bits = Intern(n);
                break;
            }
            // -1 (empty) or an index; 4294967295 is accepted as -1 too.
            const auto x = text::ParseInt(Text(n));
            if (!x || (x->negative && x->magnitude != 1) || (!x->negative && x->magnitude > 0xFFFFFFFFull))
                throw Failure{ n, "expected a string-table index or -1" };
            v.bits = x->negative ? 0xFFFFFFFFu : x->magnitude;
            break;
        }
        case Kind::F32:
            if (auto b = Bits(n, 32)) {
                v.bits = *b;
            } else {
                const auto f = text::ParseFloat(Text(n));
                if (!f) throw Failure{ n, "expected a number (a NaN is written !bits 0x...)" };
                v.bits = std::bit_cast<uint32_t>(*f);
            }
            break;
        case Kind::Half:
            if (auto b = Bits(n, 16)) {
                v.bits = *b;
            } else {
                const auto f = text::ParseFloat(Text(n));
                if (!f) throw Failure{ n, "expected a number (a NaN is written !bits 0x...)" };
                const Half h = Half::FromFloat(*f);
                if (std::bit_cast<uint32_t>(h.ToFloat()) != std::bit_cast<uint32_t>(*f))
                    throw Failure{ n, "not exactly representable as a half" };
                v.bits = h.bits;
            }
            break;
        case Kind::SizedString:
            v.str = String(n);
            break;
        case Kind::Bytes:
            v.bytes = Hex(n);
            break;
        case Kind::Struct: {
            Map(n);
            v.items.resize(t.refCount);
            size_t matched = 0;
            for (uint16_t i = 0; i < t.refCount; ++i) {
                const auto& fd = set_.fields[t.ref + i];
                const id_type c = tree_.find_child(n, ryml::csubstr(fd.name, std::strlen(fd.name)));
                if (c == ryml::NONE) {
                    v.items[i].present = false;
                    continue;
                }
                ++matched;
                v.items[i] = Parse(set_.types[fd.type], c);
            }
            if (matched != tree_.num_children(n)) {
                for (id_type c = First(n); c != ryml::NONE; c = tree_.next_sibling(c)) {
                    bool known = false;
                    for (uint16_t i = 0; i < t.refCount && !known; ++i)
                        known = Key(c) == set_.fields[t.ref + i].name;
                    if (!known) throw Failure{ c, "no field '" + std::string(Key(c)) + "' here" };
                }
                throw Failure{ n, "a field is given twice" };
            }
            break;
        }
        case Kind::Array: {
            Seq(n);
            const TypeDef& et = set_.types[t.ref];
            v.items.reserve(tree_.num_children(n));
            for (id_type c = First(n); c != ryml::NONE; c = tree_.next_sibling(c))
                v.items.push_back(Parse(et, c));
            break;
        }
        case Kind::Union: {
            Map(n);
            if (tree_.num_children(n) != 1) throw Failure{ n, "a union is a mapping with one key, its case" };
            const id_type c = First(n);
            const std::string_view name = Key(c);
            uint16_t ci = 0;
            while (ci < t.refCount && name != set_.cases[t.ref + ci].name) ++ci;
            if (ci == t.refCount) throw Failure{ c, "no case '" + std::string(name) + "' here" };
            v.bits = ci;
            const auto& cd = set_.cases[t.ref + ci];
            if (cd.type == schema::kNoField) {
                if (!tree_.has_val(c) || tree_.is_map(c) || tree_.is_seq(c) || !tree_.val_is_null(c))
                    throw Failure{ c, "case '" + std::string(name) + "' holds no value (~)" };
            } else {
                v.items.push_back(Parse(set_.types[cd.type], c));
            }
            break;
        }
        }
        return v;
    }

    const ryml::Tree& tree_;
    const SchemaSet&  set_;
    LoadMode          mode_;
    bool              authoring_ = false;
    std::unordered_map<std::string, int32_t>          ids_;          // authoring: id -> block index
    mutable std::vector<std::string>                  strings_;      // authoring: the new string table
    mutable std::unordered_map<std::string, uint32_t> stringIndex_;
};

Error At(const ryml::Tree& tree, const ryml::Parser& parser, id_type node, std::string message) {
    Error e;
    e.message = std::move(message);
    const ryml::Location loc = tree.location(parser, node);
    if (loc.line != ryml::npos) e.line = loc.line + 1;
    if (loc.col != ryml::npos) e.column = loc.col + 1;
    return e;
}

// rapidyaml's errors, raised by the callbacks below. rapidyaml's own default
// handlers print to stderr before throwing; a library shouldn't, so each parse
// installs these instead (on its own handler: no global state is touched).
struct RymlError {
    Error error;
};
Error FromLocation(ryml::csubstr msg, const ryml::Location& loc) {
    Error e;
    e.message.assign(msg.str, msg.len);
    if (loc.line != ryml::npos) e.line = loc.line + 1;
    if (loc.col != ryml::npos) e.column = loc.col + 1;
    return e;
}
[[noreturn]] void OnBasic(ryml::csubstr msg, const ryml::ErrorDataBasic&, void*) {
    throw RymlError{ FromLocation(msg, ryml::Location{}) };
}
[[noreturn]] void OnParse(ryml::csubstr msg, const ryml::ErrorDataParse& e, void*) {
    throw RymlError{ FromLocation(msg, e.ymlloc) };
}
[[noreturn]] void OnVisit(ryml::csubstr msg, const ryml::ErrorDataVisit&, void*) {
    throw RymlError{ FromLocation(msg, ryml::Location{}) };
}

}  // namespace

std::expected<NifFile, Error> Compile(std::string_view text, const SchemaSet& set, LoadMode mode) {
    ryml::Callbacks callbacks;
    callbacks.set_error_basic(&OnBasic).set_error_parse(&OnParse).set_error_visit(&OnVisit);
    ryml::EventHandlerTree handler(callbacks);
    ryml::Parser parser(&handler, ryml::ParserOptions().locations(true));
    try {
        ryml::Tree tree = ryml::parse_in_arena(&parser, ryml::csubstr(text.data(), text.size()));
        tree.resolve();   // anchors and aliases
        try {
            return Reader(tree, set, mode).Run();
        } catch (const Failure& f) {
            return std::unexpected(At(tree, parser, f.node, f.message));
        }
    } catch (const RymlError& e) {
        return std::unexpected(e.error);
    }
}

}  // namespace niffer::yaml
