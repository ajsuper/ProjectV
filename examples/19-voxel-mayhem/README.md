# 19 — Voxel Mayhem

Every physics feature the engine has, in one world, all running at once: a 220 m park of voxels laid
out round a plaza, with something to break, ride, drive, launch or knock over in every direction, and
eight tools to do it with.

```bash
cd build/examples/voxel_mayhem && ./voxel_mayhem
```

Where [17-physics-demos](../17-physics-demos/) shows each feature alone, in a set piece of its own, this
one puts them all in the same simulation -- which is what a game does, and where the features meet: a
cannonball through a castle wall throws shards onto the jump pads, which launch them into the balloons;
TNT by the gate takes the gate and the wall with it.

## The park

| Where | What | Engine features |
|---|---|---|
| **South** — playground | A ball pit of 140 rubber balls behind a gate; stand on the yellow plate and the gate opens | Sensor (`StaticCollider` on the sensor layer), `runtime::touching`, a hinge with a Position motor |
| | Three jump pads, fed by a ball fountain | Sensors, `ContactBegan`, `setVelocity`; for the walker, `setCharacterInput` with a big jump |
| | Helium balloons on ropes, eight tethered, three clusters that carry their crate off into the sky | Negative `gravityFactor`, Distance joints that slacken and break (`breakForce`), leaving the world (`BodyLeftWorld`) |
| | Three slides of ice, polished wood and slate into walls of rubber, oak and clay | Physics materials (`physics_materials.json`, palette entries naming them) |
| | A car made of joints, and a stunt ramp | Hinges with Velocity motors (drive, brakes) and Position motors (steering servos), sphere wheels |
| **Centre** — plaza | A Ferris wheel, eight gondolas each carrying a crate | A hinge motor; ball joints; the wheel and gondolas in one collision `family` so they never collide |
| **North** — castle | Four walls of ashlar, brick, plaster and planks, each breaking its own way; a brick keep with a balcony | Static colliders that `splits` and `breaksOnImpact`; the four built-in fracture patterns |
| | A crane's 4 t wrecking ball (**B**: let go, wind back) | A Distance joint (the rope), a Point joint to the world (the latch), `teleport` |
| | A composite wall: concrete, rebar, brick in mortar, pine, glass | Per-voxel materials, each breaking as far as the blow reaches in it |
| | A stack of TNT by the gate | `runtime::impacts` (TNT hit hard goes off), chain reactions |
| **East** — stacks | A 224-domino spiral set off by a pressure plate, a crate pyramid, a Jenga tower | Stacking and sleep, a sensor trigger, `addAngularImpulse` |
| **West** — machine yard | A rope bridge of 24 planks; three chains that snap under 9 kN | Point joints, `breakForce`, `JointBroke` |
| | A piston elevator carrying balls up 8.5 m every seven seconds | A Slider joint with a Position motor |
| | A bungee platform hung on four springs | Distance joints with `springFrequency` |
| | The Blender: a 12 m beam spinning through a field of crates | A kinematic body moved by its Transform |
| **North-west** — quarry | A sandstone arch, a timber tree, a chalk cliff, a shale cliff | Splitting colliders; `registerFracturePattern` -- the shale breaks in slabs, a pattern defined in `main.cpp` |
| | An earth hill held up by bedrock: dig under it and it falls | `PhysicsMaterial::anchor`, `editVoxels` / `voxelsInSphere` |
| **North-east** — bowling lane | A 6 m stone ball (**N**) into ten 5 m pins | Capsule-shaped bodies fitted to their voxels, a sphere |

## Controls

