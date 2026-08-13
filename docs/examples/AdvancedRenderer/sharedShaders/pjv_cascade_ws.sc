// =============================================================================
// pjv_cascade_ws.sc  --  WORLD-SPACE radiance-cascade gather + merge.
//
// The radiance-cascade structure -- full-res atlas, probe = a Dc x Dc direction tile, top-down
// merge, geometrically growing intervals, octahedral directions -- with a gather that traces a
// REAL voxel-DDA ray through the scene instead of marching the depth buffer.
//
// That is the whole point of the world-space variant: a screen-space ray cannot see off-screen
// geometry, so it cannot tell "blocked" from "sees sky" and defaults to sky -> a flat sky over
// everything. A world-space ray either hits a voxel (correctly occluded, and picking up that
// voxel's bounced sunlight and its own emission) or escapes the scene entirely to the true sky,
// so sky occlusion and sky shadows come out right.
//
// Probes are still anchored to the screen G-buffer (a probe's world position + normal
// come from gPos/gNormal at its tile-centre pixel). Only the RAYS and occlusion are
// world-space. A fully volumetric (view-independent) probe grid would be a much larger
// change in this 2D-texture pass framework and buys little for a primary-view diffuse GI.
//
// Each thin cascadeN.frag declares gPos/gNormal (+ upperCascade unless top), includes the
// voxel DDA (pjv_utils_DDA.sc) and then this file, sets CASCADE_INDEX (+ IS_TOP for the
// last cascade), and writes computeCascadeTexel().
//
// Requires (declared/included by the thin file BEFORE this include):
//   SAMPLER2D gPos, gNormal, gFace (+ upperCascade unless IS_TOP)
//   #include <pjv_utils_DDA.sc>   (Ray/RayQuery/raySceneIntersect/materials + the scene samplers)
// pjv_cascade_common.sc (below) brings the cascade math, constants and pjv_sun_sky.
// =============================================================================

#include <pjv_cascade_common.sc>

// World-space DDA gather tuning (WS-only; kept out of the shared common header). Each cascade
// texel casts one gather ray (+ a sun shadow ray on hit), so these are the main perf knobs.
#define WS_STEPS       48u   // Max DDA steps per gather ray (bounds cost; interval-clamped).
#define WS_FINISH_LOD  1     // Coarsen distant geometry so the longer far-cascade rays stay cheap.
#define WS_LOD_DIST    45    // Voxels from the ray start before finishLOD kicks in.

// Gather-origin lift (see gatherRayWS). Instead of skipping near hits by distance
// (which could not tell a coplanar self-relief hit from a real perpendicular wall -> either a dark
// streak or a light leak), lift the ray origin this many VOXELS off the face along N. A near-tangent
// ray then skims ABOVE the wall's own coplanar relief (clearing the false self-occlusion streak with no
// per-hit test), while a perpendicular wall standing on the surface still rises into the lifted ray and
// blocks it (no leak). ~0.5 (half a voxel) clears flat-wall relief; larger risks skimming over genuine
// low occluders / softening contact darkening. 0 falls back to NORMAL_BIAS.
//
// This matters more with face-centre origins than it would with per-pixel ones: every probe on a
// face now starts from the SAME point, so a false self-hit is not a speckle on one pixel, it is the
// whole face going dark at once. The lift is what stops that being a visible failure mode.
#define ORIGIN_LIFT 0.5

