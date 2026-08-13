$input v_color0
$input v_texcoord0

// =============================================================================
// gi.frag  --  Pass 2 of the scene editor's viewport renderer.
//
// A path tracer, run at a fraction of the viewport's resolution, whose result is
// upsampled and multiplied into the full-resolution albedo by shade.frag. This is
// what the advanced preview toggle turns on: the same sun, sky and bounce count
// Render mode uses, so the viewport predicts the render rather than approximating it.
//
// It replaced an ambient-occlusion pass that cast one ray and returned a single
// number. Occlusion is what GI does on its way past -- a point enclosed by geometry
// gathers less light because the geometry is in the way -- so the darkening survives
// and now arrives coloured, directional, and with a sun in it.
//
// =============================================================================
// WHY THIS IS ITS OWN PASS, AND WHY IT IS SMALLER THAN THE SCREEN
// =============================================================================
//
// Lighting is low frequency and albedo is high frequency. A voxel edge is a step in
// albedo, but the light arriving either side of it is nearly the same, so tracing
// light per screen pixel spends 16 rays to compute 16 nearly-equal answers while the
// thing that actually changes -- which voxel you are looking at -- is resolved for
// free by the full-resolution G-buffer pass in front of this one.
//
// So this pass runs at `scale` in resources.json (0.25, a sixteenth of the pixels)
// and albedo.frag stays at 1.0. Silhouettes and colour boundaries stay sharp because
// they were never traced here; only the light is coarse, and shade.frag reconstructs
// it with a joint bilateral upsample that respects the full-resolution geometry.
//
// The output is DEMODULATED: the albedo of the surface being shaded is deliberately
// left out, so what this writes is irradiance/pi -- the light arriving, not the light
// leaving. shade.frag multiplies by the full-resolution albedo. That is the whole
// trick that lets the trace be coarse: a quarter-resolution *product* of light and
// albedo would smear voxel colours across each other, while a quarter-resolution
// light term multiplied by a sharp albedo does not.
//
// =============================================================================
// WHAT THIS PASS DOES *NOT* CARRY: THE SUN'S DIRECT TERM
// =============================================================================
//
// The argument above -- light is low frequency, so trace it coarsely -- is true of
// almost all of the light and false of exactly one term. A shadow edge cast by the
// sun is as sharp as the sun is small, which is to say sharper than a voxel; it is
// the highest-frequency thing in the whole image after the albedo itself. Tracing it
// at a quarter resolution and then denoising it produces a soft grey suggestion of a
// shadow, and no amount of bilateral upsampling puts back an edge that was never
// sampled. It is also the term that sells the image, because a hard shadow is how the
// eye reads where a thing is standing.
//
// So the sun's direct contribution AT THE PRIMARY HIT is not computed here. shade.frag
// computes it per screen pixel, unfiltered, and adds it to this pass's upsampled
// result -- one shadow ray per pixel, which is what the viewport's fixed-sun shadow
// toggle has always cost. See directSunLight there.
//
// Everything else stays here, and the dividing line is frequency rather than the
// textbook direct/indirect one:
//
//   the sun at bounces 1..n   sunlight that has bounced at least once. Diffuse, soft,
//                             low frequency by the time it arrives -- coarse is right.
//   the sky, at every vertex  an area light the size of the hemisphere. It has no
//                             sharp edge to lose.
//   emitters, at every vertex which is a compromise, and the one thing here that is
//                             coarser than it should be: a small bright emitter casts a
//                             shadow nearly as hard as the sun's. Sampling emitters
//                             directly needs a list of the emissive voxels in the scene,
//                             which nothing in the editor builds yet -- see the same
//                             note in renderRenderer/path_trace.frag. Until then their
//                             light is found by chance, by the walk below, and arrives
//                             at this pass's resolution.
//
// Inputs (FBO 1): 0 = previewColor, 1 = previewNormal, 2 = previewPosition,
//                 3 = previewGlow. Only the geometry channels are read; the primary
//                 albedo is deliberately not, since it is demodulated out.
//                 The rays additionally read the scene through the samplers
//                 pjv_utils_DDA.sc declares at slots 9-15.
// Output (FBO 5): giRaw.rgb -- irradiance/pi at the primary hit, MINUS the sun's direct
//                 term, which shade.frag computes at full resolution instead.
// =============================================================================

#include <bgfx_shader.sh>

// Transparent voxels are seen through rather than treated as opaque, so a window does not black out
// the light behind it. A smaller budget than the primary march's: this is indirect light, and the
// difference between eight layers of glass and sixty-four is not visible in a bounce.
#define MAX_PEEL_ITERATIONS 9
#include <pjv_utils_DDA.sc>
#define TRANSPARENT_LAYERS 8u

