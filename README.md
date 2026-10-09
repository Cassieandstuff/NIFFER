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
niffer-tool tri-roundtrip <dir>         # same for .tri
niffer-tool histogram     <dir>         # block-type census
niffer-tool dump          <file.nif> [blockIdx]
niffer-tool decode        <file.nif>
niffer-tool collision     <file.nif>
```

A change to any block layout must keep `roundtrip` and `tri-roundtrip` green over the full
vanilla corpus.

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
