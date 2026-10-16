# 17 — Physics demos

Eleven set pieces for the engine's physics (`runtime/physics.h`), built from voxels in code.

```bash
cd build/examples/physics_demos && ./physics_demos
```

| | |
|---|---|
| **1** | **Pyramid.** A stepped 3D pyramid of 1496 crates (16x16 base, sixteen high), and beside it two towers side by side, thirty crates high, stepping in from 5x5 bases. **Click** throws a heavy ball where you look. **F** launches a 4 m ball where you look: kinematic, at a constant 30 m/s, shoving aside whatever it hits. |
| **2** | **Wall blast.** A running-bond wall of 120 voxel bricks, and round it a brick tower, a house with a doorway, a row of columns and a long low wall, all of fired brick. **Click** sets off a blast where the crosshair is: bricks beside it shatter, those thrown hard enough break where they land. |
| **3** | **Ball barrage.** Balls of five sizes, from 25 cm to a metre, fired at a wall of crates, one every 0.4 s. |
| **4** | **Sweeper.** A 12 m beam spinning about its middle while its middle circles, through a field of crates and balls. |
| **5** | **Titans.** A 14 m stone boulder (about 2,900 t) and a 20 m stone hammer (410 t) launched head-on across a field of 25 kg crates. **Click** launches them again. |
| **6** | **Joints.** A plank bridge on ball joints, a motor-driven windmill, a swinging door, and a four-tonne wrecking ball on a rope that swings into a weight on a breakable chain. **Click** drops a crate on the bridge and fires a ball at the door. |
| **7** | **Car.** A car made of joints, to drive: **W/S** throttle and reverse, **A/D** steer, **Space** brake, with a chase camera. A 40 m run-up to a ramp and jump, a gentler ramp, a crate wall and balls. |
| **8** | **Gear winch.** A four-peg gear on a motor turns an eight-peg gear by its pegs alone; the big gear's drum winds up a chain lifting a 40 kg weight. Hold **Up** to raise it; let go and it holds; hold **Down** and the motor lets go, so the weight unwinds it. |
| **9** | **Trebuchet.** A 36 t counterweight swings a 10.5 m arm on a motorised turntable; a 6 m sling whips a 2 t boulder round and lets it go at four seamless 32 x 24 m walls, each built differently and breaking its own way: ashlar shatters, brick parts at its mortar, planks splinter along the grain, plaster crumbles. The boulder breaks them itself, by their strength. **Click** fires (again: a new boulder, fired); **Left/Right** turn the turntable, mid-throw too; **Up/Down** set the release angle. |
| **10** (key **0**) | **Slide.** Three 14 m ramps -- ice, polished wood, slate -- the same crate on each, sliding into walls of rubber, oak and clay at their feet. **Click** starts the crates again. |
| **11** (key **-**) | **Overhangs.** Four things to cut up, each of a material that breaks its own way and each with something hanging off it: a sandstone arch, a timber tree, a chalk cliff with a shelf, a stalactite and a cornice, and a brick tower with a balcony. **Click** chips where you aim (about 0.7 m) and throws the pieces out; **F** is a harder blow. Cut away what holds an overhang up and it falls. |
| R | restart the demo |
| mouse, W/A/S/D, Space/Shift | look, fly, up/down (Esc releases the cursor, a click recaptures it) |
| click | the demo's action |
| right-drag | hold and drag the body under the crosshair; the wheel pulls it nearer or pushes it away |
| V | walk (a character: W/A/S/D, Space to jump) or fly. Walking, you pass through debris -- every piece a break made -- and a click or F aims through small debris (pieces under a 0.6 m cube) at what is behind it: collision groups, `runtime::definePhysicsGroup` |
| T / P | slow motion / pause |
| H / O | sun shadows / ambient occlusion on and off |
| U / G | the upscaler off (the raw quarter-resolution frame) / its debug view |

The title bar shows the demo, the body count (and how many are awake), and what the last physics
step cost.

## What each one shows

**Pyramid** -- stacking. 55 boxes standing ten high settle and fall asleep in a couple of seconds,
and stay exactly where they were built (the self-test checks the top crate is at 9.5 m). The
wrecking ball shows mass mattering: 4 tonnes at 32 m/s goes through 2.2 tonnes of crates.

**Wall blast** -- destruction, by material. Every brick is 30 kg of "fired brick"
(`physics_materials.json`: strength 40 kJ/m^3, `shatter`, 35 cm shards). The blast is two things.
First, `runtime::fracture` on every brick within 3.5 m, struck at its middle with the blast's energy
falling off with the square of the distance: the bricks beside it shatter into small shards, those
further out lose a corner or crack in two. Then, two steps later, the kick: everything near is thrown
outward, shards included. A brick thrown hard enough into something breaks there too -- the engine
breaks voxel bodies on impact by their material's strength -- while one falling from the top of the
tower does not; shards never break again. `DEMOS_BLASTS=30 DEMOS_SELFTEST=1 DEMOS_ONLY=2` sets off
thirty blasts round the yard and reports the frames' and steps' cost.

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
is a stiff servo; the sling a Distance joint, let go by the demo at the release angle.

