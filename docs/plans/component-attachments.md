# Component attachments: program data that travels with scene components

Status: **AT0–AT2 implemented** on branch `component-attachments` (uncommitted). AT3 not started.
See "As built" at the end for where the implementation differs from this text. This comes
**before** the runtime spine
(`runtime-spine-entt.md`). That plan's spawn path reads attachments, and its hierarchy decision
assumes `op` has already left the engine format.

## What this is

> **Narrowed** by `authored-entities.md`: attachments are for data about the voxel structure written
> by tools (the editor's boolean ops). Gameplay data lives in `entities.json` as ECS components.

A general way for a program to attach its own data to scene components and save it with them,
**without the engine knowing what the data means.** The engine stores it, copies it, saves it,
and preserves data it does not recognise. It never interprets it.

The first user is the scene editor's boolean ops, which move out of the engine format into an
attachment. The second is the editor's procedural primitive data, which today is not saved at all.
Future users are any program that needs authored data per component: physics bodies, gameplay
tags, LOD hints, spawn data.

---

## Why

### `op` is editor data living in the engine format

The engine *stores* `BooleanOp` but never *acts* on it. Every engine site:

| Where | What it does |
|---|---|
| `scene.h:210-224` | defines `BooleanOp` and `booleanOpName` |
| `scene.h:284` | `ComponentRecord::op` |
| `compose.h:130` | `ComposeComponent::op` |
| `compose_io.cpp:455-464` | parses `"op"`; unknown values warn and read as `None` |
| `compose_io.cpp:622` | copies it into the record at load |
| `compose_io.cpp:1036`, `:1262` | writes it back at save, only when not `None` |
| `scene_query.cpp:464` | copies it on duplicate |

Nothing in graphics, picking, editing or voxel_management reads it. The loader says so itself:
"Nothing in the loader acts on it -- resolving the fold is the editor's job." Its only consumer is
`examples/60-scene-editor/main.cpp`, with about 55 reads and writes. Boolean ops exist because the
editor needed them, not because any other program does.

### The editor already keeps attached data, and has nowhere to save it

`Part` (`main.cpp:1203`) is per-component data keyed by `ComponentHandle`: procedural kind,
dimensions, hollow/wall thickness, colour, cut bookkeeping, and the cached fold cells. None of it
is saved, so a primitive reopens as plain voxels and can no longer be resized exactly.
`assembly_system.md` names this gap (`"source": "proc:box?..."`) and would have solved it by
widening the engine format. Attachments solve it without touching the format again.

### Every per-component field invites the same bug

`scene_query.cpp:464` records it: `duplicateComponent` dropped `op`, so the copy of a subtracted
window "filled the hole it was cut from". Every field added to `ComponentRecord` has to be
remembered at every lifecycle site: duplicate, instantiate, save, load. An attachment mechanism
applies one policy per type, in one place.

---

## Decisions

### A1. An attachment is opaque, typed data keyed by (component, key)

- A **key** is a namespaced string: `projv.editor.csg`, `projv.editor.primitive`,
  `mygame.spawn`. The `projv.` prefix is reserved for programs in this repo.
- At most one attachment per key per component.
- The engine never branches on a key's contents. If engine code ever needs to, the data is engine
  data and belongs in `ComponentRecord`. That is the test for which side of the line a field
  falls on.

### A2. In memory: one side table per key, held by `Scene`

```cpp
// utils/attachments.h -- no nlohmann::json in this header (known-latent-issues #3).
struct SceneAttachments;   // opaque; held by Scene through a pimpl, copy-cloned

template<typename T> struct AttachmentTraits;   // specialised by the program that owns T

template<typename T> T*       getAttachment(Scene&, ComponentHandle);
template<typename T> const T* getAttachment(const Scene&, ComponentHandle);
template<typename T> T&       setAttachment(Scene&, ComponentHandle, T value);
template<typename T> void     removeAttachment(Scene&, ComponentHandle);
void                          clearAttachments(Scene&, ComponentHandle);   // every key

// For undo: everything attached to one component, typed and raw, as a value.
AttachmentSnapshot captureAttachments(const Scene&, ComponentHandle);
void               restoreAttachments(Scene&, ComponentHandle, const AttachmentSnapshot&);
```

```cpp
// utils/attachments_json.h -- included only by programs that register a type.
enum class OnDuplicate { Copy, Drop };

template<> struct AttachmentTraits<CsgRole> {
    static constexpr const char* key = "projv.editor.csg";
    static constexpr uint32_t    version = 1;
    static constexpr OnDuplicate onDuplicate = OnDuplicate::Copy;
    static nlohmann::json save(const CsgRole&);
    static CsgRole        load(const nlohmann::json&, uint32_t fileVersion);
};
```

**Storage.** Each key has its own table: a map from `ComponentHandle` to the value, type-erased
behind a base with `clone()`. Handles are the right key because they are stable for the Scene's
whole life and never reused. Deletion tombstones the slot (`name == "__deleted__"`) rather than
erasing it.

**Raw entries.** An entry loaded from disk is held as its JSON text until something asks for it as
a type. On the first `getAttachment<T>`, the raw entries under `T`'s key are decoded through
`AttachmentTraits<T>::load`. So:

- a program does not have to register types before loading a scene, and
- a key that no one in the process ever asks for stays raw and is written back unchanged (A4).

**Not EnTT.** `utils` and compose_io must stay usable without the runtime ECS. The mesh voxelizer,
a thumbnail bake, or a future command-line tool can load, edit and save assets with no registry.
Attachments are authored asset data, and ECS components are live game state. The spawn path in the
runtime plan joins the two (see "What this enables" below).

**Threading.** Main thread only, and documented as such. The only Scene state touched from workers
today is the material palette, which has its own mutex (`Scene::materialPaletteMutex`).

### A3. On disk: an `attachments` block per entry and per document

```jsonc
{
  "version": 1,
  "name": "Cathedral",
  "attachments": {                                   // document level: the folder itself
    "projv.editor.camera": { "v": 1, "position": [...], "yaw": 0.4 }
  },
  "components": [
    { "type": "data", "source": "sphere.data", "position": [...], "rotation": [...],
      "attachments": {
        "projv.editor.csg":       { "v": 1, "op": "subtract" },
        "projv.editor.primitive": { "v": 1, "kind": "sphere", "size": [32,32,32], "hollow": false }
      } }
  ]
}
```

- `"v"` is the attachment's own schema version, passed to `load` so a type can change its JSON
  without breaking older files.
- The block is omitted when empty. A file with no attachments writes exactly the JSON it does
  today, the same rule `compose_io.cpp:1036` follows for `op` now.

**Which component holds a document's attachments.** Each folder's document-level attachments
belong to the node that represents that folder:

- A nested `asset` entry is loaded as a `ComponentKind::Asset` record, so its folder's document
  block attaches to that record.
- The top-level folder of `loadComposeFromDisk` has no node: its entries become root components.
  Its document block goes to a scene-level slot, `Scene::documentAttachments`, reached with
  `INVALID_COMPONENT_HANDLE`.
- `saveComposeToDisk(scene, root, folder)` writes `root`'s children as the folder's entries, so
  `root`'s attachments become the written document block. This is symmetric with load.

### A4. Unknown attachments survive a round trip

A program that loads an asset and saves it again **must not lose attachments it never asked
about.** Otherwise, opening an editor asset in any other tool and saving it would silently erase
the CSG stack and the primitive data. Raw entries (A2) are written back byte for byte, apart from
JSON re-serialisation. Only tests can prevent this bug, and they must cover it (phase AT1).

### A5. Lifecycle: one engine path per operation

| Operation | Where | Attachments |
|---|---|---|
| Load | `loadComposeFromDisk`, `instantiateComposeInto` | read raw from each entry and document block |
| Save | `saveComposeToDisk` | typed entries saved through traits; raw entries written back unchanged; tombstoned components skipped, so a stale attachment cannot persist |
| Duplicate | `duplicateComponent` | per type, by `OnDuplicate`. Raw entries default to `Copy`, the safe choice for data whose meaning is unknown |
| Instantiate (copy asset into asset) | `instantiateComposeInto` | come from the source files, as at load |
| Reparent | `setComponentParent` | untouched. If an attachment depends on tree position, as CSG depends on child order, that is the owning program's concern |
| Delete | the **editor's** `deleteComponent` (`main.cpp:3802`; the engine has none) | the deleter calls `clearAttachments`. Save skips tombstones regardless |
| Undo of a delete or edit | the editor's edit history | `captureAttachments` / `restoreAttachments` around whatever the history already snapshots |

Each policy lives in exactly one engine function. No call site copies attachment fields by hand.

### A6. `Scene`'s hand-written copy and move members must carry the table

`Scene` lists its fields one by one in two constructors and two assignment operators
(`scene.h`, around `struct Scene`), because of `materialPaletteMutex`. A new member that is missing
from any of the four is silently dropped on copy or move. That is the same failure as the
duplicate bug above, one level up. Add the attachments member to all four, and add a test that
copies and moves a Scene with attachments.

Once the runtime plan's D2 moves resources into `entt::registry::ctx()`, the copy members can go,
and only the move members remain to keep in sync.

---

## Moving `op` out of the engine

**Engine side** (all in this repo's `src/` and `include/`):

1. Delete `ComponentRecord::op` and `ComposeComponent::op`, and remove `BooleanOp` and
   `booleanOpName` from `scene.h`.
2. Remove the parse at `compose_io.cpp:455-464`, the copy at `:622`, the writes at `:1036` and
   `:1262`, and the duplicate copy at `scene_query.cpp:464`, where `OnDuplicate::Copy` replaces it.
3. **Old files.** Existing compose.json entries carry a top-level `"op"`. At parse, the loader
   moves any top-level `op` that is not `none` into a raw `projv.editor.csg` attachment, as
   `{"v":1,"op":...}`. The engine never interprets the value; it only moves a key it used to own
   into the place it now belongs. Files are rewritten in the new form only on their next save.
   This is the one place the engine names an editor key, it is marked as a migration, and it can
   be removed once the bundled scenes have been re-saved.
4. Update `docs/data_structures/compose_data_structure.md` (now `docs/reference/formats/compose-json.md`): remove `op` and its Boolean ops
   section, document `attachments`, and point to the editor README for what `projv.editor.csg`
   means.

**Editor side:**

1. Move `BooleanOp` into the editor, beside `BooleanOpLabel`/`Glyph`/`Hint`.
2. `struct CsgRole { BooleanOp op = BooleanOp::None; };` plus its traits, with
   `OnDuplicate::Copy`, which is what the `scene_query.cpp:464` fix chose.
3. Replace about 55 `scene.components[c].op` sites with a small accessor
   `opOf(scene, c)` / `setOp(scene, c, op)`. That way `rowFolds`, `foldChildren`, the stack panel
   and undo keep their shape, with one place that turns a missing attachment into `None`.
4. Call `clearAttachments` from the editor's `deleteComponent`, and add `captureAttachments` to
   whatever the edit history snapshots for delete and duplicate undo.
5. **Other programs' view of a stack** is unchanged and now *defined*: an unbaked stack is editor
   working data, and other programs render its parts as placed. If you want something shippable,
   bake it (`Bake ▸ As a new component`).

**Then, optionally:**

6. `projv.editor.primitive`: save the procedural part of `Part` (kind, dimensions, hollow, wall
   thickness, colour), so primitives reopen as primitives and can still be resized. This closes
   the `proc:` gap without a new source scheme in the engine.
7. Later and not scheduled: the editor could save a stack as its *resolved* `.data` (the node's
   real child) with the parts held inside the node's attachment. Every program would then render
   the right result and the editor would still reopen the stack. That is `assembly_system.md`
   Hazard 8, solved without the engine knowing CSG exists.

---

## Phases

Each phase builds, runs every example, and leaves the tree shippable. The unit tests need the
`tests/unit/` scaffold, so this plan absorbs the runtime plan's P0 (EnTT excepted) as AT0.

### AT0: Test scaffold and C++20
- `tests/unit/` with doctest, plus a `ctest` target. Pure CPU, no display.
- The C++20 switch from `runtime-spine-entt.md` D0, including the `disk_io.cpp` fix. Doing it here
  means the attachments code is written in the language it will live in.
- **Exit:** `ctest` passes on the `dev` and `vcpkg` presets.

### AT1: The mechanism
- `utils/attachments.h`, `utils/attachments_json.h`, the per-key tables, raw entries and lazy
  decode, compose_io read and write for entry and document blocks, and the `duplicateComponent`
  policy. Also the four `Scene` copy and move members.
- **Tests:**
  - typed set/get/remove;
  - save, then load, then compare, for typed and raw entries;
  - **a raw key round-trips through a program that never registers it** (A4);
  - a file with no attachments writes byte-identical JSON;
  - duplicate honours `Copy` and `Drop`;
  - copying and moving a Scene keeps its attachments;
  - a version number reaches `load`;
  - the document block maps to the right node on load and on save of a subtree.
- **Exit:** tests pass, and no example's behaviour changes, because nothing uses attachments yet.

### AT2: Move `op`
- The engine and editor steps above, plus the old-file conversion.
- **Tests:**
  - a v1-style compose.json with top-level `op` loads with `projv.editor.csg` raw entries;
  - saving writes them under `attachments`, not top-level;
  - the editor's compose round-trip (save the stack, reopen, compare) still produces the same
    fold. This is the test that caught the `setComponentParent` bug, per `assembly_system.md`.
- **Exit:** the editor builds, saves and reopens a boolean stack as before. `grep BooleanOp
  include src` returns nothing.

### AT3 (optional): `projv.editor.primitive`
- **Exit:** a box made in the editor, saved and reopened, can still be resized exactly.

---

## What this enables in the runtime plan

- **The scene hierarchy is placement only, as far as the engine is concerned.** It carries no
  CSG meaning, so the runtime plan's D6 needs no rule about operands.
- **Authored data becomes game behaviour.** When an entity links to a component, a spawn system
  can read that component's attachments and add ECS components: `mygame.spawn` becomes a `Spawner`,
  `projv.physics.body` becomes a `RigidBody`. Attachments are what an asset says. ECS components
  are what the running game is doing.

## Open questions

1. **Key ownership.** Is `projv.` reserved for this repo, with programs choosing their own
   prefixes? The proposal is yes, by convention only, with no enforcement.
2. **Engine readers of attachments.** If a future engine module, physics for example, wants authored
   per-component data, does it use an attachment (`projv.physics.body`) or a `ComponentRecord`
   field? The A1 test says it is an attachment if only the physics module interprets it and the
   core voxel layer never does. That seems right, but it deserves a decision when physics starts.
3. **Size.** Attachments are JSON. Fine for ops, primitives, tags and settings, and wrong for bulk
   per-voxel data. A large attachment should be a sidecar file that the attachment names, the way
   `data` entries name a `.data`. No mechanism for that now; recorded so it is not mistaken for a
   gap later.

---

## As built

Branch `component-attachments`. `ctest --preset dev`: 11 cases, 93 assertions, all passing. A
deliberate break of raw write-back fails 3 of them, so the suite does catch A4.

**Files.** `include/utils/attachments.h`, `src/utils/attachments.cpp`; the store types are in
`include/data_structures/scene.h`; `compose.h` gained `attachments` on `ComposeComponent` and
`ComposeDoc`; `compose_io.cpp` and `scene_query.cpp` carry them; `tests/unit/` with doctest
(`external/doctest`, pinned v2.5.3), `PROJV_BUILD_TESTS` (on in `dev`) and a `dev` test preset.
C++20 is on, and the two `disk_io.cpp` format strings are fixed.

**Where it differs from the text above:**

1. **Type erasure is `std::any`, not a `clone()` hierarchy.** Raw entries are stored as JSON
   *text* (`std::string`). So `scene.h` holds the store with no JSON include and no pimpl, and
   `Scene` copies with no help. The tables are `mutable`, because a typed read through a
   `const Scene&` may decode, which is a cache fill.
2. **`load` returns `std::optional<T>`.** A refused value, or a nlohmann exception thrown inside
   `load`, is logged once and kept raw, so it is still written back. The editor uses this for
   unknown op strings: they read as `none` and survive a save unchanged.
3. **The compose format stays version 1.** `attachments` is additive, and the example above was
   corrected. An *old* engine reading a *new* file loses ops, because it ignores `attachments`. That
   was judged acceptable: old engine versions are not a supported reader of new files.
4. **`captureAttachments` / `restoreAttachments` were not built: undo is the editor's job.** The
   engine keeps no history. The editor's own undo store records each component's attachments:
   `PartRecipe::attachments` and `NodeRecipe::attachments`/`documentAttachments`, taken with
   `attachmentsForSave` and put back with `attachRaw` (`restoreRecipeAttachments`). Undo of a merge
   or bake therefore brings back every attachment on a part, including other programs' data. The
   self-test case "undo of a merge restores attachments" covers it.
5. **`clearAttachments` clears both scopes.** `duplicateAttachments` copies both, so a duplicated
   Asset keeps its folder block.
6. **The editor's accessors are `opOf` / `setOp` / `opOfEntry`.** `setOp(..., None)` removes the
   attachment. `BooleanOp`, `booleanOpName` and `parseBooleanOp` are now in `main.cpp`.

**Bundled scenes.** The two that carried a top-level `op` (`New_Asset/compose.json` under
`examples/10-scene-previewer/scenes/SmallVox/` and `SmallerVox/`) were moved to the attachment by
hand, one line each. `readLegacyOp` stays for files outside the repo.

**Editor self-tests** (`EDITOR_SELFTEST`, `EDITOR_SCULPTTEST`, `ASSEMBLYTEST`, `BRUSHTEST`,
`SAVETEST`) were run on SmallVox and Sibenik. The branch is based on `main` (`bfd6ad9`). The
baseline run used `b899ad4`, which is `main` plus the surface-voxels prototype; that prototype does
not touch the editor, scene or compose code.

| Run | PASS | Failing |
|---|---|---|
| SmallVox, this branch (on `main`) | 71 | `place with nothing open`; 5 `SELFTEST fill leak` lines |
| SmallVox, baseline `b899ad4` | 70 | the same two, identical |
| Sibenik, this branch (on `main`; `ASSEMBLYTEST`, `SAVETEST`) | 31 | none |

The extra pass is the new "undo of a merge restores attachments" case. Both failures predate this
work and depend on the scene. `place with nothing open` counts new resolves and expects 1, but
SmallVox already holds a stack (`New_Asset`), which is adopted too; it passes on Sibenik. The fill
test truncates at 16M gathered cells on SmallVox and reports the cells it did not reach as leaks.

**Not done:** AT3 (`projv.editor.primitive`).
