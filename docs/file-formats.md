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
| `.map` | level/map file | text block |
| `.prefab` | prefab file | text block |
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
    StartupMap = "startup.map"
}
```

`AssetRoot` is the root scanned by the editor and used for content-relative paths.

`StartupMap` is stored as a project-relative asset path. In practice you should treat it as pointing into the map set under the asset root.

## Map and Prefab Files: `.map` and `.prefab`

Map and prefab files use level blocks, not the generic `TextBlock` structure, but the syntax is similar.

Supported block types:

- `Object`
- `Entity`
- `Collider`
- `Prefab`

Example:

```txt
Entity {
    Class = PlayerStartEntity
    Position = -6.593,0.0,-1.073
}

Object {
    Mesh = meshes/startup.kmodel
    MaterialFile = materials/startup.mat
    Position = 0,0,0
    Rotation = 0,0,0
    Scale = 1,1,1
}
```

Common conventions:

- paths are stored relative to the asset root
- values are mostly parsed from strings by the target runtime system
- `Prefab` blocks allow map composition from other `.prefab` assets

`.prefab` uses the same block vocabulary as `.map`; it is effectively a reusable chunk of level content.

## Shader Descriptor Files: `.shader`

Shader descriptors are text blocks that point to Slang (`.slang`) source files. Shaders are written once and compiled at build time for every enabled renderer (see "Shaders" in `tools-and-pipeline.md`).

Example:

```txt
Shader {
    Name = "PSX Default"
    Source = "shaders/psx_mesh.slang"
}
```

- `Source` - one `.slang` file that defines both `vsMain` and `psMain`
- `VertexShader` / `PixelShader` - optional per-stage overrides (another `.slang` file)
- a stage that is not provided falls back to the engine's `engine/shaders/mesh_fallback.slang`

The runtime never compiles shaders. It loads the binaries built next to the source path, e.g. `shaders/psx_mesh.vs.spv` (Vulkan) or `shaders/psx_mesh.vs.dxbc` (D3D11).

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

- `Shader`
- `BaseTexture`
- `BaseColor`
- `Emissive`
- `AlphaCutoff`
- `Lit`
- `Transparent`
- `DoubleSided`
- `Node` blocks
- `Link` blocks

Example:

```txt
Material {
    Shader = "shaders/psx_default.shader"
    BaseColor = "0.85,0.35,0.25,1"
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
