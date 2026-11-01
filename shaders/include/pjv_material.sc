#ifndef PJV_MATERIAL_SC
#define PJV_MATERIAL_SC

// The engine's materials: the palette and per-voxel material bytes the scene uploads, and what a
// palette entry decodes to (VoxelMaterial). Word layout: data_structures/scene.h, projv::Material.

#include <pjv_color.sc>   // stored colours are sRGB-encoded; decodeMaterial hands out linear ones
#include <pjv_motion.sc>  // pjvMaterialIsAdvected asks the motion table

USAMPLER2D(materialIDs, 9);
USAMPLER2D(materialPalette, 10);
uniform vec4 voxelTypeDims;
uniform vec4 paletteDims;

// The material flags byte, as scene.h lays it out. Bit 0 says this material moves; bits 1-4 name the
// motion set. Read straight off the palette word rather than through decodeMaterial, because the
// traversal asks this question far more often than it needs a whole material.
#define PJV_MAT_FLAG_ANIMATED   1u
#define PJV_MAT_MOTION_SHIFT    1u
#define PJV_MAT_MOTION_MASK     0xFu

uint materialID(uint index) {
    uint w = uint(voxelTypeDims.x);
    uint shift = uint(voxelTypeDims.y);
    uint pixelIndex = uint(index) >> 2u;
    int x = int(pixelIndex & (w - 1u));
    int y = int(pixelIndex >> shift);
    int colorIndex = int(uint(index) & 3u);
    uvec4 pixel = texelFetch(materialIDs, ivec2(x, y), 0);
    return pixel[colorIndex];
}

// One palette entry is one RGBA32U texel: the four words of projv::Material. The palette used to
// pack four entries into a texel and select one component out of the fetch, so every material
// property added since then has been free -- the fetch was always returning four words and throwing
// three of them away. See the Material comment in data_structures/scene.h for the word layout and,
// more importantly, for why all-zero words 1-3 mean "shade this exactly as the colour-only pipeline
// did", which is what lets every compose.json already on disk render unchanged.
uvec4 materialPaletteTexel(uint index) {
    uint w = uint(paletteDims.x);
    uint shift = uint(paletteDims.y);
    int x = int(index & (w - 1u));
    int y = int(index >> shift);
    return texelFetch(materialPalette, ivec2(x, y), 0);
}

// Everything a shader can know about a voxel's surface. Decoded from that one texel; nothing in
// here costs a second fetch.
struct VoxelMaterial {
    vec3  albedo;
    vec3  emission;       // Already scaled by strength, so this is radiance rather than a colour.
    float glossiness;     // 0 = fully rough (Lambertian), 1 = mirror.
    float metallic;       // 0 = dielectric, 1 = conductor (the specular lobe takes the albedo tint).
    float transparency;   // 0 = opaque. Read by the traversal -- see PeelAccum / raySceneIntersect.
    float ior;            // 1.0 = no refraction.
    float transmission;
    uint  flags;
};

vec3 unpackPaletteRGB10(uint packed) {
    return vec3(float((packed >> 20u) & 0x3FFu),
                float((packed >> 10u) & 0x3FFu),
                float( packed         & 0x3FFu)) / 1023.0;
}

