# Rendering

Both renderer backends (Vulkan and D3D11) draw the same way. The backend-independent parts (light packing, shadow setup, the ambient environment, mip generation, culling) live in `engine/render_common.h/.cpp`. The shading lives in `engine/shaders/kes_common.slang`.

## Frame

1. **Shadow maps**: depth passes for the sun cascades and the local shadow slices (see below).
2. **Scene**: HDR (RGBA16F) with MSAA, at the internal render resolution (window size × `ResolutionScale`). The sky draws first, then opaque objects grouped by material and front to back, then transparent objects back to front. Objects outside the view are culled by bounding sphere. Animated (skinned) models are not culled.
3. **Resolve**: the multisampled scene into a single-sample HDR image.
4. **Tonemap** (`engine/shaders/blit.slang`): exposure (`SceneState::exposure`), the ACES filmic curve, sRGB encoding and a slight dither, drawn into the backbuffer.
5. **UI**: drawn in display space on top.

## Materials (metallic / roughness PBR)

Shading is per pixel: GGX specular, height-correlated Smith visibility, Schlick Fresnel and a Lambert diffuse. Material keys in `.mat` files:

| Key | Default | Meaning |
|---|---|---|
| `BaseColor`, `BaseTexture` | white | albedo (texture is sRGB), alpha |
| `Metallic` | 0 | 0 = dielectric, 1 = metal |
| `Roughness` | 0.8 | 0 = mirror, 1 = fully rough |
| `NormalTexture`, `NormalScale` | none, 1 | tangent-space normal map, +Y up (OpenGL / glTF convention) |
| `MetallicRoughnessTexture` | none | G = roughness, B = metallic (glTF), multiplied with the values above |
| `OcclusionTexture`, `OcclusionStrength` | none, 1 | ambient occlusion (R), affects only ambient light |
| `Emissive`, `EmissiveTexture`, `EmissiveIntensity` | black, none, 1 | emitted light (texture is sRGB), can exceed 1 (HDR) |
| `AlphaCutoff` | 0 | alpha test threshold, also cuts shadows |
| `Lit` | true | false = unlit (base color + emissive) |
| `Transparent`, `DoubleSided` | false | alpha blending, no back-face culling |
| `CastShadows` | true | transparent materials never cast shadows |

Normal mapping doesn't need vertex tangents (the tangent frame comes from screen-space derivatives). Textures get mipmaps at load time and are sampled with anisotropic filtering.

## Lights

Up to 32 lights per frame: all directional lights, then the visible point and spot lights nearest to the camera.

- `Intensity` scales `Color`. A white Lambert surface facing a light of intensity 1 is lit to white (1.0) before tonemapping.
- **Point and spot falloff:** smooth inverse-square-like, reaching 0 at `Range`: `window(d/range)² / (1 + 4 (d/range)²)`.
- **Spot cone:** `SpotAngle` is the full cone. The soft edge starts at 75 % of it.
- **`CastShadows`:** defaults to on for directional and spot lights, and off for point lights (a point light takes six shadow slices).

## Shadows

| `ShadowQuality` | Sun cascades | Local slices |
|---|---|---|
| 0 Off | none | none |
| 1 Low | 2 × 1024 | 6 × 512 |
| 2 Medium | 3 × 2048 | 8 × 512 |
| 3 High (default) | 4 × 2048 | 12 × 1024 |

- **Sun:** the first shadow-casting directional light gets cascaded shadow maps over `ShadowDistance` meters (default 60). Cascades are bounding spheres snapped to whole texels, so shadows don't shimmer as the camera moves.
- **Local lights:** shadow-casting spot lights take one slice and point lights six (one per cube face), nearest lights first, while slices last.
- **Filtering and bias:** 3×3 hardware PCF, slope-scaled depth bias, and a normal offset against acne.

## Ambient light and reflections

Ambient light comes from the sky, an image-based environment rebuilt only when the sky changes:

- **Diffuse:** 9 spherical-harmonic coefficients of the sky's irradiance.
- **Reflections:** a 128-pixel RGBA16F cube, prefiltered with GGX for 6 roughness levels, combined with an analytic split-sum BRDF.

The source is the `SkyboxEntity` gradient (without the sun disc, which a directional light should provide), scaled by its `AmbientIntensity`. Without a sky, `SceneState::ambient` is used as a uniform environment.

## Fog

`FogVolumeEntity` blends linearly from `NearDistance` to `FarDistance` (per pixel, in HDR). The old `StepCount` banding is ignored.

## Settings

`config/DefaultGameSettings.ini` / `GameSettings`: `ShadowQuality`, `AntiAliasing` (MSAA samples 1/2/4/8), `AnisotropicFiltering` (1-16), `ShadowDistance`, `ResolutionScale`. They are applied live with `ApplySettings` (`IRenderer::SetGraphicsOptions`). Unsupported values are clamped to what the GPU supports.

## Custom material shaders

A `.shader` asset can replace the mesh program for a material. Start from `engine/shaders/mesh_fallback.slang`: `KesMeshVertex` does skinning and transforms, `KesShadeSurface` does the full material, and smaller helpers (`KesApplyFog`, `KesAmbientIrradiance`, `KesEnvironmentBRDF`, ...) are available for custom lighting. Bindings are listed at the top of `kes_common.slang`.

To change only what the surface is, keep the engine lighting: `KesMaterialSurface` returns a `KesSurface` filled from the `.mat` values and textures, and `KesShadeMaterial` lights it. Override fields in between (`KesTangentNormal` converts a tangent-space normal). `KesTime()` is seconds since the first frame. The shader graph tool generates shaders of this form. The shadow pass doesn't run material shaders, so alpha changed in a shader doesn't affect shadows.
