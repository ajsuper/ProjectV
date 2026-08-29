# 40 — Advanced Renderer

A real-time **world-space probe** global illumination renderer for ProjectV voxel scenes, with animation, transparency and refraction. One renderer, fourteen passes, no menu — it loads a Compose scene from disk, frames it, and lets you fly around it under a sun you can move.

It began as the `world-cascade` renderer of the [renderer gallery](../30-renderers/) example, which exists to compare seven approaches side by side. This one exists to be the good approach, brought up to the engine as it is today. See [What changed from the renderer gallery](#what-changed-from-the-renderer-gallery) for the specifics.

## The pipeline

`advancedRenderer/render.json` is the pass order; `advancedRenderer/resources.json` names the shaders and the targets they write.

| # | Pass | Writes | What it does |
|---|------|--------|--------------|
| 1 | `gbuffer` | position, normal, albedo, direct, **face key** | One primary ray per pixel, which is also where **animation, transparency and refraction** happen — one `raySceneIntersect` with a `RayQuery` that asks for all three. Writes the **crisp half of the image** (soft-shadowed direct sun plus voxel emission), kept out of every temporal filter downstream. |
| 2 | `godrays` | shafts (half res) | Screen-space sun shafts, from the depth buffer. Cheap, and wrong in the ways screen space is wrong. |
| 3 | `volumetric` | shafts (half res) | The traced half of the same effect: real shadow rays through the fog. The expensive path, and the one that is right off-screen. |
| 4 | `volblur` | shafts | Blur, because the two above are one sample per pixel at half resolution. |
| 5 | `probe` | probe atlas | The world-space gather. Each probe texel casts **one real voxel ray**; probes are anchored to **voxel face centres**. This replaced four radiance cascades — see below. |
| 6 | `probefilter` | probe atlas | Spatial filter across neighbouring probes, gated on inter-probe **visibility** so light does not bleed through a wall. |
| 7 | `resolve` | indirect | Probe atlas → one indirect irradiance value per pixel, cosine-weighted and bilaterally interpolated. |
| 8 | `accumulate` | indirect + history | Temporal mean of that indirect term **alone**, gated on **exact voxel-face identity** with a geometric fallback. Ping-pong; the engine detects it. |
| 9 | `compose` | HDR | `direct + albedo * indirect`, where the indirect is **flat across each voxel face**, plus a sun sheen and, last of all, the **atmospheric fog**. |
| 10 | `upscale` | HDR | Reconstruction from the render scale to the output grid. `Q` bypasses it to point-sampling, which is the A/B that isolates the magnify. |
| 11 | `taa` | HDR + history | Resolves the primary ray's sub-pixel jitter into anti-aliased edges. Catmull-Rom history fetch so motion stays sharp. Ping-pong. |
| 12–13 | `bloomdown`, `bloomblur` | bloom chain | Downsample-and-blur for the bloom, at a quarter and an eighth. |
| 14 | `display` | the window | Adds the shaft and bloom terms, then vignette, exposure, ACES, gamma, contrast, saturation, dither. All the colour grading lives here. |

**The name is now historical.** This began as a four-cascade radiance-cascade renderer (`cascade3` →
`cascade0`, one atlas each, merged coarsest-first) and the cascades are gone: `probe` + `probefilter`
do the same job with one atlas, because anchoring probes to voxel *faces* made the cascade hierarchy's
angular refinement redundant — a face's indirect light is one number, so there is nothing for a finer
cascade to resolve. The `cascadeShaders/` directory name and `pjv_cascade_common.sc` still carry the
old word; renaming them is a separate change with no behaviour in it.

### Why the light is separated from the albedo

Lighting is low frequency and albedo is not. A voxel boundary is a step in albedo, but the light arriving either side of it is nearly the same. Blurring a lit image blurs the voxel colours with it; blurring the light on its own and multiplying a sharp albedo back in afterwards does not. That is why `resolve` writes irradiance rather than colour, `accumulate` averages only that, and `compose` re-applies the albedo at the end.

The same split is why direct sunlight never enters the temporal accumulation: a shadow edge is high frequency too, and it is already noise-free.

### Why the lighting is per voxel face

Indirect light on a voxel face is *one number*. The screen-space probe grid this renderer started
with did not know that: it sampled a signal whose natural rate is per-surface-element on a grid
spaced in pixels, so the density was set by how far away you happened to be standing rather than by
the geometry. Embracing the real rate changes three things:

- **The gather origin is the voxel face centre** (`gFace`), so every screen probe landing on a face
  traces from one camera-independent origin. Probe spacing goes from 8 px to 16 — a quarter the
  probes and a quarter the gather rays — and the result is steadier rather than coarser, because
  the origins stop drifting as the camera moves. It also replaces the world-grid snap that used to
  approximate this; the voxelisation already *is* that grid.
- **Temporal history is gated on exact face identity** (`gKey`), built from `hit.voxelCoord` — the
  integer the DDA carried through the march. Two pixels on the same face share a converged value
  whatever either did last frame.
- **A face's value only ever comes from a probe standing on that face.** A probe's gathered radiance
  is camera-independent: face-centre origin, face normal, fixed direction set. But *which* faces own
  a probe is decided by a lattice in screen space, so an interpolation across neighbouring faces is
  a viewpoint-dependent answer to a viewpoint-independent question — which is why the lighting used
  to change as you flew around, and change systematically with distance. `resolve` now flags whether
  a sample came from this face, and `accumulate` treats the flag as authority: interpolated samples
  bootstrap a face and stop counting the moment a real one arrives. A face is then simply refreshed
  at irregular intervals with the same number, which is invisible.
- **The composite reads one value per face.** Every pixel on a face projects the same face centre
  through the same camera, lands on the same texel, and reads the same accumulated irradiance — so
  the indirect term is exactly flat across the face, by construction. Direct sun, emission, albedo
  and specular all stay per-pixel.

Everything read at a probe anchor is **point-sampled** (`pjvSnapToTexel`). Render targets are
filtered by default and probe anchors land on pixel boundaries, so an unsnapped fetch averages two
to four neighbours — which for a face centre yields a point on neither face, sliding continuously
as the camera moves, and for a face key yields an identity matching neither.

The flat lookup needs a check rather than trust: the lookup reads the key at that texel and requires it
to be this face, which rejects a face centre that is off screen, occluded, or too small to own its
own texel. That decision depends only on the face and the camera, never on the pixel, so a face is
never part flat and part filtered.

**Why not store per face directly?** Because the engine is fragment-only. A fragment shader writes
the pixel it rasterizes, so nothing can scatter a face's radiance into a slot indexed by its
identity — which is what a real surface cache needs. `materialListIndex` off the hit is already a
dense per-voxel integer that would address one. That is the next step, and it needs a pass type that
draws points with a custom vertex shader.

### The atmosphere

Four effects were added together because they are one idea: the renderer had no **air** in it. Light
left a surface and arrived at the eye unchanged, the sky was a gradient with a disk pasted on, and
the only warmth in the frame was whatever the albedo happened to have. Each of these is cheap enough
to be free next to a voxel DDA, and none of them costs the temporal filters anything — every one is
a pure function of the camera, the sun and the G-buffer, so two frames of a still scene are still
bit-identical.

- **Height fog** (`compose.frag`). Density falls off exponentially with height, so a scene gets a
  haze layer sitting in it rather than a flat tint over everything, and the optical depth along the
  view ray has a **closed form** — two `exp()` calls and a divide, no marching and no volume texture.
  It supplies the two depth cues a voxel scene otherwise lacks entirely: contrast falling with
  distance, and hue sliding toward the sky. It is also scene-relative. `main.cpp` inverts
  Beer-Lambert on one number — `FOG_DEPTH_AT_RADIUS`, the fraction of the light lost over one
  bounding-sphere radius — so "fog" means the same fraction of the scene's extent whether the scene
  is a 32-voxel model or SanMiguel at 32768.
- **Sun inscatter**, in the fog and again as the sky's **aureole** (`pjv_sun_sky.sc`). One phase
  function, shared, because both describe the same haze: the aureole is what it does to the sky and
  the inscatter is what it does between the eye and a surface. The aureole is picked up by the GI for
  free — the probe gather's sky miss reads `skyGradient()` — so a surface facing the sun's half of
  the sky now gets warmer ambient than one facing away, which is a directional cue a bare gradient
  cannot give it.
- **Warm key, cool fill** (`SUN_WARMTH` / `SKY_WARMTH`). The warmth is applied to the **sun**, not to
  the finished frame. That distinction is the whole point: tinting the key light warms lit surfaces
  and the bounce they feed into the gather while leaving sky-lit shadows cool, and it is that
  separation — not the absolute hue — that reads as sunlight. A white balance in `display.frag` moves
  lit and shadowed pixels by the same factor and cannot produce it, which is why `TINT` there stays
  small and only finishes the job.
- **A sun sheen** (`compose.frag`). One Blinn-Phong lobe, gated on Fresnel so it is nearly absent
  head-on and shows as a rim at grazing angles, reusing the sun visibility that was already traced
  and blurred. It is the only term in the renderer that is **view-dependent**, which is exactly what
  it is for: everything else is view-independent by construction, and a scene where nothing changes
  as you fly around it reads as matte cardboard.

**How the translucency found its shape.** It began with the standard Frostbite formulation, whose
transmission axis is the light direction bent off the surface normal by a distortion term. That bend
is what widens the band of angles a blade glows over, and it was wrong here for a reason the
[per-voxel mode](#per-face-or-per-voxel) made obvious: with the bend in, every face of a voxel gets a
*different* axis, so the glow is a different value on each of the three faces you can see and the eye
reads three flat patches rather than one object with a lit edge. Per-voxel mode drops the bend because
it has no single normal to bend around — and what appeared was the effect finally working, a **rim**
along the silhouettes of grass and canopy, which is what backlit foliage actually does.

So the axis is now `-L` in both modes, and coherence rather than width is what the term is built on;
`SSS_POWER` buys back the width the bend used to. The two modes then differ only in how that one
magnitude is *distributed*, and the split falls out of what each mode means. Per-voxel owes every face
of a voxel one identical number, so it takes the magnitude flat. The per-face mode has no such
obligation and spends it as a gradient **across** each face: brightest at the voxel's edges, skewed
toward the sun (light wraps around the edge nearest the lit side), and falling to `SSS_EDGE_FLOOR`
rather than to black in the middle. That last part matters — a hard rim would outline every foliage
voxel and read as wireframe, which is the artefact any per-voxel effect invites. What varies is how
much brighter the edge is, not whether the face glows at all.

- **Bloom** (`bloomdown.frag` → `bloomblur.frag`). Emissive voxels are the brightest things in a
  voxel scene, and without a halo they are bright *pixels* rather than light *sources*. This was first
  attempted inside `godrays.frag` — a screen-space kernel over the emission channel, at half
  resolution, in one pass — and failed twice for one underlying reason: a halo needs a wide, very
  smooth falloff, and a single-pass kernel buys width only by spreading a fixed number of taps
  thinner. Spread them and the gaps show, as speckle when the pattern is randomised, as spokes when it
  is not, and as blocks wherever a depth test cut the kernel along a silhouette.
  A bloom gets its width from **resolution** instead of from tap count, which is the whole trick: blur
  at a fraction of the frame and a small kernel reaches tens of output pixels.

  It is **two scales**, and that is not an optimisation — it is the fix for the single-scale version
  looking like a square. A one-voxel emitter covers a handful of pixels, which at an eighth resolution
  is a fraction of one texel; blur that and magnify it eight times bilinearly and what reaches the
  screen is a bilinear *tent* over a single texel, a diamond with straight edges and no bright centre.
  The shape comes from the magnification, not the kernel, so no amount of blurring touches it. Real
  bloom is a bright tight core with a faint wide tail, and summing a quarter-resolution pass with an
  eighth-resolution one is the cheap way to get both. `BLOOM_CORE` balances them.

  It also **respects geometry**, which a lens bloom does not. The glow wanted here is lit *air* around
  an emitter, and air behind a solid object is not lit by it — so a lamp's cover should shadow the
  glow above it. The test that gets this right is not the obvious one: comparing the source's depth
  against the receiver's fails, because the lamp is *nearer* than the sky above the cover and "nearer"
  is exactly the case a glow should spill across. What separates the cases is what lies **between**
  them, so `bloomblur` samples the depth at the midpoint of each tap's line. The cover lands there and
  is far nearer than the sky at either end, so it blocks; open sky to the side of the lamp does not,
  so the glow spills into it as it should. One extra fetch per tap. Its limit is resolution — an
  occluder only a few pixels wide falls between the samples and its shadow is missed, which fails
  gracefully by simply carrying on through.

Two smaller ones ride along in `display.frag`: a **vignette**, applied in linear light before the
tone map because that is what a lens does, and a **dither** of one output quantum, applied after
everything. The dither is not a look — the sky gradient and the fog are both smooth ramps over
hundreds of pixels, and 8-bit output quantises them into visible bands without it. Both its noise
and the AO's tap rotation are hashed from the pixel alone and **not** the frame, so neither can
flicker on a still image.

Everything here has an off switch. `V` cycles the fog off / normal / heavy, `ADVANCED_FOG=<mult>`
sets it at launch, and setting `SUN_WARMTH` and `SKY_WARMTH` to `vec3(1.0)` restores the scene
editor's light rig exactly, term for term.

### Per-face or per-voxel

`U` switches the granularity of **every** lighting term — indirect, shadow, translucency and cosine
alike — so that a voxel is lit as one object rather than as six faces. The key format, `gFace`, the
temporal gates and the upscaler are all untouched; each term simply asks its question of the voxel
instead of the face:

| Term | Per face (default) | Per voxel (`U`) |
|---|---|---|
| **Indirect** — what a probe integrates | the hemisphere about one face's normal, from that face's centre | the whole **sphere** around the voxel: a face is chosen per ray and its hemisphere cosine-sampled from that face's centre, an unbiased estimate of the mean over all six |
| **Indirect** — which samples `resolve` counts as its own | `pjvSameFace` | `pjvSameVoxel` — any probe on this voxel, whichever face it stood on |
| **Shadow** — where the sun ray starts | the shading point, gated on `NdotL > 0` | the **voxel centre**, pushed out along the sun by half a diagonal, and *not* gated on facing |
| **Shadow** — what the blur may average | same normal, same plane | that, **or** any tap on this same voxel, with the coplanarity falloff skipped |
| **Cosine** `N·L` | the face's own | one number for the voxel — see below |
| **Translucency** | lobe bent around the face normal by `SSS_DISTORTION` | the bend dropped, leaving a transmission axis of exactly `-L`: light and eye only |
| **Sun sheen** | a Fresnel-gated specular lobe | **removed** — a highlight is a statement about one face's orientation, and there is no version of it that is also the same on all six |

**The cosine factor is the one irreducible choice.** A shadow can be pooled and a lobe can drop its
normal, but "how obliquely does the light strike" is a question about an orientation and a voxel has
six. Both defensible answers are computed from the light direction alone, so neither is a magic
constant: the **mean** over the six faces is `(|Lx|+|Ly|+|Lz|)/6` (opposite faces never both face the
light), which conserves energy exactly and is markedly darker on everything you can actually see; the
**brightest face** is `max(|Lx|,|Ly|,|Lz|)`, which is not energy-conserving but leaves a flat wall at
the brightness it already had. `PER_VOXEL_BRIGHTNESS_MATCH` lerps between them and defaults to the
brightest face, because a mode that dims the whole image threefold cannot be compared against the
mode it replaces — every difference is swamped by the exposure change. On an axis-aligned surface
whose normal is the sun's dominant axis the two modes are *exactly* equal, so the flattening shows up
where it should: on isolated voxels, edges and silhouettes.

**Why the shadow ray had to move rather than be averaged.** `gDirect.a` conflates "something is
between me and the sun" with "I am facing away from the sun" — both are written as zero. Pooling a
voxel's faces from that channel would count orientation twice, once in the mean and again in the
cosine term, and every voxel with a lit and an unlit face on screen would come out half dark in open
sunlight. Asking the voxel directly costs the same one ray and means purely "is this voxel in
shadow", leaving facing entirely to the cosine term. It also incidentally repairs the translucency's
weakest input: the regional-sun estimate reads the same channel, and a backlit blade's neighbours are
all `NdotL <= 0`, so per-face it was reporting "no sun" precisely in the case the effect exists for.

Neither mode is a fallback for the other. Per face is the finer and more correct answer. Per voxel is
a **sixth as many distinct values to converge**, and `resolve` gets up to six times the samples per
frame for each of them on top of the several texels per face it already averages — so it settles
faster and holds still through more camera motion, at the cost of flat-shading every voxel. It is
also the granularity a real surface cache indexed by `materialListIndex` would store, which is the
next step named at the end of [Why the lighting is per voxel face](#why-the-lighting-is-per-voxel-face).

It does **not** help the upscaler, which was worth checking: `upscale.frag` reconstructs edges by
testing camera rays against the face planes in `gFace`/`gNormal`, and that is a geometry question.
Making the *lighting* coarser leaves every one of those tests reading exactly what it read before.
What does change is the composite it magnifies — with fewer distinct values in it, there is less for
a reconstruction error to be wrong *about*.

### Two kinds of god ray

`.` adds a **traced** volumetric pass alongside the screen-space one. They are complements rather
than alternatives, and they fail in opposite directions:

| | Screen space (`godrays.frag`) | Traced (`volumetric.frag`) |
|---|---|---|
| Occlusion from | the depth buffer, marched from the pixel toward the sun's screen position | a real shadow ray into the scene from a point in the air |
| Blind to | anything off screen, and geometry nearer than the sampled surface | nothing |
| Weakness | shafts are approximate; an off-screen occluder casts none | **noise** — three rays per pixel is a three-sample estimate |
| Cost | ~48 depth fetches at quarter the pixels | three full scene traversals per pixel at a quarter of them |

So with the volumetric on, both are kept and the cheap one is turned *down* rather than off
(`GODRAY_SS_MIX_WITH_VOLUMETRIC`): each covers the other's blind spot, and since both estimate the
same inscatter, two full-strength copies would simply double the sunward haze.

**What makes three rays per pixel usable** is that the noise can be filtered along one axis without
touching the detail. Every point on a view ray that ends at the sun projects onto the *same line* in
screen space — the line from the pixel to the sun's screen position — so a shaft is nearly constant
along that line and changes sharply only across it. `volblur.frag` averages a long way along it
(17 taps) and barely at all perpendicular (3), so the noise integrates away while the shaft edges,
the only high-frequency detail in the signal, stay exactly where the geometry casting them is. An
isotropic blur wide enough to remove the same noise would destroy them — and would turn the traced
pass back into the screen-space one, only slower.

**Why it cannot throw fireflies.** The obvious estimator — sum `density x transmittance x visibility`
and multiply by the segment length — is unbiased and throws fireflies badly. The density exponent is
clamped at `+6`, permitting 403x the reference density, and that multiplies a distance reaching
`VOL_MAX_DISTANCE`: one sample can return **733**, where the true value of the integral it estimates
is bounded by `1 - exp(-tau)` and cannot exceed 1. Three samples per pixel and a few pixels catching
that spike is a handful of fireflies ruining the frame.

Clamping the result would work and would be a lie — it would darken the honest bright samples by the
same rule. Instead the integral is split into a magnitude and a fraction:

> inscatter = (how much light the air scatters along this ray) x (what fraction of it is lit)

The first factor has a **closed form**, `1 - exp(-tau)` — the same `fogInscatterFraction` the fog and
the screen-space shafts already use, so no sampling and no variance. The second is a weighted average
of a *binary* visibility, so it lives in `[0,1]` by construction whatever the density does. The
density spike now appears in both the numerator and the denominator of that fraction and cancels. A
firefly is not clamped away; it is arithmetically impossible.

**It is deliberately not physical.** The estimator is normalised, so a gain of 1.0 adds exactly the
inscatter the fog was already adding unshadowed — which is correct and nearly invisible. Two things
then had to be pushed. The phase lobe is the fog's `0.62` in the fog and `VOL_PHASE_G 0.35` here,
because a Mie lobe at 0.62 is 34× dimmer at ninety degrees off the sun than at it, and *a shaft is
looked at from the side*; a real atmosphere scatters by more than one mechanism and is nowhere near
that forward-peaked anyway. And the gain runs at 16, far above the integral, because this is a scene
made of boxes rather than a photograph and the shafts are the most expensive thing in the renderer —
they are worth seeing.

The perpendicular direction is also **bilateral on depth**: a shaft's brightness depends on how much
lit air is in front of the surface, so it changes abruptly at a silhouette, and averaging across one
would drag distant air's glow onto a near object's edge. Note the normalisation differs from the
bloom's on purpose — a tap rejected here means "this belongs to a different surface and I know
nothing about it", so it is dropped from the average, where a tap rejected by the bloom's occlusion
test means "no light comes from this direction", which is information and is counted as darkness.

### Seeing the resolution under motion

Every additive term `display.frag` reads is computed at a fraction of the frame — god rays and the
traced volumetric at a half, the bloom at a quarter and an eighth. On a still image that is invisible:
the quantisation is constant, so it reads as a slightly soft glow. Under camera motion it stops being
invisible, and the reason is worth stating exactly, because it is not simply "the resolution is low".

**The low-resolution grid is anchored to the screen.** The world slides across it while it stays put,
so every feature has to cross the same fixed lattice of texel boundaries — and a *bilinear* magnify
has a derivative discontinuity at each of those boundaries. A smooth ramp reconstructed bilinearly is
a chain of straight segments meeting at kinks, and the kinks sit at fixed screen positions. Still,
they are a static faceting nobody notices. Moving, the image crawls over them, and what the eye reads
is the same parts of the frame changing, locked to the screen rather than to the scene.

`display.frag` therefore magnifies with a **cubic B-spline** rather than bilinearly. It is C2
continuous everywhere, so there is no crease anywhere in the magnified image for motion to reveal —
the result swims smoothly instead of stepping. B-spline rather than Catmull-Rom deliberately:
Catmull-Rom interpolates and sharpens, which on a signal with no detail at this scale means ringing
around bright shafts, where B-spline approximates and smooths slightly, which is the right behaviour
when the source genuinely has no more information in it. It costs four bilinear taps per buffer
instead of one — the classic factorisation that lets the sampler do the inner interpolation.

That removes the creases; it does not add resolution. If the grid is still visible, the target's
`scale` in `resources.json` is the honest next lever, and `volblur.frag`'s kernel is expressed as a
fraction of the frame height (`VOL_BLUR_REACH`) precisely so that raising it does not silently narrow
the filter and change the look being tuned.

### Why the cascade gathers in world space

A screen-space gather ray cannot see off-screen geometry, so it cannot distinguish "blocked" from "sees sky" and defaults to sky — which paints a flat sky over everything. A world-space ray either hits a voxel (correctly occluded, and picking up that voxel's bounced sunlight and its own emission) or escapes the scene to the true sky. Sky occlusion and sky shadows come out right, at the cost of running the voxel DDA once per cascade texel.

The probes themselves are still anchored to the screen G-buffer. Only the rays are world-space.

**The gather is one traversal, and it took two wrong arrangements to get there.** Both were
structurally expensive rather than badly tuned, and the history is worth keeping because the second
one looked like the answer.

First, it called the plain engine march and stepped past each envelope voxel by re-issuing the query
with a larger `tMin`. That was forced: the prototype dilated the swept volume into the **geometry
tree** and spent a palette entry marking it, so every blade sat inside a sealed opaque box and a
march that treated it as solid returned pitch black. But every re-issue restarts the *whole* scene
query — loose-chunk broadphase, grid slab tests, grid DDA, root-to-leaf descent — and `tMin` advances
past exactly one cell per restart, while the shell around a blade is one to two voxels thick and a ray
through a field crosses many blades. The skip budget was spent within a few voxels of the origin, at
up to seven full scene queries per ray and eight rays per probe texel. And then it *failed*:
exhausting the loop falls through to the sky return, so a ray that could not punch through the grass
reported the **brightest** answer available after paying seven traversals for it.

Second, it called the prototype's forked envelope march, which stepped through scaffolding inside its
own DDA — correct, one traversal, and a second full copy of the scene query living in this example.

**Neither is needed.** The envelope is an adjacent quarter-resolution tree now, so the geometry tree
holds the rest pose and nothing else: a query that does not set `PJV_Q_ANIMATION` sees ordinary
geometry, in one traversal, with no scaffolding to see through and no fork to maintain. The gather and
its shadow ray both leave animation off deliberately — whether a blade is at its rest position or a
voxel from it is not a distinction indirect light can carry, and the resolve is the expensive half.

The bug that arrangement had is worth recording too, because it was silent and it was cheap to be
wrong in: while the envelope lived inside the geometry, the gather's shadow ray was blind to the fact
that scaffolding was not geometry, so **every bounce point in or near grass reported "shadowed"** and
contributed no bounced sunlight at all — a whole field's worth of indirect light missing, while the
ray terminated on the first shell voxel it met, so nothing in the frame time pointed at it. An
adjacent envelope cannot cause that.

### Why four cascades

Cascade *c* covers the interval `[start_c, start_c + len_c]` with `len_c = L0 * 4^c`, and holds `4^c` times as many directions over a quarter as many probes. Near light gets fine angular resolution where the geometry is close and the parallax is large; far light gets coarse angular resolution where it does not matter. Four levels reach roughly 85 × `L0`, which covers a room-scale scene. The knobs are at the top of `sharedShaders/pjv_cascade_common.sc`.

## Files

```
main.cpp                                  window, camera, scene load, per-frame uniforms
advancedRenderer/render.json              the fourteen passes and how they are wired
advancedRenderer/resources.json           uniforms, shader binaries, render targets
advancedRenderer/cascadeShaders/*.frag    the fourteen passes
sharedShaders/pjv_sun_sky.sc              the light rig (sun colour, sky gradient, sun-disk sampling)
sharedShaders/pjv_cascade_common.sc       cascade parameterization, octahedral directions, projection
sharedShaders/pjv_face_key.sc             what "the same voxel face" means, in one place
sharedShaders/pjv_probe.sc                the world-space probe gather
sharedShaders/pjv_atmosphere.sc           the fog's optical depth, shared with the god rays
sharedShaders/pjv_anim_controls.sc        two uniforms — what is left of the animation prototypes
```

The voxel traversal itself is not here — it is the engine's, at `include/pjv_utils_DDA.sc`, which every renderer in the tree includes. That is what keeps them all in step with a traversal change.

**And neither is the animation, any more.** This example was where the envelope animation system was
prototyped, through five versions, and what stood here was 2,720 lines of shader across
`pjv_wave.sc`, `pjv_gather.sc`, `pjv_envelope.sc` and `pjv_fire.sc` — each carrying its **own forked
copy of the scene traversal**, because hooking a leaf was the only way to do it before the engine had
a `RayQuery` to ask with — plus about 600 lines of C++ to dilate a swept volume into the geometry tree
and spend a palette entry per component marking it.

All of it is deleted. The system is in the engine: an adjacent quarter-resolution envelope tree, a
motion table, and one `raySceneIntersect`. What is left here is `pjv_anim_controls.sc` — two uniform
declarations and seven accessors — because the field's own parameters stopped being shader parameters
at all. They are rows of `AnimationState`, which the engine uploads and the traversal reads.

The fork was not free while it lasted, and it announced that on the way out: adding the advection
resolve to the engine pushed `gbuffer.frag` past the **SPIR-V id ceiling**, because `spirv-opt` inlines
every function into `main` and four forked traversals are four full copies of the march. `shaderc`
reported `ID overflow. Try running compact-ids`, wrote a **zero-length `.bin`**, and the renderer
loaded it, logged one error, and rendered a frame that measured ten times too fast and looked
plausible. The forked traversal was consuming the budget the feature that replaced it needed.

## How to build

```bash
cmake --preset dev
cmake --build --preset dev --target advanced_renderer
```

Shaders are compiled as part of the build. The binary and its staged renderer folder land in `build/examples/advanced_renderer/`; run it from there.

## How to use

```bash
./advanced_renderer                                    # opens ScenePreviewer/scenes/Untitled 3
./advanced_renderer "../10-scene-previewer/scenes/Untitled"
./advanced_renderer /path/to/any/compose/scene/
```

| Input | Action |
|-------|--------|
| `W` / `S` | Move forward / backward |
| `A` / `D` | Strafe left / right |
| `R` / `F` | Move up / down |
| `Shift` | Move 5× faster (hold) |
| Mouse | Look around (cursor is captured; `Esc` releases it, left-click re-captures) |
| Scroll wheel | Raise / lower the sun — a full day-night cycle, sunset colours included |
| `,` | **The temporal filter (TAA) itself**, on or off. Distinct from `E`, which only zeroes the sub-pixel jitter — `taa.frag` never reads that uniform, so with the jitter off the history is still reprojected, resampled and blended. Two questions, two keys |
| `.` | **Traced volumetric god rays**, added on top of the screen-space ones. Off by default, and the only effect here that is off for *cost* rather than taste — it casts real shadow rays per pixel. See [Two kinds of god ray](#two-kinds-of-god-ray) |
| `U` | Lighting granularity: **per face** (default) or **per voxel**, where all six faces of a voxel share one value for *every* term — indirect, shadow, translucency and cosine. See [Per-face or per-voxel](#per-face-or-per-voxel). Resets the GI history on the way through, because the two modes accumulate different quantities |
| `B` | Foliage translucency on/off — the backlit glow through grass, leaves and petals |
| `V` | Atmospheric fog: off / normal / heavy (3×). Three states rather than a toggle because off answers "is the fog responsible for this" and heavy answers "is it doing what I think it is" on a scene where the tuned value is too subtle to judge |
| `Z` / `X` / `C` | Render scale: full resolution / 0.75 / 0.5. Live, because the only way to judge an upscaler is to flip between it and what it approximates on the same view |
| `E` | Sub-pixel jitter: off / half / full. **This is not a TAA switch** — see `,` for that. **Off by default** — `display.frag` antialiases analytically from the voxel faces now, so the jitter is no longer the only source of AA. With it off the G-buffer is deterministic: two frames of a static scene are bit-identical, so nothing can shimmer |

Two environment variables exist for reproducible captures and measurements, for the same reason the
scene editor has `EDITOR_START_MODE` — a screenshot cannot fly, and two builds are only comparable if
both measured the same view. `ADVANCED_CAMERA="x,y,z,yaw,pitch"` overrides the automatic framing, and
`ADVANCED_LOCK_CAMERA=1` ignores input entirely (the cursor is captured, so a stray pointer nudge
otherwise turns the camera mid-run).

`ADVANCED_FOG=<multiplier>` scales the fog density at launch, `0` disabling it outright — the `V` key
without a keyboard, and the same reasoning.

`ADVANCED_NO_ANIMATION=1` starts with the animated traversal suspended, for the same reason: `G`
toggles it from a keyboard and a scripted A/B run does not have one. This is what `WAVE_MODE=0` used
to be for and is a strictly better version of it — the materials stay flagged and the envelope stays
baked and uploaded, so the **only** difference between the two runs is the traversal. (`WAVE_MODE`
itself is gone. It selected between the sway prototypes, all of which are deleted; passing it now
prints a rejection rather than quietly measuring something else.)

Transparency, refraction and fire are authored the same way, because this example has no palette UI:

| Variable | Effect |
|---|---|
| `ADVANCED_TRANSPARENT="<prefix>=<alpha>[,...]"` | Make named palette entries transparent. 0 opaque, 1 a pure filter, in between the peel's stochastic interface. |
| `ADVANCED_IOR="<prefix>=<ior>[,...]"` | 1.0 no refraction, 1.33 water, 1.5 glass, 2.42 diamond. |
| `ADVANCED_DENSITY="<prefix>=<0..1>[,...]"` | `transmission` — how *strongly* the medium absorbs, as opposed to what colour it absorbs. |
| `ADVANCED_REFRACTION=<0..3>` | How many bends the primary ray may take. **0 by default**, because the branch is not free where it is granted. `J` cycles it. |
| `ADVANCED_FIRE="<prefix>[,...]"` | Set named palette entries alight — they become the *fuel* of an advecting flame, and two entries are appended per component as the flame's colour chain. |
| `ADVANCED_FIRE_EMISSION`, `ADVANCED_FIRE_ALPHA` | How brightly the fuel glows, and how transparent the chain is. Alpha 0 makes the flame a solid emissive surface, which is the check that fire obeys its palette rather than a hardcoded "fire is see-through" rule. |

Each writes a single **byte lane** of the material, never a whole packed word — `packSurfaceWord` or
`packExtraWord` here would silently clear the transparency, the emissive strength or the animation
flags of the entry it was setting.

`ADVANCED_RENDER_SCALE=0.05..1.0` renders the whole pipeline at a fraction of the window on each axis
and lets the display pass upscale. It is the largest single performance dial here, because the G-buffer
pass casts two voxel rays per pixel with LOD off for the whole length of both, so it scales very nearly
with the pixel count — measured at a 1273x1370 window on a grass-filled view, 1.0 is 14.1ms, 0.75 is
10.2ms and 0.60 is 7.9ms.

It **defaults to 0.75** (−25%: 14.03ms → 10.55ms at that window). `ADVANCED_RENDER_SCALE=1.0` restores
full resolution exactly — the specification is left untouched rather than scaled, and the upscale takes
its plain-fetch path. Below 1.0, `display.frag` magnifies by *selection* rather than by filtering: a voxel G-buffer is a partition into piecewise-constant regions, not a band-limited signal,
so blending two faces fabricates a colour that exists nowhere in the scene. Instead it rebuilds the
camera ray for each output pixel and tests it against the candidate faces described by `gFace` /
`gNormal`, taking the nearest one that actually contains it, and falls back to the nearest source texel
when none does. Edges therefore resolve to the output grid from source-resolution shading.

What it cannot do is invent a face nothing sampled, so below about 0.6 thin geometry (grass, canopy)
starts dropping out rather than softening — a blade narrower than a source pixel is never sampled. The
per-frame decisions are hard by design; the sub-pixel jitter and the TAA history integrate them into
correct coverage over time, which is why the temporal fix in `taa.frag` is what makes a reduced scale
look right rather than merely sharp.

One caveat when measuring: the render targets are resized to the live window every frame, so a tiling
window manager that gives this window a size other than the 1920x1080 it asks for silently changes the
pixel count — and the `Target size` lines are printed once at construction and never reprinted. Build
with `cmake --preset dev -DPROJV_LOG_PERF=ON` and the frame-stats line quotes the resolution it
actually rendered at; compare only runs whose resolutions match.

The camera's start position and its flight speed are both derived from the scene's bounding box, because this example takes its scene on the command line and a hardcoded position would be inside one scene's wall and a kilometre from the next one's.

When the camera is still, the accumulate and TAA passes lengthen their running means and the image converges over a second or so. Any movement — including a sun change — switches them to their short reprojecting behaviour.

### Testing transparency, refraction and fire

The default scene (`SmallerVox`) already has a `water.main` material and a `lamp`, and the `Cave`
scene has grass and a canopy. Nothing in either is transparent as authored — this example has no
palette UI, so the environment variables above are how you set one. Three worked examples:

**Water, with refraction.** The default scene, no path argument needed:

```bash
ADVANCED_TRANSPARENT="water.main=0.85" ADVANCED_IOR="water.main=1.33" \
ADVANCED_DENSITY="water.main=0.25" ADVANCED_REFRACTION=2 ./advanced_renderer
```

Then press `J` in the window to cycle the bend budget 0 → 1 → 2 → 3 while looking at the same surface.
That is the A/B worth doing first: at 0 the water is a flat coloured filter, and each bend adds the
displacement of what is behind it. `ADVANCED_DENSITY` is the other half — it deepens the tint with
distance travelled through the body without darkening it, so raise it and the shallows stay clear
while the deep parts saturate.

**Glass, to see total internal reflection.** Any solid body works; the cave's rock is a convenient one
because it is thick:

```bash
ADVANCED_TRANSPARENT="rock=0.7" ADVANCED_IOR="rock=1.5" ADVANCED_REFRACTION=3 \
  ./advanced_renderer ../10-scene-previewer/scenes/Cave/
```

Look along a grazing angle: past the critical angle the refracted ray turns into a reflected one,
which is what makes a thick edge go mirror-like rather than transparent.

**Fire.** Nominate a material as fuel and it becomes the base of an advecting flame:

```bash
ADVANCED_FIRE="daisy.eye" ./advanced_renderer ../10-scene-previewer/scenes/Cave/
```

`F1`–`F12` and `PgUp`/`PgDn` shape it live. The check worth doing is `ADVANCED_FIRE_ALPHA`: at `1` the
flame is layered and see-through, at `0` it is a solid emissive surface that hides what is behind it,
and at `0.5` it is neither — three renders of one scene with one number changed, and nothing in the
traversal special-cases fire to make that happen. It is the palette's `transparency`, the same field
glass and leaves use.

**A caveat on these scenes.** Both carry `fire.envelope`, `fire.origin`, `fire.middle` and `fire.top`
palette entries left over from the prototype's bake, which was run against them at some point. They no
longer mean anything — `fire.envelope` in particular was scaffolding, and with the flag bit that made
it invisible now deleted it is an ordinary opaque white material. If you see white blobs in the
foliage, that is what they are, and
`ADVANCED_TRANSPARENT="fire.envelope=1.0"` will confirm it by making them vanish. The permanent fix is
to re-voxelize those assets, or to delete the slots in SceneEditor.

### A note on the scene library

This example ships no scene; it opens one from `../10-scene-previewer/scenes/`, which the previewer and the scene editor share. Several of those are older than the current `.data` container version and **load as zero chunks**, which the log says plainly:

```
loadComposeFromDisk: Loaded 0 chunk(s) ... from compose scene
readDataFile: ... is .data version 1, and only version 2 is supported. Re-voxelize this asset.
```

At the time of writing `Untitled`, `Untitled 2`, `Untitled 3`, `LittleBall`, `StonehillCastle` and `Bistro` load; `Sibenik` and `LostEmpire` do not. Re-voxelize a stale one with [MeshVoxelizer](../20-mesh-voxelizer/) to bring it back — that is how `Bistro` was brought back, and [ScenePreviewer's README](../10-scene-previewer/README.md#bundled-scenes) carries the command for each bundled scene.

## What changed from the renderer gallery

PathTracer's copy of this renderer does not run on the current engine. Four things had to change, and two more were worth changing while the file was open.

**Had to change:**

1. **Render-target sizing.** `resources.json` declared every texture `"resizable": true` with a hardcoded `resX`/`resY`. That flag is gone and now fails the load with an explicit migration message. Each target declares `"sizeMode": "relative"` with a `"scale"` instead, and is sized `ceil(scale × renderResolution)`. All sixteen here are `1.0`.
2. **`windowRes` / `texelSize` → `passTargetRes`.** Those were renderer-wide app uniforms, correct only while every pass shared one resolution. Since each pass now rasterizes at the size of *its own* target, the engine publishes `passTargetRes` = `(w, h, 1/w, 1/h)` per pass. A shader gets it by declaring it; it is not in `resources.json`. Everything measured in pixels — probe spacing, the direction-atlas layout, filter taps, sub-pixel jitter — reads it.
3. **Material lookup.** `fetchVoxelColor(hit.foundBox, hit.headerIndex)` rebuilds an integer voxel coordinate out of a world-space box. That float32 round trip loses low bits in proportion to the chunk's distance from the origin and shades a growing fraction of voxels with their *neighbour's* colour — measured at 6% for a small translation and 39% for a translation plus a rotation. `fetchVoxelMaterialFromHit(hit)` uses the material index the march already carried out: two texelFetches, no re-descent, and correct under any transform.
4. **The scene.** `SponzaScene/` is a version-1 `.data` container with a compose file that predates versioning, so it no longer loads at all. This example takes its scene as an argument instead of embedding one.

**Worth changing:**

5. **The expanded material system.** A voxel is no longer just a colour. `metallic` is subtracted out of the diffuse albedo, and `emission` is added in *two* places — to `gDirect`, so an emitter is bright on screen, and to the cascade gather, so it is an actual **GI light source**: the gather ray that lands on it carries its glow back to the probe, the merge spreads it up the cascade, and every surface that can see it gets brighter.
6. **The light rig is the scene editor's, term for term.** `pjv_sun_sky.sc` now takes the sun's intensity and the sky's from `renderParams`, and the sun's angular radius from `sunDir.w`, at the editor's default values. The primary ray's sun shadow samples that disk rather than aiming at a point, so shadow edges are as wide as the sun is and TAA resolves the penumbra.

   It has since gained two deliberate departures — `SUN_WARMTH` / `SKY_WARMTH` and the sun's aureole — so a scene lit here reads *warmer* than the editor's Render tab rather than identically. Both are single named constants at the top of that file, and zeroing them out (`vec3(1.0)`, `SUN_GLOW_STRENGTH 0.0`) makes the two agree again exactly, which is what keeps the comparison possible.

**Transparency, since carried over** — and it was the note here that said it had not been, on the grounds that peeling the cascade gather is a real piece of work every one of its rays would pay for. That reasoning held and the conclusion was still avoidable: the **primary ray and the sun shadow ray** peel, and the **gather does not**. A transparent pane is seen through where the camera looks at it and treated as opaque by the bounce that lands on it, which is wrong in a way nobody can point at and right in the way that matters. Costs measured on a canopy interior: 10.2 → 11.9 ms with a transparent material present, 13.3 ms with a whole canopy at 50%.

One limitation is worth stating rather than discovering: **shadows have the correct depth but not the correct colour.** `raySceneTransmittance` hands back a `vec3`, which is what puts coloured light on the floor under stained glass — and the G-buffer's sun-visibility channel is a scalar, so it is reduced to luminance on the way in. Fixing it needs a channel the G-buffer does not have.

## ProjectV features used

| Feature | Usage |
|---------|-------|
| **Core ECS** | `Application`, `world`, global resources, `Startup`/`Update`/`Render` stages |
| **Core Math** | `vec2`/`vec3`/`vec4`, `min`/`max`/`length` — camera, framing, uniform packing |
| **Logging** | `info`/`warn`/`error` — the scene report, and how a stale scene announces itself |
| **Compose I/O** | `loadComposeFromDisk` — a Compose folder flattened into a `Scene` |
| **GPU Interface** | `createTexturesForScene`, `GPUData` — the voxel scene as GPU textures |
| **Manage Resources** | `constructRendererSpecification`, `resizeRenderTargets` (via `renderConstructedRenderer`) |
| **Disk I/O** | `loadRendererSpecification`, `loadShader` |
| **Render Instance** | Window creation, renderer registry, active renderer |
| **Perform Renderer** | `renderConstructedRenderer`, `setUniformToValue`, per-pass view rects and `passTargetRes` |
| **Engine shader library** | `pjv_utils_DDA.sc` — `raySceneIntersect`, `rayStartDirection`, `fetchVoxelMaterialFromHit` |

## Tuning

Every knob is a `#define`, and each one names what it costs.

| Where | Knob | Effect |
|-------|------|--------|
| `pjv_probe.sc` | `PROBE_RAYS`, `PROBE_STEPS`, `PROBE_SHADOW_STEPS` | The gather's ray budget. Neither the gather nor its shadow ray asks for animation — a bounce ray does not need to know which voxel a blade occupies this frame, and the resolve is the expensive half. |
| `pjv_cascade_common.sc` | `PROBE_SPACING0`, `DIR_TILE0` | Probe density and angular resolution. **A cascade occupies exactly `(D0/s0)²` of the screen, and the atlases are declared at that size** — at 16/4 that is `scale: 0.25` in resources.json. Change either and the scale must change with it. |
| | `CASCADE_BASE_LEN` | How far cascade 0 reaches, and so how far the whole stack does (`≈ 85 ×`). |
| | `SKY_GI` | Whether the sky lights the scene at all. |
| `gbuffer.frag` | `SHADOW_ORIGIN_BIAS_VOXELS` | Where the shadow ray leaves the surface, in voxel edge lengths. |
| | `PRIMARY_TRANSPARENT_LAYERS`, `SHADOW_TRANSPARENT_LAYERS` | How many transparent layers each ray may cross. **16 and 4** — a shadow ray needs far fewer, because what it is estimating is an attenuation rather than a surface. |
| | `ENVELOPE_SHADOW_RESOLVE_VOXELS` | How far along a *shadow* ray animation is resolved. **24** — measured, not reasoned: 24 voxels saved 0.2 ms and 4 saved 1.0 ms, with the two images indistinguishable apart from the wind phase. The shadow ray leaves the grass within a few voxels of its origin, which is why it buys so little. |
| `compose.frag` | `FLAT_MIN_FACE_PIXELS` | How large a face must be on screen before it gets one flat value instead of the per-pixel filter. |
| | `SPEC_ENABLE`, `SPEC_STRENGTH`, `SPEC_SHININESS` | The sun sheen. On, weak, Fresnel-gated. |
| | `FOG_PHASE_G` | How tightly the fog throws sunlight forward. Higher is a narrower glow around the sun, not a dimmer one. |
| `pjv_sun_sky.sc` | `SUN_WARMTH`, `SKY_WARMTH` | The warm-key/cool-fill split. Both `vec3(1.0)` restores the scene editor's rig exactly. |
| | `SUN_GLOW_STRENGTH`, `SUN_GLOW_G` | The sun's aureole. Strength is measured against the sky it is added to — see the note there before raising it. |
| `pjv_face_key.sc` | `PJV_PER_VOXEL_LIGHTING` | Reads the `U` toggle. Every branch the per-voxel mode has tests this; see [Per-face or per-voxel](#per-face-or-per-voxel). |
| `compose.frag` | `SSS_POWER`, `SSS_STRENGTH`, `SSS_TINT` | The foliage translucency's width, gain and transmitted colour. |
| | `SSS_EDGE_POWER`, `SSS_EDGE_FLOOR`, `SSS_EDGE_SUN_BIAS` | The gradient across a face in per-face mode: how tightly the glow pulls into the edge, how dark the middle gets, and how far the sun skews it. `SSS_EDGE_FLOOR 1.0` gives the flat per-voxel look. |
| | `PER_VOXEL_BRIGHTNESS_MATCH` | Per-voxel only. 1.0 = the brightest face (exposure-neutral, the default), 0.0 = the six-face mean (energy-conserving, much darker). |
| `resolve.frag` | `FIREFLY_MAX` | Luminance cap on the indirect term. Matters more per-face: one bad probe is a whole face. |
| `display.frag` | `EXPOSURE`, `TINT`, `CONTRAST`, `SATURATION`, `VIGNETTE`, `DITHER` | All colour grading. |
| | `BLOOM_STRENGTH` | How much bloom is added back. The first dial to reach for. |
| | `BLOOM_CORE` | Balance of the two scales. 1.0 = core only (tight, hard), 0.0 = tail only (the flat diamond the single-scale version produced). |
| `bloomdown.frag` | `BLOOM_THRESHOLD`, `BLOOM_KNEE` | What counts as bright, in **linear HDR measured against the frame**: lit surface 0.46, sky zenith 1.08, sky horizon 1.32, sky near the sun 2.77, sun disk 6.02. Setting this below the sky is what made the first version a screen-sized wash. |
| `bloomblur.frag` | `BLOOM_SIGMA`, `BLOOM_TAPS` | How wide the tail is, in quarter-resolution texels — one is four output pixels. |
| | `BLOOM_OCCL_FADE` | How firmly geometry between an emitter and a pixel blocks the glow. |
| `volumetric.frag` | `VOL_SAMPLES` | Shadow rays per pixel. **This is the cost of the `.` mode**; everything else added lately is a handful of texture fetches. |
| | `VOL_SHADOW_STEPS`, `VOL_SHADOW_LOD` | The shadow ray's budget. A ray that runs out reports a miss (lit), so a short budget errs bright rather than toward false shadow. |
| `volblur.frag` | `VOL_BLUR_ALONG`, `VOL_BLUR_ACROSS` | The anisotropy — long along the shaft, short across it. Raising `ACROSS` softens shaft edges, which is the whole thing being protected. |
| `display.frag` | `GODRAY_SS_MIX_WITH_VOLUMETRIC` | How much screen-space god ray survives when the traced pass is on. |
| `main.cpp` | `VOLUMETRIC_GAIN` | **The** dial for the traced shafts — everything else about that pass changes its shape, this changes how much of it there is. 1.0 is the physically normalised estimate; the default is well past it, deliberately (see below). |
| `main.cpp` | `SUN_ANGULAR_RADIUS_DEGREES`, `SUN_INTENSITY`, `SKY_INTENSITY` | The light rig, at the scene editor's defaults. |
| | `FOG_DEPTH_AT_RADIUS`, `FOG_HEIGHT_FRACTION`, `FOG_INSCATTER` | The fog, quoted in units of the scene's bounding sphere so one setting suits every scene. |

`resources.json` has one more, and it is the one the sizing rework above exists to make possible: drop every target's `scale` to `0.5` and the whole pipeline traces at a quarter of the pixels, while `display` — which writes the back buffer and so is sized by the window, not by a `scale` — still runs at native resolution and upsamples. The cost is that the upsample is plain bilinear, so the image softens.

Moving the scales **independently** is the interesting version and is not supported by these shaders yet: a low-resolution cascade read against a full-resolution G-buffer needs the G-buffer point-sampled through `passInputRes` and the indirect term brought back up with a joint bilateral upsample, the way the scene editor's `gi.frag` and `shade.frag` do it. Until then, move them together.
