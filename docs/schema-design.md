# NIFFER — schema-first design

Status: **steps 1–6 implemented** (2026-10-10). The normative spec is
[schema-language.md](schema-language.md). The model follows ESPER's
(github.com/Cassieandstuff/ESPER, `docs/schema-design.md`), which settled the same questions for
plugin records; this document records where NIF differs.

## What NIFFER becomes

NIFFER becomes a clean-room, **language-neutral schema of the Skyrim SE NIF format**. The C++
library in the same repo is the schema's reference implementation and the thing that verifies
it.

| Layer | What it is | Who uses it |
|---|---|---|
| **Schema** | the descriptor-language spec, the `schema/` YAML descriptors (one file per block class), and a generated `schema.json` bundle | anyone, in any language |
| **Generated C++** | field handles, typed views and engine tables, generated from the schema and committed | the engine and C++ consumers; they never touch YAML |
| **`niffer::niffer`** | the engine (`niffer::NifFile`, `niffer::Block`), the decode helpers, and the gate tools | C++ consumers |

**One repo:** a schema change and the real-data run that proves it land in the same commit. NIFFER
is clean-room, so that gate is the schema's only check. No layout is ever taken from `nif.xml`,
nifly, NifSkope, PyFFI or niflib.

```
schema/**/*.yaml ──► tools/schemagen ──┬─► schema.json                            (data consumers)
   (authored)        (Python, PyYAML)  ├─► include/niffer/generated/Fields.h      (public handles)
                                       ├─► include/niffer/generated/Views.h       (public typed views)
                                       └─► hpp/generated/Tables.h                 (engine tables, private)
                                                        │
                               niffer::NifFile / niffer::Block (engine) ◄──┘
```

## Repo layout

```
schema/
  manifest.yaml
  structs/Vec3.yaml            reusable byte layouts, one per file
  blocks/<area>/NiNode.yaml    one file per block class (nodes, geometry, shaders, skin,
                               anim, collision, particles, extra)
schema.json                    generated: the whole schema, resolved, in one file
docs/schema-language.md        the spec (normative)
tools/schemagen/               generator + validator (Python 3, PyYAML)
include/niffer/Niffer.h        the umbrella: the only header consumers include
include/niffer/Schema.h        engine API (behind the umbrella)
include/niffer/generated/      generated handles + views (behind the umbrella)
hpp/generated/Tables.h         generated engine tables (private)
cpp/engine/                    the engine
tests/schema, tests/generated  a test-only schema exercising every construct, and its output
```

Consumers still write `#include <niffer/Niffer.h>` and nothing else. The files under
`include/niffer/` other than `Niffer.h` exist only behind that umbrella, as CommonLib's
`RE/` headers sit behind `Skyrim.h`.

## How NIF differs from plugin records

| ESPER (plugin records) | NIFFER (NIF blocks) |
|---|---|
| a record is a stream of tagged subrecords; members match by signature | a block is one flat layout; its size is declared in the header |
| groups, the ordinal rule, signature remapping | none |
| no inheritance | blocks inherit (NiNode → NiAVObject → NiObjectNET → NiObject); the layout is the ancestors' fields followed by the block's own |
| `formid` + `to:` (record types) | `ref` / `ptr` + `to:` (block classes, matched by inheritance) |
| `lstring` (localized) | `stringref` into the header string table, `sizedstring` inline |
| breaks for truncated forms | none: SSE only, one version |
| counts are mostly prefixed | counts and sizes are often earlier fields (`numVertices`, `dataSize`) and conditions read flags, sometimes on an enclosing struct |

Exact accounting is simpler here: a block must decode to exactly its declared size, or it stays
raw.

## Engine

- **Container:** `niffer::NifFile` loads a file: the header (parsed by the hand-written
  `Header::Sync`, as ESPER keeps its container code in C++), one `niffer::Block` per block, the
  root refs, and any trailing bytes. `Save(force)` writes it back.
- **Block:** holds its type name, original bytes, definition (none if raw), decoded value tree
  and a dirty bit. Encode emits the original bytes unless the block is dirty or the encode is
  forced. Values are decoded when the block loads, so refusal is known immediately.
- **Value kinds:** integers, `f32` (raw bits), block refs, string refs, strings, bytes, structs,
  arrays. Conditional fields can be absent.
- **Access:** generated handles (`niffer::fields::NiNode::children`), string paths
  (`"transform/translation/x"`) for tools, and generated typed views:

  ```cpp
  niffer::NifFile nif = *niffer::NifFile::LoadFile(path);
  for (niffer::Block& b : nif.blocks)
      if (auto shape = niffer::view::BSTriShape::From(b)) {
          niffer::NiRef<niffer::view::NiObject> shader = shape->ShaderProperty();
          shape->SetBoundRadius(2.f);           // marks the block dirty
      }
  std::vector<uint8_t> out = nif.Save();       // only the dirty block is re-encoded
  ```

