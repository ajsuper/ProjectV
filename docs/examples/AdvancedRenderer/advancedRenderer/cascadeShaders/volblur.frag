$input v_color0
$input v_texcoord0

// =============================================================================
// volblur.frag  --  The anisotropic filter that makes one shadow ray per sample usable.
//
// volumetric.frag produces a correct but noisy estimate: three real shadow rays per pixel is three
// samples of an integral, and it looks like it. The obvious fix is a wide blur, and a wide ISOTROPIC
// blur would destroy the one thing the volumetric pass was bought for -- shaft edges that land exactly
// where the geometry casting them is. Blur it round and it becomes the screen-space version, only
// slower.
//
// The signal has a structure that lets both be had. Every point on a view ray that ends at the sun
// projects onto the SAME LINE in screen space: the line from this pixel to the sun's screen position.
// So a shaft is nearly constant ALONG that line and changes sharply only ACROSS it. Averaging a long
// way along it therefore removes noise without touching a single edge, and the perpendicular pass can
// stay two or three taps wide -- just enough to knit neighbouring lines together.
//
// That is the whole idea: filter the signal along the direction it is smooth in, and leave the
// direction it is sharp in alone.
//
// THE PERPENDICULAR IS ALSO BILATERAL, on depth. A shaft's brightness depends on how much lit air is
// in front of the surface, so it changes abruptly at a silhouette -- and a filter that averaged across
// one would drag the glow of distant air onto a near object's edge, which reads as a halo that does
// not belong to anything. Weighting by depth similarity keeps the average inside one surface.
//
// Input (FBO 14): 0 the noisy volumetric estimate.
// Input (FBO 1):  1 gPos, for the depth the bilateral weight is built from.
// Output (FBO 15): 0 the filtered shafts, for display.frag to add into the linear HDR frame.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(volRaw, 0);
SAMPLER2D(gPos,   1);

#define CASCADE_SCREEN_RES passTargetRes.xy
#define CASCADE_ATLAS_RES  passTargetRes.xy
// SUN_DIR arrives with this, which already includes pjv_sun_sky.sc -- that header has no include
// guard, so pulling it in again here defines every function in it twice.
#include <pjv_cascade_common.sc>

uniform vec4 volParams;

// ---- Knobs ------------------------------------------------------------------------------------
// Taps ALONG the line to the sun, each way. This is the axis doing the denoising, so it is long --
// and it was not long enough. The argument for this filter says the signal is nearly CONSTANT along
// this line, so there is almost nothing to lose by reaching further and a great deal of noise to
// gain against; 8 taps over 4% of the frame height was leaving most of that on the table.
//
// "Almost" rather than "nothing": the inscatter does vary along the line, because the view ray's
// direction and its distance to the surface both change along it. That variation is smooth and slow,
// which is what makes a long kernel safe, but it is not zero -- so this is bounded by taste rather
// than by principle, and 16 is where the shafts stop getting cleaner without starting to look painted.
#define VOL_BLUR_ALONG 16
// ...and across it. Two or three is the point: any more and shaft edges start to soften.
#define VOL_BLUR_ACROSS 1
// How far the kernel reaches along the shaft, as a fraction of the FRAME HEIGHT rather than in
// texels -- so the filter covers the same part of the picture whatever resolution this pass runs at.
//
// That matters because the target's `scale` in resources.json is the obvious dial to reach for when
// the low-resolution grid becomes visible under motion, and expressing the kernel in texels would
// have quietly narrowed the filter every time it was raised: more pixels, same tap count, less
// denoising, and a different look from what was being tuned. Derived from the tap count so the two
// stay consistent.
#define VOL_BLUR_REACH 0.10
// Gaussian falloff along the line, in tap units. Kept comparable to VOL_BLUR_ALONG so the kernel
// actually reaches its full length rather than dying halfway -- raising the tap count without raising
// this would have added taps that carry almost no weight and cost the same as the ones that do.
#define VOL_BLUR_SIGMA 9.0
// Depth tolerance for the bilateral, relative to the pixel's own distance.
#define VOL_BLUR_DEPTH 0.12
// Below this |dot(SUN_DIR, forward)| the vanishing point is effectively at infinity and projecting it
// divides by a near-zero depth. ~0.08 is about 85 degrees off the view axis.
#define VOL_PARALLEL_EPS 0.08

