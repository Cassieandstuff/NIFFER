// Schema engine tests: niffer::Block / niffer::Nif driven by the test-only
// schema in tests/schema (generated into tests/generated), which exercises
// every language-0.1 construct, plus a smoke test of the real schema.
#include "test_harness.h"

#include "Framing.h"
#include "generated/Fields.h"
#include "generated/Tables.h"
#include "generated/Views.h"

#include <niffer/Niffer.h>

#include <bit>
#include <cstring>
#include <string>
#include <vector>

using namespace niffer;
namespace tf = niffer_test::fields;
namespace tv = niffer_test::view;

namespace {

const schema::SchemaSet& Test() { return niffer_test::tables::kSchema; }

struct Bytes {
    std::vector<uint8_t> b;
    template <class T> Bytes& put(T v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof(T));
        return *this;
    }
    Bytes& raw(std::initializer_list<uint8_t> v) { b.insert(b.end(), v); return *this; }
    Bytes& str(const std::string& s) {
        put<uint32_t>(static_cast<uint32_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
        return *this;
    }
};

constexpr uint32_t kNanBits = 0x7FC01234u;   // a NaN with a payload: must survive

// A TAll block exercising every construct. `child` / `leaves` are block indices.
std::vector<uint8_t> MakeTAll(int32_t child = 1, int32_t leaf = 2) {
    Bytes x;
    x.put<uint32_t>(5).put<uint32_t>(0x10);             // TBase: name, flags (bit 4 → rows/outer)
    x.put<uint8_t>(0xAB).put<uint16_t>(0xBEEF).put<uint32_t>(0xDEADBEEF)
     .put<uint64_t>(0x0123456789ABCDEFull);
    x.put<int8_t>(-5).put<int16_t>(-300).put<int32_t>(-70000).put<int64_t>(-1);
    x.put<uint32_t>(kNanBits);                          // f32v
    x.put<int32_t>(child).put<int32_t>(-1);             // child (ref), back (ptr)
    x.str(std::string("hi\0x", 4));                     // label: embedded NUL kept
    x.raw({1, 2, 3});                                   // fixedBytes
    x.put<uint16_t>(1).put<uint16_t>(2).put<uint16_t>(3).put<uint16_t>(4);   // pairs
    x.put<uint8_t>(2).raw({9, 8});                      // small
    x.put<uint16_t>(1).put<uint16_t>(0x1234);           // medium
    x.put<uint32_t>(2).put<int32_t>(leaf).put<int32_t>(-1);   // leaves
    x.put<uint16_t>(3).put<uint32_t>(10).put<uint32_t>(20).put<uint32_t>(30);  // count, byCount
    x.put<uint32_t>(2).raw({0xAA, 0xBB});               // blobSize, blob
    x.put<uint8_t>(2).put<uint16_t>(7).put<uint16_t>(8);   // counted
    x.put<uint8_t>(2);                                  // rows
    x.put<uint8_t>(1).put<float>(1.5f).put<uint16_t>(77);  //   row 0: mask=1 → value, outer
    x.put<uint8_t>(0).put<uint16_t>(88);                //   row 1: mask=0 → no value
    x.put<int8_t>(-3);                                  // sel: negOnly, neTwo present
    x.put<uint8_t>(0x51).put<uint8_t>(0x52);            // negOnly, neTwo
    x.raw({1, 2, 3});                                   // tail
    return x.b;
}

std::vector<uint8_t> MakeTLeaf(uint32_t value) {
    Bytes x;
    x.put<uint32_t>(0).put<uint32_t>(0).put<uint32_t>(value);
    return x.b;
}

void DecodeAndAccess() {
    const auto bytes = MakeTAll();
    Block b = Block::Load(Test(), "TAll", bytes);
    CHECK(b.Modeled());
    CHECK(b.Refusal().empty());
    CHECK(b.Encode(true) == bytes);   // forced re-encode is byte-exact
    CHECK(b.Encode() == bytes);
    CHECK(!b.Dirty());

    // Handles, including inherited fields through the declaring class's handle.
    CHECK_EQ(b.Get(tf::TBase::name)->index, 5u);
    CHECK_EQ(*b.Get(tf::TBase::flags), 0x10u);
    CHECK_EQ(*b.Get(tf::TAll::u8v), 0xAB);
    CHECK_EQ(*b.Get(tf::TAll::u16v), 0xBEEF);
    CHECK_EQ(*b.Get(tf::TAll::u32v), 0xDEADBEEFu);
    CHECK_EQ(*b.Get(tf::TAll::u64v), 0x0123456789ABCDEFull);
    CHECK_EQ(*b.Get(tf::TAll::i8v), -5);
    CHECK_EQ(*b.Get(tf::TAll::i16v), -300);
    CHECK_EQ(*b.Get(tf::TAll::i32v), -70000);
    CHECK_EQ(*b.Get(tf::TAll::i64v), -1);
    CHECK_EQ(std::bit_cast<uint32_t>(*b.Get(tf::TAll::f32v)), kNanBits);
    CHECK_EQ(b.Get(tf::TAll::label)->size(), 4u);
    CHECK(*b.Get(tf::TAll::fixedBytes) == (std::vector<uint8_t>{1, 2, 3}));
    CHECK(*b.Get(tf::TAll::small) == (std::vector<uint8_t>{9, 8}));
    CHECK(*b.Get(tf::TAll::byCount) == (std::vector<uint32_t>{10, 20, 30}));
    CHECK(*b.Get(tf::TAll::blob) == (std::vector<uint8_t>{0xAA, 0xBB}));
    CHECK(*b.Get(tf::TAll::counted::items) == (std::vector<uint16_t>{7, 8}));
    CHECK(*b.Get(tf::TAll::tail) == (std::vector<uint8_t>{1, 2, 3}));
    // A handle of an unrelated class doesn't apply.
    CHECK(!b.Get(tf::TLeaf::value).has_value());

    // Conditions: mask, outward reference, signed comparisons.
    auto p = [&](const char* s) { return *b.Find(s); };
    CHECK_EQ(*b.GetAt<float>(p("rows/0/value")), 1.5f);
    CHECK(!b.GetAt<float>(p("rows/1/value")).has_value());   // mask bit clear: absent
    CHECK_EQ(*b.GetAt<uint16_t>(p("rows/1/outer")), 88);     // TBase.flags bit 4, one struct out
    CHECK_EQ(*b.Get(tf::TAll::negOnly), 0x51);               // sel = -3 <= -1 (signed)
    CHECK(!b.Get(tf::TAll::eqTwo).has_value());
    CHECK_EQ(*b.Get(tf::TAll::neTwo), 0x52);
    CHECK(!b.Get(tf::TAll::minOne).has_value());             // -3 >= 1 is false (signed)
    CHECK(!b.Find("nope").has_value());
    CHECK(!b.Find("rows/x").has_value());

    // Inheritance.
    CHECK(b.IsA("TAll"));
    CHECK(b.IsA("TBase"));
    CHECK(b.IsA("NiObject"));
    CHECK(!b.IsA("TLeaf"));

    // Refs in layout order, with their declared targets.
    struct R { int32_t idx; bool ptr; size_t to; };
    std::vector<R> refs;
    b.ForEachRef([&](int32_t i, bool ptr, std::span<const uint16_t> to) { refs.push_back({i, ptr, to.size()}); });
    CHECK_EQ(refs.size(), 4u);
    if (refs.size() == 4) {
        CHECK(refs[0].idx == 1 && !refs[0].ptr && refs[0].to == 1);   // child -> [TBase]
        CHECK(refs[1].idx == -1 && refs[1].ptr && refs[1].to == 0);    // back
        CHECK(refs[2].idx == 2 && !refs[2].ptr && refs[2].to == 1);    // leaves[0] -> [TLeaf]
        CHECK(refs[3].idx == -1);
    }
    std::vector<uint32_t> strs;
    b.ForEachStringRef([&](uint32_t s) { strs.push_back(s); });
    CHECK(strs == std::vector<uint32_t>{5});
}

void Views() {
    const auto bytes = MakeTAll();
    Block b = Block::Load(Test(), "TAll", bytes);
    auto v = tv::TAll::From(b);
    CHECK(v.has_value());
    CHECK(!tv::TLeaf::From(b).has_value());
    CHECK(tv::TBase::From(b).has_value());
    if (!v) return;
    CHECK_EQ(v->Name().index, 5u);                 // inherited accessor
    CHECK_EQ(v->U8v(), 0xAB);
    CHECK_EQ(v->Child().index, 1);
    CHECK_EQ(v->Back().index, -1);
    CHECK_EQ(v->Leaves().size(), 2u);
    CHECK_EQ(v->Leaves()[0].index, 2);
    CHECK_EQ(v->Pairs().size(), 2u);
    CHECK_EQ(v->Pairs()[1].B(), 4);
    CHECK(v->Counted().Items() == (std::vector<uint16_t>{7, 8}));
    auto rows = v->Rows();
    CHECK_EQ(rows.size(), 2u);
    if (rows.size() == 2) {
        CHECK_EQ(*rows[0].Value(), 1.5f);
        CHECK(!rows[1].Value().has_value());
        CHECK_EQ(*rows[1].Outer(), 88);
    }
    CHECK(v->NegOnly().has_value());
    CHECK(!v->EqTwo().has_value());

    // Edits through a view.
    CHECK(v->SetU8v(0xAB));
    CHECK(!b.Dirty());                             // no-op set: stays verbatim
    CHECK(v->SetU8v(0x01));
    CHECK(b.Dirty());
    const auto edited = b.Encode();
    CHECK_EQ(edited.size(), bytes.size());
    size_t diffs = 0;
    for (size_t i = 0; i < bytes.size() && i < edited.size(); ++i) diffs += bytes[i] != edited[i];
    CHECK_EQ(diffs, 1u);
    CHECK_EQ(*Block::Load(Test(), "TAll", edited).Get(tf::TAll::u8v), 0x01);
    CHECK(rows.size() == 2 && rows[1].SetValue(2.5f));   // condition still false: not written
    CHECK(Block::Load(Test(), "TAll", b.Encode()).Modeled());
}

void LengthFields() {
    // Replacing an array updates its count field; replacing bytes updates its size field.
    Block b = Block::Load(Test(), "TAll", MakeTAll());
    CHECK(b.Set(tf::TAll::byCount, std::vector<uint32_t>{1, 2}));
    CHECK_EQ(*b.Get(tf::TAll::count), 2);
    CHECK(b.Set(tf::TAll::blob, std::vector<uint8_t>{1, 2, 3, 4, 5}));
    CHECK_EQ(*b.Get(tf::TAll::blobSize), 5u);
    CHECK(b.Set(tf::TAll::counted::items, std::vector<uint16_t>{1, 2, 3}));
    CHECK_EQ(*b.Get(tf::TAll::counted::n), 3);

    Block r = Block::Load(Test(), "TAll", b.Encode());
    CHECK(r.Modeled());
    CHECK(*r.Get(tf::TAll::byCount) == (std::vector<uint32_t>{1, 2}));
    CHECK(*r.Get(tf::TAll::blob) == (std::vector<uint8_t>{1, 2, 3, 4, 5}));
    CHECK(*r.Get(tf::TAll::counted::items) == (std::vector<uint16_t>{1, 2, 3}));
    CHECK(*r.Get(tf::TAll::tail) == (std::vector<uint8_t>{1, 2, 3}));
}

void Refusals() {
    // Exact accounting: short and long blocks stay raw, with a reason.
    auto leaf = MakeTLeaf(7);
    CHECK(Block::Load(Test(), "TLeaf", leaf).Modeled());
    auto shortLeaf = leaf;
    shortLeaf.pop_back();
    Block s = Block::Load(Test(), "TLeaf", shortLeaf);
    CHECK(!s.Modeled());
    CHECK(!s.Refusal().empty());
    CHECK(s.Encode(true) == shortLeaf);             // raw: original bytes, even forced
    auto longLeaf = leaf;
    longLeaf.push_back(0);
    Block l = Block::Load(Test(), "TLeaf", longLeaf);
    CHECK(!l.Modeled());
    CHECK(l.Refusal().find("consumed 12 of 13") != std::string::npos);

    // A bogus count refuses instead of allocating.
    auto bogus = MakeTAll();
    const size_t leavesCount = 4 + 4 + 1 + 2 + 4 + 8 + 1 + 2 + 4 + 8 + 4 + 4 + 4 + 8 + 3 + 8 + 3 + 4;
    const uint32_t huge = 0x7FFFFFFF;
    std::memcpy(&bogus[leavesCount], &huge, 4);
    Block g = Block::Load(Test(), "TAll", bogus);
    CHECK(!g.Modeled());
    CHECK(g.Refusal().find("exceeds remaining") != std::string::npos);

    // No descriptor, or an abstract one: raw, no refusal reason.
    Block u = Block::Load(Test(), "Nope", leaf);
    CHECK(!u.Modeled());
    CHECK(u.Refusal().empty());
    CHECK(u.Encode() == leaf);
    CHECK(!Block::Load(Test(), "TBase", MakeTLeaf(0)).Modeled());
    CHECK(!u.Get(tf::TBase::flags).has_value());

    // A rest-of-block field accepts zero bytes.
    Block e = Block::Load(Test(), "TRest", {});
    CHECK(e.Modeled());
    CHECK(e.Encode(true).empty());
}

void Container() {
    Header h;
    h.versionLine = "Gamebryo File Format, Version 20.2.0.7";
    h.blockTypes = {"TLeaf", "TAll", "Mystery"};
    h.blockTypeIndex = {0, 1, 0, 2};
    h.strings = {"a", "b", "c", "d", "e", "name5"};
    h.maxStringLength = 5;
    std::vector<std::vector<uint8_t>> bodies = {
        MakeTLeaf(11), MakeTAll(/*child*/ 0, /*leaf*/ 2), MakeTLeaf(22), {0xDE, 0xAD}};
    h.numBlocks = static_cast<uint32_t>(bodies.size());
    const auto file = detail::WriteFraming(h, bodies, {1}, {});

    auto nif = Nif::Load(file, Test());
    CHECK(nif.has_value());
    if (!nif) return;
    CHECK_EQ(nif->blocks.size(), 4u);
    CHECK_EQ(nif->RawCount(), 1u);                  // "Mystery"
    CHECK(nif->Save() == file);
    CHECK(nif->Save(true) == file);
    CHECK_EQ(nif->String(NiStringRef{5}), "name5");

    auto all = tv::TAll::From(nif->blocks[1]);
    CHECK(all.has_value());
    if (!all) return;
    auto child = nif->Get(all->Child());           // NiRef<view::TBase> -> view::TBase
    CHECK(child.has_value());
    auto leaf = nif->Get(all->Leaves()[0]);        // NiRef<view::TLeaf> -> view::TLeaf
    CHECK(leaf.has_value() && leaf->Value() == 22u);
    CHECK(!nif->Get(all->Leaves()[1]).has_value());   // null

    // Editing one block re-encodes only that block.
    CHECK(leaf->SetValue(33));
    auto saved = nif->Save();
    auto again = Nif::Load(saved, Test());
    CHECK(again.has_value() && tv::TLeaf::From(again->blocks[2])->Value() == 33u);
    CHECK(again.has_value() && again->blocks[3].Encode() == (std::vector<uint8_t>{0xDE, 0xAD}));

    // Non-SSE files are refused by the shared framing.
    Header le = h;
    le.userVersion = 11;
    auto bad = Nif::Load(detail::WriteFraming(le, bodies, {1}, {}), Test());
    CHECK(!bad.has_value() && bad.error().kind == NifErrorKind::UnsupportedVersion);
}

// TNew: kind, h, pick (union on kind & 3), windowSize, window (Probe[] in a
// byte window), fixedWindow (u16[] in 4 bytes), tail (Probe). Probe.y is present
// when the enclosing `kind` has bit 2 set.
std::vector<uint8_t> MakeTNew(uint8_t kind) {
    const bool y = kind & 4;
    Bytes x;
    x.put<uint8_t>(kind).put<uint16_t>(0x3C00);                 // kind, h = 1.0
    switch (kind & 3) {
    case 1: x.put<float>(1.5f); break;                          // pick/one
    case 2: case 3:                                             // pick/many
        x.put<uint8_t>(7).put<uint8_t>(1);
        if (y) x.put<uint16_t>(0x1234);
        break;
    default: break;                                             // pick/none: no bytes
    }
    x.put<uint16_t>(y ? 6 : 2);                                 // windowSize
    x.put<uint8_t>(2); if (y) x.put<uint16_t>(0x2222);          // window[0]
    x.put<uint8_t>(3); if (y) x.put<uint16_t>(0x3333);          // window[1]
    x.put<uint16_t>(9).put<uint16_t>(10);                       // fixedWindow
    x.put<uint8_t>(4); if (y) x.put<uint16_t>(5);               // tail
    return x.b;
}

void Language02() {
    // Halves.
    CHECK_EQ(Half::FromFloat(1.0f).bits, 0x3C00);
    CHECK_EQ(Half::FromFloat(-2.0f).bits, 0xC000);
    CHECK_EQ(Half::FromFloat(65504.0f).bits, 0x7BFF);
    CHECK_EQ(Half::FromFloat(1e9f).bits, 0x7C00);
    CHECK_EQ(Half::FromFloat(0.0f).bits, 0x0000);
    CHECK_EQ(Half{0x3C00}.ToFloat(), 1.0f);
    CHECK_EQ(Half::FromFloat(Half{0x3555}.ToFloat()).bits, 0x3555);   // exact round trip
    CHECK_EQ(Half::FromFloat(Half{0x0001}.ToFloat()).bits, 0x0001);   // smallest subnormal

    for (uint8_t kind : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(6), uint8_t(7), uint8_t(4)}) {
        const auto bytes = MakeTNew(kind);
        Block b = Block::Load(Test(), "TNew", bytes);
        CHECK(b.Modeled());
        if (!b.Modeled()) { std::printf("    TNew kind=%u: %s\n", kind, b.Refusal().c_str()); continue; }
        CHECK(b.Encode(true) == bytes);
        auto v = tv::TNew::From(b);
        CHECK(v.has_value());
        if (!v) continue;
        CHECK_EQ(v->H().ToFloat(), 1.0f);
        const int expected = (kind & 3) == 1 ? 0 : (kind & 3) >= 2 ? 1 : 2;
        CHECK_EQ(v->PickCase(), expected);
        CHECK_EQ(v->PickOne().has_value(), expected == 0);
        CHECK_EQ(v->PickMany().has_value(), expected == 1);
        CHECK_EQ(v->PickNone(), expected == 2);
        if (expected == 0) CHECK_EQ(*v->PickOne(), 1.5f);
        if (expected == 1) {
            CHECK_EQ(v->PickMany()->A(), 7);
            CHECK_EQ(v->PickMany()->B().X(), 1);
            CHECK_EQ(v->PickMany()->B().Y().has_value(), (kind & 4) != 0);
        }
        CHECK_EQ(v->Window().size(), 2u);
        CHECK_EQ(v->Window()[1].X(), 3);
        CHECK_EQ(v->Window()[1].Y().has_value(), (kind & 4) != 0);
        CHECK(v->FixedWindow() == (std::vector<uint16_t>{9, 10}));
        CHECK_EQ(v->Tail().X(), 4);
    }

