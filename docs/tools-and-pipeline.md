# Tools and Asset Pipeline

This document describes the current content workflow from source assets to runtime loading.

## Runtime Content Model

At runtime, the engine resolves assets relative to `ENGINE_CONTENT_DIRECTORY`, which defaults to `assets`.

Content can be provided in two ways:

- loose files inside the content directory
- `.pak` archives mounted from the same content directory

The engine mounts all `.pak` files found in the content directory during startup.

Examples with default configuration:

- loose file: `game.exe` next to `assets/maps/startup.map`
- packed content: `game.exe` next to `assets/game_data0.pak`

## Asset Path Resolution

Asset lookups are content-relative.

Examples:

- `meshes/startup.kmodel`
- `materials/startup.mat`
- `textures/ui/logo.ktex`
- `sounds/ambience/wind.ksound`

Command-line override:

- `-assetpath <path>`
- `--assetpath <path>`
- `--assetpath=<path>`

This changes the content root used by both the engine and the importer tools.

## Project File

The editor project file is intentionally small:

```txt
Project
{
    Name = "KES"
    AssetRoot = "assets"
    StartupMap = "startup.map"
}
```

Key point:

- the project no longer defines per-type roots such as mesh root or texture root

Instead, the editor scans `AssetRoot` recursively and recognizes assets by extension.

## Editor Asset Discovery

The editor currently recognizes these asset kinds:

| Kind | Extension |
| --- | --- |
| mesh | `.kmodel` |
| material | `.mat` |
| texture | `.ktex` |
| sound | `.ksound` |
| map | `.map` |
| prefab | `.prefab` |
| animation clip | `.kanim` |
| animation graph | `.kagraph` |

The default placement convention for new/imported files is:

| Kind | Default Directory |
| --- | --- |
| mesh | `meshes/` |
| material | `materials/` |
| texture | `textures/` |
| sound | `sounds/` |
| map | `maps/` |
| prefab | `prefabs/` |
| animation clip | `animations/` |
| animation graph | `animations/` |

These defaults are conventions, not discovery requirements.

## Renderer Backends

The engine has two renderer backends behind one interface (`IRenderer`, `engine/render_backend.h`):

| Backend | Platforms |
| --- | --- |
| `vulkan` | Windows, Linux |
| `d3d11` | Windows |

Which backends are compiled in is set per platform at configure time, in priority order:

```txt
KES_RENDERERS_WINDOWS = "vulkan;d3d11"   (default)
KES_RENDERERS_LINUX   = "vulkan"
KES_RENDERERS_MACOS   = "vulkan"
KES_ALLOW_RENDERER_SELECTION = ON
```

- the first entry is the default renderer
- the other entries can be chosen at runtime and are used as automatic fallbacks if the chosen one fails to initialize
- a list with a single entry forces that renderer on the platform (no runtime selection; the other backend is not compiled or linked)
- `KES_ALLOW_RENDERER_SELECTION=OFF` always uses the first entry, keeping the others only as fallbacks

Examples:

```txt
cmake .. -DKES_RENDERERS_WINDOWS="d3d11;vulkan"   # D3D11 default, Vulkan selectable
cmake .. -DKES_RENDERERS_WINDOWS=vulkan           # force Vulkan on Windows
```

Runtime selection, highest priority first (only when selection is allowed):

1. command line: `game.exe -renderer=d3d11` (or `--renderer vulkan`)
2. environment variable: `KES_RENDERER=vulkan`
3. game code: `app.config.renderer = RendererBackend::D3D11;`
4. platform default (first entry of the list)

## Shaders

