// ── niffer-tool — Tier-2 corpus gates ─────────────────────────────────────────
// Whitebox CLI (has the library's private hpp/ on its include path) driving the
// vanilla-corpus ratchet:
//   histogram <dir>        block-type census (header-only, any version).
//   roundtrip [--refs] <dir>
//                          the real-data gate: byte-exact verbatim and forced
//                          re-encode, no refusals, ref claims, an acyclic ref
//                          graph, string refs in range. LE/other-version files
//                          are reported as unsupported, never as failures.
//   tri-roundtrip <dir>    TriFile Load→Save over every .tri.
//   dump <file.nif>        header + per-block index/type/size; raw blocks flagged.
//   decode <file.nif>      the first BSTriShape's decoded geometry.
//   collision <file.nif>   ExtractCollision's output for one file.
//   collision-validate [--sweep] <dir>
//                          ExtractCollision against the corpus: compressed-mesh
//                          vertices inside their stored bounds, and the
//                          root-space collision AABB against the render
//                          (BSTriShape) AABB. --sweep re-measures each
//                          convention in collision::Options against the default.
//
// Build: -DNIFFER_BUILD_TOOLS=ON  →  niffer-tool.exe
#include "Collision.h"
#include "Stream.h"

#include <niffer/Niffer.h>

#include <algorithm>
#include <array>
#include <cmath>
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
// The real-data gate (docs/schema-design.md "Gates"). For every SSE file,
// against the same bytes:
//   - verbatim save and FORCED re-encode are byte-identical to the input, and
//     the default (packed) load saves identically;
//   - no block with a descriptor is refused;
//   - every ref/ptr is null or in range and satisfies its `to:` claim;
//   - every computed field holds its `compute:` claim;
//   - the `ref` graph (ptr excluded) is acyclic;
//   - every stringref is empty or in range.
// `--refs` additionally reports, per ref field without `to:`, the classes its
// targets actually have — the evidence for adding a `to:` claim.
struct SchemaFileResult {
    bool unsupported = false, malformed = false;
    bool verbatimOk = false, forcedOk = false, packedOk = false;
    size_t forcedDiverge = 0;
    std::string forcedDivergeType;
    uint64_t blocks = 0, modeled = 0;
    std::map<std::string, std::pair<uint64_t, std::string>> refusals;   // type -> (n, first reason)
    std::map<std::string, uint64_t> refViolations; // "Owner.field -> Target: why"
    std::map<std::string, uint64_t> badStringRefs; // by field
    uint64_t refsChecked = 0, claimsChecked = 0, stringRefsChecked = 0;
    uint64_t computedChecked = 0;                   // blocks whose computed fields were checked
    std::map<std::string, uint64_t> computeViolations;   // "Class: path (why)"
    uint64_t blockBytes = 0, rawBlockBytes = 0;
    std::map<std::string, uint64_t> opaqueBytes;    // `bytes` fields, by field
    std::string cycle;                              // first ref cycle's edge, "" if none
    std::string cycleAt;                            // …and where it is
    std::map<std::string, std::map<std::string, uint64_t>> refTargets;  // "Owner.field" -> target type -> n
};

// The class in `def`'s chain that declares layout field `i`.
static const schema::BlockDef* DeclaringClass(const schema::SchemaSet& s,
                                              const schema::BlockDef* def, uint16_t i) {
    while (def->parent >= 0 && i < def->ownFirst) def = &s.blocks[static_cast<size_t>(def->parent)];
    return def;
}

// Walks a block's leaf values with the "Owner.field[/sub]" name of each.
template <class Fn>
static void WalkNamed(const schema::SchemaSet& s, const schema::TypeDef& t, const Value& v,
                      const std::string& name, Fn& fn) {
    if (!v.present) return;
    switch (t.kind) {
    case schema::Kind::Struct:
        for (uint16_t i = 0; i < t.refCount && i < v.items.size(); ++i) {
            const auto& fd = s.fields[t.ref + i];
            WalkNamed(s, s.types[fd.type], v.items[i], name + "/" + fd.name, fn);
        }
        return;
    case schema::Kind::Array:
        for (const Value& item : v.items) WalkNamed(s, s.types[t.ref], item, name, fn);
        return;
    case schema::Kind::Union:
        if (v.bits < t.refCount && !v.items.empty()) {
            const auto& cd = s.cases[t.ref + v.bits];
            if (cd.type != schema::kNoField)
                WalkNamed(s, s.types[cd.type], v.items[0], name + "/" + cd.name, fn);
        }
        return;
    default:
        fn(name, t, v);
        return;
    }
}

