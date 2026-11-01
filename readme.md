# KES Engine

``` ⚠️ This engine isn't ready for any use. Very early work in progress. Things can and will be broken! ``` <br/>
Lightweight game engine focused on psx style graphics with vertex lighting and performance build with modern C++ 20, SDL3 and Vulkan or DirectX 11 (selectable on Windows). Shaders are written once in Slang and compiled at build time for every renderer.
<br /> 


``⚠️ Documentation is currently AI generated. 
THERE IS NONE AI GENERATED CODE IN ENGINE`` <br/>
Documentation is located in ```docs``` directory.



### Implemented in any way: 

- Lightning
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

### Remaining to be implemented (random order):

- UI System 
- - striped HTML-based
- - limited CSS 
- - zero JS
- LOD
- Particle system
- Interface System
- Packaging Tool
- Compression


# Building 
## External binaries
Prebuilt tools (Slang shader compiler, FBX2glTF) are not stored in git. Download them once (and again whenever `external/external_version` changes): <br/>
Windows: ```powershell -ExecutionPolicy Bypass -File external/download_external.ps1``` <br/>
Linux: ```./external/download_external.sh``` <br/>
To publish new binaries, upload `external.zip` to a release tagged `external-v<N>` and put `<N>` in `external/external_version`.

## Visual Studio 
1. Open the project root in Visual Studio, thanks to CmakePresets it should figure everything out by itself 
2. Select a game target
3. Build target <kbd>Ctrl + Shift + B</kbd> 
4. Done, run the game or editor by switching targets and starting from IDE <br/> 
*NOTE: game target builds all targets*

## CMake Manual
1. Open terminal in repo root 
2. Create folder `build`
3. Go to `build` folder
4. Run cmake generator command: <br/>
```cmake .. -G "Visual Studio 18 2026" -A x64``` <br/>
Replace the Visual Studio version with your installation
5. After command succeeded, build the project: <br/>
```cmake --build .```
6. Done, <br/> 
Game executeable should be in `build\game\Debug\game.exe` <br/>
Editor executeable should be in `build\engine\tools\kes-editor\Debug\kes-editor.exe`
