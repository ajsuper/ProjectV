$input v_color0
$input v_texcoord0

// =============================================================================
// compose.frag  --  Pass 8. Puts the frame back together.
//
//   hdr = gDirect + gAlbedo * R
//
// gDirect (soft-shadowed sun + emission) and gAlbedo are full-resolution and untouched by any
// temporal filter. R is the indirect irradiance, and this pass is where it becomes FLAT PER FACE.
//
// -----------------------------------------------------------------------------
// HOW ONE VALUE IS CHOSEN FOR A WHOLE FACE, WITHOUT BEING ABLE TO SCATTER
// -----------------------------------------------------------------------------
// The indirect light on a voxel face is one number. The cascade gather already treats it that way
// -- every probe on a face traces from the same origin -- but accumulate stores its result per
// screen pixel, so the pixels covering one face hold near-identical values rather than identical
// ones, and the small differences read as shimmer on a large flat wall.
//
// A per-face store would fix that at the source, and cannot be built here: a fragment shader
// writes the pixel it rasterizes, so nothing can scatter a face's radiance into a slot indexed by
// its identity. What CAN be done is to make every pixel on a face agree on which pixel to read:
//
//   Project the FACE CENTRE through this frame's camera. Every pixel on the face has the same
//   gFace, and the camera is the same for all of them, so they all compute the same UV, snap it to
//   the same texel, and read the same accumulated R. Exactly flat, by construction, with no
//   scatter and no second buffer.
//
// The lookup is checked, not trusted: read the key at that texel and require it to be this face.
// That single test rejects all three ways it can go wrong -- the face centre falling off screen
// (a face clipped at the frame edge), the centre being hidden behind nearer geometry (a face seen
// edge-on around a corner), and the face being too small for its centre texel to belong to it.
//
// The fallback is the per-pixel bilateral filter this pass used to do unconditionally, which is
// the right answer in the case that triggers it most: a face smaller than a pixel, where "flat per
// face" has no meaning and averaging neighbours is what you want anyway.
//
// Note that the choice is uniform across a face -- it depends only on the face centre, the camera
// and the face's projected size, none of which vary per pixel -- so a face is never part flat and
// part filtered, and no seam can appear down the middle of one.
//
// Inputs (FBO 7 [0..2], FBO 5 [3], FBO 1 [4..9]):
//   0 accumR  1 histKey  2 histPos   3 cascade0   4 gPos 5 gNormal 6 gAlbedo 7 gDirect
//   8 gFace 9 gKey
//
// cascade0 is read BEFORE the G-buffer, which is why render.json lists this pass's inputs as
// [7, 5, 1] rather than the [7, 1, 5] the graph order would suggest. Behind FBO 1's six
// attachments it landed at slot 9, and the engine only publishes passInputRes for slots below
// PROJV_MAX_PASS_INPUTS (8) -- so the atlas, which is a quarter of this pass's own size, would
// have had no size to address itself by.
// Output (FBO 8): 0 hdr composite, for taa to anti-alias and display to tone map.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(accumR,   0);   // FBO7[0]: temporally accumulated indirect irradiance R, a = age
SAMPLER2D(cascade0, 3);   // FBO5[0]: the directional radiance atlas, for the rough reflection
SAMPLER2D(gPos,     4);   // FBO1[0]: xyz world pos, a = camDist (a < 0 => sky)
SAMPLER2D(gNormal,  5);   // FBO1[1]: xyz normal, a = voxel edge size
SAMPLER2D(gAlbedo,  6);   // FBO1[2]
SAMPLER2D(gDirect,  7);   // FBO1[3]: direct sun + emission (or sky colour for background)
SAMPLER2D(gFace,    8);   // FBO1[4]: voxel face centre, a = voxel edge size
SAMPLER2D(gKey,     9);   // FBO1[5]: exact face identity

// Must match the cascades so the probe helpers index the same grid.
#define PROBE_SPACING0 16
// Writes at screen resolution, reads the quarter-size atlas -- as resolve does.
#define CASCADE_SCREEN_RES passTargetRes.xy
#define CASCADE_ATLAS_RES  pjvResOr(passInputRes[3].xy, passTargetRes.xy)
#include <pjv_cascade_common.sc>
#include <pjv_face_key.sc>

// ---- Rough reflection knobs -------------------------------------------------
#define SPEC_SHININESS 1.0    // glossy-lobe exponent. LOW = rough/wide, which is all ~16 directions
                              // per probe can represent; raising it aliases into the direction grid.
#define SPEC_STRENGTH  0.0    // overall reflection intensity (taste). 0 = off, the default.
#define SPEC_F0        0.04   // dielectric normal-incidence reflectance, for the Fresnel term.

