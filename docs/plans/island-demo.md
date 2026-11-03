# Island demo: break it, gather it, build with it

Status: **in progress** (2026-11-02): I0 started, paused mid-way -- see [Hand-off](#hand-off-2026-11-02). Asked for: "the most advanced example yet" -- a procedurally generated
sandbox with hills, rock and trees; tools to break the world; E to collect the rubble you look at into
a hotbar beside the tools; a click to place it; a compass at the top.

Decided with the user (2026-11-02): **single player**; **a generated island**, all of it loaded;
**restir-pt**; rubble placed as **blocks of its material**; two tools, a **pickaxe** (the big one) and a
**chisel** (one voxel, dropped as rubble); **worlds saved and loaded** -- a start screen to make a new
one or pick a saved one, autosaved as you play and when you quit -- in the build folder (it shows how
such a system works; nobody's progress is at stake); **192 m at 0.25 m**; **a pixel-art look** for its
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
deterministic island: a fBm heightfield under a radial falloff into the sea, beaches at the edge, rock
where it is steep or high, grass over dirt elsewhere, trees in clumps away from the shore and the rock.
Generated on worker threads into **one Grid component** (one lattice-aligned world is one grid), cell
by cell, before the first frame; a loading line in the window while it runs. *Why:* the user chose a
fixed island; everything loaded means breaking, rubble and placing never meet an unloaded cell.

**D2. Size: 192 m across at 0.25 m voxels**: 768 x 768 voxels, 64 m tall, in grid cells of 64^3
(12 x 4 x 12). *Why:* 0.25 m is fine enough that a pickaxe's rubble
looks like rubble and a block is a sensible size, coarse enough that the island generates in seconds
and the tile tables keep the solid ground cheap. (Open question 1.)

**D3. Trees are part of the island**, generated in place: a trunk of wood, a crown of leaves, each tree
different (height, lean, crown size) from the seed. Wood and leaves break as wood and leaves; cut a
trunk and the crown, unsupported, falls (`splits`; the ground is the anchor). *Why:* the same grid,
the same break and support rules as the ground -- no second kind of thing.

**D4. Materials: six, each with a physics material** (`physics_materials.json` beside the program):
grass, dirt, sand, stone, wood, leaves -- strength, density, fracture pattern each (stone shatters,
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
| **I0** | The island: generator, materials, grid, restir-pt; walking it | a capture from the shore; seeds give different islands |
| **I1** | Tools and breaking: the pickaxe, the chisel; trees fall | breaking by hand; the self-test's break |
| **I2** | Rubble and gathering; the inventory | the prompt, gathering by hand; the self-test's counts |
| **I3** | Placing blocks | building by hand; the self-test's placing |
| **I4** | Saving and loading: the start screen, autosave, save and quit | a world built in, quit, loaded back as it was; the self-test's reload |
| **I5** | The HUD: hotbar, compass, prompt, menu; the look | the user's check |

## Open questions

None. Answered 2026-11-02: 192 m at 0.25 m; the name; saves in the build folder; the chisel's voxel is
rubble; the menus and HUD pixel art.

## Hand-off (2026-11-02)

Paused at the user's request to continue on another computer. The work is on the branch
**`island-demo`** (not `main`: I0 is not finished). `main` has this plan up to its approval.

```bash
git fetch && git checkout island-demo
git submodule update --init --recursive        # RmlUi and FreeType arrived with ui.md U3
cmake --preset dev && cmake --build --preset dev -j$(nproc)
cd build/examples/island_demo && ./island_demo --seed 42
ISLAND_VIEW=overview ISLAND_CAPTURE=/tmp/isl ./island_demo --seed 42   # a still from above, then it exits
```

**What is built** (`examples/97-island-demo/`):

| File | What |
|---|---|
| `island.h` | The generator: seed in, world folder out (`worlds/island-<seed>/`: `compose.json`, `island.data`, one grid of 64^3 cells at 0.25 m, 12 x 4 x 12). A **2D heightfield** (fBm under a radial falloff into the sea), beaches, rock on steep or high ground, grass over dirt, a one-voxel sea, procedural trees stamped per cell. Fifteen palette shades, each naming a physics material. Generates in about 1 s (seed 42: 196 of 576 cells non-empty, 22 MB) |
| `main.cpp` | Generates the folder if it is missing, then loads it (D11's path: a new world and a saved one load the same way); physics with tile collision, cheap debris (64 voxels), impact fracture; the island a `StaticCollider` with `splits` and `anchorBase`; a first-person `CharacterController` walker; restir-pt as Voxel Mayhem draws it. `--seed n`, `ISLAND_CAPTURE`, `ISLAND_VIEW=overview` |
| `physics_materials.json` | grass, dirt, sand (crumble), stone (shatter), wood (splinter, grain up), leaves (weak), water (unbreakable) |
| `CMakeLists.txt` | target `island_demo`; stages restir-pt from `30-renderers` and the shared UI (`SHARED_UI`) |

Checked: it builds, generates seed 42, and draws the island from above (green hills, a sand rim,
round-crowned trees, a flat blue sea). Walking was not yet checked by hand.

**The user's last request, not yet done:** "use a 3D noise with a rock texture and more verticality
and trees". The rework that was about to be written:

- **A 3D density field** instead of the heightfield: solid where `base(x, z) - y + A * noise3(x, y, z) > 0`.
  `base` taller and ridged (`1 - |noise|` octaves; peaks near 50 m, under the 64 m ceiling) so there
  are mountains and cliffs; the 3D term (A about 6-10 m, stronger where the ground is steep) makes
  overhangs and ledges. The island mask still takes it down into the sea. Evaluated only in the band
  where the surface can be (`base +- A`): solid below it, air above, so the cost stays near the
  heightfield's.
- **Column-major generation**: a job per column of cells (12 x 12 jobs, four cells high each), since a
  voxel's material depends on its depth under the surface above it, which may be in a higher cell.
  Per column: the solid run from the density, then materials top down by depth from the last air.
- **Materials from the surface's normal**: the density's gradient at each air-to-solid crossing (six
  more samples, once per surface, reused down its run). Facing sideways (normal.y under about 0.65),
  or high (over about 38 m): rock. Gentle near the sea: sand. Else grass on top, dirt to about 3 m,
  stone below.
- **A rock texture**: five or six stone shades in strata -- banded by height, warped by 2D noise --
  with darker cracks, so a cliff reads as layered rock.
- **More trees**: candidates every 4 m instead of 7, higher odds; each stands on the real surface,
  found by walking the density down from the top at its (x, z); only on gentle grass below the rock
  line.
- `spawnPoint` then finds its ground the same way.

**Then the rest of I0**: walk it by hand (the user checks I0 by eye), then commit I0 and go on to
I1-I5 as above. One commit per milestone, on `main` once a milestone is finished (this branch merges
then).

**Working agreements with this user** (from the sessions that made this; also in `AGENTS.md`):

- Plans first, agreed before code; one commit per milestone with its tests and docs; push after
  each. **Stage explicit paths**, never `git add -A` (other sessions share the tree). Never
  force-push `main`; fetch before pushing.
- **Ask the user to look**: anything visual or felt (a renderer's look, a menu's feel) is the user's
  call. Unattended captures (`*_CAPTURE` variables, `bgfx::requestScreenShot`) are fine; do not
  screenshot the user's screen without asking.
- The engine ships mechanisms, not looks: a look belongs to the program's files.
- The island's HUD is **pixel art** (D9), not the frosted look of the other demos.
- On the user's main machine the display is Wayland at 170% scaling: GLFW's framebuffer is 1.7x
  the window and bgfx's back buffer is the window's size. Anything clipped or offset by about 1.7x
  is that. Another machine may differ.

**Open elsewhere** (not this demo's): ui.md U6 not started (gamepads, a document on a texture,
MSDF); [known-latent-issues.md](known-latent-issues.md) #17 (one hit in the physics example's first
demo wakes every block); the vendored RmlUi and FreeType untested in an install and the vcpkg preset;
the physics example's `DEMOS_SELFTEST` not run since ui.md U5 changed its startup; the basic renderer
has no sun shadows (asked about, not planned).
