// =============================================================================
// pjv_anim_controls.sc  --  the two uniforms this example steers the ENGINE with.
//
// Two, where there were eight. What went is the point, so it is worth naming: `waveTime`,
// `waveParams`, `waveWind`, `waveSnap`, `waveBand`, `fireParams` and `fireShape` carried the sway
// field's amplitude, gust size, speed, turbulence, wind direction, snap quantum and profile exponent,
// and the fire flow's rise, turbulence, frequency, chain step, growth, dissolve and step length.
//
// NOT ONE OF THOSE IS A SHADER PARAMETER ANY MORE, and that is the whole result of the promotion
// rather than a tidying of it. The field lives in the engine's motion table -- `pjvMotionSets`,
// uploaded from `AnimationState` -- so a shader does not describe the motion, it asks for it with a
// RayQuery flag and the traversal reads the table. There is no shader code in this example for the
// wind, the flame, the envelope, the quantiser or the resolve.
//
// What survives is what this EXAMPLE decides rather than what the engine does: how big a voxel is in
// this scene, how far to bother resolving, and which diagnostic view is on.
// =============================================================================

// x = world units per finest voxel, measured from the scene at load.
// y = how far animation is RESOLVED, in voxels. Past it, animated geometry is drawn at rest -- it
//     stops moving, it does not stop existing. The dominant cost control for the animated path, and
//     the reason it works is that a blade is subpixel long before it stops costing.
// z = clean view: substitute a flat hemispherical ambient for the GI (the `N` key).
// w = trace SHADOW rays through animated geometry at all (the `H` key). With it off, a swaying blade
//     still casts a shadow -- it is cast from the rest pose.
uniform vec4 animParams;

// x = SUSPEND the animated traversal (the `O` key).
//
//     Worth a switch rather than a rebuild, because it separates two layers that fail identically on
//     screen. The materials stay flagged, the envelope stays baked, uploaded and in the header -- the
//     query simply does not ask for animation, so the geometry march stops skipping animated voxels
//     and draws them at their rest position. If the geometry is solid and correct here then flagging,
//     baking, uploading and the g-buffer are all sound and the whole fault is in the animated path;
//     if it is still see-through here, with no animation code running at all, then every hypothesis
//     about the resolve, the envelope march and the merge is wrong.
//
// y = the why-does-this-pixel-look-wrong cycle (the `/` key). See gbuffer.frag, compose.frag (which
//     must not fog a marked pixel) and display.frag (mode 5, for terms added after compose).
// z = how many times the primary ray may be bent by refraction. Runtime rather than a #define because
//     the segment loop is not free where it is unused; see gbuffer.frag.
// w = spare.
uniform vec4 animDebug;

float animVoxelSize()       { return max(animParams.x, 1e-6); }
float animResolveVoxels()   { return max(animParams.y, 1.0); }
bool  animCleanView()       { return animParams.z > 0.5; }
bool  animShadowsResolve()  { return animParams.w > 0.5; }
bool  animationSuspended()  { return animDebug.x > 0.5; }
int   animDebugMode()       { return int(animDebug.y + 0.5); }
uint  animRefractionSegments() { return uint(max(animDebug.z, 0.0) + 0.5); }