// Lobe-weighted mean radiance of one cascade-0 probe around the reflection direction. Same shape as
// resolve's cosine-weighted integration, with a Phong lobe centred on refl instead of the cosine.
vec3 probeSpecRadiance(ivec2 probe, vec3 refl, vec2 jitter) {
    int D0 = dirTileOf(0);
    vec3  accRad = vec3(0.0);
    float accW   = 0.0;
    for (int dy = 0; dy < D0; dy++)
    for (int dx = 0; dx < D0; dx++) {
        ivec2 tile = ivec2(dx, dy);
        vec3  dir  = dirForTile(tile, D0, jitter);
        float w    = pow(max(dot(dir, refl), 0.0), SPEC_SHININESS);
        ivec2 px   = probe * D0 + tile;
        accRad += texture2D(cascade0, (vec2(px) + 0.5) / CASCADE_ATLAS_RES).rgb * w;
        accW   += w;
    }
    return accRad / max(accW, 1e-3);
}

// Bilateral-bilinear over the four nearest cascade-0 probes, each contributing its lobe-weighted
// radiance. Same probe layout and geometry weighting as the diffuse resolve.
vec3 specularReflection(vec2 uv, vec3 P, vec3 N, vec3 refl) {
    int   s0 = probeSpacingOf(0);
    ivec2 probesN = ivec2(CASCADE_SCREEN_RES) / s0;
    vec2  gpc = floor(uv * CASCADE_SCREEN_RES) / float(s0) - 0.5;
    vec2  f   = fract(gpc);
    ivec2 b   = ivec2(floor(gpc));
    vec2  jitter = cascadeJitter();

    ivec2 off[4];
    off[0] = ivec2(0, 0); off[1] = ivec2(1, 0);
    off[2] = ivec2(0, 1); off[3] = ivec2(1, 1);
    float bw[4];
    bw[0] = (1.0 - f.x) * (1.0 - f.y); bw[1] = f.x * (1.0 - f.y);
    bw[2] = (1.0 - f.x) * f.y;         bw[3] = f.x * f.y;

    vec3  acc = vec3(0.0);
    float accW = 0.0;
    vec3  accPlain = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        ivec2 probe = clamp(b + off[i], ivec2(0, 0), probesN - 1);
        vec2  puv = pjvSnapToTexel((vec2(probe) + 0.5) * float(s0) * passTargetRes.zw,
                                   CASCADE_SCREEN_RES);
        vec4  pgp = texture2D(gPos, puv);
        vec3  pN  = normalize(texture2D(gNormal, puv).xyz);
        vec3  r   = probeSpecRadiance(probe, refl, jitter);
        float w   = bw[i] * geomWeight(P, N, pgp.xyz, pN, pgp.a);
        acc      += r * w;
        accW     += w;
        accPlain += r * bw[i];
    }
    return accW > 1e-4 ? acc / accW : accPlain;
}

// A face must be at least this many pixels across before the flat path is tried. This is only a
// cheap early-out: below it the face centre's texel is shared with neighbouring faces and the key
// test would reject it anyway, so the test just skips a projection and a fetch.
//
// It is deliberately 1.0 rather than something larger. A size threshold is a CAMERA-DEPENDENT
// switch between two visibly different reconstructions, so a face crosses it as you walk towards it
// and changes appearance for no reason in the scene. At 1.0 the switch happens where the two paths
// agree most closely -- a face about a pixel across is nearly all there is inside a 3x3 filter --
// so it is the least visible place to put it, and the key check does the real rejecting.
#define FLAT_MIN_FACE_PIXELS 1.0

// Fallback filter: light depth/normal-guided bilateral blur of R, used only where the flat lookup
// does not apply. Radius 1 = 3x3, deliberately small -- the temporal mean is the primary denoiser.
#define FILTER_RADIUS 1
// Age-guided a-trous spread. accumR.a carries the temporal age. Where history is thin (freshly
// disoccluded, age near 1) the temporal filter has not kicked in yet and the pixel would show the
// raw undersampled GI, so the same 3x3 taps are SPREAD wider and pulled back in as it converges.
// Tap count is unchanged, so a converged pixel pays nothing extra.
#define FILTER_MAX_STRIDE 4.0
#define FILTER_AGE_FULL   16.0

