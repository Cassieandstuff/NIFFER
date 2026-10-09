// ── niffer-tool — Tier-2 corpus gates ─────────────────────────────────────────
// Whitebox CLI (has the library's private hpp/ on its include path) driving the
// vanilla-corpus ratchet:
//   histogram <dir>        block-type census (header-only; works before any
//                          typed block exists) — drives milestone ordering.
//   roundtrip <dir>        Load→Save→memcmp over the tree. Invariant: every
//                          SSE file byte-identical. Ratchet: unknown-block
//                          instances → 0. LE/other-version files are reported
//                          as unsupported, never as failures.
//   dump <file.nif>        header + per-block index/type/size; unknowns flagged.
//
// Build: -DNIFFER_BUILD_TOOLS=ON  →  niffer-tool.exe
#include "Stream.h"

#include <niffer/Niffer.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <execution>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace niffer;

static std::vector<uint8_t> ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return {};
    std::streamsize sz = in.tellg();
    in.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0) in.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

static std::vector<fs::path> CollectByExt(const fs::path& root, const char* wantExt) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == wantExt) out.push_back(it->path());
    }
    return out;
}
static std::vector<fs::path> CollectNifs(const fs::path& root) { return CollectByExt(root, ".nif"); }

// Parse only the header (no version whitelist) so histogram sees every file.
static bool HeaderOnly(const std::vector<uint8_t>& bytes, Header& h) {
    if (bytes.empty()) return false;
    Stream r = Stream::Reader(bytes.data(), bytes.data() + bytes.size());
    h.Sync(r);
    return r.ok;
}

// ── histogram ─────────────────────────────────────────────────────────────────
static int CmdHistogram(const fs::path& root) {
    auto files = CollectNifs(root);
    std::printf("histogram: %zu .nif files under %s\n\n", files.size(),
                root.string().c_str());
    std::map<std::string, uint64_t> counts;  // type name → block instances
    uint64_t parsed = 0, unparsed = 0;
    for (auto& p : files) {
        Header h;
        if (!HeaderOnly(ReadFile(p), h)) { ++unparsed; continue; }
        ++parsed;
        for (uint16_t ti : h.blockTypeIndex)
            if (ti < h.blockTypes.size()) ++counts[h.blockTypes[ti]];
    }
    std::vector<std::pair<std::string, uint64_t>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    for (auto& [name, n] : sorted)
        std::printf("%10llu  %s\n", static_cast<unsigned long long>(n), name.c_str());
    std::printf("\n%zu distinct block types; %llu files parsed, %llu unparsed\n",
                sorted.size(), (unsigned long long)parsed, (unsigned long long)unparsed);
    return 0;
}

// ── roundtrip ─────────────────────────────────────────────────────────────────
struct FileResult {
    bool identical = false;
    bool unsupported = false;
    bool malformed = false;
    size_t divergeOffset = 0;   // valid when !identical && loaded
    std::map<std::string, uint64_t> unknownByType;
    uint64_t layoutMismatchCount = 0;
    std::string firstMismatchType;   // for a pinpoint report
};

static FileResult RoundtripOne(const fs::path& p) {
    FileResult r;
    auto bytes = ReadFile(p);
    auto loaded = NifFile::Load(bytes);
    if (!loaded) {
        if (loaded.error().kind == NifErrorKind::UnsupportedVersion) r.unsupported = true;
        else r.malformed = true;
        return r;
    }
    for (auto& u : loaded->unknownBlocks) ++r.unknownByType[u.typeName];
    r.layoutMismatchCount = loaded->layoutMismatches.size();
    if (!loaded->layoutMismatches.empty())
        r.firstMismatchType = loaded->layoutMismatches.front().typeName;
    auto out = loaded->Save();
    if (out.size() == bytes.size() && std::equal(out.begin(), out.end(), bytes.begin())) {
        r.identical = true;
    } else {
        size_t n = std::min(out.size(), bytes.size()), i = 0;
        while (i < n && out[i] == bytes[i]) ++i;
        r.divergeOffset = i;
    }
    return r;
}

