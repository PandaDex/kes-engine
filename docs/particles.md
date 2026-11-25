# Particles

Particle systems are `.kfx` files, edited in Forge's **Particle System** tab and simulated on the CPU by the engine. The editor preview runs the same simulation code as the game.

Particles can be lights: a **Light renderer** turns every particle (or a share of them) into a shadowless point light in the renderer's clustered light list. Up to 1024 such lights can be on screen; they don't use the 32 regular light slots.

## Structure

A system holds **emitters**. Each emitter has a stack of **modules** per stage, run top to bottom, and a list of **renderers**:

| Stage | Runs | Typical modules |
|---|---|---|
| Emitter Update | once per frame per emitter | Spawn Rate, Spawn Burst, Spawn Per Unit |
| Particle Spawn | once for every new particle | Initialize Particle, Shape Location, Add Velocity, Inherit Velocity, Initialize Rotation, Set Attribute |
| Particle Update | every particle, every frame | Gravity, Drag, Linear Force, Curl Noise, Vortex, Point Attraction, Limit Speed, Scale Color, Scale Sprite Size, Scale Mesh Size, Sub UV Animation, Collision, Kill Volume, Kill If, Set Attribute |
| Render | when drawing | Sprite, Ribbon, Mesh, Light |

After the Particle Update modules the particles move (position += velocity × dt, rotations spin); Collision and Kill Volume run after that move.

