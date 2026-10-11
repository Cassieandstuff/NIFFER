#include "test_harness.h"

#include <niffer/Niffer.h>

#include <cmath>
#include <cstring>

using namespace niffer;

void run_decode_tests() {
    std::printf(" decode\n");

    // BSVertexDesc: the barrel01 static-mesh desc (0x0001B00000650407).
    {
        VertexLayout L = DecodeVertexDesc(0x0001B00000650407ull);
        CHECK_EQ(L.vertexSize, 28u);
        CHECK(L.fullPrecision);
        CHECK(L.hasVertex && L.hasUV && L.hasNormal && L.hasTangent);
        CHECK(!L.hasColor && !L.hasSkin);
        CHECK_EQ(L.uvOffset, 16u);
        CHECK_EQ(L.normalOffset, 20u);
        CHECK_EQ(L.tangentOffset, 24u);
    }
    // A skinned desc (goldring, 0x0007b0008765040b) -> vertexSize 44, skinned.
    {
        VertexLayout L = DecodeVertexDesc(0x0007b0008765040bull);
        CHECK_EQ(L.vertexSize, 44u);
        CHECK(L.hasSkin);
    }

    // DecodeMesh on a hand-built BSTriShape with one full-precision vertex.
    {
        std::vector<uint8_t> blk;
        auto put = [&](auto v) {
            const auto* p = reinterpret_cast<const uint8_t*>(&v);
            blk.insert(blk.end(), p, p + sizeof v);
        };
        put(0xFFFFFFFFu); put(0u); put(-1);                  // name, extraData[], controller
        put(14u);                                            // flags
        for (int i = 0; i < 3; ++i) put(0.f);                // translation
        for (int i = 0; i < 9; ++i) put(i % 4 == 0 ? 1.f : 0.f);   // rotation
        put(1.f); put(-1);                                   // scale, collisionObject
        for (int i = 0; i < 4; ++i) put(1.f);                // bound sphere
        put(-1); put(-1); put(-1);                           // skin, shader, alpha
        put(0x0001B00000650407ull);                          // 28B: pos@0 full, uv@16, nrm@20, tan@24
        put(uint16_t{0}); put(uint16_t{1}); put(28u);        // numTriangles, numVertices, dataSize
        put(1.5f); put(-2.0f); put(3.25f); put(0.f);         // position + w
        put(uint32_t{0});                                    // uv (two halves)
        put(uint32_t{0x7F7F7FFFu});                          // normal
        put(uint32_t{0x7F7F7FFFu});                          // tangent
        put(0u);                                             // particleDataSize
        Block b = Block::Load(schema::Main(), "BSTriShape", blk);
        auto shape = view::BSTriShape::From(b);
        CHECK(shape.has_value());
        if (shape) {
            auto m = DecodeMesh(*shape);
            CHECK_EQ(m.numVertices, 1u);
            CHECK(m.positions.size() == 3);
            if (m.positions.size() == 3) {
                CHECK(std::fabs(m.positions[0] - 1.5f) < 1e-5f);
                CHECK(std::fabs(m.positions[1] + 2.0f) < 1e-5f);
                CHECK(std::fabs(m.positions[2] - 3.25f) < 1e-5f);
            }
            CHECK(m.uvs.size() == 2);   // hasUV
            CHECK(m.normals.size() == 3);
        }
    }

    // TRI morph delta decode: int16 * baseDiff.
    {
        TriMorph m;
        m.baseDiff = 0.5f;
        int16_t d[3] = {10, -4, 6};
        m.deltas.assign(reinterpret_cast<uint8_t*>(d), reinterpret_cast<uint8_t*>(d) + 6);
        auto f = DecodeMorphDeltas(m);
        CHECK_EQ(f.size(), 3u);
        CHECK(std::fabs(f[0] - 5.0f) < 1e-5f);
        CHECK(std::fabs(f[1] + 2.0f) < 1e-5f);
        CHECK(std::fabs(f[2] - 3.0f) < 1e-5f);
    }
}