| | |
|---|---|
| mouse, W/A/S/D, Space/Shift | look, fly, up/down (Esc frees the cursor, a click takes it back) |
| 1–8 or the wheel | the tool; a click uses it (below) |
| right-drag | hold and drag a body; the wheel pulls it nearer or pushes it away |
| V | walk (a character controller) or fly; walking, you pass through crumbs of debris (collision groups) |
| E | drive the car when within 14 m of it (W/S, A/D, Space brakes); E again to get out |
| B / N | the wrecking ball / bowl (again: re-rack the pins) |
| G | gravity: Earth, the Moon, none (`setGravity`) |
| T / P | slow motion / pause |
| F5 / F9 | snapshot the physics / rewind to it (`savePhysics` / `restorePhysics`: refused once anything has been made or broken since -- no world rollback, by design) |
| R | rebuild the world |
| H / O / K | sun shadows / AO / TAA |
| U | debug view: AO only, sun visibility |
| [ ] | the sun |
| F1 | help |

| Tool | |
|---|---|
| 1 cannon | a half-tonne iron ball at 50 m/s, continuous collision |
| 2 blast | an explosion where you aim: every breakable surface a ray from it reaches is struck (`runtime::fracture`), then everything near is thrown |
| 3 pickaxe | chips where you aim, finer than the material breaks on its own, and throws the chip out of the face (frictionless twins of the materials for half a second, as in example 17) |
| 4 dig | removes every voxel within 1.5 m of where you aim: what it cuts loose falls |
| 5 spawn | a crate, barrel (capsule), bouncy ball, glass box (breaks on impact) or balloon; Q picks which |
| 6 TNT | thrown; goes off when it hits hard, and sets off the TNT round it |
| 7 orb | a 4 m kinematic ball on a `ConstantVelocity`, stopped by the floor and rolling on along it (`blockedByStatic`) |
| 8 push | shoves everything in a cone in front of you |

The camera shakes at impacts and explosions, by their energy and by how far off they are.

## What it decided

**Big breaks are tuned, not just allowed.** A cannonball through the castle wall made about 1,450
pieces and a 30 ms frame. Measured (`MAYHEM_BENCH=cannon`, and `MAYHEM_ONLY=castle` to build nothing
else), the rest of the park made no difference: the cost was the break, and most of the pieces came
from shards breaking again where they landed. Two settings fix most of it:
`PhysicsConfig::fracture.minImpactBreak = 2` (a shard breaks again only on a hit that would break two of
its material's shard sizes) and one collision family for a strike's shards for their first quarter
second (they are born flush against each other, and the solver spent the break's first steps on every
shard against its neighbours). Together: 40% less physics in the first half second, the worst frame
from about 30 ms to about 19 ms. What remains is the engine making a few hundred bodies in one step.

**System order with physics added at startup.** `installPhysics` adds `"physics"` during Startup, after
`main` has added the game's systems, and the Application places a new system between everything that
names it in `before` and in `after` -- so a system ordered `before = "physics"` must be added ahead of
any ordered `after = "physics"`, or the two contradict and physics is appended last (and every "after
physics" system silently runs a step early). `main` adds them in that order, with a comment.

**A gate in a gap jams.** The ball pit's gate hangs just outside the wall's face: turning inside the gap,
its corners swung into the wall beside it and the hinge's servo could not move it.

## Rendering

`examples/shared/voxelRenderer/` (shared with the other games and the renderer gallery;
`examples/shared/voxel_renderer.h` sets it up): a G-buffer (one primary and one sun shadow ray per
render pixel, through one call of the march), AO at two scales, the shade, the voxel reconstruction
(the upscale and the edge antialiasing), TAA and the display. Being rebuilt for speed first, then
feel, then GI: [docs/plans/voxel-renderer.md](../../docs/plans/voxel-renderer.md).

## Self-test

```bash
MAYHEM_SELFTEST=1 ./voxel_mayhem
```

Sets everything off, unattended, and checks: the wrecking ball breaks the castle wall, the bowling ball
knocks pins over, the domino plate starts the spiral, the gate plate opens the gate, the elevator rises,
the Ferris wheel turns at its motor's speed, the balloons are aloft, the jump pads launch things, the TNT
stack goes up in a chain, the breaks make pieces, nothing falls through the floor, and every body is
finite. `MAYHEM_CAPTURE=<prefix>` saves a screenshot of each part of the park as it goes.
