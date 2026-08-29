#ifndef PROJV_EDITOR_IMGUI_IMPL_BGFX_H
#define PROJV_EDITOR_IMGUI_IMPL_BGFX_H

#include <string>

#include "imgui.h"
#include "bgfx/bgfx.h"

// =============================================================================
// imgui_impl_bgfx  --  Dear ImGui renderer backend for bgfx.
//
// Dear ImGui ships platform backends (we use imgui_impl_glfw for input) and
// renderer backends for the common APIs, but not for bgfx — bgfx keeps its own
// ImGui integration inside its example framework, which drags in bx allocators,
// the entry-point layer, and its own embedded shaders. This is the same job done
// against the engine's own primitives: shaders loaded through
// projv::graphics::loadShader, geometry through bgfx transient buffers.
//
// It draws into a single bgfx view, which the editor gives a high ID so bgfx's
// ascending view order puts the interface on top of the scene passes.
//
// Textures use ImGui 1.92's ImTextureData protocol: ImGui asks the backend to
// create, update, and destroy its atlases through ImDrawData::Textures, rather
// than the backend building one font texture up front. That is what
// ImGuiBackendFlags_RendererHasTextures advertises.
// =============================================================================

namespace projv::editor {
    /**
     * Creates the GPU resources the ImGui backend needs and registers it with the current ImGui
     * context. Must be called after ImGui::CreateContext and after bgfx::init.
     * @param viewID The bgfx view the interface is drawn into. Give it an ID above every scene
     *               pass: bgfx submits views in ascending ID order, so this is what puts the
     *               interface over the scene.
     * @param vertexShaderPath Path to the compiled vs_imgui.bin.
     * @param fragmentShaderPath Path to the compiled imgui.bin.
     * @return True if the shaders loaded and the program linked.
     */
    bool initializeImGuiBgfx(bgfx::ViewId viewID, const std::string& vertexShaderPath, const std::string& fragmentShaderPath);

    /**
     * Destroys every GPU resource created by the backend, including any texture ImGui still owns.
     * Call before bgfx::shutdown.
     */
    void shutdownImGuiBgfx();

    /**
     * Draws one frame of ImGui geometry. Call after ImGui::Render(), with ImGui::GetDrawData(),
     * and before bgfx::frame().
     * @param drawData The draw data produced by ImGui::Render().
     */
    void renderImGuiDrawData(ImDrawData* drawData);

    /**
     * Wraps a bgfx texture as an ImTextureID, for passing an engine-owned texture to ImGui::Image.
     * The encoding is the backend's own -- it is not simply the bgfx handle index -- so this is the
     * only supported way to build an ImTextureID the backend will resolve correctly.
     * @param handle The texture to draw. Must stay alive until the frame has been submitted.
     * @return The ImTextureID naming that texture.
     */
    ImTextureID imGuiTextureID(bgfx::TextureHandle handle);
}

#endif
