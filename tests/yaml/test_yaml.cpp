// niffer-yaml tests: decompile → compile is byte-exact on a hand-built file
// that carries the awkward cases (a NaN payload, a non-UTF-8 string, NULs, a
// raw block, a packed vertex array), edits land, and bad input is refused with
// a line number.
#include "test_harness.h"

#include "Framing.h"

#include <niffer-yaml/NifferYaml.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace niffer;

namespace {

struct Bytes {
    std::vector<uint8_t> b;
    template <class T> Bytes& put(T v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof(T));
        return *this;
    }
};

constexpr uint32_t kNanBits = 0x7FC01234u;   // a NaN with a payload

// NiObjectNET + NiAVObject (identity rotation, translation (1, 2, 3)).
void AvObject(Bytes& x, uint32_t name) {
    x.put<uint32_t>(name).put<uint32_t>(0).put<int32_t>(-1);    // name, extraData[], controller
    x.put<uint32_t>(14);                                          // flags
    x.put<float>(1.f).put<float>(2.f).put<float>(3.f);            // translation
    for (int i = 0; i < 9; ++i) x.put<float>(i % 4 == 0 ? 1.f : 0.f);
    x.put<float>(1.f).put<int32_t>(-1);                           // scale, collisionObject
}

std::vector<uint8_t> Node(const std::vector<int32_t>& children) {
    Bytes x;
    AvObject(x, 0);
    x.put<uint32_t>(static_cast<uint32_t>(children.size()));
    for (int32_t c : children) x.put<int32_t>(c);
    x.put<uint32_t>(0);                                           // effects
    return x.b;
}

std::vector<uint8_t> Shape() {
    const uint64_t desc = 6ull | (4ull << 8) | (5ull << 16) | (0x00Bull << 44);   // 24-byte vertex
    Bytes x;
    AvObject(x, 1);
    x.put<float>(0.f).put<float>(0.f).put<float>(0.f).put<uint32_t>(kNanBits);   // bound: NaN radius
    x.put<int32_t>(-1).put<int32_t>(-1).put<int32_t>(-1);                         // skin, shader, alpha
    x.put<uint64_t>(desc).put<uint16_t>(1).put<uint16_t>(3).put<uint32_t>(3 * 24 + 6);
    for (int v = 0; v < 3; ++v) {
        x.put<float>(float(v)).put<float>(-0.f).put<float>(1e-30f).put<float>(0.f);
        x.put<uint16_t>(Half::FromFloat(0.25f * v).bits).put<uint16_t>(0x7E01);   // uv: v is a half NaN
        x.put<uint8_t>(255).put<uint8_t>(127).put<uint8_t>(0).put<uint8_t>(0);
    }
    x.put<uint16_t>(0).put<uint16_t>(1).put<uint16_t>(2);
    x.put<uint32_t>(0);                                                           // particleDataSize
    return x.b;
}

std::vector<uint8_t> MakeFile() {
    Header h;
    h.versionLine = "Gamebryo File Format, Version 20.2.0.7";
    h.author = std::string("me\0", 3);
    h.processScript = std::string("\0", 1);
    h.exportScript = "tab\there \"quoted\" back\\slash";
    h.blockTypes = { "BSFadeNode", "BSXFlags", "ZZZ_Unknown", "BSTriShape" };
    h.blockTypeIndex = { 0, 1, 2, 3 };
    h.numBlocks = 4;
    h.strings = { "Scene Root", std::string("caf\xE9", 4), "\xC3\xA9t\xC3\xA9" };   // Latin-1, then UTF-8
    h.maxStringLength = 10;
    Bytes bsx;
    bsx.put<uint32_t>(0xFFFFFFFFu).put<uint32_t>(0x82);
    return detail::WriteFraming(h, { Node({ 3 }), bsx.b, { 0xDE, 0xAD, 0xBE, 0xEF }, Shape() }, { 0 },
                                { 0x01, 0x02 });
}

std::string Replace(std::string s, const std::string& from, const std::string& to) {
    const size_t at = s.find(from);
    if (at != std::string::npos) s.replace(at, from.size(), to);
    return s;
}

}  // namespace

