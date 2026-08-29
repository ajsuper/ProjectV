// ProjectV AdvancedRenderer
//
// A real-time world-space probe global illumination renderer, with animation, transparency and
// refraction. It loads a Compose scene
// from disk and renders it with one ten-pass renderer -- there is no menu and no second pipeline,
// which is the difference between this example and PathTracer: that one exists to compare six
// approaches, this one exists to be the good approach and to be readable while it is.
//
// ---------------------------------------------------------------------------------------------
// WHAT THE RENDERER DOES  (advancedRenderer/render.json is the pass order)
// ---------------------------------------------------------------------------------------------
//   1  gbuffer      one primary ray per pixel -> position, normal, albedo, and the CRISP half of
//                   the image (soft-shadowed direct sun + voxel emission), kept out of every
//                   temporal filter downstream. Animation, transparency and refraction all happen
//                   HERE, in one raySceneIntersect with a RayQuery that asks for all three
//   2  godrays      screen-space sun shafts, from the depth buffer (half res)
//   3  volumetric   the traced half of the same effect: real shadow rays through the fog (half res)
//   4  volblur      blur, because both of the above are one sample per pixel at half resolution
//   5  probe        the world-space gather. One real voxel ray per probe texel, probes anchored to
//                   voxel FACE CENTRES -- which is what made the four radiance cascades this
//                   renderer began with redundant: a face's indirect light is one number, so there
//                   is nothing for a finer cascade to resolve
//   6  probefilter  spatial filter across neighbouring probes, gated on inter-probe VISIBILITY so
//                   light does not bleed through a wall
//   7  resolve      probe atlas -> one indirect irradiance value per pixel (cosine-weighted)
//   8  accumulate   temporal mean of that indirect term alone, reprojected while the camera moves
//   9  compose      direct + albedo * indirect, a sun sheen, and last of all the fog
//  10  upscale      reconstruction from the render scale to the output grid
//  11  taa          resolves the primary ray's sub-pixel jitter into anti-aliased edges
//  12  bloomdown    }  the bloom chain, at a quarter and an eighth
//  13  bloomblur    }
//  14  display      adds the shaft and bloom terms, then exposure, ACES, gamma -> the window
//
// The reason the indirect term is separated out and filtered on its own is that lighting is low
// frequency and albedo is not. Blurring a lit image blurs the voxel colours with it; blurring the
// light and multiplying a sharp albedo back in afterwards does not.
//
// ---------------------------------------------------------------------------------------------
// Controls:
//   W/S       -- move forward / backward
//   A/D       -- strafe left / right
//   R/F       -- move up / down
//   Shift     -- move faster (hold)
//   Mouse     -- look around (cursor is captured; Esc releases it, left-click re-captures)
//   Scroll    -- raise / lower the sun (a full day-night cycle)
//
// Animation. Every one of these is a held-key drag rather than a step, and the current values are
// logged as they change. What they edit is the ENGINE's motion table -- there is no shader in this
// example for the wind, the flame, the envelope or the resolve:
//   G         -- suspend the animated TRAVERSAL. Materials stay flagged, the envelope stays baked and
//                uploaded; the query simply stops asking for animation, so geometry draws at rest.
//                The A/B that separates "the animated path is wrong" from "everything under it is".
//   H         -- resolve animation on the sun's SHADOW rays too, on / off. Off is not "no shadow": a
//                blade still occludes, it is cast from the rest pose.
//   I         -- clean view: flat ambient instead of the 1-spp GI (on by default). The sun stays soft.
//   J         -- refraction: cycle 0..3 bends for the primary ray. Zero by default, because the bend
//                is not free where it is granted. Needs a material with an IOR -- see ADVANCED_IOR.
//   O         -- debug: draw every ENVELOPE CELL as solid instead of resolving it. The first thing to
//                reach for when animated geometry is missing, because it splits the failure in half:
//                blobs mean the bake, the upload and the envelope march are all sound and the fault
//                is in the resolve; nothing at all means the geometry never had anywhere to be drawn.
//                The two look identical on screen and are nowhere near each other.
//   /         -- debug: WHY does this pixel look wrong? Off / 1 / 2. A miss carries no reason, so a
//                hole in the canopy and empty air are the same return value. Read off
//                SceneHit::stopReason: mode 1 paints MAGENTA where a march ran out of steps, mode 2
//                ORANGE where the transparency peel ran out of passes and BLUE where its resume floor
//                stalled. Both mark surfaces as well as sky, which the prototype's versions could not
//                -- a ray that gave up can carry on and hit something, and that pixel is a hit with
//                the wrong geometry in it rather than a miss.
//   1 / 2     -- WIND: how far a blade leans downwind at full gust, in voxels. RE-BAKES on release --
//                this is one of only two controls that changes where geometry can be drawn.
//   3 / 4     -- WIND: gust size -- how many voxels across one patch of moving grass is
//   5 / 6     -- WIND: speed the weather drifts downwind (negative reverses it)
//   7 / 8     -- WIND: turbulence, 0 = big soft gusts, 1 = edges torn up at the scale of a tuft
//   Left/Right-- WIND: rotate the direction, as a compass dial
//   N / M     -- resolve distance in voxels: past it, animated geometry is drawn at rest. It stops
//                moving, it does not stop existing. THE performance control, and close to free
//                visually -- a blade is subpixel well before it stops costing.
//   F1/F2     -- FLAME: travel -- how far a parcel rises from its fuel. RE-BAKES on release (it is
//                the envelope cone's height as well as the trace's step count).
//   F3/F4     -- FLAME: turbulence, the swirl. With none a flame is a column.
//   F5/F6     -- FLAME: field scale in voxels    F7/F8 -- how fast the flow scrolls upward
//   F9/F10    -- FLAME: emission scale. A fire palette entry can carry strength 245 against a sun
//                intensity of 8, so this is a VIEWING control, not a correction to the material.
//   F11/F12   -- FLAME: dissolve -- burns parcels out with age. THIS is what leaves gaps behind, and
//                it is sampled at the parcel's SOURCE so whole tongues detach together.
//   PgUp/PgDn -- FLAME: how fast turbulence grows with age (ragged tip, laminar base)
//
// The keys 9/0 and K/L are deliberately unbound. They were the per-class height profile and the snap
// quantum, and neither exists: the engine's field is coherent (every voxel of a blade moves together,
// which is what stops a blade separating into segments) and it quantises to the lattice
// unconditionally (the drawn position must be a function of the VOXEL alone, or two rays crossing one
// voxel disagree about where it is and it is drawn sliced in half). A key that has MOVED is worse than
// one that is gone.
//
// Render and measurement:
//   Z/X/C/`   -- render scale: full / 0.75 / 0.5 / 0.25. Live, so an upscaler can be judged against
//                what it approximates on the same view without relaunching. The window title reports
//                which is in force, and what it costs.
//   Q         -- upscale reconstruction on/off, at the SAME render scale. Off shows the RAW render
//                resolution, point sampled -- no reconstruction, and taa passes through as well, so
//                there is no filter of any kind left between the samples and the screen. The A/B that
//                isolates the magnify: everything upstream is untouched, so the picture differs only
//                by what upscale.frag did (the millisecond delta also loses taa; see upscaleBypass).
//   E         -- sub-pixel jitter amplitude: off / half / full. OFF by default -- display.frag
//                antialiases analytically now, and with this off the G-buffer is deterministic,
//                so a still frame is bit-identical to the one before it and nothing can shimmer.
//   Y         -- per-pass GPU timing, logged as the mean of 120 frames. Answers "which pass is the
//                frame" directly, rather than by switching something off and watching the total --
//                an inference that only works when the thing switched off was the bottleneck.
//   T         -- sun SHADOW RAYS on / off. A measurement tool, not a look: gbuffer.frag casts two
//                voxel rays per pixel and the render scale moves both, so sweep Z/X/C/` once with
//                this on and once with it off, and the gap between the two curves is the shadow
//                ray's share of the win. The title says SHADOW RAYS OFF while it is off.
//
// Atmosphere. Two effects, two dials -- they used to share one, so asking for heavier haze also
// brightened every shaft and the shafts had no runtime control at all:
//   V         -- GENERAL VOLUMETRIC: distance fog off / normal / heavy (3x). The haze itself, its
//                inscatter, and the sky's aureole.
//   ; / '     -- the same value as a held-key drag, for a setting between those presets
//   [ / ]     -- GOD RAY strength: the sun shafts, screen-space and traced together. Independent of
//                the fog's dial, but NOT of the fog itself -- a shaft is lit air, so it still fades
//                out with the density in front of the pixel and vanishes at fog OFF.
//   .         -- traced volumetric god rays on/off (real shadow rays; the expensive path)
//
// Usage:
//   ./advanced_renderer [scene directory]
// Any folder holding a compose.json works; the camera frames whatever it finds. See
// DEFAULT_SCENE_PATH below for what it opens with none given.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>

#include "core/ecs.h"
#include "core/math.h"
#include "core/log.h"
#include "graphics/render_instance.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/type_mapping.h"
#include "utils/animation.h"
#include "utils/compose_io.h"
#include "utils/material.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

// This example ships no scene of its own; it opens one of the scene library the previewer and the
// editor share. Nothing here is specific to this example -- any folder with a compose.json in it can
// be passed on the command line instead.
//
// Several scenes in that library are older than the current .data container version (2) and load as
// zero chunks, saying so in the log. This one is current. See the README.
// Sibling path in the BUILD tree: examples land side by side under build/examples/, and the scene
// previewer stages the shared scene library beside its own binary. This said "../ScenePreviewer/..."
// until the examples were renumbered, at which point it silently loaded nothing -- loadComposeFromDisk
// reports a missing folder and carries on with an empty scene, so the window opened on a blank frame
// with no obvious cause.
static const char* DEFAULT_SCENE_PATH = "../scene_previewer/scenes/SmallerVox/";

// The renderer's own files. Both paths inside resources.json are relative to the working directory,
// so this example is run from its own folder.
static const char* RENDERER_DIRECTORY = "./advancedRenderer/";
static const char* VERTEX_SHADER_PATH = "./advancedRenderer/cascadeShaders/vs_quad.bin";

// Must match FOV in the shaders (pjv_cascade_common.sc and gbuffer.frag). The framing below places
// the camera by it, and the CPU has no other way to learn it -- the projection lives in the shader.
static const float CAMERA_VERTICAL_FOV_DEGREES = 60.0f;

// ---------------------------------------------------------------------------------------------
// The light rig, as the shaders receive it
// ---------------------------------------------------------------------------------------------
// These are the scene editor's Render-tab defaults, value for value, because sharedShaders/
// pjv_sun_sky.sc is its light rig curve for curve. A scene set up in the editor and opened here
// should be lit the same way; if these drift, that stops being true.

// The angular RADIUS of the sun disk, in degrees -- the softness of every shadow in the image, and
// the single control with the most say over how a frame reads. 0.27 is the real sun (crisp shadow
// edges), a few degrees is a hazy day, seventeen is overcast.
static const float SUN_ANGULAR_RADIUS_DEGREES = 3.0f;
static const float SUN_INTENSITY = 8.0f;
static const float SKY_INTENSITY = 1.0f;

// Mouse scroll drives the sun's elevation (see render()). GLFW scroll callbacks are plain C
// function pointers, so the accumulated wheel offset lives at file scope.
static double g_sunScrollAccum = 0.0;
static void sunScrollCallback(GLFWwindow* /*window*/, double /*xoffset*/, double yoffset) {
    g_sunScrollAccum += yoffset;
}

// Raised by the U key and consumed one frame later in render(), where it joins sunMoved. At file
// scope beside the scroll accumulator because it is the same kind of thing -- a one-shot event that
// has to survive from where it is noticed to where the frame's uniforms are assembled -- and putting
// it on FrameState would mean a member that is always false except for one frame, which reads as
// state when it is an edge.
static bool g_lightingModeChanged = false;

// =============================================================================
// WHICH MATERIALS MOVE
// =============================================================================
// The one piece of animation authoring this example still does in code, and the only reason it does
// is that it has no palette UI. An engine scene records this in the material's flags byte, which
// compose.json already round-trips and SceneEditor's Palette panel is where it is meant to be set.
//
// So read the table below as standing in for authored data, not as a mechanism. Everything else --
// the field, the envelope, the resolve, the traversal -- is the engine's.

// The motion sets this example configures. Three, and note that the first two differ by PARAMETERS
// while the third differs by KIND -- which is the test the motion table had to pass to be an engine
// feature rather than a menu: a rising flame is a table row, not a code path.
static const uint32_t ENGINE_SET_GROUND  = 1;
static const uint32_t ENGINE_SET_FOLIAGE = 2;
static const uint32_t ENGINE_SET_FIRE    = 3;

// A palette entry that moves, and which motion set moves it.
//
// `set` used to be `cls`, a 0..3 "how far does this lean" index, and it is worth recording what
// happened to it because the change is not a rename. The prototype gave each class its own amplitude
// and applied a quadratic profile along a blade in the shader, using the fact that `grass_tufts.lua`
// grows a blade as grass.blade.1..4 from root to tip -- so the palette WAS the height profile, exact
// per blade regardless of the terrain under it. Clever, and abandoned: the engine's field is coherent,
// meaning every voxel of one blade moves together, and that is a correctness requirement rather than a
// simplification. Neighbouring voxels taking different displacements under a quantiser is a blade
// separating into floating segments, which does not read as motion blur -- it reads as a bug.
//
// What survived is the distinction the class index had degenerated into anyway: WHICH FIELD, not how
// far. Ground cover and canopy want different amplitudes and different resolve distances, because a
// leaf high in a tree is subpixel long before a blade at your feet is.
struct WaveMaterialRule {
    const char* name;    // Exact palette entry name as grass_tufts.lua interns it.
    uint32_t    set;     // Motion set index -- ENGINE_SET_GROUND or ENGINE_SET_FOLIAGE.
};

