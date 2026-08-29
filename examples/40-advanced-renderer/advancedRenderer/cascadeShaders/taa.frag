$input v_color0
$input v_texcoord0

// =============================================================================
// taa.frag  --  Temporal anti-aliasing of the composed HDR frame.
//
// Resolves the gbuffer's per-frame sub-pixel jitter into anti-aliased edges (and keeps them
// smooth while the camera moves), with one change aimed at the usual "blurry under movement"
// weakness of a reprojecting TAA: the reprojected history is resampled with a sharp 9-tap
// CATMULL-ROM filter instead of plain bilinear. Bilinear refetch re-blurs the history a little
// every single frame, so under sustained motion the image softens; Catmull-Rom is (near) sharpness-
// preserving, so moving edges stay crisp while still being anti-aliased.
//
//   STILL  -> identity accumulation, own texel, long running mean -> supersampled crisp edges.
//   MOVING -> reproject this hit into last frame (worldToUV), Catmull-Rom history fetch, clamp to
//             the current 3x3 colour AABB (deghost), loose position gate, short history.
//
// RUNS AT OUTPUT RESOLUTION, downstream of upscale.frag. Everything before that pass renders at the
// reduced render scale; this one accumulates after the reconstruction, so the jitter it is resolving
// is resolved on the image that actually reaches the screen. Accumulating before the magnify -- which
// is where this used to sit -- cannot work, because the magnify then rebuilds edges from a jittered
// G-buffer every frame with nothing left downstream to average them.
//
// BYPASSED ENTIRELY by the Q key, along with the reconstruction it sits behind -- this is the second
// of the two filters between the render-resolution samples and the screen, so leaving it running would
// undo the point sample upstream. See the early-out at the top of main().
//
// Inputs (FBO 10 [0], FBO 1 [1..6], FBO 9 [7..8]):
//   0 curColor   1 gPos  2 gNormal  3 gAlbedo  4 gDirect  5 gFace  6 gKey   7 histColor  8 histPos
// Outputs (FBO 9, ping-pong):
//   0 accumColor rgb = anti-aliased HDR, a = age    1 histPos (carried for next reprojection)
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(curColor,  0);   // FBO10[0]: the reconstructed frame, at OUTPUT resolution
SAMPLER2D(gPos,      1);   // FBO1[0]: xyz world pos, a = camDist (a < 0 => sky)
SAMPLER2D(gAlbedo,   3);   // FBO1[2]: a = 1 on an animated surface (see gbuffer.frag)
SAMPLER2D(gFace,     5);   // FBO1[4]: xyz voxel FACE CENTRE, w = voxel edge size
// FBO1[1..5] -- gNormal, gAlbedo, gDirect, gFace, gKey -- are bound but unread here, so registers
// 2..6 are skipped and FBO 9's pair lands at 7 and 8. There is deliberately no normal gate: voxel
// silhouettes are a staircase of perpendicular micro-faces that must be allowed to average, which
// is exactly what anti-aliasing them means.
SAMPLER2D(histColor, 7);   // FBO9[0] prev: accumulated HDR + age
SAMPLER2D(histPos,   8);   // FBO9[1] prev: voxel FACE CENTRE, for history validation

uniform vec4 passTargetRes;
// Per bound input, indexed by sampler slot. Needed because this pass now runs at OUTPUT resolution
// while the G-buffer it validates against is still at RENDER resolution -- slot 1 is gPos, so
// passInputRes[1] is the G-buffer's own size.
uniform vec4 passInputRes[8];
// This frame's camera as well as last frame's: comparing the two is what distinguishes a camera that
// moved from a scene that changed, which frameCount.y alone cannot do. See the note in main().
uniform vec4 cameraPos;
uniform vec4 cameraDir;
// z = TAA OFF (the `,` key). x/y are the volumetric god rays; this rides along because debugParams
// has no component left and both are rendering-mode switches rather than debug flags.
//
// WHY THIS EXISTS. The `E` key zeroes the sub-pixel JITTER, and it is easy -- and was -- to read that
// as turning the temporal filter off. It does not: this pass never reads renderParams.w at all. With
// the jitter at zero the history is still reprojected through the previous camera, resampled with a
// nine-tap Catmull-Rom, clamped and blended, so a moving camera still shows temporal blur and every
// artefact reprojection can produce. Those are two separate questions and they now have two separate
// keys.
uniform vec4 volParams;

