# Teardown levels as physics scenes: props, holes and joints

Status: **draft**, 2026-10-22. Nothing built yet.

> The levels named here are Teardown's (Tuxedo Labs), read from a local install: the converter's
> input, never in the repo. The checks below run on whatever levels the person running them owns;
> the named ones are where each behaviour was found. Credit: `SOURCES.md`.

Goal: a converted Teardown level that *plays* like the original. Doors hang on their hinges, gates
slide, and West Point Marina's pipe run bends under load and snaps when its supports are shot
away. The test cases are named in "Verification" below.

The voxelizer's Teardown path (`examples/20-mesh-voxelizer/teardown/`, commit 76f7997) converts
geometry well and physics badly. This plan covers what the level files hold that it does not read
(Part A), turning their joints into `physics.joint` entities (Part B), and the two places where the
engine's joints fall short of Teardown's (Part C).

## What was found (2026-10-22)

Reported: converted bodies are "not always aligned or oriented correctly in the world".

Ruled out, with evidence:
- **The writer and loader.** A checker loaded westpointmarina twice through `loadComposeFromDisk`,
  once with bodies separate and once with `--td-bake-dynamic`. It placed every body voxel where the
  engine places it: 1,370,610 voxels in 214 bodies, 100% on or beside a baked voxel.
- **The renderer.** The renderer gallery's frames of the same view of both scenes are identical.
- **Euler order.** `quatEuler` (heading Y, attitude Z, bank X, i.e. Ry·Rz·Rx) is the exact inverse
  of TTFH's Teardown-Converter `QuatToEuler`, which writes these XML files from the game's own
  quaternions. The `.vox` pivot (floor(size/2) in x and y; MagicaVoxel y/z swapped) matches TTFH's
  writer too.

Found instead: three things the importer does not read.
1. **Free-standing holes.** `<voxbox brush="hole">` placed as a sibling, not a child, carves every
   shape it overlaps. The importer only lets a hole carve its parent (`Holes::Skip` in `place`).
   Hollowrock Island cuts a container prefab's doors away with two loose holes and puts a metal door
   in their place; we keep the container's door as a body inside the new one. A scan of every
   converted level found bodies 85-100% inside static geometry in about half of them, the likely
   symptom of this.
2. **`prop="true"`.** A shape is dynamic when it carries `prop="true"`, or when a group passes it
   down: a group's `propN="key=value"` attributes apply to every descendant (westpointmarina's
   `<group name="pipes" prop0="prop=true" prop1="rotstrength=0.5">`). The importer only knows
   `<body dynamic="true">`, so the marina's pipes, Hollowrock's cave doors and many props are baked
   static.
3. **Joints.** None are exported, so every dynamic body is a free rigidbody: a door whose hinges are
   joints falls over when the simulation starts. This is probably most of the "wrong orientation".

westpointmarina alone has 278 `<joint>` elements in main.xml and more in its prefabs: 107 hinges,
5 prismatic (motor-driven gates), the rest ball joints, plus `<rope>`s.

## What a level file holds

From the XML, and from TTFH's `Joint` struct (`entity.h`), which is what the game keeps at run time.

| Attribute | Meaning | Default |
|---|---|---|
| `type` | `ball` (attribute absent), `hinge`, `prismatic`, `cone` (TTFH, binary only so far) | ball |
| `pos`, `rot` | anchor; the axis is the joint's local **+Z** (TTFH maps axis (0,0,1) to identity) | |
| `size` | radius of the sphere searched for the shapes it joins | 0.1 m |
| `rotstrength` | rotational friction: what keeps a bent pipe bent | 0 |
| `rotspring` | spring toward the pose it was made in | 0.5 |
| `limits` | hinge: degrees; prismatic: metres | none |
| `collide` | whether the joined shapes collide with each other | false |
| `sound`, `autodisable` | audio; prismatic sleep | |
| `tags` | `motor`, `motor2`: driven by a level script | |

The XML names no bodies. At load the game attaches a joint to the (at most two) shapes that have
voxels within `size` of its position, and with one shape it is joined to the world. At run time the
game also keeps `strength`, `connection_strength` and `disconnect_dist` (TTFH: "used for planks"),
but these do not appear in level files.

Ropes: `<rope slack strength>` with `<location>` children for the end points; they attach the same
way at each end.

Snapping in Teardown is not a force limit. The voxels a joint is bound to are destroyed, and the
joint goes with them; the pipe itself breaks by ordinary voxel destruction.

## Decisions

