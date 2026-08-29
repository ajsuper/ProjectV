$input v_color0
$input v_texcoord0

// =============================================================================
// compose.frag  --  Pass 8. Puts the frame back together.
//
//   hdr = gDirect + gAlbedo * R
//
// gDirect and gAlbedo are full-resolution and untouched by any temporal filter. R is the indirect
// irradiance, and this pass is where it is filtered into ONE LOW-FREQUENCY FIELD.
//
// -----------------------------------------------------------------------------
// THE INDIRECT TERM IS PER FACE AGAIN, AND THIS TIME IT ACCUMULATES
// -----------------------------------------------------------------------------
// An earlier version of this pass was flat per voxel face and had to give that up. The mechanism was
// sound -- project the face CENTRE through the camera, have every pixel on the face read that one
// texel -- but a per-face value can only be carried between frames by matching an exact face
// IDENTITY, and that match kept failing. The mean was reset permanently; a 128-frame accumulation
// buffer never got past about four.
//
// Two things had to change before per-face could work, and both are now in place upstream:
//
//   * THE IDENTITY TEST HAD NEVER RUN. accumulate.frag's gKey was its tenth bound input, which lands
//     on bgfx texture stage 9 -- where the engine also binds the scene's materialIDs, overwriting it.
//     The gate was reading a uint scene texture through a float sampler and quietly falling through
//     to its geometric fallback. See the binding-order note in performRenderPasses.
//
//   * THE VALUE IS NOW WORTH KEEPING. Under the cascades, what a face was worth depended on where the
//     camera stood, so even a history that survived was averaging together answers to different
//     questions. probe.frag seeds its ray directions from the face key instead, so a face's R is a
//     function of the face and the frame and nothing else -- which is exactly the property a running
//     mean needs.
//
// So the spatial filter below is no longer the denoiser. It is a bootstrap for pixels whose face has
// not been gathered yet, and it gets out of the way as history builds.
//
// Inputs (FBO 7 [0..2], FBO 1 [3..8]):
//   0 accumR  1 histKey  2 histPos   3 gPos 4 gNormal 5 gAlbedo 6 gDirect  7 gFace 8 gKey
//
// The cascade-0 atlas used to be bound here too, ahead of the G-buffer, for a rough specular
// reflection. Both are gone: the cascades were replaced by the per-face probe gather (see
// pjv_probe.sc), and the reflection they fed had been switched off (SPEC_STRENGTH 0) since it could
// only represent a very wide lobe from ~16 directions anyway. Dropping it also brings this pass to
// nine bound inputs, which is the most that fits below the scene's texture stages -- see
// PROJV_MAX_PASS_SAMPLERS.
// Output (FBO 8): 0 hdr composite, for taa to anti-alias and display to tone map.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(accumR,   0);   // FBO7[0]: temporally accumulated indirect irradiance R, a = age
SAMPLER2D(gPos,     3);   // FBO1[0]: xyz world pos, a = camDist (a < 0 => sky)
SAMPLER2D(gNormal,  4);   // FBO1[1]: xyz normal, a = voxel edge size
SAMPLER2D(gAlbedo,  5);   // FBO1[2]
SAMPLER2D(gDirect,  6);   // FBO1[3]: direct sun + emission (or sky colour for background)
SAMPLER2D(gFace,    7);   // FBO1[4]: xyz the hit voxel FACE's centre, w = the voxel's edge length.
                          // Read by the foliage edge gradient, which needs to know where within its
                          // own face a pixel sits -- see foliageEdgeGlow.
SAMPLER2D(gKey,     8);   // FBO1[5]: voxel identity. Read ONLY by the per-voxel lighting mode (U),
                          // which needs to know which pixels belong to one voxel; costs nothing when
                          // the mode is off, and nothing to bind either way since FBO 1 binds it.

// The animation controls. Two things are read here: the clean-view switch, which swaps the whole
// indirect term for a flat hemispherical ambient, and the debug mode below. Included rather than
// re-declared, because there are only two of these uniforms now and the accessors name their lanes.
#include <pjv_anim_controls.sc>

// x = spare, y = shadow rays off (T), z = FOLIAGE TRANSLUCENCY OFF (B), w = PER-VOXEL LIGHTING (U).
// .z and .w are read here; the rest belong to gbuffer.frag and upscale.frag. Declared rather than
// included because this pass has no other business with those keys: the uniform is already in
// resources.json and uploaded every frame. PJV_PER_VOXEL_LIGHTING in pjv_face_key.sc names .w.
uniform vec4 debugParams;
// x = global illumination on (1) / off (0). Its own uniform rather than another debugParams bit,
// because it is a feature switch a user reaches for -- "what is the GI actually contributing" -- and
// not a diagnostic. renderParams and debugParams are both full, and overloading a lighting slot with
// a feature flag is how those two ended up holding four unrelated things each.
uniform vec4 featureToggles;

// ---- WHY THIS PASS HAS TO KNOW ABOUT THE DEBUG MODE -------------------------------------------
// .y is the why-does-this-pixel-look-wrong cycle (the `/` key). gbuffer.frag marks pixels by writing
// a colour, and every one of those marks was INVISIBLE until this was added, because both paths below
// end in applyFog -- and fog at the sky distance "saturates completely" by design, so it overwrites
// whatever it is handed. Three separate diagnoses were made from the resulting negatives before
// anyone noticed the instrument could not reach the screen.
//
// So: with a debug mode on, this pass does not fog. A marker that is erased by the thing it is trying
// to measure is worse than no marker, because it reads as a clean negative result.
int composeDebugMode() { return animDebugMode(); }

// This pass works entirely on screen-resolution buffers and no longer addresses a probe grid at all,
// but pjv_cascade_common.sc requires both grids to be named rather than guessed at, so both are its
// own target.
#define CASCADE_SCREEN_RES passTargetRes.xy
#define CASCADE_ATLAS_RES  passTargetRes.xy
#include <pjv_cascade_common.sc>
#include <pjv_face_key.sc>
// SUN_DIR and SUN_COLOR, for reassembling the direct sun term -- see the shadow blur below for why it
// arrives here in two pieces. Already pulled in by pjv_cascade_common.sc above, so not included again.
// fogParams, fogOpticalDepth and applyFog. Shared with godrays.frag, which needs the same optical
// depth -- see the header of that file for why the two must agree.
#include <pjv_atmosphere.sc>

// The fog itself moved to sharedShaders/pjv_atmosphere.sc when godrays.frag turned up needing the
// same optical depth. Nothing about it changed in the move; see that file for the model and for why
// the two passes must share one description of the air rather than each carrying its own.

