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
#
# renderShaders is Render mode's path tracer. It has no vertex shader of its own -- its three
# passes are the same fullscreen quad the viewport renderer draws, so it loads
# editorShaders/vs_quad.bin at startup rather than carrying a second copy. The varying.def.sc
# beside it still has to exist: shaderc reads it to resolve the fragment shaders' $input.
SHADER_DIRS="./editorRenderer/editorShaders ./editorRenderer/imguiShaders ./renderRenderer/renderShaders"

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

    # Fragment shaders. denoise.frag is skipped here and built separately below: it is one source
    # compiled once per a-trous level rather than once, so the generic rule cannot name its output.
    for i in $SHADER_DIR/*.frag; do
        [ -f "$i" ] || break
        [ "$(basename "$i")" = "denoise.frag" ] && continue
        NAME_BIN_EXTENSION="${i%.*}.bin"
        echo "Compiling fragment shader: \"$i\" -> $NAME_BIN_EXTENSION"
        $SHADERC -f "$i" -o "$NAME_BIN_EXTENSION" --type f \
            --platform $PLATFORM --profile $PROFILE \
            -i $PROJECTV_DIR/external/bgfx/src -i $PROJECTV_DIR/include || exit 1
    done
done

# The a-trous denoiser: one source, one binary per level, differing only in the stride between
# taps. The stride has to be baked in rather than passed as a uniform because the engine publishes
# its multiPass counter under a per-pass uniform name (`multiPassPassNumber<index>`), so a single
# shader cannot read its own iteration number -- see the note at the top of denoise.frag. Baking it
# also lets the 5x5 loop unroll with constant texture offsets.
#
# The strides must stay powers of two ascending, and the binaries must stay in the same order as
# shaderIDs 6/7/8 in resources.json and the three passes in render.json. Adding a level means a
# stride here, a shaderID there, and a fourth pass reading and writing FBO 5.
DENOISE_SOURCE=./editorRenderer/editorShaders/denoise.frag
for STRIDE in 1 2 4; do
    DENOISE_BIN="./editorRenderer/editorShaders/denoise${STRIDE}.bin"
    echo "Compiling fragment shader: \"$DENOISE_SOURCE\" (stride $STRIDE) -> $DENOISE_BIN"
    $SHADERC -f "$DENOISE_SOURCE" -o "$DENOISE_BIN" --type f \
        --platform $PLATFORM --profile $PROFILE \
        --define "ATROUS_STRIDE=$STRIDE" \
        -i $PROJECTV_DIR/external/bgfx/src -i $PROJECTV_DIR/include || exit 1
done

echo "Done."
