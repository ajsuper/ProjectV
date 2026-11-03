# Island demo: break it, gather it, build with it

Status: **in progress** (2026-11-02): I0 built (on the branch `island-demo`); I1 next. Asked for: "the most advanced example yet" -- a procedurally generated
sandbox with hills, rock and trees; tools to break the world; E to collect the rubble you look at into
a hotbar beside the tools; a click to place it; a compass at the top.

Decided with the user (2026-11-02): **single player**; **a generated island**, all of it loaded;
**restir-pt**; rubble placed as **blocks of its material**; two tools, a **pickaxe** (the big one) and a
**chisel** (one voxel, dropped as rubble); **worlds saved and loaded** -- a start screen to make a new
one or pick a saved one, autosaved as you play and when you quit -- in the build folder (it shows how
such a system works; nobody's progress is at stake); **192 m at 0.125 m** (0.25 m at first: too coarse, "a giant on a mountain"); **a pixel-art look** for its
menus and HUD, not the other demos' frosted glass; the name **island demo**.

Folder `examples/97-island-demo`, target `island_demo` (a `_demo`: many features at once, a full
renderer). Everything it needs is in the engine already; what it adds is the game around them.

| Needs | From |
|---|---|
| a world made at startup | its own generator (the terrain generator's is example-local, and streams; this one does not) |
| breaking | `runtime::fracture` on the island's grid, which has a `StaticCollider` with `splits` |
| rubble | `BodyBroken`'s pieces (`Piece` bodies) and cheap debris (`projv::Debris`) |
| aiming | `runtime::castRay` (hit point, normal, material slot) |
| placing | `runtime::editVoxels` with add ops, in the island's own palette |
| looks | restir-pt (`examples/shared/restir_pt.h`), as Voxel Mayhem |
| HUD | the user layer (`ui/user.h`): hotbar, compass, crosshair, prompt; the shared menu |
| saving | the engine's compose save and `.data` files for the island; a small JSON for the rest |

## Decisions

**D1. One program, one island, generated at startup.** A seed (`--seed n`, shown in the menu) makes a
deterministic island from a **3D density field** -- rolling hills and rounded mountains under a radial
falloff into the sea, plus a strong, warped, vertically squashed 3D noise (shelves, overhangs) and
carved tunnels and chambers (arches where they cut a slope, caves where they do not). The user asked for
it whimsical and "somewhat over the top", round and soft: no ruled lines, no strata, no cell noise.
Generated on worker threads into **one Grid component** (one lattice-aligned world is one grid), cell
by cell, before the first frame; a loading line in the window while it runs. *Why:* the user chose a
fixed island; everything loaded means breaking, rubble and placing never meet an unloaded cell.

**D2. Size: 192 m across at 0.125 m voxels**: 1536 x 1536 voxels, 56 m tall, in grid cells of 64^3
(24 x 7 x 24). *Why:* at 0.25 m the player was seven voxels tall and the world read as giant-sized; the
user asked for twice as fine. It still generates in about 2.5 s (the density sampled on a 0.5 m lattice
and interpolated, a cheap fine noise added per voxel near the surface) and saves as about 190 MB.

**D3. Trees are part of the island**, generated in place, each different from the seed: broadleaf trees
(6-9 lumpy leaf clusters in a wide dome, each on a branch; some blossom, some autumn, some with fruit),
pines (tiered drooping skirts), bushes (some with berries). Forests close overhead in their hearts; a
crown's underside keeps 2.7 m clear, since leaves are ground. Wood and leaves break as wood and leaves; cut a
trunk and the crown, unsupported, falls (`splits`; the ground is the anchor). *Why:* the same grid,
the same break and support rules as the ground -- no second kind of thing.

**D4. Materials: seven, each with a physics material** (`physics_materials.json` beside the program):
grass, dirt, sand, stone, wood, leaves, mushroom -- strength, density, fracture pattern each (stone shatters,
wood splinters, dirt and sand crumble, leaves crumble easily). The island's palette has a few shades
of each, every shade naming its material's physics. *Why:* breaking needs a strength; counting
rubble needs a material per voxel.

**D5. You walk it in first person**: a physics character (the engine's `CharacterController`), W/A/S/D,
Space to jump, the mouse to look; Esc for the shared menu.

**D6. Two tools, in the hotbar's first slots:**

| Slot | Tool | Does |
|---|---|---|
| 1 | **Pickaxe** | the general, large destroyer: a big `fracture` strike where you aim (about a 0.75 m bite, more in soft ground, less in stone), its shards thrown out of the face |
| 2 | **Chisel** | the small one: knocks out exactly the voxel you aim at, which drops as a one-voxel piece of rubble (the user's choice) |

1-9 and the wheel pick a slot; the left mouse uses it (held: again, at the tool's rate). *Why:* the
user's two -- one to clear ground fast, one to shape exactly. The chisel's voxel is removed by
`editVoxels` and dropped as a piece of rubble of its material, so it is gathered like any other.

**D7. Rubble is whatever a break throws off, and E gathers it.** Looking at rubble within reach
(5 m), the HUD says what is there ("E: gather 37 stone, 12 dirt"); E takes every piece within 1.5 m
of where you look: body pieces from `overlapSphere`, cheap debris from a pass over `Debris` (it is
in no query). Each piece's voxels are counted by their palette entry's **material** and added to the
inventory; the pieces are destroyed (with a short pull toward you first, so it reads). Pieces too
big to carry (over 512 voxels) are not rubble. *Why:* "piles of rubble" are the pieces; counting
voxels keeps it honest -- what you gather is what you broke.

**D8. The inventory is voxels of a material, and a block costs eight.** Gathered material appears in
the hotbar after the tools, a slot per material, with its count in blocks. Selecting one and clicking
places a **0.5 m block** (2 x 2 x 2 voxels) of it against the face you aim at, snapped to the island's
lattice, into the island grid itself (`editVoxels`, the material's own palette entry), so a placed
block is ground like any other: walk on it, break it, gather it again. Not inside yourself, not
floating free (it must touch something). *Why:* the user chose blocks of the material; editing the
island keeps one world.

**D12. Grass and flowers are a second component with no collider** (`plants`, `plants.data` beside
`island.data`): blades, flowers, reeds, small mushrooms, pebbles and shells, hanging vines, glowing
mushrooms in caves. Drawn, never collided with. *Why:* the engine has no non-colliding physics material,
and blades in the island's grid would trip the walker and the tools. *Cost:* breaking the ground leaves
its plants floating; I1 clears the plants over what a strike removes. The ground's props -- boulders,
fallen logs, stumps, driftwood, giant mushrooms (climbable) -- are in the island grid.

**D13. Life is looks only** (`critters.h`): butterflies and bees over the flower fields, leaves falling
from trees near the player, rabbits hopping in the meadows, gulls circling, clouds and dandelion fluff on
the wind. Each is a few voxels in a component of its own, moved through its entity's Transform (the
engine has no particles). Not physics, not saved. *Why:* the user found the island "empty and unalive".

**D9. The HUD is documents, in a pixel-art style of its own** (`ui/` beside it; not the shared
frosted look -- the user asked for pixel art): a pixel font (an OFL one, added at I5), hard edges, no
rounded corners or blur, two-tone bevelled borders, a small palette, tool and material icons as
pixel-art images drawn crisp at whole multiples. The documents: the hotbar along the
bottom (tools, then materials with counts; the selected one marked), the compass along the top, the
crosshair, the gather prompt, the shared Esc menu. The compass is a strip of ticks and N/E/S/W that
slides with the yaw -- the program binds the visible marks and their offsets. *Why:* the UI work
just landed for this.

**D10. A self-test: `ISLAND_SELFTEST=1`** generates the island (fixed seed), walks to a rock, breaks it
with the pickaxe, chisels a voxel, gathers, places three blocks, saves, loads the save again, and
checks: the island generated, the breaks made rubble, the gather counted it by material, the blocks
are in the grid, the inventory went down by 24 voxels, and the loaded world has the same voxels,
inventory and position. Windowed, like Voxel Mayhem's. *Why:* `_demo`s check themselves.

**D11. Worlds are saved, and loaded from a start screen.** Each run opens on a start screen (a
document over the sea and sky): **New world** (a name, and a seed -- random unless typed) or a list
of the saved worlds, each with its name, seed and when it was last played; click one to load it.
A world is a folder in `worlds/` beside the program, in the build folder (`build/examples/island_demo/
worlds/`, never staged over, gone with the build folder: the user's choice):

```text
worlds/<name>/
  compose.json, *.data     the island grid: the engine's own scene format, so the scene previewer and
                           editor open it too
  world.json               the seed, the player's position and look, the inventory, the time played
```

**Autosave** every two minutes while playing, and on Quit (the menu's Leave becomes **Save and quit**)
and on closing the window. Only the island's edited cells are written after the first save (a grid
cell rewrites its `.data` when its voxels changed), on a worker thread from a copy, so an autosave
does not hitch the game; quitting waits for it. Rubble lying about is not saved: it is what you have
not gathered yet, and cheap debris expires anyway. *Why:* the user asked for it; the engine's format
means a saved island is an ordinary scene.



## Milestones

| | What | Proven by |
|---|---|---|
| **I0** | The island: generator, materials, grid, restir-pt; walking it; plants and life | the user's walk (done, 2026-11-02); seeds give different islands |
| **I1** | Tools and breaking: the pickaxe, the chisel; trees fall | breaking by hand; the self-test's break |
| **I2** | Rubble and gathering; the inventory | the prompt, gathering by hand; the self-test's counts |
| **I3** | Placing blocks | building by hand; the self-test's placing |
| **I4** | Saving and loading: the start screen, autosave, save and quit | a world built in, quit, loaded back as it was; the self-test's reload |
| **I5** | The HUD: hotbar, compass, prompt, menu; the look | the user's check |

## Open questions

None. Answered 2026-11-02: 192 m (at 0.25 m, then 0.125 m: D2); the name; saves in the build folder; the chisel's voxel is
rubble; the menus and HUD pixel art.

## As built

**I0** (2026-11-02, branch `island-demo`). `examples/97-island-demo/`:

| File | What |
|---|---|
| `island.h` | The generator (D1-D4, D12): seed in, world folder out (`worlds/island-<seed>/`: `compose.json`, `island.data`, `plants.data`). Column-major jobs (24 x 24, all seven cells high), the density on a 0.5 m lattice; materials from depth under the surface and its normal, every threshold wavering with noise; rock bumped a voxel or two; regions (rocky, forest, meadow); trees, boulders and props stamped from global lists. Seed 42: about 1130 ground cells, 200 trees, 80 boulders, 130 props, in about 2.5 s |
| `critters.h` | The life (D13) |
| `main.cpp` | Generates the folder if missing, then loads it; physics with tile collision, cheap debris (512 voxels: the same size of piece as 64 at 0.25 m), impact fracture; the island a `StaticCollider` with `splits` and `anchorBase` (the plants are not spawned: no collider); a first-person walker spawned clear of crowns, facing its most open view; restir-pt. `--seed n`, `ISLAND_CAPTURE`, `ISLAND_VIEW=overview`, `ISLAND_LOOK="x,y,z,tx,ty,tz"` |
| `physics_materials.json` | grass, dirt, sand, stone, mushroom, wood, leaves, water |

Notes for I1 on:

- A world folder is made only when missing: after a generator change, delete `worlds/` (or the old
  island loads).
- Runs that capture often die on exit (a segfault or FPE in bgfx's Vulkan present, in the NVIDIA
  driver), after the capture is written: pre-existing, not this program's.
- Working agreements: plans first; one commit per milestone; stage explicit paths; ask the user to look
  at anything visual; the HUD is pixel art (D9).
