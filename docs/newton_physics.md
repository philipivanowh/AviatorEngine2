# Newton physics in AviatorEngine

How the physics layer is put together: what each piece owns, how data moves
between C++ and Newton every frame, and the exact contract between the C++
backend and the Python bridge.

## The pieces

| Layer | File | Owns |
|---|---|---|
| Components | `scene/components.h` | `RigidbodyComponent`, `ColliderComponent`, `ShapeOfComponent` - what a scene builder authors |
| Authoring helpers | `scene/scene.h` | `Scene::AddPhysicsBox` and friends: one call creates the render shape and its physics components together |
| Public API | `physics/physicsWorld.h` | `IPhysicsWorld`, `PhysicsSettings`, `CreatePhysicsWorld` - the only physics header game code includes |
| Factory + Null backend | `physics/physicsWorld.cpp` | Picks the backend; the do-nothing world used when Newton is unavailable |
| Newton backend | `physics/newtonPhysicsWorld.h/.cpp` | Embeds CPython through pybind11, turns components into bridge calls, writes `TransformComponent` back |
| Python bridge | `physics/newton/aviator_newton.py` | `World`: builds the Newton model, steps it on the GPU, hands poses back |

Every frame:

```
main loop ── Update(registry, dt) ──▶ NewtonPhysicsWorld (C++)
   kinematic TransformComponents ──▶ set_kinematic_poses ─┐
   queued forces / impulses      ──▶ set_wrenches, apply_impulses
                                      step(n)              │  aviator_newton.World (Python)
                                      Newton + Warp, one CUDA graph per fixed step
   TransformComponent ◀── interpolate + sleep test ◀── poses ┘
```

## Rules

1. `Build` reads components. During a frame, physics writes only `TransformComponent`.
2. Kinematic bodies are the one exception, in the other direction: physics reads their `TransformComponent`.
3. No Python or pybind11 type appears outside `physics/newtonPhysicsWorld.cpp`.
4. Units are SI (m, kg, s) and the world is Y-up. Quaternions are stored x, y, z, w - the memory order of both `Quat<float>` and Newton's `body_q`. Note that `Quat<float>`'s four-argument constructor takes (w, x, y, z).
5. The set of bodies is fixed between `Build` calls. Adding or removing physics entities means calling `Build` again.
6. A dynamic box renders as a mesh box. Analytic boxes only express rotation about Y.

## Using it from C++

```cpp
#include "physics/physicsWorld.h"

PhysicsSettings settings; // XPBD on the GPU, 60 Hz fixed step, 10 substeps
std::unique_ptr<IPhysicsWorld> physics = CreatePhysicsWorld(settings);

BuildPhysicsScene(scene, registry); // authors bodies with scene.AddPhysicsBox(...) and friends
physics->Build(registry);           // slow the first time: compiles GPU kernels

while (running)
{
    const PhysicsStepResult step = physics->Update(registry, dt);
    if (step.moved)
    {
        renderer.MarkSceneDirty();
    }
}
```

## Python bridge contract

Module `aviator_newton` (file `physics/newton/aviator_newton.py`) exposes
`BRIDGE_VERSION = 1` and the class `World`. All calls come from one thread.
Bad input raises a Python exception with a readable message, which the C++
side logs.

Array conventions:

- Poses: `float32`, shape `(N, 7)`: `px, py, pz, qx, qy, qz, qw` - world space, body origin.
- Wrenches: `float32`, shape `(N, 6)`: `fx, fy, fz, tx, ty, tz` - world space, about the center of mass.
- Indices: `int32`, shape `(K,)`, body indices.
- `N` is `body_count()`, and body index `i` is the `i`-th `add_body` call.
- An array passed in may be a view of C++ memory. Read it or fill it in place, and keep no reference to it after the call returns.

Enums use the same numbers as the C++ enums:

- body kind: `0` dynamic, `1` kinematic (`RigidbodyType`)
- collider shape: `0` sphere, `1` box, `2` capsule, `3` plane (`ColliderShape`)

### `World(settings: dict)`

| Key | Type | Meaning |
|---|---|---|
| `gravity` | 3 floats | m/s², world space |
| `fixed_dt` | float | simulated seconds per fixed step |
| `substeps` | int | solver substeps per fixed step |
| `solver` | str | `"xpbd"`, `"mujoco"`, `"featherstone"` or `"semi_implicit"` |
| `solver_iterations` | int | iterations, for the solvers that iterate |
| `device` | str | `"cuda"` or `"cpu"`. `"cuda"` falls back to the CPU with a warning when there is no GPU. |
| `use_cuda_graph` | bool | record one fixed step as a CUDA graph and replay it |

### Building the world

- `add_body(kind, position, rotation, mass, linear_velocity, angular_velocity) -> int`
  `position` is 3 floats, `rotation` 4 floats (x, y, z, w), velocities are world space. Returns the body index.
- `add_collider(body, shape, local_position, local_rotation, half_extent, radius, half_height, friction, restitution) -> int`
  `body = -1` makes static geometry, and the local pose is then a world pose. Capsules run along local Y; a plane's normal is local +Y. Returns the collider index, in call order.
