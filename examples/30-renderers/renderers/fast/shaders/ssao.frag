$input v_color0
$input v_texcoord0

// =============================================================================
// ssao.frag  --  Pass 2 of the fast renderer. Screen-space ambient occlusion.
//
// Casts NO scene rays. Every occlusion decision is made from the G-buffer the previous pass
// already wrote, which is what makes this the cheap renderer's AO: the cost is a handful of
// texture reads per pixel and no voxel traversal at all.
//
// The method is the standard world-space-sample SSAO. For each of AO_SAMPLES directions in the
// hemisphere around the surface normal, step a short distance from the shaded point, project that
// offset point back into screen space, and read what the G-buffer says is actually there. If the
// stored surface is nearer to the camera than the offset point, something is in the way and the
// sample is occluded.
//
// WHAT THIS GETS WRONG, deliberately: it can only see what the camera can see. An occluder just
// off-screen, or hidden behind the silhouette of something nearer, contributes nothing -- so
// contact shadows soften at frame edges and creases facing away from the camera are under-occluded.
// That is the trade the fast renderer is making. For occlusion that is right regardless of what is
// on screen, cast real rays: 30-renderers' tree64 and taa both do, and AdvancedRenderer's probe
// gather does it properly.
//
// Input  (FBO 1): 1 gPos (xyz world hit, a = camDist; a < 0 => sky), 2 gNormal, 4 blueNoise.
// Output (FBO 2): INCOMING SKY RADIANCE, occluded -- not a bare AO scalar.
//
// That distinction matters: combine_blur multiplies this by surface albedo and adds the untouched
// direct sun, so what it wants here is the coloured, directional ambient a surface actually
// receives. Writing a grey scalar instead makes the ambient flat and the occlusion nearly
// invisible, because a white ambient washes out against the sun. So each unoccluded sample
// contributes the sky colour along its own direction, and occlusion is what removes it.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_sun_sky.sc>

SAMPLER2D(gPos,      1);
SAMPLER2D(gNormal,   2);
SAMPLER2D(blueNoise, 4);

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 frameCount;
// sunDir is declared by pjv_sun_sky.sc, which skyColor() needs; declaring it again here is a
// redefinition shaderc rejects.

#define FOV 60.0

// Hemisphere samples per pixel. Cheaper than the ray-traced version's rays by a wide margin -- these
// are texture reads, not traversals -- so this can afford more of them. The combine_blur pass
// filters what noise is left.
#define AO_SAMPLES 12

// How far, in world units, a sample may sit from the shaded point. Occluders beyond this do not
// darken it.
//
// FIXED, deliberately. An earlier version scaled it with distance from the camera to keep the
// effect a constant size on screen, which is the right instinct for a triangle renderer and wrong
// here: it makes the world radius -- and therefore which geometry counts as an occluder -- a
// function of where you are standing, so occlusion strengthens as you back away and the boundary
// of the scaling shows up as a dark ring at a fixed distance from the camera. A voxel scene has
// absolute feature sizes; the AO radius should be one of them.
#define AO_RADIUS 4.0

// Guards against a surface occluding itself through depth-buffer precision at grazing angles.
#define AO_BIAS 0.05

// Occlusion only counts when the blocker is genuinely in front, not merely somewhere along the ray.
// Without this a distant wall behind a thin railing reads as an occluder of everything.
#define AO_RANGE_CHECK 1.0

// How much of the computed occlusion to apply, and how hard to bend its response. Both are here
// because AO only ever modulates the AMBIENT term -- direct sun is added untouched afterwards -- so
// on a sunlit surface even full occlusion moves the pixel very little. These are the knobs to reach
// for if the effect reads as too subtle or too heavy.
#define AO_INTENSITY 1.0
#define AO_POWER     2.0

// Projects a world point into this frame's screen space. The exact inverse of the engine's
// rayStartDirection, so the point this samples is the point the G-buffer describes.
vec2 worldToUV(vec3 P, vec3 camPos, vec3 camDir, vec2 res, float fov, out bool valid) {
    vec3 forward = normalize(camDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));

    vec3 v = P - camPos;
    float z = dot(v, forward);
    if (z <= 1e-4) { valid = false; return vec2(-1.0); }   // behind the camera

    float scale  = tan(radians(fov * 0.5));
    float aspect = res.x / res.y;
    float ndcx = (dot(v, right) / z) / (scale * aspect);
    float ndcy = (dot(v, up)    / z) / scale;

    vec2 flip = (vec2(ndcx, ndcy) + 1.0) * 0.5;
    vec2 uv = vec2(flip.x, 1.0 - flip.y);
    valid = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    return uv;
}

