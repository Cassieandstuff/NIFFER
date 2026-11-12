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

**The mapping is confirmed** (CB-SDK verdict, against the 1.6.1170 exe): the base shader loader (`0x14147b380`) reads a NIF's
`shaderFlags1` and `shaderFlags2` as one little-endian 64-bit word, so stored bit *n* of the second word is enum bit 32 + *n*.
The bits below were checked one by one in `GetRenderPasses` (`0x1414adfb0`); BK's own reader (`include/bk/world/Model.cpp`)
is listed where it uses a bit.

| Stored | Enum bit | Name | Evidence |
|---|---|---|---|
| SLSF1 3 | 3 | vertexAlpha | BK agrees |
| SLSF1 12 | 12 | modelSpaceNormals | BK agrees |
| SLSF1 18 | 18 | hairTint | exe: selects the hair-tint material |
| SLSF1 26 | 26 | decal | BK agrees |
| SLSF1 27 | 27 | dynamicDecal | BK agrees |
| SLSF1 31 | 31 | zBufferTest | BK agrees |
| SLSF2 0 | 32 | zBufferWrite | BK agrees |
| SLSF2 4 | 36 | twoSided | BK agrees |
| SLSF2 5 | 37 | vertexColors | BK agrees |
| SLSF2 6 | 38 | glowMap | BK agrees |
| SLSF2 8 | 40 | characterLighting | exe: sets the character-light technique bit |
| SLSF2 22 | 54 | noTransparencyMultiSample | not tested by the lighting code: the name rests on CommonLib alone; it is not rim lighting |
| SLSF2 23 | 55 | menuScreen | exe: the game sets it at run time |
| SLSF2 25 | 57 | softLighting | exe |
| SLSF2 26 | 58 | rimLighting | exe |
| SLSF2 27 | 59 | backLighting | exe |
| SLSF2 28 | 60 | snow | exe |

BK's `Model.cpp` reads rim lighting at stored SLSF2 bit 22 and soft lighting at 28; the verdict says 26 and 25 (a bug in that
reader, reported to its owner). Every other bit is named as CommonLib names it, not checked one by one. The shader type's values
4, 5 and 6 agree with BK's reader (facegen, skin tint, hair tint); the SSE corpus holds 0-6, 11, 14 and 16, and the rest are
named but unseen.