// ---- Bounce sun-shadow perf knobs (the dominant per-gather-hit cost) -----------------------------
// The bounce sun-shadow ray fires on EVERY gather hit, so it is the largest DDA cost in the renderer.
// SUN_SHADOW_STEPS bounds its length: FEWER steps is SAFE -- a ray that runs out of steps returns a
// MISS (== lit), so shortening it errs toward slight over-LIGHTING of long-shadowed nooks, never the
// over-DARKENING that a coarse LOD would cause (which is why the LOD stays finest). 48 keeps local
// contact shadows; only shadows cast from >~48 voxels away soften.
#define SUN_SHADOW_STEPS 48u
// Cascades with index <= this get a real shadowed bounce; FARTHER cascades (bigger, most gather hits)
// use the UNSHADOWED bounce -- no shadow ray at all. Their bounce is a coarse far-field contribution
// where a missing self-shadow is barely visible, and skipping it removes the most shadow rays. 1 =
// c0,c1 shadowed / c2,c3 unshadowed. Raise to 3 to shadow every cascade (old behaviour); lower to -1
// to never shadow a bounce.
#define SUN_BOUNCE_SHADOW_MAX_CASCADE 1

// Hard sun shadow ray from a bounce point (world space). Visibility only. FINEST LOD (finishLOD 0) on
// purpose: a coarse-LOD shadow ray tests against big merged boxes that over-occlude -> false shadow ->
// a dim bounce (the #1 cause of a dim world-space bounce). We cut cost via step count, not LOD.
bool sunVisibleWS(vec3 p, vec3 n) {
    Ray shadow;
    shadow.origin    = p + n * (0.02 * WORLD_SCALE);
    shadow.direction = SUN_DIR;
    RayQuery rq;
    rq.maxRaySteps = SUN_SHADOW_STEPS;
    rq.startLOD = 0u; rq.finishLOD = 0u; rq.distanceToFinishLOD = 100000u;
    return raySceneIntersect(shadow, rq).rayT < 0.0;   // rayT < 0 == genuine miss == lit
}

// Unshadowed bounced sunlight (no shadow ray) -- used on the far cascades.
vec3 directSunNoShadowWS(vec3 p, vec3 n, vec3 albedo) {
    float NdotL = dot(n, SUN_DIR);
    if (NdotL <= 0.0) return vec3(0.0);
    return (albedo / PI) * NdotL * SUN_COLOR;
}

// Bounced sunlight leaving a diffuse surface (albedo/PI * NdotL * sun). c selects whether a real
// shadow ray is cast (near cascades) or skipped (far cascades) per SUN_BOUNCE_SHADOW_MAX_CASCADE.
vec3 directSunWS(vec3 p, vec3 n, vec3 albedo, int c) {
    float NdotL = dot(n, SUN_DIR);
    if (NdotL <= 0.0) return vec3(0.0);
    if (c > SUN_BOUNCE_SHADOW_MAX_CASCADE) return (albedo / PI) * NdotL * SUN_COLOR;  // unshadowed
    if (!sunVisibleWS(p, n)) return vec3(0.0);
    return (albedo / PI) * NdotL * SUN_COLOR;
}

