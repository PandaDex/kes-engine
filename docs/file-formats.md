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
- `Company`
- `Version`
- `AssetRoot`
- `StartupMap`

Example:

```txt
Project
{
    Name = "KES"
    Company = "KES"
    Version = "1.0.0.0"
    AssetRoot = "assets"
    StartupMap = "startup.fmapc"
}
```

`Name`, `Company` and `Version` are baked into the game by `kes_add_game`, like `StartupMap`. `Name` is the window title and the package name, `Name` and `Company` pick the player's data folder (`%APPDATA%\<Company>\<Name>\` on Windows), and `Version` (four numbers) is the build and package version. They default to `kes`, `KES` and `1.0.0.0`; the CMake variables `KES_PROJECT_NAME`, `KES_PROJECT_PUBLISHER` and `KES_PROJECT_VERSION` override them for one build folder.

`AssetRoot` is the root scanned by the editor and used for content-relative paths. Project content uses fixed folders under it: `maps/`, `materials/` and `textures/`. The Forge map editor opens the `.kproject` to find them.

`StartupMap` is stored as a project-relative asset path. In practice you should treat it as pointing into the map set under the asset root.

`StartupMap` is also the map the game opens on start: `kes_add_game` reads it and bakes it into the exe (see [Tools and Pipeline](tools-and-pipeline.md)). It names the compiled map relative to the content folder, e.g. `maps/startup.fmapc`. Leave it empty (`StartupMap=""`) to use the engine default `ENGINE_STARTUP_MAP`.

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

### Material slots

The material table is a list of slots, each with a name and an optional `.mat` path. Every primitive names one slot (or `-1`, none), so one mesh can use several materials and each mesh (body) of a model can have its own. The importer makes one slot per source material a primitive uses: glTF/FBX material names, OBJ `usemtl` names. The model editor assigns `.mat` files to slots, adds slots and picks the slot of each mesh part.

A part's material, first match wins:

1. the entity key `Material.<slot name>` (Forge lists one field per slot under *Material slots*)
2. the slot's `.mat` in the `.kmodel`
3. the entity `Material`
4. the model's default material
5. the engine default (white)

A child model uses its slot materials, then the child's `materialPath`, then its own default material; the parent entity's keys don't reach it. Re-importing keeps slot materials by slot name, and keeps the slot picked in the model editor for a part the new source leaves without a material.

Game code can change a slot at runtime with `SkinnedModelEntity::SetSlotMaterialPath(slot, path)` (an empty path removes the override).

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
- version: `2` (version `1` files still load: float PCM, no encoding field)
- format type: binary

Stored data:

- `sourcePath`
- `sampleRate`
- `channels`
- `frameCount`
- `encoding`: `0` = float32 PCM, `1` = QOA
- payload count: samples for PCM, bytes for QOA
- payload: interleaved `float32` PCM samples, or a complete QOA stream ([Quite OK Audio](https://qoaformat.org), `external/qoa`)

Notes:

- only mono and stereo are currently supported
- the importer decodes the source (`.wav`, `.mp3`, `.ogg`) and writes QOA by default (about 3.2 bits per sample, near-transparent); `model-importer --pcm` keeps float PCM
- QOA frames are self-contained (5120 samples each), so long sounds are decoded while they play (`AudioSystem`)

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

## Translation Files: `.po`

GNU gettext PO files, one per language, in `content/localization/<code>.po`
(`en.po`, `pl.po`, `pt_BR.po`). UTF-8 only. Each entry's `msgid` is its key
(`MainMenu.NewGame`, referenced as `#MainMenu.NewGame`) and `msgstr` the text
in that file's language. Plural forms come from the header's
`Plural-Forms`. See [Localization](localization.md).

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
