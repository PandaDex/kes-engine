# Asset and File Formats

This document covers the file types currently used by KES.

## Asset Discovery

The editor scans `Project.AssetRoot` recursively and classifies assets by extension.

| Extension | Purpose | Format |
| --- | --- | --- |
| `.kmodel` | runtime model asset | binary |
| `.ktex` | runtime texture asset | binary |
| `.ksound` | runtime sound asset | binary |
| `.kanim` | runtime animation clip | binary |
| `.kagraph` | animation state graph | text block |
| `.mat` | material graph and material settings | text block |
| `.fmap` | map source, edited in the Forge editor | JSON |
| `.fmapc` | compiled map loaded by the engine | binary (chunked) |
| `.shader` | shader descriptor | text block |
| `.kproject` | project file | text block |
| `.kdec` | editor entity/property declarations | text block |
| `.pak` | packaged asset archive | binary |

The conventional asset folders are:

- `meshes/`
- `materials/`
- `textures/`
- `sounds/`
- `maps/`
- `prefabs/`
- `animations/`

The editor does not require those folders for discovery, but it uses them as the default place for newly created/imported assets.

## Shared Text-Block Syntax

Several KES files use the same simple text-block parser.

General shape:

```txt
BlockType
{
    Key = Value
    OtherKey = "Quoted value"
}
```

Rules:

- block headers are `Type {` or `Type` followed by `{` on the next line
- properties are `key = value`
- quoted strings are allowed
- comments start with `#` or `//`
- parsing is line-oriented and intentionally simple

Files using this style include `.kproject`, `.mat`, `.kagraph`, `.shader`, and `.kdec`.

## Project Files: `.kproject`

Project files are text-block files with a single `Project` block.

Current fields:

- `Name`
- `AssetRoot`
- `StartupMap`

Example:

```txt
Project
{
    Name = "KES"
    AssetRoot = "assets"
    StartupMap = "startup.fmapc"
}
```

`AssetRoot` is the root scanned by the editor and used for content-relative paths. Project content uses fixed folders under it: `maps/`, `materials/` and `textures/`. The Forge map editor opens the `.kproject` to find them.

`StartupMap` is stored as a project-relative asset path. In practice you should treat it as pointing into the map set under the asset root.

## Maps: `.fmap` and `.fmapc`

Maps are built in the Forge editor (`tools/forgeMapEditor`), a Hammer-style brush editor.

- `.fmap`: the editable source map (JSON). Keep it next to the compiled map; it isn't packaged.
- `.fmapc`: the compiled map the engine loads (`World::LoadMap`, `map_file.h`). Build it with **F9** in the editor or `forge --compile in.fmap out.fmapc`.

`.fmapc` version 3 layout (little-endian): `FORGECMP`, u32 version, u32 flags, then chunks of `{char[4] id, u32 size, data}`. Readers skip unknown chunks, so new data can be added without breaking older engines.

| Chunk | Contents |
| --- | --- |
| `MATL` | material paths (content-relative `.mat`, `""` = engine default material) |
| `GEOM` | render meshes, one per material per owner: vertices (position, normal, uv) and triangle indices |
| `ENTS` | entities: class, name, GUID, solid flag, origin, angles, bounds, keyvalues, MapScript links (output, target GUID, input, parameter, delay, once) |
| `BRSH` | convex collision hulls: owner, bounds, planes |

Data is stored in editor space (Z-up, right-handed, editor units). World geometry is in world space; geometry and brushes owned by an entity are relative to the entity origin. The engine converts on load:

- axes: editor `(x, y, z)` becomes engine `(x, z, y)`; the swap also turns right-handed into left-handed
- units: multiplied by `ENGINE_MAP_UNIT_SCALE` (default `0.0254`, so 1 unit = 1 inch, like Hammer)
- angles: pitch and yaw in degrees become the engine's `Rotation` in radians

How the engine instantiates a map:

