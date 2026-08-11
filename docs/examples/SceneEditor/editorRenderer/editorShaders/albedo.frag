$input v_color0
$input v_texcoord0

// =============================================================================
// albedo.frag  --  Pass 1 of the scene previewer.
//
// One primary ray per pixel, and the voxel's stored albedo written out with no
// lighting applied at all. This is the whole renderer: no shadow ray, no GI, no
// sky model. It is derived from the PathTracer's `fast` renderer, which is the
// cheapest of that example's six, with everything except the primary march
// removed -- so a pixel costs exactly one scene ray.
//
// What that buys is a view of what is actually *in* a scene: the colours a
// voxelizer wrote, unmodulated by any light transport. A material that reads
// wrong here is wrong in the data, not in the lighting. The editor's four viewport
// toggles (the two occlusions, normal shading, sun shadow) are applied by
// shade.frag on the way out rather than here, so this pass stays exactly that
// unmodulated view and turning them all off returns the image to it byte for byte.
//
// The two geometry targets exist only to feed that pass. They are written whether
// or not either toggle is on: branching the march on a UI toggle would change the
// pass's cost with it, and one ray per pixel is cheap enough that the two extra
// attachments are the smaller price.
//
// The sub-pixel jitter is the only stochastic input of the plain path, and it
// exists purely so the accumulate pass can resolve it into anti-aliased edges.
// Voxel silhouettes are all axis-aligned steps, which alias badly; without this
// the preview shimmers on every camera nudge.
//
// =============================================================================
// ADVANCED PREVIEW (previewSettings.x)
// =============================================================================
//
// The fifth toggle on the viewport's bar, and the one thing in this file that is
// not the unmodulated-albedo view. It answers the question the other four cannot:
// **what will the three material properties the Viewport does not draw actually
// do** -- emission, the specular lobe, and transparency. Until it existed the only
// way to find out was to switch to Render mode, which re-frames the shot under a
// sun and a sky and takes hundreds of frames to converge; this stays in the
// viewport's own flat, sunless, background-only world and adds only what the
// materials themselves contribute.
//
// Three changes, all confined to the branch below:
//
//   TRANSPARENCY  the primary march becomes raySceneIntersectPeeled, so it sees
//                 THROUGH transparent voxels to the nearest opaque surface and
//                 reports what the layers in front of it did to the light. The
//                 base colour is tinted by the accumulated transmittance, which
//                 is the whole visible content of `transparency` and the medium
//                 tint. Identical traversal to Render mode's, so a piece of glass
//                 that reads right here reads right there.
//
//   EMISSION      the emissive radiance of the hit, plus the glow of every
//                 transparent layer crossed on the way to it.
//
//   REFLECTION    one stochastic GGX sample per pixel per frame, marched into the
//                 scene, returning the stored colour of whatever it lands on (or
//                 the background). Same lobe, same Fresnel and same masking term
//                 Render mode's path tracer uses -- see specularPreview.
//
// Emission and reflection do NOT go into previewColor. They go to a SEPARATE
// target, previewGlow, which shade.frag ADDS after it has multiplied its four
// readability terms into the base. That split is the point: ambient occlusion,
// axis shading and the sun shadow are darkenings of *diffuse reflectance*, and
// letting them multiply a glow means an emitter dims when it happens to sit in a
// crease or face away from a sun that has nothing to do with it. A light source is
// not shaded by the aids that exist to make unlit geometry legible.
//
// With the toggle off previewGlow is written as zero, so shade.frag adds zero and
// the image is byte for byte what it was before this existed. That is the same
// contract the other four toggles keep, and it is why the peel and the reflection
// ray sit behind a branch rather than being folded into the march.
//
// Outputs (FBO 1):
//   gl_FragData[0] previewColor    rgb = albedo (or the background), a = hit mask
//   gl_FragData[1] previewNormal   rgb = face normal, a = voxel edge length (world)
//   gl_FragData[2] previewPosition rgb = world hit position, a = hit mask
//   gl_FragData[3] previewGlow     rgb = emission + reflection (advanced preview
//                                  only; zero otherwise), a unused
// =============================================================================

#include <bgfx_shader.sh>

