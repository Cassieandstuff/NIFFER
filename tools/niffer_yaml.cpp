// ── niffer-yaml — NIF ↔ YAML from the command line ───────────────────────────
//   decompile <file.nif> [out.yaml]   faithful-mode YAML (stdout without out)
//   compile <file.yaml> <out.nif>     back to a NIF
//   roundtrip <dir>                   the gate: for every SSE .nif under <dir>,
//                                     compile(decompile(nif)) saves the same
//                                     bytes and models the same blocks
//
// Build: -DNIFFER_BUILD_YAML=ON (on by default when NIFFER is the top level).
#include <niffer-yaml/NifferYaml.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
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

static int Decompile(const fs::path& in, const char* out) {
    auto nif = NifFile::LoadFile(in.string());
    if (!nif) {
        std::fprintf(stderr, "%s: %s\n", in.string().c_str(), nif.error().message.c_str());
        return 1;
    }
    const std::string text = yaml::Decompile(*nif);
    if (!out) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    if (!WriteBytes(out, text.data(), text.size())) {
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

namespace {
struct Result {
    enum { Skipped, Same, Differs, Refused } kind = Skipped;
    std::string why;
    size_t yamlBytes = 0, nifBytes = 0;
};
}  // namespace

static Result RoundtripOne(const fs::path& p) {
    Result r;
    const auto bytes = ReadBytes(p);
    auto nif = NifFile::Load(bytes);
    if (!nif) return r;   // not SSE: not this gate's business
    const std::string text = yaml::Decompile(*nif);
    r.yamlBytes = text.size();
    r.nifBytes = bytes.size();
    auto back = yaml::Compile(text);
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
    const auto saved = back->Save();
    if (saved != bytes) {
        size_t i = 0;
        while (i < saved.size() && i < bytes.size() && saved[i] == bytes[i]) ++i;
        r.kind = Result::Differs;
        r.why = "bytes differ at " + std::to_string(i);
        return r;
    }
    r.kind = Result::Same;
    return r;
}

static int Roundtrip(const fs::path& root) {
    std::vector<fs::path> files;
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".nif") files.push_back(e.path());
    }
    std::printf("yaml roundtrip: %zu .nif files under %s\n", files.size(), root.string().c_str());
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Result> results(files.size());
    std::transform(std::execution::par, files.begin(), files.end(), results.begin(), RoundtripOne);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    size_t same = 0, differs = 0, refused = 0, skipped = 0, shown = 0;
    uint64_t yamlBytes = 0, nifBytes = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const Result& r = results[i];
        yamlBytes += r.yamlBytes;
        nifBytes += r.nifBytes;
        switch (r.kind) {
        case Result::Skipped: ++skipped; continue;
        case Result::Same: ++same; continue;
        case Result::Differs: ++differs; break;
        case Result::Refused: ++refused; break;
        }
        if (shown++ < 20) std::printf("  %s: %s\n", files[i].string().c_str(), r.why.c_str());
    }
    std::printf("\n── yaml roundtrip summary ──\n");
    std::printf("  files checked     %zu  (unsupported-ver %zu)\n", same + differs + refused, skipped);
    std::printf("  byte-identical    %zu\n", same);
    std::printf("  differ            %zu\n", differs);
    std::printf("  refused           %zu  (on compile)\n", refused);
    std::printf("  text              %.1f MB of YAML for %.1f MB of NIF (%.1fx)\n", yamlBytes / 1e6, nifBytes / 1e6,
                nifBytes ? double(yamlBytes) / double(nifBytes) : 0.0);
    std::printf("  time              %.1f s\n", secs);
    const bool green = differs == 0 && refused == 0 && same > 0;
    std::printf("\n  GATE: %s\n", green ? "GREEN" : "RED");
    return green ? 0 : 1;
}

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "decompile" && (argc == 3 || argc == 4)) return Decompile(argv[2], argc == 4 ? argv[3] : nullptr);
    if (cmd == "compile" && argc == 4) return Compile(argv[2], argv[3]);
    if (cmd == "roundtrip" && argc == 3) return Roundtrip(argv[2]);
    std::printf("usage:\n"
                "  niffer-yaml decompile <file.nif> [out.yaml]\n"
                "  niffer-yaml compile <file.yaml> <out.nif>\n"
                "  niffer-yaml roundtrip <dir>\n");
    return 2;
}