// ---- World-space gather ray -------------------------------------------------
// Trace cascade c's interval [start_c, start_c+len_c] from probe (P,N) along dir as a real
// voxel ray. Returns rgb = radiance found on a hit (0 if none), a = visibility:
//   a = 0  -> hit a voxel inside this interval (opaque; do NOT read the upper cascade)
//   a = 1  -> passed clean through the interval (transparent; upper cascade / sky continues)
// vs = probe's voxel edge size (0 disables the grazing-relief lift; screen-anchored probes pass 0).
vec4 gatherRayWS(vec3 P, vec3 N, vec3 dir, int c, float vs) {
    float t0  = cascadeStart(c);
    float len = cascadeLen(c);

    // Lift the origin ~half a voxel off the face (see ORIGIN_LIFT) so near-tangent rays skim above the
    // wall's own coplanar relief while perpendicular walls still block -- fixes streak AND leak without
    // a per-hit test. vs = 0 (this renderer) -> just NORMAL_BIAS.
    float lift = (vs > 0.0) ? max(ORIGIN_LIFT * vs, NORMAL_BIAS) : NORMAL_BIAS;

    Ray ray;
    ray.origin    = P + N * lift + dir * t0;          // start at this interval's near end, lifted off N
    ray.direction = dir;

    RayQuery rq;
    rq.maxRaySteps         = WS_STEPS;
    rq.startLOD            = 0u;
    rq.finishLOD           = uint(WS_FINISH_LOD);     // coarse far -> long rays stay cheap
    rq.distanceToFinishLOD = uint(WS_LOD_DIST);

    SceneIntersectData h = raySceneIntersect(ray, rq);

    // Note: the old distance-based grazing self-relief skip is gone -- the ORIGIN_LIFT above clears the
    // coplanar self-relief geometrically (a lifted near-tangent ray skims over it), so near hits are now
    // trusted as real occluders. That is what stops a perpendicular thin wall being skipped (the leak).

    // Hit, and within this cascade's interval? (h.rayT is measured from ray.origin, i.e.
    // from the interval's near end, so compare against the interval length.)
    if (h.rayT >= 0.0 && h.foundBox.size >= 0.0 && h.rayT <= len) {
        // Degenerate hit: for the higher cascades the ray starts start_c world units out
        // (c3 ~12.6), which frequently lands INSIDE solid geometry. The DDA then reports a
        // hit with a zero normal ("started inside the volume") -> normalize(0) = NaN, which
        // propagates through the merge as black/NaN squares at this cascade's tile size
        // (c3 tile = 32px -> ~30px black squares). Treat it as simply blocked: opaque, no
        // light (no valid bounce normal to shade with).
        if (dot(h.normal, h.normal) < 0.5) {
            // Started inside solid (no valid hit normal). Usually a FALSE grazing self-hit --
            // the ray origin grazed into the surface's own relief (worsened when many probes
            // share a snapped origin). Stamping opaque black here produced coherent black bands;
            // treat it as transparent instead so the texel reads the merged upper cascade / sky
            // (a false near-occlusion should pass light, not paint black).
            return vec4(0.0, 0.0, 0.0, 1.0);              // transparent -> continue to merge/sky
        }
        vec3  hP = ray.origin + dir * h.rayT;
        vec3  hN = normalize(h.normal);
        // Straight off the march's own hit: two texelFetches against the material list index and
        // palette offset the DDA carried out, instead of re-fetching the chunk header and
        // re-descending the tree to the leaf it was already standing on. It is also the only
        // correct lookup once a chunk has been moved or rotated -- the older
        // fetchVoxelColor(foundBox, headerIndex) rebuilds an integer voxel coordinate out of a
        // world-space box, and that float32 round trip mis-shades a percentage of voxels with
        // their neighbour's colour that grows with the chunk's distance from the origin. See
        // SceneIntersectData::materialListIndex.
        VoxelMaterial m = fetchVoxelMaterialFromHit(h);
        // A metal has no diffuse lobe, so it bounces nothing on its own account; what it does
        // contribute is its emission. Same subtraction the editor's preview and Render mode make.
        vec3 v = m.albedo * (1.0 - m.metallic);
        // Emission enters the cascade here, which is what makes an emissive voxel an actual GI
        // light source rather than just a bright pixel: the gather ray that lands on it carries
        // its glow back to the probe, the merge spreads it up the cascade, and resolve integrates
        // it into the irradiance of every surface that can see it. Unshadowed and un-cosine'd,
        // because emission leaves a surface regardless of where the sun is.
        return vec4(directSunWS(hP, hN, v, c) + m.emission, 0.0);
    }
    return vec4(0.0, 0.0, 0.0, 1.0);                       // passed through -> transparent
}

