$input v_color0
$input v_texcoord0

// =============================================================================
// path_trace.frag  --  Pass 1 of the scene editor's Render mode.
//
// One path per pixel per frame: a primary ray into the voxel scene, then a
// diffuse random walk with next-event estimation towards the sun disk, combined
// by multiple importance sampling. Everything the image is made of is light
// transport -- there is no screen-space ambient occlusion, no fixed per-axis
// face shading and no fake shadow term. That is the whole point of the mode: the
// Viewport shows you the colours that are *in* the scene, and this shows you what
// the scene would look like lit.
//
// Nothing here is denoised. The only thing standing between one sample per pixel
// and the image is taa.frag, which averages this pass's output over the frames
// since the camera last moved. A still camera therefore converges to the
// unbiased answer -- there is no filter to blur away detail and no history
// heuristic to invent it -- which is why a still frame keeps getting cleaner for
// as long as you leave it alone. It is also why this pass has to be honestly
// unbiased rather than merely cheap: a bias here does not average out, it
// converges.
//
// Two things are estimated per bounce and weighted against each other:
//
//   NEE   a direct sample of the sun disk, shadow-rayed for visibility. This is
//         what makes hard sunlight and contact shadows resolve in a handful of
//         frames instead of never -- a cosine-sampled bounce ray finds a disk
//         0.3 radians across roughly one time in a hundred.
//   BSDF  a cosine-weighted hemisphere sample, which carries the indirect light
//         (colour bleeding, sky fill, everything the sun does not reach directly)
//         and, when it happens to land on the sun, the sun again.
//
// Both estimators can find the sun, so both are weighted by the balance heuristic
// and the two are summed. Weighting is not optional here: without it the sun is
// counted twice on the bounces where the walk finds it by accident.
//
// Outputs (FBO 1):
//   gl_FragData[0] traceColor    rgb = this frame's radiance estimate, a = hit mask
//   gl_FragData[1] traceNormal   rgb = primary hit normal, a = voxel edge length
//   gl_FragData[2] tracePosition rgb = primary hit position (world), a = hit mask
//
// The two geometry targets exist for taa.frag's reprojection, which needs to know
// where this pixel's surface was in the previous frame's image. They describe the
// primary hit only; nothing downstream reads the path beyond it.
// =============================================================================

#include <bgfx_shader.sh>

#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
// x = frame index, y = 1.0 on a frame the camera moved, z = the frame it last moved on.
// Only x is read here, and only to decorrelate this frame's samples from the last one's.
uniform vec4 frameCount;
// x = 1.0 for an orthographic projection (the editor's Orthographic and Isometric modes)
// and 0.0 for perspective; y = the world height the image spans when it is; z = how far
// back along the view direction the ray plane sits. Identical to albedo.frag's, so both
// modes frame the scene the same way and switching between them does not move the camera.
uniform vec4 cameraProjection;
// xyz = normalized sun direction, w = the angular RADIUS of the sun disk in radians.
// The radius is a control rather than a constant because it is the softness of every
// shadow in the image: 0.005 is the real sun and razor-sharp, 0.3 is an overcast day.
uniform vec4 sunDir;
// x = diffuse bounces after the primary hit, y = sun intensity, z = sky intensity,
// w = the firefly clamp (maximum radiance any one path may return).
uniform vec4 renderParams;
// Thin-lens depth of field. x = the lens radius in world units (zero disables it entirely),
// y = the focus distance measured along the view axis, z/w spare.
uniform vec4 lensParams;
// Single-scattering atmosphere, which is where the god rays come from. x = the extinction
// coefficient in inverse world units (zero disables it), y = the Henyey-Greenstein anisotropy in
// [-0.95, 0.95], z = how far along the primary ray the medium is integrated, w spare.
uniform vec4 volumeParams;

#define PI 3.14159265359

// The bounce loop's constant bound. The uniform selects how many of these iterations
// actually run; the loop still has to have a compile-time limit to unroll against on
// the HLSL/SPIR-V path.
#define BOUNCE_LIMIT 8

// The primary march is the one that decides whether a pixel has geometry at all, so it
// gets the full step budget. Secondary rays are shorter in practice (they start on a
// surface rather than outside the scene) and there are up to BOUNCE_LIMIT of them per
// path, so they get less.
#define PRIMARY_MAX_STEPS   256u
#define SECONDARY_MAX_STEPS 160u
#define SHADOW_MAX_STEPS    160u

