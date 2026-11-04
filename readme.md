# KES Engine

[![CI](https://github.com/PandaDex/kes-engine/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/PandaDex/kes-engine/actions/workflows/ci.yml) <br/>
``` ⚠️ This engine isn't ready for any use. Very early work in progress. Things can and will be broken! ``` <br/>
Lightweight game engine with modern forward rendering (per-pixel PBR, HDR with MSAA, cascaded and local shadow maps, image-based ambient light from the sky) built with modern C++ 20, SDL3 and Vulkan or DirectX 11 (selectable on Windows). Shaders are written once in Slang and compiled at build time for every renderer.
<br />
Documentation is located in `docs` directory.

### Implemented in any way:

- Lighting (per-pixel PBR, up to 32 lights, shadows, sky ambient; see `docs/rendering.md`)
- Audio System
- Entity System
- Material System
- Input Handling
- Window Handling
- Level System
- Level Editor
- Asset System
- Material/Shader Editor
- Shader System
- Ray cast System
- Animation System
- Graph-based Animation Editor
- Fog
- UI System (HTML/CSS via RmlUi: `.rml` documents, `.rcss` style sheets, data binding, no scripting; showcase sample in `samples/ui`)
- Crash Reporting (Crashpad minidumps, optional upload with player consent; see `docs/crash-reporting.md`)

### Remaining to be implemented (random order):

- LOD
- Particle system
- Interface System
- Packaging Tool
- Compression

# Building

## Submodules

Tracy, RmlUi, FreeType, sentry-native and Box3D are git submodules. Clone with `--recursive`, or fetch them afterwards: <br/>
`git submodule update --init --recursive` <br/>

## External binaries

Prebuilt tools (Slang shader compiler, FBX2glTF) are not stored in git. Download them once (and again whenever `external/external_version` changes): <br/>
Windows: `powershell -ExecutionPolicy Bypass -File external/download_external.ps1` <br/>
Linux: `./external/download_external.sh` <br/>
To publish new binaries, upload `external.zip` to a release tagged `external-v<N>` and put `<N>` in `external/external_version`.

## Tests

Unit tests are in `tests/` and don't need a GPU or a window. Build the `kes_tests` target, then run `ctest --test-dir <build folder> -C Debug --output-on-failure`, or run `kes_tests` directly with part of a test name to filter (`kes_tests pak`). <br/>
`KES_WARNINGS_AS_ERRORS` makes warnings fatal, and `KES_SANITIZE` (or the `linux-sanitize` preset) enables ASan and UBSan. CI runs both on Linux, and warnings as errors on Windows.

## Packaging

Windows: `./package.ps1 all` (zip + GDK msixvc) <br/>
Linux: `./package.sh` (tar.gz) <br/>
Packages are written to `out/package/`, along with a symbols zip to keep for reading crash reports (`docs/crash-reporting.md`). Platforms (Windows, GDK for PC, Linux), formats and pak encryption are described in `docs/tools-and-pipeline.md`.

## Visual Studio

1. Open the project root in Visual Studio, thanks to CmakePresets it should figure everything out by itself
2. Select a game target
3. Build target <kbd>Ctrl + Shift + B</kbd>
4. Done, run the game or editor by switching targets and starting from IDE <br/>
   _NOTE: game target builds all targets_

## CMake Manual

1. Open terminal in repo root
2. Create folder `build`
3. Go to `build` folder
4. Run cmake generator command: <br/>
   `cmake .. -G "Visual Studio 18 2026" -A x64` <br/>
   Replace the Visual Studio version with your installation
5. After command succeeded, build the project: <br/>
   `cmake --build .`
6. Done, <br/>
   Game executeable should be in `build\game\Debug\game.exe` <br/>
   Editor executeable should be in `build\engine\tools\kes-editor\Debug\kes-editor.exe`
