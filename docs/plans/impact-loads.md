# Impact loads: what decides a break, and how big it is

Status: **in progress**, 2026-10-25. Delivered: continuous voxel bodies, the minimum impact energy,
the load share, the ragged rim. Open: how big a break a big slow body buys (below), sustained loads,
kinematic pushers.

## Why: West Point Marina (2026-10-25)

Played with the barrage: "bodies merge into one another and the solver jitters them"; "a big boat fell
onto a 1 voxel thin floor and wedged through it"; "sometimes things hit other bodies or static bodies
and no fracture happens"; then, once speed no longer gated breaks, "a particle hits the biggest boat on
the map, which falls through the floor, fracturing the floor and a large part of the boat at the same
instant"; and "everything within the fracture radius gets fractured: a hard circle".

The diagnosis (`test_physics_tiles.cpp`, the skipped "tiles diagnosis": a 6 x 1.5 x 2 m wooden hull of
0.1 m voxels dropped on a 0.1 m concrete floor, boxes and tiles, from 10 m and 4 m):

- **Tunnelling.** Continuous collision was only for bodies under 1 m. From 10 m on tiles the hull went
  0.47 m into the floor and ended 0.17 m under it, wedged. Its walls are one voxel thick, and so is
  the floor.
- **No break.** Voxel Mayhem's world took a hit only at 12 m/s or more. The hull from 4 m (8.9 m/s)
  broke nothing, however heavy it was. A speed threshold is also wrong for loads that are not hits:
  a piston pressing a wall never moves fast.

## Delivered

- **Voxel bodies are continuous** (`Quality::Auto`: any dynamic voxel body, as well as anything under
  1 m). Jolt sweeps a body only in a step that moves it more than a share of its inner radius, so a
  body at rest costs nothing more. Hull from 10 m: lowest point -0.034 to -0.061 m, and it ends on the
  floor.
- **`minImpactEnergy`** (J, `RigidBody`, `StaticCollider`, saved, copied to pieces): the least a hit
  must carry to break the body, beside its material's strength. Voxel Mayhem's world uses it
  (20 kJ) instead of `minImpactSpeed = 12`.
- **The load share** (the Jolt patch below). Each impact strikes where the solver pushed hardest, with
  the pair's energy times that point's share of the pair's impulse
  (`Impact::share`, `BodyImpact::share`, `PhysicsWorld::contactLoad`). A ball meets a wall at one
  point and keeps all of its energy. A 2 m slab face on lands on its corners (under half,
  `test_physics_fracture.cpp`). The solver does not see a continuous body's sweep (Jolt's sweep
  stops it with its own impulse), so that impact waits one step, held by entity, and takes its share
  from the next step's load, with the two pressed together. It is held by entity because a break in
  between remakes a body under another id: the first version held it by body id and lost the third
  ball's hit in the thread test. Waiting impacts are in `savePhysics`.
- **The ragged rim** (`fracture_patterns::*`, `leaveSomeAttached`). Near the strike everything goes;
  past 40% of the reach a shard stays, cracked but held, with a chance rising to 70% at the reach.
  With a direction (`FractureInput::direction`, the strike's push) the distance is measured across
  it, so what is in the blow's way goes (a cannonball's hole runs through the wall) and the rim
  around its path stays. A kept shard with nothing left holding it falls anyway, as any island does.

### The Jolt patch

`cmake/patches/jolt-applied-impulses.patch`, applied to `external/JoltPhysics` at configure time when
it is not already applied (a failed apply stops the configure). It adds one read-only accessor:
`PhysicsSystem::GetAppliedContactImpulses`, the solver's cached normal impulses per manifold, read from
the contact cache in the same sorted order as `SaveState`. That order does not depend on threads: the
thread test holds at 0, 4 and 12 threads. A Jolt found through `find_package` lacks the patch, and
`PROJV_JOLT_APPLIED_IMPULSES` is then undefined: shares are 1 (energy alone, as before). A Jolt
upgrade re-makes the patch.

### Measured

| Marina barrage (12 s, then 10 s settle) | physics a step | worst frame | pieces | awake after |
|---|---|---|---|---|
| 12 m/s on the world (as it was) | 3.56 ms | 69 ms | 874 | 0 |
| Energy alone (no speed gate) | 4.78 ms | 505 ms | 2,131 | 0 |
| 20 kJ, ragged rim | 4.0-5.0 ms | 540-560 ms | 1,630-1,720 | 0 |
| 20 kJ, ragged rim, load share | 4.1-4.3 ms | 320-350 ms | 1,510-1,540 | 0 |

Hull diagnosis with the share: from 10 m, 6-15 small breaks and the hull ends on the floor; from 4 m
on a floor of 3 m/s, 4 breaks.

## Found playing Teardown maps (2026-10-26)

- **Unbreakable is not an anchor.** The importer makes rock, hard metal and hard masonry anchors.
  Lee Chemicals' 27 m telephone poles are a third rock from the ground to the top, so a pole cut at its
  foot stood on the rock in its own top. In Voxel Mayhem's maps, dirt alone anchors. Dirt, grass,
  rock, hard metal and hard masonry are unbreakable.
- **Support joins at edges and corners.** A static collider's support (`localSupport`, and the
  whole-body pass for one that splits) counts voxels touching at an edge or a corner as joined
  (`aroundVoxel`). The maps lay shapes onto the grid at an angle, so a diagonal brace is a staircase
  of voxels touching at corners. Joined by faces only, a pylon with its feet cut dropped its uprights
  and left its braces and top in the air (`test_physics_fracture.cpp`, "support: voxels that meet at
  an edge or a corner"). A moving body's islands are still by faces. The marina barrage: 2.4 ms a
  step, unchanged.

## Open: how big a break a big slow body buys

The worst frames left are the biggest boat on the marina, about 1,180 t, falling ~10 m onto its keel:
58 MJ at 9.9 m/s, of which its hardest point carries 41-85% (a keel is narrow). The energy is
real. What is not right is the break it buys: energy over the material's strength, and the game
strengths are low so that cannonballs make big holes (dirt 400 J/m3), so the reach is limited only
by `maxReachVoxels` (64). The boat and the ground each come apart in 6 m balls, and every island pass
over the boat costs 26-46 ms (8 of them in one barrage: 255 ms).

Candidates:
1. **A strike breaks only what the striker reaches in its step**: reach at most the closing speed times
   the step, plus the contact's size. The rest of the energy goes back into the motion (as
   `returnUnusedEnergy` already does against static bodies), so the next step hits again, deeper. A
   cannonball at 50 m/s reaches 0.8 m a step; a boat at 10 m/s, 0.17 m, and crunches in over a few
   steps instead of cratering at once.
2. **The island pass, local**: a break's island pass floods the whole body (the boat: 26-46 ms). Only
   the voxels next to what was cut can have come loose, so a pass from those, stopping when it meets
   the body's anchor or its bulk, costs the break, not the body.

## Not yet

- **Sustained loads** (a heavy body crushing a crate, a dynamic piston on a dynamic wall): the solver's
  impulse every step, with no impact. `contactLoad` gives the impulse. What it lacks is a stress to
  compare against (a material's crushing strength, Pa), and contact areas.
- **Kinematic pushers** against static voxels: Jolt solves no kinematic-static pair, so no force
  exists to measure. A pusher would carry a force of its own, given by the game.
