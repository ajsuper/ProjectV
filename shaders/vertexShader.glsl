// Simple vertex shader for rendering the output of our fragment shader (ray marcher).

#version 460 core
layout (location = 0) in vec3 aPos;
out vec2 TexCoords;

void main()
{
    gl_Position = vec4(aPos, 1.0);
    TexCoords = aPos.xy;
}
