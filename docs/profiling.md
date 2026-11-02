# Profiling (Tracy)

The engine is instrumented with [Tracy](https://github.com/wolfpld/tracy) (source in `external/tracy`, release v0.14.1, network protocol 82).
Profiling is off by default. With it off, every profiler macro compiles to nothing.

## Building with the profiler

```
cmake --preset x64-profile                 # RelWithDebInfo, KES_PROFILING=ON
# or by hand:
cmake -S . -B out/build/profile -DKES_PROFILING=ON
cmake --build out/build/profile --config RelWithDebInfo --target game
```

Profile optimized builds (`RelWithDebInfo`). Debug timings don't match what players get.

### Switches

| CMake option | Default | Meaning |
|---|---|---|
| `KES_PROFILING` | OFF | Master switch. Links Tracy and defines `KES_PROFILING=1` |
| `KES_PROFILE_<CATEGORY>` | ON | Zones for one subsystem (only used when `KES_PROFILING` is ON) |
| `KES_PROFILER_ON_DEMAND` | OFF | Collect data only while the viewer is connected |

Each category is a separate preprocessor define (`KES_PROFILE_RENDER=0/1`, ...) set on the engine target, and it is public, so game code sees the same values.

| Category | What it covers |
|---|---|
| `CORE` | `EngineTick`, startup and shutdown, SDL init, settings, frame rate limiter |
| `INPUT` | event pump, each `SDL_PollEvent`, input begin/end frame, action evaluation |
| `WINDOW` | window event handling, mode changes, cursor, minimized wait |
| `RENDER` | renderer CPU work: draw list build, opaque/transparent recording, cache misses (mesh upload, shader/pipeline creation, textures), fence wait, acquire, submit, present |
| `GPU` | GPU timestamp zones (D3D11 queries / Vulkan timestamps), shown in Tracy's GPU lane |
| `WORLD` | world update, fixed update loop, map load/instantiate, entity manager, animation |
| `ENTITIES` | one zone per entity `Start`/`Update`/`FixedUpdate`, tagged with the C++ class name |
| `PHYSICS` | collision queries (checks, sweeps, raycasts), built-in entity physics |
| `ASSETS` | asset loads (models, meshes, textures, materials, shaders, sounds, clips, maps), tagged with the path |
| `AUDIO` | audio update, mixing, clip loading |
| `IO` | pak mounting, asset file reads, log writes |
| `UI` | RmlUi event handling, layout/update, draw list building |

`ENTITIES` adds one zone per entity per frame. Turn it off (`-DKES_PROFILE_ENTITIES=OFF`) when a map has thousands of entities.

## Capturing

1. Build the Tracy viewer from `external/tracy/profiler` with the same version as the client (see `external/tracy/public/common/TracyVersion.hpp`), or download the matching release.
2. Run the profiling build of the game.
3. In the viewer, connect to `127.0.0.1`. The client only listens on localhost (`TRACY_ONLY_LOCALHOST`).

Without on-demand mode, data is buffered from launch, so startup and map loading show up even if you connect later.

You also get:
- plots: frame time, SDL events per frame, fixed steps per frame, opaque and transparent draw items
- every `EngineLog`/`GameLog` line in the message list, placed on the timeline

## Reading the capture

- **`Present` / `vkAcquireNextImageKHR` / `Wait for GPU (in-flight fence)` are long**: the CPU is waiting on the GPU or on vsync. Check the GPU lane and turn vsync off to tell them apart.
- **`Fixed update loop` runs several steps per frame**: frames are slower than the fixed timestep, which makes the next frame slower too.
- **`... (cache miss)` zones during gameplay**: resources are created on first use. These show up as hitches.
- **`Log write` is visible every frame**: something is logging every frame.

## Adding zones

```cpp
#include "profiler.h"

void MyThing::Update(float dt)
{
    KES_ZONE_FN(WORLD);                           // zone named after the function
    {
        KES_ZONE(PHYSICS, "Resolve contacts");    // name must be a string literal
        KES_ZONE_TEXTF(PHYSICS, "%d bodies", n);  // text on the zone above
        KES_ZONE_VALUE(PHYSICS, n);
        ...
    }
    KES_PROFILE_PLOT("Bodies", static_cast<int64_t>(n));
}
```

Only one `KES_ZONE` per C++ scope. Open a `{ }` block for the next one.
Category arguments are bare names: `CORE`, `INPUT`, `WINDOW`, `RENDER`, `GPU`, `WORLD`, `ENTITIES`, `PHYSICS`, `ASSETS`, `AUDIO`, `IO`, `UI`.

Other macros: `KES_PROFILE_FRAME()` (the engine already calls it once per tick), `KES_PROFILE_MESSAGE(txt, len)`, `KES_PROFILE_MESSAGE_L("literal")`, `KES_PROFILE_THREAD_NAME("name")`, `KES_PROFILE_IS_CONNECTED()`.

To add a category, add it to `KES_PROFILE_CATEGORIES` in `cmake/KesProfiler.cmake`, then add its default, its forced-off value and its color to `engine/profiler.h`.

## Notes

- Tools that link the engine (`pak-tool`, `model-importer`) also get Tracy when profiling is ON. Packaging works, but use a non-profiling build for release packages.
- Tracy listens on a network port while the game runs. Never ship a build with `KES_PROFILING=ON`.