- world geometry becomes static scene objects, one per material; world brushes become convex colliders
- entities are created by class name through the entity factory (unknown classes are skipped with a warning)
- keyvalues go to `Entity::ApplyProperty`; the entity name is added to `Tags`
- solid entities also receive `Size` (their bounds) and `ApplyMapGeometry` (meshes and hulls); `BrushEntity` renders and collides with them
- each entity keeps its editor GUID (`Entity::GetGuid`); MapScript links are registered after every entity exists (see [engine-api.md](engine-api.md#mapscript))

The engine's entity classes are declared for the editor in `tools/forgeMapEditor/entities/kes.dec`.

## Shader Descriptor Files: `.shader`

Shader descriptors are text blocks that point to Slang (`.slang`) source files. Shaders are written once and compiled at build time for every enabled renderer (see "Shaders" in `tools-and-pipeline.md`).

Example:

```txt
Shader {
    Name = "Toon"
    Source = "shaders/toon_mesh.slang"
}
```

- `Source` - one `.slang` file that defines both `vsMain` and `psMain`
- `VertexShader` / `PixelShader` - optional per-stage overrides (another `.slang` file)
- a stage that is not provided falls back to the engine's `engine/shaders/mesh_fallback.slang`

The runtime never compiles shaders. It loads the binaries built next to the source path, e.g. `shaders/toon_mesh.vs.spv` (Vulkan) or `shaders/toon_mesh.vs.dxbc` (D3D11). Materials without a `Shader` use the engine's PBR program; see [rendering.md](rendering.md#custom-material-shaders).

## Material Files: `.mat`

`.mat` files are text-block assets that combine:

- material settings for runtime rendering
- a simple authored node graph for the material editor

Current node types:

- `Color`
- `Scalar`
- `Texture`
- `Add`
- `Multiply`
- `Output`

Typical top-level data stored in a material file:

- `Shader` (optional; omitted = the engine's PBR program)
- `BaseTexture`, `BaseColor`
- `Metallic`, `Roughness`, `MetallicRoughnessTexture`
- `NormalTexture`, `NormalScale`
- `OcclusionTexture`, `OcclusionStrength`
- `Emissive`, `EmissiveTexture`, `EmissiveIntensity`
- `AlphaCutoff`
- `Lit`
- `Transparent`
- `DoubleSided`
- `CastShadows`

The keys and their defaults are described in [rendering.md](rendering.md#materials-metallic--roughness-pbr).
- `Node` blocks
- `Link` blocks

Example:

```txt
Material {
    BaseColor = "0.85,0.35,0.25,1"
    Metallic = 0
    Roughness = 0.5
    Lit = true
}

Node {
    Id = 1
    Type = "Color"
    Position = "40,40"
    Value = "0.85,0.35,0.25,1"
}

Link {
    FromNode = 1
    FromSlot = 0
    ToNode = 2
    ToSlot = 0
}
```

## Animation Graph Files: `.kagraph`

`.kagraph` files are text-block assets representing a small animation state machine.

They store:

- graph metadata such as `Name`, `Model`, and `EntryState`
- parameters
- states
- transitions
- transition conditions

Current parameter types:

- `Bool`
- `Float`
- `Trigger`

Current condition types:

- `BoolTrue`
- `BoolFalse`
- `FloatGreater`
- `FloatLess`
- `Triggered`

Each state references a `.kanim` clip by asset path.

## Editor Declaration Files: `.kdec`

`.kdec` files are editor-only declaration assets used by the map editor to describe:

- available entity classes
- property definitions
- enum choices
- display metadata such as labels, descriptions, and colors

This is similar in purpose to an FGD-style declaration file.

## Model Files: `.kmodel`

`.kmodel` is the runtime model format.

Container info:

- magic: `KMDL`
- version: `1`
- format type: binary

Stored data:

- `sourcePath`
- material table
- mesh list
- node hierarchy
- bone list

Each mesh stores:

- vertices
- indices
- primitives

Each vertex currently stores:

- position
- normal
- tangent
- UV
- vertex color
- up to 4 bone indices
- up to 4 bone weights

The format is glTF-like in structure: meshes, nodes, materials, and skinning data are all part of the same asset.

## Texture Files: `.ktex`

`.ktex` is the runtime texture format.

Container info:

- magic: `KTEX`
- version: `1`
- format type: binary

Stored data:

- `sourcePath`
- `width`
- `height`
- `format`
- `flags`
- `mipCount`
- raw pixel bytes

Current pixel format support:

- `RGBA8`

Current authoring flags:

- sRGB
- clamp U
- clamp V
- point filtered

At the moment `.ktex` is an uncompressed engine texture container with metadata.

## Sound Files: `.ksound`

`.ksound` is the runtime sound format.

Container info:

- magic: `KSND`
- version: `1`
- format type: binary

Stored data:

- `sourcePath`
- `sampleRate`
- `channels`
- `frameCount`
- `sampleCount`
- interleaved `float32` PCM sample data

Notes:

- only mono and stereo are currently supported
- the importer decodes source audio before writing `.ksound`
- `.ksound` is not MP3 or Ogg under the hood
- imported compression is discarded during conversion

## Animation Clip Files: `.kanim`

`.kanim` is the runtime animation clip format.

Container info:

- magic: `ANIM`
- version: `1`
- format type: binary

Stored data:

- `name`
- `sourcePath`
- `modelPath`
- `duration`
- `looping`
- track list

Each track targets a node and stores:

- translation keys
- rotation keys
- scale keys

This format is separate from `.kmodel`; models and clips are linked by path and node names/indexes.

## Pak Files: `.pak`

`.pak` is the packaged content archive used by the runtime.

Container info:

- magic: `KPAK`
- index magic: `INDX`
- version: `1`
- format type: binary archive

Behavior:

- stores asset files with their normalized content-relative paths
- mounts multiple pak files from the content directory
- later pak mounts can override earlier entries by path
- supports optional encryption

Current encryption details:

- encryption is optional
- encrypted and unencrypted pak files are both supported
- the key is a 32-byte key represented as 64 hex characters
- the runtime key comes from `ENGINE_PAK_ENCRYPTION_KEY_HEX` or an override
- the current implementation uses a ChaCha20-style stream cipher
- encryption is not authenticated signing

## Source Import Formats

The importer currently converts common source formats into runtime formats:

| Source | Imported Runtime Asset |
| --- | --- |
| `.obj`, `.gltf`, `.glb`, `.fbx` | `.kmodel` |
| `.png`, `.jpg`, `.jpeg`, `.bmp`, `.tif`, `.tiff` | `.ktex` |
| `.wav`, `.mp3`, `.ogg` | `.ksound` |

## Format Stability

These file formats are engine-private formats, not a stable public specification yet. Version fields exist in the binary containers, but schema changes should still be treated as likely while the engine is under active development.