// Raised well above the shared default of 16, and defined BEFORE the include so it overrides the
// #ifndef in pjv_utils_DDA.sc for this shader only.
//
// What decides how many transparent voxels a ray crosses is ANGLE, not thickness: a ray entering a
// two-voxel slab at 45 degrees already crosses three or four, and at grazing incidence dozens. So a
// small budget runs out on ordinary geometry, and running out is an approximation rather than a
// slightly wrong colour.
//
// Exactly Render mode's number, and it has to be: the two modes are looking at the same glass with
// the same traversal, and a shallower budget here would make the preview disagree with the render it
// is a preview OF -- which is the one thing it cannot afford to do.
//
// Paying for it in the plain view was the obvious objection, since this is a compile-time bound the
// HLSL/SPIR-V path unrolls and every pixel of every frame carries the code whether it peels or not.
// It was measured rather than assumed: with the advanced preview OFF, this shader runs the same speed
// at 65 as at 2 (1172 vs 1201 fps on the same scene and framing, which is noise). The branch is
// uniform across the draw, so the unrolled peel costs the plain path nothing worth trading fidelity
// for.
//
// With the preview ON it is not free, and what it costs depends on the scene rather than on this
// number: the loop leaves early at the first opaque voxel and again once the transmittance is spent,
// so an opaque scene runs one iteration and a body of clear glass runs the budget. On a test scene
// with a lot of shallow water, 65 measured about twice 33 (168 vs 310 fps). That is the price of
// agreeing with the render, and it is paid only by the person who asked to see it.
#define MAX_PEEL_ITERATIONS 65

#include <pjv_utils_DDA.sc>

// Transparent layers the primary ray may see through. One fewer than the iteration bound above, so
// the opaque surface behind the last layer still has an iteration left to resolve in. Render mode's
// figure, for the reason above.
#define TRANSPARENT_LAYERS 64u

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 frameCount;   // x = frame index
// x = 1.0 for an orthographic projection (the editor's Orthographic and Isometric
// modes) and 0.0 for perspective; y = the world height the image spans when it is;
// z = how far back along the view direction the ray plane sits. See the editor's
// cameraOrthoHeight / cameraOrthoBackoff, which compute all three.
uniform vec4 cameraProjection;
// x = advanced preview. y/z/w spare. Its own uniform rather than a fifth lane of renderSettings,
// which has none left -- see the note beside renderSettings in the editor's render loop.
uniform vec4 previewSettings;

#define FOV 60.0

#define PI 3.14159265359

// Neutral, desaturated background. A previewer's job is to let you judge stored
// colours, and a strongly tinted or bright backdrop drags perception of every
// albedo in front of it -- so this is deliberately dark and almost grey rather
// than the sky model the lit renderers use.
#define BACKGROUND_TOP    vec3(0.115, 0.125, 0.145)
#define BACKGROUND_BOTTOM vec3(0.022, 0.024, 0.030)

// --- Advanced preview constants ----------------------------------------------

// The reflection ray's step budget. Shorter than the primary's: it starts on a surface inside the
// scene rather than outside it, and it is spent per pixel on top of the primary march.
#define REFLECT_MAX_STEPS 160u

// Where the reflection ray starts, in edge lengths of the voxel it is leaving. A fixed world epsilon
// does not work across the range of scenes the editor opens -- editor scenes sit at coordinates in
// the thousands, where a float32 ULP is around a thousandth of a unit, and voxels range from 0.01
// units to 1.0. Scaling by the voxel keeps the offset meaningful at both ends. shade.frag's shadow
// ray reasons the same way, and for the same reason.
#define REFLECT_ORIGIN_BIAS_VOXELS 0.25

// Ceiling on one specular sample's weight. The half-vector estimator's masking term grows without
// bound at grazing incidence, so a single unlucky sample can return a value hundreds of times the
// image's mean -- and a running mean over 64 frames does not remove that, it keeps it as a bright
// dot. Clamping is a bias; it is the same bias Render mode's firefly clamp accepts, and for the same
// reason: the alternative is an image that is clean everywhere except the pixels the eye goes to
// first. Well above any weight a sample away from grazing produces, so it only ever catches the tail.
#define SPECULAR_WEIGHT_CLAMP 4.0

// Van der Corput / Halton for the sub-pixel jitter (base 2 and 3). Same sequence
// the fast renderer uses, so edges converge at the same rate.
float halton(int i, int base) {
    float f = 1.0;
    float r = 0.0;
    for (int k = 0; k < 16; k++) {
        if (i <= 0) break;
        f /= float(base);
        r += f * float(i - (i / base) * base);
        i /= base;
    }
    return r;
}

vec3 backgroundColor(vec3 direction) {
    float height = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(BACKGROUND_BOTTOM, BACKGROUND_TOP, height);
}

