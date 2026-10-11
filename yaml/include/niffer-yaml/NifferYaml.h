#pragma once
// ── niffer-yaml — NIF files as YAML text ─────────────────────────────────────
// An optional add-on to NIFFER (target niffer::yaml): decompiles a NifFile to
// YAML through the schema, and compiles that YAML back to a NifFile. The core
// library stays dependency-free; this target alone uses rapidyaml (MIT,
// vendored in third_party/rapidyaml).
//
// Two modes (docs/yaml-format.md):
//   - Faithful is byte-exact: Compile(Decompile(nif)) saves the same bytes as
//     `nif`. Every field is written as stored, including counts, header
//     string-table indices and block indices, so an edit that leaves the tree
//     inconsistent (a count that no longer matches its array) is refused on
//     compile rather than repaired.
//   - Authoring is for editing by hand: blocks are keyed by an id (the block's
//     name where it is unique), refs name ids, string refs are the strings
//     themselves, and counts and sizes are left out and derived on compile, as
//     are the header's tables. Compiling gives the same file up to the string
//     table's order (and the same bytes where the file used first-use order).
// Compile reads either; the text says which.
//
//   auto text = niffer::yaml::Decompile(*nif, niffer::yaml::Mode::Authoring);
//   auto back = niffer::yaml::Compile(*text);     // std::expected<NifFile, Error>
//   if (!back) { /* back.error(): line, column, message */ }

#include <niffer/Niffer.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace niffer::yaml {

// The format version Decompile writes and Compile accepts (both modes).
inline constexpr int kFormatVersion = 1;

enum class Mode : uint8_t { Faithful, Authoring };

struct Error {
    size_t      line   = 0;   // 1-based; 0 when the error has no location
    size_t      column = 0;   // 1-based; 0 when unknown
    std::string message;
};

// The whole file as YAML. Raw blocks (no descriptor, or a refused decode) are
// written as hex. Faithful mode never fails; authoring mode refuses a file it
// can't express (a ref or string ref that points at nothing).
std::expected<std::string, Error> Decompile(const NifFile& nif, Mode mode = Mode::Faithful);

// Parses YAML of either mode. Each block is rebuilt with Block::Build, so the
// result is exactly what loading its bytes would give.
std::expected<NifFile, Error> Compile(std::string_view text,
                                      const schema::SchemaSet& set = schema::Main(),
                                      LoadMode mode = LoadMode::Packed);

}  // namespace niffer::yaml
