$input v_color0 
$input v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 multiPassPassNumber1;
//SAMPLER2D(u_texColor, 0);
SAMPLER2D(globalIllumination, 0);
SAMPLER2D(directIllumination, 1);
SAMPLER2D(normals, 2);
SAMPLER2D(albedos, 3);

void main() {
    vec2 texCord = v_texcoord0;
    vec2 giTexCord = v_texcoord0;

    // Fetch the center pixel values
    vec4 giValue = texture2D(globalIllumination, giTexCord);
    vec4 diValue = texture2D(directIllumination, giTexCord);
    vec4 normal = texture2D(normals, texCord);
    vec4 albedo = vec4(texture2D(albedos, texCord).rgb, 0);

    // === A-Trous Wavelet Denoiser (using only albedo & normal) ===
    vec4 giDenoised = vec4(0.0);
    float giTotalWeight = 0.5; // Center pixel weight

    float STEP_SIZE = 1.0 + multiPassPassNumber1.x; // Multi-pass step
    STEP_SIZE = 0;

    // Loop over 5x5 neighborhood
    for(int x = -5; x <= 5; x++) {
        for(int y = -5; y <= 5; y++) {
            if(x == 0 && y == 0) continue; // skip center

            vec2 offsetCoord = vec2(float(x) * STEP_SIZE / windowRes.x,
                                    float(y) * STEP_SIZE / windowRes.y);

            // Neighboring samples
            vec4 neighborGI = max(vec4(0.0), texture2D(globalIllumination, giTexCord + offsetCoord));
            vec4 neighborNormal = texture2D(normals, texCord + offsetCoord);
            vec4 neighborAlbedo = vec4(texture2D(albedos, texCord + offsetCoord).rgb, 0);

            // === Spatial weight (B-spline / Gaussian) ===
            float radius = 9.0;
            float sigma = radius / 6.0;
            float twoSigmaSq = 2.0 * sigma * sigma;
            float spatialWeight = exp(-(float(x*x + y*y)) / twoSigmaSq);

            // === Similarity weights using only albedo & normal ===
            vec4 normalDiff = normal - neighborNormal;
            float normalMaxDiff = max(max(abs(normalDiff.x), abs(normalDiff.y)), abs(normalDiff.z));
            float normalWeight = exp(-normalMaxDiff * 99.0);

            vec4 albedoDiff = albedo - neighborAlbedo;
            float albedoMaxDiff = max(max(abs(albedoDiff.x), abs(albedoDiff.y)), abs(albedoDiff.z));
            float albedoWeight = exp(-albedoMaxDiff * 1.0);

            float totalWeight = normalWeight * albedoWeight * spatialWeight;

            if(totalWeight > 0.0001) {
                float centerWeight = 0.5;
                giDenoised = (giDenoised + giValue * centerWeight) / (giTotalWeight + centerWeight);
                giTotalWeight += totalWeight;
            }
        }
    }

    // Normalize by total weight and add center pixel contribution
    giDenoised = (giDenoised + giValue) / vec4(giTotalWeight);

    // Output
    gl_FragData[0] = vec4(giDenoised);  // Denoised GI
    gl_FragData[1] = diValue;           // Direct Illumination
    gl_FragData[2] = normal;            // Normals
    gl_FragData[3] = texture2D(albedos, texCord); // Albedo
}
