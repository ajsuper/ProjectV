# 17 — Physics demos

Ten set pieces for the engine's physics (`runtime/physics.h`), built from voxels in code.

```bash
cd build/examples/physics_demos && ./physics_demos
```

| | |
|---|---|
| **1** | **Pyramid.** A stepped 3D pyramid of 1496 crates (16x16 base, sixteen high), and beside it two towers side by side, thirty crates high, stepping in from 5x5 bases. **Space** throws a heavy ball where you look. **F** launches a 4 m ball where you look: kinematic, at a constant 30 m/s, shoving aside whatever it hits. |
| **2** | **Wall blast.** A running-bond wall of 120 voxel bricks. **Space** sets off a blast behind it. |
| **3** | **Ball barrage.** Balls of five sizes, from 25 cm to a metre, fired at a wall of crates, one every 0.4 s. |
| **4** | **Sweeper.** A 12 m beam spinning about its middle while its middle circles, through a field of crates and balls. |
| **5** | **Titans.** A 14 m stone boulder (about 2,900 t) and a 20 m stone hammer (410 t) launched head-on across a field of 25 kg crates. **Space** launches them again. |
| **6** | **Joints.** A plank bridge on ball joints, a motor-driven windmill, a swinging door, and a four-tonne wrecking ball on a rope that swings into a weight on a breakable chain. **Space** drops a crate on the bridge and fires a ball at the door. |
| **7** | **Car.** A car made of joints, to drive: **W/S** throttle and reverse, **A/D** steer, **Space** brake, with a chase camera. A 40 m run-up to a ramp and jump, a gentler ramp, a crate wall and balls. |
| **8** | **Gear winch.** A four-peg gear on a motor turns an eight-peg gear by its pegs alone; the big gear's drum winds up a chain lifting a 40 kg weight. Hold **Up** to raise it; let go and it holds; hold **Down** and the motor lets go, so the weight unwinds it. |
| **9** | **Trebuchet.** A 36 t counterweight swings a 10.5 m arm on a motorised turntable; a 6 m sling whips a 2 t boulder round and lets it go at four seamless 32 x 24 m stone walls. Where it strikes, the stone shatters into Voronoi shards. **Space** fires (again: a new boulder, fired); **Left/Right** turn the turntable, mid-throw too; **Up/Down** set the release angle. |
| **10** (key **0**) | **Slide.** Three 14 m ramps -- ice, polished wood, slate -- the same crate on each, sliding into walls of rubber, oak and clay at their feet. **Space** starts the crates again. |
| R | restart the demo |
| mouse, W/A/S/D, Q/E | look, fly, down/up (Esc releases the cursor, a click recaptures it) |
| V | walk (a character: W/A/S/D, Space to jump) or fly |
| T / P | slow motion / pause |
| H / O | sun shadows / ambient occlusion on and off |
| U / G | the upscaler off (the raw quarter-resolution frame) / its debug view |

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

**Trebuchet** -- joints under real load, and fracture. The frame turns on a hinge whose motor the
arrows drive, and the arm's axle is on the frame, so turning mid-throw carries through. The latch
is a stiff servo; the sling a Distance joint, let go by the demo at the release angle. Each wall
is one static grid. A strike scatters Voronoi seeds round it, with a ring of seeds that stay whole
so the break's edge is jagged; each broken cell is cut out of the wall and spawned as a dynamic
shard of exactly those voxels, then kicked on. Shards are at most one chunk (16 voxels) across,
since grids don't split; a wall cut away at its base doesn't fall (nothing checks support yet).

**Gear winch** -- bodies pushing bodies through contact. Each gear is crosses: two straight bars
through its middle, four pegs, laid along the voxel grid's own axes so their faces are flat and
they collide as exact boxes. The winch gear is two crosses 45 degrees apart in one body (eight
pegs), each drawn in its own grid, plus a drum. Only the driver has a motor; the winch gear turns
because pegs push pegs, at half the driver's speed, and the chain's load comes back through them.
The motor has three states: driving (Up), a servo locked at the angle it stopped at (nothing held:
a motor merely held at zero speed lets a load creep down), and off (Down), when the falling weight
spins both gears backwards.
Gears with teeth or radial pegs drawn in voxels jammed -- their stair-stepped edges caught at every
handover -- which is what smooth surfaces (contours) will fix; straight crosses sidestep it.

**Slide** -- physics materials. The ramps and walls name materials from `physics_materials.json`
(staged beside the binary, loaded with `runtime::loadPhysicsMaterials`): each palette entry of a ramp
names its ramp's material, so the three stripes of colour on each are one surface to physics and
merge into one box per grid cell. The crates are identical: friction 1, restitution 0, so the ramp
decides how fast each slides (friction combines as the geometric mean) and the wall how far it bounces
back. The self-test checks both against the textbook: the impact speed against
v^2 = 2 g (sin a - mu cos a) d, the rebound against (e v)^2 / (2 g (sin a + mu cos a)).

Every crate, brick and ball is a `duplicateComponent` of one prototype, so all crates share one
geometry blob on the GPU and one collision shape in the physics until a blast edits one.

## Rendering

`demoRenderer/` is five passes. The scene is traced at a quarter of the window's pixels (half its
width and height; `scale` in `resources.json`), with one primary ray and one sun shadow ray per pixel
-- both through one march call, as the traversal wants. Screen-space ambient occlusion reads that
G-buffer, a shade pass blurs it and softens the hard shadow, and `reconstruct.frag` rebuilds the
frame at full resolution voxel by voxel: every render texel that saw a voxel knows the whole cube, so
each output pixel is composited from how much of it each nearby voxel's silhouette covers. That is
also the antialiasing. It is 30-renderers' reconstruction, made to work for turned chunks (each
voxel's axes come from its chunk's rotation, written to the G-buffer).

`DEMOS_LOOK=houg` sets the H/O/U/G switches for unattended runs; `DEMOS_BENCH=<demo>` times the GPU
on that demo with the shadow ray on and off, interleaved, and prints each pass.

## Self-test

```bash
DEMOS_SELFTEST=1 ./physics_demos
```

Plays all ten in turn, unattended, and checks each: the pyramid stands and sleeps, the ball brings
it down; the blast makes at least ten fragments; the barrage knocks crates out of the wall; the
sweeper moves most of the field and is not moved itself; the titans meet and no crate is pressed
into the floor; the bridge holds, the chain breaks, the rope never stretches, the windmill runs at
its motor's speed and the door stays within its limits; the car reaches speed upright, turns, stops
under its brakes, and keeps its wheels on their mounts; the winch gear turns at half the driver's
speed and lifts the weight, holds it when let go, and lets it unwind on Down; each slide crate reaches its wall and rebounds as its materials
predict. `DEMOS_ONLY=9` (or `DEMOS_ONLY=2,10`) runs just those demos. Then, for the whole session: no step
dropped contacts, no body went non-finite or fell through the floor, and the recording replays
identically. `DEMOS_CAPTURE=<prefix>` saves a frame of each demo as `<prefix>-<n>.tga`.
