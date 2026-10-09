# Havok collision (bhk*) — provenance

How the bhk descriptors in `schema/blocks/collision/` (and the structs they use) and the
`ExtractCollision` conventions in `cpp/collision/Collision.cpp` were established. NIFFER is
dual-licensed, so every line has to be the copyright holder's own work; this file is the record
that these layouts came from the game's bytes and nothing else.

## Sources

**Used:** the shipped Skyrim SE meshes only (the extracted vanilla corpus, 22,761 `.nif` files,
11,482 of them with bhk blocks), NIFFER's own first-party code (header framing, `DecodeMesh`,
the schema engine) and its existing descriptors.

**Not used:** nif.xml / niftools nifxml, nifly, NifSkope, PyFFI, niflib or any other NIF
library's source or schema; the Havok SDK (headers included: none was available); the previous
`CollisionExtract.cpp` (never opened by the session that wrote this) or anything else in the
SCT repositories; no web pages about NIF, bhk or Havok packfile layouts. No layout below was
taken from memory of such sources: each is a hypothesis that was then tested against every
block of its class in the corpus, and kept only if the test held.

## Method

1. A throwaway Python script read the corpus with NIFFER's header framing
   (`cpp/Header.cpp`) and pulled out every bhk block.
2. Per class: block-size census; per 32-bit word, the number of distinct values, the share that
   look like floats or small integers, and the most common values; byte-level statistics where
   words looked mixed; pairs of offsets that are always equal; for variable sizes, the counts
   that predict the size exactly.
3. Each hypothesis was checked on **every** block of the class: exact size accounting first,
   then invariants (unit vectors, orthonormal frames, points inside stored bounds, indices in
   range, values equal to a related block's).
4. The hypotheses were encoded as descriptors and proved by the schema gate (`niffer-tool
   roundtrip --schema --refs`): byte-exact verbatim, forced and packed re-encode, no refusals,
   every `to:` claim, acyclic `ref` graph.
5. `niffer-tool collision-validate --sweep` measures the extractor's conventions against the
   render geometry (below).

Naming follows CLAUDE.md: a field is named for its layout (an array, a count, a vector shape) or
for a relation the data proves (below); everything else is `unknownN`, with the observed values
in its `doc`. "Always" below means "in every block of that class in the SSE corpus".

## Gate, before and after

| | before | after |
|---|---|---|
| block bytes decoded field by field | 95.94% | **98.16%** |
| opaque `bytes` | 4.06% | 1.84% |
| bhk opaque `bytes` | ~89.0 MB (8 fields) | 29.9 MB (`bhkMoppBvTreeShape.moppData` only) |
| refs checked against `to:` | 876,226 of 876,226 | 892,009 of 892,009 |

Gate after: 22,760 files, verbatim/forced/packed diff 0, 786,940 blocks all modeled, 0 refused,
0 regressions, 0 ref violations, 0 ref cycles, 400,426 string refs in range: **GREEN**.

## Blocks

### bhkCompressedMeshShapeData (8,214 blocks, 42.2 MB) — fully decoded

The layout parses every block to exactly its size. Measurements:

- Words 0–3 are always 17, 18, 0x3FFFF, 0x1FFFF; word 4 is always 0.001 (`quantization`).
- Two Vec4s follow: `boundsMin` / `boundsMax`. `boundsMax.w` is 1.0 in exactly the 309 blocks
  whose big-vertex list is non-empty, else 0.
- 14 constant bytes (`00 01 00 …`), split as u8, u8, u32 × 3 (the split is not established;
  every value is constant).
- `materials`: a u32 count of 8-byte entries; entry word 0 is a hash-like ID (24 of the 34
  distinct values also occur as primitive-shape materials), word 1 is 1 in 11,425 entries.
- `unknownCount`: always 0.
- `transforms`: a u32 count of 32-byte entries, a Vec4 with w = 1.0 and a unit quaternion
  (x, y, z, w). All 12,115 are the identity.
- `bigVertices`: a u32 count of Vec4s (w = 999.99994), all inside the bounds.
- `bigTriangles`: a u32 count of 12-byte entries: three u16 indices, all below the big-vertex
  count (67,450 triangles); a u16 below the material count; a u16 that is always 0; a u16.
- `chunks`: a u32 count of chunks, then a trailing u32 that is always 0.
- A chunk: a Vec4 `offset` (w = 0); a u32 below the material count; a u16 that is always
  0xFFFF and a u16 that is always below the transform count; then four u32-counted u16 arrays.
  Over all 24,457 chunks: the first array's length is a multiple of 3 (`vertices`, x y z per
  vertex); every value of the second (`indices`) is below the vertex count; the third
  (`stripLengths`) sums to at most the index count and leaves a multiple of 3; the fourth is
  always exactly as long as the index array (`unknownIndexData`).