// Hammersley, for an even spread of directions rather than a clumped random one.
float radicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint n) { return vec2(float(i) / float(n), radicalInverse(i)); }

// Cosine-weighted direction in the hemisphere around n. Cosine weighting because that is the
// distribution the ambient term is integrating against.
vec3 hemisphereDir(vec2 u, vec3 n) {
    float r   = sqrt(u.x);
    float phi = 6.2831853 * u.y;
    vec3 t = abs(n.y) > 0.999 ? vec3(1.0, 0.0, 0.0) : normalize(cross(vec3(0.0, 1.0, 0.0), n));
    vec3 b = cross(n, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u.x)));
}

void main() {
    vec4 posSample = texture2D(gPos, v_texcoord0);
    float camDist = posSample.a;

    // Sky. Nothing to occlude, and writing 1.0 keeps combine_blur's filter from pulling darkness
    // in from the background across a silhouette.
    if (camDist < 0.0) {
        gl_FragData[0] = vec4(1.0, 1.0, 1.0, 1.0);
        return;
    }

    vec3 P = posSample.xyz;
    vec3 N = normalize(texture2D(gNormal, v_texcoord0).xyz * 2.0 - 1.0);

    // Per-pixel rotation from the blue-noise LUT, so neighbouring pixels sample different
    // directions and the blur in the next pass turns AO_SAMPLES into many more effective ones.
    vec2 noiseUV = v_texcoord0 * (windowRes.xy / 256.0) + fract(frameCount.x * 0.618);
    float rot = texture2D(blueNoise, noiseUV).r;

    float radius = AO_RADIUS;

    // Sum the sky arriving from each unoccluded direction. Cosine weighting is already in the
    // sample distribution, so this is a plain mean.
    vec3  skyLight = vec3(0.0, 0.0, 0.0);
    float occlusion = 0.0;
    for (int i = 0; i < AO_SAMPLES; i++) {
        vec2 u = fract(hammersley(uint(i), uint(AO_SAMPLES)) + rot);
        vec3 dir = hemisphereDir(u, N);

        // Spread samples through the hemisphere volume rather than only on its surface, so contact
        // occlusion close to the point is represented as well as the wide sweep.
        float t = radius * (0.25 + 0.75 * float(i + 1) / float(AO_SAMPLES));
        vec3 samplePoint = P + dir * t + N * AO_BIAS;

        vec3 dirSky = skyColor(dir);

        bool valid;
        vec2 uv = worldToUV(samplePoint, cameraPos.xyz, cameraDir.xyz, windowRes.xy, FOV, valid);
        // Off-screen, or sky at that texel: nothing known to be in the way, so the sample sees sky.
        if (!valid) { skyLight += dirSky; continue; }

        vec4 occluder = texture2D(gPos, uv);
        if (occluder.a < 0.0) { skyLight += dirSky; continue; }

        // Distance along the view from the camera. If what the G-buffer holds there is nearer than
        // the sample point, the sample is behind a surface.
        float sampleDist = length(samplePoint - cameraPos.xyz);
        float diff = sampleDist - occluder.a;

        // Range check: only blockers within the AO radius count. Without it a wall far behind a
        // railing occludes everything in front of it.
        // Range check: a blocker far in front of the sample is a different surface entirely, not an
        // occluder of this one. Without it a near wall darkens everything visible past it.
        float rangeFalloff = smoothstep(0.0, 1.0, AO_RANGE_CHECK * radius / max(abs(diff), 1e-4));

        if (diff > AO_BIAS) {
            occlusion += rangeFalloff;
            skyLight  += dirSky * (1.0 - rangeFalloff);   // partial blockers still let some through
        } else {
            skyLight  += dirSky;
        }
    }

    skyLight /= float(AO_SAMPLES);

    // Shape the occlusion. AO_INTENSITY scales how much of it is applied and AO_POWER bends the
    // response so light contact darkening stays subtle while deep creases go properly dark; a raw
    // linear mean reads as a uniform grey haze rather than as occlusion.
    float ao = 1.0 - clamp(occlusion / float(AO_SAMPLES), 0.0, 1.0);
    ao = pow(ao, AO_POWER);
    ao = mix(1.0, ao, AO_INTENSITY);

    gl_FragData[0] = vec4(skyLight * ao, 1.0);
}
