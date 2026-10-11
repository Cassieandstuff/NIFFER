#pragma once
// ── Scalar text for niffer-yaml ───────────────────────────────────────────────
// How faithful-mode YAML spells strings, bytes and numbers (docs/yaml-format.md),
// in both directions. Every form here round-trips exactly:
//   - strings: double-quoted. Valid UTF-8 is written as is; control characters
//     and the code points YAML treats as line breaks or byte-order marks are
//     escaped (YAML escapes decode to UTF-8, which is the original bytes).
//     Bytes that aren't valid UTF-8 can't be written that way, so such a string
//     is written as `!hex "<bytes>"` instead;
//   - bytes: lowercase hex, two digits per byte;
//   - f32 / half: the shortest decimal that reads back to the same bits; NaNs
//     (whose payload a decimal can't carry) as `!bits 0x…`.

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace niffer::yaml::text {

// True when `s` is well-formed UTF-8 (no overlongs, surrogates or code points
// past U+10FFFF).
inline bool ValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<uint8_t>(s[i]);
        if (c < 0x80) { ++i; continue; }
        size_t n;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
        else return false;
        if (i + n >= s.size()) return false;   // continuation bytes past the end
        for (size_t k = 1; k <= n; ++k) {
            const auto cc = static_cast<uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000)) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n + 1;
    }
    return true;
}

inline void AppendHex(std::string& out, const uint8_t* p, size_t n) {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out.push_back(kDigits[p[i] >> 4]);
        out.push_back(kDigits[p[i] & 0xF]);
    }
}

// Two hex digits per byte, either case; nullopt on anything else.
inline std::optional<std::vector<uint8_t>> ParseHex(std::string_view s) {
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (s.size() % 2) return std::nullopt;
    std::vector<uint8_t> out(s.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const int hi = digit(s[2 * i]), lo = digit(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return out;
}

// A double-quoted YAML scalar for `s`, or `!hex "…"` when `s` isn't UTF-8.
inline void AppendString(std::string& out, std::string_view s) {
    if (!ValidUtf8(s)) {
        out += "!hex \"";
        AppendHex(out, reinterpret_cast<const uint8_t*>(s.data()), s.size());
        out += '"';
        return;
    }
    out += '"';
    for (size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<uint8_t>(s[i]);
        char buf[12];
        switch (c) {
        case '"':  out += "\\\""; continue;
        case '\\': out += "\\\\"; continue;
        case '\0': out += "\\0"; continue;
        case '\t': out += "\\t"; continue;
        case '\n': out += "\\n"; continue;
        case '\r': out += "\\r"; continue;
        default: break;
        }
        if (c < 0x20 || c == 0x7F) {
            std::snprintf(buf, sizeof buf, "\\x%02X", c);
            out += buf;
            continue;
        }
        // U+0080–U+009F (C1 controls; U+0085 is a YAML line break), U+2028/9
        // (line / paragraph separators) and U+FEFF (byte-order mark): escaped
        // so no YAML reader treats them as structure.
        if (c == 0xC2 && i + 1 < s.size() && static_cast<uint8_t>(s[i + 1]) <= 0x9F) {
            std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<uint8_t>(s[i + 1]));
            out += buf;
            ++i;
            continue;
        }
        if (c == 0xE2 && i + 2 < s.size() && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
            (static_cast<uint8_t>(s[i + 2]) == 0xA8 || static_cast<uint8_t>(s[i + 2]) == 0xA9)) {
            out += static_cast<uint8_t>(s[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
            i += 2;
            continue;
        }
        if (c == 0xEF && i + 2 < s.size() && static_cast<uint8_t>(s[i + 1]) == 0xBB &&
            static_cast<uint8_t>(s[i + 2]) == 0xBF) {
            out += "\\uFEFF";
            i += 2;
            continue;
        }
        out += static_cast<char>(c);
    }
    out += '"';
}

// The shortest decimal that reads back to `f` exactly; `.inf` / `-.inf` for
// infinities. Not for NaN (the caller writes those as !bits).
inline void AppendFloat(std::string& out, float f) {
    if (std::isinf(f)) {
        out += f < 0 ? "-.inf" : ".inf";
        return;
    }
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, f);
    out.append(buf, r.ptr);
}

inline std::optional<float> ParseFloat(std::string_view s) {
    if (s == ".inf" || s == "+.inf" || s == ".Inf" || s == ".INF") return INFINITY;
    if (s == "-.inf" || s == "-.Inf" || s == "-.INF") return -INFINITY;
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    float f = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), f);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || std::isnan(f)) return std::nullopt;
    return f;
}

// An integer: optional sign, then decimal or 0x hex. nullopt when it isn't one
// or doesn't fit 64 bits; `negative` reports a leading minus.
struct Int {
    uint64_t magnitude = 0;
    bool     negative  = false;
};
inline std::optional<Int> ParseInt(std::string_view s) {
    Int v;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        v.negative = s[0] == '-';
        s.remove_prefix(1);
    }
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    }
    if (s.empty()) return std::nullopt;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v.magnitude, base);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

}  // namespace niffer::yaml::text
