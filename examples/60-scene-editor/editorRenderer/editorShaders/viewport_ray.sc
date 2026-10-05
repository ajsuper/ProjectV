// =============================================================================
// viewport_ray.sc  --  The viewport's primary ray, for every pass that has to agree on it exactly.
//
// albedo.frag casts it; reconstruct_common.sc rebuilds it per pixel to re-intersect the coarse trace's
// voxels; shade.frag rebuilds it per occlusion tap to turn a stored ray distance back into the point
// it hit. Each of them has to produce the same ray for the same pixel and frame -- to the bit, for the
// reconstruction -- so there is one copy of it.
//
// The including shader declares cameraPos, cameraDir, cameraProjection, frameCount and passTargetRes
// first. passTargetRes has to be the viewport's full resolution, which it is for all three.
// =============================================================================

#ifndef FOV
#define FOV 60.0
#endif

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

// This frame's sub-pixel jitter, in pixels of whichever target the caller is writing.
vec2 viewportJitter() {
    int frame = int(frameCount.x);
    return vec2(halton(frame + 1, 2), halton(frame + 1, 3)) - 0.5;
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
        ray.direction = rayStartDirection(uv, passTargetRes.xy, cameraPos.xyz, forward, FOV);
        return ray;
    }

    // Identical to rayStartDirection's basis, including the +Z fallback for a view
    // pointing straight up or down. The two must agree exactly: the editor projects
    // its outlines and gizmo onto this image with the same construction on the CPU.
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));

    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float aspectRatio = passTargetRes.x / passTargetRes.y;
    float halfHeight = cameraProjection.y * 0.5;

    ray.origin = cameraPos.xyz - forward * cameraProjection.z +
                 right * (ndc.x * halfHeight * aspectRatio) +
                 up * (ndc.y * halfHeight);
    ray.direction = forward;
    return ray;
}
