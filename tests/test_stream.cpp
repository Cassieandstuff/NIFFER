#include "Stream.h"
#include "test_harness.h"

#include <niffer/Niffer.h>

#include <cstring>
#include <vector>

using namespace niffer;

// Every primitive: write from a value, read back, compare. Read and Write are
// independently derived in Stream, so a layout disagreement fails here.
void run_stream_tests() {
    std::printf(" stream\n");

    // Scalars + math PODs + strings, all in one buffer.
    std::vector<uint8_t> buf;
    {
        Stream w = Stream::Writer(buf);
        uint8_t  u8 = 0xAB;             w.U8(u8);
        uint16_t u16 = 0xBEEF;          w.U16(u16);
        uint32_t u32 = 0xDEADBEEF;      w.U32(u32);
        int32_t  i32 = -12345;          w.I32(i32);
        float    f = 3.14159f;          w.F32(f);
        Vec3     v3{1, 2, 3};           w.Vec3v(v3);
        Quat     q{0.1f, 0.2f, 0.3f, 0.4f}; w.Quatv(q);
        std::string ss = "mcarofano";   w.ShortString(ss);
        std::string sz = "BSFadeNode";  w.SizedString(sz);
        std::string ln = "Gamebryo File Format, Version 20.2.0.7"; w.Line(ln, 0x0A);
        CHECK(w.ok);
    }
    {
        Stream r = Stream::Reader(buf.data(), buf.data() + buf.size());
        uint8_t u8 = 0;   r.U8(u8);   CHECK_EQ(u8, 0xAB);
        uint16_t u16 = 0; r.U16(u16); CHECK_EQ(u16, 0xBEEF);
        uint32_t u32 = 0; r.U32(u32); CHECK_EQ(u32, 0xDEADBEEFu);
        int32_t i32 = 0;  r.I32(i32); CHECK_EQ(i32, -12345);
        float f = 0;      r.F32(f);   CHECK(f > 3.14f && f < 3.15f);
        Vec3 v3;          r.Vec3v(v3); CHECK(v3.x == 1 && v3.y == 2 && v3.z == 3);
        Quat q;           r.Quatv(q);  CHECK(q.w == 0.1f && q.z == 0.4f);
        std::string ss;   r.ShortString(ss); CHECK(ss == "mcarofano");
        std::string sz;   r.SizedString(sz); CHECK(sz == "BSFadeNode");
        std::string ln;   r.Line(ln, 0x0A);  CHECK(ln == "Gamebryo File Format, Version 20.2.0.7");
        CHECK(r.ok);
        CHECK_EQ(r.Remaining(), 0u);
    }

    // Bit-exact float round-trip (memcpy path, no arithmetic): a signalling NaN
    // payload and a denormal must survive.
    {
        std::vector<uint8_t> b;
        uint32_t snanBits = 0x7FA00001u, denormBits = 0x00000001u;
        float snan, denorm;
        std::memcpy(&snan, &snanBits, 4);
        std::memcpy(&denorm, &denormBits, 4);
        Stream w = Stream::Writer(b);
        w.F32(snan); w.F32(denorm);
        Stream r = Stream::Reader(b.data(), b.data() + b.size());
        float snan2, denorm2; r.F32(snan2); r.F32(denorm2);
        uint32_t o1, o2; std::memcpy(&o1, &snan2, 4); std::memcpy(&o2, &denorm2, 4);
        CHECK_EQ(o1, snanBits);
        CHECK_EQ(o2, denormBits);
    }

    // Sticky error: a read past the end sets !ok and stays there.
    {
        std::vector<uint8_t> b = {0x01, 0x02};
        Stream r = Stream::Reader(b.data(), b.data() + b.size());
        uint32_t x = 0; r.U32(x);   // needs 4, only 2 → fail
        CHECK(!r.ok);
        uint8_t y = 0xFF; r.U8(y);  // no-op once !ok
        CHECK_EQ(y, 0xFF);
    }

    // Empty ShortString (length 1, just a null) round-trips byte-exact.
    {
        std::vector<uint8_t> b;
        std::string empty = std::string("\0", 1);  // one null byte of content
        Stream w = Stream::Writer(b);
        w.ShortString(empty);
        CHECK_EQ(b.size(), 2u);       // u8 len(1) + 1 content byte
        CHECK_EQ(b[0], 1u);
        CHECK_EQ(b[1], 0u);
        Stream r = Stream::Reader(b.data(), b.data() + b.size());
        std::string out; r.ShortString(out);
        CHECK_EQ(out.size(), 1u);
        CHECK_EQ(out[0], '\0');
    }
}