Each wall is one static grid of its own material (`physics_materials.json`), and nothing in the
demo breaks it: the engine does, when the boulder hits it. The impact's energy (half the boulder's
mass times its speed squared) against the material's strength sets how far the break reaches; the
material's fracture pattern sets its shape:

| Wall (facing) | Material | Breaks |
|---|---|---|
| Ashlar (+x, ahead) | 1000 J/m^3, `shatter`, 1.2 m | Voronoi shards through blocks and mortar alike, small at the strike, the edge jagged |
| Brick (-z, left) | 1000 J/m^3, `seams`, 0.8 m | Each brick comes away whole; the mortar between crumbles |
| Planks (-x, behind) | 1500 J/m^3, `splinter`, 0.8 m, grain along the wall | Long splinters along the boards |
| Plaster (+z, right) | 700 J/m^3, `crumble`, 0.5 m | Small even pieces, a round hole |

The walls split (`StaticCollider::splits`): what a break leaves hanging -- whole bricks once the
mortar round them has crumbled, the top of a wall cut right through -- falls. The shards go on at the
boulder's speed, fading to the edge of the break, and the boulder gets back
the energy the break did not use. Whether it then gets through depends on the rubble packed in front
of it: through plaster's crumbs it always rolls on, off the floor's edge; in the stone, brick and
planks it sometimes ploughs on and sometimes stops. Shards take after the wall, so they break again
where they land hard, until they are too small for a hit to break (`fracture.minImpactBreak`).
Every piece a break makes is a `Piece`, and the engine keeps at most 3000 of them
(`PhysicsConfig::maxPieces`), clearing the oldest. The demos keep every crumb (`dustVoxels` 0): a
piece vanishing as it is made looks wrong.

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

**Overhangs** -- support, with fracture as the tool. Each structure is a static collider that splits
(`StaticCollider::splits`): after every chip, one pass finds the parts of it that no longer reach its
ground (the component's own y = 0) and drops each as a dynamic body of exactly those voxels and
palette entries, with the chip's shards, in one `BodyBroken`. A click is `runtime::fracture` at what
the crosshair hits, 90 kJ, breaking finer than the material would on its own (`Strike::shardSize`,
0.35 m) like a pickaxe; F is a demolition blow, 3 MJ, breaking to the material's own size. Either
throws its shards on the way the blow went, through what it struck. Cut from one solid, they sit
flush and their friction holds them in place, so for half a second each one's palette entries name
frictionless twins of their materials (`"<name> (thrown)"`, defined at startup: the same density
and strength). How far each blow reaches and what shape the break takes is the struck material's
(`physics_materials.json`):

| Structure | Material | Breaks |
|---|---|---|
| Arch | sandstone, 60 kJ/m^3, `shatter` | Voronoi chips. Cut one pillar and the span hangs from the other; cut both |
| Tree | timber, 40 kJ/m^3, `splinter`, grain upright | Long upright splinters. Cut the trunk through and the crown comes loose |
| Cliff | chalk, 30 kJ/m^3, `crumble` | Round bites. Undercut the shelf, the stalactite, the cornice |
| Tower | tower brick, 50 kJ/m^3, `seams` | The mortar crumbles where you chip; bricks left held by nothing drop out whole |
| Composite wall | concrete, rebar, wall brick, mortar, pine, glass | Each part its own way, in one body (below) |

The composite wall, beside the tower, is 12 m by 8 m of eighth-metre voxels, every colour naming
its material: a concrete frame of three columns and two beams (80 kJ/m^3, `shatter`) with steel bars
running through it, and four bays -- brick in mortar (`seams`; mortar 12 kJ/m^3, `crumble`), pine
boards with a window (25 kJ/m^3, `splinter` along the boards), a glass curtain on pine mullions, and
brick with a small window. Glass is the weakest (2.5 kJ/m^3, small shards) and goes furthest. Every
voxel breaks as far as the blow reaches in its own material, and the rebar is 125 times stronger
than the concrete (10 MJ/m^3): chip a beam and the concrete comes off the bars, which stay, holding
up whatever they still run through; F's blow cuts them too, within about 0.4 m of where it lands.
The bars left standing in the hole catch some of the concrete in front of them, which drops at the
wall's foot; the rest goes on through. The self-test hits the middle beam with F, as a player would,
and checks that thousands of voxels came away, a few dozen of them rebar (where it struck, not along
the 2 m the concrete broke), and that most of the shards went through the wall.

What falls breaks again where it lands hard. Something cut level -- the crown on a trunk cut evenly
all round -- settles on what is left, as it would: chip a notch first, or right-drag it off. The
self-test cuts the trunk, nudges the crown off the stump and checks it fell.

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
