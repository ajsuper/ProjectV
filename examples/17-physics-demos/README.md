# 17 — Physics demos

Four set pieces for the engine's physics (`runtime/physics.h`), built from voxels in code.

```bash
cd build/examples/physics_demos && ./physics_demos
```

| | |
|---|---|
| **1** | **Pyramid.** 55 crates, ten on the bottom row. **Space** throws a wrecking ball (a metre across, as dense as iron) where you are looking. |
| **2** | **Wall blast.** A running-bond wall of 120 voxel bricks. **Space** sets off a blast behind it. |
| **3** | **Ball barrage.** Balls of five sizes, from 25 cm to a metre, fired at a wall of crates, one every 0.4 s. |
| **4** | **Sweeper.** A 12 m beam spinning about its middle while its middle circles, through a field of crates and balls. |
| R | restart the demo |
| mouse, W/A/S/D, Q/E | look, fly, down/up (Esc releases the cursor, a click recaptures it) |
| V | walk (a character: W/A/S/D, Space to jump) or fly |
| T / P | slow motion / pause |

The title bar shows the demo, the body count (and how many are awake), and what the last physics
step cost.

## What each one shows

**Pyramid** -- stacking. 55 boxes standing ten high settle and fall asleep in a couple of seconds,
and stay exactly where they were built (the self-test checks the top crate is at 9.5 m). The
wrecking ball shows mass mattering: 4 tonnes at 32 m/s goes through 2.2 tonnes of crates.

**Wall blast** -- destruction. The blast is two things. First the voxels: a ball of space is carved
out of every brick it reaches, and the bricks near it are cracked through (`runtime::voxelsInSphere`
and `runtime::editVoxels`). The engine notices the bricks' voxels changed, rebuilds their shapes,
and splits every brick cut in pieces into bodies of its own (`BodySplit`); a few hundred fragments.
Then, one step later, the kick: everything near is thrown outward, fragments included. (Kicked in
the same step, only the whole bricks would fly: the fragments are made from the edit at the next
step, with the motion their brick had then.)

**Ball barrage** -- mass and momentum. Every ball has the same density, so a metre-wide one weighs
64 times what a 25 cm one does, and knocks crates over where the small ones bounce off.

**Sweeper** -- kinematic bodies. The beam is moved by writing its Transform every frame; physics
follows it (`MoveKinematic`), giving it exactly the velocity that takes, so what it hits is pushed
with the beam's speed. Nothing pushes it back: it stays on its path whatever it runs into. The beam
is 48 voxels long, so a grid of cells -- a kinematic grid body.

Every crate, brick and ball is a `duplicateComponent` of one prototype, so all crates share one
geometry blob on the GPU and one collision shape in the physics until a blast edits one.

## Self-test

```bash
DEMOS_SELFTEST=1 ./physics_demos
```

Plays all four in turn, unattended, and checks each: the pyramid stands and sleeps, the ball brings
it down; the blast makes at least ten fragments; the barrage knocks crates out of the wall; the
sweeper moves most of the field and is not moved itself. Then, for the whole session: no step
dropped contacts, no body went non-finite or fell through the floor, and the recording replays
identically. `DEMOS_CAPTURE=<prefix>` saves a frame of each demo as `<prefix>-<n>.tga`.
