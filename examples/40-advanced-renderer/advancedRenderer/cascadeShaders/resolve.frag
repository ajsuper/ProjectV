$input v_color0
$input v_texcoord0

// =============================================================================
// resolve.frag  --  Pass 3. Quarter-res probe gather -> full-res indirect irradiance.
//
// probe.frag computed the cosine-weighted mean radiance R for one voxel face per quarter-res texel.
// This pass gives every full-res pixel the R belonging to ITS face.
//
// -----------------------------------------------------------------------------
// THIS IS A LOOKUP, NOT AN INTERPOLATION
// -----------------------------------------------------------------------------
// That is the whole difference from the version this replaces. The old resolve bilinearly blended
// the four nearest cascade-0 probes, bilaterally weighted, because a probe's value belonged to
// wherever the probe happened to STAND and the pixel had to reconstruct its own value from
// neighbours. Its own comments record what that cost: "a viewpoint-dependent answer to a
// viewpoint-independent question", lighting that changed as you flew around, and two failed
// attempts to special-case a pixel that landed exactly on a probe.
//
// R is now a property of the FACE, not of the probe position -- probe.frag seeds its ray directions
// from the face key, so every texel that gathered for this face computed the identical number. So
// there is nothing to interpolate. This pass searches a small neighbourhood for a texel that
// gathered for THIS pixel's face and takes its value whole. Any match is as good as any other and
// the first one found wins.
//
// The bilateral blend survives only as the bootstrap for a face that no texel gathered for this
// frame -- a face thinner than the 4-pixel probe lattice, or one that has just come around a
// silhouette. Those pixels get a plausible number now and their own exact value within a frame or
// two, and accumulate.frag will not let a blended sample overwrite a converged face.
//
// Inputs (FBO 1 [0..5] then FBO 2 [6]):
//   0 gPos  1 gNormal  2 gAlbedo  3 gDirect  4 gFace  5 gKey  6 probeR
// Output (FBO 6): rgb = indirect irradiance R for this pixel, a = 1 when it came from a texel on
// this pixel's own face and 0 when it had to be blended. accumulate does not read the flag today;
// it is written because it is the one number that says whether this pixel is being told the truth,
// and it costs a channel that is already there.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gPos,    0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gKey,    5);
SAMPLER2D(probeR,  6);

// w = PER-VOXEL LIGHTING (U), read through PJV_PER_VOXEL_LIGHTING in pjv_face_key.sc. It decides
// which probe samples this pixel is allowed to treat as estimates of its own quantity.
uniform vec4 debugParams;

// This pass writes at screen resolution and reads the quarter-size probe buffer, so the screen grid
// is its own target and the probe grid is input 6.
#define CASCADE_SCREEN_RES passTargetRes.xy
#define CASCADE_ATLAS_RES  pjvResOr(passInputRes[6].xy, passTargetRes.xy)
#include <pjv_cascade_common.sc>
#include <pjv_face_key.sc>

#define GI_STRENGTH 1.0

// Probe texels searched around this pixel, as a radius in PROBE texels. 1 = a 3x3 window, which at
// quarter resolution reaches 6 screen pixels in each direction. Wide enough that a face at least a
// few pixels across nearly always contains a probe anchor; going wider mostly buys taps on faces
// that were never going to match.
#define SEARCH_RADIUS 1

// Where a probe texel sampled the G-buffer. probe.frag point-samples the screen grid at its own
// texel centre, and this MUST reproduce that expression exactly -- a half-texel disagreement reads a
// neighbouring face's key, and the exact match then either misses (the face falls back to a blend
// forever) or, worse, hits on the wrong face.
vec2 probeAnchorUV(ivec2 probeTexel) {
    vec2 centre = (vec2(probeTexel) + 0.5) / CASCADE_ATLAS_RES;
    return pjvSnapToTexel(centre, CASCADE_SCREEN_RES);
}