SAMPLER2D(previewColor,    0);
SAMPLER2D(previewNormal,   1);
SAMPLER2D(previewPosition, 2);
SAMPLER2D(previewGlow,     3);

uniform vec4 passTargetRes;                        // (w, h, 1/w, 1/h) of THIS pass's target.
// 8 must equal PROJV_MAX_PASS_INPUTS in constructedRenderer.h -- the engine sets that many.
uniform vec4 passInputRes[8];  // Per input slot; [1] and [2] are the G-buffer.
uniform vec4 frameCount;
// x = advanced preview. Zero writes black and stops: with the toggle off shade.frag must see no
// light at all, so that the four readability aids are exactly what they always were.
uniform vec4 previewSettings;
// Render mode's light rig, uniform for uniform, so the two modes agree.
// sunDir: xyz = direction, w = the sun's angular radius in radians (its shadow softness).
uniform vec4 sunDir;
// x = diffuse bounces after the primary hit, y = sun intensity, z = sky intensity,
// w = the firefly clamp (the most radiance any one path may return).
uniform vec4 renderParams;

#define PI 3.14159265359

// Compile-time bound on the bounce loop. renderParams.x is the runtime count and is clamped to this.
// Matches Render mode's BOUNCE_LIMIT so a scene set up for eight bounces previews at eight.
#define PREVIEW_BOUNCE_LIMIT 8

// Step budgets. Shorter than Render mode's, and this is the one place the preview deliberately
// differs: a bounce ray that runs out of steps stops early and collects sky, which reads as slightly
// too bright rather than as an artifact. Buying the last few percent of accuracy here would cost the
// pass its interactivity, which is the only reason it exists.
#define GI_PRIMARY_STEPS 160u
#define GI_SHADOW_STEPS  128u

// Where a ray leaves the surface, in edge lengths of the voxel it starts on. Identical in value and
// reasoning to the bias the occlusion pass used and shade.frag's shadow ray still uses: a fixed
// world epsilon is below the float32 noise floor at the coordinates editor scenes sit at, so the
// offset is scaled by the voxel instead.
#define GI_ORIGIN_BIAS_VOXELS 0.25

// --- The light rig -----------------------------------------------------------------
// Lifted verbatim from renderRenderer/path_trace.frag rather than shared through a header, for the
// reason given there: a light rig is not part of the engine, and an example does not reach into
// another example's files. The two copies must agree, and that is the point -- this pass exists to
// predict that one.

vec3 sunRadianceColor() {
    float elevation = sunDir.y;
    vec3  tint      = mix(vec3(1.0, 0.42, 0.15), vec3(1.0, 0.96, 0.90),
                          smoothstep(0.0, 0.35, elevation));
    float intensity = renderParams.y * smoothstep(-0.05, 0.15, elevation);
    return tint * intensity;
}

vec3 skyZenith() {
    float day = smoothstep(-0.18, 0.22, sunDir.y);
    return mix(vec3(0.02, 0.03, 0.06), vec3(0.30, 0.50, 0.95) * 2.2, day) * renderParams.z;
}

vec3 skyHorizon() {
    float elevation = sunDir.y;
    float day  = smoothstep(-0.18, 0.22, elevation);
    float warm = day * (1.0 - smoothstep(0.0, 0.30, elevation));
    vec3  base = mix(vec3(0.03, 0.04, 0.08), vec3(0.75, 0.85, 1.00) * 1.6, day);
    return mix(base, vec3(1.0, 0.50, 0.25) * 1.8, warm * 0.85) * renderParams.z;
}

vec3 skyGround() {
    float day = smoothstep(-0.18, 0.22, sunDir.y);
    return mix(vec3(0.01, 0.01, 0.02), vec3(0.25, 0.24, 0.22) * 0.6, day) * renderParams.z;
}

vec3 skyGradient(vec3 direction) {
    float up = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 sky = mix(skyHorizon(), skyZenith(), up);
    return mix(skyGround(), sky, smoothstep(-0.05, 0.05, direction.y));
}

// --- Sampling ----------------------------------------------------------------------

float randomUnit(inout uint seed) {
    seed = seed * 747796405u + 2891336453u;
    uint word = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    return float((word >> 22u) ^ word) * (1.0 / 4294967296.0);
}

uint hashSeed(uvec3 v) {
    uint h = v.x * 1973u + v.y * 9277u + v.z * 26699u;
    h = (h ^ (h >> 15u)) * 2246822519u;
    h = (h ^ (h >> 13u)) * 3266489917u;
    return h ^ (h >> 16u);
}

mat3 basisAround(vec3 n) {
    vec3 tangent = abs(n.z) < 0.999 ? normalize(cross(vec3(0.0, 0.0, 1.0), n))
                                    : normalize(cross(vec3(0.0, 1.0, 0.0), n));
    return mat3(tangent, cross(n, tangent), n);
}