static SchemaFileResult RoundtripOne(const fs::path& p, bool wantRefs) {
    SchemaFileResult r;
    auto bytes = ReadFile(p);
    const schema::SchemaSet& s = schema::Main();
    // Materialized: the forced re-encode below then proves every element of
    // every packed array, not just its stored bytes.
    auto nif = NifFile::Load(bytes, s, LoadMode::Materialized);
    if (!nif) {
        if (nif.error().kind == NifErrorKind::UnsupportedVersion) r.unsupported = true;
        else r.malformed = true;
        return r;
    }
    // The default (packed) load must decode the same blocks and save identically.
    if (auto packed = NifFile::Load(bytes, s, LoadMode::Packed)) {
        bool same = packed->RawCount() == nif->RawCount();
        r.packedOk = same && packed->Save(false) == bytes && packed->Save(true) == bytes;
    }

    auto verbatim = nif->Save(false);
    r.verbatimOk = verbatim == bytes;
    auto forced = nif->Save(true);
    r.forcedOk = forced == bytes;
    if (!r.forcedOk) {
        size_t n = std::min(forced.size(), bytes.size()), i = 0;
        while (i < n && forced[i] == bytes[i]) ++i;
        r.forcedDiverge = i;
        for (auto& b : nif->blocks) {   // which block re-encoded differently
            if (b.Modeled() && b.Encode(true) != std::vector<uint8_t>(b.Original().begin(), b.Original().end())) {
                r.forcedDivergeType = std::string(b.TypeName());
                break;
            }
        }
    }

    const size_t nb = nif->blocks.size();
    r.blocks = nb;
    std::vector<std::vector<std::pair<int32_t, std::string>>> refEdges(nb);
    for (size_t i = 0; i < nb; ++i) {
        const Block& b = nif->blocks[i];
        r.blockBytes += b.Original().size();
        if (!b.Modeled()) {
            r.rawBlockBytes += b.Original().size();
            if (!b.Refusal().empty()) {
                auto& e = r.refusals[std::string(b.TypeName())];
                if (e.first++ == 0) e.second = b.Refusal();
            }
            continue;
        }
        ++r.modeled;
        ++r.computedChecked;
        if (const std::string bad = Block::ComputedMismatch(s, b.TypeName(), *b.Root()); !bad.empty())
            ++r.computeViolations[std::string(b.TypeName()) + ": " + bad.substr(0, bad.find(" is "))];

        const schema::TypeDef& layout = s.types[b.Def()->type];
        for (uint16_t fi = 0; fi < layout.refCount && fi < b.Root()->items.size(); ++fi) {
            const auto& fd = s.fields[layout.ref + fi];
            const std::string owner = DeclaringClass(s, b.Def(), fi)->name;
            auto visit = [&](const std::string& name, const schema::TypeDef& t, const Value& v) {
                if (t.kind == schema::Kind::Bytes) {
                    r.opaqueBytes[name] += v.bytes.size();
                    return;
                }
                if (t.kind == schema::Kind::StringRef) {
                    const uint32_t si = static_cast<uint32_t>(v.bits);
                    if (si != 0xFFFFFFFFu && si >= nif->header.strings.size()) ++r.badStringRefs[name];
                    ++r.stringRefsChecked;
                    return;
                }
                if (t.kind != schema::Kind::Ref && t.kind != schema::Kind::Ptr) return;
                const int32_t idx = static_cast<int32_t>(static_cast<uint32_t>(v.bits));
                const bool isPtr = t.kind == schema::Kind::Ptr;
                const auto to = s.links.subspan(t.linkFirst, t.linkCount);
                if (idx == -1) return;
                ++r.refsChecked;
                if (idx < 0 || static_cast<size_t>(idx) >= nb) {
                    ++r.refViolations[name + " (in " + std::string(b.TypeName()) + "): out of range"];
                    return;
                }
                const Block& target = nif->blocks[static_cast<size_t>(idx)];
                if (!isPtr) refEdges[i].push_back({idx, name});
                if (wantRefs && to.empty()) ++r.refTargets[name][std::string(target.TypeName())];
                if (to.empty()) return;
                if (!target.Modeled()) {
                    ++r.refViolations[name + " -> " + std::string(target.TypeName()) + ": target not modeled"];
                    return;
                }
                ++r.claimsChecked;
                bool ok = false;
                for (uint16_t tt : to) ok = ok || target.IsA(tt);
                if (!ok) ++r.refViolations[name + " -> " + std::string(target.TypeName()) + ": not a declared target"];
            };
            WalkNamed(s, s.types[fd.type], b.Root()->items[fi], owner + "." + fd.name, visit);
        }
    }

    // The ref graph must be acyclic (iterative three-colour DFS).
    std::vector<uint8_t> colour(nb, 0);
    for (size_t start = 0; start < nb && r.cycle.empty(); ++start) {
        if (colour[start]) continue;
        std::vector<std::pair<size_t, size_t>> stack{{start, 0}};
        colour[start] = 1;
        while (!stack.empty() && r.cycle.empty()) {
            auto& [node, next] = stack.back();
            if (next < refEdges[node].size()) {
                const auto& edge = refEdges[node][next++];
                const size_t to = static_cast<size_t>(edge.first);
                if (colour[to] == 1) {
                    r.cycle = edge.second + " (in " + std::string(nif->blocks[node].TypeName()) + ") -> " +
                              std::string(nif->blocks[to].TypeName());
                    r.cycleAt = "[" + std::to_string(node) + "] -> [" + std::to_string(to) + "]";
                } else if (colour[to] == 0) {
                    colour[to] = 1;
                    stack.push_back({to, 0});
                }
            } else {
                colour[node] = 2;
                stack.pop_back();
            }
        }
    }
    return r;
}