// NOTE ON TRANSPARENCY: acting on `transparency` is not a shading change -- it is a traversal change,
// because every march in this file stops at the first solid voxel. The traversal now makes it: a
// query with PJV_Q_TRANSPARENCY reads the material at a leaf and decides there whether the voxel stops
// the ray. The material fetch that costs is also the one the caller would have done anyway, so the
// work moved rather than doubling -- see SceneHit::material.
//
// `ior` and `transmission` are still decoded and carried without being acted on. That one IS a
// separate change: a refracted ray changes direction, so it is a new ray rather than a continuation
// of the same march. PJV_Q_REFRACTION is where it will hang.
// The material of nothing: black, non-emissive, fully rough, opaque. By the zero rule this is what a
// palette entry of all zeroes decodes to, so "empty" and "never had properties set" agree.
VoxelMaterial emptyVoxelMaterial() {
    VoxelMaterial m;
    m.albedo = vec3(0.0);
    m.emission = vec3(0.0);
    m.glossiness = 0.0;
    m.metallic = 0.0;
    m.transparency = 0.0;
    m.ior = 1.0;
    m.transmission = 0.0;
    m.flags = 0u;
    return m;
}
// Does this material move, and by which mechanism? Named here so a renderer does not have to
// open-code the flags byte to answer either question -- both are read by the g-buffer to decide how
// a surface should be shaded and how a temporal filter may reuse it.
//
// The two are a genuine distinction rather than a convenience. A swayed voxel keeps its SOURCE cell
// (the animated march reports it on purpose), so a temporal filter can gate it on an exact voxel
// identity and give it a long mean. An advected parcel has no such anchor -- it moves THROUGH the
// lattice, so the voxel under it is a different one each frame -- and can only be matched
// positionally, with a looser radius and a shorter mean. Collapsing the two into one flag is why
// fire still flickered after grass had stopped: it was being offered grass's gate, which nothing
// about fire can pass.
bool pjvMaterialIsAnimated(VoxelMaterial m) { return (m.flags & PJV_MAT_FLAG_ANIMATED) != 0u; }

// ...and specifically by ADVECTION. Answered from the motion table rather than from a flag bit,
// because "which kind of field moves this" is a property of the field and the material only names
// one. The prototype spent a palette flag bit on it and had to keep the two in step by hand.
bool pjvMaterialIsAdvected(VoxelMaterial m) {
    if ((m.flags & PJV_MAT_FLAG_ANIMATED) == 0u) return false;
    return pjvMotionSet((m.flags >> PJV_MAT_MOTION_SHIFT) & PJV_MAT_MOTION_MASK).kind == PJV_MOTION_ADVECT;
}

VoxelMaterial decodeMaterial(uvec4 texel) {
    VoxelMaterial m;
    // A palette colour is stored sRGB-encoded (docs/plans/color-convention.md); shading wants linear light,
    // so it is decoded here, once, for every renderer. The emission colour likewise; its strength is linear.
    m.albedo = pjvSrgbToLinear(unpackPaletteRGB10(texel.x));

    // Exponential, matching projv::unpackEmissiveStrength on the CPU: raw 0 is exactly zero, so a
    // legacy entry is not an emitter, and 1..255 spans roughly 0.004 to 245 with raw 128 sitting at
    // 1.0. A linear byte would put every usable dim emitter inside its first step.
    uint strengthRaw = (texel.w >> 24u) & 0xFFu;
    float strength = strengthRaw == 0u ? 0.0 : exp2(float(strengthRaw) / 16.0 - 8.0);
    // A zero emission word means "emit in the albedo's colour" -- emitting black is meaningless, so
    // zero is free to carry that. Keeps the strength control alive on a material that has never had
    // an emission colour picked, without giving up independent emission for the surfaces that need
    // it (a black voxel cannot glow if emission is albedo * strength). See
    // projv::materialEffectiveEmissionColor.
    vec3 emissionColour = texel.y == 0u ? m.albedo : pjvSrgbToLinear(unpackPaletteRGB10(texel.y));
    m.emission = emissionColour * strength;

    m.glossiness   = float((texel.z >> 24u) & 0xFFu) / 255.0;
    m.metallic     = float((texel.z >> 16u) & 0xFFu) / 255.0;
    m.transparency = float((texel.z >>  8u) & 0xFFu) / 255.0;
    m.ior          = 1.0 + float(texel.z & 0xFFu) / 128.0;
    m.transmission = float((texel.w >> 16u) & 0xFFu) / 255.0;
    m.flags        = (texel.w >> 8u) & 0xFFu;
    return m;
}

#endif
