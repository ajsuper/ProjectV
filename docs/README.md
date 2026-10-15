# ProjectV documentation

ProjectV is a C++20 voxel game engine: tree64 voxel storage, ray-marched rendering through bgfx,
an EnTT runtime, and Jolt physics built from the voxels themselves.

```text
docs/
├── getting-started/   build it, run the examples, write a first program
├── concepts/          how the engine works, one idea per page
├── guides/            one task each, start to finish
├── reference/         per-module API map, file formats, shader library, glossary
├── contributing/      how to work on the engine and on these docs
└── plans/             design notes and delivery logs
```

Plain Markdown; the same files build a searchable site with `cmake --build build --target docs`
([details](contributing/writing-docs.md#building-and-checking)).

## Start here

| If you want to... | Read |
|---|---|
| Build the engine and run something | [Build](getting-started/build.md), then the [examples](../examples/README.md) |
| Write your own program | [Your first app](getting-started/first-app.md) |
| Understand how it fits together | [Architecture](concepts/architecture.md), then the rest of [Concepts](concepts/README.md) |
| Do one specific thing | [Guides](guides/README.md) |
| Look up a function, a type or a file format | [Reference](reference/README.md) |
| Look up a word | [Glossary](reference/glossary.md) |
| Change the engine | [Contributing](contributing/README.md), [known latent issues](plans/known-latent-issues.md) |
| See what is planned or why something was built | [Plans](plans/README.md) |
| Read the generated API docs | [API reference](api/index.html) |

## The engine in one picture

```text
             disk                         memory                          GPU
 ┌───────────────────────────┐   ┌──────────────────────────┐   ┌────────────────────┐
 │ compose.json  (layout)    │──▶│ Scene                    │──▶│ GPUData            │
 │ *.data        (voxels)    │   │  components, chunks,     │   │  tree64, materials,│
 │ entities.json (behaviour) │─┐ │  grids, geometry pool    │   │  headers, palette  │
 └───────────────────────────┘ │ └────────────▲─────────────┘   └─────────┬──────────┘
                               │              │ Scene bridge              │ renderer passes
                               │ ┌────────────┴─────────────┐   ┌─────────▼──────────┐
                               └▶│ World (entt::registry)   │   │ render.json        │
                                 │  entities, Transform,    │   │ resources.json     │
                                 │  RigidBody, gameplay ... │   │ shaders            │
                                 └────────────▲─────────────┘   └────────────────────┘
                                              │ systems, each frame
                                 ┌────────────┴─────────────┐
                                 │ Application              │
                                 │  Startup → [PreUpdate →  │
                                 │  FixedUpdate* → Update → │
                                 │  PostUpdate → Render]    │
                                 └──────────────────────────┘
```