// ---- PER-VOXEL DIRECT LIGHT --------------------------------------------------------------------
// The cosine factor, when a voxel is lit as a unit instead of face by face.
//
// N.L is the one term in the direct sun that is IRREDUCIBLY per face -- a shadow can be pooled and a
// translucency lobe can drop its normal, but "how obliquely does the light strike" is a question
// about an orientation, and a voxel has six. So the mode has to choose which single number stands in
// for all six, and there are exactly two defensible answers. Both are computed from the light
// direction alone, so neither is a magic constant:
//
//   THE MEAN over the six faces. Opposite faces never both face the light, so summing
//   max(dot(N_f, L), 0) over the six axis normals collapses to |L.x| + |L.y| + |L.z|, and dividing by
//   six gives the mean irradiance over the voxel's whole surface. This is the ENERGY-CONSERVING
//   answer: the voxel emits exactly what it did before, spread evenly over its faces. It is also
//   markedly darker on everything you can actually see, because what you see is mostly the lit face
//   and this hands it the average of one lit face and five that are not.
//
//   THE BRIGHTEST FACE, max(|L.x|, |L.y|, |L.z|). Not energy-conserving -- a voxel's shaded side is
//   lifted to its lit side's value rather than the two meeting in the middle -- but it is the one
//   that leaves a flat wall of voxels at the brightness it already had, so switching the mode on
//   changes the SHADING without also changing the exposure. On solid geometry, where only lit faces
//   are visible anyway, it is very nearly a no-op; the flattening shows up exactly where it should,
//   on isolated voxels, edges and silhouettes.
//
// Defaulted to the brightest face for that reason: a mode that dims the whole image by a factor of
// three cannot be A/B'd against the mode it replaces, because every difference is swamped by the
// exposure change. Set PER_VOXEL_BRIGHTNESS_MATCH to 0.0 for the energy-conserving endpoint; the
// lerp between them is continuous and both ends mean something.
#define PER_VOXEL_BRIGHTNESS_MATCH 1.0

float perVoxelNdotL(vec3 L) {
    vec3  a    = abs(L);
    float mean = (a.x + a.y + a.z) / 6.0;
    float peak = max(a.x, max(a.y, a.z));
    return mix(mean, peak, PER_VOXEL_BRIGHTNESS_MATCH);
}

// ---- A SUN SHEEN ------------------------------------------------------------------------------
// One Blinn-Phong lobe on the sun, reusing the visibility that was traced and blurred for the diffuse
// term -- so it costs a normalize, a pow and nothing else. No new ray, no new sampler, no new pass.
//
// It is here because a purely Lambertian voxel scene has no light that MOVES. Every other term in
// this renderer is view-independent by construction, which is what makes it stable but also what
// makes it read as matte cardboard: fly around a scene and nothing on it changes but its outline.
// A specular highlight is the one cue that says a surface has an orientation relative to YOU.
//
// Kept deliberately weak and gated on Fresnel, so it is nearly absent head-on and shows up as a rim
// at grazing angles -- which is where a real rough dielectric puts it, and is also the placement
// least able to look like plastic. There is no per-material roughness to read here (gAlbedo.a carries
// the motion class), so it is one constant lobe for the whole scene; a material-driven version wants
// a roughness channel in the G-buffer first.
// Flip SPEC_ENABLE to 0 to remove it entirely -- a separate integer rather than testing the strength,
// because `#if` takes integer constant expressions only and a float comparison there is an error, not
// a fold.
#define SPEC_ENABLE     1
#define SPEC_STRENGTH   0.06
#define SPEC_SHININESS  42.0
vec3 sunSheen(vec3 N, vec3 V, float sunVisibility) {
#if SPEC_ENABLE
    if (sunVisibility <= 0.0) return vec3(0.0);
    vec3  Hv    = normalize(SUN_DIR + V);
    float lobe  = pow(max(dot(N, Hv), 0.0), SPEC_SHININESS);
    // Schlick, at a dielectric's 0.04 normal reflectance.
    float f     = 0.04 + 0.96 * pow(1.0 - max(dot(N, V), 0.0), 5.0);
    return SUN_COLOR * (lobe * f * SPEC_STRENGTH * sunVisibility * max(dot(N, SUN_DIR), 0.0));
#else
    return vec3(0.0);
#endif
}

// ---- SCREEN-SPACE SUBSURFACE SCATTERING, FOR FOLIAGE -------------------------------------------
// A blade of grass with the sun behind it GLOWS. That is not a specular highlight and it is not
// bounced light -- it is sunlight that entered the far side of a thin leaf, scattered a couple of
// times inside it, and came out toward the eye, picking up the leaf's own colour on the way through.
// It is the single most recognisable thing about lit vegetation, and its absence is most of why a
// field of voxel grass reads as green gravel: every blade here was a perfectly opaque Lambertian
// surface, so a blade facing away from the sun was simply black.
//
// WHY THIS IS THE CHEAP VERSION. The physically-argued route is to trace the light through the
// medium, which for one-voxel-thick blades scattered over a whole field is not affordable at any
// resolution. The standard substitute -- Dice's, from Frostbite -- is to note that the exit lobe of
// a thin translucent sheet is roughly the light direction pushed backwards and bent slightly around
// the surface normal, and to evaluate that lobe analytically:
//
//     H = normalize(L + N * distortion)      the transmission axis, bent off the light direction
//     I = pow(saturate(dot(V, -H)), power)   how much of it is aimed at the eye
//
// which is three instructions and needs nothing the G-buffer does not already carry.
//
// The part that IS screen-space is everything the analytic lobe cannot know: how much leaf the light
// had to cross, and whether the sun reaches this patch at all. Both come out of one 8-tap ring below.
//
// WHAT IT IS APPLIED TO, and why that gate is free. gAlbedo.a already carries a motion class --
// 0 stationary, 0.5 displaced (grass, flowers, leaves), 1.0 advected (fire) -- written by
// gbuffer.frag for the temporal filters. The set of materials it marks as displaced is exactly the
// set of materials that are thin and translucent: main.cpp's WAVE_MATERIAL_RULES names blades,
// petals, leaves and needles, and deliberately excludes bark and dead wood. So the channel that says
// "this sways" is already the channel that says "light passes through this", and this effect costs
// nothing on any pixel that is not vegetation -- including every pixel of every scene with no
// vegetation in it, where the branch is uniformly false across whole warps.
//
// It is DETERMINISTIC, like everything else added here: a pure function of the G-buffer, so it
// cannot flicker and the temporal filters never see it move.
#define SSS_ENABLE 1
// ---- WHY THE LOBE NO LONGER BENDS AROUND THE NORMAL --------------------------------------------
// This used to bend the transmission axis off the light direction by SSS_DISTORTION * N, which is
// the standard Frostbite formulation and is what widens the band of angles a blade glows over. It is
// gone, and the per-voxel lighting mode is what showed why.
//
// In that mode the bend is dropped (there is no single face normal to bend around), and the result
// was not worse -- it was the effect finally working. What appears is a RIM: the edges of grass and
// canopy light up along their silhouettes, which is exactly what backlit foliage does and what the
// per-face version never produced. The reason is coherence. With the bend in, every face of a voxel
// gets a different transmission axis, so the glow is a different value on each of the three faces you
// can see and the eye reads three flat patches instead of one object with a lit edge. Without it the
// axis is exactly -L -- a function of the light and the eye and nothing else -- so a whole blade, or
// a whole leaf cluster, glows as ONE thing and the silhouette emerges.
//
// So the axis is now -L in both modes, and what the bend used to buy is bought by SSS_POWER instead:
// lower it to widen the band of angles the glow covers. What the per-face mode adds on top is a
// spatial gradient WITHIN each face rather than a different lobe per face -- see foliageEdgeGlow.
//
// Lobe tightness. Lower is a wider, softer glow over more of the field.
#define SSS_POWER      3.0
// A floor under the lobe -- the part of the transmitted light that comes out in every direction
// rather than forward. Without it a blade lit from directly overhead is as black on its shaded side
// as an opaque one, which is wrong: a leaf is still translucent when you are not looking through it.
#define SSS_AMBIENT    0.22
// Overall gain, in the same units as the diffuse term next to it -- both divide by PI and multiply
// SUN_COLOR, so this is directly comparable to an albedo. At 0.6 a fully backlit blade reads a little
// brighter than the same blade would if it were facing the sun, which is what backlit foliage does.
#define SSS_STRENGTH   0.6
// The colour the light picks up going through. Multiplies the albedo rather than replacing it, so a
// blade's own colour still comes through and a red flower transmits red. The tint is what turns a
// green blade's transmission the yellow-green that chlorophyll actually passes -- it kills blue hard,
// keeps green, and lifts red, which is the transmission spectrum of a leaf in one constant.
#define SSS_TINT       vec3(1.55, 1.05, 0.35)
// The 8-tap ring, in render pixels. One radius serves both jobs it does; see the function.
#define SSS_RING_TAPS   8
#define SSS_RING_PIXELS 6.0
// How much a dense clump dims the glow relative to an isolated blade standing against the sky.
#define SSS_DENSITY_DIM 0.45

