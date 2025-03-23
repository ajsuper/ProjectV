#version 460 core

in vec2 TexCoords;

layout (location = 0) out vec4 FragColor;
layout (location = 1) out float FragDepth;
layout (location = 2) out vec3 FragNormal;
layout (location = 3) out vec4 FragAlbedo;

uniform int PassNumber;
uniform sampler2D screenTexture;
uniform sampler2D depthTexture;
uniform sampler2D colorTexture;
uniform sampler2D normalTexture;
uniform sampler2D albedoTexture;

//float colorWeight = 1.0;
//float normalWeight = 0.00000003;
//float depthWeight = 15.0;
float brightnessWeight = 0.01f;
float albedoWeight = 2.0f;

float colorWeight = 0.99;
float normalWeight = 1.0;
float positionWeight = 0.12;

float exposure = 1.0;

vec4 ToneMapReinhardExposure(vec4 color, float exposure) {
    vec4 mapped = exposure * color;
    return mapped / (mapped + vec4(1.0));
}

vec4 bayerMatrix[4] = vec4[4](
    vec4(0.0, 8.0, 2.0, 10.0),
    vec4(12.0, 4.0, 14.0, 6.0),
    vec4(3.0, 11.0, 1.0, 9.0),
    vec4(15.0, 7.0, 13.0, 5.0)
);

void main(){
    vec4 sum = vec4(0.0);
    vec2 stepSize = 1/textureSize(screenTexture, 0);
    vec4 centerColor = texture(colorTexture, TexCoords);
    FragDepth = texture(depthTexture, TexCoords).r;
    FragNormal = texture(normalTexture, TexCoords).rgb; 
    FragAlbedo = texture(albedoTexture, TexCoords);   

    float kernel[25];
    kernel = float[](
        0.01818, 0.03636, 0.05455, 0.03636, 0.01818,
        0.03636, 0.07273, 0.10909, 0.07273, 0.03636,
        0.05455, 0.10909, 0.16364, 0.10909, 0.05455,
        0.03636, 0.07273, 0.10909, 0.07273, 0.03636,
        0.01818, 0.03636, 0.05455, 0.03636, 0.01818
    ); 

    vec2 offsets[25] = vec2[](
        vec2(-2.0, -2.0), vec2(-1.0, -2.0), vec2(0.0, -2.0), vec2(1.0, -2.0), vec2(2.0, -2.0),
        vec2(-2.0, -1.0), vec2(-1.0, -1.0), vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(2.0, -1.0),
        vec2(-2.0,  0.0), vec2(-1.0,  0.0), vec2(0.0,  0.0), vec2(1.0,  0.0), vec2(2.0,  0.0),
        vec2(-2.0,  1.0), vec2(-1.0,  1.0), vec2(0.0,  1.0), vec2(1.0,  1.0), vec2(2.0,  1.0),
        vec2(-2.0,  2.0), vec2(-1.0,  2.0), vec2(0.0,  2.0), vec2(1.0,  2.0), vec2(2.0,  2.0)
    );
    if(PassNumber <= 8){
        float cumulativeWeight = 0;
        for(int i = 0; i < 25; i++){ 
            vec2 uv = (offsets[i] / textureSize(screenTexture, 0))*(11-PassNumber);

            vec4 sampleColor = texture(screenTexture, TexCoords+uv);
            vec4 clampedCenterColor = clamp(centerColor, 0.0, 1.0);
            vec4 clampedSampleColor = clamp(sampleColor, 0.0, 1.0);
            vec4 tColor = clampedCenterColor - clampedSampleColor;
            float dist2 = dot(tColor, tColor);
            float weightFromColor = min(exp(-(dist2)/colorWeight), 1.0);

            vec3 sampleNormal = texture(normalTexture, TexCoords+uv).rgb;
            vec3 tNormal = FragNormal - sampleNormal;
            float distNormal = dot(tNormal, tNormal);
            float weightFromNormal = min(exp(-(distNormal)/normalWeight), 1.0);

            vec4 samplePosition = texture(albedoTexture, TexCoords+uv);
            vec4 tPosition = FragAlbedo-samplePosition;
            float distPosition = dot(tPosition, tPosition);
            float weightFromPosition = min(exp(-(distPosition)/positionWeight), 1.0);

            float weight = weightFromColor*weightFromNormal*weightFromPosition;
            sum += sampleColor * weight * kernel[i];
            cumulativeWeight += weight*kernel[i];
        }  
        FragColor = sum/cumulativeWeight;
    } else {
        
        vec4 toneMappedColor = ToneMapReinhardExposure(centerColor, exposure);

        vec3 gradedColor = toneMappedColor.rgb;
        gradedColor.r = pow(gradedColor.r, 1.0);
        gradedColor.g = pow(gradedColor.g, 1.0);
        gradedColor.b = pow(gradedColor.b, 1.0);

        float contrast = 1.0;
        float saturation = 1.3;
        float brightness = 1.5;

        gradedColor = ((gradedColor - 0.5) * contrast + 0.5);

        float luminance = dot(gradedColor, vec3(0.2126, 0.7152, 0.0722));
        gradedColor = mix(vec3(luminance), gradedColor, saturation);

        gradedColor *= brightness;

        FragColor = vec4(gradedColor, toneMappedColor.a);      
        
    }
}