// PCG, one round. Matches Render mode's generator, so the two modes make the same stochastic
// decisions from the same seed -- which is what keeps a stochastically-alpha'd glass surface from
// resolving to two different images in the two tabs.
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

// An orthonormal basis around n, for turning a local-space sample into a world one.
mat3 basisAround(vec3 n) {
    vec3 tangent = abs(n.z) < 0.999 ? normalize(cross(vec3(0.0, 0.0, 1.0), n))
                                    : normalize(cross(vec3(0.0, 1.0, 0.0), n));
    return mat3(tangent, cross(n, tangent), n);
}

// --- The specular lobe --------------------------------------------------------
//
// All four functions below are the same ones Render mode's path_trace.frag uses, and that is not a
// copy for convenience: the point of this preview is to show what the Render tab will do with a
// material, so the two have to agree about the lobe's shape. A different roughness curve or a
// different Fresnel here would make the preview a picture of a material nobody is going to render.

// One direction's term of the separable Smith masking-shadowing function for GGX.
float smithG1(float cosTheta, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * cosTheta /
           max(cosTheta + sqrt(a2 + (1.0 - a2) * cosTheta * cosTheta), 1e-9);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0) - f0) * (m2 * m2 * m);
}

// GGX roughness from the stored glossiness. The perceptual roughness is floored rather
// than allowed to reach zero: a true mirror is a delta lobe, which this estimator cannot
// sample, and an alpha of zero makes the distribution divide by zero. At the floor the
// lobe is tight enough to read as a mirror and still has finite density.
float alphaFromGlossiness(float glossiness) {
    float perceptual = clamp(1.0 - glossiness, 0.03, 1.0);
    return perceptual * perceptual;
}

