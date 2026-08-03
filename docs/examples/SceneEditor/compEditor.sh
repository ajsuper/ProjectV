#!/bin/bash
# Compiles the scene editor's shaders to bgfx .bin: the viewport renderer (a copy of the scene
# previewer's, with the display pass targeting an offscreen texture instead of the back buffer) and
# the two ImGui draw shaders.
echo "Compiling scene editor shaders..."
PROJECTV_DIR=../../../

PLATFORM=windows
PROFILE=spirv
if [[ $OSTYPE == *"linux"* ]]; then
    PLATFORM=linux
    PROFILE=spirv
fi
if [[ $OSTYPE == *"darwin"* ]]; then
    PLATFORM=osx
    PROFILE=metal
fi

SHADERC=$PROJECTV_DIR/build/tools/shadercRelease

# Each directory carries its own varying.def.sc, which shaderc picks up from beside the shader:
# ImGui's vertex is 2D with a packed colour, the viewport renderer's is a 3D fullscreen quad.
SHADER_DIRS="./editorRenderer/editorShaders ./editorRenderer/imguiShaders"

for SHADER_DIR in $SHADER_DIRS; do
    # Vertex shaders (vs_*.sc).
    for i in $SHADER_DIR/vs_*.sc; do
        [ -f "$i" ] || break
        NAME_BIN_EXTENSION="${i%.*}.bin"
        echo "Compiling vertex shader: \"$i\" -> $NAME_BIN_EXTENSION"
        $SHADERC -f "$i" -o "$NAME_BIN_EXTENSION" --type v \
            --platform $PLATFORM --profile $PROFILE \
            -i $PROJECTV_DIR/external/bgfx/src -i $PROJECTV_DIR/include || exit 1
    done

    # Fragment shaders.
    for i in $SHADER_DIR/*.frag; do
        [ -f "$i" ] || break
        NAME_BIN_EXTENSION="${i%.*}.bin"
        echo "Compiling fragment shader: \"$i\" -> $NAME_BIN_EXTENSION"
        $SHADERC -f "$i" -o "$NAME_BIN_EXTENSION" --type f \
            --platform $PLATFORM --profile $PROFILE \
            -i $PROJECTV_DIR/external/bgfx/src -i $PROJECTV_DIR/include || exit 1
    done
done

echo "Done."
