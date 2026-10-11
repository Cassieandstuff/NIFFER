#pragma once
// ── Stream — the co-location serialization channel ────────────────────────────
// One Stream drives BOTH directions of the file framing (Header::Sync, the
// footer) and of TRI: Sync(Stream&) reads INTO its fields in Read mode and
// writes FROM the same fields in Write mode, so read and write layouts can never
// drift. Block bodies don't use it; the schema engine encodes those.
// Little-endian, host-native (SSE x64). Sticky error: once !ok every op is a
// no-op, so callers can run a whole structure then check once.
//
// Byte-exactness discipline: length-prefixed strings store their RAW content
// bytes verbatim (including any embedded/trailing null), so re-emitting
// length==content.size() reproduces the input regardless of null convention —
// no recomputation, no guessing.

#include <niffer/Niffer.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace niffer {

enum class Dir { Read, Write };

class Stream {
public:
    // Read mode over a borrowed [beg, end) window.
    static Stream Reader(const uint8_t* beg, const uint8_t* end) {
        Stream s;
        s.m_dir = Dir::Read;
        s.m_beg = beg;
        s.m_cur = beg;
        s.m_end = end;
        return s;
    }
    // Write mode appending into `out` (borrowed, must outlive the Stream).
    static Stream Writer(std::vector<uint8_t>& out) {
        Stream s;
        s.m_dir = Dir::Write;
        s.m_out = &out;
        return s;
    }

    Dir  dir() const { return m_dir; }
    bool reading() const { return m_dir == Dir::Read; }
    bool ok = true;
    size_t errorOffset = 0;
    std::string error;

    // Bytes consumed (read) or produced (write) so far.
    size_t Position() const {
        return reading() ? static_cast<size_t>(m_cur - m_beg) : m_written;
    }
    size_t Remaining() const {
        return (reading() && m_cur < m_end) ? static_cast<size_t>(m_end - m_cur) : 0u;
    }

    // Read-mode peek of the next int32 without advancing (0 if <4 bytes remain).
    int32_t PeekI32() const {
        if (!reading() || m_cur + 4 > m_end) return 0;
        int32_t v; std::memcpy(&v, m_cur, 4); return v;
    }

    // ── scalars ───────────────────────────────────────────────────────────────
    void U8(uint8_t& v)   { scalar(v); }
    void U16(uint16_t& v) { scalar(v); }
    void U32(uint32_t& v) { scalar(v); }
    void U64(uint64_t& v) { scalar(v); }
    void I32(int32_t& v)  { scalar(v); }
    void F32(float& v)    { scalar(v); }

    // ── strings (raw content stored verbatim; byte-exact round-trip) ────────────
    // u8 length prefix (Bethesda export strings; length historically counts the
    // trailing null, which is part of the stored content here).
    void ShortString(std::string& s) { lenPrefixedString<uint8_t>(s); }
    // u32 length prefix (block-type table + global string table; no null).
    void SizedString(std::string& s) { lenPrefixedString<uint32_t>(s); }

    // Delimiter-terminated line (the header version string). Stores content
    // WITHOUT the delimiter; re-emits content + delimiter. Byte-exact.
    void Line(std::string& s, uint8_t delim) {
        if (!ok) return;
        if (reading()) {
            const uint8_t* start = m_cur;
            while (m_cur < m_end && *m_cur != delim) ++m_cur;
            if (m_cur >= m_end) { fail("Line: delimiter not found"); return; }
            s.assign(reinterpret_cast<const char*>(start),
                     static_cast<size_t>(m_cur - start));
            ++m_cur;  // consume delimiter
        } else {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
            m_out->insert(m_out->end(), p, p + s.size());
            m_out->push_back(delim);
            m_written += s.size() + 1;
        }
    }

    // ── raw bytes ────────────────────────────────────────────────────────────────
    void Bytes(std::span<uint8_t> buf) {
        if (!ok) return;
        if (reading()) {
            if (m_cur + buf.size() > m_end) { fail("Bytes: read past end"); return; }
            std::memcpy(buf.data(), m_cur, buf.size());
            m_cur += buf.size();
        } else {
            m_out->insert(m_out->end(), buf.begin(), buf.end());
            m_written += buf.size();
        }
    }
    // Read n raw bytes into a fresh vector (read mode); write the vector as-is
    // (write mode).
    void RawVector(std::vector<uint8_t>& v, size_t n) {
        if (!ok) return;
        if (reading()) {
            if (m_cur + n > m_end) { fail("RawVector: read past end"); return; }
            v.assign(m_cur, m_cur + n);
            m_cur += n;
        } else {
            m_out->insert(m_out->end(), v.begin(), v.end());
            m_written += v.size();
        }
    }

private:
    Stream() = default;

    void fail(const char* what) {
        if (ok) { ok = false; errorOffset = Position(); error = what; }
    }

    template <class T>
    void scalar(T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!ok) return;
        if (reading()) {
            if (m_cur + sizeof(T) > m_end) { fail("scalar: read past end"); return; }
            std::memcpy(&v, m_cur, sizeof(T));
            m_cur += sizeof(T);
        } else {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
            m_out->insert(m_out->end(), p, p + sizeof(T));
            m_written += sizeof(T);
        }
    }

    template <class LenT>
    void lenPrefixedString(std::string& s) {
        if (!ok) return;
        if (reading()) {
            LenT n = 0;
            scalar(n);
            if (!ok) return;
            if (m_cur + n > m_end) { fail("string: length past end"); return; }
            s.assign(reinterpret_cast<const char*>(m_cur), n);
            m_cur += n;
        } else {
            LenT n = static_cast<LenT>(s.size());
            scalar(n);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
            m_out->insert(m_out->end(), p, p + s.size());
            m_written += s.size();
        }
    }

    Dir m_dir = Dir::Read;
    // read window
    const uint8_t* m_beg = nullptr;
    const uint8_t* m_cur = nullptr;
    const uint8_t* m_end = nullptr;
    // write sink
    std::vector<uint8_t>* m_out = nullptr;
    size_t m_written = 0;
};

}  // namespace niffer
