#include "test_harness.h"

#include <niffer/Niffer.h>

#include <cstring>
#include <vector>

using namespace niffer;

// Hand-build a minimal FRTRI003 (header + tiny geometry + one diff morph),
// Save->Load->Save, assert byte-identity + field fidelity.
void run_tri_tests() {
    std::printf(" tri\n");

    TriFile t;
    t.vertexNum = 2;
    t.faceNum = 0;
    t.uvVertexNum = 0;   // no UV -> no uv sections
    t.morphNum = 1;
    t.addMorphNum = 0;
    t.addVertexNum = 0;
    t.reservedA.assign(12, 0);
    t.reservedB.assign(20, 0);
    t.baseVertices.assign(static_cast<size_t>(t.vertexNum) * 12, 0);  // 2 verts
    // faceIndices empty (faceNum 0); no uv
    TriMorph m;
    m.name = std::string("Aah\0", 4);   // len-prefixed raw incl null
    m.baseDiff = 0.5f;
    m.deltas.assign(static_cast<size_t>(t.vertexNum) * 6, 7);  // int16[3] per vert
    t.diffMorphs.push_back(m);

    std::vector<uint8_t> bytes = t.Save();
    CHECK(std::memcmp(bytes.data(), "FRTRI003", 8) == 0);

    auto loaded = TriFile::Load(bytes);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK_EQ(loaded->vertexNum, 2);
        CHECK_EQ(loaded->morphNum, 1);
        CHECK_EQ(loaded->diffMorphs.size(), 1u);
        if (!loaded->diffMorphs.empty()) {
            CHECK(loaded->diffMorphs[0].name == std::string("Aah\0", 4));
            CHECK(loaded->diffMorphs[0].baseDiff == 0.5f);
            CHECK_EQ(loaded->diffMorphs[0].deltas.size(), 12u);
        }
        CHECK(loaded->Save() == bytes);   // byte-identical re-write
    }

    // A non-FRTRI003 buffer is rejected.
    std::vector<uint8_t> junk(64, 0);
    auto bad = TriFile::Load(junk);
    CHECK(!bad.has_value());

    // ── Morph blend (DecodeBaseVertices + ApplyMorphs) ──────────────────────
    // 2 verts, one named morph with int16 delta 10 and baseDiff 0.5 → decoded
    // displacement 5.0 per component. Blend at weight 2.0 → +10 per component.
    {
        TriFile b;
        b.vertexNum = 2;
        b.baseVertices.assign(2 * 12, 0);   // decoded base = all zero
        TriMorph mm;
        mm.name = "Grin";
        mm.baseDiff = 0.5f;
        mm.deltas.assign(2 * 6, 0);         // 6 int16 total
        for (size_t i = 0; i < mm.deltas.size() / 2; ++i) {
            int16_t d = 10; std::memcpy(mm.deltas.data() + i * 2, &d, 2);
        }
        b.diffMorphs.push_back(mm);

        std::vector<float> base = DecodeBaseVertices(b);
        CHECK_EQ(base.size(), 6u);          // vertexNum*3
        CHECK(base[0] == 0.f);

        // Named + weighted → +10 everywhere.
        std::map<std::string, float> w{ { "Grin", 2.0f } };
        std::vector<float> out = ApplyMorphs(base, b, w);
        CHECK_EQ(out.size(), 6u);
        CHECK(out[0] == 10.f);
        CHECK(out[5] == 10.f);

        // Unmatched name and zero weight both contribute nothing (base unchanged).
        std::map<std::string, float> none{ { "Nope", 3.0f }, { "Grin", 0.0f } };
        std::vector<float> out2 = ApplyMorphs(base, b, none);
        CHECK(out2 == base);
    }
}
