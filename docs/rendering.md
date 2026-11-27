# Rendering

Both renderer backends (Vulkan and D3D11) draw the same way. The backend-independent parts (light packing, shadow setup, the ambient environment, mip generation, culling) live in `engine/render_common.h/.cpp`. The shading lives in `engine/shaders/kes_common.slang`.

## Frame

1. **Shadow maps**: depth passes for the sun cascades and the local shadow atlas tiles that need drawing this frame (see below).
2. **Depth prepass** (when a screen-space effect is on, see Screen-space effects): the opaque objects into a depth buffer and a normal + roughness image, without MSAA.
3. **Screen-space lighting**: ambient occlusion and the sun's contact shadows from the prepass (`screen_space_lighting.slang`, then a 4x4 depth-aware blur), and screen-space reflections (`ssr.slang`).
4. **Scene**: HDR (RGBA16F) with MSAA, at the internal render resolution (window size × `ResolutionScale`). The sky draws first, then opaque objects grouped by material and front to back, then transparent objects back to front. Objects outside the view are culled by bounding sphere. Animated (skinned) models are not culled. Opaque surfaces read the AO, contact shadow and reflection images at their pixel.
5. **Resolve**: the multisampled scene into a single-sample HDR image.
6. **Scene color passes** (each optional): TAA, depth of field, motion blur. Each reads a copy of the scene color and writes the result back.
7. **Eye adaptation** (compute, when auto exposure is on): a log2 luminance histogram of that image and the adapted exposure (see Post processing).
8. **Bloom** (when on): a downsample / upsample chain of the HDR image (see Post processing).
9. **Tonemap** (`engine/shaders/blit.slang`): sharpening, bloom, exposure, vignette, colour grading, the tonemap curve, lift/gamma/gain, sRGB encoding and a slight dither, drawn into the backbuffer. With post anti-aliasing it draws into an RGBA8 image at the render resolution instead.
10. **Post anti-aliasing** (FXAA / CMAA2, see below): CMAA2 on that image, then `engine/shaders/present.slang` draws it into the backbuffer, either as a plain copy or with FXAA.
11. **UI**: drawn in display space on top.

## Post processing

Exposure, bloom, vignette, colour grading, the tonemapper and sharpening come from `PostProcessSettings` (`engine/scene.h`). The renderer reads `Scene::GetPostProcess()`, which `World::Update` resolves once a frame from:

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

**Colour grading.** Before the tonemap, in scene linear:
- White balance: `temperature` and `tint` (-1..1), scaled in LMS space like Unity's.
- `colorFilter` (RGB multiply).
- `contrast` around middle grey (0.18), in log space.
- `saturation` (0 = grey).

After the tonemap, in display linear (0-1): `lift` (added to the shadows), `gamma` (above 1 brightens the midtones) and `gain` (scales the highlights).

