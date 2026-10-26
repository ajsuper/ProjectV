# Cheap debris: small pieces of a break are not bodies

Status: **delivered**, 2026-10-24 (phases 1 and 2; see "Delivered").

> Measured on Teardown levels (Tuxedo Labs) converted from a local install with `mesh_voxelizer
> --td-*`. They are not in the repo and may not be redistributed, so the numbers here are a record,
> not something a checkout can rerun. Credit: `SOURCES.md`.

Goal: a break that makes thousands of small pieces costs what the big pieces cost. A piece of a few
dozen voxels is something to see fly and land, not something to stack, push or stand on -- Teardown's
small debris is particles for this reason. So below a size, a piece is drawn as it is now (its voxels,
a component of its own) but moved by a simple fall-and-bounce against the static world, with no Jolt
body, no shape, and nothing for the island pass, the rebuilds or the solver to do.

## Why: West Point Marina's barrage (2026-10-24)

Voxel Mayhem on the converted level, `MAYHEM_BENCH=barrage` (a blast every 0.4 s for 12 s; made repeatable for
this: shots on physics ticks, aimed by a generator of their own), the physics system's 780 steps:

| | ms a step |
|---|---|
| As it was | 9.1-9.9 |
| Bookkeeping that walked every body fixed (below) | ~1 ms less (by stage) |
| Pieces of 64 voxels or fewer as dust (none made at all): the floor for debris | 5.0-6.1 |
| The piece budget at 1,000 instead of 2,500 | 5.5-5.7 |

By stage, over the 900 steps (temporary timers, removed): Jolt's step 4.1 s; the strikes 0.9 s; the
island pass 0.65 s; making the new bodies 0.65 s; rebuilds 0.2 s. With the small pieces gone, the
three that make pieces fall to ~0.45 s together, and Jolt to 3.2-3.9 s: small pieces cost little in
the solver and a lot in being made. 753 of the 935 pieces alive were 64 voxels or fewer.

What did not help: pieces of up to 8 or 27 voxels as dust (the budget stays full, of slightly
bigger pieces); budget pieces deleted without waking their neighbours (not what keeps pieces awake:
~1,300 are simply in flight, a blast every 0.4 s making up to 512).

## Fixed on the way: bookkeeping over every body

Two passes in the step looked at every body each time anything changed anywhere:

- **Materials (2c)**: any palette version changing -- every break makes components, each with one --
  re-derived every body's material key: 699 ms of the 900 steps. Now each component's palette version
  is kept, and only bodies over a palette that existed and changed are looked at (every body when the
  library changes): 20 ms.