    // Paths through a union case, and a case that isn't selected.
    {
        Block b = Block::Load(Test(), "TNew", MakeTNew(6));
        auto y = b.Find("pick/many/b/y");
        CHECK(y.has_value() && *b.GetAt<uint16_t>(*y) == 0x1234);
        CHECK(b.Find("pick/one").has_value());
        CHECK(!b.GetAt<float>(*b.Find("pick/one")).has_value());
        CHECK(!b.SetAt<float>(*b.Find("pick/one"), 2.f));   // the selector chooses the case
        CHECK(!b.Dirty());
        CHECK(!b.Find("pick/nope").has_value());
        CHECK_EQ(*b.Get(tf::TNew::pick::many::a), 7);

        // Editing inside the selected case re-encodes exactly.
        CHECK(b.Set(tf::TNew::pick::many::a, uint8_t(9)));
        Block r = Block::Load(Test(), "TNew", b.Encode());
        CHECK(r.Modeled() && *r.Get(tf::TNew::pick::many::a) == 9);
    }

    // Replacing a byte-window array updates its size field to the encoded size.
    {
        Block b = Block::Load(Test(), "TNew", MakeTNew(4));   // Probe has y: 3 bytes each
        Value three;   // window is packed: Visit hands it over unpacked
        CHECK(b.Visit(*b.Find("window"), [&](const schema::TypeDef&, const Value& v) { three = v; }));
        CHECK_EQ(three.items.size(), 2u);
        if (three.items.size() != 2) return;
        three.items.push_back(three.items[0]);
        CHECK(b.StoreValue(*b.Find("window"), three));
        CHECK_EQ(*b.Get(tf::TNew::windowSize), 9);
        Block r = Block::Load(Test(), "TNew", b.Encode());
        CHECK(r.Modeled());
        CHECK_EQ(tv::TNew::From(r)->Window().size(), 3u);
    }

