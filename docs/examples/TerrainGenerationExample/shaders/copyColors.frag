#version 460

in vec2 TexCoords;

layout(location = 0) out vec4 outColor;

uniform sampler2D intermediateBuffer_color;

void main(){
    outColor = texture(intermediateBuffer_color, TexCoords);
}