- **Vertex decode**: `offset + q * quantization` puts all 3,174,859 chunk and big vertices
  inside `boundsMin..boundsMax` (worst excess 6.1e-5, under one quantization step). This is the
  decode `ExtractCollision` uses; it also refuses any shape whose vertices fall outside.
- **Index order**: strips first (alternating winding), then a triangle list. The other order
  produces 13,209 degenerate triangles and 80× more doubled directed edges (321,890 against
  3,948); strips-first produces none.

### bhkCompressedMeshShape (8,214 blocks, 56 bytes) — fully decoded

`unknown0` always 0 (it is not an owning ref; see bugs.md); `unknown4` looks like a 32-bit
address; `radius` always 0.005; `unknown12` an f32; two Vec4s always (1, 1, 1, 0) around an f32
always 0.005; `data`, a ref whose 8,214 targets are all `bhkCompressedMeshShapeData` (`to:`
confirmed by the gate). `radius` is named from the MOPP relation below.

### bhkMoppBvTreeShape (8,362 blocks, 30.2 MB) — header decoded, byte code opaque

The size is 41 + the u32 at offset 20 in every block, so that u32 is `dataSize` and the payload
is the last `dataSize` bytes. The byte before the payload is 1 in 8,279 blocks and 0xCD (an
uninitialized-memory fill) in 78, so it is a separate field, not part of the payload. For the
8,214 trees over a compressed mesh: the Vec3 at offset 24 equals the data's `boundsMin - radius`
(8,066) and the f32 at offset 36 times (largest bounds extent + 2·radius) equals 254 · 65536
(8,076): `origin` and `scale`. 13 trees (Whiterun/Skaal roof corridors, imperial stairs) have
data bounds up to 0.034 outside that grid on one axis. The word at offset 4 is 0 in 8,336 blocks
and otherwise a primitive-shape material (`material`). The tree byte code itself is kept as
`bytes`: its opcodes are not decoded.

### bhkRigidBody / bhkRigidBodyT (14,880 blocks) — fully typed, partly named

Sizes 250, 254, 262, 266 = 250 + 4 · n, where n is the u32 at offset 244 (0, 1, 3, 4): a ref
list, whose targets are the constraint classes and nothing else, and every constraint block is
referenced from exactly one rigid body (1,632 of 1,632), so it is the owning `ref`
(`constraints`) and the constraints' entity links are `ptr`. A trailing u16 (0 except once).
bhkRigidBodyT has the identical layout and now inherits bhkRigidBody.

- Offsets 4 and 8 are copied at 36 and 40 in every block; byte 12 is always 1 (2 in phantoms);
  12 bytes at 16 are always 0, 0, 0x80000000. Typed as `unknownN`.
- `translation` (Vec4, w = 0 in 14,871) and `rotation` (Vec4, |q| = 1 in every block): named
  from the render-bounds measurement below, where applying them to bhkRigidBodyT and not to
  bhkRigidBody is decisively better than either alternative.
