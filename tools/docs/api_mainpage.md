# ProjectV API reference {#mainpage}

Generated from the comments in `include/`. Concepts, guides and file formats are in `docs/`.

## Modules

| Module | Headers | Namespace |
|---|---|---|
| core | `core/*.h`: application, events, time, world, log, math, paths | `projv`, `projv::core` |
| data_structures | `data_structures/*.h`: Scene, Chunk, GeometryBlob, GPUData, renderer specs | `projv` |
| utils | `utils/*.h`: compose I/O, component tree, editing, materials, picking, collision | `projv::utils` |
| graphics | `graphics/*.h`: window, input, renderers, GPU upload | `projv::graphics` |
| runtime | `runtime/*.h`: Scene bridge, entities, prefabs, NetId, physics, fracture | `projv::runtime` |

## Where to start

| Type or function | Header | What |
|---|---|---|
| projv::Application | core/application.h | The loop: add systems to stages, `run()` |
| projv::Scene | data_structures/scene.h | Everything loaded: components, chunks, grids, geometry |
| projv::utils::loadComposeFromDisk | utils/compose_io.h | A compose folder to a Scene |
| projv::utils::updateScene | utils/editing.h | Apply queued voxel edits |
| projv::graphics::RenderInstance | graphics/render_instance.h | The window and bgfx |
| projv::graphics::createTexturesForScene | graphics/gpu_interface.h | Upload a Scene |
| projv::runtime::installSceneBridge | runtime/scene_bridge.h | Link entities to components |
| projv::runtime::installPhysics | runtime/physics.h | Rigid bodies from voxels |
| projv::runtime::PhysicsWorld | runtime/physics/physics_world.h | The simulation core |
