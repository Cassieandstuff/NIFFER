# NIFFER

A clean-room C++23 library for reading and writing NetImmerse/Gamebryo **NIF** files from
**Skyrim Special Edition**, plus FaceGen **TRI** morph files (FRTRI003). Initially built for
Skyrim Content Tools' Scene Editor.

- **Scope is SSE only:** NIF 20.2.0.7, user version 12, BS stream version 100. Legendary
  Edition, Fallout 4 and other games are rejected as `UnsupportedVersion`, never guessed at.
- **Byte-exact round-trip:** `NifFile::Load` followed by `Save` reproduces the input exactly.
  Block types without a typed layout pass through as raw `UnknownBlock`s and are listed on
  `NifFile::unknownBlocks`. A typed layout that disagrees with the file is demoted to raw and
  listed on `NifFile::layoutMismatches`, so a wrong layout is always loud.
- **Coverage:** the whole vanilla corpus (22,760 SSE NIFs, 569 TRIs) round-trips
  byte-identically with zero unknown blocks.
- **Decode helpers** turn disk-precision payloads into usable values: `DecodeMesh`
  (BSVertexDesc vertex unpack, skinned shapes via NiSkinPartition), `DecodeMorphDeltas` /
  `ApplyMorphs` for TRI, `ExtractCollision` for bhk collision hulls.

No dependencies beyond the standard library.

## Using it

Pull it in with CMake's FetchContent and link `niffer::niffer`:

```cmake
include(FetchContent)
FetchContent_Declare(NIFFER
    GIT_REPOSITORY https://github.com/Cassieandstuff/NIFFER.git
    GIT_TAG        main)   # pin a tag or commit for reproducible builds
FetchContent_MakeAvailable(NIFFER)

target_link_libraries(MyTool PRIVATE niffer::niffer)
```

Then include the one header, typically from your precompiled header:

```cpp
#include <niffer/Niffer.h>

auto nif = niffer::NifFile::LoadFile("meshes/clutter/barrel01.nif");
if (!nif) { /* nif.error(): kind, offset, blockIndex, message */ }
for (auto& block : nif->blocks)
    if (auto* shape = dynamic_cast<niffer::BSTriShape*>(block.get()))
        niffer::DecodedMesh mesh = niffer::DecodeMesh(*shape);
std::vector<uint8_t> bytes = nif->Save();   // == the input bytes
```

Everything lives in `namespace niffer`.

NIFFER does not set the MSVC runtime library; it inherits `CMAKE_MSVC_RUNTIME_LIBRARY` from
your project, so it links into both static-CRT SKSE plugins and dynamic-CRT desktop tools.
CMake 3.28+ and a C++23 compiler (`std::expected`) are required.

To develop against a local checkout, configure the consumer with
`-DFETCHCONTENT_SOURCE_DIR_NIFFER=<path to NIFFER>`.

## Building standalone

```
cmake -S . -B build
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo
```

| Option | Default | |
|---|---|---|
| `NIFFER_BUILD_TESTS` | ON when top-level | `niffer-test`, the synthetic self-verifying suite (registered with CTest) |
| `NIFFER_BUILD_TOOLS` | OFF | `niffer-tool`, the real-data corpus gates |

`niffer-tool` needs extracted game meshes, so it is not part of CTest:

```
niffer-tool roundtrip     <dir>         # every SSE .nif under <dir> must re-emit byte-identically
niffer-tool roundtrip --schema [--refs] <dir>   # the schema path's gate (see "The schema")
niffer-tool tri-roundtrip <dir>         # same for .tri
niffer-tool histogram     <dir>         # block-type census
niffer-tool dump          <file.nif> [blockIdx]
niffer-tool decode        <file.nif>
niffer-tool collision     <file.nif>
niffer-tool collision-validate [--sweep] <dir>  # ExtractCollision against bounds and render geometry
```

A change to any block layout must keep `roundtrip`, `roundtrip --schema` and `tri-roundtrip`
green over the full vanilla corpus.

## The schema

Block layouts are described once, as YAML, in [`schema/`](schema): one file per block class,
with inheritance, block references and their allowed targets, conditions and length fields. The
descriptor language is specified in [docs/schema-language.md](docs/schema-language.md), and the
design is in [docs/schema-design.md](docs/schema-design.md). `tools/schemagen` compiles the
schema into:

- [`schema.json`](schema.json): the whole schema, resolved into one file, for tools in any
  language;
- `include/niffer/generated/`: typed handles (`niffer::fields`) and typed views (`niffer::view`);
- `hpp/generated/Tables.h`: the tables the engine interprets.

All of these are committed, so consumers never need Python. One generic engine,
`niffer::Nif` / `niffer::Block`, interprets the tables for every block class:

```cpp
auto nif = niffer::Nif::LoadFile("meshes/clutter/barrel01.nif");
for (niffer::Block& b : nif->blocks)
    if (auto shape = niffer::view::BSTriShape::From(b)) {
        auto shader = nif->Get(shape->ShaderProperty());   // a typed ref -> a view
        shape->SetBoundRadius(2.f);                        // marks only this block dirty
    }
std::vector<uint8_t> bytes = nif->Save();   // unmodified blocks are written verbatim
```

To change the schema, you need Python 3 and PyYAML:

```
# edit schema/..., then:
cmake --build build --target niffer-schemagen            # regenerate (commit the result)
ctest --test-dir build -C RelWithDebInfo                 # includes the freshness + validation checks
niffer-tool roundtrip --schema <extracted meshes>        # the real-data gate (NIFFER_BUILD_TOOLS=ON)
```

The schema gate does more than check bytes. Every block reference is checked against its
declared target classes, the owning-reference graph must be acyclic, and every string-table
index must be in range. The byte round-trip alone can't catch a mislabeled field; these checks
can.

## Status

NIFFER is moving from hand-written typed structs (`NifFile` and the block structs in `Niffer.h`)
to the schema-driven engine. Both ship side by side until the migration completes. Today the
schema models every SSE block class the typed path does, including vertices, triangles, skin
partitions, animation keys, material values and Havok collision: 98.16% of the vanilla corpus's
block data decodes field by field. What remains opaque is mostly Havok MOPP byte code and legacy
NiTriShapeData. The schema also
corrects a few typed-path labels the new checks found ([docs/bugs.md](docs/bugs.md)). The public
API will change while the rest lands.

## Provenance

NIFFER is a clean-room implementation. Block layouts are derived from published format
documentation and verified against bytes from the shipped game. The full-corpus round-trip is
the gate, and each layout comment records the file and block it was confirmed against.

NIFFER is **not** derived from `nif.xml`, nifly, NifSkope, PyFFI or any other NIF library's
source or schema, and contributions must not be either. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

GPL-3.0-or-later; see [LICENSE](LICENSE). This applies to anything you distribute that links
NIFFER, including SKSE plugins.

NIFFER is also made available by its copyright holder under other terms, so contributions are
accepted only under a Contributor License Agreement; see [CONTRIBUTING.md](CONTRIBUTING.md).