static int CmdRoundtrip(const fs::path& root, bool wantRefs) {
    auto files = CollectNifs(root);
    const auto& s = schema::Main();
    std::printf("roundtrip: %zu .nif files under %s (schema %s, language %s)\n",
                files.size(), root.string().c_str(), s.schemaVersion, s.languageVersion);

    std::vector<SchemaFileResult> results(files.size());
    std::transform(std::execution::par, files.begin(), files.end(), results.begin(),
                   [&](const fs::path& p) { return RoundtripOne(p, wantRefs); });

    uint64_t checked = 0, unsupported = 0, malformed = 0, verbatimBad = 0, forcedBad = 0, packedBad = 0;
    uint64_t blocks = 0, modeled = 0, badStrings = 0, cycles = 0;
    std::map<std::string, uint64_t> badStringsByField, cyclesByEdge;
    uint64_t refsChecked = 0, claimsChecked = 0, stringRefsChecked = 0;
    uint64_t blockBytes = 0, rawBlockBytes = 0;
    std::map<std::string, uint64_t> opaqueBytes, computeViolations;
    std::map<std::string, std::string> cycleExample;
    std::map<std::string, std::pair<uint64_t, std::string>> refusals;
    std::map<std::string, uint64_t> refViolations, forcedByType;
    std::map<std::string, std::map<std::string, uint64_t>> refTargets;
    std::vector<size_t> forcedFiles;
    for (size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        if (r.unsupported) { ++unsupported; continue; }
        if (r.malformed) { ++malformed; continue; }
        ++checked;
        verbatimBad += r.verbatimOk ? 0 : 1;
        packedBad += r.packedOk ? 0 : 1;
        if (!r.forcedOk) {
            ++forcedBad;
            ++forcedByType[r.forcedDivergeType];
            if (forcedFiles.size() < 10) forcedFiles.push_back(i);
        }
        blocks += r.blocks;
        modeled += r.modeled;
        refsChecked += r.refsChecked;
        claimsChecked += r.claimsChecked;
        stringRefsChecked += r.stringRefsChecked;
        for (auto& [k, n] : r.computeViolations) computeViolations[k] += n;
        blockBytes += r.blockBytes;
        rawBlockBytes += r.rawBlockBytes;
        for (auto& [f, n] : r.opaqueBytes) opaqueBytes[f] += n;
        for (auto& [f, n] : r.badStringRefs) { badStringsByField[f] += n; badStrings += n; }
        for (auto& [t, e] : r.refusals) {
            auto& agg = refusals[t];
            if (agg.first == 0) agg.second = e.second;
            agg.first += e.first;
        }
        for (auto& [k, n] : r.refViolations) refViolations[k] += n;
        for (auto& [f, m] : r.refTargets)
            for (auto& [t, n] : m) refTargets[f][t] += n;
        if (!r.cycle.empty()) {
            ++cycles;
            if (cyclesByEdge[r.cycle]++ == 0)
                cycleExample[r.cycle] = r.cycleAt + " " + files[i].string();
        }
    }

    if (!forcedFiles.empty()) {
        std::printf("\nFORCED RE-ENCODE differs (first block type per file):\n");
        for (auto& [t, n] : forcedByType) std::printf("  %8llu  %s\n", (unsigned long long)n, t.c_str());
        for (size_t i : forcedFiles)
            std::printf("  @%zu  %s\n", results[i].forcedDiverge, files[i].string().c_str());
    }
    if (!refusals.empty()) {
        std::printf("\nREFUSED blocks (descriptor exists, decode refused):\n");
        for (auto& [t, e] : refusals)
            std::printf("  %8llu  %s — %s\n", (unsigned long long)e.first, t.c_str(), e.second.c_str());
    }
    if (!refViolations.empty()) {
        std::printf("\nREF VIOLATIONS:\n");
        for (auto& [k, n] : refViolations) std::printf("  %8llu  %s\n", (unsigned long long)n, k.c_str());
    }
    if (!cyclesByEdge.empty()) {
        std::printf("\nREF CYCLES by closing edge (a `ref` that should be a `ptr`):\n");
        for (auto& [e, n] : cyclesByEdge)
            std::printf("  %8llu  %s\n            e.g. %s\n", (unsigned long long)n, e.c_str(),
                        cycleExample[e].c_str());
    }
    if (!computeViolations.empty()) {
        std::printf("\nCOMPUTE VIOLATIONS (a `compute:` claim that doesn't hold), first per block:\n");
        for (auto& [k, n] : computeViolations) std::printf("  %8llu  %s\n", (unsigned long long)n, k.c_str());
    }
    if (!badStringsByField.empty()) {
        std::printf("\nBAD STRINGREFS by field:\n");
        for (auto& [f, n] : badStringsByField) std::printf("  %8llu  %s\n", (unsigned long long)n, f.c_str());
    }
    if (wantRefs && !refTargets.empty()) {
        std::printf("\nREF TARGETS of fields without `to:` (evidence for `to:` claims):\n");
        for (auto& [f, m] : refTargets) {
            std::printf("  %s\n", f.c_str());
            for (auto& [t, n] : m) std::printf("      %10llu  %s\n", (unsigned long long)n, t.c_str());
        }
    }

    uint64_t opaque = 0;
    for (auto& [f, n] : opaqueBytes) opaque += n;
    if (wantRefs && !opaqueBytes.empty()) {
        std::vector<std::pair<std::string, uint64_t>> v(opaqueBytes.begin(), opaqueBytes.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::printf("\nOPAQUE `bytes` fields (largest first):\n");
        for (auto& [f, n] : v)
            if (n) std::printf("  %12llu  %s\n", (unsigned long long)n, f.c_str());
    }

    uint64_t refused = 0, violations = 0, computeBad = 0;
    for (auto& [t, e] : refusals) refused += e.first;
    for (auto& [k, n] : computeViolations) computeBad += n;
    for (auto& [k, n] : refViolations) violations += n;

    std::printf("\n── roundtrip summary ──\n");
    std::printf("  files checked     %llu  (unsupported-ver %llu, malformed %llu)\n",
                (unsigned long long)checked, (unsigned long long)unsupported, (unsigned long long)malformed);
    std::printf("  verbatim diff     %llu\n", (unsigned long long)verbatimBad);
    std::printf("  forced diff       %llu\n", (unsigned long long)forcedBad);
    std::printf("  packed-load diff  %llu\n", (unsigned long long)packedBad);
    std::printf("  blocks            %llu  modeled %llu (%.2f%%)\n",
                (unsigned long long)blocks, (unsigned long long)modeled,
                blocks ? 100.0 * double(modeled) / double(blocks) : 0.0);
    auto pct = [&](uint64_t n) { return blockBytes ? 100.0 * double(n) / double(blockBytes) : 0.0; };
    std::printf("  block bytes       %llu: decoded %.2f%%, opaque `bytes` %.2f%%, raw blocks %.2f%%\n",
                (unsigned long long)blockBytes, pct(blockBytes - opaque - rawBlockBytes), pct(opaque),
                pct(rawBlockBytes));
    std::printf("  refused           %llu\n", (unsigned long long)refused);
    std::printf("  refs              %llu non-null, %llu checked against `to:`\n",
                (unsigned long long)refsChecked, (unsigned long long)claimsChecked);
    std::printf("  ref violations    %llu\n", (unsigned long long)violations);
    std::printf("  ref cycles        %llu files\n", (unsigned long long)cycles);
    std::printf("  computed fields   %llu blocks violate a `compute:` claim\n", (unsigned long long)computeBad);
    std::printf("  stringrefs        %llu checked, %llu bad\n",
                (unsigned long long)stringRefsChecked, (unsigned long long)badStrings);

    const bool green = malformed == 0 && verbatimBad == 0 && forcedBad == 0 && packedBad == 0 && refused == 0 &&
                       violations == 0 && cycles == 0 && badStrings == 0 && computeBad == 0;
    std::printf("\n  GATE: %s\n", green ? "GREEN" : "RED");
    return green ? 0 : 1;
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
// `dump <file> <blockIdx>` — additionally hexdump that block's bytes (an RE aid:
//                            works for modeled and raw blocks alike).
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
        const Block& b = f.blocks[i];
        std::string note;
        if (!b.Modeled())
            note = b.Refusal().empty() ? "   (raw: no descriptor)" : "   (REFUSED: " + std::string(b.Refusal()) + ")";
        std::printf("  [%3zu] size=%-6u %s%s\n", i, f.header.blockSizes[i],
                    std::string(b.TypeName()).c_str(), note.c_str());
    }
    if (hexBlock >= 0 && static_cast<size_t>(hexBlock) < f.blocks.size()) {
        const Block& b = f.blocks[static_cast<size_t>(hexBlock)];
        const std::vector<uint8_t> body(b.Original().begin(), b.Original().end());
        std::printf("\nblock [%d] %s  (%zu bytes):\n", hexBlock,
                    std::string(b.TypeName()).c_str(), body.size());
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
        auto s = view::BSTriShape::From(b);
        if (!s) continue;
        auto L = DecodeVertexDesc(s->VertexDesc());
        auto m = DecodeShapeMesh(*loaded, b);
        std::printf("%s: vertexSize=%u full=%d uv=%d nrm=%d tan=%d col=%d skin=%d\n",
                    std::string(b.TypeName()).c_str(), L.vertexSize, L.fullPrecision,
                    L.hasUV, L.hasNormal, L.hasTangent, L.hasColor, L.hasSkin);
        std::printf("  verts=%u tris=%zu\n", m.numVertices, m.triangles.size() / 3);
        if (!m.positions.empty())
            std::printf("  pos[0]=(%.3f, %.3f, %.3f)  uv[0]=(%.3f, %.3f)\n",
                        m.positions[0], m.positions[1], m.positions[2],
                        m.uvs.empty() ? 0.f : m.uvs[0], m.uvs.empty() ? 0.f : m.uvs[1]);
        return 0;
    }
    std::printf("no BSTriShape in %s\n", file.string().c_str());
    return 1;
}

