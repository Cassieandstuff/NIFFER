// ── niffer-yaml — NIF ↔ YAML from the command line ───────────────────────────
//   decompile [--authoring] <file.nif> [out.yaml]
//                              YAML (stdout without out); faithful by default
//   compile <file.yaml> <out.nif>
//                              back to a NIF (either mode; the text says which)
//   roundtrip [--authoring] <dir>
//                              the gate, for every SSE .nif under <dir>:
//                              faithful: compile(decompile(nif)) saves the same
//                              bytes and models the same blocks;
//                              authoring: the same file up to the string
//                              table's order, byte-identical where the file
//                              used first-use order, and decompiling the result
//                              gives the same text again.
//
// Build: -DNIFFER_BUILD_YAML=ON (on by default when NIFFER is the top level).
#include <niffer-yaml/NifferYaml.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <execution>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace niffer;

static std::vector<uint8_t> ReadBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), {} };
}

static bool WriteBytes(const fs::path& p, const void* data, size_t n) {
    std::ofstream out(p, std::ios::binary);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    return static_cast<bool>(out);
}

static int Decompile(const fs::path& in, const char* out, yaml::Mode mode) {
    auto nif = NifFile::LoadFile(in.string());
    if (!nif) {
        std::fprintf(stderr, "%s: %s\n", in.string().c_str(), nif.error().message.c_str());
        return 1;
    }
    const auto text = yaml::Decompile(*nif, mode);
    if (!text) {
        std::fprintf(stderr, "%s: %s\n", in.string().c_str(), text.error().message.c_str());
        return 1;
    }
    if (!out) {
        std::fwrite(text->data(), 1, text->size(), stdout);
        return 0;
    }
    if (!WriteBytes(out, text->data(), text->size())) {
        std::fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    return 0;
}

static int Compile(const fs::path& in, const fs::path& out) {
    const auto bytes = ReadBytes(in);
    auto nif = yaml::Compile(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!nif) {
        const auto& e = nif.error();
        std::fprintf(stderr, "%s:%zu:%zu: %s\n", in.string().c_str(), e.line, e.column, e.message.c_str());
        return 1;
    }
    const auto saved = nif->Save();
    if (!WriteBytes(out, saved.data(), saved.size())) {
        std::fprintf(stderr, "cannot write %s\n", out.string().c_str());
        return 1;
    }
    return 0;
}

// ── same file up to the string table's order ─────────────────────────────────
namespace {

using schema::Kind;

// Whether two values of type `t` are equal, string refs compared by their text
// (each in its own file's table) and everything else exactly.
bool SameValue(const schema::SchemaSet& s, const schema::TypeDef& t, const Value& a, const Value& b,
               const NifFile& fa, const NifFile& fb) {
    if (a.present != b.present) return false;
    if (!a.present) return true;
    switch (t.kind) {
    case Kind::Struct:
    case Kind::Array:
        if (a.items.size() != b.items.size()) return false;
        for (size_t i = 0; i < a.items.size(); ++i) {
            const auto& et = t.kind == Kind::Array ? s.types[t.ref] : s.types[s.fields[t.ref + i].type];
            if (!SameValue(s, et, a.items[i], b.items[i], fa, fb)) return false;
        }
        return true;
    case Kind::Union:
        if (a.bits != b.bits || a.items.size() != b.items.size()) return false;
        return a.items.empty() ||
               SameValue(s, s.types[s.cases[t.ref + a.bits].type], a.items[0], b.items[0], fa, fb);
    case Kind::StringRef: {
        const auto ia = static_cast<uint32_t>(a.bits), ib = static_cast<uint32_t>(b.bits);
        if (ia == 0xFFFFFFFFu || ib == 0xFFFFFFFFu) return ia == ib;
        return ia < fa.header.strings.size() && ib < fb.header.strings.size() &&
               fa.header.strings[ia] == fb.header.strings[ib];
    }
    case Kind::SizedString: return a.str == b.str;
    case Kind::Bytes: return a.bytes == b.bytes;
    default: return a.bits == b.bits;
    }
}

// The same file, the string table's order aside: same header facts, same set
// of strings, same blocks with the same values.
bool SameFile(const NifFile& a, const NifFile& b, std::string& why) {
    const Header& ha = a.header;
    const Header& hb = b.header;
    if (ha.author != hb.author || ha.processScript != hb.processScript || ha.exportScript != hb.exportScript ||
        ha.blockTypes != hb.blockTypes || ha.groups != hb.groups || ha.maxStringLength != hb.maxStringLength) {
        why = "header differs";
        return false;
    }
    auto sa = ha.strings, sb = hb.strings;
    std::sort(sa.begin(), sa.end());
    std::sort(sb.begin(), sb.end());
    if (sa != sb) {
        why = "the string tables hold different strings";
        return false;
    }
    if (a.blocks.size() != b.blocks.size() || a.roots != b.roots || a.trailing != b.trailing) {
        why = "blocks, roots or trailing bytes differ";
        return false;
    }
    for (size_t i = 0; i < a.blocks.size(); ++i) {
        const Block& x = a.blocks[i];
        const Block& y = b.blocks[i];
        if (x.TypeName() != y.TypeName() || x.Modeled() != y.Modeled()) {
            why = "block " + std::to_string(i) + " differs in class or modeling";
            return false;
        }
        if (!x.Modeled()) {
            if (!std::equal(x.Original().begin(), x.Original().end(), y.Original().begin(), y.Original().end())) {
                why = "raw block " + std::to_string(i) + " differs";
                return false;
            }
            continue;
        }
        // Compare materialized values (packed arrays decoded).
        const auto& s = *x.Schema();
        const Block mx = Block::Load(s, x.TypeName(), x.Encode(), LoadMode::Materialized);
        const Block my = Block::Load(s, y.TypeName(), y.Encode(), LoadMode::Materialized);
        if (!SameValue(s, s.types[mx.Def()->type], *mx.Root(), *my.Root(), a, b)) {
            why = "block " + std::to_string(i) + " (" + std::string(x.TypeName()) + ") differs";
            return false;
        }
    }
    return true;
}

struct Result {
    enum { Skipped, Same, SameUpToStrings, Differs, Refused } kind = Skipped;
    std::string why;
    size_t yamlBytes = 0, nifBytes = 0;
};

}  // namespace

static Result RoundtripOne(const fs::path& p, yaml::Mode mode) {
    Result r;
    const auto bytes = ReadBytes(p);
    auto nif = NifFile::Load(bytes);
    if (!nif) return r;   // not SSE: not this gate's business
    const auto text = yaml::Decompile(*nif, mode);
    if (!text) {
        r.kind = Result::Refused;
        r.why = "decompile: " + text.error().message;
        return r;
    }
    r.yamlBytes = text->size();
    r.nifBytes = bytes.size();
    auto back = yaml::Compile(*text);
    if (!back) {
        r.kind = Result::Refused;
        r.why = "line " + std::to_string(back.error().line) + ": " + back.error().message;
        return r;
    }
    if (back->RawCount() != nif->RawCount()) {
        r.kind = Result::Differs;
        r.why = "modeled blocks changed";
        return r;
    }
    if (mode == yaml::Mode::Authoring) {
        const auto again = yaml::Decompile(*back, mode);
        if (!again || *again != *text) {
            r.kind = Result::Differs;
            r.why = "decompiling the compiled file gives different text";
            return r;
        }
    }
    const auto saved = back->Save();
    if (saved == bytes) {
        r.kind = Result::Same;
        return r;
    }
    if (mode == yaml::Mode::Authoring && SameFile(*nif, *back, r.why)) {
        r.kind = Result::SameUpToStrings;
        return r;
    }
    if (r.why.empty()) {
        size_t i = 0;
        while (i < saved.size() && i < bytes.size() && saved[i] == bytes[i]) ++i;
        r.why = "bytes differ at " + std::to_string(i);
    }
    r.kind = Result::Differs;
    return r;
}

static int Roundtrip(const fs::path& root, yaml::Mode mode) {
    const bool authoring = mode == yaml::Mode::Authoring;
    std::vector<fs::path> files;
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".nif") files.push_back(e.path());
    }
    std::printf("yaml roundtrip (%s): %zu .nif files under %s\n", authoring ? "authoring" : "faithful", files.size(),
                root.string().c_str());
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Result> results(files.size());
    std::transform(std::execution::par, files.begin(), files.end(), results.begin(),
                   [&](const fs::path& p) { return RoundtripOne(p, mode); });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    size_t same = 0, upTo = 0, differs = 0, refused = 0, skipped = 0, shown = 0;
    uint64_t yamlBytes = 0, nifBytes = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const Result& r = results[i];
        yamlBytes += r.yamlBytes;
        nifBytes += r.nifBytes;
        switch (r.kind) {
        case Result::Skipped: ++skipped; continue;
        case Result::Same: ++same; continue;
        case Result::SameUpToStrings: ++upTo; continue;
        case Result::Differs: ++differs; break;
        case Result::Refused: ++refused; break;
        }
        if (shown++ < 20) std::printf("  %s: %s\n", files[i].string().c_str(), r.why.c_str());
    }
    std::printf("\n── yaml roundtrip summary ──\n");
    std::printf("  files checked     %zu  (unsupported-ver %zu)\n", same + upTo + differs + refused, skipped);
    std::printf("  byte-identical    %zu\n", same);
    if (authoring) std::printf("  same file         %zu  (string table in another order)\n", upTo);
    std::printf("  differ            %zu\n", differs);
    std::printf("  refused           %zu\n", refused);
    std::printf("  text              %.1f MB of YAML for %.1f MB of NIF (%.1fx)\n", yamlBytes / 1e6, nifBytes / 1e6,
                nifBytes ? double(yamlBytes) / double(nifBytes) : 0.0);
    std::printf("  time              %.1f s\n", secs);
    const bool green = differs == 0 && refused == 0 && same + upTo > 0 && (authoring || upTo == 0);
    std::printf("\n  GATE: %s\n", green ? "GREEN" : "RED");
    return green ? 0 : 1;
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    yaml::Mode mode = yaml::Mode::Faithful;
    if (args.size() > 1 && args[1] == "--authoring") {
        mode = yaml::Mode::Authoring;
        args.erase(args.begin() + 1);
    }
    const std::string cmd = args.empty() ? "" : args[0];
    if (cmd == "decompile" && (args.size() == 2 || args.size() == 3))
        return Decompile(args[1], args.size() == 3 ? args[2].c_str() : nullptr, mode);
    if (cmd == "compile" && args.size() == 3 && mode == yaml::Mode::Faithful) return Compile(args[1], args[2]);
    if (cmd == "roundtrip" && args.size() == 2) return Roundtrip(args[1], mode);
    std::printf("usage:\n"
                "  niffer-yaml decompile [--authoring] <file.nif> [out.yaml]\n"
                "  niffer-yaml compile <file.yaml> <out.nif>\n"
                "  niffer-yaml roundtrip [--authoring] <dir>\n");
    return 2;
}