uniform vec4 prevCameraPos;
uniform vec4 prevCameraDir;
uniform vec4 frameCount;
// x = the upscale bypass (Q key). Read here as well as in upscale.frag because this pass is the OTHER
// filter standing between the render-resolution samples and the screen -- see the early-out in main().
uniform vec4 debugParams;

#define FOV 60.0

// STILL long so jitter fully resolves; MOVING short so reprojected history stays responsive. A bit
// long for a moving history because Catmull-Rom keeps it sharp, so more of it can be held for
// stability without the extra blur a bilinear refetch would compound frame over frame.
#define STILL_MAX_AGE  64.0
#define MOVING_MAX_AGE 12.0
// An ANIMATED surface under a still camera. Short, because the surface itself is going somewhere and a
// long mean of it lags; not one, because a single sample of a jittered ray and a one-sample shadow is
// exactly the noise this pass exists to remove. This is the number that decides how much a swaying
// blade may smear against how much it may flicker.
#define ANIMATED_MAX_AGE 8.0

// Exact inverse of rayStartDirection (same as the reprojection renderer's worldToUV): a world point
// -> the screen UV it projects to under the given camera.
vec2 worldToUV(vec3 P, vec3 camPos, vec3 camDir, vec2 res, float fov, out bool valid) {
    vec3 forward = normalize(camDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));
    vec3 v = P - camPos;
    float z = dot(v, forward);
    if (z <= 1e-4) { valid = false; return vec2(-1.0); }
    float scale  = tan(radians(fov * 0.5));
    float aspect = res.x / res.y;
    float ndcx = (dot(v, right) / z) / (scale * aspect);
    float ndcy = (dot(v, up)    / z) / scale;
    vec2 flip = (vec2(ndcx, ndcy) + 1.0) * 0.5;
    vec2 uv = vec2(flip.x, 1.0 - flip.y);
    valid = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    return uv;
}

// Sharp 9-tap Catmull-Rom history resample (the standard bilinear-optimized form: 5 bilinear
// fetches). Preserves edge sharpness across reprojection, unlike a single bilinear tap.
vec3 sampleCatmullRom(sampler2D tex, vec2 uv, vec2 res) {
    vec2 samplePos = uv * res;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;

    vec2 texPos0  = (texPos1 - 1.0)      / res;
    vec2 texPos3  = (texPos1 + 2.0)      / res;
    vec2 texPos12 = (texPos1 + offset12) / res;

    vec3 result = vec3(0.0);
    result += texture2D(tex, vec2(texPos0.x,  texPos0.y )).rgb * (w0.x  * w0.y );
    result += texture2D(tex, vec2(texPos12.x, texPos0.y )).rgb * (w12.x * w0.y );
    result += texture2D(tex, vec2(texPos3.x,  texPos0.y )).rgb * (w3.x  * w0.y );
    result += texture2D(tex, vec2(texPos0.x,  texPos12.y)).rgb * (w0.x  * w12.y);
    result += texture2D(tex, vec2(texPos12.x, texPos12.y)).rgb * (w12.x * w12.y);
    result += texture2D(tex, vec2(texPos3.x,  texPos12.y)).rgb * (w3.x  * w12.y);
    result += texture2D(tex, vec2(texPos0.x,  texPos3.y )).rgb * (w0.x  * w3.y );
    result += texture2D(tex, vec2(texPos12.x, texPos3.y )).rgb * (w12.x * w3.y );
    result += texture2D(tex, vec2(texPos3.x,  texPos3.y )).rgb * (w3.x  * w3.y );
    return result;
}