// ---- THE EDGE GRADIENT, IN THE PER-FACE MODE ---------------------------------------------------
// Per-voxel mode gives every face of a voxel one flat value, which is what makes the rim appear -- but
// flat is also all it is, so a voxel close enough to fill a good part of the screen glows as a solid
// block. What the per-face mode can add, and per-voxel by definition cannot, is variation ACROSS the
// face: the same coherent per-voxel magnitude, distributed so it is strongest at the voxel's edges and
// falls away toward the middle of each face.
//
// That is what the real thing does. Light that entered the far side of a leaf has to travel further to
// reach the middle of the near side than to reach its edge, so a thin translucent object is brightest
// where it is thinnest -- at its rim. The gradient is what turns "this voxel is glowing" into "this
// voxel has a lit edge", and it is the difference between foliage that looks lit from behind and
// foliage that looks like it is made of green light.
//
// The gradient is deliberately SMOOTH rather than a band at the border. A hard rim would draw a bright
// outline around every foliage voxel and read as wireframe, which is exactly the artefact a
// per-voxel-anything invites; and SSS_EDGE_FLOOR keeps the middle of a face lit rather than black, so
// what varies is how much brighter the edge is, not whether the face glows at all.
//
// Range of the ramp: 0 at the face centre, 1 at the face rim. Raise the power to pull the glow tighter
// into the edge, lower it to spread it across the whole face.
#define SSS_EDGE_POWER  2.0
// How dark the middle of a face gets relative to its edge. 1.0 is the flat per-voxel look.
#define SSS_EDGE_FLOOR  0.35
// How much the SUN's direction skews the gradient across the face. Light wraps around the edge nearest
// the lit side, so the rim on the sunward half of a face is brighter than the rim away from it. 0 makes
// the ramp symmetric and the glow sits equally on all four edges, which reads as an outline rather than
// as light coming from somewhere.
#define SSS_EDGE_SUN_BIAS 0.55

// Where this point sits within its own voxel face, as a glow multiplier.
//
// The face is an axis-aligned square of side `edge` centred on `faceCentre`, so the distance to its rim
// is a Chebyshev distance in the plane -- max of the two in-plane axes -- and the axes do not have to be
// named: projecting out the normal leaves the normal's own component at zero, and multiplying each
// component by (1 - |N|) drops it exactly rather than approximately.
//
// SAFE AT THE CENTRE. The sun skew divides by the in-plane length of both vectors, and both go to zero
// there -- d because the point is at the centre, Lt because the sun can be perpendicular to the face.
// Written so that neither division can be taken: `along` is normalised by the half-edge and the sun's
// in-plane length with a floor, so d = 0 gives along = 0 and a neutral 0.5 skew rather than a NaN that
// a later multiply by zero would fail to clear.
float foliageEdgeGlow(vec3 P, vec3 N, vec3 faceCentre, float edge) {
    vec3 d = P - faceCentre;
    d -= N * dot(d, N);                                  // into the face's plane

    // Not `half`: that is a type name in HLSL, and this file is compiled for D3D on Windows even
    // though the Linux build only ever sees the SPIR-V path.
    float halfEdge = max(edge * 0.5, 1e-6);
    vec3  an   = abs(N);
    vec3  t    = abs(d) / halfEdge;
    // The normal's axis contributes nothing; (1 - |N|) is 0 on it and 1 on the two tangent axes.
    float rim  = clamp(max(max(t.x * (1.0 - an.x), t.y * (1.0 - an.y)), t.z * (1.0 - an.z)),
                       0.0, 1.0);

    vec3  Lt   = SUN_DIR - N * dot(SUN_DIR, N);          // the sun, projected into the face
    float ltLen = sqrt(max(dot(Lt, Lt), 0.0));
    float along = dot(d, Lt) / max(halfEdge * ltLen, 1e-6);  // ~[-1,1] across the face, toward the sun
    float skew  = clamp(0.5 + 0.5 * along, 0.0, 1.0);

    float ramp = pow(rim, SSS_EDGE_POWER) * mix(1.0 - SSS_EDGE_SUN_BIAS, 1.0, skew);
    return mix(SSS_EDGE_FLOOR, 1.0, clamp(ramp, 0.0, 1.0));
}

