#pragma once
// ── niffer-yaml — NIF files as YAML text ─────────────────────────────────────
// An optional add-on to NIFFER (target niffer::yaml): decompiles a NifFile to
// YAML through the schema, and compiles that YAML back to a NifFile. The core
// library stays dependency-free; this target alone uses rapidyaml (MIT,
// vendored in third_party/rapidyaml).
//
// Faithful mode (the only mode so far) is byte-exact: Compile(Decompile(nif))
// saves the same bytes as `nif`. Every field is written as stored, including
// counts, header string-table indices and block indices, so an edit that
// leaves the tree inconsistent (a count that no longer matches its array) is
// refused on compile rather than repaired. The format is specified in
// docs/yaml-format.md.
//
//   std::string text = niffer::yaml::Decompile(*nif);
//   auto back = niffer::yaml::Compile(text);      // std::expected<NifFile, Error>
//   if (!back) { /* back.error(): line, column, message */ }

#include <niffer/Niffer.h>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace niffer::yaml {

// The format version Decompile writes and Compile accepts.
inline constexpr int kFormatVersion = 1;

struct Error {
    size_t      line   = 0;   // 1-based; 0 when the error has no location
    size_t      column = 0;   // 1-based; 0 when unknown
    std::string message;
};

// The whole file as faithful-mode YAML. Raw blocks (no descriptor, or a
// refused decode) are written as hex.
std::string Decompile(const NifFile& nif);

// Parses faithful-mode YAML. Each block is rebuilt with Block::Build, so the
// result is exactly what loading its bytes would give.
std::expected<NifFile, Error> Compile(std::string_view text,
                                      const schema::SchemaSet& set = schema::Main(),
                                      LoadMode mode = LoadMode::Packed);

}  // namespace niffer::yaml
