# data_structures

The plain types the other modules operate on. Namespace `projv`. Never includes EnTT or the runtime.

Explained in: [The scene](../concepts/scene.md), [Voxel data](../concepts/voxel-data.md),
[Materials](../concepts/materials.md), [Rendering](../concepts/rendering.md).

## `data_structures/scene.h`

[`include/data_structures/scene.h`](../../include/data_structures/scene.h): everything loaded.

| Name | What |
|---|---|
| `Scene` | `components`, `chunks`, `grids`, `looseChunks`, `geometryPool`, `attachments`, `slots`, `deletions`, `documentPath` |
| `ComponentRecord` | One component: `kind`, `name`, `localId`, `parent`/`children`, local transform, `materialPalette`, `editQueue`, `generation` |
| `ComponentKind` | `Chunk`, `Grid`, `Asset` |
| `ComponentHandle`, `ChunkHandle` | Row indices; `INVALID_COMPONENT_HANDLE`, `INVALID_CHUNK_HANDLE` |
| `ComponentRef` | Handle + generation: safe across frames |
| `Chunk`, `ChunkHeader` | A placed cube of voxels; `geometryPoolIndex`, `gridIndex`/`cellIndex`, `alive`, `requestedLOD` |
| `GPUChunkHeader` | The packed header row the shader reads (`GPU_HEADER_TEXELS` texels) |
| `SceneGrid` | `origin`, `cellSize`, `dims`, `rotation`, `cellToChunk`, `originCellCoord`, `rendered` |
| `GeometryBlob` | `geometry`, `materialIDs`, `envelope`, `brickMap`, `refCount`, `contentStamp`, `dirty`, `renderLOD` |
| `Material` | Four GPU words + `name` + `physics`; `pack*` / `material*` accessors, `setMaterialAnimation` |
| `Mutability` | `Locked`, `Direct`, `Copy` |
| `PendingVoxelOp`, `ComponentEditQueue`, `DataReference` | Edit queue entries |
| `AttachmentStore`, `AttachmentTable`, `AttachmentScope`, `OnDuplicate` | Attachment storage (use `utils/attachments.h`) |
| `poolInsertBlob`, `releaseBlob`, `internChunkGeometry`, `forkBlob` | Geometry pool operations |
| `markBlobContentChanged(blob)` | Call after replacing `geometry` in place |
| `setBlobRenderLOD`, `requestChunkLOD`, `replaceChunkGeometry` | LOD and regenerated geometry |

## `data_structures/voxel.h`

[`include/data_structures/voxel.h`](../../include/data_structures/voxel.h): brick maps and tree64 bits.

| Name | What |
|---|---|
| `VoxelBrickMap`, `BrickData` | The editable form: 64³ bricks, bitmask + slot map |
| `BRICK_SIZE` (64) and friends | Brick constants |
| `TREE64_LEAF_FLAG`, `TREE64_UNIFORM_FLAG`, `encodeLeafNode`, `tree64IsLeaf`, `tree64LeafIsUniform`, `tree64LeafMatOffset` | A node's third word |
| `clampRenderLOD`, `resolutionAtLOD` | Storage LOD maths (4× per step) |
| `packColor(Color)`, `unpackColor` | 8-bit RGB ↔ R10G10B10 |
| `Voxel`, `VoxelGrid`, `VoxelBatch` | Older Z-order voxel lists |

## `data_structures/compose.h`

[`include/data_structures/compose.h`](../../include/data_structures/compose.h): in-memory forms of
the files. Formats: [compose.json](formats/compose-json.md), [.data](formats/data-file.md).

| Name | What |
|---|---|
| `ComposeDoc`, `ComposeComponent`, `ComponentType` | A parsed `compose.json` |
| `DataFile`, `DataBlock` | A parsed `.data`: resolution, voxelScale, blocks (a tile table, or a v2–v3 tree64 + material bytes; envelope) |
| `DataFileHeader`, `BlockEntry` | The block table alone, for streaming |

