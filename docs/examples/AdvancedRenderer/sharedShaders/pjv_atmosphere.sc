// =============================================================================
// pjv_atmosphere.sc  --  The air between the eye and everything else.
//
// One description of the participating medium, shared by the two passes that need it. It started
// inside compose.frag, which is still its main consumer; it moved out here when godrays.frag turned
// up wanting the SAME optical depth. That is not a convenience -- the two effects are the same
// physics split by direction. Fog is how much light the air takes OUT of the view ray and adds back
// isotropically; a god ray is the part of that inscatter that came from the sun along an unoccluded
// path. If the two disagreed about how thick the air is, shafts would hang in clear space or fail to
// appear in haze, and no amount of tuning either one separately would fix it.
//
// Requires pjv_sun_sky.sc (SUN_DIR, SUN_COLOR, pjvSkyBase, pjvMiePhase) and cameraPos, both of which
// arrive with pjv_cascade_common.sc. Include this AFTER that.
// =============================================================================

// x = extinction coefficient per world unit, AT the reference height z. Zero switches the whole
//     effect off -- fog, inscatter and shafts together -- and the branches below return early.
// y = the height over which the fog thins by 1/e.
// z = the reference height, which main.cpp sets to the scene's floor.
// w = sun inscatter strength.
//
// SCENE-RELATIVE, and it has to be: this example takes its scene on the command line, and the
// bundled ones run from a 32-voxel model to SanMiguel at 32768. A density in world units that looks
// like haze in one is an opaque wall in the next. main.cpp derives all four from the scene's own
// bounding sphere -- see FOG_* there -- so "fog" means the same fraction of the scene's extent
// everywhere, and so does the god-ray gain that rides on fogParams.w.
uniform vec4 fogParams;

// How tightly the air throws sunlight forward. Shared by the fog's inscatter and, through
// pjv_sun_sky.sc, by the sky's aureole -- see the note on pjvMiePhase for why one lobe serves both.
#define FOG_PHASE_G 0.62

// ---- HEIGHT FOG, ANALYTICALLY -----------------------------------------------------------------
// Distance haze, evaluated in closed form from the G-buffer. No marching, no extra samplers, no
// volume texture, and no temporal cost -- it is a pure function of the camera, the sun and the world
// position already sitting in gPos, so it is bit-identical between two frames of a still scene and
// the accumulate and TAA gates downstream never see it move.
//
// WHY IT IS WORTH HAVING. A voxel scene's failure mode at distance is that everything stays equally
// crisp and equally saturated, so the eye gets no depth cue past stereopsis and the frame reads flat.
// Fog supplies the two cues that are missing at once: contrast falls with distance, and hue slides
// toward the sky. It also hides the far field's real weaknesses -- the LOD crossover, the thinning
// grass, the coarser cascade -- behind exactly the thing that would obscure them in air.
//
// THE MODEL. Density falls off exponentially with height, which is what air does, so a scene has a
// haze layer sitting in its valleys rather than a uniform tint over everything:
//
//     density(y) = d * exp(-(y - y0) / H)
//
// The optical depth along a ray from C in direction D over distance t integrates in closed form:
//
//     tau = d * exp(-(C.y - y0)/H) * (1 - exp(-D.y*t/H)) / (D.y/H)
//
// with the removable singularity at D.y = 0 (a horizontal ray, constant density) handled as d*t.
// That closed form is the entire cost of this effect: two exp() calls and a divide.
float fogOpticalDepth(vec3 origin, vec3 dir, float dist) {
    if (fogParams.x <= 0.0) return 0.0;

    float H = max(fogParams.y, 1e-4);
    // Density at the eye, relative to the reference height. CLAMPED: a camera flown far below the
    // reference plane makes this exponent large and positive, and an inf here propagates into the
    // colour rather than saturating the fog like the maths says it should. +6 is already e^6 = 400x
    // the reference density, which is opaque within a fraction of a voxel.
    float atEye = exp(clamp(-(origin.y - fogParams.z) / H, -30.0, 6.0));

    float b = dir.y / H;
    // The same guard on the other exponent, for a ray pointing steeply down over a long distance.
    float integral = abs(b) < 1e-5 ? dist : (1.0 - exp(min(-b * dist, 30.0))) / b;

    return clamp(fogParams.x * atEye * integral, 0.0, 40.0);
}

// How much of this view ray is air that could scatter something at the eye. 0 at the near plane,
// approaching 1 through deep haze. It is `1 - transmittance`, which is the same fraction the fog
// blend below uses -- and it is what godrays.frag modulates its shafts by, so that a shaft only
// appears where there is enough air in front of the camera to carry it. A pixel a metre away gets
// none however much sky is behind it, which is the difference between a light shaft and a smear.
float fogInscatterFraction(vec3 dir, float dist) {
    return 1.0 - exp(-fogOpticalDepth(cameraPos.xyz, dir, dist));
}

// How far a sky pixel is. The G-buffer writes the background as `origin + direction * 1e5`, so this
// is that placeholder's distance and not an arbitrary big number -- the two must agree, or the sky
// and the geometry silhouetted against it would be fogged by different amounts and the silhouette
// would come back as a seam.
#define FOG_SKY_DISTANCE 1e5

// What the fog itself is glowing with, in a given view direction.
//
// TWO TERMS. The first is the sky the fog is sitting under -- and it is built from the gradient's
// endpoints directly rather than by calling pjvSkyBase, for two reasons that are both about the
// HORIZON SEAM this used to draw.
//
//   NO GROUND TERM. pjvSkyBase blends toward SKY_GROUND below the horizon, and the fog has no
//   business with that: the haze between the eye and distant terrain is lit from above by the sky,
//   so it reads as the pale horizon band, not as the dark ground beneath it.
//
//   NO HARD FOLD. This used to pass max(dir.y, 0.0) into pjvSkyBase, which sounds like it avoids the
//   ground term and does not: every downward direction lands on exactly y = 0, and y = 0 is the
//   MIDDLE of pjvSkyBase's own smoothstep(-0.05, 0.05) ground blend -- so the fog faded toward a
//   colour half-mixed with SKY_GROUND while the sky a pixel higher up was most of the way to the
//   horizon band. That difference is a step in brightness running along the horizon, which is the
//   line that had to go. Interpolating from SKY_HORIZON with the upward component alone has no such
//   crossover: at dir.y = 0 it is exactly SKY_HORIZON, from either side.
//
// The second is sun inscatter, through the same phase function the sky's aureole uses -- so looking
// toward a low sun through haze brightens and warms the whole distance, and looking away from it does
// not. This is what makes fog read as lit air rather than as grey paint, and it costs one dot and one
// divide.
//
// It is UNSHADOWED on purpose: this term assumes the sun reaches every parcel of air along the ray.
// The correction for the parcels it does not reach is exactly what godrays.frag computes, in screen
// space, at a quarter of the pixels -- so the two are complements rather than duplicates, and the
// shafts are added on top of this rather than replacing it.
vec3 fogInscatteredColor(vec3 dir) {
    vec3 haze = mix(SKY_HORIZON, SKY_ZENITH, clamp(dir.y, 0.0, 1.0));
    return haze + SUN_COLOR * (fogParams.w * pjvMiePhase(dot(dir, SUN_DIR), FOG_PHASE_G));
}

// Beer-Lambert: what survives the trip, plus what the air added along the way.
vec3 applyFog(vec3 color, vec3 dir, float dist) {
    float transmittance = exp(-fogOpticalDepth(cameraPos.xyz, dir, dist));
    return mix(fogInscatteredColor(dir), color, transmittance);
}
