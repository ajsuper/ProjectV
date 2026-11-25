$input v_color0
$input v_texcoord0

// =============================================================================
// reflect.frag -- what the glossy bits reflect (santa-fe-reference S21), at the trace pass's resolution, before
// the composite (shadow_soft.frag adds it, under the clouds).
//
// The trace pass chooses the ray (gGloss: xyz its direction times 1 + the roughness, w its weight -- Fresnel,
// strength, and the air and haze between the surface and the eye): a glint cell's mirror direction, or the
// mirror direction of the rough sheen of snow, rock or leaves. The sheen (S22e) reflects only the lights: the
// sky, with this frame's clouds blurred by the roughness where the screen shows that direction, two directions
// of the lobe's sky past the screen (the sun's highlight is the trace's, exact). A glint's mirror is followed
// through the screen:
//   * marched through this frame's gPos, steps growing: where it goes behind what the screen shows (within a
//     thickness), last frame's image (resolved) where that point was -- terrain, trees, snow, lit as they are;
//   * where it leaves the screen or finds nothing: the sky. Last frame's image in that direction where the
//     screen shows sky there (clouds and all), the analytic sky (mountains_sky.sc) past the screen's edge,
//     blended across the edge.
// The sun's disc is not here: the trace adds the glints' sun through the sun's own shadow ray, which is what
// a reflected ray toward the sun would meet (and it works off the screen).
//
// Inputs: the trace pass (colour 0, gPos 1, gNormal 2, gFace 3, gRot 4, gMotion 5, gSun 6, gGloss 7), then
// the resolve (last frame: resolved 8, its history 9), the clouds (this frame: 10, their depth 11).
// Output: rgb the reflected light, weighted (added to the lit frame).
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>
#include <pjv_camera.sc>
#define SF_SKY_TEX_SLOT 12   // sky_ambient.frag (S23): after the clouds (10, 11)
#include <mountains_sky.sc>

// ---- Tunables ---------------------------------------------------------------------------------------
#define SF_SSR_STEPS          28     // march steps
#define SF_SSR_FIRST_STEP     2.0    // the first step, in render texels' worth of world at the surface
#define SF_SSR_GROWTH         1.22   // each step this much longer than the last: near detail, far reach
#define SF_SSR_THICKNESS      1.5    // how far behind the screen's surface a hit may be, in steps
#define SF_SSR_EDGE_FADE      0.08   // the share of the screen at its edges over which the screen fades to the sky
#define SF_SSR_SUN_GUARD      0.9998 // a sky lookup this close to the sun reads the analytic sky (the disc is the trace's)

SAMPLER2D(gPos, 1);
SAMPLER2D(gNormal, 2);
SAMPLER2D(gGloss, 7);
SAMPLER2D(resolved, 8);
SAMPLER2D(clouds, 10);       // clouds.frag, this frame: rgb their light, a what they let through (9: the resolve's history)

// ---- The sheen (S22e) -----------------------------------------------------------------------------
#define SF_SHEEN_BLUR_PIXELS  12.0   // the clouds' blur at roughness 1, in window pixels (S24: the lobe is sampled now, a frame at a time)
#define SF_SHEEN_SPREAD       0.6    // off the screen: the second sky direction, this share of the roughness off the mirror

float sfEdgeFade(vec2 uv) {
    vec2 e = min(uv, 1.0 - uv);
    return smoothstep(0.0, SF_SSR_EDGE_FADE, min(e.x, e.y));
}

bool sfOnScreen(vec2 uv) { return uv.x >= 0.0 && uv.y >= 0.0 && uv.x < 1.0 && uv.y < 1.0; }

