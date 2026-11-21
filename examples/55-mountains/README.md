# 55 — Mountains

The Sangre de Cristo at Santa Fe, generated, in voxels from 0.25 m underfoot to 256 m on a horizon
130 km out, under the clear air of 2 km up. Plan: [`docs/plans/mountains-santa-fe.md`](../../docs/plans/mountains-santa-fe.md).

You start on the Plaza, 2,130 m up, looking east: a piedmont of juniper and pinyon, foothills of
ponderosa, and 15 to 20 km off a crest at 3,800 m with snow on its north faces and cirques under its
summits. West across the Rio Grande the Jemez is a blue band 60 km away; the Sandias stand 70 km to
the south-west. Beyond about 95 km the world is generated province by province, without end.

## Build and run

```bash
cmake --build --preset dev --target mountains mountains_lab -j$(nproc)
cd build/examples/mountains && ./mountains
```

| Input | Action |
|---|---|
| `W` `A` `S` `D`, `R` / `F` | Move; up / down |
| Mouse | Look (`Esc` releases the cursor, a click captures it) |
| `Shift` / `Ctrl` | x20 / x0.1 speed |
| `G` | Walk: stay 1.7 m over the ground |
| `[` `]`, `,` `.` | Lower / raise the sun; turn it |

| Environment | |
|---|---|
| `MOUNTAINS_CAMERA` | `plaza` (default), `atalaya`, `baldy`, `aerial`, or `x,z,above,yaw,pitch` (m and radians; yaw 0 is east) |
| `MOUNTAINS_SEASON` | `october` (default: gold aspens, rust oak) or `november` (bare, more snow) |
| `MOUNTAINS_SUN` | `azimuth,elevation`, degrees, azimuth clockwise from north (default `250,22`, late afternoon) |
| `MOUNTAINS_SEED` | The world |
| `MOUNTAINS_RINGS`, `MOUNTAINS_VOXEL_RINGS`, `MOUNTAINS_N` | How many rings (11), how many of them are voxels (5; the rest are heightfields), and how many voxels from a ring's centre to its edge (512) |
| `MOUNTAINS_CAPTURE` | Save a screenshot once everything has generated, print the frame time and GPU time by pass, exit |
| `MOUNTAINS_FLY` | Fly straight ahead at this many m/s (for whether streaming keeps up) |
| `MOUNTAINS_RENDERER=basic` | The engine's basic renderer instead of `renderer/` |
| `MOUNTAINS_AIR`, `MOUNTAINS_EXPOSURE` | The air (`density,haze,g,multiple`) and the exposure, for tuning |
| `MOUNTAINS_DEBUG` | `noshadow`, `noskip`: turn parts of the renderer off to measure them |

`./mountains --bench` generates every ring around the camera with no window and reports the time and
memory per ring. `./mountains --selftest` generates, flies 5 km and checks for holes, stray cells and
leaks (exit code 0 when it passes). `./mountains_lab` draws the landform in 2D in seconds -- a relief map of 160 km, a
closer one of the Plaza to the crest, profiles and panoramas from the Plaza -- and prints elevations at
reference points (the Plaza, Atalaya, Baldy, Redondo, Sandia Crest, ...) against their real values;
`--check` measures how well each ring agrees with the next.

## How it works

**The ground** (`include/mountains/landform.hpp`) is a function of position: a base falling toward the
Rio Grande; ranges, a volcanic dome, hill masses and a basalt mesa field laid out like Santa Fe's
surroundings; erosion as gullies octave by octave, each following the slope the octaves before it left;
detail; cirques; the river's gorge. Erosion is chaotic, so the slope each octave follows is read from a
lattice every caller shares: a 0.25 m ring and a 256 m ring agree on where every valley is.

**The rings** (`main.cpp`, `include/mountains/columns.hpp`). Eleven, the first five a `SceneGrid` each of 64^3
chunks, voxels doubling ring to ring (0.25 m to 4 m, to about 2 km); past them six heightfield rings, the
ground as a smooth surface the renderer marches, to 131 km. Voxels double ring to ring, so a voxel is about the same few pixels wide at every distance and the cost
grows with the log of the distance. A ring moves a cell at a time with the camera; it generates the hole
under the finer ring too, and hides it only once the finer ring has the ground there, so nothing is ever
missing while it streams. Columns are generated on a pool of threads: the ground sampled a voxel column
at a time, filled to just below its lowest neighbour, trees stamped in, tile tables built.

**Trees** (`include/mountains/trees.hpp`) stand on a world grid by the canopy cover and the elevation
band: junipers and pinyons low, ponderosa, fir and spruce up the slopes, aspen. Drawn as voxels where
voxels are 2 m or less; beyond, a canopy voxel stands up off the ground.

**The renderer** (`renderer/`) is the gallery's [fast](../30-renderers/renderers/fast/) with the air
added: an analytic atmosphere (air and haze, optical depth in closed form) for the sky, the ambient light
and the aerial perspective on everything by its distance; the sun's colour from its airmass, through
ozone; shadows and AO sized by voxel; a depth pre-pass so camera rays skip the rings in front; past the voxels,
the far field marched as a heightfield (`include/mountains_far.sc`), with detail finer than it put back in the
normal.

## Files

```
main.cpp                      the rings, streaming, the camera, the renderer's uniforms
include/mountains/landform.hpp   the ground
include/mountains/surface.hpp    materials: what the ground is made of, by elevation, slope and aspect
include/mountains/columns.hpp    one column of a ring as chunks
include/mountains/trees.hpp      trees
renderer/                     render.json, resources.json, shaders/, include/mountains_sky.sc
tools/lab.cpp                 mountains_lab
```