    // The same named struct resolved at a different site: TNew2's kind sits
    // after a u32, and Probe still reads it.
    {
        Bytes x;
        x.put<uint32_t>(0xAAAAAAAA).put<uint8_t>(4).put<uint8_t>(8).put<uint16_t>(0x4444);
        Block b = Block::Load(Test(), "TNew2", x.b);
        CHECK(b.Modeled());
        auto v = tv::TNew2::From(b);
        CHECK(v && v->P().Y() == uint16_t(0x4444));
        CHECK(b.Encode(true) == x.b);
    }

    // A byte window larger than what's left refuses. Kind 4 selects the empty
    // case, so windowSize sits right after kind (1) and h (2).
    {
        auto bad = MakeTNew(4);
        bad[3] = 0xFF;   // windowSize = 255: past the end of the block
        Block b = Block::Load(Test(), "TNew", bad);
        CHECK(!b.Modeled());
        CHECK(b.Refusal().find("window") != std::string::npos);
    }
}

void Packed() {
    const auto bytes = MakeTAll();
    Block b = Block::Load(Test(), "TAll", bytes);              // LoadMode::Packed (default)
    Block m = Block::Load(Test(), "TAll", bytes, LoadMode::Materialized);
    CHECK(b.Modeled() && m.Modeled());
    const RelPath byCount = *b.Find("byCount");

    // byCount (uniform u32 elements) is stored packed; rows (elements differ in
    // size: value is conditional per row) falls back to materialized.
    CHECK(b.GetValue(byCount) && b.GetValue(byCount)->packed);
    CHECK(!m.GetValue(byCount)->packed);
    CHECK(!b.GetValue(*b.Find("rows"))->packed);
    CHECK_EQ(b.Count(byCount), 3u);
    CHECK(!b.GetValue(byCount.Child(1)));                      // no tree pointer into packed storage…
    CHECK_EQ(*b.GetAt<uint32_t>(byCount.Child(1)), 20u);        // …but reads decode the element
    CHECK(b.Has(byCount.Child(2)) && !b.Has(byCount.Child(3)));
    CHECK(*b.Get(tf::TAll::byCount) == (std::vector<uint32_t>{10, 20, 30}));
    CHECK(b.Encode(true) == bytes && m.Encode(true) == bytes);

    // Editing one element unpacks the array; the block re-encodes correctly.
    CHECK(b.SetAt<uint32_t>(byCount.Child(1), 99u));
    CHECK(!b.GetValue(byCount)->packed);
    CHECK(*b.Get(tf::TAll::byCount) == (std::vector<uint32_t>{10, 99, 30}));
    Block r = Block::Load(Test(), "TAll", b.Encode());
    CHECK(r.Modeled() && *r.Get(tf::TAll::byCount) == (std::vector<uint32_t>{10, 99, 30}));

    // A packed array of structs whose layout reads an enclosing field (TNew's
    // window of Probe, kind = 6: every Probe carries y).
    Block t = Block::Load(Test(), "TNew", MakeTNew(6));
    const RelPath window = *t.Find("window");
    CHECK(t.GetValue(window) && t.GetValue(window)->packed);
    auto v = tv::TNew::From(t);
    CHECK(v && v->Window().size() == 2u && v->Window()[1].Y() == uint16_t(0x3333));
    CHECK_EQ(*t.GetAt<uint16_t>(*t.Find("window/0/y")), 0x2222);
}