Emitter properties: name, local space (particles follow the system's transform), max particles, duration (0 = endless), loop and loop count, delay, seed (0 = new random sequence each activation) and custom attributes.

The module catalogue (parameters, defaults, descriptions) is `ParticleModuleCatalog()` / `ParticleRendererCatalog()` in `engine/particle_asset.cpp`; the editor builds its menus from it.

## Parameter values

Every numeric parameter of a module or renderer is a **value** with one of these modes:

| Mode | Meaning |
|---|---|
| Constant | a number, vector or color (HDR allowed) |
| Random | between Min and Max, fixed per particle. *Uniform* uses one random number for every component (keeps a size square). |
| Curve | Scale × curve(input) |
| Gradient | an RGBA gradient over the input |
| Graph | the output of a node graph |

Curve and gradient inputs: NormalizedAge (0 at birth, 1 at death), Age, Speed, Random (per particle), EmitterTime, NormalizedLoopTime. Renderer values are evaluated per particle too: a light's radius can shrink with age, a sprite's emissive can flicker.

## Graphs

Set a value to **Graph** and Forge creates a small node graph for it, opened in the Graph panel under the preview. A graph has one Output node; its input is the value the parameter gets. Nodes (`engine/particle_graph.cpp`):

- Inputs: Particle Attributes (position, velocity, color, size, age, normalized age, lifetime, random, mass, id, speed), Custom Attribute, Emitter (time, loop time, delta time, position), User Parameter
- Constants: Float, Vector, Color
- Math: Add, Subtract, Multiply, Divide, Min, Max, Power, Lerp, Clamp, Saturate, Abs, Negate, One Minus, Sin, Cos, Frac, Floor, Sqrt, Step, Smoothstep, Remap, Sine Wave
- Vector: Length, Normalize, Dot, Cross, Distance, Make Vector, Make Color, Split
- Random: Random Range (per particle or per frame), Noise (smooth 3D noise, scalar and vector)
- Curves: Curve, Gradient (keys as text)

Values are float4 per particle; scalars widen to vectors, and wider values are cut to what the parameter needs. Graphs are compiled once and evaluated for a batch of particles at a time.

**Set Attribute** writes any attribute (built in, or one of the emitter's custom attributes) from a value or graph; **Kill If** kills particles whose Condition is above 0.5. Together with graphs they cover custom behaviour without new code.

## Renderers

- **Sprite**: camera-facing quads, velocity aligned (stretched by speed) or facing a fixed axis. Material (`.mat` base color and base texture), blend mode (Additive, Alpha, Premultiplied), flipbook grid (`SubImages` = columns rows, frame from Sub UV Animation), back-to-front sorting, Lit (ambient, lights and cluster lights on a wrapped normal), Emissive multiplier. Without a base texture a sprite is a soft disc.
- **Ribbon**: a strip through the emitter's particles from newest to oldest (trails). Width per particle, Stretch or Tile texturing.
- **Mesh**: a `.kmodel` per particle, oriented by the particle's mesh rotation or its velocity, scaled by Mesh Scale. Drawn like scene objects (shadows included).
- **Light**: Radius, Intensity and Color (values, so they can follow curves or graphs), Use Particle Color (color × alpha of the particle), Fraction (share of particles that emit light, chosen by the particle's random number) and MaxLights per emitter. Over MaxLights, the particles with the lowest random numbers keep their lights (the same ones every frame) and take over the energy of the dropped ones, so the total brightness doesn't flicker.

Sprite and ribbon batches are drawn in this order: `SortOrder` (higher on top), then alpha-blended before additive (additive glow never pops behind smoke when the camera moves), then far to near.

## Lights and the clustered light list

The renderer divides the view into a 16 × 9 × 24 grid of clusters (exponential depth slices) and bins every point light into the clusters its sphere touches (`render::BuildClusters` in `render_common.cpp`, on the CPU). Shaders look up their pixel's cluster and loop over its lights after the regular lights (`KesShadeMaterial` in `kes_common.slang`), so every material, including custom material shaders, is lit by particles. Limits: 1024 lights per frame, 255 per cluster, 96K cluster entries. Cluster lights don't cast shadows.

Game code can add lights to the same list for one frame:

```cpp
ScenePointLight flash = {};
flash.position[0] = p.x; flash.position[1] = p.y; flash.position[2] = p.z;
flash.radius = 6.0f;
flash.color[0] = 4.0f; flash.color[1] = 3.0f; flash.color[2] = 2.0f;
world.GetScene().SubmitPointLight(flash); // cleared at the start of every World::Update
```

## In maps

Place a **ParticleSystemEntity** (`System` = the `.kfx`, `AutoActivate`, `Scale`, and `User.<name>` keys for user parameters). It follows the entity's position and rotation.

MapScript inputs: Activate (resume spawning), Deactivate (stop spawning, live particles finish), Toggle, Restart, Kill (remove every particle), SetUserParameter (`"Heat 2"`). Output: OnFinished (every emitter finished and the last particle died).

## From code

```cpp
ParticleWorld& particles = world.GetParticles();          // or game->particles in an entity
ParticleSystemHandle h = particles.Spawn("particles/campfire.kfx", position, rotationRadians,
                                         /*autoDestroy*/ false);
if (ParticleSystemInstance* fx = particles.Find(h))
{
    fx->SetTransform(newPosition, rotationRadians);
    fx->SetUserFloat("Heat", 2.0f);
    fx->Deactivate();
}
particles.Destroy(h);

// One-shot effect, removed by itself once it completes:
particles.Spawn("particles/explosion.kfx", hitPoint);
```

User parameters are system-level floats declared in the System details of the editor and read in graphs with the User Parameter node.

## Forge: Particle System tab

File > New > Particle System, Assets > New > Particle System, or open any `.kfx`.

- **System** panel (left): System, then each emitter with Properties, its stages and its renderers. Checkboxes enable or disable emitters, modules and renderers. Right-click or **+ Module** adds modules to the selected stage (or renderers under Render); **+ Emitter** adds an emitter from a template (Fountain, Sparks with lights, Fire, Smoke, Fireflies with lights, Trail, Debris meshes). Double-click an emitter to rename it.
- **Details** panel (right): the selected item's settings. Each value has a mode box; curves and gradients are edited in place (drag keys, double-click to add, right-click to remove).
- **Graph** panel (under the preview): switch a value's mode box to **Graph** and a graph is created for it and shown here. Selecting a module or renderer whose values already use a graph opens the first one; *Open graph* opens a specific one. Add nodes with Tab or right-click on the canvas.
- **Preview**: the system over a floor lit by the particle lights. Play/Pause (Space), Restart (Ctrl+R), speed, Loop, Move (moves the system in a circle for trails and Spawn Per Unit), Floor, Frame (F), and the preview lighting (Day is close to the game's default ambient; Dusk and Night show particle lights best, and lit sprites such as smoke darken accordingly). Orbit with the left mouse button, pan with the right or middle one, zoom with the wheel. The status bar shows particle and light counts and the simulation time; graph errors appear on its left.

Edit > Remove Unused Graphs deletes graphs no parameter uses any more.

## File format

`.kfx` uses the engine's text block format (like `.mat`): a `System` block, `UserParam` blocks, then per emitter an `Emitter` block followed by `Module`, `Renderer`, `Graph`, `GraphNode` and `GraphLink` blocks that name their emitter by index. Module parameters are `Param.<Name> = "<value>"`, values as `const 1 2 3`, `random uniform 0 0 ; 1 1`, `curve NormalizedAge 1 ; 0 0 ; 1 1`, `gradient NormalizedAge ; 0 1 1 1 1 ; 1 1 1 1 0` or `graph 3`. See `LoadParticleSystemFile` / `WriteParticleSystemText` in `engine/particle_asset.cpp` and the sample `game/content/particles/campfire.kfx`.

## Code

| File | |
|---|---|
| `engine/particle_asset.*` | data, module catalogue, `.kfx` load/save |
| `engine/particle_graph.*` | graph node catalogue, compiler and evaluator |
| `engine/particle_sim.*` | `ParticleSystemInstance` (simulation), sprite/ribbon geometry, light and mesh extraction |
| `engine/particle_world.*` | `ParticleWorld`: the world's systems, asset cache, scene render data |
| `engine/ent_particle_system.*` | `ParticleSystemEntity` |
| `engine/shaders/particle.slang` | sprite and ribbon shader |
| `tools/forgeMapEditor/src/modules/particles` | the Forge tab |

The simulation files depend only on `text_block_file` and the math types, so Forge compiles them directly (`forge_engine` in Forge's CMakeLists).