// ── collision-validate ────────────────────────────────────────────────────────
// Per file: the root-space AABB of ExtractCollision's output against the
// root-space AABB of the render geometry (every BSTriShape with its own
// vertices, through its node chain). `err` is the largest distance between
// corresponding AABB faces, relative to the render AABB's diagonal.
namespace {
struct Aabb {
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    bool  empty = true;
    void Add(const float* p) {
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
        empty = false;
    }
};
Aabb CollisionAabb(const CollisionGeometry& g) {
    Aabb a;
    for (size_t i = 0; i + 2 < g.triVerts.size(); i += 3) a.Add(&g.triVerts[i]);
    for (auto& h : g.hulls)
        for (size_t i = 0; i + 2 < h.size(); i += 3) a.Add(&h[i]);
    return a;
}
double AabbErr(const Aabb& c, const Aabb& r) {
    double d2 = 0, worst = 0;
    for (int k = 0; k < 3; ++k) d2 += double(r.hi[k] - r.lo[k]) * double(r.hi[k] - r.lo[k]);
    const double diag = std::sqrt(d2);
    for (int k = 0; k < 3; ++k)
        worst = std::max({ worst, std::fabs(double(c.lo[k] - r.lo[k])), std::fabs(double(c.hi[k] - r.hi[k])) });
    return diag > 0 ? worst / diag : 1e9;
}
struct Variant { const char* name; collision::Options opt; };
std::vector<Variant> Variants(bool sweep) {
    std::vector<Variant> v{ { "default", {} } };
    if (!sweep) return v;
    collision::Options o;
    o = {}; o.niRowMajor = !o.niRowMajor;         v.push_back({ "NiAVObject matrix transposed", o });
    o = {}; o.havokColumns = !o.havokColumns;     v.push_back({ "HavokTransform basis as the other axis", o });
    o = {}; o.quatConjugate = !o.quatConjugate;   v.push_back({ "rigid-body quaternion conjugated", o });
    o = {}; o.bodyTransform = 0;                  v.push_back({ "rigid-body transform never applied", o });
    o = {}; o.bodyTransform = 2;                  v.push_back({ "rigid-body transform always applied", o });
    o = {}; o.nodeTransform = !o.nodeTransform;   v.push_back({ "node placement toggled", o });
    o = {}; o.expandByRadius = !o.expandByRadius; v.push_back({ "box/convex radius expansion toggled", o });
    o = {}; o.cylinderRadius = 28;                v.push_back({ "cylinders decoded, radius word 28", o });
    o = {}; o.cylinderRadius = 48;                v.push_back({ "cylinders decoded, radius word 48", o });
    return v;
}
struct ValidateResult {
    bool loaded = false, hasCollision = false;
    std::vector<double> err;                 // per variant; -1 when not comparable
    double sizeRatio = -1;                   // default variant: collision / render AABB size, mean over axes
    collision::Diagnostics diag;             // default variant
    int decoded = 0, skipped = 0;
    std::string topClass;                    // "<body class>/<shape class>" of the first body
};
}  // namespace