// Matched by exact name. A material not named here is left alone and does not move.
static const WaveMaterialRule WAVE_MATERIAL_RULES[] = {
    // Ground cover: blades, stems and flower heads, all on one field.
    //
    // A flower's whole stalk is ONE material (`growStalk` writes it with `ramp` false), where a blade
    // has a real four-step ramp -- which mattered when the class index was an amplitude and does not
    // now, because the field is coherent and the whole plant moves together either way. So the
    // asymmetry that used to be worth watching for is simply gone.
    {"grass.blade.1",    ENGINE_SET_GROUND},
    {"grass.blade.2",    ENGINE_SET_GROUND},
    {"grass.blade.3",    ENGINE_SET_GROUND},
    {"grass.blade.4",    ENGINE_SET_GROUND},
    {"grass.stem",       ENGINE_SET_GROUND},
    {"daisy.petal",      ENGINE_SET_GROUND},
    {"daisy.eye",        ENGINE_SET_GROUND},
    {"poppy.petal",      ENGINE_SET_GROUND},
    {"poppy.eye",        ENGINE_SET_GROUND},
    {"buttercup",        ENGINE_SET_GROUND},
    {"bluebell",         ENGINE_SET_GROUND},
    {"dandelion.puff",   ENGINE_SET_GROUND},
    {"dandelion.head",   ENGINE_SET_GROUND},

    // Canopy: a leaf rustles rather than leaning, and it does it faster. A separate set rather than a
    // separate amplitude on the same one, which is the difference between a table row and a code path.
    {"tree.leaf.1",      ENGINE_SET_FOLIAGE},
    {"tree.leaf.2",      ENGINE_SET_FOLIAGE},
    {"tree.leaf.3",      ENGINE_SET_FOLIAGE},
    {"tree.leaf.4",      ENGINE_SET_FOLIAGE},
    {"tree.needle.1",    ENGINE_SET_FOLIAGE},
    {"tree.needle.2",    ENGINE_SET_FOLIAGE},
    {"tree.needle.3",    ENGINE_SET_FOLIAGE},
    {"tree.blossom",     ENGINE_SET_FOLIAGE},

    // tree.bark.* and tree.dead.* are deliberately absent: a trunk does not rustle, and bending a
    // branch is coherent whole-body motion -- a rigid transform of a subtree, which is a different
    // mechanism from a displacement field and not one this system claims to do.
    //
    // clover.leaf is deliberately absent: it is a flat rosette lying ON the ground with no stalk
    // (grass_tufts.lua says so outright), so it is the one plant that should not move at all.
    // grass.lush.* / grass.dry.* are likewise absent -- those are the ground tint that grass_tint
    // paints onto the dirt, not anything standing up out of it.
};

// The live controls, and note what KIND of thing is left in here.
//
// Nothing below is a shader parameter. The wind and the flame are described to the engine as
// `MotionSet` rows (see configureEngineMotion), which the traversal reads from a uniform table; a key
// press edits the row and re-uploads it. So a control here is a number the CPU owns, and the shader
// has no opinion about the motion at all.
//
// What that removed, and it is most of the struct's old contents: `profileExponent` (v1's per-class
// height ramp -- the engine's field is coherent and every voxel of a blade moves together, which is
// what stops a blade separating into segments), `quantum` (the engine quantises to the lattice, full
// stop, because the drawn position must be a function of the VOXEL alone or a voxel is drawn in two
// places at once), and seven fire knobs that are now `MotionSet` fields.
struct AnimControls {
    // Does animation resolve at all? The `O` key. See animationSuspended in pjv_anim_controls.sc for
    // why this is a switch rather than a rebuild -- it separates two layers that fail identically.
    bool  animationSuspended = false;
    // Do SHADOW rays resolve animation, or cast from the rest pose? The `H` key. Off is not "no
    // shadow": a blade still occludes, it is just not asked which voxel it occupies this frame.
    bool  waveShadows = true;

    // ---- THE WIND, IN FOUR NUMBERS -------------------------------------------------------------
    // There were six, and two of them (per-clump jitter, cross-wave mix) existed only to disguise the
    // plane wave the field used to be built on. An advected noise has no wave to disguise, and what is
    // left is the set of things somebody actually wants to say about wind: how hard, how big the
    // gusts, how fast, how ragged.
    float amplitude = 1.0f;    // Voxels a blade leans, downwind, at full gust.
    float gustSize = 40.0f;    // Voxels across one gust patch.
    float speed = 1.6f;        // How fast the weather drifts downwind.
    float turbulence = 0.4f;   // 0 = big soft gusts, 1 = edges torn up at tuft scale.
    float windX = 1.0f, windZ = 0.35f;

    // ---- THE FLAME ------------------------------------------------------------------------------
    // Only ever reached by a material ADVANCED_FIRE has flagged, so these cost a table row and
    // nothing else when nothing is alight. Each maps to a MotionSet field of the same name.
    float fireTravel = 10.0f;      // Voxels a parcel rises from its fuel. Also the trace's step count.
    float fireTurbulence = 0.30f;  // The swirl. A flame with none is a column.
    float fireTurbGrowth = 1.5f;   // How much the swirl grows with age: laminar base, ragged tip.
    float fireDissolve = 0.85f;    // How readily a risen parcel burns out. The gaps are what read as fire.
    float fireScale = 9.0f;        // Voxels across one eddy of the flow field.
    float fireSpeed = 6.0f;        // How fast the flow scrolls along the rise.
    // Viewing scale for accumulated flame emission. A fire palette entry can carry an emissive
    // strength of 245 against a sun intensity of 8, so unscaled it saturates to white and hides the
    // colour chain entirely. A VIEWING control, not a correction to the material.
    float fireEmissionScale = 0.006f;

    // ---- THE SCENE, AND THE VIEW ----------------------------------------------------------------
    float voxelSize = 1.0f;    // World units per voxel, measured from the scene at load.
    // Voxels past which animation is no longer RESOLVED -- geometry is drawn at rest beyond it. The
    // dominant cost control, and close to free visually: a blade is subpixel well before it stops
    // costing. 400 was too expensive once foliage joined, because a canopy makes nearly every cell a
    // ray crosses a target.
    float resolveVoxels = 300.0f;

    // Diagnostic lighting: a flat ambient instead of the GI.
    //
    // NO LONGER READ. compose.frag and probe.frag branch on animParams.z, which is now driven by
    // FrameState::globalIllumination and the G key instead -- this field being the real switch while
    // a separate "GI on/off" control sat downstream of it was why that control appeared to do
    // nothing. Kept as a field only so the struct's layout is not disturbed mid-review; the value
    // here is dead.
    bool  cleanView = true;
    // Draw every envelope cell as SOLID instead of resolving it, so the baked envelope is directly
    // visible. The `O`-shift view, and the first diagnostic to reach for: blobs mean the bake, the
    // upload and the envelope march are all sound and the fault is in the resolve; nothing at all
    // means the geometry never had anywhere to be drawn. The two look identical on screen.
    bool  debugEnvelope = false;
    // Why is a pixel sky? A miss carries no reason, so a hole in the canopy and empty air are the same
    // return value. Read off SceneHit::stopReason -- see the debug views in gbuffer.frag.
    //   0  off
    //   1  MAGENTA where the march ran out of steps
    //   2  ORANGE where the peel ran out of passes / BLUE where the resume floor stalled
    int   debugWhy = 0;
    // How many times the primary ray may be bent by refraction. Zero by default; the segment branch
    // is not free where it is granted. ADVANCED_REFRACTION sets it at launch.
    int   refractionSegments = 0;

    bool  dirty = true;        // Log the values next frame (so a key press shows what it did).
};

// =============================================================================
// ANIMATION
// =============================================================================
// The whole of this example's side of it: name which materials move, describe the field as motion-
// table rows, bake, and set the clock each frame. There is no shader here for the field, the
// envelope, the resolve or the flame -- all four are in include/pjv_utils_DDA.sc, reached through a
// RayQuery flag.
//
// That is worth stating as a size rather than a principle. What used to be under this banner was
// 2,720 lines of prototype shader across four files, each carrying its own forked copy of the scene
// traversal, plus about 600 lines of C++ to bake a swept volume into the geometry tree and spend a
// palette entry per component marking it. What replaced it is below.
//
// Which materials move is still decided by the name table above, because THIS example has no palette
// UI. That is the one piece still standing in for authored data -- an engine scene records it in the
static void configureEngineMotion(projv::AnimationState& anim, const AnimControls& wave) {
    float windLength = std::sqrt(wave.windX * wave.windX + wave.windZ * wave.windZ);
    if (windLength < 1e-6f) windLength = 1.0f;
    projv::core::vec3 dir(wave.windX / windLength, 0.0f, wave.windZ / windLength);

    projv::MotionSet ground;
    ground.kind = projv::MotionKind::Sway;
    ground.amplitude = wave.amplitude;
    // The field must be smooth over TENS of voxels, not per voxel. gustSize is that scale in voxels,
    // so the frequency is its reciprocal -- neighbouring voxels of one blade have to move together or
    // the blade separates into floating segments.
    ground.frequency = 1.0f / std::max(wave.gustSize, 1.0f);
    ground.speed = wave.speed * std::max(wave.gustSize, 1.0f);
    ground.direction = dir;
    ground.turbulence = wave.turbulence;

    projv::MotionSet foliage = ground;
    // A leaf rustles rather than leaning, and it does it faster.
    foliage.amplitude = wave.amplitude * 0.75f;
    foliage.frequency = ground.frequency * 1.6f;

    anim.sets[ENGINE_SET_GROUND] = ground;
    anim.sets[ENGINE_SET_FOLIAGE] = foliage;

    // ---- The advecting set -------------------------------------------------------------------
    //
    // Only ever reached by a material ADVANCED_FIRE flagged, so configuring it unconditionally costs
    // a table row and nothing else -- a set nothing points at is never evaluated.
    projv::MotionSet fire;
    fire.kind = projv::MotionKind::Advect;
    fire.direction = projv::core::vec3(0.0f, 1.0f, 0.0f);   // rise
    // Straight off the live controls, so the F-keys edit the flame while looking at it. They used to
    // write `fireParams` / `fireShape`, two uniforms a prototype shader read; they write a table row
    // now and the engine's traversal reads it. Same keys, and the difference is the whole point.
    //
    // `travel` is how far a parcel rises from its fuel, and therefore how tall the flame is. It is
    // also the trace's step count AND the envelope cone's height, which makes it the one flame
    // parameter that costs something to raise and the one that needs a re-bake -- see the F1/F2
    // handling in updateAnimControls.
    fire.travel = wave.fireTravel;
    // The swirl. Fire wants far more of it than grass does -- a flame with none is a column.
    fire.turbulence = wave.fireTurbulence;
    fire.turbulenceGrowth = wave.fireTurbGrowth;
    fire.dissolve = wave.fireDissolve;
    // The flow field's scale, in voxels, on the same reciprocal convention as the wind above.
    fire.frequency = 1.0f / std::max(wave.fireScale, 1.0f);
    fire.speed = wave.fireSpeed;
    // Unused by advection -- the reach is `travel` -- but left non-zero so a set that is misread as
    // Sway somewhere does not silently become a no-op.
    fire.amplitude = 1.0f;

    anim.sets[ENGINE_SET_FIRE] = fire;
}

// A float knob read from the environment. The flame's SHAPE is on the F-keys; these are the two
// authoring values a key cannot reasonably carry -- how bright the fuel glows and how transparent the
// flame's colour chain is -- and they have to be reachable from a scripted run for the same reason
// ADVANCED_CAMERA does.
static float envFloat(const char* name, float fallback) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    return static_cast<float>(std::atof(raw));
}