- `finalize() -> None`
  Builds the model, solver and collision pipeline. Each body's `mass` is spread over its colliders by volume. Also compiles the kernels and records the CUDA graph, then restores the initial state - so the first `step` has no hitch and no body has moved.

### Simulating

- `step(num_steps, previous_poses_out=None) -> None`
  Advances `num_steps` fixed steps. If `previous_poses_out` `(N, 7)` is given, it is filled with the poses from before the final fixed step, for interpolation. When `num_steps` is 1, those are the poses at the start of the call.
- `read_body_poses(out) -> None`
  Fills `out` `(N, 7)` with the current poses.
- `write_body_poses(poses) -> None`
  Teleports every body to `poses` `(N, 7)` and zeroes every velocity and pending wrench.
- `set_body_velocities(indices, linear, angular) -> None`
  `linear` and `angular` are `(K, 3)`, world space.
- `apply_impulses(indices, impulses) -> None`
  `impulses` `(K, 3)`: linear velocity += impulse × inverse mass. Kinematic bodies ignore it.
- `set_wrenches(wrenches) -> None`
  `(N, 6)`, applied during every substep of the next `step` call only.
- `set_kinematic_poses(indices, poses) -> None`
  `poses` `(K, 7)`: where those kinematic bodies must be at the end of the next `step` call. They move there at constant linear and angular velocity, so contacts push dynamic bodies realistically. A kinematic body with no new target holds still.

### Info and teardown

- `body_count() -> int`
- `description() -> str`, e.g. `"Newton 1.6.0 / Warp 1.17.0 (XPBD, cuda:0, CUDA graph)"`
- `close() -> None` - releases GPU resources. Safe to call twice.

## Build and run

Newton is optional in the strong sense: with `AVIATOR_WITH_NEWTON=OFF`, or with
its Python environment missing, the factory falls back to the Null backend and
the app still builds, runs and renders. Nothing moves, and CMake says why.

Create the environment once:

```
python -m venv .venv-newton
.venv-newton/Scripts/python -m pip install newton
```

Then configure and build as usual - CMake finds `.venv-newton` by itself:

```
cmake -S . -B build
cmake --build build --config Debug --target app
```

| Command | What it does |
|---|---|
| `cmake --build build --config Debug --target physics_smoke` | Tests the backend alone: no window, no renderer, exits non-zero if the simulation is wrong |
| `build/bin/app.exe` | The demo scene |
| `build/bin/app.exe --frames 300 --no-mouse-capture` | Unattended run that quits by itself |
| `cmake -S . -B build -DAVIATOR_WITH_NEWTON=OFF` | Build without Newton |
| `cmake -S . -B build -DAVIATOR_NEWTON_VENV=<path>` | Use a different Python environment |

Keys in the demo scene: **P** pause, **N** single step while paused, **R** reset,
**B** blast, **F** fire the cannonball.

### The first run is slow

Warp compiles Newton's GPU kernels the first time they run and caches them under
`%LOCALAPPDATA%\NVIDIA\warp\Cache\<version>`. A cold cache costs minutes; after
that `Build` takes well under a second. This is why `main.cpp` builds physics
*before* it opens the window - otherwise the first launch looks like a hung app.

### Performance, and why Debug does not count

Measured on an RTX 3070 with the 53-body demo scene, 10 substeps at 60 Hz:

| Build | Physics per frame | Result |
|---|---|---|
| Release | 2.3 - 2.9 ms, one step per frame | 60 fps |
| Debug | 100 - 1500 ms, pinned at maxStepsPerUpdate | about 7 fps |

Debug is not a little slower, it is unusable - and it compounds: once a frame
misses the fixed step, every later frame runs the full `maxStepsPerUpdate`, which
makes the next frame slower still, until the clamp is the only thing holding it
together. Build Release or RelWithDebInfo for anything interactive. Debug is for
stepping through code, and `P` (pause) makes it bearable.

`Build` itself costs about 0.55 s for 53 bodies in Release once Warp's kernels
are cached, and 1.8 s in Debug.

Note that the demo's sweeper arm never stops, so the scene never settles and the
path tracer never converges. Pause physics with `P` to let an image resolve.

### Tuning

Penetration of a five-box stack after 3 seconds, measured on an RTX 3070. Raising
iterations is the cheaper fix: substeps cost scales linearly, iterations do not.

| substeps / iterations | bottom box sinks | top box drifts |
|---|---|---|
| 4 / 2 | 3.74 mm | 0.02 mm |
| 8 / 2 | 0.93 mm | 0.00 mm |
| **10 / 2 (default)** | **0.59 mm** | 0.01 mm |
| 10 / 4 | 0.29 mm | 0.00 mm |
| 20 / 2 | 0.15 mm | 0.00 mm |

### Things worth knowing

- **Friction and restitution are averaged between the two touching surfaces**, so
  a ball with restitution 0.8 on a floor with 0.0 bounces at 0.4. Author both.
- **Leave `useCudaGraph` on.** Recording a fixed step as a CUDA graph is worth
  10-15x here: 2.4 ms per step for 60 bodies, against 25-40 ms without it.
- `PhysicsSolver::MuJoCo` needs the `mujoco-warp` package, which this environment
  does not have; XPBD is the default and handles every collider shape.
- Planes must be static. Every body needs at least one collider, or `Build` skips
  it with a warning.