| # | Decision | Why |
|---|---|---|
| T1 | Teardown semantics are reproduced in the converter. The engine gains only mechanism a game other than a Teardown importer would also want (physics-jolt D8) | The engine stays free of format-specific rules |
| T2 | Joints become ordinary `physics.joint` entities in `entities.json`, bodies by `projv.netid` | Built, saved, snapshotted and networked already; nothing new to load |
| T3 | A joint's attachment to voxels is engine mechanism (Part C1), opt-in per joint | Any voxel game wants "the hinge goes when the wall it is screwed to goes". Joints that ask for nothing behave as today |
| T4 | Behaviour Teardown gets from scripts (motors on gates, cranes, elevators) is not converted. The joint is written with its motor off and the tag kept in the entity's name | Scripts are not read (README: "What is not"); a game can drive `setJointMotor` |
| T5 | Every `rotstrength`/`rotspring` mapping is calibrated against one level's behaviour, with the factors in one table in the converter | Teardown's units for these are unknown |

## Part A: geometry and bodies the importer misses

Converter only. `level.cpp`, plus `writer.cpp` for the extra bodies.

- **A1. Free-standing holes carve.** A hole shape that is not merged into a parent (a `voxbox` or
  `vox` with `brush="hole"`, or a shape whose voxels are all MAT_HOLE) is collected with its world
  transform instead of being stamped. Once every shape is placed, each collected hole carves the
  world canvas and every body canvas it overlaps (body canvases through `frame.inverse()`). A body
  carved empty is dropped, as empty bodies are now.
  To confirm in the game first: do loose holes carve dynamic bodies, or only static shapes? The
  Hollowrock door says static at least. Check one body case in the editor before relying on it.
- **A2. Group properties.** `propN="key=value"` on any node applies `key=value` to every
  descendant that does not set `key` itself. The walker carries a small inherited-attribute map, and
  `attrBool`/`attrFloat` look there after the node's own attributes. Keys that matter now: `prop`,
  `rotstrength`, `texture`, `tags`.
- **A3. Prop shapes are bodies.** A top-level shape (not inside a `<body>`) with `prop` true becomes
  a body of its own, framed at the shape's world transform. Its child shapes merge into it as now; a
  `prop` shape inside a non-dynamic `<body>` is promoted the same way. Bodies get a stable id at
  creation (A4).
- **A4. Body identity.** Every body (dynamic, and now prop) gets a sequential NetId in walk order.
  Its entity in `entities.json` carries `projv.netid`, which joints need (T2). Static geometry is
  `b = 0`, the world.

Done when the scan that found the overlaps (bodies sharing >15% of their voxels with static
geometry) is clean on every level, except overlaps the game also has. Hollowrock's cave door must
come out as two hinged prop bodies with no container door behind them.

## Part B: joints into entities.json

Converter: a new `teardown/joints.cpp`.

- **B1. Collect.** During the walk, each `<joint>` and `<rope>` is recorded with its world
  transform, its attributes (A2 applied), and the shape it was nested in, if any.
- **B2. Resolve.** After all canvases exist, for each joint find which canvases (world, or each
  body) have a solid voxel within `size` of the joint's position. The nested shape's body comes
  first; the second is the nearest other body, or the world. A joint touching only its own body
  joins it to the world. A joint touching nothing is dropped and counted in the report, and with
  `--td-verbosity 2` each one is printed with its XML offset.
- **B3. Map.** One `physics.joint` entity per joint. Positions are in world units: a metre times
  `--td-scale` (see "Units").

  | Teardown | `physics.joint` |
  |---|---|
  | ball | `point` (C2 adds friction and spring) |
  | hinge | `hinge`, `axis` = world rot × +Z, `limits` degrees → radians |
  | prismatic | `slider`, `axis` = world rot × +Z, `limits` metres × scale |
  | cone | `point` with a swing limit (C2); none seen in level files yet |
  | `rotstrength` | `friction` (hinge, slider; ball after C2), × the factor from T5 |
  | `rotspring` | a position motor holding the made angle, `motorFrequency` from the factor |
  | `collide` | `collideConnected` |
  | `size` | the voxel binding radius (C1) |
  | `tags` | entity `name` (`joint`, `joint:motor`) |

- **B4. Ropes.** A chain of short dynamic segments joined by `point` joints, or a single `distance`
  joint with `slack` as the range, behind `--td-ropes chain|distance|none`, default `distance`.
  `strength` becomes the break force. (physics-jolt R11 wants ropes as a system over Distance
  joints; this does not need to wait for it.)
- **B5. Report.** Joints written, by type; joints dropped (nothing within size); ropes written.

Done when a converted level's doors and gates stay in their frames for a minute of simulation, and
hinged doors swing when pushed and stop at their frames (found on West Point Marina and Hollowrock
Island).

## Part C: engine joints that act like Teardown's

Engine, `src/runtime/physics*`. Mechanism only (T1); these go in as physics-jolt R11 items.