// ADVANCED_FIRE="<name-prefix>[,<name-prefix>...]" turns named palette entries into the FUEL of an
// advecting flame, and builds the material chain the flame colours itself with.
//
// This example has no palette UI and no scene with fire authored into it, so the fuel has to be
// nominated at load the way ADVANCED_TRANSPARENT nominates glass. What it demonstrates is the thing
// worth demonstrating: the same traversal, the same envelope and the same flags byte, with one table
// row of a different KIND.
//
// ---- THE CHAIN, AND WHY IT IS BUILT HERE ---------------------------------------------------------
//
// A flame is not one colour. What makes it read as fire is that its colour is a function of how far a
// parcel has risen -- blue-white at the fuel, yellow through the body, orange at the tip -- and the
// advection resolve gets that for free, because the number of backward steps it took IS the age. What
// it needs from the data is somewhere to go with that number, which is the `nextMaterial` link in the
// reserved byte of each palette entry's extra word.
//
// So two entries are appended per component and linked fuel -> body -> tip. Written to the BYTE LANE
// in place: packExtraWord would rebuild the whole word and clear the animation flags this function
// just set, on the very entry whose chain it is trying to write.
//
// Transparency rises along the chain deliberately. Fire occludes exactly as much as its palette says
// -- there is no "fire is see-through" flag anywhere in the engine, which was the prototype's bug --
// so a dense blue base and a wispy tip is authored here rather than special-cased in the traversal.
static int applyFireOverrides(projv::Scene& scene) {
    const char* spec = std::getenv("ADVANCED_FIRE");
    if (!spec) return 0;

    std::vector<std::string> prefixes;
    {
        std::string rest(spec);
        while (!rest.empty()) {
            size_t comma = rest.find(',');
            std::string entry = rest.substr(0, comma);
            rest = (comma == std::string::npos) ? std::string() : rest.substr(comma + 1);
            if (!entry.empty()) prefixes.push_back(entry);
        }
    }
    if (prefixes.empty()) return 0;

    const float baseStrength = envFloat("ADVANCED_FIRE_EMISSION", 6.0f);
    int flagged = 0;

    for (projv::ComponentRecord& component : scene.components) {
        std::vector<size_t> fuel;
        for (size_t slot = 0; slot < component.materialPalette.size(); slot++) {
            const std::string& name = component.materialPalette[slot].name;
            for (const std::string& prefix : prefixes) {
                if (name.rfind(prefix, 0) != 0) continue;
                fuel.push_back(slot);
                break;
            }
        }
        if (fuel.empty()) continue;

        // ---- The two entries the chain walks into ---------------------------------------------
        //
        // Shared by every fuel slot in this component, because the chain is a colour ramp for the
        // flame and not a property of what is burning. Two entries per component, so the 255-slot cap
        // is only a problem for a palette that was already at it -- and if it is, the flame still
        // works, it is just one colour. Reported rather than silently degraded.
        uint32_t bodySlot = 0, tipSlot = 0;
        if (component.materialPalette.size() + 2 <= 255) {
            auto makeLayer = [&](const char* suffix, float r, float g, float b,
                                 float transparency, float strength) {
                projv::Material m;
                m.name = std::string("fire.") + suffix;
                m.packedColor = projv::packRGB10(r, g, b);
                m.packedEmission = projv::packRGB10(r, g, b);
                m.packedSurface = projv::packSurfaceWord(0.0f, 0.0f, transparency, 1.0f);
                m.packedExtra = projv::packExtraWord(strength, 0.0f, 0u);
                component.materialPalette.push_back(m);
                return static_cast<uint32_t>(component.materialPalette.size() - 1);
            };
            // ADVANCED_FIRE_ALPHA scales the chain's transparency, which is the flame's quality knob
            // AND its cost knob: a transparent layer is a peel pass, and a peel pass is a whole scene
            // re-traversal. At 0 the flame is a solid emissive surface, which is the check that fire
            // obeys its palette rather than a hardcoded "fire is see-through" rule.
            const float alpha = envFloat("ADVANCED_FIRE_ALPHA", 1.0f);
            bodySlot = makeLayer("body", 1.0f, 0.62f, 0.12f, 0.62f * alpha, baseStrength * 0.8f);
            tipSlot  = makeLayer("tip",  1.0f, 0.24f, 0.04f, 0.82f * alpha, baseStrength * 0.45f);
        } else {
            projv::core::warn("ADVANCED_FIRE: component '{}' has no room for the flame's colour chain "
                              "({} slots used). The flame will render in the fuel's own colour.",
                              component.name, component.materialPalette.size());
        }

        for (size_t slot : fuel) {
            projv::Material& m = component.materialPalette[slot];
            // The fuel is what the trace looks for, so it must carry the flag AND the set. Everything
            // else about it -- colour, transparency -- stays as authored, which is what makes
            // "set fire to the leaves" a one-line change rather than a repaint.
            projv::setMaterialAnimation(m, true, ENGINE_SET_FIRE);
            // A dim glow at the base, so the fuel itself reads as burning rather than as lit.
            if (projv::materialEmissiveStrength(m) <= 0.0f) {
                m.packedExtra = (m.packedExtra & 0x00FFFFFFu) |
                                (projv::packEmissiveStrength(baseStrength) << 24);
            }
            if (bodySlot != 0) {
                m.packedExtra = (m.packedExtra & ~0xFFu) | (bodySlot & 0xFFu);
            }
            flagged++;
        }
        if (bodySlot != 0) {
            // body -> tip, and the tip's own link stays zero: that is what ends the walk.
            projv::Material& body = component.materialPalette[bodySlot];
            body.packedExtra = (body.packedExtra & ~0xFFu) | (tipSlot & 0xFFu);
        }
    }

    if (flagged == 0) {
        projv::core::warn("ADVANCED_FIRE: no palette entry matched '{}' -- nothing will burn.", spec);
    } else {
        projv::core::info("ADVANCED_FIRE: {} palette entr(ies) set alight under motion set {}.",
                          flagged, ENGINE_SET_FIRE);
    }
    return flagged;
}

// Flags the palette using the ENGINE's bits, then bakes the envelope. Replaces both
// flagWavingMaterials and bakeWaveEnvelope in mode 6 -- and note what it does not need: no palette
// entry per component for scaffolding, so the 255-slot cap cannot silently delete a scene's foliage
// the way it could when the envelope lived in the geometry's own palette.
static void setupEngineAnimation(projv::Scene& scene, projv::AnimationState& anim,
                                 const AnimControls& wave) {
    configureEngineMotion(anim, wave);

    int flagged = 0;
    for (projv::ComponentRecord& component : scene.components) {
        for (projv::Material& material : component.materialPalette) {
            for (const WaveMaterialRule& rule : WAVE_MATERIAL_RULES) {
                if (material.name != rule.name) continue;
                // The table names the set outright. This used to sniff a "tree." prefix off the
                // material's own name to pick between the two, which worked and was a second place
                // the grouping was decided -- so a rule added for a plant that is not a tree and not
                // grass would have been silently grouped with the grass.
                projv::setMaterialAnimation(material, true, rule.set);
                flagged++;
                break;
            }
        }
    }
    // Fire is nominated separately, and it counts towards "does anything in this scene move" -- a
    // scene with no grass but a lit torch must still be baked.
    flagged += applyFireOverrides(scene);

    if (flagged == 0) {
        projv::core::warn("Engine animation: no palette entry matched a rule -- nothing in this scene "
                          "will move. This scene has no grass_tufts materials in it.");
        return;
    }
    projv::core::info("Engine animation: flagged {} palette entr(ies).", flagged);

    projv::utils::EnvelopeBakeReport report = projv::utils::bakeAnimationEnvelope(scene, anim);
    // The envelope is ADJACENT: the geometry tree is untouched, so these numbers are additions
    // alongside it rather than growth of it. Worth printing as a ratio, because that ratio is what
    // the quarter-resolution adjacent tree buys -- around 0.1 cells per source voxel on a canopy,
    // against the dilated-into-the-geometry version which grew each blob's tree by a multiple.
    projv::core::info("Engine animation: {} blob(s) enveloped, {} cell(s) from {} animated voxel(s) "
                      "({:.2f} cells per source voxel).",
                      report.blobsBaked, report.envelopeCells, report.sourceVoxels,
                      report.sourceVoxels ? double(report.envelopeCells) / double(report.sourceVoxels) : 0.0);

}

// ADVANCED_TRANSPARENT="<name-prefix>=<alpha>[,<name-prefix>=<alpha>...]" makes named palette
// entries transparent at load, for the same reason ADVANCED_CAMERA and WAVE_MODE exist: the feature
// has to be observable from a scripted run, and this example has no palette UI to author it with
// (that is SceneEditor's job -- see the Palette panel).
//
// Matches by PREFIX, because scene palettes name things like "tree.leaf.02" and the useful selection
// is almost always a family rather than one slot. Alpha is the engine's `transparency`: 0 opaque,
// 1 a pure filter, in between the peel's stochastic interface.
//
// Writes the LANE, not the whole word -- packSurfaceWord here would silently zero the material's
// glossiness, metallic and IOR along with it.
// ADVANCED_IOR="<name-prefix>=<ior>[,...]" and ADVANCED_DENSITY="<name-prefix>=<0..1>[,...]".
//
// The other two halves of a transparent material, and they exist for the same reason
// ADVANCED_TRANSPARENT does: this example has no palette UI, and a feature that cannot be reached
// from a scripted run cannot be checked from one. SceneEditor's Palette panel is where these are
// authored for real -- it has had IOR and Transparency sliders since before either was rendered.
//
// `ior` is what bends the ray: 1.0 is no refraction (and is what raw 0 in the byte decodes to, so a
// palette written before this existed cannot bend anything), 1.33 water, 1.5 glass, 2.42 diamond.
// `density` is `transmission`, the achromatic extinction multiplier -- how strongly the medium
// absorbs, as opposed to what colour it absorbs, which is the albedo's job. 0 is what every palette
// on disk carries and reproduces the previous absorption exactly.
//
// Both write their own BYTE LANE. packSurfaceWord / packExtraWord here would rebuild the whole word
// and silently clear the transparency, the emissive strength or the animation flags along with it.
static void applyMaterialByteOverride(projv::Scene& scene, const char* envName,
                                      bool surfaceWord, int shift,
                                      float lo, float hi, bool isIOR) {
    const char* spec = std::getenv(envName);
    if (!spec) return;

    int applied = 0;
    std::string rest(spec);
    while (!rest.empty()) {
        size_t comma = rest.find(',');
        std::string entry = rest.substr(0, comma);
        rest = (comma == std::string::npos) ? std::string() : rest.substr(comma + 1);

        size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            projv::core::warn("{}: '{}' is not <name-prefix>=<value> -- skipping it.", envName, entry);
            continue;
        }
        std::string prefix = entry.substr(0, eq);
        float value = std::max(lo, std::min(hi, std::strtof(entry.c_str() + eq + 1, nullptr)));
        uint32_t byteValue = isIOR ? projv::packIOR(value) : projv::packByteField(value);

        for (projv::ComponentRecord& component : scene.components) {
            for (projv::Material& material : component.materialPalette) {
                if (material.name.rfind(prefix, 0) != 0) continue;
                uint32_t& word = surfaceWord ? material.packedSurface : material.packedExtra;
                word = (word & ~(0xFFu << shift)) | ((byteValue & 0xFFu) << shift);
                applied++;
            }
        }
    }
    if (applied == 0) {
        projv::core::warn("{}: no palette entry matched -- nothing changed.", envName);
    } else {
        projv::core::info("{}: {} palette entr(ies) updated.", envName, applied);
    }
}

static void applyTransparencyOverrides(projv::Scene& scene) {
    const char* spec = std::getenv("ADVANCED_TRANSPARENT");
    if (!spec) return;

    int applied = 0;
    std::string rest(spec);
    while (!rest.empty()) {
        size_t comma = rest.find(',');
        std::string entry = rest.substr(0, comma);
        rest = (comma == std::string::npos) ? std::string() : rest.substr(comma + 1);

        size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            projv::core::warn("ADVANCED_TRANSPARENT: '{}' is not <name-prefix>=<alpha> -- skipping it.",
                              entry);
            continue;
        }
        std::string prefix = entry.substr(0, eq);
        float alpha = std::strtof(entry.c_str() + eq + 1, nullptr);
        alpha = std::max(0.0f, std::min(1.0f, alpha));

        for (projv::ComponentRecord& component : scene.components) {
            for (projv::Material& material : component.materialPalette) {
                if (material.name.rfind(prefix, 0) != 0) continue;
                uint32_t byteValue = uint32_t(std::lround(alpha * 255.0f)) & 0xFFu;
                material.packedSurface = (material.packedSurface & ~(0xFFu << 8)) | (byteValue << 8);
                applied++;
            }
        }
    }
    if (applied == 0) {
        projv::core::warn("ADVANCED_TRANSPARENT: no palette entry matched -- nothing is transparent. "
                          "Palette names are logged by the scene report.");
    } else {
        projv::core::info("ADVANCED_TRANSPARENT: {} palette entr(ies) made transparent.", applied);
    }
}

// World units per voxel, from the first live chunk. Amplitude and gust size are quoted in VOXELS
// throughout, so the shader needs this to turn them into world distances -- and the snap quantum
// needs it to land the offset on the lattice rather than merely on a round number.
static float detectVoxelSize(const projv::Scene& scene) {
    for (const projv::Chunk& chunk : scene.chunks) {
        if (!chunk.alive || chunk.header.scale <= 0.0f || chunk.header.resolution == 0) continue;
        return chunk.header.scale / float(chunk.header.resolution);
    }
    return 1.0f;
}


// =============================================================================
// Camera framing
// =============================================================================

// Where the camera starts and how fast it flies, both derived from the scene rather than hardcoded.
// This example takes its scene on the command line, so a fixed position would put the camera inside
// one scene's wall and a kilometre from the next one.
struct CameraFraming {
    projv::core::vec3 position;
    float yaw;
    float pitch;
    float moveSpeed;   // World units per frame.

    // The scene's own scale, carried back out of the framing because the framing is the one place that
    // already measures it. The distance fog is derived from these two -- see FOG_* below -- for the
    // same reason the flight speed is: a density quoted in world units is haze in a 32-voxel model and
    // an opaque wall in SanMiguel at 32768.
    float sceneRadius = 1.0f;   // Half the diagonal of the bounding box.
    float sceneFloorY = 0.0f;   // The bottom of it, which is where the fog is thickest.
};

// ---- Distance fog, in units of the scene ------------------------------------------------------
// FOG_DEPTH_AT_RADIUS is the one number that means anything here: the fraction of the light from a
// surface one bounding-sphere RADIUS away that is lost to haze on the way to the eye, at the scene's
// floor. 0.35 is a clear day with visible aerial perspective -- the far side of the scene reads as
// further away, and nothing is hidden.
static const float FOG_DEPTH_AT_RADIUS = 0.35f;
// How far up the haze reaches, as a fraction of the radius. Below this the air is thick, above it the
// fog thins out exponentially, so a scene gets a layer sitting in it rather than a flat tint over
// everything -- and flying up out of it is a visible thing that happens.
static const float FOG_HEIGHT_FRACTION = 0.35f;
// How strongly the fog throws sunlight forward, into an eye looking toward the sun through it. This
// is the term that makes haze read as lit air; see FOG_PHASE_G in compose.frag for the lobe's shape.
// At 0.25 it adds about 1.7 to the fog's own ~1.3 when looking straight at the sun and about 0.6 at
// thirty degrees off, so the haze brightens toward the sun by roughly a stop and does not become a
// second sun. 0.55 was the first guess and washed out the whole sunward half of the far field.
static const float FOG_INSCATTER = 0.25f;