## `data_structures/gpuData.h`

[`include/data_structures/gpuData.h`](../../include/data_structures/gpuData.h): the scene on the GPU.

| Name | What |
|---|---|
| `GPUData` | The seven scene textures and samplers, allocators, palette offsets, `blobRanges`, `animation` |
| `GPUBlobRange` | Where one blob lives in the textures, and at what LOD |

## `data_structures/constructedRenderer.h`

[`include/data_structures/constructedRenderer.h`](../../include/data_structures/constructedRenderer.h):
a renderer as bgfx objects.

| Name | What |
|---|---|
| `ConstructedRenderer` | `resources` + `dependencyGraph` (the passes) |
| `BGFXResources` | Handles, uniform values, textures, framebuffers, and the engine uniforms `passTargetRes`, `passInputRes`, `pjvMotionSets`, `pjvAnimTime` |
| `BGFXDependencyGraph` | One pass: inputs, target, program, `enabled` |
| `PROJV_MAX_PASS_SAMPLERS` (9), `PROJV_MAX_PASS_INPUTS` (8), `PROJV_MOTION_SET_VEC4S` (48) | Limits |

## `data_structures/rendererSpecification.h`, `resources.h`, `dependencyGraph.h`

[`rendererSpecification.h`](../../include/data_structures/rendererSpecification.h),
[`resources.h`](../../include/data_structures/resources.h),
[`dependencyGraph.h`](../../include/data_structures/dependencyGraph.h): a renderer as read from its
folder, before construction. Paths: `data_structures/rendererSpecification.h`,
`data_structures/resources.h`, `data_structures/dependencyGraph.h`.

| Name | What |
|---|---|
| `RendererSpecification` | `resources` + `dependencyGraph` |
| `Resources` | `uniforms`, `shaders`, `textures`, `FrameBuffers` |
| `DependencyGraph`, `RenderPass` | The pass list: shader, inputs, output, `multiPass` |

## `data_structures/texture.h`, `framebuffer.h`, `uniform.h`, `shader.h`

[`texture.h`](../../include/data_structures/texture.h),
[`framebuffer.h`](../../include/data_structures/framebuffer.h),
[`uniform.h`](../../include/data_structures/uniform.h),
[`shader.h`](../../include/data_structures/shader.h): the entries of `resources.json`. Paths:
`data_structures/texture.h`, `data_structures/framebuffer.h`, `data_structures/uniform.h`,
`data_structures/shader.h`.

| Name | What |
|---|---|
| `Texture` | `textureID`, `format`, `sizeMode` (`Fixed`/`Relative`), `scale`, `origin`, `pingPongFlag`, `readBack` |
| `TextureOrigin`, `TextureSizeMode` | `CreateNew`/`CPUBuffer`; `Fixed`/`Relative` |
| `ConstructedTextures` | Handles, sizes, formats, flags per texture id |
| `FrameBuffer`, `ConstructedFramebuffers` | Texture ids per framebuffer; handles, ping-pong pairs |
| `Uniform`, `UniformType` | `Vec4`, `Mat3x3`, `Mat4x4` |
| `Shader` | `shaderID`, `filePath` |

## Small types

| Header | Path | What |
|---|---|---|
| [`camera.h`](../../include/data_structures/camera.h) | `data_structures/camera.h` | `Camera` and a global `cam` (legacy; examples keep their own) |
| [`color.h`](../../include/data_structures/color.h) | `data_structures/color.h` | `Color`: 8-bit RGB |
| [`nodeStructure.h`](../../include/data_structures/nodeStructure.h) | `data_structures/nodeStructure.h` | `nodeStructure`, `nodeStructureTree64`: tree-building scratch |
| [`posTexVertex.h`](../../include/data_structures/posTexVertex.h) | `data_structures/posTexVertex.h` | `PosTexVertex`: the full-screen quad's vertex |