void run_yaml_tests() {
    std::printf(" yaml\n");
    const auto bytes = MakeFile();
    auto nif = NifFile::Load(bytes);
    CHECK(nif.has_value());
    if (!nif) return;
    CHECK_EQ(nif->RawCount(), 1u);   // only ZZZ_Unknown

    const std::string text = *yaml::Decompile(*nif);
    CHECK(text.find("!bits 0x7FC01234") != std::string::npos);   // f32 NaN payload
    CHECK(text.find("!bits 0x7E01") != std::string::npos);       // half NaN
    CHECK(text.find("!hex \"636166e9\"") != std::string::npos);  // not UTF-8
    CHECK(text.find("\"\xC3\xA9t\xC3\xA9\"") != std::string::npos);   // UTF-8 written as is
    CHECK(text.find("\"me\\0\"") != std::string::npos);
    CHECK(text.find("deadbeef") != std::string::npos);            // the raw block
    CHECK(text.find("# \"Scene Root\"") != std::string::npos);    // string-ref comment

    // Byte-exact both ways, in both load modes.
    auto back = yaml::Compile(text);
    CHECK(back.has_value());
    if (!back) {
        std::printf("  compile: line %zu: %s\n", back.error().line, back.error().message.c_str());
        return;
    }
    CHECK(back->Save() == bytes);
    CHECK_EQ(back->RawCount(), 1u);
    CHECK(*yaml::Decompile(*back) == text);
    auto mat = yaml::Compile(text, schema::Main(), LoadMode::Materialized);
    CHECK(mat.has_value() && mat->Save() == bytes);

    // An edit lands, and only where it was made.
    {
        auto edited = yaml::Compile(Replace(text, "integerData: 130", "integerData: 7"));
        CHECK(edited.has_value());
        if (edited) {
            auto bsx = view::BSXFlags::From(edited->blocks[1]);
            CHECK(bsx.has_value() && bsx->IntegerData() == 7u);
            const auto saved = edited->Save();
            CHECK_EQ(saved.size(), bytes.size());
            size_t diffs = 0;
            for (size_t i = 0; i < saved.size() && i < bytes.size(); ++i) diffs += saved[i] != bytes[i];
            CHECK_EQ(diffs, 1u);
        }
    }

    // Refusals carry the line of the offending node.
    auto lineOf = [&](const std::string& t, const std::string& needle) {
        size_t line = 1;
        const size_t at = t.find(needle);
        for (size_t i = 0; i < at && i < t.size(); ++i) line += t[i] == '\n';
        return line;
    };
    {
        const std::string bad = Replace(text, "integerData: 130", "integerDatum: 130");
        auto r = yaml::Compile(bad);
        CHECK(!r.has_value());
        if (!r) {
            CHECK(r.error().message.find("integerDatum") != std::string::npos);
            CHECK_EQ(r.error().line, lineOf(bad, "integerDatum"));
        }
    }
    {
        auto r = yaml::Compile(Replace(text, "integerData: 130", "integerData: 4294967296"));
        CHECK(!r.has_value() && r.error().message.find("out of range") != std::string::npos);
    }
    {
        // numVertices no longer matches the vertex array.
        auto r = yaml::Compile(Replace(text, "numVertices: 3", "numVertices: 2"));
        CHECK(!r.has_value() && r.error().message.find("BSTriShape") != std::string::npos);
    }
    {
        auto r = yaml::Compile(Replace(text, "ZZZ_Unknown:", "ZZZ_Missing:"));
        CHECK(!r.has_value() && r.error().message.find("blockTypes") != std::string::npos);
    }
    {
        auto r = yaml::Compile(Replace(text, "format: faithful", "format: authoring"));
        CHECK(!r.has_value());
    }
    {
        auto r = yaml::Compile("header: [unclosed\n");
        CHECK(!r.has_value() && r.error().line > 0);
    }
}