// The per-pixel path: what this pass did for every pixel before the faces had identities.
vec3 filteredIndirect(vec2 uv, vec3 P, vec3 N) {
    // abs: accumulate uses the sign to mark a face no probe has landed on yet.
    float age = abs(texture2D(accumR, uv).a);
    float t   = clamp(1.0 - (age - 1.0) / (FILTER_AGE_FULL - 1.0), 0.0, 1.0);
    float stride = mix(1.0, FILTER_MAX_STRIDE, t);

    vec3  acc  = vec3(0.0);
    float accW = 0.0;
    for (int dy = -FILTER_RADIUS; dy <= FILTER_RADIUS; dy++)
    for (int dx = -FILTER_RADIUS; dx <= FILTER_RADIUS; dx++) {
        vec2 sUV = uv + vec2(float(dx), float(dy)) * passTargetRes.zw * stride;
        vec4 sgp = texture2D(gPos, sUV);
        vec3 sN  = normalize(texture2D(gNormal, sUV).xyz);
        float w  = geomWeight(P, N, sgp.xyz, sN, sgp.a);
        acc  += texture2D(accumR, sUV).rgb * w;
        accW += w;
    }
    return accW > 1e-4 ? acc / accW : texture2D(accumR, uv).rgb;
}

void main() {
    vec2 uv = v_texcoord0;
    vec4 gp = texture2D(gPos, uv);
    vec3 direct = texture2D(gDirect, uv).rgb;

    // Sky / background: no indirect term at all, gDirect holds the sky colour.
    if (gp.a < 0.0) {
        gl_FragData[0] = vec4(direct, 1.0);
        return;
    }

    vec3 P = gp.xyz;
    vec3 N = normalize(texture2D(gNormal, uv).xyz);
    vec3 albedo = texture2D(gAlbedo, uv).rgb;

    vec4  face = texture2D(gFace, uv);
    vec4  key  = texture2D(gKey,  pjvSnapToTexel(uv, CASCADE_SCREEN_RES));
    vec3  faceCentre = face.xyz;
    float voxelSize  = face.a;

    // How many pixels the face spans, from its own size and the distance to its centre. The
    // projection scale is the same one rayStartDirection uses, so this agrees with what is
    // actually on screen. Uniform across the face: every term comes from the face, not the pixel.
    float centreDepth = max(dot(faceCentre - cameraPos.xyz, normalize(cameraDir.xyz)), 1e-4);
    float projectionScale = passTargetRes.y * 0.5 / tan(radians(FOV * 0.5));
    float facePixels = voxelSize * projectionScale / centreDepth;

    vec3 R;
    bool perFace = false;   // not `flat` -- that is an interpolation qualifier in GLSL

    if (facePixels >= FLAT_MIN_FACE_PIXELS) {
        bool onScreen;
        vec2 faceUV = worldToUV(faceCentre, cameraPos.xyz, cameraDir.xyz,
                                passTargetRes.xy, FOV, onScreen);
        if (onScreen) {
            // Snap to the texel centre so every pixel on the face lands on the same texel rather
            // than on whichever side of a boundary its own rounding falls.
            vec2 snapUV = (floor(faceUV * passTargetRes.xy) + 0.5) * passTargetRes.zw;
            // The check: is the pixel at the face's centre actually looking at this face? If a
            // nearer surface occludes it, or the centre is not quite inside the visible part of
            // the face, this fails and the per-pixel path takes over.
            if (pjvSameFace(texture2D(gKey, snapUV), key)) {
                R = texture2D(accumR, snapUV).rgb;
                perFace = true;
            }
        }
    }

    if (!perFace) {
        R = filteredIndirect(uv, P, N);
    }

    // ---- Rough specular, from the same cascade -------------------------------------------
    // Deliberately still PER PIXEL, and deliberately outside everything above. Specular is
    // view-dependent: it is the one term that genuinely differs from one pixel of a face to the
    // next, so flattening it to the face would be wrong in a way flattening the diffuse is not.
    // It is also read fresh from cascade 0 rather than from the accumulated buffer, because the
    // temporal pass reprojects by world position -- correct for a view-independent quantity, and a
    // smear on a highlight that is supposed to slide across the surface as you move.
    vec3 toEye = normalize(cameraPos.xyz - P);
    vec3 refl  = reflect(-toEye, N);
    float fres = SPEC_F0 + (1.0 - SPEC_F0) * pow(1.0 - max(dot(N, toEye), 0.0), 5.0);
    vec3 specular = specularReflection(uv, P, N, refl) * (fres * SPEC_STRENGTH);

    // Direct light and albedo are full-resolution and per-pixel; only the diffuse indirect is
    // per-face. That is the same bet the demodulation upstream makes -- indirect light is low
    // frequency, so quantizing it to the surface costs nothing you can see, while quantizing
    // albedo or a shadow edge would be obvious immediately.
    gl_FragData[0] = vec4(direct + albedo * R + specular, 1.0);
}
