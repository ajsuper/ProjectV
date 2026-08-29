// =============================================================================
// pjv_cascade.sc  --  Screen-space radiance-cascade gather + merge (the passes 2..5).
//
// Radiance cascades (Sannikov) store a radiance field at several scales. Cascade c
// trades spatial resolution for angular resolution: as c increases the probes get
// SPARSER (spacing doubles) while each probe stores 4x MORE directions cast over a
// 4x LONGER interval further from the probe. So near surfaces we have many probes / few
// directions (fine spatial, coarse angular near field) and far away few probes / many
// directions (coarse spatial, fine angular far field) -- matching how radiance varies.
// Every cascade holds the same texel count, so each cascade is one full-res atlas:
//   pixel (x,y) -> probe (x/Tc, y/Tc), direction tile-coord (x%Tc, y%Tc).
//
// SCREEN-SPACE variant: a probe's rays are marched against the G-buffer depth (gPos), so
// GI is a few texture reads per step instead of a voxel-DDA march. It sees only on-screen
// geometry (the accepted screen-space GI tradeoff) -- which is what was asked for.
//
// The user's two requested light paths arrive here as:
//   INDIRECT SUN -- a gather ray that HITS a surface samples gDirect there (that surface's
//                   direct sunlight, albedo*sun, hard shadow) -> bounced sun into the scene.
//   SKY          -- a ray that passes clean through its whole interval at the TOP cascade
//                   samples skyGradient(dir) -> open-sky fill, merged down into every cascade.
//
// Each thin cascadeN.frag declares gPos/gNormal/gDirect (+ upperCascade unless top), sets
// CASCADE_INDEX (and IS_TOP for the last cascade), includes this file, and writes
// computeCascadeTexel() for its atlas texel. (Cascade index can't be a per-pass uniform --
// uniforms are global per frame -- so it is a per-file #define.)
// =============================================================================

#include <pjv_cascade_common.sc>

// ---- Screen-space gather ray ------------------------------------------------
// March one ray of cascade c from probe world pos P in world direction dir over the
// interval [start_c, start_c+len_c], testing occlusion against the depth buffer (gPos).
// Returns rgb = incoming radiance found on a hit (0 if none), a = visibility:
//   a = 0  -> blocked inside this interval (opaque; do NOT read the upper cascade)
//   a = 1  -> passed clean through (transparent; upper cascade / sky continues the ray)
vec4 gatherRay(vec3 P, vec3 dir, int c) {
    float t0    = cascadeStart(c);
    float t1    = t0 + cascadeLen(c);
    float dt    = (t1 - t0) / float(MARCH_STEPS);
    vec3  fwd   = normalize(cameraDir.xyz);

    // Robust depth-CROSSING occlusion. A hit is when the ray goes from clearly IN FRONT of
    // the depth surface (open space) to BEHIND it -- not "lands in a thin shell", which the
    // old test required and which missed almost every occluder, leaving the sky term
    // unshadowed = a flat ambient over the whole scene. `armed` guarantees we only count a
    // crossing after the ray has been in genuine open space, so it can't self-occlude on the
    // probe's own surface at the origin. `thick` rejects a crossing against a FAR background
    // (a thin silhouette edge) so light still passes behind thin geometry.
    bool armed = false;

    for (int i = 0; i < MARCH_STEPS; i++) {
        float t = t0 + dt * (float(i) + 0.5);
        vec3  S = P + dir * t;

        bool valid;
        vec2 uv = worldToUV(S, cameraPos.xyz, cameraDir.xyz, windowRes.xy, FOV, valid);
        if (!valid) return vec4(0.0, 0.0, 0.0, 1.0);   // left the screen -> continue upward

        vec4 g = texture2D(gPos, uv);
        if (g.a < 0.0) { armed = true; continue; }      // sky pixel -> open ahead -> arm

        float camDistS = dot(S - cameraPos.xyz, fwd);
        float camDistG = dot(g.xyz - cameraPos.xyz, fwd);
        float delta    = camDistS - camDistG;           // >0: ray is behind the surface
        float eps      = max(0.03, camDistG * 0.004);   // in-front margin (depth-scaled)
        float thick    = max(dt * DEPTH_THICKNESS_SCALE, eps * 2.0);

        if (armed && delta >= 0.0 && delta < thick) {
            vec3 radiance = texture2D(gDirect, uv).rgb;
            return vec4(radiance, 0.0);                 // crossed behind a surface -> occluded
        }
        if (delta < -eps) armed = true;                 // clearly in open space -> arm
    }
    return vec4(0.0, 0.0, 0.0, 1.0);                    // clean pass -> transparent
}