**Tonemapper.** `tonemapper` picks the curve: `ACES` (Hill's fit, default), `AgX`, `Neutral` (Khronos PBR Neutral), `Reinhard` or `None` (clamp). Volumes switch it once their weight passes 0.5.

**Sharpening.** `sharpen` (0-1, 0 = off) runs AMD's contrast adaptive sharpening on a 5-tap cross of the scene at the render resolution. It runs inside the tonemap pass, on exposed HDR behind a reversible tonemap. It is most useful with `ResolutionScale` below 100.

### Screen-space effects

They share one depth + normal prepass (`depth_normal.slang`) of the opaque objects. The prepass runs when at least one of them is on. It uses the `.mat` surface, so what a material shader changes about the surface itself (normals, roughness) is not seen there. Transparent objects and particles are not in the prepass and get none of the lighting effects. Each effect can be switched off for the player with a `GameSettings` option and per scene with its `PostProcessSettings` value.

**Ambient occlusion** (`bAmbientOcclusion`). It takes 12 samples over the normal's hemisphere within `ambientOcclusionRadius` (meters, default 0.5). The result, raised to `ambientOcclusionIntensity` (default 1, 0 = off), darkens the ambient light and the environment reflection. It does not darken direct light.

**Contact shadows** (`bContactShadows`). A 12-step ray towards the sun, `contactShadowLength` meters long (default 0.3, 0 = off), catches the small shadows the shadow maps miss. It applies to the shadowed sun, or else to the first directional light.

**Reflections** (`bScreenSpaceReflections`):
- A ray march against the prepass depth, up to `reflectionMaxDistance` (default 30 m), for surfaces smoother than `reflectionMaxRoughness` (default 0.5).
- The hit is shaded from last frame's scene color: a copy taken after the scene pass, reprojected with last frame's camera.
- The reflection replaces the environment reflection by its weight, scaled by `reflectionIntensity` (default 1). The weight fades at the screen edges, with distance, near the roughness limit and for rays towards the camera.
- Reflections need one frame of history, so they show from the second frame after they are turned on.

**Depth of field** (`bDepthOfField`). It is off until `depthOfFieldMaxBlur` (percent of the screen height, up to 5) is above 0:
- The scene is sharp at `depthOfFieldFocusDistance` and fully blurred `depthOfFieldFocusRange` meters nearer or further away.
- It uses a single-pass golden-angle gather (after Dennis Gustafsson) on the HDR scene, before bloom.

**Motion blur** (`bMotionBlur`). It is off until `motionBlurIntensity` is above 0 (1 = the motion of a whole frame). It blurs along the camera's motion since last frame, at most 5% of the screen. Moving objects don't blur on their own.

**TAA** (`PostAntiAliasing=TAA`):
- The projection is jittered every frame (Halton 2,3, 8 phases).
- `taa.slang` blends the frame into the reprojected history: bicubic history, variance clipping in YCoCg, current weight 0.1.
- The history is reprojected from camera motion only. Moving objects rely on the clipping, so they can blur or ghost slightly.
- TAA softens the image, and `sharpen` (around 0.3-0.5) pairs well with it.

### `PostProcessVolumeEntity`

A brush volume, like `FogVolumeEntity`. It applies while the camera is inside, fading in over `BlendDistance` meters outside, or everywhere with `Unbound`.
- `Priority` and `Weight` work as for layers.
- A setting the map leaves empty is not overridden.
- Settings: `ExposureCompensation`, `AutoExposure`, `AutoExposureMin`, `AutoExposureMax`, `AdaptationSpeedUp`, `AdaptationSpeedDown`, `BloomIntensity`, `BloomRadius`, `VignetteIntensity`, `VignetteSmoothness`, `VignetteColor`, `Tonemapper` (0 ACES, 1 AgX, 2 Neutral, 3 Reinhard, 4 None), `Temperature`, `Tint`, `Saturation`, `Contrast`, `ColorFilter`, `Lift`, `Gamma`, `Gain`, `Sharpen`, `AmbientOcclusionIntensity`, `AmbientOcclusionRadius`, `ContactShadowLength`, `ReflectionIntensity`, `ReflectionMaxRoughness`, `ReflectionMaxDistance`, `DepthOfFieldFocusDistance`, `DepthOfFieldFocusRange`, `DepthOfFieldMaxBlur`, `MotionBlurIntensity`.
- Inputs: `Enable`, `Disable`, `Toggle`, `SetWeight(float)`.
- From code, `GetLayer()` gives the layer it submits.

## Anti-aliasing

`AntiAliasing` sets the MSAA sample count. `PostAntiAliasing` (`Off`, `FXAA`, `CMAA2`, `TAA`) picks the post mode. FXAA and CMAA2 work on the final tonemapped image. TAA works on the HDR scene before the post effects (see Screen-space effects). It is independent of MSAA, and the two can be combined. Both post modes run at the render resolution before the upscale to the window, and the UI is not affected.

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

`config/DefaultGameSettings.ini` / `GameSettings`: `ShadowQuality`, `AntiAliasing` (MSAA samples 1/2/4/8), `PostAntiAliasing` (`Off` / `FXAA` / `CMAA2` / `TAA`, `GameSettings::SetPostAntiAliasing`), `bBloom` (`GameSettings::SetBloom`), `bAmbientOcclusion`, `bContactShadows`, `bScreenSpaceReflections`, `bDepthOfField`, `bMotionBlur` (`GameSettings::Set...`), `AnisotropicFiltering` (1-16), `ShadowDistance`, `ResolutionScale`. They are applied live with `ApplySettings` (`IRenderer::SetGraphicsOptions`). Unsupported values are clamped to what the GPU supports.

## Custom material shaders

A `.shader` asset can replace the mesh program for a material. Start from `engine/shaders/mesh_fallback.slang`: `KesMeshVertex` does skinning and transforms, `KesShadeSurface` does the full material, and smaller helpers (`KesApplyFog`, `KesAmbientIrradiance`, `KesEnvironmentBRDF`, ...) are available for custom lighting. Bindings are listed at the top of `kes_common.slang`.

To change only what the surface is, keep the engine lighting: `KesMaterialSurface` returns a `KesSurface` filled from the `.mat` values and textures, and `KesShadeMaterial` lights it. Override fields in between (`KesTangentNormal` converts a tangent-space normal). `KesTime()` is seconds since the first frame. The shader graph tool generates shaders of this form. The shadow pass doesn't run material shaders, so alpha changed in a shader doesn't affect shadows.
