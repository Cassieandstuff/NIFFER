#include "Framing.h"
#include "Stream.h"
#include "test_harness.h"

#include <niffer/Niffer.h>

#include <algorithm>
#include <vector>

using namespace niffer;

// The header through Stream, then hand-built minimal SSE files (header + blocks
// + footer): Load→Save must be byte-identical, raw and modeled blocks alike.
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

    // A minimal SSE header for the file-level tests below.
    auto header = [](std::vector<std::string> types, std::vector<std::string> strings) {
        Header h;
        h.versionLine = "Gamebryo File Format, Version 20.2.0.7";
        h.author = std::string("me\0", 3);
        h.processScript = std::string("\0", 1);
        h.exportScript = std::string("\0", 1);
        h.blockTypes = types;
        for (uint16_t i = 0; i < types.size(); ++i) h.blockTypeIndex.push_back(i);
        h.numBlocks = static_cast<uint32_t>(types.size());
        h.maxStringLength = 0;
        for (auto& str : strings) h.maxStringLength = std::max<uint32_t>(h.maxStringLength, static_cast<uint32_t>(str.size()));
        h.strings = std::move(strings);
        return h;
    };

    // --- Blocks with no descriptor pass through raw, byte-identical ---
    {
        // Type names no schema will ever describe, so this always exercises
        // the passthrough path.
        const auto bytes = detail::WriteFraming(header({"ZZZ_FakeBlockA", "ZZZ_FakeBlockB"}, {}),
                                                {{1, 2, 3, 4, 5, 6, 7, 8}, {0xFF, 0x00, 0xAA, 0x55}},
                                                {0}, {});
        auto loaded = NifFile::Load(bytes);
        CHECK(loaded.has_value());
        if (loaded) {
            CHECK_EQ(loaded->blocks.size(), 2u);
            CHECK_EQ(loaded->RawCount(), 2u);   // no descriptor: raw
            CHECK(!loaded->blocks[0].Modeled());
            CHECK_EQ(loaded->roots.size(), 1u);
            CHECK_EQ(loaded->roots[0], 0);
            CHECK(loaded->header.TypeNameOf(0) == "ZZZ_FakeBlockA");
            CHECK(loaded->Save() == bytes);         // byte-identical re-write
            CHECK(loaded->Save(true) == bytes);     // forced: raw blocks stay verbatim
        }
    }

    // --- A modeled block (BSXFlags) decodes through its view ---
    {
        std::vector<uint8_t> body;
        Stream w = Stream::Writer(body);
        uint32_t name = 0, flags = 0xCAFE;
        w.U32(name);
        w.U32(flags);
        const auto bytes = detail::WriteFraming(header({"BSXFlags"}, {"BSX"}), {body}, {0}, {});
        auto loaded = NifFile::Load(bytes);
        CHECK(loaded.has_value());
        if (loaded) {
            CHECK_EQ(loaded->RawCount(), 0u);   // modeled, not raw
            auto bsx = view::BSXFlags::From(loaded->blocks[0]);
            CHECK(bsx.has_value());
            if (bsx) {
                CHECK_EQ(bsx->IntegerData(), 0xCAFEu);
                CHECK_EQ(bsx->Name().index, 0u);
                CHECK(loaded->String(bsx->Name()) == "BSX");
            }
            CHECK(loaded->Save() == bytes);
            CHECK(loaded->Save(true) == bytes);
        }
    }

    // --- Rejects a non-SSE version ---
    {
        Header h = header({}, {});
        h.bsVersion = 83;  // LE
        auto loaded = NifFile::Load(detail::WriteFraming(h, {}, {}, {}));
        CHECK(!loaded.has_value());
        if (!loaded) CHECK(loaded.error().kind == NifErrorKind::UnsupportedVersion);
    }
}
