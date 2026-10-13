# 17 — Physics demos

Seven set pieces for the engine's physics (`runtime/physics.h`), built from voxels in code.

```bash
cd build/examples/physics_demos && ./physics_demos
```

| | |
|---|---|
| **1** | **Pyramid.** 55 crates, ten on the bottom row. **Space** throws a wrecking ball (a metre across, as dense as iron) where you are looking. |
| **2** | **Wall blast.** A running-bond wall of 120 voxel bricks. **Space** sets off a blast behind it. |
| **3** | **Ball barrage.** Balls of five sizes, from 25 cm to a metre, fired at a wall of crates, one every 0.4 s. |
| **4** | **Sweeper.** A 12 m beam spinning about its middle while its middle circles, through a field of crates and balls. |
| **5** | **Titans.** A 14 m stone boulder (about 2,900 t) and a 20 m stone hammer (410 t) launched head-on across a field of 25 kg crates. **Space** launches them again. |
| **6** | **Joints.** A plank bridge on ball joints, a motor-driven windmill, a swinging door, and a four-tonne wrecking ball on a rope that swings into a weight on a breakable chain. **Space** drops a crate on the bridge and fires a ball at the door. |
| **7** | **Car.** A car made of joints, to drive: **W/S** throttle and reverse, **A/D** steer, **Space** brake, with a chase camera. A 40 m run-up to a ramp and jump, a gentler ramp, a crate wall and balls. |
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

**Titans** -- big bodies, and the mass ratio that breaks solvers. Each giant is a grid of 16-voxel
cells moving as one dynamic body (a compound of its cells' shapes), and each is about a hundred
thousand times the mass of a crate. The self-test checks they met and that no crate was pressed
into the floor under them.

**Joints** -- `runtime::Joint`, on an entity of its own, joining two bodies (or a body and the world)
by NetId:
- The bridge: sixteen planks, each joined to the next by two ball joints (one at either side, so
  it hangs without twisting), the ends to the world. It sags under what lands on it, and holds.
- The windmill: a hinge with a motor at 1.2 rad/s. A crate it carries round is flung off. A running
  motor keeps what it drives awake, so a jam that clears does not leave it asleep for good.
- The door: a hinge with limits of 110 degrees either way and a little friction.
- The chain: six links on ball joints with a break force of 9 kN, three times what the 300 kg weight
  pulls at rest. The wrecking ball (a distance joint: a rope that can slacken but not stretch)
  swings into the weight, the chain snaps, and the weight flies on -- a breaking joint gives back
  what it could not hold, so it does not stop what broke it.

**Car** -- joints as a machine. A 900 kg voxel body and four wheels. The rear wheels are on hinges
to the body with motors: driving is a motor's velocity (the throttle), braking is all four motors
held at zero. Each front wheel is on a free hinge to a small knuckle, and each knuckle is on a
vertical hinge to the body whose motor is in Position mode -- a servo -- holding the steering
angle. The wheels collide as spheres, which roll smoothly where a disc of voxels would bump, and
the bodies a joint joins do not collide with each other (`collideConnected`), so a wheel does not
fight the body it hangs from. A front wheel is joined to its knuckle, not to the body, so the
wheels are set out far enough that their spheres clear it.

Every crate, brick and ball is a `duplicateComponent` of one prototype, so all crates share one
geometry blob on the GPU and one collision shape in the physics until a blast edits one.

## Self-test

```bash
DEMOS_SELFTEST=1 ./physics_demos
```

Plays all seven in turn, unattended, and checks each: the pyramid stands and sleeps, the ball brings
it down; the blast makes at least ten fragments; the barrage knocks crates out of the wall; the
sweeper moves most of the field and is not moved itself; the titans meet and no crate is pressed
into the floor; the bridge holds, the chain breaks, the rope never stretches, the windmill runs at
its motor's speed and the door stays within its limits; the car reaches speed upright, turns, stops
under its brakes, and keeps its wheels on their mounts. Then, for the whole session: no step
dropped contacts, no body went non-finite or fell through the floor, and the recording replays
identically. `DEMOS_CAPTURE=<prefix>` saves a frame of each demo as `<prefix>-<n>.tga`.