- **C1. Joints bound to voxels.** New `Joint` fields: `bindRadius` (metres, 0 = unbound) and the
  binding itself, the voxel cells of `a` and of `b` within `bindRadius` of the anchor, recorded when
  the joint is made, in each body's component voxel space.
  - When a body's voxels change (an edit, a fracture, an island cut), each bound joint on it counts
    how many of its cells are still solid. If none are left, the joint breaks. It is reported as
    `JointBroke` with a new cause, `voxelsGone`, beside force and `bodyGone`.
  - When `cutIslands` splits a body, a bound joint moves to the piece that holds its cells
    (the most cells, if they are split). Today it stays on the original body whatever happens, so
    a pipe cut in two stays hinged at the wrong end. An unbound joint keeps today's behaviour.
  - The binding is saved with the joint (`entities.json`: `"bind": radius`, with the cells
    recomputed on load, so files stay small) and is part of the snapshot and the state hash.
  - Tests: a hinge bound to a plank breaks when the voxels at the hinge are cleared and not before;
    a bar jointed at one end and cut in the middle keeps the joint on the jointed half; replay is
    deterministic; an unbound joint's behaviour and hash are unchanged.
- **C2. Ball joints with friction and a spring.** `Point` gains `friction` (N m) and
  `springFrequency`/`springDamping` toward the pose it was made in, by building a Jolt
  `SwingTwistConstraint` (with no swing or twist limits unless asked) when either is set. The plain
  `PointConstraint` stays for the frictionless case. Optional swing and twist limits give Teardown's
  cone. Tests: a pendulum with friction decays and stops at an angle; one with a spring returns to
  its rest pose; frictionless is unchanged.

C1 and C2 are independent of each other and of Part B. Part B writes `point` without friction until
C2 lands, and joints without binding until C1 does.

## Units

ProjectV's physics works in metres, and a body's mass is volume × scale³ × density. A Teardown level
converted at the default `--td-scale 10` is ten times its real size to the physics: it falls √10
slower than it should and weighs 1000 times as much. Geometry did not care; physics does.

Options, to decide before Part B is tuned (Open question 1):
- (a) Convert physics scenes at `--td-scale 1`. One voxel is 0.1 world units, so the scene is true
  to scale, but everything else in ProjectV assumes one unit per voxel.
- (b) Keep 10 and have the scene declare a gravity of 98.1 and densities ÷ 1000. Everything
  physical stays consistent at ×10, but forces in other code (a game's explosion) need the same
  rescale.
- (c) An engine-wide "metres per unit" setting in `PhysicsConfig`. The cleanest, and the most
  invasive.

## Milestones

1. **M1 (Part A).** Holes, group properties, prop bodies, NetIds. Re-convert all 21 levels. The
   overlap scan is clean; previews are rechecked. Converter only.
2. **M2 (B1-B3, B5).** Joints exported. A playable check in 17-physics, or a small loader that runs
   `installPhysics` on a converted folder: doors and gates hold.
3. **M3 (C2).** Ball friction and spring in the engine, then the converter's `rotstrength` and
   `rotspring` factors tuned against a jointed pipe run (West Point Marina has one): under its own
   weight with one support removed, it sags and stays sagged.
4. **M4 (C1).** Voxel-bound joints. The pipe run snaps where its supports or joint voxels are shot
   away, and the loose length falls and stays jointed to its neighbour.
5. **M5 (B4).** Ropes.

M1 also fixes the reported misplacement on its own and ships first. M2 depends on M1. M3 and M4
can go in either order.

## Verification

- **Overlap scan** (the checker from 2026-10-22, moved into `--td-verify`): the percentage of each
  body's voxels inside static geometry, listed above 15%.
- **Joint resolution report** per level: written, dropped, by type. Dropped joints are inspected by
  hand for one level.
- **Scenes:** a converted level with each of these. Where they were found:
  - a jointed pipe run, prismatic gates with motor tags, a boat: West Point Marina (group "pipes").
  - a hinged door in a frame: Hollowrock Island's cave door.
  - many overlapping bodies: Quilez Security (30 before M1).
- **Engine tests** as listed under C1 and C2, in `tests/test_physics_joints*.cpp`.
- Determinism: a converted level simulated for 600 steps twice gives identical state hashes.

## Not in scope

- Level scripts, so no motors driven, no elevators, cranes or alarms (T4).
- Vehicles as vehicles. Their bodies and wheels convert as bodies; suspension, steering and drive
  are a separate plan.
- The campaign's compiled `.bin` levels (TTFH's converter could turn them into XML first, and that
  is the user's own install; worth a separate look).
- Teardown's per-voxel structural integrity beyond what physics-jolt's fracture and support do.

## Open questions

1. Units: (a), (b) or (c) above.
2. Do free-standing holes carve dynamic bodies in the game, or only static shapes (A1)?
3. With more than two shapes inside a joint's sphere, which does the game pick: the nearest, or the
   first in file order? Check against a level where it matters before calling B2 done.
4. `rotstrength`/`rotspring` units (T5): calibrate by eye, or measure in the game? A pipe's
   sag angle under a known load in Teardown would give a number.

Sources: TTFH's Teardown-Converter (github.com/TTFH/Teardown-Converter: `src/math_utils.cpp`
`QuatToEuler`, `src/write_scene.cpp` `WriteJoint`, `src/entity.h` `Joint`), and the westpointmarina
and hollowrockisland level files, read from a local install.
