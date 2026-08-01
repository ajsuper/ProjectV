#!/bin/bash
# Compiles the reprojectionRenderer shaders (vertex + fragment) to bgfx .bin.
echo "Compiling reprojectionRenderer shaders..."
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
SHADER_DIR=./reprojectionRenderer/pathTracerShaders

# Vertex shaders (*.sc except the shared include which has no $input/$output).
for i in $SHADER_DIR/vs_*.sc; do
    [ -f "$i" ] || break
    NAME_BIN_EXTENSION="${i%.*}.bin"
    echo "Compiling vertex shader: \"$i\" -> $NAME_BIN_EXTENSION"
    $SHADERC -f "$i" -o "$NAME_BIN_EXTENSION" --type v \
        --platform $PLATFORM --profile $PROFILE -i $PROJECTV_DIR/external/bgfx/src -i ./sharedShaders -i $PROJECTV_DIR/include || exit 1
done

# Fragment shaders.
for i in $SHADER_DIR/*.frag; do
    [ -f "$i" ] || break
    NAME_BIN_EXTENSION="${i%.*}.bin"
    echo "Compiling fragment shader: \"$i\" -> $NAME_BIN_EXTENSION"
    $SHADERC -f "$i" -o "$NAME_BIN_EXTENSION" --type f \
        --platform $PLATFORM --profile $PROFILE -i $PROJECTV_DIR/external/bgfx/src -i ./sharedShaders -i $PROJECTV_DIR/include || exit 1
done

echo "Done."
