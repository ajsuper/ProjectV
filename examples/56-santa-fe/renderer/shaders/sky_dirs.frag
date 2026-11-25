$input v_color0
$input v_texcoord0

// =============================================================================
// sky_dirs.frag -- the sky's light, direction by direction, once a frame (santa-fe-reference S23a/b).
//
// A 64 x 7 target. Rows 0-5: the hemisphere round +x, -x, +y, -y, +z, -z, one fixed cosine-weighted direction a
// texel, and what a diffuse ray from the camera that leaves the scene sees along it --
//   up:   the volumetric clouds themselves (sfCloudMarch, the camera rays' own march, fewer steps): a thick
//         deck's base is dark and opaque, a thin one lets the sky through; then the sky behind them
//   down: the land where the direction meets it (the far field's height under the camera), lit by the sun
//         through the clouds' shadow there, through the air
// Row 6: the clouds' shadow on the land round the camera, 64 points out to SF_OVERCAST_RADIUS: how much of
// the sun reaches it (the overcast). sky_ambient.frag averages each row.
//
// Inputs: cloudNoise 0, the far field 1-4, the cloud-shadow map 5. Output: rgb the radiance (row 6: r the
// transmittance).
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>
#include <pjv_camera.sc>
#include <pjv_color.sc>

SAMPLER2D(cloudNoise, 0);
SAMPLER2D(farHeight, 1);
SAMPLER2D(farColour, 2);
SAMPLER2D(farState, 3);
SAMPLER2D(farCanopy, 4);
SAMPLER2D(cloudShadowMap, 5);
#define SF_HAS_CLOUD_SHADOW_MAP 1
#define SF_SKY_TEX_SLOT 6   // sky_ambient.frag's last frame (the land's light under the clouds)

uniform vec4 renderParams;   // y: the camera rays' cloud steps

#define PI 3.14159265358979
#include <mountains_sky.sc>
#include <mountains_far.sc>
#include <santa_fe_clouds.sc>

#define SF_SKY_SAMPLES 64
#define SF_OVERCAST_RADIUS 9000.0   // m

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    int i = at.x;
    int row = at.y;
    vec3 o = pjvViewPosition();
    float groundH;
    int ring;
    if (!mtnFarHeight(o.xz, groundH, ring)) groundH = o.y - 50.0;
    if (row == 6) {
        // The overcast: the clouds' shadow at 64 points of a disc round the camera (a golden spiral).
        float r = sqrt((float(i) + 0.5) / float(SF_SKY_SAMPLES)) * SF_OVERCAST_RADIUS;
        float a = float(i) * 2.39996323;
        vec3 p = vec3(o.x + cos(a) * r, groundH, o.z + sin(a) * r);
        gl_FragColor = vec4(sfCloudShadow(p, SUN_DIR, 0.5), 0.0, 0.0, 1.0);
        return;
    }
    vec3 axis = row == 0 ? vec3(1.0, 0.0, 0.0) : row == 1 ? vec3(-1.0, 0.0, 0.0) : row == 2 ? vec3(0.0, 1.0, 0.0)
              : row == 3 ? vec3(0.0, -1.0, 0.0) : row == 4 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 0.0, -1.0);
    vec3 t = abs(axis.y) < 0.99 ? normalize(cross(axis, vec3(0.0, 1.0, 0.0))) : vec3(1.0, 0.0, 0.0);
    vec3 b = cross(axis, t);
    float u1 = (float(i) + 0.5) / float(SF_SKY_SAMPLES);
    float phi = float(i) * 2.39996323;
    float rr = sqrt(u1);
    vec3 d = normalize(t * (rr * cos(phi)) + b * (rr * sin(phi)) + axis * sqrt(max(0.0, 1.0 - u1)));
    vec3 L;
    if (d.y > 0.0) {
        vec3 skyUp = mtnSkyFrom(o, vec3(0.0, 1.0, 0.0));
        float tCloud;
        vec4 cl = sfCloudMarch(o, d, 1.0e9, SUN_DIR, mtnSunAir.rgb, skyUp, mtnGroundUp(), 0.5,
                               max(int(renderParams.y) / 2, 8), tCloud);
        L = mtnSkyFrom(o, d) * cl.a;
        if (cl.a < 1.0) {
            vec3 T, Lair;
            mtnAtmosphere(o, d, tCloud, T, Lair);
            L += cl.rgb * T + Lair * mtnAirLit() * (1.0 - cl.a);
        }
    } else {
        float dist = clamp((o.y - groundH) / max(-d.y, 0.02), 1.0, 4.0e5);
        vec3 p = o + d * dist;
        vec3 T, Lair;
        mtnAtmosphere(o, d, dist, T, Lair);
        L = T * MTN_GROUND_ALBEDO * SUN_COLOR * max(SUN_DIR.y, 0.0) * 0.3183 * sfCloudShadow(p, SUN_DIR, 0.5) + Lair * mtnAirLit();
    }
    if (any(isnan(L)) || any(isinf(L))) L = vec3(0.0, 0.0, 0.0);
    gl_FragColor = vec4(max(L, vec3(0.0, 0.0, 0.0)), 1.0);
}