// ---- Merge: continue a transparent ray with the cascade above --------
// Bilinear over the four surrounding upper-cascade probes, but BILATERAL: each upper probe is
// weighted by how well its own surface (gPos/gNormal at its centre pixel) matches this probe's
// surface, so the merge doesn't pull radiance from a probe on a different face / behind an edge
// (that cross-surface pull is a major light-leak source). Falls back to plain bilinear if every
// neighbour is rejected (e.g. an isolated probe) so nothing goes black.
#ifndef IS_TOP
// Average the 4 child directions (the 2x2 sub-tile of parentTile) of one c+1 probe. upD is the
// c+1 direction-tile dim; the c+1 probe's directions live at pixels upProbe*upD + childTile.
vec3 upperProbeDir(ivec2 upProbe, ivec2 parentTile, int upD) {
    vec3 sum = vec3(0.0);
    for (int j = 0; j < 2; j++)
    for (int k = 0; k < 2; k++) {
        ivec2 childTile = parentTile * 2 + ivec2(k, j);
        ivec2 px = upProbe * upD + childTile;
        vec2 uv = (vec2(px) + 0.5) / CASCADE_ATLAS_RES;
        sum += texture2D(upperCascade, uv).rgb;
    }
    return sum * 0.25;
}
// Surface (pos, normal, valid) of a c+1 probe, read at its own screen anchor ((idx+0.5)*upS).
void upperProbeSurface(ivec2 upProbe, int upS, out vec3 sp, out vec3 sn, out float sv) {
    vec2 uv = pjvSnapToTexel((vec2(upProbe) + 0.5) * float(upS) / CASCADE_SCREEN_RES,
                             CASCADE_SCREEN_RES);
    vec4 gp = texture2D(gPos, uv);
    sp = gp.xyz; sv = gp.a;
    sn = normalize(texture2D(gNormal, uv).xyz);
}
// Continue a transparent ray with cascade c+1: bilateral-bilinear over the 4 surrounding c+1
// probes (weighted by surface match so it doesn't pull light across a different face), each
// contributing its 4 angular children of this probe's direction tile.
vec3 mergeUpper(ivec2 probe, ivec2 tile, int c, vec3 P, vec3 N) {
    int upD = dirTileOf(c + 1);
    int upS = probeSpacingOf(c + 1);
    ivec2 upProbesN = ivec2(CASCADE_SCREEN_RES) / upS;

    // This c probe's location in c+1 probe-index space (c+1 spacing is 2x this cascade's).
    vec2 gp = (vec2(probe) + 0.5) * 0.5 - 0.5;
    vec2 f  = fract(gp);
    ivec2 b = ivec2(floor(gp));

    ivec2 off[4];
    off[0] = ivec2(0, 0); off[1] = ivec2(1, 0);
    off[2] = ivec2(0, 1); off[3] = ivec2(1, 1);
    float bw[4];
    bw[0] = (1.0 - f.x) * (1.0 - f.y); bw[1] = f.x * (1.0 - f.y);
    bw[2] = (1.0 - f.x) * f.y;         bw[3] = f.x * f.y;

    vec3  acc = vec3(0.0);
    float accW = 0.0;
    vec3  accValid  = vec3(0.0);   // fallback over VALID (non-sky) upper probes only
    float accValidW = 0.0;
    for (int i = 0; i < 4; i++) {
        ivec2 upProbe = clamp(b + off[i], ivec2(0, 0), upProbesN - 1);
        vec3 sp, sn; float sv;
        upperProbeSurface(upProbe, upS, sp, sn, sv);
        vec3 r = upperProbeDir(upProbe, tile, upD);
        float w = bw[i] * geomWeight(P, N, sp, sn, sv);
        acc      += r * w;
        accW     += w;
        // Exclude sky/background upper probes (sv < 0) from the fallback so their 0 radiance never
        // leaks in when every neighbour is geometry-rejected (the pulsing black-patch source).
        float valid = sv >= 0.0 ? 1.0 : 0.0;
        accValid  += r * bw[i] * valid;
        accValidW += bw[i] * valid;
    }
    // Bilateral blend; else plain bilinear over VALID probes only; else nothing (never sky zeros).
    return accW > 1e-4 ? acc / accW
         : (accValidW > 1e-4 ? accValid / accValidW : vec3(0.0));
}
#endif