static ValidateResult ValidateOne(const fs::path& p, const std::vector<Variant>& variants) {
    ValidateResult r;
    auto bytes = ReadFile(p);
    auto nif = NifFile::Load(bytes);
    if (!nif) return r;
    r.loaded = true;

    std::vector<std::pair<size_t, DecodedMesh>> meshes;
    for (size_t i = 0; i < nif->blocks.size(); ++i)
        if (auto s = view::BSTriShape::From(nif->blocks[i])) {
            DecodedMesh m = DecodeMesh(*s);
            if (!m.positions.empty()) meshes.emplace_back(i, std::move(m));
        }
    for (size_t vi = 0; vi < variants.size(); ++vi) {
        const auto& opt = variants[vi].opt;
        collision::Diagnostics d;
        CollisionGeometry g = collision::Extract(*nif, opt, vi == 0 ? &d : nullptr);
        if (vi == 0) {
            r.diag = d;
            r.decoded = g.shapesDecoded;
            r.skipped = g.shapesSkipped;
            r.hasCollision = !g.triIndices.empty() || !g.hulls.empty();
        }
        const auto wt = collision::ComputeWorldTransforms(*nif, opt);
        Aabb ra;
        for (auto& [bi, m] : meshes) {
            if (!wt.valid[bi]) continue;
            for (size_t k = 0; k + 2 < m.positions.size(); k += 3) {
                const auto q = collision::Apply(wt.world[bi], { m.positions[k], m.positions[k + 1], m.positions[k + 2] });
                ra.Add(q.data());
            }
        }
        const Aabb ca = CollisionAabb(g);
        r.err.push_back(!ca.empty && !ra.empty ? AabbErr(ca, ra) : -1.0);
        if (vi == 0 && !ca.empty && !ra.empty) {
            double sum = 0; int n = 0;
            for (int k = 0; k < 3; ++k)
                if (ra.hi[k] - ra.lo[k] > 1.f) { sum += double(ca.hi[k] - ca.lo[k]) / double(ra.hi[k] - ra.lo[k]); ++n; }
            if (n) r.sizeRatio = sum / n;
        }
    }
    for (auto& b : nif->blocks) {
        auto rb = view::bhkRigidBody::From(b);
        if (!rb) continue;
        const Block* s = nif->GetBlock(rb->Shape().index);
        r.topClass = std::string(b.TypeName()) + "/" + (s ? std::string(s->TypeName()) : "?");
        break;
    }
    return r;
}

