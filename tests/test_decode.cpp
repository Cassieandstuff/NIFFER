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

    // Full round of DecodeMesh on a hand-built full-precision vertex.
    {
        BSTriShape s;
        s.vertexDesc = 0x0001B00000650407ull;  // 28B: pos@0 full, uv@16, nrm@20, tan@24
        s.numVertices = 1;
        s.numTriangles = 0;
        s.geometryData.assign(28, 0);
        float px = 1.5f, py = -2.0f, pz = 3.25f;
        std::memcpy(&s.geometryData[0], &px, 4);
        std::memcpy(&s.geometryData[4], &py, 4);
        std::memcpy(&s.geometryData[8], &pz, 4);
        auto m = DecodeMesh(s);
        CHECK_EQ(m.numVertices, 1u);
        CHECK(m.positions.size() == 3);
        CHECK(std::fabs(m.positions[0] - 1.5f) < 1e-5f);
        CHECK(std::fabs(m.positions[1] + 2.0f) < 1e-5f);
        CHECK(std::fabs(m.positions[2] - 3.25f) < 1e-5f);
        CHECK(m.uvs.size() == 2);   // hasUV
        CHECK(m.normals.size() == 3);
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
