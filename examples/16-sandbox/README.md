# 16 — Sandbox

A physics toy built out of the runtime. Throw balls, crates and bombs into an arena; watch them
bounce, roll and pile up; pop them; set off chain reactions.

```bash
cd build/examples/sandbox && ./sandbox
```

| | |
|---|---|
| mouse, W/A/S/D, R/F | look, fly, up/down (Esc releases the cursor, a click recaptures it) |
| **left click** | throw the selected prefab |
| **1 / 2 / 3** | select ball / crate / bomb |
| **right click** | pop whatever is under the crosshair |
| **E** (hold) | tractor beam: pull every body toward a point in front of you |
| **B** | rain twenty of the selected prefab from the sky |
| **T** | slow motion |
| **P** | pause |
| **G** | low gravity |
| **X** | turn the arena's spawners off and on |
| **C** | pop everything |

The window title shows the selection, how many bodies exist, how many you have popped, and the
time scale.

Try this: press **3** and throw a few bombs into a crowd, then right-click one.

## What it is made of

Everything here is a system the engine has had since the runtime spine landed, and each is doing
real work.

**Prefabs are compose folders.** `prefabs/ball`, `prefabs/crate` and `prefabs/bomb` each hold their
voxels and, as a *folder-level attachment*, their physics. Open `prefabs/bomb/compose.json`:

```json
"attachments": {
  "sandbox.body": { "explosive": 9.0, "mass": 1.5, "radius": 1.2, "restitution": 0.4, "v": 1 }
}
```

**Spawning is the Scene bridge.** A throw is two calls:

```cpp
ComponentHandle root = utils::instantiateComposeInto(scene, prefabs / kind, INVALID_COMPONENT_HANDLE, position);
Entity entity = runtime::spawnComponent(world, root, LinkMode::Root, OnUnlink::Destroy);
```

`instantiateComposeInto` grafts the folder into the live Scene, and the folder's own attachment
lands on the new node. `spawnComponent` links an entity to it and runs the spawn handlers. The
document-scope handler for `sandbox.body` turns the attachment into a `Body` component. With
`OnUnlink::Destroy`, destroying the entity later deletes its voxels too.

**The arena is data too.** `scene/compose.json` is the arena (floor and walls: one component, one
grid) and two pylons. The pylons
carry a `sandbox.spawner` attachment, and `spawnFromCompose` brings them to life. No line of code
names a pylon. Edit the interval or the prefab in that file and the arena behaves differently.

**Physics is `FixedUpdate`.** It runs 60 steps a second whatever the frame rate: gravity, the floor
and walls, ball-to-ball collisions and rolling. `Update` then draws each body between its last two
steps by `Time::fixedAlpha`. That is why slow motion (**T**) is smooth: at a fifth of the speed
there is a fixed step only every few frames, and every frame between them is interpolated.

**A pop is an event.** Right-click sends `Popped`, and three handlers answer it, none of which knows
about the others: a shockwave that pushes neighbours away, the score, and removal. A bomb's
shockwave also pops everything inside its radius. Those pops are *sent*, and anything sent while
events are being delivered waits for the next frame. So a chain reaction spreads outward one ring
per frame, instead of resolving in one invisible instant (or recursing forever).

**Input and Time** drive everything else. Flying uses the unscaled clock, so pausing the world does
not freeze the camera.

## The frame

```
PreUpdate    platform: poll the window, fill Input, queue the close button
FixedUpdate  physics (0..n times): spawners launch, bodies integrate and collide
Update       controls (throw, pop, keys) -> present (interpolate into Transforms) -> title bar
PostUpdate   the Scene bridge writes every changed Transform into its component
  (pump)     Popped and friends are delivered: shockwave, score, removal
Render       flushSceneUpdates (new prefabs, moved headers, deleted voxels), then draw
```

## Limits worth knowing

- **Every body costs GPU time on every pixel.** Bodies are loose chunks, and the shader visits every
  loose chunk for every pixel; there is no acceleration structure for them yet. Measured on an
  integrated Radeon 840M/860M at 1600×900:

  | | GPU / frame |
  |---|---|
  | the arena alone | 34 ms |
  | the arena + 150 resting balls | 210 ms |

  That is about 1.2 ms per body. A BVH over loose chunks is the planned fix. Measure it with
  `SANDBOX_MEASURE=<bodies> ./sandbox`, which lays out that many balls, waits, and reports frame
  and GPU time.
- The arena is one component, which is one grid: continuous geometry on one lattice. Built as a
  floor chunk and four wall chunks instead, it cost 65 ms a frame on its own, because each chunk's
  cubic bounds filled the arena's whole airspace and every ray marched through them.

- Collisions are spheres against spheres, over every pair. A few hundred bodies is fine; the
  spawners hold off above 300. A real broadphase is what the engine's content bounds (promotion
  candidate #4) are for, and a real dynamics library would replace all of `physics()`.
- A crate collides as a sphere.
- Every throw reads the prefab from disk again, and every deleted body leaves a tombstoned
  component behind. Both are invisible at this scale.

## Self-test

```bash
SANDBOX_SELFTEST=600 ./sandbox
```

Plays by itself for 600 frames: it throws every kind, rains bombs into a crowd, pops at random and
sets off chain reactions, and finishes in slow motion. It then checks that every body spawned is
either alive or was popped, and that every popped body's voxels are gone from the Scene. The result
is logged as `SANDBOXTEST: ... | PASS` or `FAIL`.

## Regenerating the assets

```bash
./sandbox --write-assets ../../../examples/16-sandbox
```

writes `scene/` and `prefabs/` from code.