// Gain on the TRACED volumetric god rays (`.`).
//
// Was 0.8, on the reasoning that the pass computes a physically normalised estimate of the same
// inscatter the fog adds unshadowed, so 1.0 would roughly double the sunward haze. That reasoning is
// sound and the result was still invisible, because it accounts only for the magnitude AT the sun --
// and a shaft is looked at from the side, where the phase lobe had thrown away another factor of
// thirty. VOL_PHASE_G in volumetric.frag fixes the lobe; this covers the rest of the gap.
//
// Measured against a lit surface of ~0.46, at sixty degrees off the sun and a few hundred units of
// view distance, 6.0 landed the shafts in the same range rather than two orders below it -- correct,
// and still understated for a scene made of boxes. This is not a photorealistic renderer and the
// shafts are the most expensive thing in it, so they are pushed well past the physical estimate to
// be seen: at 16 they run brighter than the surfaces they fall across, which is the look asked for
// rather than the one the integral gives.
//
// THE dial for this effect. Everything else about the traced pass -- sample count, blur width, phase
// lobe -- changes its shape; this changes how much of it there is.
static const float VOLUMETRIC_GAIN = 16.0f;

// The world-space bounding box of every live chunk. A chunk header carries its world position
// (minimum corner) and its scale, which is all a bounding box needs -- no geometry is touched, so
// this stays instant on a large scene.
static bool measureSceneBounds(const projv::Scene& scene, projv::core::vec3& boundsMin,
                               projv::core::vec3& boundsMax) {
    bool found = false;
    for (const projv::Chunk& chunk : scene.chunks) {
        if (!chunk.alive || chunk.header.scale <= 0.0f) continue;

        projv::core::vec3 chunkMin = chunk.header.position;
        projv::core::vec3 chunkMax = chunk.header.position + projv::core::vec3(chunk.header.scale);
        if (!found) {
            boundsMin = chunkMin;
            boundsMax = chunkMax;
            found = true;
        } else {
            boundsMin = projv::core::min(boundsMin, chunkMin);
            boundsMax = projv::core::max(boundsMax, chunkMax);
        }
    }
    return found;
}

// Places the camera outside the scene's bounding sphere, looking at its centre from a raised
// three-quarter angle -- the view that shows the most of an unfamiliar scene. Same construction the
// scene editor frames with, so opening a scene in both puts you in roughly the same place.
static CameraFraming frameScene(const projv::Scene& scene) {
    using namespace projv::core;

    CameraFraming framing;
    framing.yaw = 3.14159265f * 0.25f;   // Looking along +X/+Z, so the camera sits on the -X/-Z side.
    framing.pitch = -0.35f;              // Slightly above, looking down onto the subject.

    vec3 boundsMin, boundsMax;
    if (!measureSceneBounds(scene, boundsMin, boundsMax)) {
        warn("Scene has no live chunks -- there will be nothing but sky to render.");
        framing.position = vec3(0.0f, 0.0f, -100.0f);
        framing.moveSpeed = 1.0f;
        return framing;
    }

    vec3 center = (boundsMin + boundsMax) * 0.5f;
    float radius = length(boundsMax - boundsMin) * 0.5f;
    if (radius <= 0.0f) radius = 1.0f;

    framing.sceneRadius = radius;
    framing.sceneFloorY = boundsMin.y;

    info("Scene bounds: ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f})",
         boundsMin.x, boundsMin.y, boundsMin.z, boundsMax.x, boundsMax.y, boundsMax.z);

    // Far enough back that the bounding sphere fits the shaders' vertical FOV, with a margin so the
    // subject is not jammed against the frame edge.
    const float fovRadians = CAMERA_VERTICAL_FOV_DEGREES * 3.14159265f / 180.0f;
    float distance = (radius / std::tan(fovRadians * 0.5f)) * 1.25f;

    vec3 forward = {
        std::cos(framing.pitch) * std::cos(framing.yaw),
        std::sin(framing.pitch),
        std::cos(framing.pitch) * std::sin(framing.yaw)
    };
    framing.position = center - forward * distance;

    // Crossing the scene should take a few seconds at 60 fps regardless of how big it is. Voxels are
    // one world unit, so a 512-voxel model and a 32-voxel one otherwise fly at wildly different
    // apparent speeds.
    framing.moveSpeed = std::max(radius * 0.005f, 0.01f);

    // ADVANCED_CAMERA="x,y,z,yaw,pitch" pins the camera somewhere specific, overriding the framing
    // above. This exists for the same reason the scene editor's EDITOR_START_MODE does: a
    // screenshot cannot fly, and a frame time is only comparable between two builds if both
    // measured the same view. Paired with ADVANCED_LOCK_CAMERA below.
    if (const char* overrideCamera = std::getenv("ADVANCED_CAMERA")) {
        float x, y, z, yaw, pitch;
        if (std::sscanf(overrideCamera, "%f,%f,%f,%f,%f", &x, &y, &z, &yaw, &pitch) == 5) {
            framing.position = vec3(x, y, z);
            framing.yaw = yaw;
            framing.pitch = pitch;
            info("ADVANCED_CAMERA: ({:.1f}, {:.1f}, {:.1f}) yaw {:.3f} pitch {:.3f}", x, y, z, yaw, pitch);
        } else {
            warn("ADVANCED_CAMERA is not \"x,y,z,yaw,pitch\" -- ignoring it: {}", overrideCamera);
        }
    }
    return framing;
}

// =============================================================================
// Per-frame state
// =============================================================================

// Everything render() carries between frames. A struct rather than function-local statics so that
// the previous camera and the current one cannot get out of step -- the temporal passes reproject
// through the previous camera, and a stale value there is a smear rather than an error.
struct FrameState {
    projv::core::vec3 cameraPosition;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float moveSpeed = 1.0f;

    projv::core::vec3 prevCameraPosition;
    projv::core::vec3 prevCameraDirection = {0.0f, 0.0f, 1.0f};
    bool prevCameraInitialized = false;

    // The frame the camera (or the sun) last moved on. The accumulate and TAA passes switch between
    // their long still-camera mean and their short reprojecting one on this.
    int frameLastMovedOn = 0;

    bool mouseCaptured = true;
    bool mouseInitialized = false;
    double lastMouseX = 0.0;
    double lastMouseY = 0.0;

    double prevSunScroll = 0.0;

    // The animation controls, and a real-time clock rather than a frame count so the motion's speed
    // does not change with the frame rate -- which matters here specifically, because resolving
    // animation costs enough to visibly move the frame rate.
    AnimControls wave;
    // A key that changes the envelope's SHAPE is pending a re-bake. Deferred until the key is released
    // rather than run per frame: these are continuous drags and a bake is seconds. See the handler.
    bool animRebakePending = false;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    // Render scale, live. The DECLARED scale of every relative target, captured from the specification
    // before any scaling is applied, so a new scale can be derived from the originals rather than by
    // repeatedly multiplying the live ones -- see setRenderScale.
    std::unordered_map<unsigned int, float> declaredTextureScales;
    float renderScale = 1.0f;

    // Q: bypass the reconstruction in upscale.frag and point-sample the render-resolution frame onto
    // the output grid instead, WITHOUT changing the render scale. That is the whole point of it --
    // comparing against full resolution compares two different amounts of work, while this holds every
    // pass upstream identical and swaps only how the result reaches the output grid.
    //
    // RAW means raw: taa.frag reads this flag too and passes the frame through, because its running
    // mean and its Catmull-Rom history resample would otherwise put a filtered magnify back after the
    // point sample and there would be nothing to see. So what is on screen with this on is one render
    // sample per output pixel and nothing else. The caveat is in the milliseconds rather than the
    // picture: the delta Q moves is the reconstruction plus taa's accumulation, not the reconstruction
    // on its own.
    bool upscaleBypass = false;

    // Global illumination on/off. The probe chain still runs -- switching it off here changes what
    // compose.frag does with the result, not what is computed -- because the question it answers is
    // what the indirect bounce contributes to the image, not what it costs.
    bool globalIllumination = true;


    // Y: per-pass GPU timing, off until asked for. See the readout in render().
    bool gpuProfile = false;

    // ---- Distance fog ---------------------------------------------------------------------------
    // Measured off the scene at load (see CameraFraming) and then held constant. Two numbers rather
    // than one because the fog is a HEIGHT field: the extinction coefficient says how thick the air is
    // at the floor, the height says how fast it thins going up.
    float fogDensity = 0.0f;
    float fogHeight  = 1.0f;
    float fogFloorY  = 0.0f;

    // V: fog strength, as a multiplier on the density above. Three states rather than a toggle, for
    // the reason the jitter key has three: off answers "is the fog responsible for this", and heavy
    // answers "is it doing what I think it is" on a scene where the tuned value is too subtle to
    // judge. Off is a genuine off -- fogParams.x reaches the shader as 0 and the branch in
    // fogOpticalDepth returns before either exp().
    //
    // Also draggable on ; and ' for values between the presets. The cycle stays because the useful
    // question is usually "off, normal, or obviously too much" rather than a fine setting.
    //
    // It still takes the GOD RAYS with it at ZERO, and that much is deliberate rather than a
    // shortcut: a shaft is inscattered sunlight, so it is a property of the air. Air with no haze in
    // it has nothing to scatter, and shafts hanging in a clear scene would be a decal on the frame
    // rather than something in it. That dependence is physical and per-pixel -- `air` in godrays
    // .frag is computed from this same density -- so it survives the split below.
    float fogScale = 1.0f;

    // [ / ]: GOD-RAY strength, independent of the fog above. Scales both shaft paths together --
    // the screen-space march and the traced volumetric one -- because display.frag combines them as
    // two estimates of ONE quantity, so a separate dial for each would be a dial for "how much do my
    // two approximations disagree" rather than for anything visible.
    //
    // Why this had to be split out: both passes took their overall gain straight off fogParams.w,
    // the FOG's inscatter strength, which V moves. So there was exactly one knob for two effects.
    // Asking for heavier atmosphere also brightened every shaft, and asking for stronger shafts
    // meant editing GODRAY_GAIN (a #define, so a shader rebuild) or VOLUMETRIC_GAIN (a C++ const, so
    // a full rebuild). Neither was reachable while looking at the scene, which is the only place the
    // judgement can be made.
    //
    // What is NOT decoupled, on purpose: shafts still scale with the medium through `air`, a
    // per-pixel measurement of the optical depth actually in front of that pixel. That is the
    // physical relationship. The dial that has been separated is the artistic one.
    float godrayScale = 1.0f;



    // `.`: TRACED volumetric god rays, on top of the screen-space ones. OFF by default, and the only
    // effect here that is off for cost rather than for taste: it casts VOL_SAMPLES real shadow rays
    // per pixel at half resolution, which is the single most expensive thing this renderer can be
    // asked to do. Everything else added recently is a handful of texture fetches.
    //
    // It does not replace the screen-space pass, it joins it -- see GODRAY_SS_MIX_WITH_VOLUMETRIC in
    // display.frag for why both are kept and the cheap one is turned down rather than off.
    bool volumetricGodrays = false;

    // `,`: the TEMPORAL FILTER itself, on or off. ON by default.
    //
    // Distinct from `E`, which zeroes the sub-pixel jitter and nothing else -- taa.frag never reads
    // that uniform. With the jitter off the history is still reprojected, resampled and blended, so a
    // moving camera still shows temporal blur and anything else reprojection can do wrong. Telling
    // those two apart needed two keys, and not having the second one is why "TAA jitter fully
    // disabled" and "still seeing temporal blur" could both be true at once.
    bool temporalFilter = true;


};

// =============================================================================
// Application stages
// =============================================================================

// The scene path, parked here by main() because the ECS stages take only the Application.
struct SceneRequest {
    std::string path = DEFAULT_SCENE_PATH;
};

// =============================================================================
// Render scale
// =============================================================================