// ---- Merge: continue a transparent ray with the cascade above --------------
#ifndef IS_TOP
// Average the four child directions (the 2x2 sub-tile in c+1) of parent tile coord, for
// one c+1 probe, reading the already-merged upperCascade atlas.
vec3 upperProbeDir(ivec2 upProbe, ivec2 parentTile, int upTc) {
    vec3 sum = vec3(0.0);
    for (int j = 0; j < 2; j++)
    for (int k = 0; k < 2; k++) {
        ivec2 childTile = parentTile * 2 + ivec2(k, j);
        ivec2 px = upProbe * upTc + childTile;
        vec2 uv = (vec2(px) + 0.5) / windowRes.xy;
        sum += texture2D(upperCascade, uv).rgb;
    }
    return sum * 0.25;
}

// Bilinear over the four c+1 probes nearest the probe centre at screen pos SPpx.
vec3 mergeUpper(vec2 SPpx, ivec2 parentTile, int c) {
    int upTc = tileSizeOf(c + 1);
    vec2 gp = SPpx / float(upTc) - 0.5;   // c+1 probe-grid coord (centres at (i+0.5)*upTc)
    vec2 f  = fract(gp);
    ivec2 b = ivec2(floor(gp));
    vec3 r00 = upperProbeDir(b + ivec2(0, 0), parentTile, upTc);
    vec3 r10 = upperProbeDir(b + ivec2(1, 0), parentTile, upTc);
    vec3 r01 = upperProbeDir(b + ivec2(0, 1), parentTile, upTc);
    vec3 r11 = upperProbeDir(b + ivec2(1, 1), parentTile, upTc);
    return mix(mix(r00, r10, f.x), mix(r01, r11, f.x), f.y);
}
#endif

// ---- Per-texel entry point --------------------------------------------------
// fragPx = integer pixel coord of this cascade-atlas texel.
vec3 computeCascadeTexel(ivec2 fragPx) {
    int c  = CASCADE_INDEX;
    int Tc = tileSizeOf(c);

    ivec2 probe = fragPx / Tc;          // which probe this texel belongs to
    ivec2 tile  = fragPx - probe * Tc;  // direction tile coord within the probe

    // Probe anchor = screen pixel at the tile centre; its G-buffer sample is the probe's
    // world position + surface normal.
    vec2 probeCenterPx = (vec2(probe) + 0.5) * float(Tc);
    vec2 probeUV = probeCenterPx / windowRes.xy;
    vec4 gp = texture2D(gPos, probeUV);
    vec3 P  = gp.xyz;
    vec3 N  = normalize(texture2D(gNormal, probeUV).xyz);

    vec3 dir = dirForTile(tile, Tc, cascadeJitter());

    // Sky/background probe, or a direction below this surface's horizon: nothing to gather
    // (the surface itself blocks sub-horizon directions).
    if (gp.a < 0.0 || dot(dir, N) <= 0.0) {
        return vec3(0.0);
    }

    vec3 origin = P + N * NORMAL_BIAS;
    vec4 g = gatherRay(origin, dir, c);   // rgb = hit radiance, a = visibility

#ifdef IS_TOP
    // Top cascade: what lies beyond the last interval. With SKY_GI this is the open sky
    // (sky becomes a light source); with SKY_GI 0 it is BLACK, so the only light in the GI
    // is the sun and its bounces off sunlit surfaces (a ray only carries light if it hit a
    // sunlit surface via gDirect). The sky gradient is the ONLY sky term that feeds the GI
    // -- it propagates down every cascade through the merge -- so zeroing it here removes
    // all "flat sky" from surface lighting. (The visible sky BACKGROUND behind geometry is
    // separate: gbuffer writes it into gDirect for sky pixels and resolve passes it through.)
    #if SKY_GI
    vec3 upper = skyGradient(dir);
    #else
    vec3 upper = vec3(0.0);
    #endif
#else
    vec3 upper = mergeUpper(probeCenterPx, tile, c);
#endif

    return g.rgb + g.a * upper;           // this interval + (if it passed) what lies beyond
}
