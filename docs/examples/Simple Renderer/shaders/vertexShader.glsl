#version 460 core
layout (location = 0) in vec3 aPos; // Position
layout (location = 1) in vec2 aTexCoords; // Texture Coordinates

out vec2 TexCoords; // Passing texture coordinates to fragment shader

void main()
{
    gl_Position = vec4(aPos, 1.0);
    TexCoords = aTexCoords; // Pass texture coordinates as they are
}


/*
#version 460 core
layout (location = 0) in vec3 aPos;
out vec2 TexCoords;

void main()
{
    gl_Position = vec4(aPos, 1.0);
    TexCoords = aPos.xy;
}
*/