// Cosine-weighted over the hemisphere, by Malley's method. Cosine-weighted because the cosine is in
// the estimator anyway: sampling it out of the pdf is what leaves the throughput below as a bare
// albedo multiply with no per-sample weight at all.
vec3 cosineHemisphere(vec3 n, inout uint seed) {
    float u1 = randomUnit(seed);
    float u2 = randomUnit(seed);
    float radius = sqrt(u1);
    float phi = 2.0 * PI * u2;
    return basisAround(n) * vec3(radius * cos(phi), radius * sin(phi), sqrt(max(0.0, 1.0 - u1)));
}

// A direction inside the sun's cone, uniformly. This is what makes the shadow's edge as soft as the
// sun is wide, and it is why widening sunDir.w softens every shadow without changing the exposure.
vec3 sampleSunCone(inout uint seed) {
    vec3 axis = normalize(sunDir.xyz);
    float cosMax = cos(sunDir.w);
    float cosTheta = mix(cosMax, 1.0, randomUnit(seed));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 2.0 * PI * randomUnit(seed);
    return normalize(basisAround(axis) * vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta));
}

RayQuery walkQuery(uint maxSteps) {
    RayQuery query;
    query.maxRaySteps = maxSteps;
    // No distance LOD. A bounce that gathers light from a coarsened version of the scene reports the
    // wrong colour, and unlike a primary ray there is nothing on screen to tell you it happened.
    query.startLOD = 0;
    query.finishLOD = 0;
    query.distanceToFinishLOD = 100000;
    return query;
}

// How much sunlight reaches a point, as a tint rather than a yes/no -- so coloured glass puts
// coloured light on the floor instead of casting a black shadow.
vec3 sunVisibility(vec3 position, vec3 normal, float voxelSize, vec3 lightDirection) {
    Ray shadowRay;
    shadowRay.origin = position + normal * (GI_ORIGIN_BIAS_VOXELS * voxelSize);
    shadowRay.direction = lightDirection;
    return raySceneTransmittance(shadowRay, walkQuery(GI_SHADOW_STEPS), 1e30, TRANSPARENT_LAYERS);
}