// How much of the window the pipeline actually renders, on each axis.
//
// THE ONE LARGE LEVER LEFT ON THIS SCENE, and it is not about the sway at all. The G-buffer pass is
// most of the frame -- 82% of it at full resolution -- and it is the only large thing that answers to
// this dial. Everything below is per-pass GPU time from the Y key rather than whole-frame ms, which is
// what finally made the parts separable.
//
//   scale   pixels   frame   gbuffer      upscale
//   1.00     100%    18.0    14.8 (82%)   2.16 (12%)
//   0.50      25%     5.0     3.5 (70%)   0.65 (13%)
//   0.25    6.25%     3.0     1.5 (50%)   0.42 (14%)
//
// WHAT THE DIAL ACTUALLY BUYS. Near-ideal down to 0.5 and then flat: 1.0 -> 0.5 takes 13ms off the
// frame, 0.5 -> 0.25 takes 2ms more. gbuffer tracks the pixel count exactly over the first step (3.5ms
// measured against 3.69ms predicted from the 1.0 point) and then leaves the curve -- 1.5ms measured
// against 0.92ms predicted. That last ~0.8ms is a floor rather than work: at a quarter scale there are
// too few rays in flight to hide the traversal's memory latency, so the pass stops being able to get
// cheaper. Below 0.5 you are paying image quality for nothing much.
//
// The first step is slightly BETTER than linear, and that is the primary ray's LOD, not noise. It is
// ON (PRIMARY_LOD_ENABLE in gbuffer.frag) and its crossover distance is derived from passTargetRes.y,
// so a lower render scale widens every pixel's footprint and coarsens each ray sooner. The dial buys
// fewer rays AND shorter marches. Only the SHADOW ray is pinned to LOD 0 for its whole length, which
// is a different decision made for a different reason (see the note at its RayQuery).
//
// WHICH RAY IT IS. The camera ray, essentially alone. Turning the sun shadow ray off entirely (T key)
// moves gbuffer by about 0.1ms out of 14.8 -- 0.7% of the pass. That is not a statement that ray
// marching is cheap here; it is that the two rays are nothing like the same length. The camera ray
// runs from the eye until it hits, while the shadow ray leaves the grass within a few voxels of the
// surface it started on and is answered. Soft shadows are free on this scene; the primary trace is not.
//
// STILL OPEN: how much of gbuffer's 14.8ms is the march itself versus the six MRT attachments it
// writes (72 bytes per pixel, three of them RGBA32F). The clean way to split it is a PRIMARY_LOD_BIAS
// sweep at a fixed resolution -- march length moves, the writes do not, and the Y-key gbuffer row
// reports the difference.
//
// It is only worth defaulting to because of what magnifies it. An earlier attempt at this default was
// reverted when the upscale was a bilinear filter, which is the wrong reconstruction for this content
// and was verifiably blurry -- see the note at the top of the upscale in display.frag. That pass now
// SELECTS one source sample per output pixel by testing the ray against the candidate voxel faces, so
// edges resolve to the output grid and nothing is ever blended between two surfaces.
//
// It is still a magnifier, and the honest limit is that it reconstructs edges between faces that WERE
// sampled and cannot invent a blade no source ray hit. What softens that is temporal: the sub-pixel
// jitter moves the sample points every frame and taa.frag integrates the hard per-frame decisions into
// correct coverage -- which only started working once its still/moving branch was fixed, so this
// default and that fix are the same change.
//
// ADVANCED_RENDER_SCALE=1.0 restores full resolution exactly (the specification is left untouched
// rather than scaled, and the upscale takes its plain-fetch path). 0.5 is the far end of the useful
// range -- most of the available win, and past it the curve flattens while thin geometry starts
// dropping out rather than merely coarsening.
static const float DEFAULT_RENDER_SCALE = 0.75f;

// ADVANCED_RENDER_SCALE overrides it. The right value is a judgement about how soft the image is
// allowed to look, which is a decision for whoever is looking at it rather than for this file.
static float readRenderScale() {
    const char* scaleOverride = std::getenv("ADVANCED_RENDER_SCALE");
    if (scaleOverride == nullptr) return DEFAULT_RENDER_SCALE;

    float requested = std::strtof(scaleOverride, nullptr);
    // Clamped rather than trusted: 0 would create 1x1 targets and a blank screen, and above 1 is
    // supersampling this example has no downsample filter for -- the display pass would just point
    // sample it and look worse than 1.0 for several times the cost.
    if (!(requested > 0.05f) || requested > 1.0f) {
        projv::core::warn("ADVANCED_RENDER_SCALE must be in (0.05, 1.0] -- ignoring it: {}",
                          scaleOverride);
        return DEFAULT_RENDER_SCALE;
    }
    return requested;
}

// Scales every RELATIVE render target in the specification.
//
// UNIFORMLY, which is the point: the ratios between targets are load-bearing and survive only if
// everything moves together. The cascades are declared at 0.25 because the probe grid is a quarter of
// the G-buffer on each axis -- scaling only the G-buffer would make them a quarter of the WINDOW
// instead, and the probe math would be reading a grid that is no longer the one it is indexing.
//
// Fixed-size textures are skipped by definition: their size is a property of their contents (uploaded
// images, lookup tables), not of the window, which is exactly what TextureSizeMode::Fixed means.
//
// Nothing downstream needs to know this happened. Passes read their real resolution from
// passTargetRes/passInputRes rather than re-deriving it (resizeRenderTargets rounds up, so
// ceil(w*s) != w*s and re-deriving would disagree), and the display pass samples with a normalised UV
// against bilinear filtering, so it upscales to the window on its own.
// Targets that live at OUTPUT resolution and must not follow the render scale.
//
// Everything up to and including compose renders at the reduced resolution; upscale.frag reconstructs
// to the output grid, and taa accumulates AFTER it. So these three are the output-side of that split,
// and scaling them down would put the magnify back where it started -- reconstructing to a small
// target and then letting the display blit stretch it, which is the arrangement the reorder exists to
// get rid of.
static bool isOutputResolutionTarget(const std::string& textureName) {
    return textureName == "upscaled" || textureName == "taaColor" || textureName == "taaPos";
}

static void applyRenderScale(projv::RendererSpecification& rendererSpec, float renderScale) {
    if (renderScale >= 0.999f) {
        projv::core::info("Render scale: 1.0 -- full resolution, targets untouched.");
        return;
    }

    int scaledTextures = 0;
    for (projv::Texture& texture : rendererSpec.resources.textures) {
        if (texture.sizeMode != projv::TextureSizeMode::Relative) continue;
        if (isOutputResolutionTarget(texture.name)) continue;
        texture.scale *= renderScale;
        scaledTextures++;
    }

    projv::core::info("Render scale: {:.2f} on each axis ({:.0f}% of the pixels), applied to {} "
                      "relative target(s). The display pass upscales. ADVANCED_RENDER_SCALE overrides.",
                      renderScale, renderScale * renderScale * 100.0f, scaledTextures);
}

// Changes the render scale on a renderer that has already been constructed.
//
// Possible at all because a Relative texture's size is not baked in at construction: the driver calls
// resizeRenderTargets every frame (perform_renderer.cpp), and that function derives each target's size
// from ConstructedTextures::relativeTextureScales right then. Writing a new scale into that map is
// therefore all it takes -- the next frame sees a size that differs from the one on record and rebuilds
// the texture and any framebuffer holding it. Nothing here has to touch bgfx.
//
// Scales are recomputed from the DECLARED ones captured at startup, never by multiplying the current
// values by a ratio. The ratio form drifts as it accumulates, and worse, it loses the relationship
// between targets: the cascades are declared at 0.25 because the probe grid is a quarter of the
// G-buffer on each axis, and that has to stay true at every scale.
static void setRenderScale(projv::graphics::RenderInstance& renderInstance, FrameState& state,
                           float renderScale) {
    if (renderScale == state.renderScale) return;

    std::shared_ptr<projv::ConstructedRenderer> activeRenderer = renderInstance.getActiveRenderer();
    if (!activeRenderer) {
        projv::core::warn("Render scale: no active renderer to rescale.");
        return;
    }

    std::unordered_map<uint, float>& liveScales =
        activeRenderer->resources.textures.relativeTextureScales;
    for (std::unordered_map<uint, float>::iterator it = liveScales.begin(); it != liveScales.end();
         ++it) {
        std::unordered_map<uint, float>::const_iterator declared =
            state.declaredTextureScales.find(it->first);
        if (declared == state.declaredTextureScales.end()) continue;
        it->second = declared->second * renderScale;
    }

    state.renderScale = renderScale;
    projv::core::info("Render scale -> {:.2f} ({:.0f}% of the pixels).", renderScale,
                      renderScale * renderScale * 100.0f);
}

// =============================================================================
// Per-pass GPU timing
// =============================================================================

// Names for bgfx's per-view stats, in render.json's `renderer` order.
//
// The engine submits pass i as bgfx VIEW i -- manage_resources.cpp assigns `dependencyGraph.
// renderPassID = uint(i)` -- so this array is positional and has to be kept in step with render.json.
// A name in the wrong slot is worse than no name at all, because it attributes a cost to a pass that
// did not incur it and every conclusion drawn afterwards is wrong. startup() checks the length against
// the graph the renderer actually built and refuses to name anything if they disagree.
// In render.json's order, which is the order the GPU timer reports views in. `godrays` sits second
// because its only input is the G-buffer, so it is placed as early as its dependency allows.
static const char* const PASS_NAMES[] = {
    "gbuffer", "godrays", "volumetric", "volblur", "probe", "probefilter", "resolve", "accumulate",
    "compose", "upscale", "taa", "bloomdown", "bloomblur", "display"
};
static const size_t PASS_NAME_COUNT = sizeof(PASS_NAMES) / sizeof(PASS_NAMES[0]);

void startup(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance =
        projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
    renderInstance.initialize(1920, 1080, "ProjectV Advanced Renderer");

    // Capture the cursor so mouse motion drives the camera (FPS-style mouse look).
    glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetScrollCallback(renderInstance.window, sunScrollCallback);

    // createGlobalResource returns the existing one if main() already made it, so a path given on
    // the command line survives to here and the default fills in when none was.
    SceneRequest& request = projv::core::createGlobalResource<SceneRequest>(app.world);

    projv::Scene& scene = projv::core::createGlobalResource<projv::Scene>(app.world);
    projv::GPUData& gpuData = projv::core::createGlobalResource<projv::GPUData>(app.world);
    FrameState& state = projv::core::createGlobalResource<FrameState>(app.world);

    projv::core::info("Loading scene: {}", request.path);
    scene = projv::utils::loadComposeFromDisk(request.path);

    // A scene that loads no geometry still opens a window and renders a perfectly clean empty frame,
    // which looks like a renderer bug rather than a missing scene. Two causes, both silent: the
    // folder is not there, or its .data containers are a version this loader rejects (several
    // scenes in the shared library are still version 1). Say which.
    if (scene.chunks.empty()) {
        if (!std::filesystem::exists(request.path)) {
            projv::core::error("Scene folder not found: {}\n"
                               "Pass one on the command line, or run this from its own build "
                               "directory so the sibling path resolves.", request.path);
        } else {
            projv::core::error("Scene at {} loaded no chunks -- the image will be empty. If the log "
                               "above reports a rejected .data version, that asset needs "
                               "re-voxelizing.", request.path);
        }
    }
    // Before the palette is uploaded, so an overridden entry is transparent everywhere that reads it
    // rather than only where this example happens to look.
    applyTransparencyOverrides(scene);
    // packedSurface's low byte is the IOR; packedExtra's transmission lane is bits 16-23.
    applyMaterialByteOverride(scene, "ADVANCED_IOR",     true,  0, 1.0f, 2.99f, true);
    applyMaterialByteOverride(scene, "ADVANCED_DENSITY", false, 16, 0.0f, 1.0f, false);

    // Flag which materials move, describe the field as motion-table rows, and bake the envelope.
    // That is the entire CPU side of animation in this example -- there is no second path to choose
    // between any more, so there is no mode to read before doing it.
    setupEngineAnimation(scene, gpuData.animation, state.wave);

    // WAVE_MODE is REJECTED rather than clamped or silently mapped onto the engine path. It named the
    // six sway prototypes, all of which are deleted along with their forked traversals; a script that
    // still asks for one is asking to measure a renderer that no longer exists, and quietly handing it
    // a different one is how a benchmark keeps running while reporting the wrong thing. That is the
    // same rule this code applied to modes 2 and 3 when those two were removed.
    if (const char* modeOverride = std::getenv("WAVE_MODE")) {
        projv::core::warn("WAVE_MODE is gone (you passed {}). It selected between the sway prototypes, "
                          "which have been deleted along with their forked traversals -- the engine's "
                          "animation path is now the only one. `O` suspends it at runtime, and "
                          "ADVANCED_NO_ANIMATION=1 starts it suspended.", modeOverride);
    }
    // The scripted-run equivalent of `G`, for the same reason ADVANCED_CAMERA exists: a comparison is
    // only valid if both runs measured the same thing, and a locked benchmark run has no keyboard.
    // This is what WAVE_MODE=0 used to be for, and it is a strictly better version of it -- the
    // materials stay flagged and the envelope stays baked, so the only difference between the two runs
    // is the traversal.
    if (const char* noAnim = std::getenv("ADVANCED_NO_ANIMATION")) {
        state.wave.animationSuspended = std::atoi(noAnim) != 0;
        state.wave.dirty = true;
    }
    // How many bends the primary ray may take. Zero by default -- the branch is not free where it is
    // granted -- and `J` cycles it at runtime.
    if (const char* refr = std::getenv("ADVANCED_REFRACTION")) {
        state.wave.refractionSegments = std::max(0, std::min(8, std::atoi(refr)));
        state.wave.dirty = true;
    }

    // ADVANCED_CLEAN_VIEW=0 turns the diagnostic flat-ambient substitute off at launch, i.e. renders
    // with the real GI. Same reason as WAVE_MODE: `I` toggles it from a keyboard a scripted run does
    // not have, and the GI is not even evaluated while clean view is on.
    if (const char* cleanViewOverride = std::getenv("ADVANCED_CLEAN_VIEW")) {
        state.wave.cleanView = std::atoi(cleanViewOverride) != 0;
        state.wave.dirty = true;
    }

    state.wave.voxelSize = detectVoxelSize(scene);
    projv::core::info("Wave: voxel size {:.3f} world unit(s) -- amplitude and gust size are quoted "
                      "in voxels and scaled by this.", state.wave.voxelSize);

    CameraFraming framing = frameScene(scene);
    state.cameraPosition = framing.position;
    state.prevCameraPosition = framing.position;
    state.yaw = framing.yaw;
    state.pitch = framing.pitch;
    state.moveSpeed = framing.moveSpeed;

    // Fog, in units of this scene. Inverting Beer-Lambert on the target loss at one radius gives the
    // extinction coefficient: T = exp(-d * r) => d = -ln(T) / r, with T = 1 - FOG_DEPTH_AT_RADIUS.
    // Doing the inversion here rather than picking a coefficient by eye is what makes the constant
    // above mean something a reader can check against the image.
    state.fogHeight  = std::max(framing.sceneRadius * FOG_HEIGHT_FRACTION, 1e-3f);
    state.fogFloorY  = framing.sceneFloorY;
    state.fogDensity = -std::log(std::max(1.0f - FOG_DEPTH_AT_RADIUS, 1e-4f)) /
                       std::max(framing.sceneRadius, 1e-3f);

    // ADVANCED_FOG=<multiplier> for the same reason WAVE_MODE and ADVANCED_CAMERA exist: V cycles it
    // from a keyboard, and a scripted A/B capture does not have one. 0 disables it outright.
    if (const char* fogOverride = std::getenv("ADVANCED_FOG")) {
        float scale = std::strtof(fogOverride, nullptr);
        if (scale >= 0.0f && scale <= 100.0f) {
            state.fogScale = scale;
        } else {
            projv::core::warn("ADVANCED_FOG must be in [0, 100] -- ignoring it: {}", fogOverride);
        }
    }
    projv::core::info("Fog: {:.5f}/unit at y={:.1f}, thinning over {:.1f} units, scale {:.2f}. "
                      "V cycles it; ADVANCED_FOG overrides at launch.",
                      state.fogDensity, state.fogFloorY, state.fogHeight, state.fogScale);

    projv::RendererSpecification rendererSpec =
        projv::graphics::loadRendererSpecification(RENDERER_DIRECTORY);
    // Captured BEFORE the scale is applied: these are what Z/X/C recompute from at runtime.
    for (const projv::Texture& texture : rendererSpec.resources.textures) {
        if (texture.sizeMode != projv::TextureSizeMode::Relative) continue;
        // Output-resolution targets are left out entirely, so setRenderScale never touches them: a
        // texture absent from this map keeps whatever scale it was constructed with.
        if (isOutputResolutionTarget(texture.name)) continue;
        state.declaredTextureScales[texture.textureID] = texture.scale;
    }
    state.renderScale = readRenderScale();
    applyRenderScale(rendererSpec, state.renderScale);
    renderInstance.addRendererSpecification(1, rendererSpec);

    bgfx::ShaderHandle vertexShader = projv::graphics::loadShader(VERTEX_SHADER_PATH);
    std::shared_ptr<projv::ConstructedRenderer> constructedRenderer =
        projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1),
                                                        vertexShader);

    renderInstance.setActiveRenderer(constructedRenderer);
    gpuData = projv::graphics::createTexturesForScene(scene);

    // Label the views so the Y-key readout says "gbuffer" rather than "view 0". Names are set once
    // and persist; the profiler itself stays off until asked for, because collecting per-view GPU
    // timestamps is not free and a measurement tool that is always on changes what it measures.
    const size_t passCount = constructedRenderer->dependencyGraph.size();
    if (passCount != PASS_NAME_COUNT) {
        projv::core::warn("Pass timing: render.json has {} passes but this build knows {} names -- "
                          "leaving views unnamed rather than mislabelling them. Update PASS_NAMES.",
                          passCount, PASS_NAME_COUNT);
    } else {
        for (size_t i = 0; i < passCount; i++) {
            bgfx::setViewName(bgfx::ViewId(constructedRenderer->dependencyGraph[i].renderPassID),
                              PASS_NAMES[i]);
        }
    }

}