// SCREEN-SPACE DENSITY: the fraction of the ring that is solid geometry at about this depth. This is
// the thickness estimate. A blade on the edge of a clump, silhouetted against sky or against distant
// ground, has most of its ring miss -- it is thin, and it lights up. A blade buried in the middle of a
// tuft has a full ring, and the sun reaching it has crossed several other blades first. It is a crude
// measure of thickness and it is the right KIND of crude: what it produces is variation across a
// field, so a lawn stops glowing as one flat sheet and instead lights up at its edges and along its
// silhouettes, which is where a real one does.
//
// THIS USED TO ALSO RETURN A "REGIONAL SUN EXPOSURE" -- the mean of the sun's visibility over the same
// ring -- because the pixel's own visibility was useless for the job: gbuffer.frag wrote zero whenever
// NdotL <= 0, and a backlit blade always has NdotL <= 0. The ring was an attempt to ask the
// neighbourhood instead, and it was wrong in the case that mattered most: a backlit blade's neighbours
// are all backlit too, so the ring reported "no sun" exactly where the truth was "full sun on the far
// side". gbuffer.frag now casts a real per-voxel shadow ray for every foliage voxel, so the answer is
// traced rather than guessed, and the eight gDirect fetches that used to ride along here are gone.
float foliageNeighbourhood(vec2 uv, float camDist) {
    float solid = 0.0;
    for (int i = 0; i < SSS_RING_TAPS; i++) {
        float a = (float(i) + 0.5) * (6.2831853 / float(SSS_RING_TAPS));
        // Snapped, for the reason everything in this file snaps: these targets filter bilinearly, and
        // a G-buffer tap between texels returns a depth belonging to no surface -- which here would
        // invent an occluder halfway to the sky's 1e5-unit placeholder point.
        vec2 tapUV = pjvSnapToTexel(uv + vec2(cos(a), sin(a)) * SSS_RING_PIXELS * passTargetRes.zw,
                                    passTargetRes.xy);
        float tapDist = texture2D(gPos, tapUV).a;
        // Sky is not thickness. A blade against the sky is the thinnest thing in the frame, and this
        // is the branch that says so.
        if (tapDist < 0.0) continue;
        // Nor is a wall fifty metres behind it. Without the depth window, distant background counts
        // as material the light had to cross, and every blade silhouetted against a far surface stops
        // glowing -- which is precisely the silhouette the effect is for. The window is relative to
        // depth because the G-buffer's own precision is.
        if (abs(tapDist - camDist) > camDist * 0.06 + 1e-3) continue;
        solid += 1.0;
    }
    // Over the WHOLE ring, not over the taps that survived the depth window: a blade at the edge of a
    // clump has few neighbours, and dividing by that few would call it as dense as one buried in a
    // tuft.
    return solid / float(SSS_RING_TAPS);
}

// The transmitted light itself.
//
// The MAGNITUDE is per voxel in both modes: the transmission axis is exactly -L, so a whole blade or
// leaf cluster glows as one object and its silhouette emerges. See the note at SSS_POWER for why the
// normal bend that used to be here is gone.
//
// The DISTRIBUTION is what the two modes differ in, and the split falls where each mode's definition
// puts it. Per-voxel mode owes every face of a voxel one identical number, so it takes the magnitude
// flat -- a within-face gradient would contradict the mode. The per-face mode has no such obligation,
// so it spends the same magnitude as a gradient across the face: brightest at the voxel's edges,
// skewed toward the sun, falling to SSS_EDGE_FLOOR in the middle. That is the shape the real thing
// has, and it is the difference between a voxel that is glowing and a voxel with a lit edge.
// `voxelSunlit` is the TRACED answer to "is this voxel in sunlight", which gbuffer.frag now casts a
// real per-voxel shadow ray for on every foliage voxel -- from the voxel's centre, pushed out along
// the sun, ungated by facing. That is the input this effect always needed and never had: a backlit
// blade's own face reports zero visibility by construction, so the term was previously driven by a
// screen-space average of its neighbours, which are backlit too. It is the blurred value rather than
// the raw one, so the glow fades across a shadow boundary instead of switching.
vec3 foliageTransmission(vec3 N, vec3 V, vec3 albedo, float density, float voxelSunlit,
                         vec3 P, vec3 faceCentre, float faceEdge) {
#if SSS_ENABLE
    // Straight back along the light, then asked how much of it points at the eye.
    float lobe = pow(clamp(dot(V, -SUN_DIR), 0.0, 1.0), SSS_POWER) + SSS_AMBIENT;

    float thin = mix(1.0, SSS_DENSITY_DIM, density);           // thin where the ring is empty
    float edge = PJV_PER_VOXEL_LIGHTING ? 1.0
                                        : foliageEdgeGlow(P, N, faceCentre, faceEdge);

    return (albedo * SSS_TINT / PI) * SUN_COLOR * (lobe * thin * voxelSunlit * edge * SSS_STRENGTH);
#else
    return vec3(0.0);
#endif
}

// ---- SOFT SHADOWS, SPATIALLY ------------------------------------------------------------------
// The radius of the shadow blur, in render-resolution pixels.
//
// This is where shadow softness comes from now, and it is a deliberate substitution for the physical
// route. Widening the sun disk is how a penumbra really forms, but it makes the one shadow ray per
// pixel stochastic, so the penumbra arrives as one-sample noise and is smooth only after many frames
// have been averaged. Anything that moves never gets those frames. Blurring a sharp, deterministic
// visibility instead has no noise in it to converge: a moving blade's shadow is as smooth on its first
// frame as on its hundredth.
//
// What is blurred is ONLY the visibility. The shading it multiplies -- (albedo/PI) * NdotL -- stays
// per-pixel and razor sharp, which is why this softens shadows without softening voxel edges.
//
// Measured in RENDER pixels, so at a reduced render scale the penumbra covers proportionally more of
// the final image. Arguably right (a cheaper frame gets a softer shadow) but it is a side effect, not
// a decision -- if it matters, scale this by the ratio of render to output resolution.
#define SHADOW_BLUR_SIGMA  1.6
// Taps reach +-this many render pixels, so 2 is a 5x5 kernel. Spacing is ONE pixel deliberately: the
// first version of this took nine taps two pixels apart, and a sparse box does not read as a blur --
// it reads as three lobes, because the gaps between taps are as wide as the features being smoothed.
// Cost is 25 taps; drop to 1 (3x3) with a smaller sigma if that matters more than smoothness.
#define SHADOW_BLUR_EXTENT 2