Shaders are written once in [Slang](https://shader-slang.org) and compiled at build time by `slangc` from `external/slang` (`cmake/KesShaders.cmake`). Nothing is compiled at runtime and the engine no longer depends on shaderc or d3dcompiler.

- engine shaders live in `engine/shaders/` and are deployed to `<exe>/content/engine/shaders/`
- every `*.slang` file under `game/content/` is compiled and deployed next to its source in `<exe>/content/`
- each file is compiled once per stage for every enabled backend:

```txt
shaders/foo.slang -> shaders/foo.vs.spv  shaders/foo.ps.spv    (Vulkan, SPIR-V 1.3)
                     shaders/foo.vs.dxbc shaders/foo.ps.dxbc   (D3D11, SM 5.0)
```

Writing a shader:

- entry points are `[shader("vertex")] vsMain` and `[shader("fragment")] psMain`
- `#include "kes_common.slang"` for the engine constant buffers (`SceneCB`, `ModelCB`, `MaterialCB`), the mesh vertex input and the helpers `KesShadeMeshVertex`, `KesShadeMeshPixel` and `KesApplyFog`
- `engine/shaders/mesh_fallback.slang` is a good starting template
- files without entry points are include files: they are not compiled on their own, but editing one rebuilds the shaders around it
- `KES_TARGET_SPIRV` / `KES_TARGET_DXBC` are defined for the rare backend-specific line
- matrices are row-major, multiplied as `mul(vector, matrix)`

Reference the shader from a material through a `.shader` descriptor (`Source = "shaders/foo.slang"`, see `file-formats.md`). Compile errors show up as normal build errors.

Note: with Visual Studio generators, the first build after adding or removing a `.slang` file regenerates the project, and after a removal it can fail once. Build again and it goes through. Ninja does not have this problem.

## Import Tool: `model-importer`

`model-importer` is the current asset import CLI.

Usage:

```txt
model-importer [-assetpath <content-root>] <input> [output]
```

If `output` is omitted, the tool writes next to the input file using the appropriate runtime extension.

Supported imports:

| Source | Output |
| --- | --- |
| `.obj`, `.gltf`, `.glb`, `.fbx` | `.kmodel` |
| `.png`, `.jpg`, `.jpeg`, `.bmp`, `.tif`, `.tiff` | `.ktex` |
| `.wav`, `.mp3`, `.ogg` | `.ksound` |

Examples:

```txt
model-importer source/robot.glb assets/meshes/robot.kmodel
model-importer source/albedo.png assets/textures/albedo.ktex
model-importer source/impact.wav assets/sounds/impact.ksound
```

## Sound Import Notes

When sound is imported:

- source audio is decoded first
- mono and stereo are supported
- runtime output is `.ksound`
- `.ksound` stores interleaved `float32` PCM samples

That means imported MP3/Ogg compression does not remain in the runtime asset.

## Pak Tool: `pak-tool`

`pak-tool` builds a `.pak` archive from a source directory.

Usage:

```txt
pak-tool <source-dir> <output.pak> [--encrypt <hex-key> | --generate-key]
```

Examples:

```txt
pak-tool game/assets out/assets/game_data0.pak
pak-tool game/assets out/assets/game_data0.pak --generate-key
pak-tool game/assets out/assets/game_data0.pak --encrypt 001122...
```

Behavior:

- walks the source directory recursively
- stores files with normalized content-relative paths
- preserves the runtime folder layout inside the archive
- can create encrypted or unencrypted pak files

## Pak Encryption

Pak encryption is optional.

Rules:

- unencrypted pak files load without any key
- encrypted pak files require a 64-hex-character key
- the runtime key comes from `ENGINE_PAK_ENCRYPTION_KEY_HEX`
- `pak-tool --generate-key` prints a generated key after packing
- the runtime accepts mixed encrypted and unencrypted pak files as long as encrypted ones have the correct key available

Important limitation:

- encryption hides asset bytes, but it is not authenticated signing or tamper-proof integrity

## Typical Workflow

Loose-file workflow:

1. Import source files into runtime asset formats under `assets/`.
2. Open the editor project.
3. The editor scans `AssetRoot` and shows recognized assets by extension.
4. Run the game against loose content.

Packed workflow:

1. Import source files into runtime asset formats under `assets/`.
2. Build one or more pak files from that asset directory.
3. Place the `.pak` files in the runtime content directory.
4. Set `ENGINE_PAK_ENCRYPTION_KEY_HEX` if encrypted pak files are used.
5. Run the game. The engine mounts all `.pak` files in the content directory at startup.

## Current Boundaries

The current pipeline is intentionally simple:

- import is explicit, not automatic background cooking
- `.pak` archives are supported, but there is no broader build/cook/package database yet
- the editor is extension-driven, not asset-database-driven

That keeps the workflow transparent, but it also means content authors are still working close to the engine's runtime formats.