void main() {
    vec2 uv = v_texcoord0;

    vec3 curRad = texture2D(curColor, uv).rgb;
    // POINT-SAMPLED, not bilinear. The G-buffer is smaller than this pass's target now, so a plain
    // normalised fetch would interpolate BETWEEN texels -- and interpolating a G-buffer is meaningless
    // in a way that matters: halfway between a sky texel (camDist -1) and a surface texel at 200 lands
    // on a positive depth belonging to nothing, so silhouette pixels would be classified as surfaces
    // at an invented distance. Snapping to the nearest render texel keeps every value one the
    // G-buffer actually wrote.
    vec2 gUV = (floor(uv * passInputRes[1].xy) + 0.5) * passInputRes[1].zw;

    vec4 pos4   = texture2D(gPos, gUV);
    vec3 curP   = pos4.xyz;
    float curD  = pos4.w;
    // The voxel face this pixel is standing on. curP is still what reprojection needs -- it is the
    // actual point in the world this pixel sees -- but the face centre is what history VALIDATION
    // needs, for the reason spelled out at the gate below.
    vec4 curFace = texture2D(gFace, gUV);

    // ---- BYPASS (Q key): NOTHING BETWEEN THE SAMPLES AND THE SCREEN -----------------------------
    // upscale.frag already took its own early-out and point-sampled the render-resolution frame onto
    // this grid, so curColor is the raw samples, blocky, one render texel per output block. This pass
    // would then put a filter back: the running mean integrates the sub-pixel jitter, and the moving
    // path resamples an output-resolution history with a 9-tap Catmull-Rom built out of five BILINEAR
    // fetches. Both soften the blocks, and the second is a filtered magnify in everything but name --
    // which is exactly what the bypass exists to be free of. So the whole chain is off together and
    // what reaches display.frag is one render sample per output pixel, unfiltered, at every camera
    // state.
    //
    // The cost of this pass leaves the A/B along with the filter, which is the one honest caveat:
    // the milliseconds Q moves are upscale.frag's reconstruction plus taa's accumulation, not the
    // reconstruction alone. Everything upstream of upscale is still untouched, so the comparison is
    // still same-work-in, and taa is a cheap pass next to the one being measured.
    //
    // histPos is still written, so the frame after Q is toggled back off validates against a real face
    // centre rather than whatever was last left here. histColor's age comes out as 1, which is what a
    // rejected history means anyway -- accumulation restarts from this frame.
    if (debugParams.x > 0.5) {
        gl_FragData[0] = vec4(curRad, 1.0);
        gl_FragData[1] = vec4(curFace.xyz, curD);
        return;
    }

    // Sky / background (gPos.a < 0): nothing jitter-dependent to resolve, pass through.
    if (curD < 0.0) {
        gl_FragData[0] = vec4(curRad, 1.0);
        gl_FragData[1] = pos4;
        return;
    }

    // ---- WHICH SIGNAL MEANS WHAT ---------------------------------------------------------------
    // This used to be `bool moving = frameCount.y != 0.0`, and that single flag drove all three of
    // reprojection, history length and deghosting. frameCount.y is a "something in the scene changed"
    // fold -- main.cpp raises it for a moved sun and, critically, for the grass sway, which means it is
    // up on EVERY frame the sway runs. So a still camera looking at swaying grass took the moving path
    // forever: a 12-frame history instead of 64, and a Catmull-Rom resample of a history that needed
    // no resampling, every frame, compounding. The +-0.5px Halton jitter this pass exists to resolve
    // therefore never resolved, and showed up as permanent shimmer -- at full resolution as much as
    // at reduced. accumulate.frag already guards against exactly this; see the note at its camMoved.
    //
    // The three behaviours answer three different questions, so they read three different signals:
    //
    //   camMoved      Do I have to REPROJECT? Only camera motion moves a fixed surface to a new pixel,
    //                 and only reprojection needs the sharp resample that costs sharpness to apply.
    //                 It also sets the history length: what makes jitter resolvable is a still camera,
    //                 not a still scene.
    //   sceneChanged  Do I have to DEGHOST? Anything that changes what is visible at a pixel -- a
    //                 displaced blade, a moved sun -- can strand history that no longer belongs, and
    //                 the neighbourhood clamp is the mechanism for that. This is what frameCount.y is
    //                 genuinely good for.
    //
    // The clamp is what lets the long history be safe here: a converged edge pixel holds a blend of
    // both surfaces, and the current 3x3 neighbourhood contains both, so the clamp leaves it alone --
    // while a pixel a blade has swayed off is pulled to the range that is actually there now.
    bool camMoved = any(notEqual(prevCameraPos.xyz, cameraPos.xyz)) ||
                    any(notEqual(prevCameraDir.xyz, cameraDir.xyz));
    bool sceneChanged = frameCount.y != 0.0;

    bool  moving = camMoved;
    bool  deghost = camMoved || sceneChanged;
    // Responsiveness comes from what the surface IS, not from watching it move. gbuffer.frag marks an
    // animated material outright; a static surface then keeps the full history no matter how the
    // jitter wanders across it, which is what stops the image shifting frame to frame.
    float animatedSurface = texture2D(gAlbedo, gUV).a;
    float maxAge = camMoved ? MOVING_MAX_AGE
                            : mix(STILL_MAX_AGE, ANIMATED_MAX_AGE, animatedSurface);

    vec3  accum = curRad;
    float age   = 1.0;

    // TAA OFF: this frame, untouched, and an age of 1 so nothing downstream believes it accumulated.
    // Placed after curFace/curD are resolved so the history target below is still written correctly --
    // switching the filter back on then finds a valid previous frame rather than one frame of garbage.
    if (volParams.z > 0.5) {
        gl_FragData[0] = vec4(curRad, 1.0);
        gl_FragData[1] = vec4(curFace.xyz, curD);
        return;
    }

    if (frameCount.x > 0.0) {
        vec2 sampUV;
        bool valid = true;

        if (moving) {
            sampUV = worldToUV(curP, prevCameraPos.xyz, prevCameraDir.xyz,
                               passTargetRes.xy, FOV, valid);
        } else {
            // Still: identity, own texel centre -> zero resampling blur while jitter converges.
            sampUV = (floor(uv * passTargetRes.xy) + 0.5) / passTargetRes.xy;
        }

        if (valid) {
            float hAge = texture2D(histColor, sampUV).a;
            if (isnan(hAge) || hAge < 1.0) hAge = 1.0;

            // Sharp Catmull-Rom history fetch when moving; identity texel needs no resample.
            vec3 hist = moving ? sampleCatmullRom(histColor, sampUV, passTargetRes.xy)
                               : texture2D(histColor, sampUV).rgb;
            bool sane = !any(isnan(hist));

            if (deghost) {
                // Neighbourhood colour clamp (3x3 of the current frame): the deghoster. History
                // that no longer fits the local colour range (occlusion, edges, or a blade that has
                // swayed off this pixel) is pulled back instead of smearing. No normal gate (voxel
                // micro-face staircases need to average to average). A loose distance-scaled
                // position gate catches gross disocclusion only.
                //
                // Runs on the STILL path too, which is what makes a 64-frame history safe while the
                // grass is moving: it bounds history to what is actually visible now, per pixel,
                // instead of the old approach of disabling convergence for the whole frame because
                // something somewhere in it moved.
                vec2 texel = 1.0 / passTargetRes.xy;
                vec3 cmin = curRad, cmax = curRad;
                for (int y = -1; y <= 1; y++)
                for (int x = -1; x <= 1; x++) {
                    vec3 s = texture2D(curColor, uv + vec2(float(x), float(y)) * texel).rgb;
                    cmin = min(cmin, s);
                    cmax = max(cmax, s);
                }
                hist = clamp(hist, cmin, cmax);

                // ---- HAS THE SURFACE AT THIS PIXEL MOVED? ------------------------------------
                // Compared on the voxel FACE CENTRE, not on the ray hit point, and that is what makes
                // it able to see a swaying blade at all.
                //
                // The hit point is useless for this. It moves with the sub-pixel jitter, and at 200
                // units from the camera that jitter shifts it further than a blade's entire
                // displacement -- so a tolerance loose enough to accept a jittered static surface
                // (the old max(0.5, curD*0.03), six world units out there) accepts a displaced blade
                // too. Which is exactly what happened: moving grass reached a 64-frame history and
                // became a lagging mean of a surface that had moved on, and a lagging mean of moving
                // geometry is precisely the instability it looks like.
                //
                // The face centre is quantised to the voxel, so the jitter does not move it AT ALL --
                // a static voxel's centre is bit-identical frame to frame. A voxel drawn somewhere
                // else moves it by the snap quantum, a whole voxel by default. The two cases stop
                // overlapping, and the tolerance can be tight enough to mean something.
                vec3  hFace = texture2D(histPos, sampUV).xyz;

                // ---- ONLY WHERE A VOXEL IS BIGGER THAN A PIXEL ------------------------------
                // The comparison above is exact only while a voxel covers more than a pixel. Once it
                // is smaller -- which is everything at distance -- the sub-pixel jitter lands on a
                // DIFFERENT FACE every frame all by itself, with nothing having moved. The gate then
                // rejects history on almost every distant pixel, the 1-sample-per-frame sun shadow
                // never accumulates, and the result is the hard flickering that showed up across the
                // far field. The test was not wrong, it was being asked a question that stops having
                // an answer once geometry is finer than the sampling.
                //
                // So weigh it by the voxel's projected size and let it fade out below about a pixel.
                // Under that, face identity carries no information and the colour clamp above is the
                // right validator -- it asks whether the history still fits what is visible, which is
                // a question that survives at any scale.
                float pixelWorld  = curD * (2.0 * tan(radians(FOV * 0.5))) / passTargetRes.y;
                float voxelPixels = curFace.w / max(pixelWorld, 1e-6);
                float geometryIsResolved = smoothstep(0.75, 2.0, voxelPixels);

                // ---- AND ONLY WHERE THE PIXEL IS ACTUALLY ON ONE FACE ------------------------
                // The size test above asks whether a voxel is big enough to be resolved. It does not
                // ask whether THIS pixel sits inside one, and at a silhouette the answer is no however
                // large the voxel is -- which is why the gate failed worst at close range, where
                // voxels are hugest and faceTol is at its floor.
                //
                // A silhouette pixel straddles two surfaces. The jitter samples the foreground on one
                // frame and the background on the next, which is not a disocclusion, it is the
                // sub-pixel signal this whole pass exists to average. The face centre still jumps by
                // the full separation between them, so the gate rejected the history on every such
                // pixel on nearly every frame, age never left 1, and what reached the screen was the
                // raw jittered sample -- permanent edge flicker under a completely still camera.
                //
                // upscale.frag integrated the front face's coverage to shade this pixel and hands it
                // over in alpha. 1 means the pixel is wholly one face and the centre comparison is
                // exact; below that it is a blend and no single identity describes it. The colour
                // clamp above is the validator for those, which is the same argument the size test
                // makes for sub-pixel geometry -- it asks whether the history still fits what is
                // visible, which is a question that survives at any coverage.
                float purity = texture2D(curColor, uv).a;
                float gateStrength = geometryIsResolved * purity;

                // GROSS DISOCCLUSION ONLY, at every camera state. It was briefly tightened to a
                // quarter of a voxel when still, to catch a swaying blade -- and that is what made the
                // whole image visibly shift once the jitter was on. The jitter lands on a neighbouring
                // voxel at any face boundary, moving the centre by about a voxel, which a quarter-voxel
                // tolerance reads as a disocclusion; history was thrown away across most of the frame
                // every frame, so what reached the screen was the raw jittered sample.
                //
                // Displacement is not this test's job any more -- ANIMATED_MAX_AGE handles it from a
                // flag that knows the answer. This one is back to asking only whether the pixel has
                // landed on something else entirely.
                float faceTol = max(curFace.w * 2.0, curD * 0.03);
                // Relaxing the tolerance rather than skipping the test: at the crossover a voxel is
                // about a pixel and the answer is genuinely partial, so it should fade rather than
                // switch, or the transition becomes a visible band at a fixed distance.
                //
                // DIVIDED, not lerped towards a large number. `mix(1e9, faceTol, s)` reads like a fade
                // and is a step: at s = 0.99 it still returns 1e7, so the gate was fully off until s
                // hit exactly 1.0 and then snapped to its true tolerance. Dividing gives the fade the
                // comment above always described -- half confidence is twice the tolerance -- and it
                // is monotone in s with no discontinuity at either end.
                faceTol /= max(gateStrength, 1e-4);
                if (distance(curFace.xyz, hFace) >= faceTol) sane = false;
            }

            if (sane) {
                age = min(min(hAge, maxAge) + 1.0, maxAge);
                accum = mix(hist, curRad, 1.0 / age);   // equal-weight running mean
            }
        }
    }

    gl_FragData[0] = vec4(accum, age);
    // The FACE CENTRE, not the hit point: this target exists only to validate next frame's history,
    // and the gate above compares centres. Storing the hit point instead is what left the gate unable
    // to tell a displaced voxel from a jittered ray. Its .w keeps camDist, which nothing reads today
    // but which costs nothing to carry and is the natural companion to a position.
    gl_FragData[1] = vec4(curFace.xyz, curD);
}