// The sun's visibility at this pixel, averaged along the surface it is standing on.
//
// ---- SCREEN-SPACE AMBIENT OCCLUSION -----------------------------------------------------------
// Contact shading for the indirect term, from the G-buffer this pass already binds -- no new inputs,
// which matters because this pass is at the nine-sampler ceiling (see PROJV_MAX_PASS_SAMPLERS).
//
// The estimator is the Alchemy/SAO one: for a neighbour at Q, the quantity max(0, (Q-P).N) / |Q-P|^2
// is that neighbour's contribution to the occlusion integral over the hemisphere, and it needs only a
// world position and a normal per texel. Both are exactly what gPos and gNormal hold, so nothing has
// to be reconstructed from depth and there is no projection matrix to get wrong.
//
// WHY THIS EARNS ITS PLACE HERE. The cascade gather is 8 rays a frame over a whole hemisphere, so it
// resolves the low-frequency bounce well and the small-scale darkening where surfaces meet hardly at
// all -- that detail is below its angular resolution and no amount of temporal averaging recovers it.
// AO is the standard substitute and is exactly complementary: it is entirely small-scale, entirely
// deterministic, and it multiplies a term that is already low frequency.
//
// APPLIED TO THE INDIRECT TERM ONLY. AO approximates how much of the *ambient* hemisphere reaches a
// point; the sun is a single direction with its own visibility already traced per pixel and blurred
// above. Multiplying direct light by AO is the classic mistake -- it darkens lit surfaces that have
// an unobstructed view of the sun and reads as dirt.
// OFF. The estimator below is kept intact rather than deleted -- it costs nothing while this is 0
// (ambientOcclusion returns a constant 1.0 and the compiler folds every multiply by it away), and the
// P-key AO view still needs something to call. Flip to 1 to bring it back.
#define AO_ENABLE 0
// Radius in VOXELS rather than world units, so it follows the scene's own scale: a scene authored at
// a finer voxel grid gets proportionally finer contact shading without retuning. gNormal.a carries the
// hit voxel's edge length.
#define AO_RADIUS_VOXELS 6.0
// Taps. The spiral below distributes them evenly, so this trades cost against pattern directly.
#define AO_SAMPLES 16
#define AO_SPIRAL_TURNS 7.0
// Screen-space ceiling on the radius. Without it a close-up surface projects its whole world radius
// across half the frame and every tap lands far outside anything relevant -- expensive and wrong.
#define AO_MAX_PIXELS 48.0
// Self-occlusion guard, scaled by depth because the G-buffer's own precision is. Too small and a flat
// surface shadows itself in a fine mesh; too large and genuine contacts are missed.
#define AO_BIAS 0.008
#define AO_EPS  0.0001
// Strength and contrast. AO is not a physical quantity here, it is a perceptual one.
#define AO_INTENSITY 1.0
#define AO_POWER     1.6

// Frame-INDEPENDENT per-pixel rotation, and the choice is the whole reason this can be applied to a
// term the renderer spent the last several passes stabilising.
//
// The usual way to hide a sparse tap pattern is to rotate it per pixel AND per frame and let the
// temporal filter average the result. That would put a fresh random value into every pixel every
// frame -- reintroducing precisely the flicker the accumulate and taa gates exist to remove, into the
// one term least able to absorb it. Hashing the pixel alone makes the AO a pure function of the
// geometry: identical every frame, so it cannot flicker at all, and a still camera converges because
// there is nothing left to converge. The cost is that the residual pattern is static rather than
// averaged away, which is why the tap count is 16 rather than 4.
float aoHash(vec2 pixel) {
    return fract(sin(dot(pixel, vec2(12.9898, 78.233))) * 43758.5453);
}

float ambientOcclusion(vec2 uv, vec3 P, vec3 N, float camDist, float voxelSize) {
#if AO_ENABLE
    float radiusWorld = AO_RADIUS_VOXELS * max(voxelSize, 1e-4);
    // World units per pixel at this depth -- the same footprint the temporal gates measure against.
    float pixelWorld  = max(camDist * (2.0 * tan(radians(FOV * 0.5))) / passTargetRes.y, 1e-6);
    float radiusPixels = min(radiusWorld / pixelWorld, AO_MAX_PIXELS);
    // Under about a pixel there is no neighbourhood left to sample and every tap would read this same
    // texel, which sums to zero occlusion anyway -- return early rather than pay 16 fetches for it.
    if (radiusPixels < 1.0) return 1.0;

    float rotation = aoHash(floor(uv * passTargetRes.xy)) * 6.2831853;
    float sum = 0.0;

    for (int i = 0; i < AO_SAMPLES; i++) {
        // Even area coverage: radius as sqrt of the sample index spreads taps uniformly over the disc
        // instead of clustering them at the centre, which a linear ramp would.
        float t     = (float(i) + 0.5) / float(AO_SAMPLES);
        float angle = rotation + t * 6.2831853 * AO_SPIRAL_TURNS;
        float r     = radiusPixels * sqrt(t);

        // Snapped, for the reason the rest of this file now snaps: these render targets filter
        // bilinearly by default, and a G-buffer tap between texels returns a blend belonging to no
        // surface -- here it would invent an occluder halfway to the sky's placeholder point.
        vec2 sUV = pjvSnapToTexel(uv + vec2(cos(angle), sin(angle)) * r * passTargetRes.zw,
                                  passTargetRes.xy);
        vec4 sp = texture2D(gPos, sUV);
        if (sp.a < 0.0) continue;                  // sky occludes nothing

        vec3  v  = sp.xyz - P;
        float vv = dot(v, v);
        // Beyond the radius a neighbour is not a contact, it is a different part of the scene. Without
        // this cut a foreground object draws a dark halo onto the distant geometry behind it.
        if (vv > radiusWorld * radiusWorld) continue;

        float vn = dot(v, N);
        sum += max(0.0, vn - camDist * AO_BIAS) / (vv + AO_EPS);
    }

    // The 2/N normalisation is the estimator's own: sum approximates the hemisphere integral, and this
    // turns it into an occluded FRACTION.
    float ao = max(0.0, 1.0 - (2.0 * AO_INTENSITY / float(AO_SAMPLES)) * sum * radiusWorld);
    return pow(clamp(ao, 0.0, 1.0), AO_POWER);
#else
    return 1.0;
#endif
}

