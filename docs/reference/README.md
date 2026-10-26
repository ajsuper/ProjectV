# Reference

One page per module, the file formats, the shader library, the glossary.

## The API reference

| Where | How |
|---|---|
| **The headers** | `include/`: every declaration is documented in place. The authority. |
| **Doxygen** | [API reference](../api/index.html), from the same comments. `cmake --build build --target docs`. |
| **These pages** | Which header has what. |

## Modules

| Page | Headers | Namespace |
|---|---|---|
| [core](core.md) | `include/core/`: application, events, time, world, log, math, paths | `projv`, `projv::core` |
| [data_structures](data-structures.md) | `include/data_structures/`: `Scene`, `Chunk`, `GPUData`, renderer specs | `projv` |
| [utils](utils.md) | `include/utils/`: compose I/O, the component tree, editing, materials, picking | `projv::utils` |
| [graphics](graphics.md) | `include/graphics/`: window, input, renderers, GPU upload | `projv::graphics` (`projv` for `Input`) |
| [runtime](runtime.md) | `include/runtime/`: Scene bridge, entities, prefabs, `NetId`, physics, fracture | `projv::runtime` (`projv` for components) |

## Also here

| Page | What |
|---|---|
| [File formats](formats/README.md) | `compose.json`, `.data`, `entities.json`, `physics_materials.json`, renderer folders, tree64 |
| [Shader library](shader-library.md) | `pjv_utils_DDA.sc`: bindings, queries, hits, materials |
| [Glossary](glossary.md) | Engine terms |
