# Authored entities: gameplay data saved as ECS components

Status: **implemented** on `main`, after `c56dfdc` (uncommitted when this was written). It
supersedes the spawning half of `runtime-spine-entt.md` D6 and narrows `component-attachments.md`.

## The problem it fixes

With attachments carrying gameplay data, every behaviour was written three times:
- an attachment type with its JSON traits (what the file says);
- an ECS component (what runs);
- a spawn handler to translate one into the other.

The sandbox had `BodyDef`/`Body`/`spawnBody` and `SpawnerDef`/`Spawner`/`spawnSpawner`;
15-entities had `Motion`/`Spin`+`Orbit`/`spawnMotion`. That split brought four problems:
- the authored copy and the running copy drifted apart at runtime;
- saving game state would have needed a second serializer;
- prefab physics had to be a folder-level attachment so it would land on the grafted node;
- attachments decode lazily, which is why they are main-thread only.

## The split

| Data | Where | Example |
|---|---|---|
| read by engine code | a `ComponentRecord` field | transform, palette |
| about the voxel structure, written by tools | an attachment in compose.json (`utils/attachments.h`) | the editor's boolean ops |
| what a running program does | ECS components on authored entities, in `entities.json` (`runtime/entities.h`) | Body, Spawner, Spin |

The voxel layer still never sees EnTT. entities.json is read and written only by `runtime/`.

## What was built

**Persistent local component ids** (voxel layer).
- `ComponentRecord::localId` is unique among siblings and saved as each entry's `"id"`.
- Ids are local rather than global because one folder can be loaded several times; each copy
  resolves the file's ids against its own node (`utils::findComponentByLocalId(scene, node, id)`).
- The file's own ids always win at load. Only a missing id, or the second of two that clash, is
  renumbered, and only after every written id is in place.
- `addComponent`, `duplicateComponent` and `setComponentParent` keep ids unique. A copy's children
  keep theirs.
- The writer numbers any hand-built record with no id.
- `Scene::documentPath` records the top-level folder. An Asset made by `instantiateComposeInto`
  records its folder in `sourcePath`, as nested assets already did.

**`entities.json`** (runtime). Format reference: [`docs/reference/formats/entities-json.md`](../reference/formats/entities-json.md).
- An entity's `"link"` is a component id in the same folder, `"document"` (the node the folder
  becomes when nested or grafted), or absent (a standalone entity).
- `"components"` are ECS components written by `runtime::ComponentTraits<T>`, registered with
  `registerComponent<T>`.
- Keys nothing registered, and values their type refuses, are kept (`UnknownComponents`) and
  written back.
- `projv.transform` is the engine's own: it places unlinked entities and is not saved for linked
  ones.
- `spawnEntities` reads every folder's file, inner first. An outer file naming an already-linked
  component *merges* into that entity and overrides shared keys (the prefab-override rule).
- `spawnEntitiesUnder` does the same for a grafted subtree. `instantiatePrefab` is graft plus
  spawn, and the returned entity owns the voxels.
- `saveEntities` writes one folder, and **every entity belongs to exactly one file**: the one it
  came from (`Authored`), or else the one its linked component is an entry of. A first version
  wrote an entity linked to a nested node into both files; the fix and a test followed.
- Setup that needs the entity (a starting angle, an inverse mass) is an `on_construct` signal,
  which runs after the link has seeded the Transform.

**Retired:** `registerSpawnHandler`, `registerDocumentSpawnHandler`, `spawnFromCompose` and
`SceneDocument`. `spawnComponent` stays as "link a new entity".

**Examples moved over:**
- 15-entities: `Spin`/`Orbit` live in `scene/entities.json` and `scene/Planet/entities.json`. The
  moon's entity is in the planet's own file, so the hand-written Part-link loop is gone.
- 16-sandbox: each prefab's `entities.json` has a `"document"` entity carrying its `Body`, and the
  arena's `entities.json` gives the pylons their `Spawner`s. Both examples' `--write-*` modes
  produce these files with `saveEntities`.

## Verification
- Unit tests: 65 cases.
  - `test_local_ids`: numbering, save and reload, a hand-reordered file, assignment after the
    file's own ids, duplicates, reparenting, and grafting twice.
  - `test_entities`: link kinds, nested merge and override, unknown and refused components kept,
    save round trip including an unlinked entity's transform, a missing link kept on save, a prefab
    instantiated twice, `on_construct` seeing the Transform, and one-file ownership.
- A deliberate break of the inner-first order fails the override test.
- `SANDBOX_SELFTEST=300`: PASS; both spawners came from `entities.json`.
- 15-entities spawns 3 entities.
- The editor's self-tests on SmallVox are unchanged (71 PASS, the one known scene-dependent
  failure).

## Open
- **No editor support yet.** The scene editor neither shows nor edits entities. It preserves ids,
  but it does not carry `entities.json` when it saves a folder elsewhere. Copying a folder by hand
  keeps it.
- **Merging attributes ownership to the inner file.** An entity merged from an outer file's
  overrides is saved by the inner file with the merged values. This is fine for loading; a real
  override-aware save is future work.
- **Ids are written into every compose.json on its next save,** including bundled scenes saved by
  the editor. The files grow by one field per entry.
