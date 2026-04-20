#!/bin/bash
echo "Searching for files..."
PROJECTV_DIR=../../../
# Handle platform specific compile variables
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

for i in ./tree64Renderer/pathTracerShaders/*.sc; do
    [ -f "$i" ] || break
    echo "Compiling file: \"$i\"..."
    NAME_BIN_EXTENSION="${i%.*}.bin"
    echo $NAME_BIN_EXTENSION
    $PROJECTV_DIR/build/tools/shadercRelease \
        -f $i \
        -o $NAME_BIN_EXTENSION \
        --type v \
        --platform $PLATFORM \
        --profile $PROFILE \
        -i $PROJECTV_DIR/external/bgfx/src
done

for i in ./tree64Renderer/pathTracerShaders/*.frag; do
    [ -f "$i" ] || break
    echo "Compiling file: \"$i\"..."
    NAME_BIN_EXTENSION="${i%.*}.bin"
    echo $NAME_BIN_EXTENSION
    $PROJECTV_DIR/build/tools/shadercRelease \
        -f $i \
        -o $NAME_BIN_EXTENSION \
        --type f \
        --platform $PLATFORM \
        --profile $PROFILE \
        -i $PROJECTV_DIR/external/bgfx/src
done

