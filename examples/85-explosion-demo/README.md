# 85 — Explosion (demo)

A big explosion, three ways: a city, a house on a suburban street, and a pine forest, each blown apart from
its middle by day or on a dark night, while a mushroom cloud of glowing voxel embers rises out of the ruins,
cools to soot and drifts off.

```bash
cd build/examples/explosion_demo && ./explosion_demo                      # the city
./explosion_demo --map house       # or --map forest; 1, 2, 3 switch maps while it runs
./explosion_demo --graphics 1      # 1 | 2 | 3: how much lighting, against frame rate
```

It goes off by itself four seconds after a map is built (Space sooner, R to rebuild).

Where [80-voxel-mayhem-demo](../80-voxel-mayhem-demo/) has every physics feature in one park, a few of each, this one is
about scale: whole buildings taken apart, thousands of pieces in flight, thousands of trees knocked down at
once -- and what it takes to keep that at a frame rate a game could live with.

## The maps

| Map | What stands there | What the blast does |
|---|---|---|
| **city** | A five-by-five grid of blocks round a sixty-metre tower: towers, apartments and shops of concrete, brick, plaster and glass, lit windows, cars, street lamps | Everything within 95 m is taken apart whole, into floor plates and wall panels; within 140 m the side facing the blast is torn out; within 185 m the windows facing it go |
| **house** | A brick house at ground zero with its garden -- fence, shed, car on the drive, trees -- its neighbours either side and across the road. The explosion at half size | The house is rubble; the neighbours are torn open; fences, the shed and the garden trees go down; windows down the street are blown in |
| **forest** | A pine forest 580 m across round a clearing, a cabin by a track | Tunguska: trees near ground zero are shattered; out to 190 m they are knocked flat, lying away from the blast (stripped bare out to 120 m); out to 290 m the wind behind the front bends them hard over |