void main() {
    if (volParams.x < 0.5) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, -1.0);
        return;
    }

    vec2 uv    = v_texcoord0;
    vec2 texel = passTargetRes.zw;
    vec2 res   = passTargetRes.xy;

    // Resolution-independent tap spacing; see VOL_BLUR_REACH.
    float stepPixels = (VOL_BLUR_REACH * res.y) / float(VOL_BLUR_ALONG);

    // ---- THE AXIS THE SHAFTS RUN ALONG, IN ANY CAMERA ORIENTATION ------------------------------
    // For a directional light this is the line from the pixel to the light's VANISHING POINT, where
    // every ray parallel to the sun converges on screen. Two cases the first version got wrong:
    //
    //   SUN BEHIND THE CAMERA. worldToUV bails out with a (-1,-1) sentinel when the projected point
    //   is behind the near plane, and normalising toward that gives a fixed diagonal unrelated to any
    //   shaft -- so the filter smeared along a direction the signal is not smooth in. The vanishing
    //   point of -SUN_DIR is the answer: it names the SAME family of screen lines, and the kernel is
    //   symmetric in s, so which end of the line it points at makes no difference.
    //
    //   SUN PERPENDICULAR TO THE VIEW. There is no vanishing point on any finite plane -- the shafts
    //   are parallel on screen rather than convergent -- and projecting one divides by a depth near
    //   zero. That case is computed straight from the camera basis instead. The two agree in the
    //   limit: just past the threshold the vanishing point is far off screen, so the direction toward
    //   it is already almost constant across the frame, which is what "parallel" means.
    vec3 forward = normalize(cameraDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 camRight = normalize(cross(forward, worldUp));
    vec3 camUp    = normalize(cross(camRight, forward));

    float zSun = dot(SUN_DIR, forward);

    vec2 axis;
    if (abs(zSun) > VOL_PARALLEL_EPS) {
        bool unused;
        vec2 vpUV = worldToUV(cameraPos.xyz + SUN_DIR * (zSun > 0.0 ? 1.0 : -1.0),
                              cameraPos.xyz, cameraDir.xyz, res, FOV, unused);
        // In PIXELS, so the kernel is not stretched by the aspect ratio.
        axis = (uv - vpUV) * res;
    } else {
        // The sun's own direction projected onto the screen axes. y is negated because uv runs down
        // the screen while the camera's up runs up it.
        axis = vec2(dot(SUN_DIR, camRight), -dot(SUN_DIR, camUp));
    }

    float len = length(axis);
    // Degenerate only AT the vanishing point itself, where every direction is equally good and the
    // shaft is a disc anyway.
    vec2 along  = len > 1e-4 ? axis / len : vec2(1.0, 0.0);
    vec2 across = vec2(-along.y, along.x);

    float centreDepth = texture2D(gPos, pjvSnapToTexel(uv, passInputRes[1].xy)).a;

    vec3  sum    = vec3(0.0);
    float weight = 0.0;

    for (int a = -VOL_BLUR_ACROSS; a <= VOL_BLUR_ACROSS; a++)
    for (int s = -VOL_BLUR_ALONG;  s <= VOL_BLUR_ALONG;  s++) {
        vec2 offsetPixels = along * (float(s) * stepPixels) + across * float(a);
        vec2 tapUV = clamp(uv + offsetPixels * texel, vec2(0.0), vec2(1.0));

        // Gaussian along the shaft; the perpendicular taps are weighted flat, because there are only
        // three of them and their job is to join neighbouring lines rather than to blur.
        float w = exp(-float(s * s) / (2.0 * VOL_BLUR_SIGMA * VOL_BLUR_SIGMA));

        // Bilateral on depth, so the average never crosses a silhouette.
        float tapDepth = texture2D(gPos, pjvSnapToTexel(tapUV, passInputRes[1].xy)).a;
        // Sky matches sky, and a surface matches a surface at a similar distance. Comparing the two
        // directly would call a sky texel (-1) infinitely close to everything.
        bool bothSky = (tapDepth < 0.0) && (centreDepth < 0.0);
        if (!bothSky) {
            if ((tapDepth < 0.0) != (centreDepth < 0.0)) continue;   // one is sky, the other is not
            if (abs(tapDepth - centreDepth) > centreDepth * VOL_BLUR_DEPTH) continue;
        }

        sum    += texture2D(volRaw, tapUV).rgb * w;
        weight += w;
    }

    // Normalised by the weight that SURVIVED here, not by the full kernel -- the opposite of the
    // choice the bloom makes, and for the opposite reason. A rejected tap there meant "no light comes
    // from this direction", which is information. A rejected tap here means "this sample belongs to a
    // different surface and I know nothing about it", which is not: counting it as darkness would draw
    // a dark seam along every silhouette, which is precisely what the bilateral is there to avoid.
    // Alpha carries `centreDepth` through, because display.frag magnifies THIS buffer -- not the raw
    // one -- and needs the depth to reject a tap from a different surface. This pass is already
    // bilateral on the same depth, so the value is in hand; writing 1.0 here threw it away and left
    // the final magnify depth-blind, which is the one filter in the chain that could not afford to be.
    gl_FragData[0] = vec4(weight > 1e-5 ? sum / weight : texture2D(volRaw, uv).rgb, centreDepth);
}