void Elements() {
    // ForEachElement visits each element once, in order, packed or not.
    Block b = Block::Load(Test(), "TAll", MakeTAll());
    std::vector<uint32_t> seen;
    CHECK(b.GetValue(*b.Find("byCount"))->packed);
    CHECK(b.ForEachElement(*b.Find("byCount"), [&](size_t i, const schema::TypeDef& t, const Value& v) {
        CHECK_EQ(i, seen.size());
        CHECK(t.kind == schema::Kind::U32);
        seen.push_back(static_cast<uint32_t>(v.bits));
    }));
    CHECK(seen == (std::vector<uint32_t>{10, 20, 30}));

    // Materialized rows, indexed with the generated field-index constants.
    std::vector<int> hasValue;
    CHECK(b.ForEachElement(*b.Find("rows"), [&](size_t, const schema::TypeDef&, const Value& row) {
        hasValue.push_back(row.items[tv::TAllRows::kValue].present ? 1 : 0);
    }));
    CHECK(hasValue == (std::vector<int>{1, 0}));
    CHECK_EQ(tv::TAllRows::kMask, 0);
    CHECK_EQ(tv::TAllRows::kOuter, 2);

    // Packed elements whose layout reads an enclosing field: TNew's Probe window
    // (kind 6: y present), decoded into one reused scratch value.
    Block t = Block::Load(Test(), "TNew", MakeTNew(6));
    std::vector<uint16_t> ys;
    CHECK(t.ForEachElement(*t.Find("window"), [&](size_t, const schema::TypeDef&, const Value& e) {
        CHECK(e.items[tv::Probe::kY].present);
        ys.push_back(static_cast<uint16_t>(e.items[tv::Probe::kY].bits));
    }));
    CHECK(ys == (std::vector<uint16_t>{0x2222, 0x3333}));
    CHECK(!t.ForEachElement(*t.Find("kind"), [](size_t, const schema::TypeDef&, const Value&) {}));
}