// Where a bounce or shadow ray starts, in edge lengths of the voxel it is leaving. A
// fixed world epsilon does not work across the range of scenes the editor opens --
// editor scenes sit at coordinates in the thousands, where a float32 ULP is around a
// thousandth of a unit, and voxels range from 0.01 units to 1.0. Scaling by the voxel
// keeps the offset meaningful at both ends. See shade.frag, which reasons the same way
// about its shadow ray.
#define RAY_ORIGIN_BIAS_VOXELS 0.25

// =============================================================================
// Sun and sky
// =============================================================================
//
// Ported from the PathTracer example's shared pjv_sun_sky.sc, with the intensities
// exposed as uniforms. The colours are functions of the sun's elevation alone, so
// dragging the sun down towards the horizon warms the light and reddens the sky and
// pushing it below darkens everything towards night -- one control, a whole day.
//
// This is not a copy for its own sake: that header lives in the PathTracer example's
// own sharedShaders directory, and an example does not reach into another example's
// files. The alternative -- promoting it to the engine's include/ -- would make a
// light rig part of the engine, which it is not.

vec3 sunRadianceColor() {
    float elevation = sunDir.y;
    vec3  tint      = mix(vec3(1.0, 0.42, 0.15), vec3(1.0, 0.96, 0.90),
                          smoothstep(0.0, 0.35, elevation));
    // Fades out below the horizon, so the sun setting actually ends the day rather
    // than shining up through the ground.
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
    // Strongest as the sun nears the horizon and gone once it is high: the sunset band.
    float warm = day * (1.0 - smoothstep(0.0, 0.30, elevation));
    vec3  base = mix(vec3(0.03, 0.04, 0.08), vec3(0.75, 0.85, 1.00) * 1.6, day);
    return mix(base, vec3(1.0, 0.50, 0.25) * 1.8, warm * 0.85) * renderParams.z;
}

vec3 skyGround() {
    float day = smoothstep(-0.18, 0.22, sunDir.y);
    return mix(vec3(0.01, 0.01, 0.02), vec3(0.25, 0.24, 0.22) * 0.6, day) * renderParams.z;
}

// The bare atmosphere in a direction: no sun disk, just the gradient. This is what a
// path that escapes the scene collects, and the sun is excluded from it because the
// disk is accounted for separately (and weighted) by the MIS arithmetic below.
vec3 skyGradient(vec3 direction) {
    float up = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 sky = mix(skyHorizon(), skyZenith(), up);
    return mix(skyGround(), sky, smoothstep(-0.05, 0.05, direction.y));
}

// The solid angle the sun disk subtends, and the radiance across it that integrates to
// sunRadianceColor()'s irradiance. Both follow from the disk's angular radius, so
// widening the sun softens the shadows without changing how bright the scene is.
float sunSolidAngle() {
    return 2.0 * PI * (1.0 - cos(sunDir.w));
}

// =============================================================================
// Sampling
// =============================================================================

// PCG, one round. White noise rather than the blue-noise LUT the PathTracer example
// loads: blue noise buys perceptual quality at a handful of samples, and this mode is
// built around converging over hundreds. Both reach the same answer, and this one is
// one texture and one image file fewer for the editor to carry.
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

// Van der Corput, for the sub-pixel jitter. The whole image shares one offset per
// frame, so the jitter is a stratified sequence over the frames the accumulation
// averages rather than independent noise per pixel -- which is what turns a voxel's
// axis-aligned silhouette into a clean edge in a few dozen frames instead of a few
// hundred.
float halton(int index, int base) {
    float f = 1.0;
    float r = 0.0;
    for (int k = 0; k < 16; k++) {
        if (index <= 0) break;
        f /= float(base);
        r += f * float(index - (index / base) * base);
        index /= base;
    }
    return r;
}

// An orthonormal basis around n, for turning a local-space sample into a world one.
mat3 basisAround(vec3 n) {
    vec3 tangent = abs(n.z) < 0.999 ? normalize(cross(vec3(0.0, 0.0, 1.0), n))
                                    : normalize(cross(vec3(0.0, 1.0, 0.0), n));
    return mat3(tangent, cross(n, tangent), n);
}