void main() {
    vec2 uv = v_texcoord0;
    vec4 gp = texture2D(gPos, uv);

    // Sky / background carries no indirect; display shows gDirect (the sky colour) for these pixels.
    if (gp.a < 0.0) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    vec3 P = gp.xyz;
    vec3 N = normalize(texture2D(gNormal, uv).xyz);
    vec4 key = texture2D(gKey, pjvSnapToTexel(uv, CASCADE_SCREEN_RES));

    // The probe texel this pixel sits on, and the window around it.
    ivec2 baseTexel = ivec2(floor(uv * CASCADE_ATLAS_RES));
    ivec2 maxTexel  = ivec2(CASCADE_ATLAS_RES) - 1;

    // EXACT matches are averaged, not raced. Every texel that gathered for this face is an
    // INDEPENDENT estimate of the same quantity -- probe.frag gives each texel its own offset into
    // the face's direction sequence for exactly this reason -- so averaging the matches multiplies
    // the effective ray count by however many of them there are. On a face a few pixels across that
    // is nine texels' worth of rays in one frame instead of one texel's, and it is the difference
    // between a face that visibly pulses and one that sits still. Taking the first match instead (an
    // earlier version of this loop did) throws eight ninths of the budget away.
    vec3  exactSum   = vec3(0.0);
    float exactCount = 0.0;

    vec3  blend  = vec3(0.0);
    float blendW = 0.0;

    // ---- TWO PROGRESSIVELY LOOSER TIERS BEHIND THE BLEND ----------------------------------------
    // These exist because the strict blend's failure was black, and black is not a neutral answer --
    // it is the darkest value in the range, applied to a whole probe cell, and it reads as a hole.
    //
    // hemi: any probe on a surface facing the same hemisphere, with the ^4 corner cutoff and the
    //       coplanarity term both dropped. Wrong in detail, right in magnitude.
    // near: any probe that gathered anything at all. The last thing between this pixel and a hole.
    vec3  hemi  = vec3(0.0); float hemiW  = 0.0;
    vec3  near  = vec3(0.0); float nearW  = 0.0;

    for (int dy = -SEARCH_RADIUS; dy <= SEARCH_RADIUS; dy++)
    for (int dx = -SEARCH_RADIUS; dx <= SEARCH_RADIUS; dx++) {
        ivec2 t = clamp(baseTexel + ivec2(dx, dy), ivec2(0, 0), maxTexel);
        vec2  tUV = (vec2(t) + 0.5) / CASCADE_ATLAS_RES;

        vec4 r = texture2D(probeR, tUV);
        if (r.a < 0.5) continue;                 // that texel landed on sky -- it gathered nothing

        vec2 aUV = probeAnchorUV(t);
        vec4 tKey = texture2D(gKey, aUV);

        // An EXACT match means "this sample is an estimate of the same quantity I want". Which
        // samples qualify is exactly what the per-voxel mode changes: off, only a probe standing on
        // this same face; on, any probe standing anywhere on this same voxel, because in that mode
        // every face of a voxel integrates the identical sphere (see pjvProbeGather) and so all six
        // are independent estimates of one number.
        //
        // Averaging them is therefore not a loosening of the test -- it is the same argument the
        // exact tier already rests on, applied to a coarser quantity, and it multiplies the effective
        // ray count by up to six on top of what the several texels per face already give.
        bool sameSurface = PJV_PER_VOXEL_LIGHTING ? pjvSameVoxel(tKey, key)
                                                  : pjvSameFace(tKey, key);
        if (sameSurface) {
            exactSum   += r.rgb;
            exactCount += 1.0;
            continue;                            // never also a candidate for the bootstrap blend
        }

        // Not this face. It is a candidate for the bootstrap blend, weighted by how well its surface
        // matches this pixel's so radiance does not leak across a silhouette or an inner corner.
        vec4  tgp = texture2D(gPos, aUV);
        // The probe said it hit something, so its anchor should be a surface -- but read the G-buffer
        // rather than trust that, because `normalize` of a sky texel's (0,0,0) normal is NaN and one
        // of those would poison every tier below through the dot products.
        if (tgp.a < 0.0) continue;
        vec3  tN  = normalize(texture2D(gNormal, aUV).xyz);

        // Prefer near taps among equally plausible ones, so the bootstrap varies smoothly across a
        // face rather than snapping as the window slides onto a different neighbour.
        float falloff = 1.0 / (1.0 + float(dx * dx + dy * dy));

        float w = geomWeight(P, N, tgp.xyz, tN, tgp.a) * falloff;
        if (w > 0.0) { blend += r.rgb * w; blendW += w; }

        float hw = max(dot(N, tN), 0.0) * falloff;
        if (hw > 0.0) { hemi += r.rgb * hw; hemiW += hw; }

        near  += r.rgb * falloff;
        nearW += falloff;
    }

    if (exactCount > 0.0) {
        gl_FragData[0] = vec4((exactSum / exactCount) * GI_STRENGTH, 1.0);
        return;
    }

    // ---- NO TEXEL GATHERED FOR THIS FACE --------------------------------------------------------
    // This used to fall to the geometry-weighted blend and then, if that found nothing, to BLACK --
    // on the reasoning that accumulate treats a fresh sample as low-confidence and "the face will own
    // a real one shortly". The second half of that is what does not hold, and it is the black squares
    // along every voxel-to-sky edge.
    //
    // A face only ever owns a probe if a probe ANCHOR lands on it, and anchors sit on a quarter-res
    // lattice -- one per 4 screen pixels. A face thinner than that lattice is not waiting for its
    // turn, it can never be gathered for at all, so "shortly" is never and the black is permanent.
    // Shrinking the window makes every face thinner in pixels and pushes more of them under the
    // lattice, which is exactly why this appears only at low window sizes.
    //
    // At a sky silhouette the strict blend then fails too, and for a reason that has nothing to do
    // with the light: the surviving neighbours are sky texels (skipped above) or the perpendicular
    // side faces of the same voxel, and geomWeight's ^4 corner cutoff sends those to zero by design.
    // So every tap is rejected, and a whole probe cell writes zero -- a black square, at the exact
    // places the geometry is most detailed.
    //
    // The tiers below never invent a value; each one widens what counts as a plausible neighbour, and
    // the alpha stays 0 throughout so accumulate still treats all of this as the low-confidence
    // bootstrap it is and will not let it overwrite a converged face. Some irradiance from a
    // perpendicular face is a small error in a term that is already low-frequency and about to be
    // averaged; a black hole is a large one that nothing downstream can undo.
    vec3 indirect = blendW > 1e-4 ? blend / blendW
                  : hemiW  > 1e-4 ? hemi  / hemiW
                  : nearW  > 1e-4 ? near  / nearW
                  : vec3(0.0);   // genuinely nothing within reach: an isolated sliver against sky
    gl_FragData[0] = vec4(indirect * GI_STRENGTH, 0.0);
}