A map is a `MapSpec` (`explosion.h`): its scale, its radii, its ground, its camera and its builder
(`maps.h`). The scale sizes the explosion against the city's: lengths by it, the embers' speeds by it (they
ride the cloud's field, scaled alike), and what is thrown under real gravity by its square root.

## Controls

| | |
|---|---|
| Space / Enter | detonate |
| R | rebuild the map (it goes off again 4 s later) |
| 1 2 3 | city, house, forest |
| F | fly (mouse, W/A/S/D, Space/Shift, Ctrl fast; Esc frees the cursor) / the orbiting camera |
| Left/Right, Up/Down | turn the orbit, nearer / farther |
| T / P | slow motion (a quarter, a tenth, a thirtieth) / pause |
| N | day / night |
| M | the smoke: pure voxels (embers become smoke puffs) / the grid as voxel blocks / soft volumetric gas |
| E / H / I / K | the ember chunks / shadows / ray-traced GI / TAA |
| [ ] | exposure |

## What it decided

**One strike or edit a thing, never a lattice of them.** Every strike on a body is an island pass over the
whole body. The first version broke each building with a strike every five metres as the front passed --
a hundred strikes a step, physics steps of 100–330 ms, four frames a second for seven seconds. Now the front
acts on each target once (`shockSweep`): a strike that reaches all of it, or a strike at the face toward the
blast, or one edit taking out its windows. Edits, unlike strikes, are gathered into one pass a body a step.

**Its own fracture pattern.** The built-in patterns weigh every voxel against every seed and stop at 256
seeds, which is right for a blow and wrong for a building. `jitteredCells` (registered as `demolish` and
`rubble`) looks only at the cells round each voxel, so one strike takes a sixty-metre tower apart in
milliseconds. `demolish` is a storey tall and centred on the floor slabs, so each piece is a slab with the
walls above and below it: a cell spanning two slabs came apart into two plates, as the engine cuts a shard
into its connected parts.

**Debris is cheap.** Pieces collide as boxes, never break again where they land, lose their lit windows, and
once a piece has lain still for 0.6 s its body goes and it stays where it lies (`settleTick`). Breaking is
metered (about 250 pieces started a step, nearest first) and so is clearing (`cullPieces`: the smallest
first, at most 120 a step), because making or destroying a piece costs the step ~0.15 ms and a symmetric map
breaks eight buildings in the same step. Fragments of eight voxels or fewer are dust.

**Embers are particles, not bodies.** Thousands of sphere bodies were most of the physics step. Each ember
is now dragged through the cloud's field and kept above the ground, nothing more, and drawn between its last
two steps. They are traced in the G-buffer as clusters of 3×3×3 voxel lumps (`traceEmbers`), binned into a
coarse grid each frame (`uploadEmbers`, which touches only the cells in use).

**Trees are edits of groves.** Thousands of trees as prefab copies could be neither edited nor drawn fast
(2,160 overlapping instances made the G-buffer 38 ms). Trees are stamped into one voxel grid per 24 m tile
(`plantGrove`) -- a few hundred components -- and each is a target of its own. The wind tips a tree over
about its stump in two gusts (`tip`): the tree regrown turned, each place in the turned box taking the voxel
it came from, only what still joins the stump put back. No bodies: so a whole forest can go down at once.
Only the groves near ground zero, where trees are shattered, are bodies at all; the rest are edited in the
scene directly, with no island pass or shape rebuild. Two things the edits taught:

- `editVoxels` makes an edit's additions **before** its removals, so a place both taken away and put back
  ends up empty. A tree regrown in place lost its stump, and its crown fell as a body. `tip` takes away only
  what the tree no longer fills.
- Trees lying across each other cut holes in each other, and every scrap a hole left came away as a body
  (2,000 of them made the G-buffer 30 ms). Each grove keeps a bitmap of what is filled (`Grove`): a tree goes
  only where nothing is, and takes away only what it put down.

## Rendering

`nightRenderer/` is the examples' shared renderer ([voxelRenderer](../shared/voxelRenderer/)) with a night
beside its day (`skyParams.x`; **N**), the explosion as a second, shadowed light (`flashParams`), the ember
lumps traced in the G-buffer, and a smoke pass. `--graphics` picks what it does beyond the primary ray:

| | | City, at the cloud's peak (RTX 3070, 1600×900) |
|---|---|---|
| `--graphics 1` | direct light and SSAO | 100–130 fps |
| `--graphics 2` | and shadows: the sun's or moon's, and the fire's | 75–100 fps |
| `--graphics 3` | and the colour bounce and ray-traced GI (the default) | 65–90 fps |

The shadows are the costly part: two rays a pixel, through all the flying debris. In the forest, at
`--graphics 1`, it holds 55–60 fps while the front knocks the trees down and ~105 after; at `--graphics 3`,
~30 and ~56.

## Recording

`EXPLOSION_RECORD=fps` makes every rendered frame exactly one 1/fps step of the simulation, however long it
takes, and saves it (`<EXPLOSION_CAPTURE>_00000.tga`, ...) from `EXPLOSION_RECORD_FROM` (half a second before
the detonation) to `EXPLOSION_EXIT`, at 1920×1080. The machine runs in slow motion; the frames, played at
fps, are real time:

```bash
mkdir -p record
EXPLOSION_RECORD=60 EXPLOSION_EXPOSURE=5 EXPLOSION_LOOK=-190,170,-215,0,35,0 EXPLOSION_CAPTURE=record/f \
  EXPLOSION_DETONATE=3 EXPLOSION_EXIT=21 ./explosion_demo --map city
ffmpeg -framerate 60 -i record/f_%05d.tga -c:v libx264 -crf 18 -pix_fmt yuv420p explosion.mp4
```

High cameras that frame each map: city `-190,170,-215,0,35,0`; house `-55,38,-62,0,6,0`; forest
`-230,170,-260,0,15,0`.

## Unattended runs

`EXPLOSION_LOOK=x,y,z,tx,ty,tz` (a fixed camera), `EXPLOSION_SHOTS=t1,t2,...` with `EXPLOSION_CAPTURE=<prefix>`
(screenshots), `EXPLOSION_EXIT=t`, `EXPLOSION_DETONATE=t`, `EXPLOSION_STATS=1` (a line a second of where the
frame went), `EXPLOSION_GPU=1` (each pass's GPU time), `EXPLOSION_NIGHT=1`, `EXPLOSION_EXPOSURE=x`,
`EXPLOSION_EMBERS=x` (every ember count and rate, times x; 1.5 by default), `EXPLOSION_SMOKE=0|1|2`,
`EXPLOSION_TIMESCALE=s`, `EXPLOSION_SIZE=WxH` (an unattended run's window, not resizable),
`EXPLOSION_PIECES=n`, `EXPLOSION_SETTLE=0`, `EXPLOSION_THREADS=n`, `EXPLOSION_NOCRATER=1`, `EXPLOSION_VERBOSE=1`.