static int CmdCollisionValidate(const fs::path& root, bool sweep) {
    auto files = CollectNifs(root);
    const auto variants = Variants(sweep);
    std::printf("collision-validate: %zu .nif files under %s (%zu variant%s)\n", files.size(),
                root.string().c_str(), variants.size(), variants.size() == 1 ? "" : "s");
    std::vector<ValidateResult> results(files.size());
    std::transform(std::execution::par, files.begin(), files.end(), results.begin(),
                   [&](const fs::path& p) { return ValidateOne(p, variants); });

    uint64_t withCollision = 0, compared = 0, decoded = 0, skipped = 0, phantoms = 0;
    uint64_t cmsChecked = 0, cmsOutside = 0, moppChecked = 0, moppOutside = 0;
    std::map<std::string, uint64_t> decodedBy, skippedBy;
    std::map<std::string, std::array<uint64_t, 4>> byClass;   // n, <=5%, <=10%, <=25%
    std::map<std::string, std::vector<double>> ratioByClass;
    std::vector<double> errs;
    std::vector<std::array<uint64_t, 3>> pair(variants.size());   // changed, better, worse
    std::vector<std::array<uint64_t, 3>> pairMesh(variants.size());   // the same, compressed-mesh files only
    for (auto& r : results) {
        if (!r.loaded) continue;
        decoded += static_cast<uint64_t>(r.decoded);
        skipped += static_cast<uint64_t>(r.skipped);
        phantoms += static_cast<uint64_t>(r.diag.phantomsIgnored);
        cmsChecked += static_cast<uint64_t>(r.diag.cmsVerticesChecked);
        cmsOutside += static_cast<uint64_t>(r.diag.cmsVerticesOutside);
        moppChecked += static_cast<uint64_t>(r.diag.moppChecked);
        moppOutside += static_cast<uint64_t>(r.diag.moppOutside);
        for (auto& [k, n] : r.diag.decoded) decodedBy[k] += static_cast<uint64_t>(n);
        for (auto& [k, n] : r.diag.skipped) skippedBy[k] += static_cast<uint64_t>(n);
        if (!r.hasCollision) continue;
        ++withCollision;
        if (r.err.empty() || r.err[0] < 0) continue;
        ++compared;
        const double e = r.err[0];
        errs.push_back(e);
        auto& c = byClass[r.topClass];
        ++c[0]; c[1] += e <= 0.05; c[2] += e <= 0.10; c[3] += e <= 0.25;
        if (r.sizeRatio > 0) ratioByClass[r.topClass].push_back(r.sizeRatio);
        for (size_t v = 1; v < variants.size(); ++v) {
            if (r.err[v] < 0 || std::fabs(r.err[v] - e) < 1e-4) continue;
            ++pair[v][0];
            if (r.err[v] < e) ++pair[v][1]; else ++pair[v][2];
            if (r.topClass.find("bhkMoppBvTreeShape") != std::string::npos) {
                ++pairMesh[v][0];
                if (r.err[v] < e) ++pairMesh[v][1]; else ++pairMesh[v][2];
            }
        }
    }
    std::sort(errs.begin(), errs.end());
    auto rate = [&](double t) {
        size_t n = 0;
        for (double e : errs) n += e <= t;
        return errs.empty() ? 0.0 : 100.0 * double(n) / double(errs.size());
    };
    std::printf("\nShapes decoded by class:\n");
    for (auto& [k, n] : decodedBy) std::printf("  %8llu  %s\n", (unsigned long long)n, k.c_str());
    std::printf("Shapes skipped by reason:\n");
    for (auto& [k, n] : skippedBy) std::printf("  %8llu  %s\n", (unsigned long long)n, k.c_str());
    std::printf("\nAgreement with render bounds by first body/shape (n, <=5%%, <=10%%, <=25%% of the render diagonal,"
                " median collision/render AABB size ratio):\n");
    for (auto& [k, c] : byClass) {
        auto& rv = ratioByClass[k];
        std::sort(rv.begin(), rv.end());
        std::printf("  %6llu  %5.1f%%  %5.1f%%  %5.1f%%  %5.2f  %s\n", (unsigned long long)c[0],
                    100.0 * double(c[1]) / double(c[0]), 100.0 * double(c[2]) / double(c[0]),
                    100.0 * double(c[3]) / double(c[0]), rv.empty() ? 0.0 : rv[rv.size() / 2], k.c_str());
    }
    if (variants.size() > 1) {
        std::printf("\nConvention sweep (files whose AABB error changes: variant better / worse than default):\n");
        for (size_t v = 1; v < variants.size(); ++v)
            std::printf("  %6llu changed  %6llu better  %6llu worse   %s\n", (unsigned long long)pair[v][0],
                        (unsigned long long)pair[v][1], (unsigned long long)pair[v][2], variants[v].name);
        std::printf("\n  ...restricted to files whose first body holds a bhkMoppBvTreeShape (mesh collision, which\n"
                    "  should follow the render geometry closely):\n");
        for (size_t v = 1; v < variants.size(); ++v)
            std::printf("  %6llu changed  %6llu better  %6llu worse   %s\n", (unsigned long long)pairMesh[v][0],
                        (unsigned long long)pairMesh[v][1], (unsigned long long)pairMesh[v][2], variants[v].name);
    }
    std::printf("\n── collision-validate summary ──\n");
    std::printf("  shapes            %llu decoded, %llu skipped; %llu phantom bodies ignored\n",
                (unsigned long long)decoded, (unsigned long long)skipped, (unsigned long long)phantoms);
    std::printf("  bounds            %llu compressed-mesh vertices checked, %llu outside their data bounds\n",
                (unsigned long long)cmsChecked, (unsigned long long)cmsOutside);
    std::printf("  mopp grid         %llu checked, %llu with data bounds outside origin..origin+254*65536/scale\n",
                (unsigned long long)moppChecked, (unsigned long long)moppOutside);
    std::printf("  files             %llu with collision, %llu also with render geometry\n",
                (unsigned long long)withCollision, (unsigned long long)compared);
    std::printf("  render agreement  <=5%%: %.1f%%  <=10%%: %.1f%%  <=25%%: %.1f%%  median err %.3f\n",
                rate(0.05), rate(0.10), rate(0.25), errs.empty() ? 0.0 : errs[errs.size() / 2]);
    return 0;
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
                    "  niffer-tool roundtrip [--refs] <dir>\n"
                    "  niffer-tool tri-roundtrip <dir>\n"
                    "  niffer-tool dump <file.nif> [blockIdx]\n"
                    "  niffer-tool decode <file.nif>\n"
                    "  niffer-tool collision <file.nif>\n"
                    "  niffer-tool collision-validate [--sweep] <dir>\n");
        return 2;
    }
    if (std::string(argv[1]) == "decode") return CmdDecode(argv[2]);
    if (std::string(argv[1]) == "roundtrip") {
        bool refs = std::string(argv[2]) == "--refs";
        if (argc < (refs ? 4 : 3)) { std::printf("roundtrip --refs needs a directory\n"); return 2; }
        return CmdRoundtrip(argv[refs ? 3 : 2], refs);
    }
    if (std::string(argv[1]) == "collision-validate") {
        bool sweep = argc >= 4 && std::string(argv[2]) == "--sweep";
        return CmdCollisionValidate(argv[sweep ? 3 : 2], sweep);
    }
    std::string cmd = argv[1];
    fs::path arg = argv[2];
    if (cmd == "collision") return CmdCollision(arg);
    if (cmd == "histogram") return CmdHistogram(arg);
    if (cmd == "tri-roundtrip") return CmdTriRoundtrip(arg);
    if (cmd == "dump") return CmdDump(arg, argc >= 4 ? std::atoi(argv[3]) : -1);
    std::printf("unknown command: %s\n", cmd.c_str());
    return 2;
}
