#include "Stream.h"
#include "test_harness.h"

#include <niffer/Niffer.h>

#include <vector>

using namespace niffer;

// Hand-build a minimal but structurally complete SSE NIF (header + two unknown
// blocks + footer), Save→Load→Save, and assert byte-identity + field fidelity.
// This is the Phase-1 stand-in for the corpus gate: passthrough must round-trip
// byte-exact before any typed block exists.
void run_header_tests() {
    std::printf(" header/file\n");

    // --- Header field round-trip through Stream directly ---
    {
        Header h;
        h.versionLine = "Gamebryo File Format, Version 20.2.0.7";
        h.version = kNifVersion;
        h.endian = 1;
        h.userVersion = kUserVersion;
        h.numBlocks = 2;
        h.bsVersion = kStreamVersion;
        h.author = std::string("mcarofano\0", 10);
        h.processScript = std::string("\0", 1);
        h.exportScript = std::string(" PE Static Art\0", 15);
        h.blockTypes = {"BSFadeNode", "BSXFlags"};
        h.blockTypeIndex = {0, 1};
        h.blockSizes = {24, 4};
        h.maxStringLength = 0;
        h.strings = {};
        h.groups = {};

        std::vector<uint8_t> buf;
        Stream w = Stream::Writer(buf);
        h.Sync(w);
        CHECK(w.ok);

        Header h2;
        Stream r = Stream::Reader(buf.data(), buf.data() + buf.size());
        h2.Sync(r);
        CHECK(r.ok);
        CHECK_EQ(r.Remaining(), 0u);
        CHECK(h2.versionLine == h.versionLine);
        CHECK_EQ(h2.version, kNifVersion);
        CHECK_EQ(h2.numBlocks, 2u);
        CHECK(h2.author == h.author);
        CHECK(h2.exportScript == h.exportScript);
        CHECK_EQ(h2.blockTypes.size(), 2u);
        CHECK(h2.blockTypes[0] == "BSFadeNode");
        CHECK(h2.TypeNameOf(1) == "BSXFlags");
    }

    // --- Full NifFile Save → Load → Save byte-identity (all UnknownBlock) ---
    {
        NifFile f;
        f.header.versionLine = "Gamebryo File Format, Version 20.2.0.7";
        f.header.version = kNifVersion;
        f.header.endian = 1;
        f.header.userVersion = kUserVersion;
        f.header.bsVersion = kStreamVersion;
        f.header.author = std::string("me\0", 3);
        f.header.processScript = std::string("\0", 1);
        f.header.exportScript = std::string("\0", 1);
        // Deliberately-unregistered type names so this exercises the passthrough
        // path deterministically, regardless of which blocks become typed later.
        f.header.blockTypes = {"ZZZ_FakeBlockA", "ZZZ_FakeBlockB"};
        f.header.blockTypeIndex = {0, 1};
        f.header.blockSizes = {0, 0};  // recomputed on Save
        f.header.numBlocks = 2;

        auto a = std::make_unique<UnknownBlock>();
        a->typeName = "ZZZ_FakeBlockA";
        a->raw = {1, 2, 3, 4, 5, 6, 7, 8};
        auto b = std::make_unique<UnknownBlock>();
        b->typeName = "ZZZ_FakeBlockB";
        b->raw = {0xFF, 0x00, 0xAA, 0x55};
        f.blocks.push_back(std::move(a));
        f.blocks.push_back(std::move(b));
        f.roots = {0};

        std::vector<uint8_t> bytes = f.Save();

        auto loaded = NifFile::Load(bytes);
        CHECK(loaded.has_value());
        if (loaded) {
            CHECK_EQ(loaded->blocks.size(), 2u);
            CHECK_EQ(loaded->unknownBlocks.size(), 2u);   // both types unregistered
            CHECK_EQ(loaded->layoutMismatches.size(), 0u);
            CHECK_EQ(loaded->roots.size(), 1u);
            CHECK_EQ(loaded->roots[0], 0);
            CHECK(loaded->header.TypeNameOf(0) == "ZZZ_FakeBlockA");

            std::vector<uint8_t> bytes2 = loaded->Save();
            CHECK_EQ(bytes.size(), bytes2.size());
            CHECK(bytes == bytes2);   // byte-identical re-write
        }
    }

    // --- Typed block (BSXFlags) round-trips typed, not as unknown ---
    {
        NifFile f;
        f.header.versionLine = "Gamebryo File Format, Version 20.2.0.7";
        f.header.version = kNifVersion;
        f.header.endian = 1;
        f.header.userVersion = kUserVersion;
        f.header.bsVersion = kStreamVersion;
        f.header.author = std::string("\0", 1);
        f.header.processScript = std::string("\0", 1);
        f.header.exportScript = std::string("\0", 1);
        f.header.blockTypes = {"BSXFlags"};
        f.header.blockTypeIndex = {0};
        f.header.blockSizes = {0};
        f.header.numBlocks = 1;
        f.header.strings = {"BSX"};
        f.header.maxStringLength = 3;

        auto x = std::make_unique<BSXFlags>();
        x->name.index = 0;
        x->integerData = 0xCAFE;
        f.blocks.push_back(std::move(x));

        auto bytes = f.Save();
        auto loaded = NifFile::Load(bytes);
        CHECK(loaded.has_value());
        if (loaded) {
            CHECK_EQ(loaded->unknownBlocks.size(), 0u);   // typed, not raw
            CHECK_EQ(loaded->layoutMismatches.size(), 0u);
            auto* bsx = dynamic_cast<BSXFlags*>(loaded->GetBlock(0));
            CHECK(bsx != nullptr);
            if (bsx) {
                CHECK_EQ(bsx->integerData, 0xCAFEu);
                CHECK_EQ(bsx->name.index, 0u);
                CHECK(loaded->String(bsx->name) == "BSX");
            }
            CHECK(loaded->Save() == bytes);   // still byte-identical
        }
    }

    // --- Rejects a non-SSE version ---
    {
        NifFile f;
        f.header.versionLine = "Gamebryo File Format, Version 20.2.0.7";
        f.header.version = kNifVersion;
        f.header.endian = 1;
        f.header.userVersion = kUserVersion;
        f.header.bsVersion = 83;  // LE
        f.header.numBlocks = 0;
        std::vector<uint8_t> bytes = f.Save();
        auto loaded = NifFile::Load(bytes);
        CHECK(!loaded.has_value());
        if (!loaded) CHECK(loaded.error().kind == NifErrorKind::UnsupportedVersion);
    }
}