// The sky a rough surface reflects along R: the analytic sky, and this frame's clouds where the screen shows
// that direction, blurred by the roughness (a ring of taps); off the screen, the sky along two directions of
// the lobe -- the mirror and one bent toward the normal -- with no clouds.
vec3 sfSheenSky(vec3 origin, vec3 R, vec3 n, float alpha, vec3 camP, vec3 camF, float tanHalf) {
    vec3 sky = mtnSkyFrom(origin, R);
    bool inFront;
    vec2 res = vec2(textureSize(gPos, 0));
    vec2 uv = pjvPerspectiveUV(camP + R * 1.0e6, res, camP, camF, tanHalf, inFront);
    float on = inFront && sfOnScreen(uv) ? sfEdgeFade(uv) : 0.0;
    vec3 offScreen = 0.5 * (sky + mtnSkyFrom(origin, normalize(mix(R, n, SF_SHEEN_SPREAD * alpha))));
    if (on <= 0.0) return offScreen;
    vec2 csize = vec2(textureSize(clouds, 0));
    float r = SF_SHEEN_BLUR_PIXELS * alpha / res.y * 0.5;   // in uv, of the screen's height
    vec4 cl = texture2D(clouds, uv);
    float wsum = 1.0;
    for (int i = 0; i < 8; i++) {
        float a = float(i) * 0.785398;
        vec2 q = uv + vec2(cos(a) * res.y / res.x, sin(a)) * r;
        if (!sfOnScreen(q)) continue;
        cl += texture2D(clouds, q);
        wsum += 1.0;
    }
    cl /= wsum;
    vec3 seen = sky * clamp(cl.a, 0.0, 1.0) + max(cl.rgb, vec3(0.0, 0.0, 0.0));
    return mix(offScreen, seen, on);
}

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec4 g = texelFetch(gGloss, at, 0);
    vec4 p0 = texelFetch(gPos, at, 0);
    if (g.w <= 0.0 || p0.w < 0.0) { gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0); return; }

    float alpha = max(length(g.xyz) - 1.0, 0.0);   // the trace's roughness (0: a glint's mirror)
    vec3 R = normalize(g.xyz + vec3(0.0, 1e-6, 0.0));
    vec3 nf = texelFetch(gNormal, at, 0).xyz;
    vec3 camP = pjvViewPosition();
    vec3 camF = pjvViewDirection();
    float tanHalf = pjvViewTanHalfFov();
    vec2 res = vec2(textureSize(gPos, 0));
    vec2 resLast = vec2(textureSize(resolved, 0));
    float texelWorld = max(p0.w, 1e-3) * 2.0 * tanHalf / res.y;
    vec3 origin = p0.xyz + nf * max(texelWorld, 0.005);
    if (alpha > 0.02) {
        vec3 sky = sfSheenSky(origin, R, nf, alpha, camP, camF, tanHalf);
        if (any(isnan(sky)) || any(isinf(sky))) sky = vec3(0.0, 0.0, 0.0);
        gl_FragColor = vec4(max(sky, vec3(0.0, 0.0, 0.0)) * g.w, 1.0);
        return;
    }

    // The sky along R: the analytic sky, and last frame's sky there where the screen shows it.
    vec3 sky = mtnSkyFrom(origin, R);
    if (dot(R, SUN_DIR) < SF_SSR_SUN_GUARD) {
        bool inFront;
        vec2 uv = pjvPerspectiveUV(camP + R * 1.0e6, res, camP, camF, tanHalf, inFront);
        if (inFront && sfOnScreen(uv) && texelFetch(gPos, ivec2(uv * res), 0).w < 0.0) {
            vec2 then = pjvPrevViewUV(pjvPrevViewPosition() + R * 1.0e6, resLast, inFront);
            if (inFront && sfOnScreen(then))
                sky = mix(sky, texelFetch(resolved, ivec2(then * resLast), 0).rgb, sfEdgeFade(uv) * sfEdgeFade(then));
        }
    }

    vec3 light = sky;
    float stride = SF_SSR_FIRST_STEP * texelWorld;
    vec3 Q = origin;
    for (int i = 0; i < SF_SSR_STEPS; i++) {
        Q += R * stride;
        bool inFront;
        vec2 uv = pjvPerspectiveUV(Q, res, camP, camF, tanHalf, inFront);
        if (!inFront || !sfOnScreen(uv)) break;
        vec4 seen = texelFetch(gPos, ivec2(uv * res), 0);
        float depth = length(Q - camP);
        // Behind what the screen shows there, by less than the thickness -- unless what is there lies in this
        // surface's own plane (a grazing ray over flat snow "hits" the snow beyond it: a plane does not see itself).
        bool ownPlane = abs(dot(seen.xyz - origin, nf)) < texelWorld;
        if (seen.w >= 0.0 && !ownPlane && depth > seen.w && depth - seen.w < stride * SF_SSR_THICKNESS + texelWorld) {
            vec2 then = pjvPrevViewUV(seen.xyz, resLast, inFront);
            if (inFront && sfOnScreen(then))
                light = mix(sky, texelFetch(resolved, ivec2(then * resLast), 0).rgb, sfEdgeFade(uv) * sfEdgeFade(then));
            break;
        }
        stride *= SF_SSR_GROWTH;
    }
    if (any(isnan(light)) || any(isinf(light))) light = vec3(0.0, 0.0, 0.0);
    gl_FragColor = vec4(max(light, vec3(0.0, 0.0, 0.0)) * g.w, 1.0);
}
