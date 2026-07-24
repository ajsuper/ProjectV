$input v_color0
$input v_texcoord0

#include <bgfx_shader.sh>
#include <pjv_utils_DDA.sc>

uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 windowRes;

#define FOV 60.0

void main() {
    vec2 uv = v_texcoord0;

    Ray ray;
    ray.origin = cameraPos.xyz;
    ray.direction = rayStartDirection(uv, windowRes.xy, cameraPos.xyz, normalize(cameraDir.xyz), FOV);

    RayQuery rq;
    rq.maxRaySteps = 256u;
    rq.startLOD = 0;
    rq.finishLOD = 2;
    rq.distanceToFinishLOD = 5000;

    SceneIntersectData sceneHit = raySceneIntersect(ray, rq);
    vec3 nrm = sceneHit.normal;
    if (sceneHit.foundBox.size < 0.0 || sceneHit.rayT <= 0.0 || dot(nrm, nrm) < 0.5) {
        gl_FragColor = vec4(0.68, 0.85, 0.90, 1.0);
        return;
    }

    vec3 voxelColor = fetchVoxelColor(sceneHit.foundBox, sceneHit.headerIndex);
    float ambientDot = dot(nrm, normalize(vec3(1, 1, 1))) + 0.4 + 0.2;

    Ray sunRay;
    sunRay.origin = ray.origin + ray.direction * sceneHit.rayT;
    sunRay.direction = normalize(vec3(1, 1, 1));
    sceneHit = raySceneIntersect(sunRay, rq);
    vec3 sunColor = vec3(1);
    if (sceneHit.foundBox.size < 0.0 || sceneHit.rayT <= 0.0) {
        sunColor = vec3(2);
    }

    gl_FragColor = vec4((voxelColor * ambientDot) * sunColor, 1.0);
}
