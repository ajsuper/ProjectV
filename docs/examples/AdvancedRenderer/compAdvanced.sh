#!/bin/bash
# Compiles the AdvancedRenderer's shaders (one vertex, fourteen fragment) to bgfx .bin.
echo "Compiling AdvancedRenderer shaders..."
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
SHADER_DIR=./advancedRenderer/cascadeShaders

# Three include roots, and each carries a different kind of thing:
#   external/bgfx/src  bgfx's own bgfx_shader.sh
#   ./sharedShaders    this example's light rig and cascade math (pjv_sun_sky, pjv_cascade_*)
#   $PROJECTV_DIR/include   the ENGINE's shader library -- pjv_utils_DDA.sc, the voxel traversal
#
# The last one is why a shader here does not have to know how the scene is stored. It is also why
# every renderer in the tree stays in step with a traversal change: there is one copy of it.

# Vertex shaders (vs_*.sc). The shared .sc includes have no $input/$output and must not be compiled.
for i in $SHADER_DIR/vs_*.sc; do
    [ -f "$i" ] || break
    NAME_BIN_EXTENSION="${i%.*}.bin"
    echo "Compiling vertex shader: \"$i\" -> $NAME_BIN_EXTENSION"
    $SHADERC -f "$i" -o "$NAME_BIN_EXTENSION" --type v \
        --platform $PLATFORM --profile $PROFILE \
        -i $PROJECTV_DIR/external/bgfx/src -i ./sharedShaders -i $PROJECTV_DIR/include || exit 1
done

# Fragment shaders.
for i in $SHADER_DIR/*.frag; do
    [ -f "$i" ] || break
    NAME_BIN_EXTENSION="${i%.*}.bin"
    echo "Compiling fragment shader: \"$i\" -> $NAME_BIN_EXTENSION"
    $SHADERC -f "$i" -o "$NAME_BIN_EXTENSION" --type f \
        --platform $PLATFORM --profile $PROFILE \
        -i $PROJECTV_DIR/external/bgfx/src -i ./sharedShaders -i $PROJECTV_DIR/include || exit 1
done

echo "Done."