// Authoring mode: ids, strings and derived counts; edits by id; refusals.
void run_authoring_tests() {
    std::printf(" yaml authoring\n");
    const auto bytes = MakeFile();
    auto nif = NifFile::Load(bytes);
    CHECK(nif.has_value());
    if (!nif) return;

    const auto decompiled = yaml::Decompile(*nif, yaml::Mode::Authoring);
    CHECK(decompiled.has_value());
    if (!decompiled) return;
    const std::string& text = *decompiled;
    CHECK(text.find("format: authoring") != std::string::npos);
    CHECK(text.find("  \"Scene Root\": !BSFadeNode\n") != std::string::npos);   // id = the unique name
    CHECK(text.find("  BSXFlags#0: !BSXFlags\n") != std::string::npos);         // no name
    CHECK(text.find("  BSTriShape#0: !BSTriShape\n") != std::string::npos);     // name isn't UTF-8
    CHECK(text.find("  ZZZ_Unknown#0: !ZZZ_Unknown \"deadbeef\"") != std::string::npos);   // raw
    CHECK(text.find("    name: \"Scene Root\"\n") != std::string::npos);        // string refs are strings
    CHECK(text.find("    name: !hex \"636166e9\"\n") != std::string::npos);
    CHECK(text.find("      - BSTriShape#0\n") != std::string::npos);            // refs name ids
    CHECK(text.find("numVertices") == std::string::npos);                        // derived
    CHECK(text.find("numTriangles") == std::string::npos);
    CHECK(text.find("dataSize") != std::string::npos);                           // not a length field
    CHECK(text.find("  author: \"me\"\n") != std::string::npos);                 // without its NUL
    CHECK(text.find("strings") == std::string::npos);                            // tables are derived
    CHECK(text.find("roots: [\"Scene Root\"]") != std::string::npos);

    auto back = yaml::Compile(text);
    CHECK(back.has_value());
    if (!back) {
        std::printf("  compile: line %zu: %s\n", back.error().line, back.error().message.c_str());
        return;
    }
    // The unused third string is gone; everything else is the same file.
    CHECK(back->header.strings == (std::vector<std::string>{ "Scene Root", std::string("caf\xE9", 4) }));
    CHECK(back->header.author == std::string("me\0", 3));
    CHECK(back->header.blockTypes == nif->header.blockTypes);
    CHECK_EQ(back->header.maxStringLength, 10u);
    CHECK(back->trailing == nif->trailing && back->roots == nif->roots);
    for (size_t i = 0; i < nif->blocks.size(); ++i)
        CHECK(back->blocks[i].Encode() == nif->blocks[i].Encode());   // same string indices here
    CHECK(*yaml::Decompile(*back, yaml::Mode::Authoring) == text);

    // Edits by id: a renamed block with its refs, and a dropped triangle
    // (numTriangles follows).
    {
        std::string edited = Replace(text, "  BSTriShape#0: !BSTriShape", "  Barrel: !BSTriShape");
        edited = Replace(edited, "      - BSTriShape#0\n", "      - Barrel\n");
        CHECK(edited.find("triangles: [{a: 0, b: 1, c: 2}]") != std::string::npos);
        edited = Replace(edited, "triangles: [{a: 0, b: 1, c: 2}]", "triangles: []");
        auto r = yaml::Compile(edited);
        CHECK(r.has_value());
        if (r) {
            auto shape = view::BSTriShape::From(r->blocks[3]);
            CHECK(shape.has_value() && shape->NumTriangles() == 0 && shape->Triangles().empty());
            auto node = view::NiNode::From(r->blocks[0]);
            CHECK(node.has_value() && node->Children().size() == 1 && node->Children()[0].index == 3);
        } else {
            std::printf("  edit: line %zu: %s\n", r.error().line, r.error().message.c_str());
        }
    }
    // Refusals.
    {
        auto r = yaml::Compile(Replace(text, "      - BSTriShape#0\n", "      - Nowhere\n"));
        CHECK(!r.has_value() && r.error().message.find("'Nowhere'") != std::string::npos && r.error().line > 0);
    }
    {
        auto r = yaml::Compile(Replace(text, "  BSXFlags#0: !BSXFlags", "  \"Scene Root\": !BSXFlags"));
        CHECK(!r.has_value() && r.error().message.find("used twice") != std::string::npos);
    }
    {
        auto r = yaml::Compile(Replace(text, "  BSXFlags#0: !BSXFlags", "  BSXFlags#0:"));
        CHECK(!r.has_value() && r.error().message.find("tagged") != std::string::npos);
    }
    // A ref to nothing can't be written in authoring mode.
    {
        NifFile broken = *NifFile::Load(bytes);
        auto node = view::NiNode::From(broken.blocks[0]);
        CHECK(node && node->SetChildren({ NiRef<view::NiAVObject>{ { 99 } } }));
        auto r = yaml::Decompile(broken, yaml::Mode::Authoring);
        CHECK(!r.has_value() && r.error().message.find("points at no block") != std::string::npos);
        CHECK(yaml::Decompile(broken).has_value());   // faithful mode writes it as is
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("niffer-yaml tests\n");
    run_yaml_tests();
    run_authoring_tests();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
