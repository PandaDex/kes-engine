# Rendering

Both renderer backends (Vulkan and D3D11) draw the same way. The backend-independent parts (light packing, shadow setup, the ambient environment, mip generation, culling) live in `engine/render_common.h/.cpp`. The shading lives in `engine/shaders/kes_common.slang`.

## Frame

1. **Shadow maps**: depth passes for the sun cascades and the local shadow atlas tiles that need drawing this frame (see below).
2. **Scene**: HDR (RGBA16F) with MSAA, at the internal render resolution (window size × `ResolutionScale`). The sky draws first, then opaque objects grouped by material and front to back, then transparent objects back to front. Objects outside the view are culled by bounding sphere. Animated (skinned) models are not culled.
3. **Resolve**: the multisampled scene into a single-sample HDR image.
4. **Eye adaptation** (compute, when auto exposure is on): a log2 luminance histogram of that image and the adapted exposure (see Post processing).
5. **Bloom** (when on): a downsample / upsample chain of the HDR image (see Post processing).
6. **Tonemap** (`engine/shaders/blit.slang`): bloom, exposure, vignette, the ACES filmic curve, sRGB encoding and a slight dither, drawn into the backbuffer. With post anti-aliasing it draws into an RGBA8 image at the render resolution instead.
7. **Post anti-aliasing** (optional, see below): CMAA2 on that image, then `engine/shaders/present.slang` draws it into the backbuffer, either as a plain copy or with FXAA.
8. **UI**: drawn in display space on top.

## Post processing

Exposure, bloom and vignette come from `PostProcessSettings` (`engine/scene.h`). The renderer reads `Scene::GetPostProcess()`, which `World::Update` resolves once a frame from:

1. `Scene::GetPostProcessDefaults()`: the base values. Game code may change them.
2. Layers (`PostProcessLayer`): settings, an `overrides` mask (`PostProcessOverride_*`, only those fields are applied), a `priority` and a `weight` (0-1). Layers are applied over the defaults in ascending priority, each blending its fields in by weight. Booleans (`autoExposure`) switch once the weight passes 0.5.
   - Code layers: `AddPostProcessLayer` returns an id for `FindPostProcessLayer` (change it at any time, e.g. fade the weight) and `RemovePostProcessLayer`. They stay until removed, also across map loads, and win priority ties.
   - `PostProcessVolumeEntity` submits a layer every frame (`SubmitPostProcessLayer`). Submitted layers are cleared each unpaused frame.

```cpp
PostProcessLayer hurt;
hurt.overrides = PostProcessOverride_VignetteIntensity | PostProcessOverride_VignetteColor;
hurt.settings.vignetteIntensity = 0.7f;
hurt.settings.vignetteColor[0] = 0.5f; // reddish edges
hurt.priority = 100;
PostProcessLayerId id = game->scene->AddPostProcessLayer(hurt);
// later
if (PostProcessLayer* layer = game->scene->FindPostProcessLayer(id))
    layer->weight = damageFlash;
game->scene->RemovePostProcessLayer(id);
```

`Scene::ResetExposureAdaptation()` snaps eye adaptation to the current view on the next frame (use after a teleport or camera cut). Map loads do this on their own.

**Exposure.** The HDR image is multiplied by `SceneState::exposure` × 2^`exposureCompensation`, and with `autoExposure` by the adapted exposure as well:
- `engine/shaders/exposure_histogram.slang` builds a 256-bin histogram of log2 luminance (-12 to +6).
- `exposure_adapt.slang` averages the pixels between the 40th and 95th percentile and clamps the result to `autoExposureMin` / `autoExposureMax` (log2 luminance, EV). It moves the adapted value towards it with `adaptationSpeedUp` / `adaptationSpeedDown` (1/s, exponential), and maps it to middle grey (0.18).
- The result stays on the GPU (a small state buffer) and is never read back.
- Auto exposure needs compute on the graphics queue. Without it the manual exposure is used and a message is logged.

**Bloom.** It follows Jimenez's Call of Duty: Advanced Warfare method:
- `bloom_downsample.slang` uses 13 taps. The first pass from the scene uses a Karis average against flicker.
- The chain has up to 6 levels, starting at half the render resolution.
- `bloom_upsample.slang` applies a 3x3 tent filter scaled by `bloomRadius`, added onto each larger level.
- There is no threshold. The tonemap blends the result in as `lerp(scene, bloom, bloomIntensity)`, so bright areas bloom the most.
- The `bBloom` game setting (`GraphicsOptions::bloom`) skips it entirely, and so does an intensity of 0.

**Vignette.** It darkens towards `vignetteColor` at the edges, in HDR before tonemapping:
- `vignetteIntensity` runs from 0 (off) to 1.
- `vignetteSmoothness` sets the falloff.
- The shape is a circle, corrected for the aspect ratio.

### `PostProcessVolumeEntity`

A brush volume, like `FogVolumeEntity`. It applies while the camera is inside, fading in over `BlendDistance` meters outside, or everywhere with `Unbound`.
- `Priority` and `Weight` work as for layers.
- A setting the map leaves empty is not overridden.
- Settings: `ExposureCompensation`, `AutoExposure`, `AutoExposureMin`, `AutoExposureMax`, `AdaptationSpeedUp`, `AdaptationSpeedDown`, `BloomIntensity`, `BloomRadius`, `VignetteIntensity`, `VignetteSmoothness`, `VignetteColor`.
- Inputs: `Enable`, `Disable`, `Toggle`, `SetWeight(float)`.
- From code, `GetLayer()` gives the layer it submits.

## Anti-aliasing

