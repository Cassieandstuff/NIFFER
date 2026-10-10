# NIFFER — working notes

Clean-room C++23 NIF + FaceGen TRI library for Skyrim SE. Extracted from the Skyrim Content
Tools monorepo (`NIFFER/`) with fresh history. GPL-3.0-or-later publicly; the copyright holder
also uses it under other terms (dual licensing).

## Licensing and provenance (load-bearing)

- **Dual licensing depends on the copyright holder owning every line.** Never take code,
  layouts or schema from `nif.xml` (GPL-3.0), nifly (GPL-3.0), NifSkope, PyFFI or niflib, not
  even "just to check". Layouts come from shipped game bytes. Each layout comment or `verified:`
  entry cites the file and block it was confirmed against.
- **External contributions need a signed CLA** (see CONTRIBUTING.md). Never merge an
  external PR without one.
- Stay a dependency-free leaf. Any dependency must be permissively licensed.

## Public surface

- Consumers link `niffer::niffer` and `#include <niffer/Niffer.h>`, and nothing else. It is an
  umbrella, like CommonLib's `Skyrim.h`. The only other files under `include/niffer/` sit behind
  it: `Schema.h` (the schema engine) and `generated/` (generated handles and views). Never add a
  header there that consumers would include directly. Internals live in private `hpp/`
  (`"Stream.h"`, `"Registry.h"`, `"Framing.h"`, `"generated/Tables.h"`).
- Everything is in `namespace niffer` (generated: `niffer::fields`, `niffer::view`).
- Never set `CMAKE_MSVC_RUNTIME_LIBRARY`. NIFFER builds under the consumer's choice
  (static-CRT SKSE plugins and dynamic-CRT desktop tools both consume it).

## Fidelity doctrine

- **Round-trip or gate.** `Load` followed by `Save` reproduces the input byte-for-byte. Header
  facts are stored and re-emitted verbatim, never recomputed; only block sizes are re-measured
  on save. Length-prefixed strings keep their raw bytes, including any nulls.
- **One description drives both directions.** On the typed path that's one `Sync(Stream&)` per
  block. On the schema path it's one descriptor, interpreted by one engine. Never add a separate
  read or write path.
- **Unknowns are loud.** A block with no descriptor, or one whose decode doesn't consume exactly
  its declared size, stays raw and is reported (typed path: `unknownBlocks` / `layoutMismatches`;
  schema path: `Block::Modeled()` / `Refusal()`).
- **Size-match is not verification.** A wrong layout that consumes the right byte count still
  round-trips. The schema gate's semantic checks (`to:` claims, the acyclic `ref` graph, string
  ref ranges) catch mislabels the byte round-trip can't; see docs/bugs.md for what they found.
- **Raw at disk precision, decode at the edge.** Payloads the language can't yet describe stay
  `bytes`. Decode helpers (`DecodeMesh`, `DecodeMorphDeltas`, `ApplyMorphs`, `ExtractCollision`)
  are never on the round-trip path.

## Direction: schema-first

NIFFER is moving from hand-written typed structs (`Niffer.h` + `cpp/blocks/`) to a schema the
owner writes. Design and migration plan: [docs/schema-design.md](docs/schema-design.md). Spec:
[docs/schema-language.md](docs/schema-language.md). The model is ESPER's
(github.com/Cassieandstuff/ESPER).

- **Engine:** `niffer::Nif` / `niffer::Block` (`include/niffer/Schema.h`, `cpp/engine/`)
  interpret generated tables. It's one generic engine; never add per-block code to it.
- **Typed path:** still ships until migration step 6. Don't extend it; new coverage goes in
  `schema/`. It shares the file framing with the schema path (`hpp/Framing.h`).
- **No host-language escape hatches.** If the language can't express something, the language
  grows (spec + generator + engine + `tests/schema`, and a `LANGUAGE_VERSION` bump).

## Schema workflow

- **Source of truth:** `schema/` (YAML, one file per block class or struct) plus the spec. Never
  edit `include/niffer/generated/`, `hpp/generated/`, `tests/generated/` or `schema.json` by
  hand. Regenerate with the `niffer-schemagen` target (Python 3 + PyYAML) and commit them in the
  same change as the YAML. CTest's `*-fresh` checks fail on stale output.
- **Claims about data:** `to:`, `ref` vs `ptr`, and any field meaning describe the data. Add them
  only when the gate confirms them on the vanilla corpus. `roundtrip --schema --refs` lists the
  observed target classes of every ref field that has no `to:` yet.
- **New layouts are measured first:** before modeling a payload, measure it over the corpus
  (distinct selector values, sizes per selector, value shapes per word), then describe exactly
  what the data shows. Name a field from first-party code (SCT, NIFFER's helpers) or from its
  layout; anything else is `unknownN`.
- **Packed arrays:** mark large arrays `packed: true`. Read them through `Visit`, `GetAt`,
  `Count`, `Has` or the views, never through `Value::items`.
- A descriptor change must keep `roundtrip --schema` **GREEN**.

## Gates

- `ctest` (top-level build): `niffer-test` (synthetic: stream, header, TRI, decode helpers, and
  the schema engine over `tests/schema`), `niffer-schemagen-test` (generator validation), and
  the two freshness checks.
- With `-DNIFFER_BUILD_TOOLS=ON`, over the local extracted SE corpus at
  `D:/Modding/Development/Common/Vanilla Source Files/1170`:
  - `niffer-tool roundtrip <dir>`: the typed path. 22,760 of 22,761 NIFs byte-identical (the
    one other is a non-SSE version), with 0 unknown blocks and 0 layout mismatches.
  - `niffer-tool roundtrip --schema <dir>`: the schema path. It loads materialized, so forced
    re-encode proves every element, and it checks that the default packed load saves
    identically. Verbatim and forced re-encode are byte-exact; 786,940 blocks, all modeled;
    98.67% of block bytes decoded field by field; 1,098,425 refs checked against `to:`; an acyclic
    `ref` graph; 728,961 string refs in range. `--refs` also lists the remaining opaque `bytes`
    fields, largest first.
  - `niffer-tool collision-validate [--sweep] <dir>`: `ExtractCollision` over the corpus. 17,139
    leaf shapes decoded, 93 skipped; 0 of 3,174,859 compressed-mesh vertices outside their
    stored bounds; the collision AABB is within 10% of the render AABB's diagonal in 87.4% of
    the 11,094 files with both. `--sweep` re-measures each extraction convention
    (docs/collision-provenance.md).
  - `niffer-tool decode-compare <dir>`: the schema path's `DecodeShapeMesh` against the typed
    `DecodeMesh` on every shape, bit for bit. 83,178 of 83,178 equal.
  - `niffer-tool tri-roundtrip <dir>`: 569 of 569 TRIs byte-identical.
- **Bulk reads** go through `Block::ForEachElement` and the views' field-index constants
  (`view::BSVertex::kPosition`): one decode per element, no allocation in the steady state.

## Bug log — sync is mandatory

`docs/bugs.md`: symptom, what's ruled out, root cause, exact `file:line`, status
(`OPEN`/`INVESTIGATING`/`FIXED`). Add an entry when you find a bug, flip it to `FIXED` (with the
commit) when you fix one, in the same change as the code.