// A half vector from the GGX distribution, for the reflection ray's direction.
vec3 sampleGGXHalfVector(vec3 n, float alpha, inout uint seed) {
    float u1 = randomUnit(seed);
    float u2 = randomUnit(seed);
    float phi = 2.0 * PI * u1;
    float a2 = alpha * alpha;
    float cosTheta = sqrt(clamp((1.0 - u2) / (1.0 + (a2 - 1.0) * u2), 0.0, 1.0));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    return basisAround(n) * vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// What the surface reflects towards the camera, from one stochastic sample of its specular lobe.
// Zero for a material that has no such lobe, which is every palette entry written before materials
// had properties -- glossiness 0 and metallic 0 drive the dielectric F0 to exactly zero, this
// returns before spending a ray, and the advanced preview costs such a scene nothing but the peel.
//
// One bounce, and what it finds is the reflected voxel's STORED colour rather than a shaded version
// of it. That is the viewport's premise applied one ray deeper: a reflection here shows you the
// colours that are in the file, the same way the direct view does. It is also the only answer this
// renderer can give -- there is no sun and no sky in this mode to shade the reflected surface with.
vec3 specularPreview(vec3 position, vec3 normal, vec3 viewDirection, VoxelMaterial m,
                     float voxelSize, inout uint seed) {
    // A dielectric's F0 is scaled by glossiness rather than being the usual fixed 0.04, matching
    // Render mode: a constant would be more physical, and it would also quietly put a sheen on every
    // scene already on disk. A metal reflects in its own albedo and has no diffuse lobe at all.
    float dielectricF0 = 0.04 * m.glossiness;
    if (max(dielectricF0, m.metallic) <= 0.0) return vec3(0.0);

    vec3  f0 = mix(vec3(dielectricF0), m.albedo, m.metallic);
    float alpha = alphaFromGlossiness(m.glossiness);

    vec3 halfVector = sampleGGXHalfVector(normal, alpha, seed);
    vec3 lightDirection = reflect(-viewDirection, halfVector);

    float cosLight = dot(normal, lightDirection);
    float cosView  = dot(normal, viewDirection);
    float cosHalf  = dot(normal, halfVector);
    float viewDotHalf = dot(viewDirection, halfVector);
    // A GGX sample can reflect below the horizon, and a grazing view can put the shading normal on
    // the wrong side of the geometric one. Both are ordinary and both mean this sample carries
    // nothing, rather than meaning something is wrong.
    if (cosLight <= 0.0 || cosView <= 0.0 || cosHalf <= 0.0 || viewDotHalf <= 0.0) return vec3(0.0);

    // bsdf * cos(light) / pdf for a half-vector-sampled GGX lobe, in closed form. The distribution
    // appears in both the BSDF and the density and cancels exactly, which is what leaves this as
    // Fresnel times the masking term and no D term at all -- the same estimator Render mode's
    // evaluateSurface builds the long way round, written out because there is only one lobe here to
    // weigh and nothing to weigh it against.
    vec3 weight = fresnelSchlick(viewDotHalf, f0) *
                  (smithG1(cosView, alpha) * smithG1(cosLight, alpha) * viewDotHalf /
                   max(cosView * cosHalf, 1e-4));
    weight = min(weight, vec3(SPECULAR_WEIGHT_CLAMP));

    Ray reflectRay;
    reflectRay.origin = position + normal * (REFLECT_ORIGIN_BIAS_VOXELS * voxelSize);
    reflectRay.direction = lightDirection;

    RayQuery reflectQuery;
    reflectQuery.maxRaySteps = REFLECT_MAX_STEPS;
    // No distance LOD, matching the primary march: a reflection of a simplified scene is a picture
    // of the wrong thing, and it is the one place a blocky distant surface would be read as a
    // material problem rather than as a renderer setting.
    reflectQuery.startLOD = 0;
    reflectQuery.finishLOD = 0;
    reflectQuery.distanceToFinishLOD = 100000;

    // Not the peel. A reflection ray that saw through glass would double this shader's unrolled
    // traversal code for the one case of a mirror looking at a window, so a transparent voxel
    // reflects as its own surface here. The primary ray is where transparency is answered.
    SceneIntersectData reflectHit = raySceneIntersect(reflectRay, reflectQuery);
    if (reflectHit.foundBox.size < 0.0 || reflectHit.rayT <= 0.0 ||
        dot(reflectHit.normal, reflectHit.normal) < 0.5) {
        return weight * backgroundColor(lightDirection);
    }

    VoxelMaterial reflected = fetchVoxelMaterialAtCoord(reflectHit.voxelCoord, reflectHit.headerIndex);
    return weight * (reflected.albedo + reflected.emission);
}

// The primary ray, under whichever projection the editor has selected.
//
// Perspective is rayStartDirection's job and unchanged. Orthographic is the same
// camera basis used the other way round: every ray points along the view direction,
// and it is the *origin* that slides across a plane `orthoHeight` world units tall.
// The plane is pushed back by cameraProjection.z rather than left at the camera
// position, because parallel rays have no equivalent of "the camera is outside
// everything in front of it" -- without the offset, anything the camera has flown
// past would simply be missing from an orthographic view of the same scene.
Ray primaryRay(vec2 uv) {
    vec3 forward = normalize(cameraDir.xyz);

    Ray ray;
    if (cameraProjection.x < 0.5) {
        ray.origin = cameraPos.xyz;
        ray.direction = rayStartDirection(uv, windowRes.xy, cameraPos.xyz, forward, FOV);
        return ray;
    }

    // Identical to rayStartDirection's basis, including the +Z fallback for a view
    // pointing straight up or down. The two must agree exactly: the editor projects
    // its outlines and gizmo onto this image with the same construction on the CPU.
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));

    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float aspectRatio = windowRes.x / windowRes.y;
    float halfHeight = cameraProjection.y * 0.5;

    ray.origin = cameraPos.xyz - forward * cameraProjection.z +
                 right * (ndc.x * halfHeight * aspectRatio) +
                 up * (ndc.y * halfHeight);
    ray.direction = forward;
    return ray;
}