- **References:** `ForEachRef` visits every visible `ref`/`ptr`. A future step adds block
  insertion and deletion. Like ESPER's FormID remap, it will refuse when a raw block or opaque
  bytes could hide references.

## Gates

- `niffer-test` (CTest): the synthetic suite, including the schema engine over `tests/schema`.
- `niffer-tool roundtrip <dir>` (real data). For every SSE file:
  - verbatim save and **forced** re-encode are both byte-identical to the input, and so is the
    default packed load's save;
  - every block with a descriptor decodes (no refusals);
  - every `ref`/`ptr` is null or in range, and every `to:` claim holds;
  - the `ref` graph is acyclic;
  - every `stringref` is empty or in range.
- `niffer-tool tri-roundtrip <dir>` stays as it is until TRI joins the schema.

## Performance

A generic value tree costs more than a hand-written struct. Fully materialized, a skinned body
mesh (`femalebody_1.nif`, 192 KB) becomes a 12.3 MB tree (64×) and loads in 5.1 ms, against
0.5 ms on the typed path. **Packed arrays** (`packed: true`, spec §4.4) keep vertices,
triangles, skin tables and keys as their encoded bytes, and decode an element only when a path
reaches it. The same mesh then holds a 0.36 MB tree (1.9× its bytes) and loads in 2.4 ms. It's
a general storage mode in the engine, not per-block code:

- `LoadMode::Packed` is the default. `LoadMode::Materialized` builds every element, and the gate
  uses it so the forced re-encode proves each element's layout. The gate also checks that the
  packed load saves identically.
- Reads go through `Block::Visit` / `GetAt` / `Count` / `Has` and the views, which decode just
  the element a path reaches. `GetValue` returns tree pointers only, so it stops at a packed
  array. Writing into a packed array unpacks that array first.
- **Bulk reads:** `Block::ForEachElement` decodes each element of an array exactly once, into
  one reused scratch value (decoding overwrites a value in place, so the steady state allocates
  nothing). Readers index an element with the views' generated field-index constants
  (`view::BSVertex::kPosition`). Per-attribute view accessors remain the convenient path for
  single reads; each of those decodes its element again.
- **Measured** (`decode-compare`, all 83,178 shapes in the corpus, summed over threads, before
  step 6 removed it with the typed path): the schema `DecodeShapeMesh` took 17.3 s against the
  typed `DecodeMesh`'s 4.0 s, about 0.2 ms per shape. Before element reuse it was 155.7 s. The rest of the gap is the interpreter's
  per-field work (conditions, frames); a later optimization could compile a per-array element
  plan.

## Migration plan

Each step shipped on its own. The typed path (the hand-written `NifFile` and the block structs
in `Niffer.h`) kept working until step 6 deleted it.

1. **Done (2026-10-09). Spec, generator, engine; seed every typed layout.** Language 0.1, the
   generator (validation plus all four outputs), the engine, and the test schema. A one-shot
   script (not kept) transcribed every typed layout into `schema/`: 144 concrete blocks, 10
   abstract bases and 8 structs, one file each. Raw tails stay `bytes`.

   Result on the SSE corpus (`niffer-tool roundtrip --schema`, 22,760 files):
   - all 786,940 blocks decode, and re-encode byte-exactly both verbatim and forced;
   - the schema models every block the typed path does;
   - all 884,440 non-null refs are in range and satisfy a `to:` claim, and the `ref` graph is
     acyclic;
   - all 400,426 string refs are in range.

   The checks the typed path never had found three typed-path mislabels (a field misread as
   the BSLightingShaderProperty name, a bhk back pointer read as an owning link, and two
   extra-data classes typed as raw) and a registry race. All are logged in
   [bugs.md](bugs.md). The schema carries the corrected layouts; the typed path is left as it is
   until step 6.
2. **Done (2026-10-09). Language 0.2 and the big payloads.** Language 0.2 adds `half`,
   unions, byte-window arrays, packed arrays, and named structs that read enclosing fields. With
   it the schema now describes:
   - BSTriShape vertices and triangles (`BSVertex`, laid out by the `vertexDesc` flags);
   - BSDynamicTriShape's dynamic vertices;
   - NiSkinPartition's vertex data and partition tables;
   - every key group (float, bool, Vec3) and NiTransformData (quaternion, TBC and XYZ
     rotations);
   - BSLightingShaderProperty's material scalars and shader-type extras.

   Each layout was measured on the corpus first: all 33 distinct `vertexDesc`s, key sizes per
   interpolation type, and material-tail word types per shader type. Result:
   `roundtrip --schema` stays GREEN, and **95.94% of the corpus's 2.28 GB of block data now
   decodes field by field** (opaque `bytes` 4.06%, mostly Havok). Field names come from
   first-party code (SCT's NifDocument, NIFFER's decode helpers) or from layout; words whose
   meaning isn't established are `unknownN`.