// GUIDED BY ORIENTATION AND PLANE, not by depth. This is the whole correctness of the filter, and
// depth alone gets it exactly wrong in the case that matters most.
//
// A voxel's lit top face and its shadowed side face are at essentially the SAME DEPTH -- they meet
// along an edge a fraction of a voxel wide. A depth guide therefore sees no reason to reject either
// from the other's neighbourhood, averages their visibility together, and the result is that the
// boundary between light and shadow gets the blur radius applied to it precisely where it coincides
// with a GEOMETRIC edge. The voxel edge stops being crisp and takes on the resolution of the shadow
// filter, which is the one thing this renderer must never do.
//
// So the question a tap has to answer is not "are you at a similar distance" but "are you on the same
// SURFACE" -- same facing, and in the same plane:
//
//   * same normal rejects the two faces of one voxel from each other, so a shadow boundary that runs
//     along an edge stays as sharp as the edge does;
//   * coplanarity rejects a parallel face at a different height, so a step does not bleed its shadow
//     onto the tread below it.
//
// What survives both tests is a neighbouring patch of the SAME flat surface -- and a shadow boundary
// crossing a flat surface is exactly the thing that should be soft. Geometry stays the renderer's
// business; only the light is filtered.
// PER-VOXEL MODE adds one clause and changes nothing else. A tap is accepted if it is on the same
// flat surface as before, OR if it is on the SAME VOXEL, whichever face of it -- and a same-voxel tap
// skips the coplanarity falloff, because the whole point is that a voxel's perpendicular faces should
// now be averaged together rather than rejected from each other.
//
// Written as an addition rather than a replacement because the two clauses do different jobs and both
// are needed. The same-surface clause is where SOFTNESS comes from: it reaches across neighbouring
// voxels of one wall, which is the only direction a penumbra can be built along. Restricting the
// kernel to one voxel instead would pool the faces correctly and then hand every distant voxel --
// which covers a single pixel -- a one-tap "blur", losing the soft shadow exactly where it is most of
// what is left of the geometry. Keeping both clauses gives a shadow that is soft across a surface and
// flat across a voxel, which is what the mode is asking for.
float blurredSunVisibility(vec2 uv, vec3 centreP, vec3 centreN, float voxelSize) {
    vec2  texel       = passTargetRes.zw;
    float twoSigmaSq  = 2.0 * SHADOW_BLUR_SIGMA * SHADOW_BLUR_SIGMA;
    float planeSigma  = max(voxelSize * 0.5, 1e-4);

    bool perVoxel = PJV_PER_VOXEL_LIGHTING;
    // Snapped: an identity read between two texels names neither face. Only fetched in per-voxel mode.
    vec4 centreKey = perVoxel ? texture2D(gKey, pjvSnapToTexel(uv, passTargetRes.xy)) : vec4(0.0);

    float sum = 0.0;
    float weight = 0.0;
    for (int y = -SHADOW_BLUR_EXTENT; y <= SHADOW_BLUR_EXTENT; y++)
    for (int x = -SHADOW_BLUR_EXTENT; x <= SHADOW_BLUR_EXTENT; x++) {
        vec2 offset = vec2(float(x), float(y));
        vec2 tapUV  = uv + offset * texel;

        vec4 tapPos = texture2D(gPos, tapUV);
        if (tapPos.a < 0.0) continue;                            // sky: nothing to average

        bool sameVoxel = perVoxel &&
                         pjvSameVoxel(texture2D(gKey, pjvSnapToTexel(tapUV, passTargetRes.xy)),
                                      centreKey);

        // Voxel faces are axis-aligned, so same-facing is a near-exact test rather than a threshold
        // that needs tuning: two faces either share a normal or are 90 degrees apart. This one stays a
        // hard cut ON PURPOSE -- it falls exactly on a geometric edge, and that edge should be sharp.
        // In per-voxel mode a tap on this same voxel is admitted regardless, which is precisely the
        // edge the mode exists to stop being sharp.
        if (!sameVoxel && dot(texture2D(gNormal, tapUV).xyz, centreN) < 0.99) continue;

        // GAUSSIAN, not a box. A box weights a tap two pixels away as heavily as the one next door, so
        // its output has a flat top and hard shoulders and reads as a smear with edges of its own.
        float w = exp(-dot(offset, offset) / twoSigmaSq);

        // Coplanarity as a SMOOTH falloff rather than the hard cut this had. A hard cut makes the
        // kernel's total weight jump between neighbouring pixels, and a filter whose shape changes
        // abruptly does not look like a blur -- it looks like banding, which is what the earlier
        // version's discontinuous tap count produced.
        //
        // Skipped for a same-voxel tap: the far face of a voxel is a full edge length off this face's
        // plane by construction, so the falloff would weight it to nothing and the faces would never
        // pool.
        if (!sameVoxel) {
            float planeDistance = dot(tapPos.xyz - centreP, centreN) / planeSigma;
            w *= exp(-planeDistance * planeDistance);
        }

        sum += texture2D(gDirect, tapUV).a * w;
        weight += w;
    }
    // A pixel whose whole neighbourhood was rejected keeps its own sharp value, which is the correct
    // degenerate case: a face with no coplanar neighbours has nothing to soften against, and a
    // sub-pixel one should stay sharp anyway.
    return weight > 1e-5 ? sum / weight : texture2D(gDirect, uv).a;
}

// Depth/normal-guided bilateral blur of R. This is the LAST line of defence for a pixel with no
// history -- a freshly disoccluded one, or a silhouette that straddles two surfaces and so can never
// hold history at all. probefilter.sc has already smoothed the probe buffer in world space; this
// catches what is left at screen resolution.
//
// Radius 2 (5x5), raised from 1. A 3x3 at age 1 is averaging nine samples of an 8-ray estimate,
// which is not enough to hide it, and disocclusions were taking visibly too long to settle as a
// result. The cost is paid only where it is needed -- see the age guide below.
#define FILTER_RADIUS 2
// Age-guided a-trous spread. accumR.a carries the temporal age. Where history is thin (freshly
// disoccluded, age near 1) the temporal filter has not kicked in yet and the pixel would show the
// raw undersampled GI, so the taps are SPREAD wider and pulled back in as it converges. Tap count is
// unchanged, so a converged pixel pays nothing extra -- at full age the stride is 1 and the 5x5
// collapses onto a neighbourhood that is already all the same value.
#define FILTER_MAX_STRIDE 4.0
#define FILTER_AGE_FULL   16.0

// The per-pixel path: what this pass did for every pixel before the faces had identities.
vec3 filteredIndirect(vec2 uv, vec3 P, vec3 N, float camDist, float voxelSize) {
    // abs: accumulate uses the sign to mark a face no probe has landed on yet.
    float age = abs(texture2D(accumR, uv).a);
    float t   = clamp(1.0 - (age - 1.0) / (FILTER_AGE_FULL - 1.0), 0.0, 1.0);

    // WIDENED WHERE THE GEOMETRY IS SUB-PIXEL, on top of the age guide.
    //
    // Age answers "has this pixel had time to converge". It does not answer "is there anything here a
    // spatial filter would destroy", and past the crossover the answer is no: many voxels fall inside
    // one pixel, so the indirect term genuinely IS an average over a neighbourhood and widening the
    // filter computes more of the same integral rather than smearing detail. Below the crossover this
    // is 0 and the filter behaves exactly as before, so nothing that is resolved gets touched.
    //
    // This is the deliberate trade: blur the far-field indirect term to buy temporal stability. It is
    // cheap to take because the term is low frequency by construction -- direct sun, emission and
    // albedo are all kept out of it upstream and stay per-pixel sharp.
    float pixelWorld  = max(camDist * (2.0 * tan(radians(FOV * 0.5))) / passTargetRes.y, 1e-6);
    float subPixel    = 1.0 - smoothstep(0.75, 2.0, voxelSize / pixelWorld);

    float stride = mix(1.0, FILTER_MAX_STRIDE, max(t, subPixel));

    vec3  acc  = vec3(0.0);
    float accW = 0.0;
    for (int dy = -FILTER_RADIUS; dy <= FILTER_RADIUS; dy++)
    for (int dx = -FILTER_RADIUS; dx <= FILTER_RADIUS; dx++) {
        // POINT-SAMPLED, and this is the bug that drew a square of wrong indirect light around every
        // voxel-to-sky edge in the frame.
        //
        // `stride` is a lerp, so it is FRACTIONAL -- 2.7, not 3 -- and these taps therefore landed
        // between texel centres. Every render target here is created with BGFX_TEXTURE_RT and no
        // BGFX_SAMPLER_POINT (manage_resources.cpp), so it bilinear-filters by default, and a
        // fractional tap does not read a neighbouring texel: it reads a BLEND of four, for both the
        // G-buffer that decides the weight and the accumR that supplies the colour.
        //
        // Interpolating a G-buffer is meaningless in the specific way taa.frag's own snap warns about.
        // Halfway between a sky texel (camDist -1) and a surface texel at 200 is a POSITIVE depth
        // belonging to no surface, so geomWeight's `probeValid < 0` sky rejection never fires and the
        // tap is admitted -- carrying a world position interpolated toward the sky's 1e5-unit
        // placeholder point, and an accumR colour blended with the sky's zero indirect. The admitted
        // taps then drag the weighted mean toward black.
        //
        // It shows up as a SQUARE because that is the filter's footprint, and at sky edges because
        // that is where the -1/+depth discontinuity is. It is worst exactly where it is most visible:
        // stride widens as age falls, age is lowest along silhouettes, so the darkened block is
        // largest precisely at the edges that have it.
        //
        // Snapping to a texel centre keeps every value one the G-buffer actually wrote. The taps stay
        // distinct after snapping (a stride of 2.7 gives offsets 0, 3, 5), so the a-trous spread is
        // unchanged.
        vec2 sUV = pjvSnapToTexel(uv + vec2(float(dx), float(dy)) * passTargetRes.zw * stride,
                                  passTargetRes.xy);
        vec4 sgp = texture2D(gPos, sUV);
        // Sky rejected up front rather than left to geomWeight's early-out. Two reasons beyond
        // clarity: gNormal is (0,0,0) on a sky texel and `normalize` of that is NaN, which is a value
        // this function should never construct even where it is currently unused; and the accumulation
        // below is a MULTIPLY, where a zero weight does not neutralise a bad tap -- NaN * 0.0 and
        // Inf * 0.0 are both NaN, so one poisoned texel would take the whole neighbourhood with it.
        // That is the failure mode this filter is shaped to invite, and skipping is what forecloses it.
        if (sgp.a < 0.0) continue;

        vec3 sN  = normalize(texture2D(gNormal, sUV).xyz);
        float w  = geomWeight(P, N, sgp.xyz, sN, sgp.a);
        if (w <= 0.0) continue;                 // as probefilter.frag does, and for the same reason

        acc  += texture2D(accumR, sUV).rgb * w;
        accW += w;
    }
    // accW is at least the centre tap's 1.0 in practice, so this is a formality -- but it is the right
    // formality: no history, rather than a fabricated value.
    return accW > 1e-4 ? acc / accW : texture2D(accumR, uv).rgb;
}