void main() {
    vec4 geometry = texture2D(previewNormal,   v_texcoord0);
    vec4 surface  = texture2D(previewPosition, v_texcoord0);

    // Black is the identity shade.frag adds nothing to, so it is what both the background and the
    // switched-off case write. Keeps the pass behind this one free of a second toggle test.
    if (previewSettings.x < 0.5 || surface.w < 0.5) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // POINT-SAMPLED, not filtered. The G-buffer is four times this pass's width, so an ordinary
    // texture2D at this pass's UV returns a bilinear blend of sixteen full-resolution texels -- and
    // averaging positions and normals across a silhouette produces a point that is on neither
    // surface, floating in front of one and buried in the other. Every ray from it would be wrong.
    //
    // Snapping the UV to the centre of one source texel picks a real sample instead of inventing one.
    // passInputRes[2] is previewPosition's own resolution, which is the only way this pass can know
    // how big an input it did not size is -- see BGFXResources::passInputRes.
    vec2 sourceRes = passInputRes[2].xy;
    vec2 sourceTexel = passInputRes[2].zw;
    if (sourceRes.x > 0.5) {
        vec2 snappedUV = (floor(v_texcoord0 * sourceRes) + 0.5) * sourceTexel;
        geometry = texture2D(previewNormal,   snappedUV);
        surface  = texture2D(previewPosition, snappedUV);
        if (surface.w < 0.5) {
            gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
    }

    vec3  position  = surface.xyz;
    vec3  normal    = normalize(geometry.xyz);
    float voxelSize = geometry.w;

    // Per pixel and per frame. The pixel term decorrelates neighbours so the noise is noise rather
    // than a pattern the filter cannot touch; the frame term is what the accumulate pass averages
    // away over the frames since the camera last moved. The pixel coordinate comes from the UV
    // rather than gl_FragCoord, which bgfx does not expose to a fragment shader on the SPIR-V path.
    ivec2 pixel = ivec2(v_texcoord0 * passTargetRes.xy);
    uint seed = hashSeed(uvec3(uint(pixel.x), uint(pixel.y), uint(frameCount.x)));

    int bounces = int(clamp(renderParams.x, 0.0, float(PREVIEW_BOUNCE_LIMIT)));

    // Irradiance / pi at the primary hit. DEMODULATED: the throughput starts at one rather than at
    // the surface's albedo, so what accumulates here is the light arriving and shade.frag supplies
    // the albedo at full resolution. See the note at the top of the file.
    vec3 light = vec3(0.0);
    vec3 throughput = vec3(1.0);

    vec3 sunIrradiance = sunRadianceColor();
    bool sunIsUp = dot(sunIrradiance, sunIrradiance) > 0.0;

    for (int bounce = 0; bounce <= PREVIEW_BOUNCE_LIMIT; bounce++) {
        if (bounce > bounces) break;

        // --- Sunlight at this vertex, by next-event estimation ----------------------
        // throughput already carries the BRDF factor of every vertex up to and including this one
        // (and, at the primary hit, the demodulated 1 that stands in for its albedo), so this is
        // simply irradiance * cos / pi, tinted by whatever the shadow ray survived.
        //
        // SKIPPED AT THE PRIMARY HIT -- bounce 0 -- and that skip is the direct/indirect split. The
        // term is not dropped, it is computed by shade.frag at full resolution, because it is the one
        // term in this shader whose edge is sharper than this pass's pixels; see the header. From
        // bounce 1 on it is sunlight that has already bounced, which is diffuse and belongs here.
        //
        // The two must not both compute it. A vertex sampled twice is a vertex lit twice, and it
        // would show up as the shadowed side of the split being half as dark as the render's.
        if (sunIsUp && bounce > 0) {
            vec3 lightDirection = sampleSunCone(seed);
            float cosLight = dot(normal, lightDirection);
            if (cosLight > 0.0) {
                vec3 visibility = sunVisibility(position, normal, voxelSize, lightDirection);
                if (dot(visibility, visibility) > 0.0) {
                    light += throughput * sunIrradiance * visibility * (cosLight / PI);
                }
            }
        }

        if (bounce == bounces) break;

        // --- Extend the path -------------------------------------------------------
        Ray bounceRay;
        bounceRay.origin = position + normal * (GI_ORIGIN_BIAS_VOXELS * voxelSize);
        bounceRay.direction = cosineHemisphere(normal, seed);

        PeeledHit peeled = raySceneIntersectPeeled(bounceRay, walkQuery(GI_PRIMARY_STEPS),
                                                   TRANSPARENT_LAYERS, seed);
        // Whatever the transparent layers on the way emitted, and their filtering of what is behind.
        light += throughput * peeled.emission;
        throughput *= peeled.transmittance;

        if (peeled.hit.foundBox.size < 0.0 || peeled.hit.rayT < 0.0) {
            // Escaped: the path ends on the sky. The cosine and its pdf already cancelled into the
            // throughput, so the gradient enters unweighted.
            light += throughput * skyGradient(bounceRay.direction);
            break;
        }

        VoxelMaterial material = peeled.material;
        // An emitter's own glow is not scaled by its own albedo, so this is added before the albedo
        // folds into the throughput below.
        light += throughput * material.emission;

        // Cosine-sampled Lambertian: brdf * cos / pdf = (albedo/pi) * cos / (cos/pi) = albedo. The
        // whole estimator is one multiply, which is the payoff for sampling the cosine.
        throughput *= material.albedo * (1.0 - material.metallic);

        position  = bounceRay.origin + bounceRay.direction * peeled.hit.rayT;
        normal    = normalize(peeled.hit.normal);
        voxelSize = peeled.hit.foundBox.size;

        // Nothing further can contribute enough to see. Cheaper than spending the remaining bounces
        // resolving light that will be multiplied by ~zero.
        if (max(throughput.r, max(throughput.g, throughput.b)) < 0.004) break;
    }

    // Firefly clamp, on the same control Render mode uses. One path that finds the sun through a
    // one-voxel gap returns a value thousands of times the image's mean, and a running mean does not
    // remove that -- it keeps it as a bright dot that fades over minutes. Clamping is a bias, and it
    // is the one bias worth accepting: the alternative is an image that is clean everywhere except
    // the pixels the eye goes to first.
    // On the path's PEAK CHANNEL, with the whole vector scaled to meet it, exactly as Render mode's
    // does and for the reason written out at length there: a per-component min() does not dim a
    // colour, it erases it, dropping every channel that is over the ceiling to the ceiling and
    // leaving the rest, so a strongly tinted bounce off a bright emitter arrived white. The peak
    // form bounds the intensity and leaves the chromaticity alone, which is the only behaviour that
    // makes sense for a preview whose entire job is showing what colour something is.
    light.r = light.r > 0.0 ? light.r : 0.0;
    light.g = light.g > 0.0 ? light.g : 0.0;
    light.b = light.b > 0.0 ? light.b : 0.0;

    float clampMax = max(renderParams.w, 0.0);
    float peakChannel = max(light.r, max(light.g, light.b));
    if (clampMax > 0.0 && peakChannel > clampMax) {
        light *= clampMax / peakChannel;
    }

    gl_FragColor = vec4(light, 1.0);
}