3. **Done (2026-10-09). Close the remaining tails,** largest first by
   `roundtrip --schema --refs`, each layout measured on the corpus first.
   - **Havok:** every bhk class is decoded field by field except the MOPP byte code, by a session
     that never saw the old, nifly-derived `CollisionExtract.cpp`
     ([collision-provenance.md](collision-provenance.md)). `ExtractCollision` (step 4's
     collision part) now reads through the schema.
   - **Legacy geometry** (`NiTriShapeData`, `NiTriStripsData`), **`NiControllerSequence`**,
     **`BSEffectShaderProperty`**, **`NiDefaultAVObjectPalette`**, particle data and particle
     systems, and the scale and color modifiers: decoded field by field.
   - **Every remaining ref** in the long tail of particle controllers, modifiers, emitters,
     colliders and legacy shapes is typed. What's still opaque there holds no refs.

   Result: `roundtrip --schema` stays GREEN at **98.67%** decoded (opaque `bytes` 1.33%, mostly
   MOPP byte code), with 1,098,425 refs checked against `to:` and 728,961 string refs in range.
   `schemaVersion` is 0.2.0: steps 2–3 renamed and re-parented fields.
4. **Done (2026-10-10). Decode helpers on views.** The schema path has its own helpers,
   declared at the end of `Schema.h`:
   - `DecodeMesh(view::BSTriShape)` and `DecodeMesh(view::NiSkinPartition)`;
   - `DecodeShapeMesh(Nif&, Block&)`, which carries the geometry-selection rule SCT's
     NifDocument had: the shape's own vertices; else its skin partition's; for
     `BSDynamicTriShape`, the partition's attributes with positions from the dynamic vertices;
   - `ExtractCollision(Nif&)`, with no save-and-reload round trip.

   They read through `Block::ForEachElement` (new) and the views' field-index constants (new).
   `niffer-tool decode-compare` requires the schema path to equal the typed path bit for bit on
   every shape in the corpus. All 83,178 match; for the 37 shapes whose vertex layout has no
   position attribute, the typed decoder reads other bytes as positions, so only sizes are
   compared there. The TRI helpers (`DecodeMorphDeltas`, `DecodeBaseVertices`, `ApplyMorphs`)
   stay on `TriFile`: TRI is its own file format, outside the NIF schema.
   `GlossinessToRoughness` and `DecodeVertexDesc` are pure functions and serve both paths. The
   typed `DecodeMesh` overloads stay until step 6.
5. **Done (2026-10-10). Port SCT.** SCT's `NifDocument` loads with the schema container and reads
   every block through the views; geometry comes from `DecodeShapeMesh`, and the viewport's
   collision from `ExtractCollision` (Skyrim-Content-Tools PR 15). Old and new `NifDocument`
   were compared over the corpus: geometry, textures, shader flags, material values, alpha and
   skin bindings are bit-identical. The differences are corrections: blocks the typed path kept
   raw (`BSDamageStage` and `BSMasterParticleSystem` children, `BSFurnitureMarkerNode` and
   `BSDecalPlacementVectorExtraData` as extra data), and `shaderType`, which the typed path
   never exposed.
6. **Done (2026-10-10). Delete the typed world.** The hand-written block structs, their `Sync`
   functions, the registry, `UnknownBlock`, the math PODs and the typed `DecodeMesh` overloads
   are gone, with `roundtrip`'s typed half and `decode-compare`. `Nif` took over `NifFile`'s
   name; `niffer-tool roundtrip` is the schema gate. `Stream` remains for the header, footer and
   TRI. The gates are unchanged: `roundtrip` GREEN (786,940 blocks, all modeled; 98.67%
   decoded), `tri-roundtrip` 569 of 569, `collision-validate` identical to before.
7. **Text form (optional):** a `niffer-yaml` target that decompiles a NIF to YAML through the
   schema and compiles it back. It writes blocks in faithful mode (byte-exact) or authoring mode
   (named refs, tables rebuilt).

## Decisions

Settled 2026-10-09:

- **ESPER's model:** one interpreted engine over generated tables; generated views are accessors
  only; `schema.json` ships as a product. No host-language escape hatches. If the language can't
  express something, the language grows.
- **YAML descriptors, one file per class,** compiled by a Python generator whose output is
  committed.
- **Verbatim unless modified,** with a forced re-encode real-data gate.
- **The container stays in C++** (header, block framing, footer), as ESPER's plugin container does.
- **Clean-room and dual-licensed:** descriptors are written from real-data verification only.