- Two Vec4s always zero.
- `inertia`: three Vec4 rows of a symmetric 3×3 matrix (w = 0). `mass` (offset 180) is 0 exactly
  when the matrix is all zero (14,868 of 14,880). For bodies with a bhkSphereShape the diagonal
  is 0.4 · mass · radius² in most of them (the sphere's moment of inertia, which also confirms
  the sphere's radius word).
- `centerOfMass`: the capsule midpoint for 1,465 of 1,468 capsule bodies, the origin for every
  box and sphere body, inside the vertex AABB for 1,552 of 1,569 convex bodies.
- Ten f32s at 184–220, eight u8s at 224–231 and three u32s at 232–243: value distributions only
  (in the field docs); meanings not established.

### bhkSimpleShapePhantom (190 blocks, 100 bytes) — fully typed

A shape ref (it now inherits bhkShapeWrapper, `to:` confirmed), the first 28 bytes as in the
rigid body, 8 unstructured bytes, then a `HavokTransform`.

### HavokTransform (bhkConvexTransformShape 1,681, bhkTransformShape 94, phantoms 190)

Four Vec4s: the first three are orthonormal with determinant +1 and w = 0 in every block, the
fourth a translation (w = 0 except 4 blocks). Whether the three are the rotation's rows or
columns is settled by the render-bounds sweep (columns: 37 files better, 3 worse; 24 to 0 in mesh
files). In bhkConvexTransformShape the material and radius words always equal the child
shape's; in bhkTransformShape the material equals a primitive child's (79 of 79) and the next
word is always 0.

### Primitive shapes

- **bhkSphereShape** (207, 8 bytes): `material`, `radius` (confirmed by the inertia relation).
- **bhkBoxShape** (3,038, 32 bytes): material, radius, 8 unstructured bytes, three positive
  floats (`halfExtents`), and an f32 always equal to one of the three. Named from the render
  sweep: corners at ±(halfExtents + radius) match the render bounds.
- **bhkCapsuleShape** (2,342, 48 bytes): material, radius, 8 unstructured bytes, `point1`, an
  f32 always equal to radius, `point2`, an f32 always equal to radius. Body centres of mass sit
  at the midpoint of the two points.
- **bhkCylinderShape** (70, 64 bytes): material, radius (always 0.05), 8 unstructured bytes,
  two Vec3 points each followed by the same f32 (`unknown28` = `unknown44`), an f32 always equal
  to `unknown28 - radius`, 12 unstructured bytes. Which word is the cylinder's radius is **not**
  established (the render sweep is inconclusive), so the points are named and the rest is not.
- **bhkConvexVerticesShape** (3,628): material, radius, 24 bytes always
  0, 0, 0x80000000, 0, 0, 0x80000000, then a u32-counted Vec4 array (w always 0) and a
  u32-counted Vec4 array whose xyz is a unit vector (`planes`); the size formula holds for every
  block. For 89% of the 106,180 planes every vertex satisfies n·v + w ≤ 0, and the gap is the
  radius in 59%, so they are plane equations, but not all of them bound the stored vertices.
- **bhkPlaneShape** (1 block): material, then float words; one sample establishes nothing more.

### Containers

- **bhkListShape** (592): the sub-shape ref list, a material (always a primitive-shape
  material), the same 24 constant bytes as bhkConvexVerticesShape, and a u32-counted u32 list
  always as long as the sub-shape list (all 0 but one).
- **bhkNiTriStripsShape** (27): unknown0, an f32 always 0.005, constants, a ref list whose 31
  targets are all NiTriStripsData (`to:` confirmed), and a u32 list always as long as it.
- **bhkBlendCollisionObject** (1,193, 18 bytes): bhkCollisionObject's 10 bytes (it now inherits
  it; its target and body claims hold) plus two f32 always 1.0.

### Constraints (1,632 blocks)

- An abstract `bhkConstraint` holds the common start: a u32 count (always 2) of entity links,
  all bhkRigidBody/bhkRigidBodyT or null (`to:` confirmed), then a u32 always 1.
- **HavokFrame**: in bhkHingeConstraint (8), bhkLimitedHingeConstraint (823) and
  bhkRagdollConstraint (752), offsets 16–79 and 80–143 are each three Vec4s whose xyz form an
  orthonormal right-handed basis in every block, then a point (w = 0). In every limited-hinge
  and ragdoll frame axis 0's w equals axis 2's y and axis 1's w axis 2's x (not in the hinges).
- bhkHingeConstraint: two frames (144 bytes). bhkLimitedHingeConstraint: two frames, an f32
  (not positive in 819 of 823), an f32 (not negative in 819), an f32 (0.01 in 814), a u8
  always 0. bhkRagdollConstraint: two frames, five f32s (all five whole or half degrees in
  radians in 418 of 752 blocks), an f32 (10.0 in 748), a u8 always 0.
