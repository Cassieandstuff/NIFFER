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
- Stay a dependency-free leaf. Any dependency must be permissively licensed and live in an
  optional target, never the core. The one so far: rapidyaml 0.15.2 (MIT), vendored in
  `third_party/rapidyaml` for `niffer::yaml` only. Third-party code keeps its own license and
  isn't the owner's; never mix it into NIFFER's own sources.

## Public surface

- Consumers link `niffer::niffer` and `#include <niffer/Niffer.h>`, and nothing else. It is an
  umbrella, like CommonLib's `Skyrim.h`. The only other files under `include/niffer/` sit behind
  it: `Schema.h` (the schema engine) and `generated/` (generated handles and views). Never add a
  header there that consumers would include directly. Internals live in private `hpp/`
  (`"Stream.h"`, `"Framing.h"`, `"Collision.h"`, `"generated/Tables.h"`).
- Everything is in `namespace niffer` (generated: `niffer::fields`, `niffer::view`).
- **Optional `niffer::yaml`** (`yaml/`, `-DNIFFER_BUILD_YAML`): NIF ↔ YAML text. Its consumers
  include `<niffer-yaml/NifferYaml.h>`, which lives outside `include/niffer/`. Format:
  [docs/yaml-format.md](docs/yaml-format.md). Like the engine it is generic over the schema;
  never add per-block code to it.
- Never set `CMAKE_MSVC_RUNTIME_LIBRARY`. NIFFER builds under the consumer's choice
  (static-CRT SKSE plugins and dynamic-CRT desktop tools both consume it).

## Fidelity doctrine

- **Round-trip or gate.** `Load` followed by `Save` reproduces the input byte-for-byte. Header
  facts are stored and re-emitted verbatim, never recomputed; only block sizes are re-measured
  on save. Length-prefixed strings keep their raw bytes, including any nulls.
- **One description drives both directions.** A block class is one descriptor, interpreted by
  one engine; the header, footer and TRI are one `Sync(Stream&)` each. Never add a separate read
  or write path.
- **Unknowns are loud.** A block with no descriptor, or one whose decode doesn't consume exactly
  its declared size, stays raw and is reported (`Block::Modeled()` / `Block::Refusal()`).
- **Size-match is not verification.** A wrong layout that consumes the right byte count still
  round-trips. The schema gate's semantic checks (`to:` claims, the acyclic `ref` graph, string
  ref ranges) catch mislabels the byte round-trip can't; see docs/bugs.md for what they found.
- **Raw at disk precision, decode at the edge.** Payloads the language can't yet describe stay
  `bytes`. Decode helpers (`DecodeMesh`, `DecodeMorphDeltas`, `ApplyMorphs`, `ExtractCollision`)
  are never on the round-trip path.

## Schema-driven

Block layouts are a schema the owner writes; there are no hand-written block structs (they
were deleted in migration step 6). Design and migration history:
[docs/schema-design.md](docs/schema-design.md). Spec:
[docs/schema-language.md](docs/schema-language.md). The model is ESPER's
(github.com/Cassieandstuff/ESPER).

- **Engine:** `niffer::NifFile` / `niffer::Block` (`include/niffer/Schema.h`, `cpp/engine/`)
  interpret generated tables. It's one generic engine; never add per-block code to it.
- **Framing** (header, block windows, footer) is `hpp/Framing.h` over `hpp/Stream.h`.
- **No host-language escape hatches.** If the language can't express something, the language
  grows (spec + generator + engine + `tests/schema`, and a `LANGUAGE_VERSION` bump).

## Schema workflow

- **Source of truth:** `schema/` (YAML, one file per block class or struct) plus the spec. Never
  edit `include/niffer/generated/`, `hpp/generated/`, `tests/generated/` or `schema.json` by
  hand. Regenerate with the `niffer-schemagen` target (Python 3 + PyYAML) and commit them in the
  same change as the YAML. CTest's `*-fresh` checks fail on stale output.
- **Claims about data:** `to:`, `ref` vs `ptr`, `compute:`, and any field meaning describe the
  data. Add them only when the gate confirms them on the vanilla corpus. `roundtrip --refs` lists the
  observed target classes of every ref field that has no `to:` yet.
- **New layouts are measured first:** before modeling a payload, measure it over the corpus
  (distinct selector values, sizes per selector, value shapes per word), then describe exactly
  what the data shows. Name a field from first-party code (SCT, NIFFER's helpers) or from its
  layout; anything else is `unknownN`.
- **Packed arrays:** mark large arrays `packed: true`. Read them through `Visit`, `GetAt`,
  `Count`, `Has` or the views, never through `Value::items`.
- A descriptor change must keep `roundtrip` **GREEN**.

## Gates

- `ctest` (top-level build): `niffer-test` (synthetic: stream, header, TRI, decode helpers, and
  the schema engine over `tests/schema`), `niffer-yaml-test` (decompile/compile on a hand-built
  file), `niffer-schemagen-test` (generator validation), and the two freshness checks.
- With `-DNIFFER_BUILD_TOOLS=ON`, over the local extracted SE corpus at
  `D:/Modding/Development/Common/Vanilla Source Files/1170`:
  - `niffer-tool roundtrip <dir>`: the real-data gate. It loads materialized, so forced
    re-encode proves every element, and it checks that the default packed load saves
    identically. Verbatim and forced re-encode are byte-exact; 786,940 blocks, all modeled;
    98.67% of block bytes decoded field by field; 1,098,425 refs checked against `to:`; an acyclic
    `ref` graph; 728,961 string refs in range; every `compute:` claim holds. `--refs` also lists the remaining opaque `bytes`
    fields, largest first.
  - `niffer-tool collision-validate [--sweep] <dir>`: `ExtractCollision` over the corpus. 17,139
    leaf shapes decoded, 93 skipped; 0 of 3,174,859 compressed-mesh vertices outside their
    stored bounds; the collision AABB is within 10% of the render AABB's diagonal in 87.4% of
    the 11,096 files with both. `--sweep` re-measures each extraction convention
    (docs/collision-provenance.md).
  - `niffer-tool tri-roundtrip <dir>`: 569 of 569 TRIs byte-identical.
- With `-DNIFFER_BUILD_YAML=ON` (the default at top level), over the same corpus:
  - `niffer-yaml roundtrip <dir>` (faithful): decompile → compile → save. 22,760 of 22,760
    NIFs byte-identical with the same blocks modeled, none refused; 14.0 GB of YAML for 2.3 GB
    of NIF, about 2 minutes.
  - `niffer-yaml roundtrip --authoring <dir>`: the same file up to the string table's order in
    all 22,760 (21,009 byte-identical; the other 1,751 order their string tables differently),
    and decompiling the result gives the same text. About 3 minutes.
  - A change to the language, the engine or `yaml/` must keep both GREEN.
- **Bulk reads** go through `Block::ForEachElement` and the views' field-index constants
  (`view::BSVertex::kPosition`): one decode per element, no allocation in the steady state.

## Bug log — sync is mandatory

`docs/bugs.md`: symptom, what's ruled out, root cause, exact `file:line`, status
(`OPEN`/`INVESTIGATING`/`FIXED`). Add an entry when you find a bug, flip it to `FIXED` (with the
commit) when you fix one, in the same change as the code.