`AntiAliasing` sets the MSAA sample count. `PostAntiAliasing` (`Off`, `FXAA`, `CMAA2`) works on the final tonemapped image. It is independent of MSAA, and the two can be combined. Both post modes run at the render resolution before the upscale to the window, and the UI is not affected.

- **FXAA**: FXAA 3.11 quality (preset 29), one fullscreen pass that also writes the backbuffer. It is cheap and smooths every high-contrast edge, but it softens texture detail and text in the scene a little.
- **CMAA2**: Intel's Conservative Morphological AA 2.3 (`engine/shaders/cmaa2.slang`, Apache 2.0, ported with small changes listed in its header). It runs as four compute shaders with indirect dispatch: edges, shape candidates, blending and the deferred color apply. It changes far fewer pixels than FXAA and keeps the image sharper. It needs compute on the graphics queue; otherwise, and if its shaders fail to load, post AA stays off and a message is logged.

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

Up to 4 directional lights per frame, in scene order. Point and spot lights go through the clustered path shared with particle lights: the visible ones nearest to the camera first, up to 4096 local lights per frame (scene lights before particle lights), 255 per cluster and 256K cluster entries. Each pixel only loops over the lights of its cluster.

- `Intensity` scales `Color`. A white Lambert surface facing a light of intensity 1 is lit to white (1.0) before tonemapping.
- **Point and spot falloff:** smooth inverse-square-like, reaching 0 at `Range`: `window(d/range)² / (1 + 4 (d/range)²)`.
- **Spot cone:** `SpotAngle` is the full cone. The soft edge starts at 75 % of it.
- **`CastShadows`:** defaults to on for directional and spot lights, and off for point lights (a point light takes six shadow tiles).
- **Particle and submitted lights:** particle Light renderers and `Scene::SubmitPointLight` add shadowless point lights to the same clusters, after the scene's lights. Same falloff as point lights. See [particles.md](particles.md#lights-and-the-clustered-light-list).

Particles (sprites and ribbons) are drawn after the transparent meshes with `particle.slang`; mesh particles are drawn as scene objects. See [particles.md](particles.md).

## Shadows

| `ShadowQuality` | Sun cascades | Local shadow atlas | Largest local tile |
|---|---|---|---|
| 0 Off | none | none | |
| 1 Low | 2 × 1024 | 2048² | 512 |
| 2 Medium | 3 × 2048 | 4096² | 512 |
| 3 High (default) | 4 × 2048 | 4096² | 1024 |

- **Sun:** the first shadow-casting directional light gets cascaded shadow maps over `ShadowDistance` meters (default 60). Cascades are bounding spheres snapped to whole texels, so shadows don't shimmer as the camera moves.
- **Local lights:** shadow-casting spot and point lights get tiles in one depth atlas: a spot one tile, a point light six (one per cube face, a size smaller). Tile size follows how big the light is on screen, from 128 up to the largest tile. Up to 128 tiles per frame, nearest lights first; when they don't fit, the largest tiles of the farthest lights shrink first.
- **Shadow caching:** atlas tiles are kept between frames. A light's tiles are drawn again only when the light changes, or when a shadow caster in its range moves, appears or goes away (redraws caused by casters are limited to 16 tiles a frame, oldest first; new and moved lights are always drawn at once). A still scene draws no local shadow maps at all. Tiles of lights out of view stay until the space is needed. Animated models count as moving every frame they animate.
- **Filtering and bias:** 3×3 hardware PCF, slope-scaled depth bias, and a normal offset against acne.

## Ambient light and reflections

Ambient light comes from the sky, an image-based environment rebuilt only when the sky changes:

- **Diffuse:** 9 spherical-harmonic coefficients of the sky's irradiance.
- **Reflections:** a 128-pixel RGBA16F cube, prefiltered with GGX for 6 roughness levels, combined with an analytic split-sum BRDF.

The source is the `SkyboxEntity` gradient (without the sun disc, which a directional light should provide), scaled by its `AmbientIntensity`. Without a sky, `SceneState::ambient` is used as a uniform environment.

## Fog

`FogVolumeEntity` blends linearly from `NearDistance` to `FarDistance` (per pixel, in HDR). The old `StepCount` banding is ignored.

## Settings

`config/DefaultGameSettings.ini` / `GameSettings`: `ShadowQuality`, `AntiAliasing` (MSAA samples 1/2/4/8), `PostAntiAliasing` (`Off` / `FXAA` / `CMAA2`, `GameSettings::SetPostAntiAliasing`), `bBloom` (`GameSettings::SetBloom`), `AnisotropicFiltering` (1-16), `ShadowDistance`, `ResolutionScale`. They are applied live with `ApplySettings` (`IRenderer::SetGraphicsOptions`). Unsupported values are clamped to what the GPU supports.

## Custom material shaders

A `.shader` asset can replace the mesh program for a material. Start from `engine/shaders/mesh_fallback.slang`: `KesMeshVertex` does skinning and transforms, `KesShadeSurface` does the full material, and smaller helpers (`KesApplyFog`, `KesAmbientIrradiance`, `KesEnvironmentBRDF`, ...) are available for custom lighting. Bindings are listed at the top of `kes_common.slang`.

To change only what the surface is, keep the engine lighting: `KesMaterialSurface` returns a `KesSurface` filled from the `.mat` values and textures, and `KesShadeMaterial` lights it. Override fields in between (`KesTangentNormal` converts a tangent-space normal). `KesTime()` is seconds since the first frame. The shader graph tool generates shaders of this form. The shadow pass doesn't run material shaders, so alpha changed in a shader doesn't affect shadows.