- bhkBallAndSocketConstraint (30, 48 bytes): two Vec4 points. bhkStiffSpringConstraint (5, 52
  bytes): two Vec4 points and an f32.
- bhkBreakableConstraint (8; 73 or 77 bytes): a u32 selector, a second entity list (always two
  nulls) and a u32 always 1, then wrapped data whose layout is exactly bhkBallAndSocketConstraint's
  own fields when the selector is 0 (73 bytes) and bhkStiffSpringConstraint's when it is 8 (77),
  then an f32 and a u8. Modeled as a union on the selector.
- bhkBallSocketConstraintChain (6): a u32-counted Vec4 list whose length is always 2 · (chain
  entities − 1), four f32s identical in all six, a u32-counted chain-entity list, then the
  common two-entity list (always the first two chain entities) and a u32 always 1.

## ExtractCollision

`cpp/collision/Collision.cpp` serializes the `NifFile`, loads it as a schema `Nif`, and reads
through the generated views. Conventions, each chosen by measurement:

| Convention | Measurement |
|---|---|
| Compressed-mesh vertex = chunk offset + q · quantization; strips then list | the bounds and degeneracy checks above (0 of 3,174,859 vertices outside) |
| Havok → Skyrim units: `kHavokToSkyrim` (unchanged public constant) | collision/render AABB size ratio has median 1.00 for mesh, box and convex bodies |
| Bodies are placed at their collision object's target node, in root space | toggling it: 1,157 files worse, 106 better |
| NiAVObject Matrix33 is row-major (p' = M·p) | transposed: 541 worse, 257 better; in mesh files 295 worse, 90 better |
| bhkRigidBodyT applies its translation and quaternion; bhkRigidBody does not | never: 2,475 worse, 212 better; always: 1,779 worse, 94 better |
| Quaternion applied as stored (x, y, z, w), not conjugated | conjugated: 212 worse, 25 better |
| HavokTransform basis vectors are the rotation's columns | rows: 37 worse, 3 better (24 to 0 in mesh files) |
| Convex shapes are grown by their radius (box: ±(h + r); convex vertices: each vertex offset by r along the 6 axis and 8 corner directions) | not growing: 2,163 worse, 619 better |
| Spheres and capsules: 26 directions at radius r around the centre / both points | |

Each line comes from `niffer-tool collision-validate --sweep`: for every file with both
collision and render geometry (11,094 files), the largest distance between corresponding faces
of the root-space collision AABB and the render AABB (every BSTriShape with its own vertices,
through its node chain), relative to the render diagonal. A variant counts as "better" or "worse"
on a file when it changes that error.

Skipped and counted: bhkCylinderShape (radius word not established; sweep 6/9 and 7/7),
bhkPlaneShape (one sample), bhkNiTriStripsShape (NiTriStripsData is still opaque), a compressed
mesh with a chunk referencing a non-identity transform (none in the corpus), out-of-range
indices, or vertices outside the stored bounds. Phantom bodies (bhkSimpleShapePhantom, 190) are
trigger volumes, not world collision, and are ignored.

Result over the corpus: **17,139 leaf shapes decoded, 93 skipped**; render agreement **76.8% of
files within 5% of the render diagonal, 87.4% within 10%, 95.0% within 25%**, median error 1.4%.
Mesh collision agrees best (93.6% / 91.5% within 10% for bhkRigidBody / bhkRigidBodyT over a
MOPP). Sphere and capsule bodies agree least (18% and 53% within 10%), mostly skeletons and
creatures whose first body is one bone among many, and small round items with a single sphere.

`tests/test_collision.cpp` checks the conventions on a hand-built file (box under a list under a
rotated, translated bhkRigidBodyT; a compressed mesh under a MOPP under a plain bhkRigidBody with
a translation that must not apply; a skipped cylinder).

## Not established

- The MOPP byte code (29.9 MB, the only bhk `bytes` left).
- The meaning of the rigid-body scalars at 184–243, the constraint scalars, the u16 per index in
  compressed-mesh chunks, the big-triangle trailing u16, the material-table second word, and
  the many constant words. They are typed and documented, not named.
- The cylinder's radius word; bhkPlaneShape beyond word shapes.
- Whether the constraint points and frames are in body A / body B space (the frames are proven
  orthonormal, the points are not related to the bodies here).
