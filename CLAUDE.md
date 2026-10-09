# NIFFER — working notes

Clean-room C++23 NIF + FaceGen TRI library for Skyrim SE. Extracted from the Skyrim Content
Tools monorepo (`NIFFER/`) with fresh history. GPL-3.0-or-later publicly; the copyright holder
also uses it under other terms (dual licensing).

## Licensing and provenance (load-bearing)

- **Dual licensing depends on the copyright holder owning every line.** Never take code,
  layouts or schema from `nif.xml` (GPL-3.0), nifly (GPL-3.0), NifSkope, PyFFI or niflib, not
  even "just to check". Layouts come from shipped game bytes. Each layout comment cites the
  file and block it was confirmed against.
- **External contributions need a signed CLA** (see CONTRIBUTING.md). Never merge an
  external PR without one.
- Stay a dependency-free leaf. Any dependency must be permissively licensed.

## Public surface

- Consumers link `niffer::niffer` and `#include <niffer/Niffer.h>`, and nothing else.
  `include/niffer/` holds that one header and must never gain a second file. Internals live
  in private `hpp/` (included as `"Stream.h"`, `"Registry.h"`) and are at most
  forward-declared in the public header.
- Everything is in `namespace niffer`.
- Never set `CMAKE_MSVC_RUNTIME_LIBRARY`. NIFFER builds under the consumer's choice
  (static-CRT SKSE plugins and dynamic-CRT desktop tools both consume it).

## Fidelity doctrine

- **Round-trip or gate.** `Load` followed by `Save` reproduces the input byte-for-byte. Header
  facts are stored and re-emitted verbatim, never recomputed; only block sizes are re-measured
  on save. Length-prefixed strings keep their raw bytes, including any nulls.
- **One `Sync(Stream&)` per block drives both directions.** The omnidirectional rule: never
  write a separate read or write path, so the two layouts can't drift.
- **Unknowns are loud.** Untyped blocks become `UnknownBlock` and are listed on
  `unknownBlocks`. A typed layout that disagrees with the declared block size is demoted to
  raw and listed on `layoutMismatches`, because that means NIFFER's layout is wrong.
- **Size-match is not verification.** A wrong layout that consumes the right byte count still
  round-trips, so mislabeled fields can't be caught by the gate. Example: NiAVObject's inline
  transform order (Translation, Rotation, Scale) is the reverse of the NiTransform compound's.
  Defer a block, raw, until its field meanings are verified.
- **Raw at disk precision, decode at the edge.** Vertex, key, morph and Havok payloads are
  stored raw on the round-trip path. Decode helpers (`DecodeMesh`, `DecodeMorphDeltas`,
  `ApplyMorphs`, `ExtractCollision`) turn them into usable values and are never on the
  round-trip path.

## Gates

- `niffer-test` (CTest, synthetic) proves the code.
- `niffer-tool roundtrip <dir>` and `tri-roundtrip <dir>` (`-DNIFFER_BUILD_TOOLS=ON`) prove it
  on real data. The local extracted SE corpus is at
  `D:/Modding/Development/Common/Vanilla Source Files/1170`.
- Baseline at extraction (2026-10-09): 22,760 of 22,761 NIFs byte-identical (the one other is
  a non-SSE version), 0 unknown blocks, 0 layout mismatches; 569 of 569 TRIs byte-identical.
  Every change must keep both gates GREEN and must not add unknowns or mismatches.

## Direction: schema-driven (planned)

The next phase moves NIFFER from hand-written typed structs to a schema the owner writes (one
YAML file per block or class). A generator emits the structs, `Sync` functions and reflection
tables, and its output is committed so consumers never run it. ESPER
(github.com/Cassieandstuff/ESPER) already took this path for plugin records (`schema/`,
`tools/schemagen`, `docs/schema-language.md`), so look there first. The schema must be
authored clean-room: it must not be derived from `nif.xml`.