- **Freezing (3b''')**: every dynamic body, every step, for world loading's Kept and holdBody: 227 ms.
  Now only bodies with LoadState, PhysicsHold or FrozenMotion (every frozen body has one): 1 ms.
- A primitive fitted to its voxels (a box) built its voxels' shape to learn their bounds: now only the
  bounds.

The goldens are unchanged by them.

## The design

- `PhysicsConfig::debris`: `maxVoxels` (0, the default: off), `seconds` (how long one lasts),
  `maxCount` (past it the oldest go), `restitution`, `friction`.
- The island pass cuts pieces as now. A piece of `maxVoxels` or fewer from a break of a body without a
  NetId becomes **debris**: its component and entity, a `Debris` component (motion, the sphere it
  collides as, when it was made), no RigidBody, no Piece. `BodyBroken::debris` lists them, apart from
  `pieces` (which handlers shape as bodies).
- Each fixed step, after the solver: each moving debris falls under gravity and is swept as a sphere --
  its voxels' centre, half its content box's smallest side -- against static bodies only (one
  `castSphere`); on a hit it moves to the surface and bounces (restitution along the normal, friction
  across it). Slow and touching for half a second, it rests and is not stepped; when a static body's
  voxels change (an edit, a break) resting debris looks down again, and falls if nothing is there.
  Its pose is written to its Transform, interpolated between steps as bodies' are.
- Not networked: debris is a break's look, not its outcome. A networked body's small pieces stay
  bodies (the server already sends fragments below `dustVoxels` as dust, nothing).
- Not in the simulation: it pushes nothing and nothing pushes it; rays and overlaps do not find it.
  Mayhem's map debris already passes through itself and is aimed through.

## Delivered (2026-10-24)

- `PhysicsConfig::debris` (`DebrisConfig`), the `Debris` component, `BodyBroken::debris`
  (`runtime/physics.h`); the island pass (`cutIslands`) makes them; `stepDebris` runs after the solver
  (step 7c); presentation interpolates them as it does bodies. A debris entity's component goes with
  it (OnUnlink::Destroy, whatever the broken body's link said).
- Tests (`test_physics_debris.cpp`): a crate's nub cut off its bridge is debris, not a body, and rests
  on the voxel floor a sphere's radius up; it goes after its seconds, its component with it; past the
  count the oldest goes; cutting the floor from under resting debris drops it; a networked body's nub
  is a body; off, it is a body. The unit suite: 367 of 367 on boxes and on tiles.
- Voxel Mayhem's maps: debris up to `MAP_DEBRIS_VOXELS` (64); `MAYHEM_DEBRIS=<voxels>` (0: bodies).
  The barrage bench now fires on physics ticks with a generator of its own (repeatable to ~1% in
  pieces; times still vary ~8% run to run), and reports frames and debris.

West Point Marina, `MAYHEM_BENCH=barrage` (12 s of blasts):

| | debris off | debris up to 64 voxels |
|---|---|---|
| Physics a step | 9.0-9.5 ms | 5.9-6.2 ms |
| Frames on average | 8.6 ms | 4.7 ms |
| Worst frame | 73-117 ms | 60-63 ms |
| Bodies at most | 2,715 (the piece budget full) | 658-717 |
| At the end | 6,224-6,492 pieces made | 870-955 pieces made, 1,712 debris alive |

Within reach of the floor measured with the small pieces not made at all (5.0-6.1 ms): what is left
of a small piece's cost is cutting its component, which drawing it needs.

Found after (2026-10-24, playing the marina: "bodies jittering back and forth, never sleeping"), with
`MAYHEM_BENCH_SETTLE=10` (the barrage over, 10 s more, then what is still awake):
- **Debris** stood still in grooves and corners while gathering speed -- 7 to 60 m/s: met by two
  surfaces in one step, each in turn sent its speed into the other, and gravity added more, so it was
  never slow enough to rest. It now slides along up to three surfaces a step, and nothing of its
  velocity or move goes back into one met that step (along the crease, or nowhere). 183-207 debris of
  ~1,250 still moving 10 s on -> 0.
- **Pieces** (Voxel Mayhem's, not the engine's): a break's shards passed through each other for their
  first 15 steps; those that fell into each other meanwhile were inside each other when it ended,
  wedged, and pushed back and forth for good at ~0 speed (Jolt's sleep test watches points move, and the
  solver's position correction moved them) -- and kept every piece touching them awake (Jolt sleeps
  islands whole): 238 pieces awake 10 s on, on tiles and boxes alike. Cut to 4 steps it looked fixed in
  one run; three gave 217, 1 and 233. Any length wedges some, so the window is gone: 0 awake in three
  runs, and the barrage and the 10 s after it cost 3.8-4.0 s of physics instead of 6.7 (the piles
  sleep). A break's first half second costs more (the park's cannon: 234 ms, 182 with the window).
  (Tried and not kept: putting a body that goes nowhere for 4 s to sleep. It must take every body it
  touches with it, or they wake it and their own sleep starts over; and the few wedged bodies that
  never count as going nowhere -- turning past 0.1 rad back and forth -- held their whole piles awake.)
- **Debris turns**: friction acts on the slip where its sphere meets the ground, its spin counted (a
  solid sphere), so debris that lands sliding rolls and tumbles; debris thrown by a break starts with a
  tumble (half its speed over its radius, at most 15 rad/s); a little rolling resistance lets it rest.
- **Debris spins no faster than `DebrisConfig::maxAngularSpeed`** (10 rad/s by default; 0: it never
  turns). 2026-10-25, playing the marina: "rotational velocity of the debris seems to have no limit
  and it looks strange". Small debris that rolls truly spins fast (5 cm at 5 m/s: 100 rad/s); past
  the limit it slides instead, and friction still brings it to rest.

Left: networked breaks make no debris (a client could make its own from the dust a split carries);
debris is a sphere to the world, so a long thin shard lands as a ball would; nothing hits debris
(rays, overlaps, bodies).

## Phases

1. The engine: config, `Debris`, the island pass, the step, presentation, the budget and lifetime.
   Tests: debris falls and rests on voxel ground; bounces off a wall; lasts its seconds; past the
   count the oldest go; resumes falling when the ground under it is cut away; a networked body's
   small pieces are still bodies.
2. Voxel Mayhem's maps: debris at `MAP_DEBRIS_VOXELS` (64). Measured on the barrage.