// ---- Per-texel entry point (identical structure to the screen-space version) -----------
vec3 computeCascadeTexel(ivec2 fragPx) {
    int c  = CASCADE_INDEX;
    int Dc = dirTileOf(c);       // direction-tile dim (Dc x Dc directions per probe)
    int sc = probeSpacingOf(c);  // probe spacing in screen px

    // The region this cascade fills is probesN * Dc = SCREEN * D0/s0 on each axis, the same for
    // every cascade because s and D both double. The atlas is DECLARED at exactly that size
    // (scale 0.25 in resources.json for s0=16, D0=4), so this test now trims only the ragged edge
    // left by integer division rather than fifteen sixteenths of the pass. Keep it: a screen
    // resolution that is not a multiple of s0 still leaves a sliver.
    ivec2 probesN  = ivec2(CASCADE_SCREEN_RES) / sc;
    ivec2 regionPx = probesN * Dc;
    if (fragPx.x >= regionPx.x || fragPx.y >= regionPx.y) {
        return vec3(0.0);
    }

    ivec2 probe = fragPx / Dc;
    ivec2 tile  = fragPx - probe * Dc;

    // Probe's world anchor: map the (sparse) probe grid back onto the full-res G-buffer, SNAPPED to
    // a texel centre so every channel read below is one real sample rather than a blend of
    // neighbours. See pjvSnapToTexel -- for gFace in particular the blend is what made the "face
    // centre" drift with the camera.
    vec2 probeUV = pjvSnapToTexel((vec2(probe) + 0.5) * float(sc) / CASCADE_SCREEN_RES,
                                  CASCADE_SCREEN_RES);
    vec4 gp = texture2D(gPos, probeUV);
    vec3 Praw = gp.xyz;                        // raw surface (used for bilateral merge weights)
    vec3 N    = normalize(texture2D(gNormal, probeUV).xyz);

    vec3 dir = dirForTile(tile, Dc, cascadeJitter());

    if (gp.a < 0.0 || dot(dir, N) <= 0.0) {
        return vec3(0.0);
    }

    // ---- The gather origin is the voxel FACE CENTRE ----------------------------------------
    // Not the pixel's own hit point, and not a snap of it to some arbitrary world grid. The face
    // centre is camera-independent and identical for every screen probe that lands on this face,
    // so all of them gather from ONE origin -- which is what makes this effectively one probe per
    // voxel face rather than one per probe-grid cell, and what lets PROBE_SPACING0 be coarse.
    //
    // It also replaces the world-grid snap this used to do. That was reaching for exactly this
    // property -- "sample the same world location each frame so the temporal pass re-finds a
    // converged cell instead of chasing a moving one" -- by quantizing to an arbitrary lattice
    // that had to be smaller than a voxel to avoid pushing the origin into a neighbouring solid
    // cell. The voxelisation ALREADY IS that lattice, and its cell centres are guaranteed to lie
    // on the surface. (The old snap had in any case been tuned down to 1e-14, below a float32
    // ULP, which made it an identity -- so nothing here regresses.)
    vec4  gf = texture2D(gFace, probeUV);
    vec3  P  = gf.xyz;
    float vs = gf.a;   // voxel edge size -> enables the grazing-relief lift below
    // A probe whose anchor pixel found no surface has no face to gather from.
    if (vs <= 0.0) return vec3(0.0);

    vec4 g = gatherRayWS(P, N, dir, c, vs);   // rgb = hit radiance, a = visibility

#ifdef IS_TOP
    #if SKY_GI
    vec3 upper = skyGradient(dir);        // escaped the whole scene -> true sky
    #else
    vec3 upper = vec3(0.0);
    #endif
#else
    vec3 upper = mergeUpper(probe, tile, c, Praw, N);
#endif

    vec3 result = g.rgb + g.a * upper;
    // Defensive: never let a NaN/Inf or negative escape into the atlas -- it would spread
    // through the merge + temporal accumulation as persistent black/garbage tiles.
    if (any(isnan(result)) || any(isinf(result))) result = vec3(0.0);
    return max(result, vec3(0.0));
}
