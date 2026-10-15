# Concepts

How the engine works, one idea per page. Read them in order the first time. Each one assumes the
ones above it.

| Page | Covers |
|---|---|
| [Architecture](architecture.md) | The five modules, what may include what, namespaces |
| [The frame loop](frame-loop.md) | `Application`, stages, systems, `Time`, fixed steps, `Events` |
| [The scene](scene.md) | `Scene`: components, chunks, grids, the geometry pool, handles and refs |
| [Voxel data](voxel-data.md) | tree64, brick maps, material bytes, resolution, LOD |
| [Materials](materials.md) | Per-component palettes, the four material words, physics materials |
| [Editing](editing.md) | Edit queues, `updateScene`, copy-on-write, getting edits to the GPU and to disk |
| [Rendering](rendering.md) | Renderers as data, passes, the GPU mirror of the scene, uniforms |
| [Entities](entities.md) | The runtime: entities linked to components, authored entities, prefabs, `NetId` |
| [Physics](physics.md) | Bodies from voxels, ticks and commands, determinism, fracture |
| [Where data goes](where-data-goes.md) | Which file, field or component a piece of data belongs in |