// A one-shape file: a BSTriShape with three vertices (position, uv, normal) and
// one triangle, decoded by both paths.
void SchemaDecodeMesh() {
    const uint64_t desc = 6ull | (4ull << 8) | (5ull << 16) | (0x00Bull << 44);   // 24-byte vertex
    Bytes s;
    s.put<uint32_t>(0xFFFFFFFF).put<uint32_t>(0).put<int32_t>(-1);    // name, extraData, controller
    s.put<uint32_t>(14);                                                 // flags
    for (int i = 0; i < 3; ++i) s.put<float>(0.f);                       // translation
    for (int i = 0; i < 9; ++i) s.put<float>(i % 4 == 0 ? 1.f : 0.f);   // rotation
    s.put<float>(1.f).put<int32_t>(-1);                                  // scale, collisionObject
    for (int i = 0; i < 4; ++i) s.put<float>(1.f);                       // bound sphere
    s.put<int32_t>(-1).put<int32_t>(-1).put<int32_t>(-1);               // skin, shader, alpha
    s.put<uint64_t>(desc).put<uint16_t>(1).put<uint16_t>(3).put<uint32_t>(3 * 24 + 6);
    const float pos[3][3] = {{1.f, 2.f, 3.f}, {-4.f, 5.f, 6.f}, {7.f, -8.f, 9.5f}};
    for (int v = 0; v < 3; ++v) {
        s.put<float>(pos[v][0]).put<float>(pos[v][1]).put<float>(pos[v][2]).put<float>(0.f);
        s.put<uint16_t>(Half::FromFloat(0.25f * v).bits).put<uint16_t>(Half::FromFloat(0.5f).bits);
        s.raw({255, 127, 0, 0});                                         // normal + w
    }
    s.put<uint16_t>(0).put<uint16_t>(1).put<uint16_t>(2);               // triangle
    s.put<uint32_t>(0);                                                  // particleDataSize

    Header h;
    h.versionLine = "Gamebryo File Format, Version 20.2.0.7";
    h.blockTypes = {"BSTriShape"};
    h.blockTypeIndex = {0};
    h.numBlocks = 1;
    const auto file = detail::WriteFraming(h, {s.b}, {0}, {});

    auto typed = NifFile::Load(file);
    auto nif = Nif::Load(file);
    CHECK(typed.has_value() && nif.has_value());
    if (!typed || !nif) return;
    auto* shape = dynamic_cast<BSTriShape*>(typed->blocks[0].get());
    CHECK(shape != nullptr && nif->blocks[0].Modeled());
    if (!shape) return;
    const DecodedMesh a = DecodeMesh(*shape);
    const DecodedMesh b = DecodeShapeMesh(*nif, nif->blocks[0]);
    CHECK_EQ(b.numVertices, 3u);
    CHECK(a.positions == b.positions && a.normals == b.normals && a.uvs == b.uvs);
    CHECK(a.triangles == b.triangles && b.triangles == (std::vector<uint16_t>{0, 1, 2}));
    CHECK(b.positions.size() == 9 && b.positions[8] == 9.5f && b.uvs[2] == 0.25f);
    CHECK(b.normals.size() == 9 && b.normals[0] == 1.f && b.normals[2] == -1.f);
    CHECK(b.colors.empty() && b.boneWeights.empty() && b.tangents.empty());
}