// Cosine-weighted over the hemisphere about n. Its pdf is cos(theta)/PI, which cancels
// the Lambertian BRDF's own cos/PI exactly -- so a diffuse bounce multiplies the
// throughput by the albedo and nothing else.
vec3 sampleCosineHemisphere(vec3 n, inout uint seed) {
    float u1 = randomUnit(seed);
    float u2 = randomUnit(seed);
    float phi = 2.0 * PI * u1;
    float cosTheta = sqrt(1.0 - u2);
    float sinTheta = sqrt(u2);
    return basisAround(n) * vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// Uniform over a unit disk, by Shirley's concentric mapping. The obvious
// polar construction -- r = u1, theta = 2*pi*u2 -- clumps samples at the centre; taking the
// square root fixes the density but distorts the stratification. Concentric does neither, which
// matters here because the disk is the camera's aperture and any structure left in it prints
// itself onto every out-of-focus highlight in the image.
vec2 sampleConcentricDisk(inout uint seed) {
    vec2 offset = vec2(randomUnit(seed), randomUnit(seed)) * 2.0 - 1.0;
    if (offset.x == 0.0 && offset.y == 0.0) return vec2(0.0, 0.0);

    float radius;
    float theta;
    if (abs(offset.x) > abs(offset.y)) {
        radius = offset.x;
        theta = (PI / 4.0) * (offset.y / offset.x);
    } else {
        radius = offset.y;
        theta = (PI / 2.0) - (PI / 4.0) * (offset.x / offset.y);
    }
    return radius * vec2(cos(theta), sin(theta));
}

// Henyey-Greenstein phase function: how much light scattering off a particle continues in a given
// direction. g > 0 is forward scattering, which is the whole reason god rays exist -- the haze in
// front of a shaft of sunlight throws most of that light onward towards the camera, so the shaft is
// far brighter looking towards the sun than away from it. g = 0 is isotropic and gives flat,
// directionless fog.
float phaseHenyeyGreenstein(float cosTheta, float g) {
    float gg = g * g;
    float denominator = 1.0 + gg - 2.0 * g * cosTheta;
    return (1.0 - gg) / (4.0 * PI * max(pow(max(denominator, 1e-4), 1.5), 1e-4));
}

// Uniform over the sun's cone. pdf = 1 / sunSolidAngle().
vec3 sampleSunCone(inout uint seed) {
    float u1 = randomUnit(seed);
    float u2 = randomUnit(seed);
    float cosTheta = mix(cos(sunDir.w), 1.0, u1);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 2.0 * PI * u2;
    return basisAround(normalize(sunDir.xyz)) *
           vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// Power heuristic with beta = 2. Standard MIS: weights each estimator by how likely it
// was to have produced the sample, so the two sum to one estimate rather than two.
float powerHeuristic(float pdfA, float pdfB) {
    float a = pdfA * pdfA;
    return a / max(a + pdfB * pdfB, 1e-9);
}

// =============================================================================
// Rays
// =============================================================================

// The primary ray, under whichever projection the editor has selected. Identical
// construction to albedo.frag's -- the two must agree exactly, because the editor
// projects its outlines and gizmo onto the image with the same basis on the CPU, and
// because switching modes should not move the camera by a pixel.
//
// Depth of field is layered on top of the perspective case as a thin lens: the ray keeps the point
// it was aimed at on the focal plane, and its ORIGIN moves to a random point on an aperture disk.
// Everything at the focus distance therefore lands in the same place whichever point of the lens
// it came through, and everything nearer or further spreads over a circle whose radius grows with
// how far out of focus it is. That is not an approximation of defocus -- it is what defocus is, so
// bokeh, foreground blur and the way an out-of-focus highlight takes the shape of the aperture all
// come out on their own rather than having to be faked.
//
// It costs one disk sample per pixel per frame and nothing else. There is no blur pass and no
// circle-of-confusion buffer: the accumulation the whole renderer is built on averages the lens
// samples exactly the way it averages the sub-pixel jitter, so the blur resolves as the image does.
//
// Orthographic and isometric views get no depth of field, and cannot: parallel rays have no lens
// and no aperture to sample. The editor greys the control out in those modes for the same reason.
Ray primaryRay(vec2 uv, inout uint seed) {
    vec3 forward = normalize(cameraDir.xyz);

    // Identical to rayStartDirection's basis, including the +Z fallback for a view pointing
    // straight up or down. Needed by both branches below.
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));

    Ray ray;
    if (cameraProjection.x < 0.5) {
        ray.origin = cameraPos.xyz;
        ray.direction = rayStartDirection(uv, windowRes.xy, cameraPos.xyz, forward, 60.0);

        if (lensParams.x > 0.0) {
            // The focus distance is measured along the VIEW AXIS, not along this pixel's ray, so
            // the plane in focus is a plane rather than a sphere centred on the camera. Dividing by
            // the cosine between the two is what turns one into the other; without it the corners
            // of the image focus nearer than the centre.
            float axialCosine = max(dot(ray.direction, forward), 1e-4);
            vec3 focalPoint = ray.origin + ray.direction * (lensParams.y / axialCosine);

            vec2 lensOffset = sampleConcentricDisk(seed) * lensParams.x;
            ray.origin += right * lensOffset.x + up * lensOffset.y;
            ray.direction = normalize(focalPoint - ray.origin);
        }
        return ray;
    }

    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float aspectRatio = windowRes.x / windowRes.y;
    float halfHeight = cameraProjection.y * 0.5;

    ray.origin = cameraPos.xyz - forward * cameraProjection.z +
                 right * (ndc.x * halfHeight * aspectRatio) +
                 up * (ndc.y * halfHeight);
    ray.direction = forward;
    return ray;
}