// Frame timing profiler. Compiled out entirely unless PROJV_ENABLE_PERF is defined.
void update(projv::Application& app) {
    // ---- The controls, printed once, after everything else has had its say -------------------
    //
    // Not at the end of startup(): the renderer logs its uniform and pass names while submitting the
    // FIRST FRAME, which happens after startup returns, so a list printed there still ends up buried
    // under thirty lines of [RND]. Waiting two frames puts it last, where it will actually be read.
    //
    // Written straight to stdout rather than through the logger so it arrives as one block, without
    // timestamps and level tags breaking up a table.
    {
        static bool controlsPrinted = false;
        if (!controlsPrinted && app.frameCount >= 2) {
            controlsPrinted = true;
        std::printf(
            "\n"
            "  ProjectV Advanced Renderer\n"
            "  ---------------------------------------------------------------------------\n"
            "   Move        W A S D          forward / left / back / right\n"
            "               R F              up / down\n"
            "               Shift            hold to move 5x faster\n"
            "               Mouse            look around  (Esc releases, left-click recaptures)\n"
            "\n"
            "   Sun         Scroll wheel     raise / lower  (a full day-night cycle)\n"
            "\n"
            "   Render      Z X C `          render scale: full / 0.75 / 0.5 / 0.25\n"
            "   scale                        lower is faster; the reconstruction fills it back in\n"
            "\n"
            "   Upscale     Q                reconstruction on / off\n"
            "                                off shows the raw render resolution, point sampled --\n"
            "                                the A/B that isolates what the reconstruction is worth\n"
            "\n"
            "   GI          G                global illumination on / off\n"
            "                                off substitutes a flat ambient, so the image stays\n"
            "                                readable and the bounce's contribution is visible\n"
            "\n"
            "   God rays    [ ]              strength down / up\n"
            "               .                traced volumetric god rays on / off (the expensive path)\n"
            "\n"
            "   Fog         V                off / normal / heavy\n"
            "               ; '              hold to drag it to a value between those presets\n"
            "  ---------------------------------------------------------------------------\n"
            "\n");
        std::fflush(stdout);
        }
    }

#if defined(PROJV_ENABLE_PERF)
    static auto lastFrameTime = std::chrono::high_resolution_clock::now();
    auto currentFrameTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> frameDuration = currentFrameTime - lastFrameTime;
    lastFrameTime = currentFrameTime;

    static int frameCount = 0;
    static double frameTimes[100];
    frameTimes[frameCount % 100] = frameDuration.count() * 1000.0;
    if (++frameCount % 100 == 0) {
        double sum = 0, mn = 1e9, mx = 0;
        for (int i = 0; i < 100; i++) {
            sum += frameTimes[i];
            mn = std::min(mn, frameTimes[i]);
            mx = std::max(mx, frameTimes[i]);
        }
        // The RESOLUTION is part of the measurement, not context around it. resizeRenderTargets()
        // follows the live window every frame (perform_renderer.cpp), so a tiling window manager that
        // lands this window at a size other than the one initialize() asked for silently changes the
        // pixel count -- and two runs then measure different amounts of work at identical settings,
        // with nothing in the log to say so. The "Target size" lines are printed once at construction
        // and never reprinted on a resize, so they cannot be used for this. Quote the size with the
        // time or the time does not mean anything.
        int windowWidth = 0, windowHeight = 0;
        glfwGetWindowSize(
            projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world).window,
            &windowWidth, &windowHeight);
        projv::core::perf("Frame stats (last 100): avg={:.2f}ms min={:.2f}ms max={:.2f}ms at {}x{}",
                          sum / 100.0, mn, mx, windowWidth, windowHeight);
    }
#else
    (void)app;
#endif
}

// Mouse look. The cursor is captured (GLFW_CURSOR_DISABLED), so its absolute position is read each
// frame and the delta applied to yaw/pitch. Returns whether the view turned.
static bool updateMouseLook(projv::graphics::RenderInstance& renderInstance, FrameState& state) {
    // Cursor capture toggle: Esc releases the cursor so it can leave the window, left-click
    // re-captures it. Only look around while captured.
    if (state.mouseCaptured && glfwGetKey(renderInstance.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        state.mouseCaptured = false;
    } else if (!state.mouseCaptured &&
               glfwGetMouseButton(renderInstance.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        glfwSetInputMode(renderInstance.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        state.mouseCaptured = true;
    }

    double mouseX, mouseY;
    glfwGetCursorPos(renderInstance.window, &mouseX, &mouseY);
    // Reset the reference point whenever tracking (re)starts, so re-capturing after a release does
    // not apply one huge jump.
    if (!state.mouseInitialized || !state.mouseCaptured) {
        state.lastMouseX = mouseX;
        state.lastMouseY = mouseY;
        state.mouseInitialized = true;
    }
    double deltaX = mouseX - state.lastMouseX;
    double deltaY = mouseY - state.lastMouseY;
    state.lastMouseX = mouseX;
    state.lastMouseY = mouseY;

    if (!state.mouseCaptured || (deltaX == 0.0 && deltaY == 0.0)) return false;

    const float mouseSensitivity = 0.0025f;
    state.yaw   += float(deltaX) * mouseSensitivity;   // right -> look right
    state.pitch -= float(deltaY) * mouseSensitivity;   // up    -> look up
    // Just shy of straight up/down, to avoid the gimbal flip at the pole.
    const float pitchLimit = 1.55f;   // ~89 degrees
    state.pitch = std::max(-pitchLimit, std::min(pitchLimit, state.pitch));
    return true;
}

// W/A/S/D/R/F flight. Forward comes from yaw alone so W/S flies level regardless of where the
// camera is looking. Returns whether the camera moved.
static bool updateMovement(projv::graphics::RenderInstance& renderInstance, FrameState& state) {
    float speed = state.moveSpeed;
    if (glfwGetKey(renderInstance.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) speed *= 5.0f;

    projv::core::vec3 forward = {std::cos(state.yaw), 0.0f, std::sin(state.yaw)};
    projv::core::vec3 right = {std::cos(state.yaw - 1.5707963f), 0.0f,
                               std::sin(state.yaw - 1.5707963f)};

    bool moved = false;
    if (glfwGetKey(renderInstance.window, GLFW_KEY_W)) { state.cameraPosition += forward * speed; moved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_S)) { state.cameraPosition -= forward * speed; moved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_D)) { state.cameraPosition += right * speed;   moved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_A)) { state.cameraPosition -= right * speed;   moved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_R)) { state.cameraPosition.y += speed;         moved = true; }
    if (glfwGetKey(renderInstance.window, GLFW_KEY_F)) { state.cameraPosition.y -= speed;         moved = true; }
    return moved;
}

// The sun, from the scroll wheel. The angle traces a FULL great circle in a fixed vertical plane --
// rise, overhead, set on the far side, below the horizon (night), back -- and is deliberately
// unbounded so it never stops. sunDir = cos(theta) * azimuth + sin(theta) * up is unit length for a
// unit azimuth, so there is nothing to renormalize.
static projv::core::vec4 sunFromScroll() {
    float angle = 0.86f + float(g_sunScrollAccum) * 0.08f;   // radians; ~4.6 degrees per notch
    const float azimuthX = -0.6402f, azimuthZ = 0.7682f;     // = normalize(vec2(-0.5, 0.6))
    float c = std::cos(angle), s = std::sin(angle);
    // w is the disk's angular radius in radians, which is what makes the shadows soft. Floored well
    // above zero in the shader: a zero-radius sun has no solid angle to sample.
    const float degreesToRadians = 3.14159265f / 180.0f;
    // w is the disk's angular radius. It now sizes ONLY THE SUN DRAWN IN THE SKY -- the shadow ray
    // aims at the centre and ignores it (see sunVisible in gbuffer.frag).
    //
    // They used to be the same number, and that coupling was the problem. Widening the disk is how a
    // penumbra forms physically, but it makes the one shadow ray per pixel stochastic, so the shadow
    // arrives as one-sample noise that is smooth only after many frames have been averaged -- which
    // nothing that moves ever gets. Clean view worked around it by shrinking the disk to a point,
    // which bought a noise-free shadow by giving up both the soft shadow AND the visible sun.
    //
    // Decoupled, both can be had: a deterministic shadow softened spatially in compose.frag
    // (SHADOW_BLUR_SIGMA), and a sun in the sky that is still the size of a sun.
    return {c * azimuthX, s, c * azimuthZ, SUN_ANGULAR_RADIUS_DEGREES * degreesToRadians};
}

