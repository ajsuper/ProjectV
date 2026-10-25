$input v_color0
$input v_texcoord0

// gbuffer.frag -- pass 1, render resolution: the camera ray and one sun shadow ray, through one
// restirTrace call site. The shadow ray goes straight at the sun's centre, not to a random point of its
// disc: hard shadows, but no noise in the direct light. (The bounces' sun rays, in sample.frag, still
// sample the disc; their light is denoised.)
//
// Each texel is traced through the centre of one of the window pixels it covers (jitterParams: 2x2 of
// them at half scale, in turn), so over four frames every window pixel is sampled exactly, and a still
// view repeats the same four: the resolve settles to them and stays.
//
// The sun's light is not added here: shadow_soft.frag softens the shadow first (a penumbra sized by the
// distance to what casts it), so this pass hands on whether the sun is seen and, if not, how far away the
// blocker is.
//
// Outputs (in the slots the voxel renderer's reconstruction reads them from):
//   0 gPos     xyz the hit, w distance (< 0 sky)
//   1 gNormal  xyz the face's normal, w the sun's blocker: its distance along the shadow ray, -1 none
//   2 gAlbedo  rgb albedo, a the sun's visibility (1 seen, 0 blocked or facing away)
//   3 gDirect  emission; the sky's radiance for a miss
//   4 gFace    xyz the face's centre, w the voxel's edge (0 sky)
//   5 gRot     the chunk's rotation

#include <bgfx_shader.sh>
#include "restir_trace.sc"

void main() {
    vec2 uv = v_texcoord0 + jitterParams.xy * passTargetRes.zw;
    vec3 direction = rayStartDirection(uv, passTargetRes.xy, cameraPos.xyz, normalize(cameraDir.xyz), RESTIR_FOV);
    vec3 toSun = normalize(SUN_DIR);

    vec3 origin = cameraPos.xyz;
    vec3 rayDir = direction;
    int kind = RESTIR_KIND_CAMERA;
    vec4 position = vec4(cameraPos.xyz + direction * 1.0e4, -1.0);
    vec3 normal = vec3(0.0, 0.0, 0.0);
    vec3 albedo = vec3(0.0, 0.0, 0.0);
    vec3 direct = restirSkyWithSun(direction);
    vec4 face = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 rotation = vec4(0.0, 0.0, 0.0, 1.0);
    float facing = 0.0;
    float visible = 0.0;
    float blocker = -1.0;
    int rays = min(int(restirSettings.x), 2);
    for (int i = 0; i < rays; i++) {
        RestirHit hit = restirTrace(origin, rayDir, kind, 1e30);
        if (kind == RESTIR_KIND_CAMERA) {
            if (!hit.hit) break;
            normal = dot(hit.normal, hit.normal) > 0.5 ? normalize(hit.normal) : -direction;
            position = vec4(cameraPos.xyz + direction * hit.t, hit.t);
            albedo = hit.albedo;
            direct = hit.emission;
            face = hit.face;
            rotation = hit.rotation;
            facing = dot(normal, toSun);
            if (facing <= 0.0) break;
            origin = position.xyz + normal * restirSettings.y;
            rayDir = toSun;
            kind = RESTIR_KIND_SHADOW;
        } else {
            visible = hit.hit ? 0.0 : 1.0;
            blocker = hit.hit ? hit.t : -1.0;
        }
    }
    gl_FragData[0] = position;
    gl_FragData[1] = vec4(normal, blocker);
    gl_FragData[2] = vec4(albedo, visible);
    gl_FragData[3] = vec4(direct, 1.0);
    gl_FragData[4] = face;
    gl_FragData[5] = rotation;
}