RayQuery walkQuery(uint maxSteps) {
    RayQuery query;
    query.maxRaySteps = maxSteps;
    // No distance LOD anywhere in this renderer. LOD trades geometric detail for march
    // steps, and this mode exists to show the scene as it is -- a converged image of a
    // simplified scene is a very high quality picture of the wrong thing.
    query.startLOD = 0;
    query.finishLOD = 0;
    query.distanceToFinishLOD = 100000;
    return query;
}

// =============================================================================
// The path
// =============================================================================

void main() {
    int frame = int(frameCount.x);

    // Per-pixel, per-frame. The pixel term decorrelates neighbours (so noise is noise
    // rather than a pattern) and the frame term is what the accumulation averages away.
    ivec2 pixel = ivec2(v_texcoord0 * windowRes.xy);
    uint seed = hashSeed(uvec3(uint(pixel.x), uint(pixel.y), uint(frame)));

    // Halton for the frame's shared offset, plus a per-pixel Cranley-Patterson rotation
    // so the shared sequence does not print itself onto the image as a moving grid.
    vec2 jitter = fract(vec2(halton(frame + 1, 2), halton(frame + 1, 3)) +
                        vec2(randomUnit(seed), randomUnit(seed))) - 0.5;
    vec2 uv = v_texcoord0 + jitter / windowRes.xy;

    Ray ray = primaryRay(uv, seed);

    // The camera segment, kept because the atmosphere below is integrated along exactly it. With
    // depth of field on, the origin is a point on the aperture rather than the camera, which is the
    // right thing for the medium to start from too.
    vec3 primaryOrigin = ray.origin;
    vec3 primaryDirection = ray.direction;

    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);

    // Filled in from the primary hit and written to the geometry targets at the end.
    vec3  primaryNormal = vec3(0.0);
    vec3  primaryPosition = vec3(0.0);
    float primaryVoxelSize = 0.0;
    float primaryHit = 0.0;
    // How far the camera ray travelled before it hit anything. On a miss it stays at the medium's
    // own range, which is what makes "the sky" and "something very far away" attenuate the same.
    float primaryDistance = max(volumeParams.z, 0.0);

    vec3  sunDirection = normalize(sunDir.xyz);
    float sunPdf = 1.0 / max(sunSolidAngle(), 1e-9);
    vec3  sunRadiance = sunRadianceColor() / max(sunSolidAngle(), 1e-9);
    float cosSunRadius = cos(sunDir.w);

    // The MIS weight to apply if the *next* miss lands on the sun disk. One on the
    // primary ray (looking straight at the sun shows the sun, unweighted -- there is no
    // NEE estimator competing for it) and the bounce's own weight after that.
    float bsdfSunWeight = 1.0;

    int bounces = int(renderParams.x);

    for (int bounce = 0; bounce <= BOUNCE_LIMIT; bounce++) {
        if (bounce > bounces) break;

        RayQuery query = walkQuery(bounce == 0 ? PRIMARY_MAX_STEPS : SECONDARY_MAX_STEPS);
        SceneIntersectData hit = raySceneIntersect(ray, query);

        // Trust the march's own hit data rather than re-intersecting foundBox: a fresh
        // slab test disagrees with the march by ULPs on boundary-exact hits and reports
        // them as misses, which is where the fireflies used to come from.
        bool missed = hit.foundBox.size < 0.0 || hit.rayT <= 0.0 ||
                      dot(hit.normal, hit.normal) < 0.5;

        if (missed) {
            if (dot(ray.direction, sunDirection) >= cosSunRadius) {
                // The disk itself. Weighted, because on every bounce past the primary
                // the NEE sample above already estimated this same light.
                radiance += bsdfSunWeight * throughput * sunRadiance;
            } else {
                radiance += throughput * skyGradient(ray.direction);
            }
            break;
        }

        // The march's own integer cell, not its world-space box: recovering a coordinate
        // back out of world space is a float32 round trip that shades voxels with their
        // neighbour's colour once a chunk has been moved or rotated. See
        // SceneIntersectData::voxelCoord.
        vec3 albedo = fetchVoxelColorAtCoord(hit.voxelCoord, hit.headerIndex);
        vec3 normal = normalize(hit.normal);
        vec3 position = ray.origin + ray.direction * hit.rayT;
        float originBias = RAY_ORIGIN_BIAS_VOXELS * hit.foundBox.size;

        if (bounce == 0) {
            primaryNormal = normal;
            primaryPosition = position;
            primaryVoxelSize = hit.foundBox.size;
            primaryHit = 1.0;
            primaryDistance = hit.rayT;
        }

        // --- Next event estimation: one shadow ray at the sun -------------------
        vec3 lightDirection = sampleSunCone(seed);
        float cosAtSurface = dot(normal, lightDirection);
        if (cosAtSurface > 0.0 && dot(sunRadiance, sunRadiance) > 0.0) {
            Ray shadowRay;
            shadowRay.origin = position + normal * originBias;
            shadowRay.direction = lightDirection;

            SceneIntersectData shadowHit = raySceneIntersect(shadowRay, walkQuery(SHADOW_MAX_STEPS));
            if (shadowHit.rayT < 0.0 || shadowHit.foundBox.size < 0.0) {
                // Lambertian: f = albedo / PI. The estimator is f * cos * L / pdf, and
                // it is weighted against the chance the bounce sample below would have
                // found the same disk on its own.
                float bsdfPdf = cosAtSurface / PI;
                float weight = powerHeuristic(sunPdf, bsdfPdf);
                radiance += weight * throughput * (albedo / PI) * cosAtSurface *
                            sunRadiance / sunPdf;
            }
        }

        // --- BSDF sample: the diffuse random walk -------------------------------
        vec3 nextDirection = sampleCosineHemisphere(normal, seed);
        float cosNext = dot(normal, nextDirection);
        if (cosNext <= 0.0) break;

        // f * cos / pdf = (albedo / PI) * cos / (cos / PI) = albedo. Cosine-weighted
        // sampling is chosen precisely so this reduces to the one multiply.
        throughput *= albedo;

        // What this ray's weight would be if it escapes onto the sun disk.
        bsdfSunWeight = powerHeuristic(cosNext / PI, sunPdf);

        ray.origin = position + normal * originBias;
        ray.direction = nextDirection;

        // Cheap termination for paths that can no longer contribute. Not Russian
        // roulette -- roulette is unbiased but noisy, and at these bounce counts the
        // paths it would kill are the ones already multiplied down to nothing.
        if (max(throughput.r, max(throughput.g, throughput.b)) < 0.002) break;
    }

    // =========================================================================
    // Atmosphere: single scattering along the camera ray
    // =========================================================================
    //
    // This is where god rays come from, and they are not an effect bolted on afterwards -- there is
    // no radial blur from a screen-space sun, no light-shaft pass, no depth-buffer trick. The
    // medium is a real participating volume between the camera and whatever it is looking at, and
    // the shafts appear because the parts of it the sun can see scatter light towards the camera
    // and the parts in shadow do not. That is why they bend correctly around geometry, why they
    // appear through a window or an arch without anything being told there is a window, and why
    // they hold up when the camera moves through them.
    //
    // ONE sample per pixel per frame, stochastically:
    //
    //   pick a distance t uniformly along the camera segment
    //   pick a direction on the sun's disk
    //   shadow-ray from that point at that direction
    //   add sigma_s * transmittance(t) * phase(theta) * sunIrradiance * visibility / pdf(t)
    //
    // Unbiased, so the accumulation converges to true single scattering. A deterministic march
    // would need dozens of steps per pixel to look smooth in a single frame and would still band;
    // one random step per frame is noise, and noise is the one thing this renderer is built to
    // remove. The cost is one extra shadow ray per pixel per frame regardless of the medium's
    // depth -- a march's cost scales with it.
    //
    // Multiple scattering is not modelled. It is what makes thick fog glow rather than merely
    // dim, and at the densities this control is useful at it is a small correction; the extinction
    // term below still removes the light it would have carried, so a dense setting reads as
    // slightly too dark rather than wrong.
    if (volumeParams.x > 0.0 && volumeParams.z > 0.0) {
        float extinction = volumeParams.x;
        float segmentLength = min(primaryDistance, volumeParams.z);

        // Everything gathered so far arrived along the camera segment, so it is all attenuated by
        // the same amount. Applied to the accumulated radiance in one multiply rather than at each
        // point it was added -- the medium is homogeneous, so the two are identical.
        radiance *= exp(-extinction * segmentLength);

        if (segmentLength > 0.0) {
            float distanceAlongRay = randomUnit(seed) * segmentLength;
            // pdf of that uniform choice is 1 / segmentLength, so dividing by it is multiplying by
            // segmentLength. Written out because it is the one place the estimator's weight lives.
            float distancePdf = 1.0 / segmentLength;

            vec3 scatterPoint = primaryOrigin + primaryDirection * distanceAlongRay;
            float transmittance = exp(-extinction * distanceAlongRay);

            // Non-absorbing: everything the medium takes out of the beam it puts back somewhere
            // else. One control instead of two, and it is the assumption that makes haze read as
            // haze rather than as smoke.
            float scattering = extinction;

            vec3 lightDirection = sampleSunCone(seed);
            float phase = phaseHenyeyGreenstein(dot(primaryDirection, lightDirection),
                                                clamp(volumeParams.y, -0.95, 0.95));

            vec3 sunIrradiance = sunRadianceColor();
            if (dot(sunIrradiance, sunIrradiance) > 0.0) {
                Ray shaftRay;
                shaftRay.origin = scatterPoint;
                shaftRay.direction = lightDirection;

                SceneIntersectData shaftHit = raySceneIntersect(shaftRay, walkQuery(SHADOW_MAX_STEPS));
                if (shaftHit.rayT < 0.0 || shaftHit.foundBox.size < 0.0) {
                    // The sun's cone was sampled uniformly, and integrating the disk's radiance
                    // over its own solid angle gives exactly the irradiance sunRadianceColor()
                    // returns -- so the solid angle and its pdf cancel and neither appears here.
                    radiance += scattering * transmittance * phase * sunIrradiance / distancePdf;
                }
            }

            // The sky lights the haze too, and leaving it out is what makes shadowed fog read as
            // black smoke. Taken unshadowed and isotropic -- the average of the gradient's three
            // endpoints, scattered evenly -- rather than as a second traced sample: a shadow ray
            // per pixel spent on the ambient term would halve the rate the shafts converge at, and
            // the shafts are the reason anyone turns this on. Haze indoors is therefore slightly
            // too bright, which is the visible cost of that choice.
            vec3 ambientSky = (skyZenith() + skyHorizon() + skyGround()) / 3.0;
            radiance += scattering * transmittance * ambientSky / distancePdf;
        }
    }

    // Firefly clamp. A single path that finds the sun through a one-voxel gap returns a
    // radiance thousands of times the image's mean, and a running mean over a few
    // hundred frames does not average that away -- it keeps it as a bright dot that
    // fades over minutes. Clamping is a bias, and it is the one bias this renderer
    // accepts: the alternative is an image that is converged everywhere except the
    // handful of pixels the eye goes to first.
    radiance = min(max(radiance, vec3(0.0)), vec3(renderParams.w));

    gl_FragData[0] = vec4(radiance, primaryHit);
    gl_FragData[1] = vec4(primaryNormal, primaryVoxelSize);
    gl_FragData[2] = vec4(primaryPosition, primaryHit);
}