// Everything the fourteen passes read. Every uniform declared in resources.json is set here: a uniform
// the renderer declares but never uploads holds whatever bgfx last had in it, which is a black sun
// or a black sky rather than an error.
static void uploadFrameUniforms(const std::shared_ptr<projv::ConstructedRenderer>& renderer,
                                const FrameState& state, projv::core::vec3 cameraDirection,
                                projv::core::vec4 sunDirection, bool cameraMoved, bool sunMoved,
                                int frame) {
    using projv::graphics::setUniformToValue;

    // Copied into locals rather than passed straight out of `state`. setUniformToValue deduces the
    // uniform's type from its argument, and a member of a const struct arrives as `const vec3&`,
    // which the deduction does not recognise -- the value is then silently never uploaded and the
    // shader reads whatever bgfx last had under that name. The engine says as much when it happens,
    // but the fix belongs here.
    projv::core::vec3 cameraPosition = state.cameraPosition;
    projv::core::vec3 prevCameraPosition = state.prevCameraPosition;
    projv::core::vec3 prevCameraDirection = state.prevCameraDirection;

    setUniformToValue(renderer, "cameraPos", cameraPosition);
    setUniformToValue(renderer, "cameraDir", cameraDirection);
    setUniformToValue(renderer, "prevCameraPos", prevCameraPosition);
    setUniformToValue(renderer, "prevCameraDir", prevCameraDirection);

    // x = frame index (drives the per-frame jitter), y = did anything move this frame (which
    // temporal branch to take), z = the frame it last moved on, w = spare.
    // w = the SUN moved this frame, on its own rather than folded into y. y means "something in the
    // scene changed" and the sway raises it every frame; a GI history that resets on y therefore
    // resets forever. See lightMoved in accumulate.frag.
    projv::core::vec4 frameCount = {float(frame), cameraMoved ? 1.0f : 0.0f,
                                    float(state.frameLastMovedOn), sunMoved ? 1.0f : 0.0f};
    setUniformToValue(renderer, "frameCount", frameCount);

    setUniformToValue(renderer, "sunDir", sunDirection);
    // x is the GI debug view selector the shaders still branch on; 0 is the normal renderer and
    // this example no longer offers the views, so it stays 0. w was the sub-pixel jitter amplitude,
    // and 0 means fully deterministic -- display.frag antialiases analytically, so nothing is lost.
    projv::core::vec4 renderParams = {0.0f, SUN_INTENSITY, SKY_INTENSITY, 0.0f};
    setUniformToValue(renderer, "renderParams", renderParams);

    // x = upscale reconstruction bypass (Q). The other three carried debug switches this example no
    // longer exposes -- shadow rays off, foliage translucency off, per-voxel lighting -- and every
    // one of them is 0 for the normal renderer, so the shaders take their usual path unchanged.
    projv::core::vec4 debugParams = {state.upscaleBypass ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    setUniformToValue(renderer, "debugParams", debugParams);

    // x = global illumination on. Its own uniform rather than a fifth meaning for debugParams.
    projv::core::vec4 featureToggles = {state.globalIllumination ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    setUniformToValue(renderer, "featureToggles", featureToggles);

    // x = extinction per world unit at the floor, y = the height it thins over, z = that floor,
    // w = sun inscatter strength. Read by compose.frag through the shared pjv_atmosphere.sc; see the
    // uniform's note there for what each component means to the closed-form integral. The inscatter
    // is scaled alongside the density so that turning the fog down does not leave a sun glow hanging
    // in clear air.
    //
    // The 1.5 ceiling on that scaling is not the 3.0 the heavy setting asks of the DENSITY. Heavy fog
    // should be thicker, which is what the density does; it should not also be a brighter sun,
    // which is what an unclamped inscatter would make it, and that washed out the whole sunward half
    // of the far field when it was tried.
    //
    // .w NO LONGER DRIVES THE GOD RAYS. It used to, which made this one number the strength of the
    // haze, the sky's aureole and both shaft passes at once. The shafts now take volParams.w below.
    projv::core::vec4 fogParams = {state.fogDensity * state.fogScale, state.fogHeight, state.fogFloorY,
                                   FOG_INSCATTER * std::min(state.fogScale, 1.5f)};
    setUniformToValue(renderer, "fogParams", fogParams);

    // x = traced volumetric god rays on, y = their gain, w = god-ray inscatter strength. Read by
    // volumetric.frag, godrays.frag, volblur.frag and display.frag. A uniform of its own rather than
    // another component of debugParams, which is full, and this is a rendering mode rather than a
    // debug switch.
    // z is inverted relative to the state it carries -- taa.frag asks "am I skipping this", which is
    // the question at the branch, and 0 is the normal renderer.
    //
    // .w is seeded from FOG_INSCATTER so that a godrayScale of 1 reproduces the old picture EXACTLY:
    // the shaft passes previously multiplied by fogParams.w, which is this same constant at the
    // default fog. What changed is only that the fog's scale is no longer folded into it.
    projv::core::vec4 volParams = {state.volumetricGodrays ? 1.0f : 0.0f, VOLUMETRIC_GAIN,
                                   state.temporalFilter ? 0.0f : 1.0f,
                                   FOG_INSCATTER * state.godrayScale};
    setUniformToValue(renderer, "volParams", volParams);

    // ---- The animation controls, in two uniforms ------------------------------------------------
    //
    // There were eight, and six of them described the FIELD -- amplitude, gust size, speed,
    // turbulence, wind direction, snap quantum, profile exponent, and seven more for the flame. None
    // of that is a shader parameter now: the field is a row of the engine's motion table, uploaded
    // from `AnimationState`, and a key press edits the row rather than a uniform. What is left is what
    // this EXAMPLE decides rather than what the engine does.
    //
    // The time uniform is gone from here too. `gpuData.animation.time` is engine-owned and uploaded
    // alongside `passTargetRes`, which is what docs/plans/animation.md asked for and what this
    // example's throwaway `waveTime.x` was standing in for.
    const AnimControls& wave = state.wave;

    projv::core::vec4 animParams = {wave.voxelSize, wave.resolveVoxels,
                                    state.globalIllumination ? 0.0f : 1.0f,
                                    wave.waveShadows ? 1.0f : 0.0f};
    setUniformToValue(renderer, "animParams", animParams);

    projv::core::vec4 animDebug = {wave.animationSuspended ? 1.0f : 0.0f,
                                   float(wave.debugWhy),
                                   float(wave.refractionSegments),
                                   0.0f};
    setUniformToValue(renderer, "animDebug", animDebug);
}

void render(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance =
        projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    projv::GPUData& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
    FrameState& state = projv::core::getGlobalResource<FrameState>(app.world);

    // ADVANCED_LOCK_CAMERA=1 ignores input entirely. Needed for any measurement or capture: the
    // cursor is captured, so a pointer nudge from the window manager -- or from the screenshot tool
    // itself -- turns the camera mid-run, and two builds then quietly measure different views.
    static const bool cameraLocked = std::getenv("ADVANCED_LOCK_CAMERA") != nullptr;

    bool cameraMoved = false;
    if (!cameraLocked) {
        cameraMoved = updateMouseLook(renderInstance, state);
        cameraMoved = updateMovement(renderInstance, state) || cameraMoved;
    }

    // The animation prototype's control surface is gone -- it existed to tune the wave/fire/refraction
    // parameters while they were being developed, and they are settled. The features remain, at their
    // tuned values; what went is the ~30 keys that let you change them at runtime.
    bool needsRebake = false;
    // ---- RE-BAKE WHEN THE ENVELOPE'S SHAPE CHANGED --------------------------------------------
    //
    // Only for the two controls that change WHERE animated geometry can be drawn; see
    // updateAnimControls for the split and for why getting it wrong makes geometry vanish.
    //
    // Held OFF while a key is still down, which matters because these are continuous drags: a bake is
    // a few seconds and firing one per frame of a held key would lock the window solid. So the bake
    // runs when the key is RELEASED, and until then the frame renders against the previous envelope --
    // which is the graceful direction to be wrong in, because an envelope baked for a smaller
    // amplitude simply clips the extra motion rather than losing the geometry.
    if (needsRebake) state.animRebakePending = true;
    else if (state.animRebakePending) {
        state.animRebakePending = false;
        projv::Scene& scene = projv::core::getGlobalResource<projv::Scene>(app.world);
        configureEngineMotion(gpuData.animation, state.wave);
        projv::utils::EnvelopeBakeReport report =
            projv::utils::bakeAnimationEnvelope(scene, gpuData.animation);
        projv::core::info("Anim: re-baked for lean {:.2f}v / flame travel {:.1f}v -- {} cell(s) "
                          "across {} blob(s).", state.wave.amplitude, state.wave.fireTravel,
                          report.envelopeCells, report.blobsBaked);
        // The bake marks the blobs dirty; this is what actually sends them. Incremental, so only the
        // blobs whose envelope changed are re-uploaded -- and the envelope is allocated out of the
        // SAME range allocators as the geometry, so a bigger envelope reallocates exactly the way a
        // grown geometry blob does.
        projv::graphics::flushSceneUpdates(scene, gpuData);
    }

    // Z anchors the animated slab to wherever you are standing. The warp's height profile is
    // spatial, so it has to be told where the ground is, and no part of the scene knows -- the grass
    // sits on terrain at whatever height that terrain happens to be. Fly into the grass, press U.
    //

    // ---- Z / X / C: render scale ---------------------------------------------------------------
    // Live, because the only way to judge an upscaler is to flip between it and the thing it is
    // approximating on the same view -- and a relaunch loses the view. See setRenderScale for why
    // this can be changed after construction at all.
    //
    // The quarter is on BACKTICK rather than continuing the Z/X/C run onto V, because V is the warp
    // slab-ramp control and every other letter on the keyboard is bound too. It is the least bad free
    // key: single-purpose, no modifier, and reachable without looking.
    //
    // 0.25 is a sixteenth of the pixels and is past where this renderer is trying to be good -- it is
    // here to make the reconstruction's failure modes visible rather than to be used. Two things move
    // with it that are worth knowing when reading the result. The probe atlas is declared at a quarter
    // of the G-buffer, so it lands at a sixteenth of the window on each axis and faces thinner than
    // its lattice stop owning probes (resolve.frag's fallback tiers carry those). And the LOD
    // crossover is derived from the render height, so it comes in four times nearer than at full
    // resolution -- correctly, since a pixel covers four times as much world.
    {
        static bool prevZ = false, prevX = false, prevC = false, prevTick = false;
        bool z = glfwGetKey(renderInstance.window, GLFW_KEY_Z) == GLFW_PRESS;
        bool x = glfwGetKey(renderInstance.window, GLFW_KEY_X) == GLFW_PRESS;
        bool c = glfwGetKey(renderInstance.window, GLFW_KEY_C) == GLFW_PRESS;
        bool tick = glfwGetKey(renderInstance.window, GLFW_KEY_GRAVE_ACCENT) == GLFW_PRESS;
        if (z && !prevZ) setRenderScale(renderInstance, state, 1.0f);
        if (x && !prevX) setRenderScale(renderInstance, state, 0.75f);
        if (c && !prevC) setRenderScale(renderInstance, state, 0.5f);
        if (tick && !prevTick) setRenderScale(renderInstance, state, 0.25f);
        prevZ = z; prevX = x; prevC = c; prevTick = tick;
    }



    // Q: reconstruction on/off, at the SAME render scale. See FrameState::upscaleBypass.
    {
        static bool prevQ = false;
        bool q = glfwGetKey(renderInstance.window, GLFW_KEY_Q) == GLFW_PRESS;
        if (q && !prevQ) {
            state.upscaleBypass = !state.upscaleBypass;
            projv::core::info("Upscale -> {}", state.upscaleBypass
                ? "BYPASSED (raw render resolution, point sampled)"
                : "face reconstruction");
        }
        prevQ = q;
    }

    // G: global illumination on/off. Off substitutes a flat ambient so the image stays readable --
    // the question is what the indirect bounce contributes, and that only shows against something.
    {
        static bool prevG = false;
        bool g = glfwGetKey(renderInstance.window, GLFW_KEY_G) == GLFW_PRESS;
        if (g && !prevG) {
            state.globalIllumination = !state.globalIllumination;
            projv::core::info("Global illumination -> {}",
                              state.globalIllumination ? "on" : "off (flat ambient)");
        }
        prevG = g;
    }


    // V: distance fog off / normal / heavy. See FrameState::fogScale. No longer drags the god rays
    // along by their gain -- they keep only the physical dependence, through the air in front of
    // each pixel -- so this is the ATMOSPHERE dial and [ / ] below is the SHAFT dial.
    {
        static bool prevV = false;
        bool v = glfwGetKey(renderInstance.window, GLFW_KEY_V) == GLFW_PRESS;
        if (v && !prevV) {
            state.fogScale = state.fogScale <= 0.001f  ? 1.0f
                           : (state.fogScale <= 1.001f ? 3.0f : 0.0f);
            projv::core::info("Fog -> {}", state.fogScale <= 0.001f ? "OFF"
                                         : state.fogScale <= 1.001f ? "normal"
                                                                    : "heavy (3x)");
        }
        prevV = v;
    }

    // The two atmosphere strengths as continuous drags, held-key style like the wave knobs. Same
    // Drag shape as updateAnimControls uses; not folded into that function because it takes a
    // AnimControls and these are not part of the sway.
    {
        struct AtmoDrag { int key; float* value; float step; float lo; float hi; const char* name; };
        const AtmoDrag drags[] = {
            // God-ray strength. Both shaft paths at once -- see FrameState::godrayScale.
            {GLFW_KEY_LEFT_BRACKET,  &state.godrayScale, -0.02f, 0.0f, 6.0f, "God rays"},
            {GLFW_KEY_RIGHT_BRACKET, &state.godrayScale,  0.02f, 0.0f, 6.0f, "God rays"},
            // General volumetric strength: the same value V cycles, so V snaps it to a preset and
            // these fine-tune from there.
            {GLFW_KEY_SEMICOLON,     &state.fogScale,    -0.02f, 0.0f, 6.0f, "Fog"},
            {GLFW_KEY_APOSTROPHE,    &state.fogScale,     0.02f, 0.0f, 6.0f, "Fog"},
        };
        for (const AtmoDrag& d : drags) {
            if (glfwGetKey(renderInstance.window, d.key) != GLFW_PRESS) continue;
            float before = *d.value;
            *d.value = std::clamp(*d.value + d.step, d.lo, d.hi);
            // Only on a real change, so holding a key at the end of its range stops logging.
            if (*d.value != before) {
                projv::core::info("{} strength -> {:.2f}", d.name, *d.value);
            }
        }
    }


    // `.`: traced volumetric god rays on/off. See FrameState::volumetricGodrays.
    {
        static bool prevPeriod = false;
        bool period = glfwGetKey(renderInstance.window, GLFW_KEY_PERIOD) == GLFW_PRESS;
        if (period && !prevPeriod) {
            state.volumetricGodrays = !state.volumetricGodrays;
            projv::core::info("God rays -> {}", state.volumetricGodrays
                ? "screen-space + TRACED volumetric (real shadow rays; the expensive one)"
                : "screen-space only");
        }
        prevPeriod = period;
    }




    if (state.gpuProfile) {
        // ACCUMULATED over 120 frames rather than read per frame. A single frame's GPU timestamps are
        // noisy enough that two passes a factor of two apart can swap places, which is exactly the
        // kind of reading that sends you optimising the wrong one.
        static double passMs[32] = {};
        static double frameMs = 0.0;
        static int    samples = 0;

        const bgfx::Stats* stats = bgfx::getStats();
        if (stats != nullptr && stats->gpuTimerFreq > 0) {
            const double toMs = 1000.0 / double(stats->gpuTimerFreq);
            for (uint16_t v = 0; v < stats->numViews; v++) {
                const bgfx::ViewStats& viewStats = stats->viewStats[v];
                if (viewStats.view < 32) {
                    passMs[viewStats.view] +=
                        double(viewStats.gpuTimeEnd - viewStats.gpuTimeBegin) * toMs;
                }
            }
            frameMs += double(stats->gpuTimeEnd - stats->gpuTimeBegin) * toMs;
            samples++;
        }

        if (samples >= 120) {
            const double inv = 1.0 / double(samples);
            projv::core::info("---- GPU per pass, mean of {} frames  |  render {:.0f}% scale, "
                              "----",
                              samples, state.renderScale * 100.0f);
            for (size_t i = 0; i < 32; i++) {
                if (passMs[i] <= 0.0) continue;
                const char* name = i < PASS_NAME_COUNT ? PASS_NAMES[i] : "?";
                projv::core::info("  {:>12}  {:6.3f} ms  {:5.1f}%", name, passMs[i] * inv,
                                  frameMs > 0.0 ? passMs[i] / frameMs * 100.0 : 0.0);
            }
            // The frame's own begin-to-end GPU time, which is NOT the sum of the rows above and is
            // not meant to be. The GPU overlaps the end of one view with the start of the next, so
            // the rows can add up to more than this. Read them as relative weights -- which pass is
            // large, and how each one responds to a change -- rather than as a budget that balances.
            projv::core::info("  {:>12}  {:6.3f} ms  (wall, overlaps not deducted)", "FRAME",
                              frameMs * inv);
            for (size_t i = 0; i < 32; i++) passMs[i] = 0.0;
            frameMs = 0.0;
            samples = 0;
        }
    }

    if (state.wave.dirty) {
        const AnimControls& w = state.wave;
        projv::core::info("Anim: {} | wind lean={:.2f}v gust={:.1f}v speed={:.2f} turbulence={:.2f} "
                          "dir=({:.2f}, {:.2f}) | resolve={:.0f}v shadows={} | refraction={} "
                          "| lighting={}",
                          w.animationSuspended ? "SUSPENDED (traversal off, envelope still baked)"
                                               : "engine path, 1 traversal/ray",
                          w.amplitude, w.gustSize, w.speed, w.turbulence, w.windX, w.windZ,
                          w.resolveVoxels, w.waveShadows ? "resolved" : "from rest pose",
                          w.refractionSegments == 0 ? std::string("off")
                                                    : std::to_string(w.refractionSegments) + " bend(s)",
                          state.globalIllumination ? "full GI" : "FLAT (GI off, soft sun)");
        projv::core::info("Anim: flame travel={:.1f}v turbulence={:.2f} growth={:.2f} dissolve={:.2f} "
                          "scale={:.1f}v speed={:.2f} emissionScale={:.4f}",
                          w.fireTravel, w.fireTurbulence, w.fireTurbGrowth, w.fireDissolve,
                          w.fireScale, w.fireSpeed, w.fireEmissionScale);
        if (w.debugEnvelope) projv::core::info("Anim: DEBUG -- drawing every envelope cell as solid.");

        // ---- CAN THE RUNTIME ASK FOR A CELL THE BAKE NEVER MADE? ---------------------------
        //
        // This used to be two live warnings and they are both gone, which is worth recording because
        // both described real failures that could not be designed away in the prototype:
        //
        //   * The envelope was baked for one fixed amplitude, so turning the lean past it displaced
        //     voxels into cells that did not exist -- drawn nowhere, at both ends, with the sky
        //     showing through. And because the field is COHERENT, a whole gust patch crossed the
        //     rounding threshold at the same moment, so it was a patch that vanished rather than a
        //     sprinkle of voxels.
        //   * Foliage got a PLUS-shaped envelope (four axis neighbours, no diagonals), so a wind
        //     rotated toward a diagonal opened holes in the canopy.
        //
        // Neither is reachable now, and not because the knobs were limited. `reachableOffsets` marks
        // exactly the cells a source can be drawn in -- the rounded points of the displacement's line
        // segment, enumerated rather than sampled -- so there is no shape for a diagonal to fall
        // outside of. And the envelope is REBAKED when the amplitude changes (see updateAnimControls),
        // so there is no fixed amplitude to exceed.
        //
        // The rebake is what makes the warnings unnecessary rather than merely unnecessary-looking. It
        // is affordable because the bake is a few seconds on a scene this size and only runs on a key
        // press; a renderer animating a scene it did not bake would want the old warning back.
        if (w.debugWhy == 1) {
            projv::core::info("Wave: DEBUG why-sky 1 -- magenta = the march ran out of steps.");
        } else if (w.debugWhy == 5) {
            projv::core::info("Wave: DEBUG why-sky 5 -- the HALF-RESOLUTION shaft term alone (screen-"
                              "space god rays + traced volumetric), magnified exactly as display.frag "
                              "magnifies it before adding it to the frame. If the holes are visible in "
                              "this term by itself they are made here, not in the traversal. Toggle "
                              "the traced half with '.' and scale the screen-space half with [ ].");
        } else if (w.debugWhy == 4) {
            projv::core::info("Wave: DEBUG why-sky 4 -- FOG DISTANCE banded on three scales. A pixel "
                              "whose camDist comes back far too large saturates to the fog colour "
                              "while its neighbours do not, which reads as a sky-coloured hole in "
                              "solid geometry. A hole caused by that shows here as a patch that does "
                              "not belong to the gradient around it.");
        } else if (w.debugWhy == 3) {
            projv::core::info("Wave: DEBUG why-sky 3 -- MAGENTA GLOW on any surface whose SUN SHADOW "
                              "RAY ran out of steps. Such a surface is reported LIT regardless of "
                              "what is above it, so a patch of them reads as a hole with sky or sun "
                              "through it, moves with the sun angle, and flips together when a gust "
                              "changes which cells are drawn. This is the mode for holes that modes "
                              "1 and 2 do not touch.");
        } else if (w.debugWhy == 2) {
            projv::core::info("Wave: DEBUG why-sky 2 -- magenta = out of steps; RED = crossed grass "
                              "and drew none of it (brighter = more cells, so the RESOLVE or the bake "
                              "extent is at fault); BLUE = inside a chunk but never met a target cell "
                              "(so traversal never reached the grass); dark green = real sky.");
        }
        state.wave.dirty = false;
    }

    // An animated scene never converges, which docs/plans/animation.md calls out and accepts: this
    // renderer's accumulate pass takes a 64-frame mean and its TAA another, both gated on the camera
    // having settled. With the grass moving under a still camera, that machinery averages a blade
    // against the sky behind it and the field turns to smeared mush -- which reads as "the warp is
    // broken" rather than as "the accumulator is". So while anything is actually moving, the frame
    // is declared non-static and both filters take their short reprojecting branch.
    //
    // This is a blunt instrument, not a fix. The real answer is a per-face cache -- and the g-buffer
    // already carries what one needs, since `motionClass` states outright whether a surface holds
    // still and distinguishes a swayed voxel (which keeps its source cell, so it can be gated on an
    // exact identity) from an advected one (which cannot). What this does is merely stop the
    // accumulator from hiding the thing being looked at. The cost is a noisier indirect term while
    // anything is moving.
    if (!state.wave.animationSuspended && state.wave.amplitude > 0.001f) cameraMoved = true;


    projv::core::vec3 cameraDirection = {
        std::cos(state.pitch) * std::cos(state.yaw),
        std::sin(state.pitch),
        std::cos(state.pitch) * std::sin(state.yaw)
    };

    // On the first frame there is no history, so the previous camera is made equal to the current
    // one (identity reprojection) rather than left at whatever it was initialized to.
    if (!state.prevCameraInitialized) {
        state.prevCameraPosition = state.cameraPosition;
        state.prevCameraDirection = cameraDirection;
        state.prevCameraInitialized = true;
    }

    // A sun change counts as movement: it invalidates every accumulated frame just as thoroughly as
    // a camera move does, and without this the new lighting fades in over the history's length.
    projv::core::vec4 sunDirection = sunFromScroll();
    // The sun moving is a LIGHTING change, and it needs to stay distinguishable from the sway. Both
    // used to be folded into cameraMoved, so accumulate.frag -- which knocks the GI history down on a
    // lighting change -- saw one every frame the sway was on and could never build a mean at all.
    bool sunMoved = g_sunScrollAccum != state.prevSunScroll;
    // A lighting-MODE change is a lighting change in exactly the sense this flag means: every
    // accumulated value stops being an estimate of the thing now being asked for. Folded in here
    // rather than given its own uniform because accumulate.frag's response -- drop the history and
    // rebuild -- is the one that is wanted, and it already implements it for the sun. Consumed here
    // so a single keypress invalidates one frame and not every frame after it.
    if (g_lightingModeChanged) { sunMoved = true; g_lightingModeChanged = false; }
    if (sunMoved) cameraMoved = true;
    state.prevSunScroll = g_sunScrollAccum;

    if (cameraMoved) state.frameLastMovedOn = app.frameCount;

    uploadFrameUniforms(renderInstance.getActiveRenderer(), state, cameraDirection, sunDirection,
                        cameraMoved, sunMoved, app.frameCount);

    // ---- The engine's animation state ---------------------------------------------------------
    // Time is a MEMBER of this rather than a global, which is the point: a scripted capture or a
    // voxelizer asked for frame 37 both need to sample deterministically. The engine uploads
    // whatever is here to every pass that declares pjvAnimTime / pjvMotionSets -- no resources.json
    // entry, and no uniform this example has to own.
    //
    // The motion sets are re-read every frame so the keyboard controls stay live. Most of them are
    // pure runtime knobs -- speed, gust size, turbulence, the whole flame -- because they change the
    // field without changing WHERE it can put anything. `amplitude` and `fireTravel` are the two that
    // do change that, so a key press on either triggers a re-bake; see updateAnimControls.
    gpuData.animation.time =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - state.startTime).count();
    configureEngineMotion(gpuData.animation, state.wave);

    // Resizes the render targets to the window, runs the passes at their own targets' sizes, and
    // presents. This driver draws straight into the window, so the back buffer and the render
    // resolution are the same thing and the engine's own entry point is the right one -- the scene
    // editor calls the two halves of this separately because its targets follow a panel instead.
    projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(),
                                               &gpuData);

    state.prevCameraPosition = state.cameraPosition;
    state.prevCameraDirection = cameraDirection;

    // ---- WINDOW TITLE STATS ---------------------------------------------------------------------
    // Deliberately NOT behind PROJV_ENABLE_PERF. That profiler prints a hundred-frame summary to the
    // log, which is the right tool for a measurement you are going to write down; this is for the
    // question you ask constantly while looking at the picture -- what am I actually rendering, and
    // what is it costing right now -- and an answer that needs a rebuild to see is no use for that.
    //
    // The RENDER resolution is quoted alongside the output one because every judgement about this
    // renderer depends on the ratio between them, and nothing on screen otherwise says what it is.
    // Z/X/C change it live, so it cannot be inferred from how the program was started either.
    {
        static auto lastTitleTime = std::chrono::steady_clock::now();
        static auto lastFrameStamp = std::chrono::steady_clock::now();
        static double emaMilliseconds = 0.0;

        auto now = std::chrono::steady_clock::now();
        double frameMs = std::chrono::duration<double, std::milli>(now - lastFrameStamp).count();
        lastFrameStamp = now;
        // Smoothed, because a per-frame number in a title bar is unreadable noise. Seeded on the first
        // real frame rather than from 0 so it does not spend a second climbing out of a fake value.
        emaMilliseconds = emaMilliseconds > 0.0 ? emaMilliseconds * 0.9 + frameMs * 0.1 : frameMs;

        // Retitled a few times a second. Every frame would be pure syscall for no readability.
        if (std::chrono::duration<double>(now - lastTitleTime).count() >= 0.25) {
            lastTitleTime = now;

            int windowWidth = 0, windowHeight = 0;
            glfwGetWindowSize(renderInstance.window, &windowWidth, &windowHeight);
            // ceil, matching resizeRenderTargets -- so this reports the size the targets actually
            // have rather than an idealised one that disagrees by a pixel.
            int renderWidth  = int(std::ceil(float(windowWidth)  * state.renderScale));
            int renderHeight = int(std::ceil(float(windowHeight) * state.renderScale));

            char title[256];
            std::snprintf(title, sizeof(title),
                          "AdvancedRenderer  |  %.2f ms  (%.0f fps)  |  render %dx%d -> %dx%d "
                          "(%.0f%% scale, %.0f%% pixels)  |  upscale: %s  |  GI: %s",
                          emaMilliseconds,
                          emaMilliseconds > 0.0 ? 1000.0 / emaMilliseconds : 0.0,
                          renderWidth, renderHeight, windowWidth, windowHeight,
                          state.renderScale * 100.0f,
                          state.renderScale * state.renderScale * 100.0f,
                          state.upscaleBypass ? "RAW (bypassed)" : "reconstruction",
                          state.globalIllumination ? "on" : "off");
            glfwSetWindowTitle(renderInstance.window, title);
        }
    }
}

int main(int argc, char** argv) {
    projv::Application app = projv::core::createApp();
    projv::core::assignSystemStage(app, projv::SystemStage::Startup, startup);
    projv::core::assignSystemStage(app, projv::SystemStage::Update, update);
    projv::core::assignSystemStage(app, projv::SystemStage::Render, render);

    // The scene given on the command line has to survive until startup runs, and the ECS stages take
    // only the Application -- so it is parked in a global resource that startup reads.
    if (argc > 1) {
        SceneRequest& request = projv::core::createGlobalResource<SceneRequest>(app.world);
        request.path = argv[1];
        if (!request.path.empty() && request.path.back() != '/') request.path += '/';
    }

    projv::core::runApplication(app);
    return 0;
}
