# Tangent space — provenance

What `DecodedMesh::tangents` / `bitangents` hold, and the corpus evidence for it. Measured by
`niffer-tool tangents-validate` over the extracted SSE corpus (22,760 files; 76,930 shapes with
UVs, normals and tangents; 33,383,393 vertices). No outside source was used: the convention is
read off the stored vectors and the UVs.

## The stored frame

A BSVertex (schema/structs/BSVertex.yaml) with a normal and a tangent carries three more values
in its W slots: `positionW` (f32), `normalW` and `tangentW` (signed-normalized bytes, like the
normal and the tangent). Read as one vector B = (positionW, normalW, tangentW):

| Measurement | Vertices |
|---|---|
| \|B\| within 0.05 of 1 | 33,373,660 (99.97%) |
| B orthogonal to the normal (\|cos\| < 0.1) | 33,121,045 (99.2%) |
| B orthogonal to the tangent (\|cos\| < 0.1) | 32,112,741 (96.2%) |

So the W slots hold a third unit vector, square to the normal: the bitangent. A
**BSDynamicTriShape** (FaceGen heads, beards) keeps its positions in the shape's dynamic
vertices (one Vec4 each) and none in its skin partition, so its bitangent's x is that Vec4's w;
before reading it there, B was unit in only 89.5% of vertices.

## Which way each vector runs

Per vertex, the area-weighted UV gradients dP/dU and dP/dV of the triangles around it, against
the stored vectors (|cos| > 0.7 counted as following):

| | +dP/dU | -dP/dU | +dP/dV | -dP/dV | neither |
|---|---|---|---|---|---|
| tangent field T | 58,228 | 72,280 | **31,824,007** | 37,577 | 1,240,607 |
| bitangent B | **32,403,702** | 63,381 | 98,012 | 86,040 | 581,564 |

The field the format calls the tangent follows +dP/dV, and the bitangent follows +dP/dU: the
names are the other way round from the usual convention. NIFFER keeps the format's names (the
owner's decision, 2026-11-13): `tangents` xyz is the tangent field, `bitangents` the W-slot
vector.

## Handedness

W = sign((N x T) . B), with N, T and B as stored. It is +1 for 21,693,885 vertices and -1 for
11,689,508. Against the UVs, for the 32,708,910 vertices whose UV frame isn't degenerate
(|(N x dP/dU) . dP/dV| >= 0.3 after normalizing):

- W = -sign((N x dP/dU) . dP/dV) for 32,539,149 (99.48%). Since T runs along +V and B along +U,
  an unmirrored frame (V running down the image) has W = +1;
- of the 11,311,330 vertices with W = -1, in 17,947 files, 11,213,173 (99.1%) are mirrored by
  their own UVs: symmetric meshes sharing one half of a texture.

`DecodeShapeMesh` computes W exactly this way; it equals the stored frame's sign for every vertex
where |(N x T) . B| >= 0.05 (33,297,160; the other 86,233 have a near-degenerate frame or a
non-finite bitangent, and get W = +1 unless the product is negative).

A renderer that wants T along +U and B = W' (N x T) along +V (RENDER's lighting shader) takes
T' = `bitangents` and W' = -W.

## Writing it

`EncodeTangentSpace(nif, shape, mesh)` writes the tangent bytes and the bitangent into the
vertices `DecodeShapeMesh` read (the shape's own, or its skin partition's, plus a
BSDynamicTriShape's dynamic w). Without `bitangents` it writes B = W (N x T). Bytes are written
as round((x + 1) * 127.5), the exact inverse of the decode, so writing back a decoded frame is a
no-op: over the corpus, 0 of 22,760 files change and no shape is refused.

`ComputeTangentSpace(mesh)` rebuilds the frame from positions, normals, UVs and triangles: T
along the area-weighted dP/dV and B along dP/dU, each made orthogonal to the normal (not to each
other: the stored B is square to T in only 96.2% of vertices). Against vanilla's stored frames:
W equal in 32,973,646 vertices (98.8%), T within 25 degrees in 30,847,662 (92.4%), B in
32,787,268 (98.2%). The rest are vertices whose stored frame was smoothed or welded differently
by the exporter.
