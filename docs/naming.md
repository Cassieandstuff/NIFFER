# Names that come from outside the bytes

NIFFER names a field or value from first-party code, from its layout, or from CB-SDK's recovered loaders (CLAUDE.md). This
file records the exceptions the owner allowed, and what each name rests on.

## Shader flags and the lighting shader type (owner's decision, 2026-11-11)

The exe holds no names for these, and CB-SDK has no shader loader. They are named from **CommonLibSSE-NG 4.15.0**
(`530bbc73e514251ce96be899375d59f6ccd9fe9f`, MIT): `RE::BSShaderProperty::EShaderPropertyFlag` for the two stored flag words,
`RE::BSShaderMaterial::Feature` for `BSLightingShaderProperty.shaderType`. Value names drop CommonLib's `k` and start
lowercase (`kZBufferTest` → `zBufferTest`, `kLODLandscape` → `lodLandscape`).

Descriptors: `schema/enums/ShaderPropertyFlags1.yaml`, `ShaderPropertyFlags2.yaml`, `LightingShaderType.yaml`. Fields:
`shaderFlags1` / `shaderFlags2` of `BSLightingShaderProperty` and `BSEffectShaderProperty`, and `shaderType`.

**The mapping is unverified.** CommonLib's flag enum is the runtime's 64-bit word; NIFFER takes a NIF's `shaderFlags1` as its
bits 0-31 and `shaderFlags2` as bits 32-63 (stored bit *n* of the second word = enum bit 32 + *n*). Where BK's own reader
(`include/bk/world/Model.cpp`) already uses a bit, the table says whether it agrees.

| Stored | Enum bit | Name | Status |
|---|---|---|---|
| SLSF1 3 | 3 | vertexAlpha | agrees with BK |
| SLSF1 12 | 12 | modelSpaceNormals | agrees with BK |
| SLSF1 18 | 18 | hairTint | **in doubt**: another reading disagrees; check against the exe |
| SLSF1 26 | 26 | decal | agrees with BK |
| SLSF1 27 | 27 | dynamicDecal | agrees with BK |
| SLSF1 31 | 31 | zBufferTest | agrees with BK |
| SLSF2 0 | 32 | zBufferWrite | agrees with BK |
| SLSF2 4 | 36 | twoSided | agrees with BK |
| SLSF2 5 | 37 | vertexColors | agrees with BK |
| SLSF2 6 | 38 | glowMap | agrees with BK |
| SLSF2 8 | 40 | characterLighting | **in doubt**: another reading disagrees; check against the exe |
| SLSF2 22 | 54 | noTransparencyMultiSample | **in doubt**: BK reads stored bit 22 as rim lighting |
| SLSF2 23 | 55 | menuScreen | **in doubt**: another reading disagrees; check against the exe |
| SLSF2 25 | 57 | softLighting | unverified (BK reads soft lighting at stored bit 28) |
| SLSF2 26 | 58 | rimLighting | unverified (BK reads rim lighting at stored bit 22) |
| SLSF2 28 | 60 | snow | **in doubt**: BK reads stored bit 28 as soft lighting |

Every other bit is named as CommonLib names it and is unverified. The shader type's values 4, 5 and 6 agree with BK's reader
(facegen, skin tint, hair tint); the SSE corpus holds 0-6, 11, 14 and 16, and the rest are named but unseen.