void RealSchemaSmoke() {
    const auto& s = schema::Main();
    CHECK(s.Find("NiNode") != nullptr);
    CHECK(s.Find("NiObjectNET") && s.Find("NiObjectNET")->abstract);

    // BSXFlags: NiExtraData.name + u32.
    Bytes x;
    x.put<uint32_t>(0).put<uint32_t>(0x82);
    Block b = Block::Load(s, "BSXFlags", x.b);
    auto v = view::BSXFlags::From(b);
    CHECK(v.has_value() && v->IntegerData() == 0x82u && v->Name().index == 0u);
    CHECK(b.IsA("NiExtraData"));

    // NiNode: the NiObjectNET / NiAVObject chain, AvTransform order, child refs.
    Bytes n;
    n.put<uint32_t>(0xFFFFFFFF).put<uint32_t>(0).put<int32_t>(-1);    // name, extraData[], controller
    n.put<uint32_t>(14);                                                 // flags
    n.put<float>(1.f).put<float>(2.f).put<float>(3.f);                   // translation
    for (int i = 0; i < 9; ++i) n.put<float>(i % 4 == 0 ? 1.f : 0.f);   // rotation
    n.put<float>(1.f).put<int32_t>(-1);                                  // scale, collisionObject
    n.put<uint32_t>(2).put<int32_t>(1).put<int32_t>(2).put<uint32_t>(0); // children, effects
    Block nb = Block::Load(s, "NiNode", n.b);
    auto node = view::NiNode::From(nb);
    CHECK(node.has_value());
    if (node) {
        CHECK_EQ(node->Transform().Translation().Z(), 3.f);
        CHECK_EQ(node->Children().size(), 2u);
        CHECK_EQ(node->Flags(), 14u);
        CHECK(nb.Encode(true) == n.b);
    }
}

}  // namespace

void run_schema_tests() {
    std::printf(" schema\n");
    DecodeAndAccess();
    Views();
    LengthFields();
    Refusals();
    Container();
    Language02();
    Packed();
    Elements();
    SchemaDecodeMesh();
    RealSchemaSmoke();
}
