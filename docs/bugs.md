# NIFFER — bug log

Symptom · what's ruled out · root cause · exact `file:line` · status (`OPEN`/`INVESTIGATING`/`FIXED`).
Keep in sync in the SAME change as the code.

## Open

None.

## Fixed

### Typed path: BSLightingShaderProperty's leading fields are mislabeled — FIXED
- **Symptom:** the typed struct calls the first u32 `name` (a string ref) and the next three
  `headerA/B/C` ("observed -1/0/-1"). As a string index, `name` is out of range in 4,310 SSE
  blocks. SCT's `NifDocument::shaderType` is declared but never set, because the typed path
  exposes no shader-type field.
- **Ruled out:** a byte round-trip can't see this: the bytes come back identical whatever the
  labels say.
- **Root cause:** the first u32 is the shader type. Across the SSE corpus it takes 10 values
  (0–6, 11, 14, 16), and the block size is a function of it alone. The next three words are
  NiObjectNET's name, extra-data count and controller ref: `C` is a block index (7, 8, 11,
  12, …) whenever the shader is animated. If the extra-data count is ever non-zero, refs follow
  it, and every typed field after `headerC` reads from the wrong offset. Vanilla never does this.
- **Where:** `include/niffer/Niffer.h` `struct BSLightingShaderProperty`,
  `cpp/blocks/Shaders.cpp` `BSLightingShaderProperty::Sync`.
- **Status:** fixed in the schema (`schema/blocks/shaders/BSLightingShaderProperty.yaml`:
  `shaderType`, `name`, `extraData`, `controller`); SCT reads `shaderType` since migration step
  5, and step 6 deleted the typed path.

### Typed path: `bhkCompressedMeshShape` / `bhkNiTriStripsShape` `.shape` is not a child shape — FIXED
- **Symptom:** treating the first field as an owning `ref` makes the block graph cyclic in
  7,918 SSE files: the shape "owns" the root node. In one `bhkNiTriStripsShape` it is out of
  range.
- **Root cause:** in `bhkCompressedMeshShape` it is always 0, the root, in the corpus, so it
  isn't an owning link; whether it is a back pointer or a plain integer isn't established from
  the data alone. In `bhkNiTriStripsShape` it isn't a block reference at all: it is 0 in 26
  blocks and `-455893853` in one.
- **Where:** `cpp/blocks/Collision.cpp` (both registered as `bhkShapeWrapper`).
- **Status:** fixed in the schema (`unknown0: u32` in both); migration step 6 deleted the typed
  path.

### Typed path: BSFurnitureMarkerNode / BSDecalPlacementVectorExtraData typed as plain raw — FIXED
- **Root cause:** both are extra data. They are referenced from `NiObjectNET.extraData` in 304
  SSE blocks, and their first u32 is always a valid string index (`NiExtraData.name`).
- **Status:** fixed in the schema (both inherit `NiExtraData`); migration step 6 deleted the
  typed path. SCT's NifDocument flags them as extra data since step 5.

### CollisionExtract.cpp is not clean-room — FIXED
- **Symptom:** `cpp/blocks/CollisionExtract.cpp` (`ExtractCollision`, used by SCT's
  ViewportPanel for cell collision) documented its Havok wire layouts as taken from nifly
  (GPL-3.0), and its compressed-mesh vertex decompression from the Havok SDK headers. It came
  into NIFFER with the extraction from SCT (SCT commit 8658f1474).
- **Why it mattered:** NIFFER is dual-licensed, which needs the copyright holder to own every
  line. Layouts derived from nifly break the clean-room rule (CLAUDE.md, CONTRIBUTING.md).
- **Fix:** the old file is deleted. A session that never read it reverse-engineered the bhk
  payloads from the SSE corpus alone (no nif.xml, nifly or other NIF library; no Havok SDK
  headers either) and described them in `schema/blocks/collision/` (every bhk class but the
  MOPP byte code is now decoded field by field; the schema gate GREEN). The new
  `ExtractCollision` (same API: `CollisionGeometry`, `kHavokToSkyrim`) is
  `cpp/collision/Collision.cpp` over the schema engine, with its internals in
  `hpp/Collision.h`. Every convention it applies was selected by corpus measurement
  (`niffer-tool collision-validate --sweep`), and `tests/test_collision.cpp` covers it on a
  hand-built file. Evidence for every field and convention: `docs/collision-provenance.md`.
- **Behaviour vs the old file (not compared: it was never read):** phantom bodies are ignored;
  cylinders, planes and NiTriStrips-backed shapes are skipped and counted; convex shapes are
  grown by their radius.
- **Related:** step 1 named `bhkCompressedMeshShape`'s first field `target` partly from the old
  file. It's `unknown0` (data alone: always 0).

### EnsureBlocksRegistered raced on first use — FIXED
- **Symptom:** under parallel loads, the typed path's modeled-block count varied between runs
  of the same corpus (786,937 vs 786,940): a few blocks came back as unknown.
- **Root cause:** `EnsureBlocksRegistered` set `done = true` *before* running the registrars.
  A concurrent first `NifFile::Load` saw `done`, then looked up an `unordered_map` that was still
  being filled. That is undefined behavior, and in practice "no factory" for some names, so
  those blocks became UnknownBlocks. Any consumer that loads NIFs from several threads could
  hit it at startup.
- **Fix:** registration runs inside a function-local static initializer, which runs exactly once
  and blocks concurrent callers until it finishes (`cpp/Registry.cpp` `EnsureBlocksRegistered`).
  The registry itself was deleted with the typed path in migration step 6.
