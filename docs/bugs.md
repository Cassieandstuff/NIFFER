# NIFFER — bug log

Symptom · what's ruled out · root cause · exact `file:line` · status (`OPEN`/`INVESTIGATING`/`FIXED`).
Keep in sync in the SAME change as the code.

## Open

### Typed path: BSLightingShaderProperty's leading fields are mislabeled — OPEN (typed path only)
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
  `shaderType`, `name`, `extraData`, `controller`). The typed path is left as-is; it is deleted
  at migration step 6, and SCT moves to the schema path at step 5.

### Typed path: `bhkCompressedMeshShape` / `bhkNiTriStripsShape` `.shape` is not a child shape — OPEN (typed path only)
- **Symptom:** treating the first field as an owning `ref` makes the block graph cyclic in
  7,918 SSE files: the shape "owns" the root node. In one `bhkNiTriStripsShape` it is out of
  range.
- **Root cause:** in `bhkCompressedMeshShape` it is the target node, a back pointer; it is always
  0, the root, in the corpus. `cpp/blocks/CollisionExtract.cpp` already reads it that way. In
  `bhkNiTriStripsShape` it isn't a block reference at all: it is 0 in 26 blocks and
  `-455893853` in one.
- **Where:** `cpp/blocks/Collision.cpp` (both registered as `bhkShapeWrapper`).
- **Status:** fixed in the schema (`target: ptr` and `unknown0: u32`). The typed path is left
  as-is until step 6.

### Typed path: BSFurnitureMarkerNode / BSDecalPlacementVectorExtraData typed as plain raw — OPEN (typed path only)
- **Root cause:** both are extra data. They are referenced from `NiObjectNET.extraData` in 304
  SSE blocks, and their first u32 is always a valid string index (`NiExtraData.name`).
- **Status:** fixed in the schema (both inherit `NiExtraData`).

## Fixed

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