static int CmdRoundtrip(const fs::path& root) {
    auto files = CollectNifs(root);
    std::printf("roundtrip: %zu .nif files under %s\n", files.size(),
                root.string().c_str());

    // Positional parallel transform: results[i] corresponds to files[i].
    std::vector<FileResult> results(files.size());
    std::transform(std::execution::par, files.begin(), files.end(),
                   results.begin(), [](const fs::path& p) { return RoundtripOne(p); });

    uint64_t identical = 0, unsupported = 0, malformed = 0, diffs = 0;
    uint64_t totalUnknown = 0, totalMismatch = 0;
    std::map<std::string, uint64_t> unknownByType;
    std::map<std::string, uint64_t> mismatchByType;
    std::vector<size_t> diffFiles, mismatchFiles;
    for (size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        if (r.unsupported) { ++unsupported; continue; }
        if (r.malformed) { ++malformed; continue; }
        if (r.identical) ++identical;
        else { ++diffs; if (diffFiles.size() < 20) diffFiles.push_back(i); }
        for (auto& [t, n] : r.unknownByType) { unknownByType[t] += n; totalUnknown += n; }
        totalMismatch += r.layoutMismatchCount;
        if (r.layoutMismatchCount) {
            mismatchByType[r.firstMismatchType] += r.layoutMismatchCount;
            if (mismatchFiles.size() < 8) mismatchFiles.push_back(i);
        }
    }
    if (!mismatchByType.empty()) {
        std::printf("\nLAYOUT MISMATCHES by type:\n");
        for (auto& [t, n] : mismatchByType)
            std::printf("  %8llu  %s\n", (unsigned long long)n, t.c_str());
        std::printf("example files:\n");
        for (size_t i : mismatchFiles)
            std::printf("  %s\n", files[i].string().c_str());
    }

    if (!diffFiles.empty()) {
        std::printf("\nBYTE-DIFF failures (first %zu):\n", diffFiles.size());
        for (size_t i : diffFiles)
            std::printf("  @%zu  %s\n", results[i].divergeOffset,
                        files[i].string().c_str());
    }
    if (!unknownByType.empty()) {
        std::vector<std::pair<std::string, uint64_t>> s(unknownByType.begin(),
                                                        unknownByType.end());
        std::sort(s.begin(), s.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::printf("\nUNKNOWN block types (%llu instances, %zu types):\n",
                    (unsigned long long)totalUnknown, s.size());
        for (auto& [t, n] : s)
            std::printf("  %10llu  %s\n", (unsigned long long)n, t.c_str());
    }

    std::printf("\n── roundtrip summary ──\n");
    std::printf("  files            %zu\n", files.size());
    std::printf("  byte-identical   %llu\n", (unsigned long long)identical);
    std::printf("  byte-diff        %llu\n", (unsigned long long)diffs);
    std::printf("  unsupported-ver  %llu\n", (unsigned long long)unsupported);
    std::printf("  malformed        %llu\n", (unsigned long long)malformed);
    std::printf("  unknown blocks   %llu instances\n", (unsigned long long)totalUnknown);
    std::printf("  layout mismatch  %llu\n", (unsigned long long)totalMismatch);

    bool gateGreen = (diffs == 0 && malformed == 0);
    bool parity = gateGreen && totalUnknown == 0 && totalMismatch == 0;
    std::printf("\n  GATE: %s   PARITY: %s\n",
                gateGreen ? "GREEN (all supported files byte-identical)" : "RED",
                parity ? "REACHED" : "not yet (unknowns/mismatches remain)");
    return gateGreen ? 0 : 1;
}

static void HexDump(const std::vector<uint8_t>& b) {
    for (size_t i = 0; i < b.size(); i += 16) {
        std::printf("  %4zu:", i);
        for (size_t j = 0; j < 16 && i + j < b.size(); ++j)
            std::printf(" %02x", b[i + j]);
        std::printf("   ");
        for (size_t j = 0; j < 16 && i + j < b.size(); ++j) {
            uint8_t c = b[i + j];
            std::printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        std::printf("\n");
    }
}

// ── dump ──────────────────────────────────────────────────────────────────────
// `dump <file>`            — header + block table.
// `dump <file> <blockIdx>` — additionally hexdump that block's serialized bytes
//                            (an RE aid: works for typed and raw blocks alike).
static int CmdDump(const fs::path& file, int hexBlock) {
    auto bytes = ReadFile(file);
    auto loaded = NifFile::Load(bytes);
    if (!loaded) {
        std::printf("load failed: %s (offset %zu, block %d)\n",
                    loaded.error().message.c_str(), loaded.error().offset,
                    loaded.error().blockIndex);
        return 1;
    }
    auto& f = *loaded;
    std::printf("%s\n", f.header.versionLine.c_str());
    std::printf("version=0x%08X user=%u bs=%u  blocks=%zu roots=%zu strings=%zu\n",
                f.header.version, f.header.userVersion, f.header.bsVersion,
                f.blocks.size(), f.roots.size(), f.header.strings.size());
    for (size_t i = 0; i < f.blocks.size(); ++i) {
        bool unknown = std::any_of(f.unknownBlocks.begin(), f.unknownBlocks.end(),
                                   [&](auto& u) { return u.blockIndex == i; });
        bool demoted = std::any_of(f.layoutMismatches.begin(), f.layoutMismatches.end(),
                                   [&](auto& m) { return m.blockIndex == i; });
        std::printf("  [%3zu] size=%-6u %s%s\n", i, f.header.blockSizes[i],
                    std::string(f.blocks[i]->TypeName()).c_str(),
                    unknown ? "   (UNKNOWN/raw)" : demoted ? "   (DEMOTED/raw)" : "");
    }
    if (!f.layoutMismatches.empty()) {
        std::printf("\nLAYOUT MISMATCHES:\n");
        for (auto& m : f.layoutMismatches)
            std::printf("  [%u] %s declared=%u consumed=%u\n", m.blockIndex,
                        m.typeName.c_str(), m.declaredSize, m.consumedSize);
    }
    if (hexBlock >= 0 && static_cast<size_t>(hexBlock) < f.blocks.size()) {
        std::vector<uint8_t> body;
        Stream w = Stream::Writer(body);
        f.blocks[hexBlock]->Sync(w);
        std::printf("\nblock [%d] %s  (%zu bytes):\n", hexBlock,
                    std::string(f.blocks[hexBlock]->TypeName()).c_str(), body.size());
        HexDump(body);
        std::printf("\nstring table:\n");
        for (size_t i = 0; i < f.header.strings.size(); ++i)
            std::printf("  %3zu  \"%s\"\n", i, f.header.strings[i].c_str());
    }
    return 0;
}

// ── tri-roundtrip ─────────────────────────────────────────────────────────────
static int CmdTriRoundtrip(const fs::path& root) {
    auto files = CollectByExt(root, ".tri");
    std::printf("tri-roundtrip: %zu .tri files under %s\n", files.size(),
                root.string().c_str());
    std::vector<int> ok(files.size());   // 1 identical, 0 diff, -1 unsupported, -2 error
    std::vector<size_t> off(files.size());
    std::transform(std::execution::par, files.begin(), files.end(), ok.begin(),
                   [&](const fs::path& p) -> int {
        auto bytes = ReadFile(p);
        auto t = TriFile::Load(bytes);
        if (!t) return t.error().kind == NifErrorKind::UnsupportedVersion ? -1 : -2;
        auto out = t->Save();
        return (out.size() == bytes.size() &&
                std::equal(out.begin(), out.end(), bytes.begin())) ? 1 : 0;
    });
    uint64_t ident = 0, diff = 0, unsup = 0, err = 0;
    std::vector<size_t> diffFiles, errFiles;
    for (size_t i = 0; i < files.size(); ++i) {
        switch (ok[i]) {
            case 1: ++ident; break;
            case -1: ++unsup; break;
            case -2: ++err; if (errFiles.size() < 20) errFiles.push_back(i); break;
            default: ++diff; if (diffFiles.size() < 20) diffFiles.push_back(i);
        }
    }
    for (size_t i : diffFiles)
        std::printf("  BYTE-DIFF  %s\n", files[i].string().c_str());
    for (size_t i : errFiles) {
        auto t = TriFile::Load(ReadFile(files[i]));
        std::printf("  ERROR @%zu %s  %s\n", t ? 0 : t.error().offset,
                    t ? "?" : t.error().message.c_str(), files[i].string().c_str());
    }
    std::printf("\n── tri-roundtrip summary ──\n");
    std::printf("  files            %zu\n", files.size());
    std::printf("  byte-identical   %llu\n", (unsigned long long)ident);
    std::printf("  byte-diff        %llu\n", (unsigned long long)diff);
    std::printf("  not-FRTRI003     %llu\n", (unsigned long long)unsup);
    std::printf("  errors           %llu\n", (unsigned long long)err);
    std::printf("\n  GATE: %s\n", (diff == 0 && err == 0) ? "GREEN" : "RED");
    return (diff == 0 && err == 0) ? 0 : 1;
}

// ── decode ────────────────────────────────────────────────────────────────────
// Spot-check the vertex decoder: decode the first BSTriShape's geometry.
static int CmdDecode(const fs::path& file) {
    auto loaded = NifFile::LoadFile(file.string());
    if (!loaded) { std::printf("load failed: %s\n", loaded.error().message.c_str()); return 1; }
    for (auto& b : loaded->blocks) {
        auto* s = dynamic_cast<BSTriShape*>(b.get());
        if (!s) continue;
        auto L = DecodeVertexDesc(s->vertexDesc);
        auto m = DecodeMesh(*s);
        std::printf("%s: vertexSize=%u full=%d uv=%d nrm=%d tan=%d col=%d skin=%d\n",
                    std::string(s->TypeName()).c_str(), L.vertexSize, L.fullPrecision,
                    L.hasUV, L.hasNormal, L.hasTangent, L.hasColor, L.hasSkin);
        std::printf("  verts=%u tris=%zu\n", m.numVertices, m.triangles.size() / 3);
        if (m.numVertices > 0)
            std::printf("  pos[0]=(%.3f, %.3f, %.3f)  uv[0]=(%.3f, %.3f)\n",
                        m.positions[0], m.positions[1], m.positions[2],
                        m.uvs.empty() ? 0.f : m.uvs[0], m.uvs.empty() ? 0.f : m.uvs[1]);
        return 0;
    }
    std::printf("no BSTriShape in %s\n", file.string().c_str());
    return 1;
}

static int CmdCollision(const fs::path& file) {
    auto nif = niffer::NifFile::LoadFile(file.string());
    if (!nif) { std::printf("load failed\n"); return 1; }
    niffer::CollisionGeometry g = niffer::ExtractCollision(*nif);
    const size_t nTris = g.triIndices.size() / 3;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < g.triVerts.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], g.triVerts[i + k]);
            hi[k] = std::max(hi[k], g.triVerts[i + k]);
        }
    std::printf("%s\n  shapesDecoded=%d skipped=%d  tris=%zu hulls=%zu\n",
                file.filename().string().c_str(), g.shapesDecoded, g.shapesSkipped,
                nTris, g.hulls.size());
    if (nTris)
        std::printf("  AABB(skyrim units) min=(%.1f %.1f %.1f) max=(%.1f %.1f %.1f)\n",
                    lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage:\n"
                    "  niffer-tool histogram <dir>\n"
                    "  niffer-tool roundtrip <dir>\n"
                    "  niffer-tool tri-roundtrip <dir>\n"
                    "  niffer-tool dump <file.nif> [blockIdx]\n"
                    "  niffer-tool decode <file.nif>\n"
                    "  niffer-tool collision <file.nif>\n");
        return 2;
    }
    if (std::string(argv[1]) == "decode") return CmdDecode(argv[2]);
    std::string cmd = argv[1];
    fs::path arg = argv[2];
    if (cmd == "collision") return CmdCollision(arg);
    if (cmd == "histogram") return CmdHistogram(arg);
    if (cmd == "roundtrip") return CmdRoundtrip(arg);
    if (cmd == "tri-roundtrip") return CmdTriRoundtrip(arg);
    if (cmd == "dump") return CmdDump(arg, argc >= 4 ? std::atoi(argv[3]) : -1);
    std::printf("unknown command: %s\n", cmd.c_str());
    return 2;
}