void main() {
    int  frame  = int(frameCount.x);
    vec2 jitter = vec2(halton(frame + 1, 2), halton(frame + 1, 3)) - 0.5;
    vec2 uvJit  = v_texcoord0 + jitter / windowRes.xy;

    Ray ray = primaryRay(uvJit);

    RayQuery rayQuery;
    rayQuery.maxRaySteps = 256u;
    // Full-resolution primary march, matching the fast renderer. Distance LOD was
    // measured there to buy almost nothing while making distant geometry blocky,
    // and a previewer is exactly where blocky-at-distance would mislead.
    rayQuery.startLOD = 0;
    rayQuery.finishLOD = 2;
    rayQuery.distanceToFinishLOD = 10000;

    bool advanced = previewSettings.x > 0.5;

    // Per pixel, per frame. The pixel term decorrelates neighbours, so the peel's stochastic alpha
    // and the reflection sample read as noise rather than as a pattern; the frame term is what the
    // accumulate pass averages away over the 64 frames since the camera last moved. The pixel
    // coordinate is rebuilt from the UNJITTERED uv rather than from gl_FragCoord, which bgfx does
    // not expose to a fragment shader on the HLSL/SPIR-V path.
    ivec2 pixel = ivec2(v_texcoord0 * windowRes.xy);
    uint  seed = hashSeed(uvec3(uint(pixel.x), uint(pixel.y), uint(frame)));

    SceneIntersectData sceneHit;
    // What the transparent layers in front of the hit did to the light behind them, and what they
    // emitted on their own account. Both identities under the plain path, which never peels.
    vec3 transmittance = vec3(1.0);
    vec3 glow = vec3(0.0);
    // Only the advanced path needs the whole material; the plain path fetches the colour alone.
    VoxelMaterial material = emptyVoxelMaterial();

    if (advanced) {
        // Sees through transparency to the nearest opaque surface. A scene with no transparent
        // materials takes exactly the plain path's traversal: the peel stops at the first hit, and
        // the material it fetched to decide that is handed back rather than fetched again below.
        PeeledHit peeled = raySceneIntersectPeeled(ray, rayQuery, TRANSPARENT_LAYERS, seed);
        sceneHit = peeled.hit;
        material = peeled.material;
        transmittance = peeled.transmittance;
        glow = peeled.emission;
    } else {
        sceneHit = raySceneIntersect(ray, rayQuery);
    }

    // Trust the march's own hit data rather than re-intersecting foundBox: a fresh
    // slab test disagrees with the march by ULPs on boundary-exact hits and
    // misclassifies them as misses.
    vec3 normal = sceneHit.normal;
    if (sceneHit.foundBox.size < 0.0 || sceneHit.rayT <= 0.0 || dot(normal, normal) < 0.5) {
        // The background, filtered by any transparent layers between it and the camera -- which is
        // how a pane of glass against the sky reads as glass rather than as nothing. Under the plain
        // path the transmittance is exactly one and this is the untouched backdrop it always was.
        gl_FragData[0] = vec4(backgroundColor(ray.direction) * transmittance, 0.0);
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);
        // a = 0 marks "no geometry here". shade.frag tests this rather than the colour
        // target's mask so a background pixel can never be read as a world position at
        // the origin, which an ambient occlusion tap would then treat as an occluder.
        gl_FragData[2] = vec4(0.0, 0.0, 0.0, 0.0);
        // A glowing pane in front of nothing still glows, so this is written on the miss path too.
        // Zero under the plain path, which is what keeps shade.frag's background branch a copy.
        gl_FragData[3] = vec4(glow, 0.0);
        return;
    }

    // Straight from the march's own arithmetic, for the same reason the hit test above
    // trusts it: a position re-derived from a fresh slab test lands off the surface by
    // ULPs, and the occlusion estimator measures exactly that kind of small offset.
    vec3 hitPosition = ray.origin + ray.direction * sceneHit.rayT;

    vec3 albedo;
    if (advanced) {
        // A metal has no diffuse lobe: what it shows is its reflection, tinted by its own albedo,
        // and leaving the albedo underneath would draw a chrome voxel as a bright grey one with a
        // reflection layered over it. The subtraction is the honest preview, and it is also what
        // Render mode does with the same material.
        //
        // A default material is untouched by both terms here -- metallic 0 leaves the albedo whole,
        // and an opaque scene's transmittance is exactly one -- so turning the toggle on does not
        // move the colour of anything that has no material properties set.
        albedo = material.albedo * (1.0 - material.metallic) * transmittance;
        // The surface's own emission and its reflection, both dimmed by whatever transparency sits
        // in FRONT of them, added to the glow the layers themselves contributed.
        glow += transmittance * (material.emission +
                                 specularPreview(hitPosition, normalize(normal), -ray.direction,
                                                 material, sceneHit.foundBox.size, seed));
    } else {
        // The march's own integer cell, not its world-space box: recovering a coordinate back out of
        // world space is a float32 round trip that shades voxels with their neighbour's colour once
        // the chunk has been moved or rotated. See SceneIntersectData::voxelCoord.
        albedo = fetchVoxelColorAtCoord(sceneHit.voxelCoord, sceneHit.headerIndex);
    }

    gl_FragData[0] = vec4(albedo, 1.0);
    // The voxel's edge length travels with the normal because it is what gives the
    // occlusion radius a scene-independent unit -- see shade.frag's AO_RADIUS_VOXELS.
    gl_FragData[1] = vec4(normalize(normal), sceneHit.foundBox.size);
    gl_FragData[2] = vec4(hitPosition, 1.0);
    gl_FragData[3] = vec4(glow, 0.0);
}