void main() {
    vec2 uv = v_texcoord0;
    vec4 gp = texture2D(gPos, uv);
    // gDirect.rgb is EMISSION on a surface and the sky colour on the background; its alpha is the
    // sun's binary visibility. The lit sun term is rebuilt below, after the visibility is blurred.
    vec3 emission = texture2D(gDirect, uv).rgb;

    // Sky / background: no indirect term at all, gDirect holds the sky colour.
    //
    // FOGGED TOO, and this is what actually removes the line along the horizon. The sky used to be
    // returned untouched while everything else faded toward the haze, so the two met at the skyline
    // as two different colours and the join was a hard edge -- worst exactly where a scene's terrain
    // ends and the background shows through, which is the silhouette the eye is already drawn to.
    //
    // Fogging it is not a patch, it is the missing half of the model: the sky is seen THROUGH the
    // same air as everything else, and a height-fog layer is deep along a horizontal line of sight
    // and shallow straight up. The closed-form integral gives that for free -- with dir.y > 0 the
    // optical depth converges to d * atEye * H / dir.y as the distance runs to infinity, so the
    // zenith keeps a touch of haze and the horizon saturates completely. Distant terrain and the sky
    // above it therefore converge to the SAME value, and there is nothing left at the join to see.
    if (gp.a < 0.0) {
        // gPos.xyz for a background pixel is `origin + direction * 1e5`, so the view direction comes
        // straight back out of it and the distance is FOG_SKY_DISTANCE by construction.
        vec3 skyDir = normalize(gp.xyz - cameraPos.xyz);
        // Unfogged with a debug mode on, or the marker gbuffer.frag wrote here is erased. See animDebug.
        gl_FragData[0] = composeDebugMode() != 0
                       ? vec4(emission, 1.0)
                       : vec4(applyFog(emission, skyDir, FOG_SKY_DISTANCE), 1.0);
        return;
    }

    vec3 P = gp.xyz;
    vec3 N = normalize(texture2D(gNormal, uv).xyz);
    vec3 albedo = texture2D(gAlbedo, uv).rgb;
    float voxelSize = texture2D(gNormal, uv).a;

    // Toward the surface, and back toward the eye. Both from the G-buffer position rather than from a
    // reconstructed camera ray, so they are exact at any render scale and cost one normalize.
    vec3 viewDir = normalize(P - cameraPos.xyz);
    vec3 V = -viewDir;

    // Contact shading for the ambient terms. Deterministic, so it costs the temporal filters nothing.
    float ao = ambientOcclusion(uv, P, N, gp.a, voxelSize);

    // Sharp shading, soft shadow. The blur is applied to the visibility alone and the per-pixel
    // (albedo/PI) * NdotL is applied after it, so a voxel face keeps its exact lit colour and only
    // the boundary between lit and unlit is soft. This is the same expression gbuffer.frag used to
    // evaluate whole; it is split so that the two halves can be filtered differently.
    float sunVisibility = blurredSunVisibility(uv, P, N, texture2D(gNormal, uv).a);

    // The cosine factor, and the one place the direct sun stops being per face. See perVoxelNdotL for
    // why a voxel has to pick one number out of its six faces and which one it picks.
    float cosineTerm = PJV_PER_VOXEL_LIGHTING ? perVoxelNdotL(SUN_DIR)
                                              : max(dot(N, SUN_DIR), 0.0);

    // The sheen is DROPPED in per-voxel mode rather than flattened. It is a specular lobe, so it is a
    // statement about how one face is oriented relative to the eye -- there is no version of it that
    // is both a highlight and the same on all six faces, and averaging it over the faces would leave a
    // dull uniform gloss that is neither. A voxel lit as a unit simply has no highlight.
    vec3 sheen = PJV_PER_VOXEL_LIGHTING ? vec3(0.0) : sunSheen(N, V, sunVisibility);

    vec3  direct = emission +
                   (albedo / PI) * cosineTerm * SUN_COLOR * sunVisibility +
                   sheen;

    // ---- Foliage translucency -------------------------------------------------------------------
    // gAlbedo.a is the motion class gbuffer.frag wrote for the temporal filters: 0 stationary,
    // 0.5 displaced, 1.0 advected. Displaced IS the vegetation set -- see the note on the effect --
    // so the test is one compare against a channel that was already being fetched, and everything
    // below it is skipped entirely for stone, bark, dirt and fire.
    //
    // ADDED TO `direct` rather than to the indirect term, deliberately. It is sunlight arriving by a
    // known path, it is per-pixel, and it is noise-free -- so it belongs on the side of the split
    // that stays full-resolution and out of the temporal filters, exactly like the sun and the
    // emission it sits next to. Putting it in the indirect term would hand a sharp, view-dependent
    // signal to a filter built to smear a low-frequency one.
    float motionClass = texture2D(gAlbedo, uv).a;
    if (debugParams.z < 0.5 && abs(motionClass - 0.5) < 0.25) {
        // The face this pixel is standing on, for the edge gradient. Snapped for the usual reason: a
        // bilinear read of gFace between two faces returns a centre that is on neither of them, which
        // here would put the rim of the gradient somewhere inside the voxel.
        vec4 gf = texture2D(gFace, pjvSnapToTexel(uv, passTargetRes.xy));
        direct += foliageTransmission(N, V, albedo, foliageNeighbourhood(uv, gp.a), sunVisibility,
                                      P, gf.xyz, gf.w);
    }

    // ---- PROTOTYPE clean view ------------------------------------------------------------------
    // The indirect term is one bounce per pixel per frame, and it is only watchable because
    // accumulate averages 64 frames of it. The sway forces every frame to count as moving (an
    // animated scene never converges -- docs/plans/animation.md accepts this and defers it to a
    // per-face cache), so that mean never builds and the GI arrives as raw 1-spp noise. Which is
    // exactly the wrong thing to be looking through while judging whether the VOXELS are moving
    // correctly.
    //
    // So: swap it for a flat hemispherical ambient. Deterministic, free, and it still shades by
    // orientation so a blade's faces stay distinguishable. It used to be paired with a hard sun in
    // main.cpp, because the soft shadow was the other 1-spp term; that pairing is gone, since taa.frag
    // now converges the shadow properly and the sun can stay soft.
    //
    // This is a diagnostic mode, not a lighting model. It is not what the renderer looks like. The
    // distance fog is skipped here for the same reason the GI is: this view exists to answer whether
    // the VOXELS are moving correctly, and fading the far field toward the sky is the one thing that
    // would make that harder to see.
    if (animCleanView()) {
        vec3 ambient = mix(vec3(0.10, 0.10, 0.09), vec3(0.34, 0.40, 0.52), N.y * 0.5 + 0.5);
        gl_FragData[0] = vec4(direct + albedo * ambient * ao, 1.0);
        return;
    }

    // ---- The indirect term ------------------------------------------------------------------
    // What arrives in accumR is now already flat across a voxel face and already converged: probe.frag
    // gathers per face with directions seeded by the face key, and accumulate averages that per face
    // over a long history. So this filter is no longer doing the denoising -- it is a small cleanup
    // for the pixels the per-face path cannot serve yet, and its age-guided stride means it widens
    // only where history is thin (a freshly disoccluded face) and collapses to almost nothing
    // everywhere else. On a converged frame it is close to a pass-through.
    vec3 R = filteredIndirect(uv, P, N, gp.a, voxelSize);

    // Direct light and albedo stay full-resolution and per-pixel; only the diffuse indirect is
    // low frequency. That is the same bet the demodulation upstream makes -- indirect light varies
    // slowly, so smoothing it costs nothing you can see, while smoothing albedo or a shadow edge
    // would be obvious immediately.
    // ---- GI DEBUG VIEW (P key, renderParams.x) --------------------------------------------------
    // The temporal age of the indirect term, as a ramp. See FrameState::giDebugView in main.cpp for
    // why this is worth a key: the indirect term is a few Monte Carlo rays a frame and is only
    // watchable because it is averaged over a long history, so every bug that breaks that average
    // looks the same from the composite -- noise -- whatever caused it. This picture separates them.
    //
    // Read it as: black = age 1, no history is being reused here at all. Gold = the animated cap.
    // White = STILL_MAX_AGE, fully converged. Large black regions are a fault; black threads along
    // silhouettes are the edge tier failing; black on foliage is the animated gate failing.
    if (renderParams.x > 0.5 && renderParams.x < 1.5) {
        float t = clamp(abs(texture2D(accumR, uv).a) / 128.0, 0.0, 1.0);
        gl_FragData[0] = vec4(clamp(t * 4.0, 0.0, 1.0),
                              clamp(t * 2.0, 0.0, 1.0),
                              clamp((t - 0.5) * 2.0, 0.0, 1.0), 1.0);
        return;
    }

    // ---- AO DEBUG VIEW (P key, renderParams.x == 3) ---------------------------------------------
    // The occlusion term alone, as greyscale. Worth a key for the same reason the age view is: AO is
    // multiplied into a term that is already dim and already filtered, so a radius that is far too
    // large or a bias that is self-shadowing looks, from the composite, like slightly wrong GI. Here
    // it is the only thing on screen. White = unoccluded, black = fully occluded; a flat surface with
    // nothing near it should be pure white, and grey on one means AO_BIAS is too low.
    if (renderParams.x > 2.5 && renderParams.x < 3.5) {
        gl_FragData[0] = vec4(ao, ao, ao, 1.0);
        return;
    }

    // AO on the indirect term alone. `direct` carries the sun, whose visibility was traced per pixel
    // and blurred above, and the emission, which is the surface's own light -- neither is an ambient
    // quantity and occluding them would be double-counting at best.
    // GI off leaves the direct sun and a flat ambient in its place, so the image stays readable
    // rather than going black in shadow -- the question the toggle answers is what the indirect
    // bounce is contributing, and that is only visible against something.
    vec3 indirect = (featureToggles.x > 0.5)
                  ? R
                  : vec3(0.10, 0.11, 0.13);
    vec3 hdr = direct + albedo * indirect * ao;

    // Fog LAST, over the assembled radiance, because that is where it belongs physically: everything
    // above describes light leaving the surface, and this is what happens to it on the way here. It is
    // also why the sky path returned early and unfogged -- a sky pixel is at infinity, and the colour
    // the fog would fade it toward is the same sky it already is.
    //
    // Ahead of taa rather than in display.frag, so a fogged silhouette is anti-aliased against the fog
    // it sits in rather than having a hard edge tinted after the fact.
    int dbg = composeDebugMode();

    // ---- MODE 4: THE FOG DISTANCE ITSELF -----------------------------------------------------
    // `gp.a` is camDist, and it is the ONLY thing that decides how much of a surface survives the
    // fog. A pixel whose distance comes back far too large saturates to the inscatter colour while
    // its neighbours do not, which on screen is a sky-coloured hole in otherwise solid geometry --
    // no missing geometry, no failed shadow, just one number too big. Banded on three scales so a
    // wrong distance breaks the pattern instead of blending into it: a hole that is a distance bug
    // shows up here as a patch that does not belong to the gradient around it.
    if (dbg == 4) {
        gl_FragData[0] = vec4(fract(gp.a / 64.0), fract(gp.a / 512.0), fract(gp.a / 4096.0), 1.0);
        return;
    }

    // Fog LAST -- but not at all with a debug mode on, so a marker written upstream survives to the
    // screen. See animDebug in pjv_anim_controls.sc.
    if (dbg == 0) hdr = applyFog(hdr, viewDir, gp.a);

    gl_FragData[0] = vec4(hdr, 1.0);
}
