// Dear ImGui's renderer backend for bgfx (ui/dev.h). ImGui ships platform backends and renderer
// backends for the common APIs, but not for bgfx, which keeps its own integration inside its example
// framework. This is the same job against the engine's primitives: its embedded shaders
// (shaders/engine/imgui/), geometry through bgfx transient buffers.
//
// Textures use ImGui 1.92's ImTextureData protocol: ImGui asks the backend to create, update and
// destroy its atlases through ImDrawData::Textures, rather than the backend building one font texture
// up front. That is what ImGuiBackendFlags_RendererHasTextures advertises.
//
// The state is per ImGui context (io.BackendRendererUserData), so a program's own context (the scene
// editor's) and the dev layer's cannot share or clobber it.

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "ui/dev.h"

#include "core/log.h"
#include "graphics/engine_shaders.h"

namespace projv::ui::dev {

    // Everything the backend owns for one context.
    struct ImGuiBgfxState {
        bgfx::ViewId viewID = 0;
        bgfx::VertexLayout vertexLayout;
        bgfx::ProgramHandle shaderProgram = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle textureSampler = BGFX_INVALID_HANDLE;
        bool initialized = false;
    };

    static ImGuiBgfxState* backend() {
        return ImGui::GetCurrentContext() ? static_cast<ImGuiBgfxState*>(ImGui::GetIO().BackendRendererUserData) : nullptr;
    }

    // ImGui stores one backend-defined value per texture. A bgfx handle is a 16-bit index, so it
    // goes in and out of ImTextureID (a 64-bit integer) directly, with no allocation.
    //
    // Biased by one in both directions: bgfx hands out index 0 as an ordinary handle, but ImGui
    // spells "no texture" as ImTextureID_Invalid, which is 0. Without the bias those two collide --
    // ImGui would read a real texture as absent, and the destroy paths below would take a cleared
    // ImTextureID for a live handle and destroy whatever bgfx resource happened to hold index 0.
    static bgfx::TextureHandle textureHandleFromImGuiID(ImTextureID textureID) {
        bgfx::TextureHandle handle;
        handle.idx = (textureID == ImTextureID_Invalid) ? bgfx::kInvalidHandle : uint16_t(textureID - 1);
        return handle;
    }

    static ImTextureID imGuiIDFromTextureHandle(bgfx::TextureHandle handle) {
        return ImTextureID(handle.idx + 1);
    }

    ImTextureID textureID(bgfx::TextureHandle handle) {
        return imGuiIDFromTextureHandle(handle);
    }

    namespace {
        // Draw-list markers around an opaque image: the backend draws what is between them without
        // blending. Compared by address, never called.
        void opaqueBegin(const ImDrawList*, const ImDrawCmd*) {}
    }

    void opaqueImage(bgfx::TextureHandle handle, ImVec2 size, ImVec2 uv0, ImVec2 uv1) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddCallback(opaqueBegin, nullptr);
        ImGui::Image(ImTextureRef(textureID(handle)), size, uv0, uv1);
        drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

    // Honours one texture request from ImGui. ImGui owns the pixels and tells us what changed; the
    // backend owns the GPU object and reports back through SetTexID/SetStatus.
    static void updateImGuiTexture(ImTextureData* texture) {
        if (texture->Status == ImTextureStatus_WantCreate) {
            // ImGui only ever asks for RGBA32 or Alpha8, and we never set TexDesiredFormat, so it
            // is RGBA32 here. Asserting is better than silently uploading garbage if that changes.
            IM_ASSERT(texture->Format == ImTextureFormat_RGBA32 && texture->BytesPerPixel == 4);

            // Created empty and filled immediately after, rather than handed its pixels here.
            // bgfx marks any texture given creation-time data immutable (createTexture2D passes
            // "NULL != _mem" as its immutable flag), and updateTexture2D on an immutable texture
            // returns without doing anything -- silently, since the warning behind it is compiled
            // out of bgfxRelease. ImGui rasterizes glyphs on demand and expects those later writes
            // to land, so an immutable atlas loses every glyph baked after this first frame: the
            // metrics come from the CPU-side atlas and are correct, but the texels are never
            // uploaded, so the glyph draws as blank space.
            bgfx::TextureHandle handle = bgfx::createTexture2D(
                uint16_t(texture->Width), uint16_t(texture->Height), false, 1,
                bgfx::TextureFormat::RGBA8, 0, nullptr);

            if (!bgfx::isValid(handle)) {
                core::error("ImGui backend: failed to create a {}x{} texture.", texture->Width, texture->Height);
                return;
            }

            const bgfx::Memory* pixels = bgfx::copy(texture->GetPixels(), uint32_t(texture->GetSizeInBytes()));
            bgfx::updateTexture2D(handle, 0, 0, 0, 0, uint16_t(texture->Width), uint16_t(texture->Height), pixels);

            texture->SetTexID(imGuiIDFromTextureHandle(handle));
            texture->SetStatus(ImTextureStatus_OK);
            return;
        }

        if (texture->Status == ImTextureStatus_WantUpdates) {
            // ImGui hands us the exact dirty rectangles in texture->Updates, but an update only
            // happens when a font is added or a glyph is rasterized for the first time — a handful
            // of times over a session. Re-uploading the whole atlas costs a few hundred kilobytes
            // on those frames and keeps this free of sub-rectangle copy logic.
            bgfx::TextureHandle handle = textureHandleFromImGuiID(texture->GetTexID());
            const bgfx::Memory* pixels = bgfx::copy(texture->GetPixels(), uint32_t(texture->GetSizeInBytes()));
            bgfx::updateTexture2D(handle, 0, 0, 0, 0, uint16_t(texture->Width), uint16_t(texture->Height), pixels);
            texture->SetStatus(ImTextureStatus_OK);
            return;
        }

        // UnusedFrames > 0 means ImGui has not drawn with this texture since the last frame the GPU
        // could still be reading it, so destroying now is safe.
        if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0) {
            bgfx::TextureHandle handle = textureHandleFromImGuiID(texture->GetTexID());
            if (bgfx::isValid(handle)) {
                bgfx::destroy(handle);
            }
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }

    bool initRenderer(bgfx::ViewId viewID) {
        ImGuiIO& io = ImGui::GetIO();
        IM_ASSERT(io.BackendRendererUserData == nullptr && "An ImGui renderer backend is already installed.");

        bgfx::ShaderHandle vertexShader = graphics::loadEngineShader("vs_imgui");
        bgfx::ShaderHandle fragmentShader = graphics::loadEngineShader("imgui");
        if (!bgfx::isValid(vertexShader) || !bgfx::isValid(fragmentShader)) {
            core::error("ImGui backend: no engine ImGui shaders for this renderer");
            if (bgfx::isValid(vertexShader)) bgfx::destroy(vertexShader);
            if (bgfx::isValid(fragmentShader)) bgfx::destroy(fragmentShader);
            return false;
        }

        ImGuiBgfxState* state = IM_NEW(ImGuiBgfxState)();
        state->viewID = viewID;
        state->shaderProgram = bgfx::createProgram(vertexShader, fragmentShader, true);
        state->textureSampler = bgfx::createUniform("s_imguiTexture", bgfx::UniformType::Sampler);

        // Matches ImDrawVert exactly. The colour is normalized on the way in so the shader sees the
        // 0..1 vertex colour ImGui intends, not a 0..255 byte.
        state->vertexLayout.begin()
            .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
            .end();

        io.BackendRendererName = "imgui_bgfx (ProjectV)";
        io.BackendRendererUserData = state;
        // VtxOffset lets one draw list hold more than 64k vertices with 16-bit indices, which a
        // dense scene hierarchy reaches. HasTextures opts into the ImTextureData protocol above.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

        state->initialized = true;
        return true;
    }

    void shutdownRenderer() {
        ImGuiBgfxState* state = backend();
        if (state == nullptr) {
            return;
        }

        // ImGui's atlases outlive the draw loop, so their GPU textures are ours to release here.
        for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
            if (texture->RefCount != 1) {
                continue;
            }
            bgfx::TextureHandle handle = textureHandleFromImGuiID(texture->GetTexID());
            if (bgfx::isValid(handle)) {
                bgfx::destroy(handle);
            }
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }

        if (bgfx::isValid(state->shaderProgram)) {
            bgfx::destroy(state->shaderProgram);
        }
        if (bgfx::isValid(state->textureSampler)) {
            bgfx::destroy(state->textureSampler);
        }

        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = nullptr;
        io.BackendRendererUserData = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
        IM_DELETE(state);
    }

    void renderDrawData(ImDrawData* drawData) {
        ImGuiBgfxState* state = backend();
        if (state == nullptr || !state->initialized || drawData == nullptr) {
            return;
        }
        const ImGuiBgfxState& backendState = *state;

        // Create/update/destroy requests must be served before the draw calls that reference them.
        if (drawData->Textures != nullptr) {
            for (ImTextureData* texture : *drawData->Textures) {
                if (texture->Status != ImTextureStatus_OK) {
                    updateImGuiTexture(texture);
                }
            }
        }

        int framebufferWidth = int(drawData->DisplaySize.x * drawData->FramebufferScale.x);
        int framebufferHeight = int(drawData->DisplaySize.y * drawData->FramebufferScale.y);
        if (framebufferWidth <= 0 || framebufferHeight <= 0) {
            return;
        }

        // ImGui emits vertices in framebuffer pixels with y growing downwards, so the projection is
        // an orthographic box over the window with a flipped vertical axis (top < bottom below).
        // Depth is unused — every vertex sits at z = 0 — but the range still has to match the
        // backend's clip space, which is [-1, 1] on OpenGL and [0, 1] everywhere else.
        const float left = drawData->DisplayPos.x;
        const float right = drawData->DisplayPos.x + drawData->DisplaySize.x;
        const float top = drawData->DisplayPos.y;
        const float bottom = drawData->DisplayPos.y + drawData->DisplaySize.y;
        const bool homogeneousDepth = bgfx::getCaps()->homogeneousDepth;

        float orthoProjection[16] = { 0.0f };
        orthoProjection[0] = 2.0f / (right - left);
        orthoProjection[5] = 2.0f / (top - bottom);
        orthoProjection[10] = homogeneousDepth ? 2.0f : 1.0f;
        orthoProjection[12] = (left + right) / (left - right);
        orthoProjection[13] = (top + bottom) / (bottom - top);
        orthoProjection[14] = homogeneousDepth ? -1.0f : 0.0f;
        orthoProjection[15] = 1.0f;

        // Sequential mode keeps the draw calls in submission order: ImGui's painter's algorithm
        // (panel background, then its contents, then anything on top) depends on it.
        bgfx::setViewMode(backendState.viewID, bgfx::ViewMode::Sequential);
        bgfx::setViewTransform(backendState.viewID, nullptr, orthoProjection);
        bgfx::setViewRect(backendState.viewID, 0, 0, uint16_t(framebufferWidth), uint16_t(framebufferHeight));

        const ImVec2 clipOffset = drawData->DisplayPos;
        const ImVec2 clipScale = drawData->FramebufferScale;

        for (int listIndex = 0; listIndex < drawData->CmdListsCount; listIndex++) {
            const ImDrawList* drawList = drawData->CmdLists[listIndex];
            const uint32_t vertexCount = uint32_t(drawList->VtxBuffer.Size);
            const uint32_t indexCount = uint32_t(drawList->IdxBuffer.Size);

            // Transient buffers live for exactly one frame, which is the lifetime of ImGui geometry.
            // A list that does not fit is dropped rather than partially drawn.
            if (bgfx::getAvailTransientVertexBuffer(vertexCount, backendState.vertexLayout) < vertexCount ||
                bgfx::getAvailTransientIndexBuffer(indexCount, sizeof(ImDrawIdx) == 4) < indexCount) {
                core::warn("ImGui backend: out of transient buffer space, dropping a draw list.");
                continue;
            }

            bgfx::TransientVertexBuffer vertexBuffer;
            bgfx::TransientIndexBuffer indexBuffer;
            bgfx::allocTransientVertexBuffer(&vertexBuffer, vertexCount, backendState.vertexLayout);
            bgfx::allocTransientIndexBuffer(&indexBuffer, indexCount, sizeof(ImDrawIdx) == 4);

            memcpy(vertexBuffer.data, drawList->VtxBuffer.Data, vertexCount * sizeof(ImDrawVert));
            memcpy(indexBuffer.data, drawList->IdxBuffer.Data, indexCount * sizeof(ImDrawIdx));

            bool opaque = false;   // between opaqueImage's markers
            for (int commandIndex = 0; commandIndex < drawList->CmdBuffer.Size; commandIndex++) {
                const ImDrawCmd& command = drawList->CmdBuffer[commandIndex];

                if (command.UserCallback != nullptr) {
                    // ImDrawCallback_ResetRenderState asks the backend to restore its state: every
                    // draw sets its own in full, so it only ends an opaque run.
                    if (command.UserCallback == opaqueBegin) {
                        opaque = true;
                    } else if (command.UserCallback == ImDrawCallback_ResetRenderState) {
                        opaque = false;
                    } else {
                        command.UserCallback(drawList, &command);
                    }
                    continue;
                }
                if (command.ElemCount == 0) {
                    continue;
                }

                // Clip rectangle in framebuffer pixels, clamped to the framebuffer. A rectangle
                // entirely off-screen means the command is invisible and can be skipped.
                float clipMinX = (command.ClipRect.x - clipOffset.x) * clipScale.x;
                float clipMinY = (command.ClipRect.y - clipOffset.y) * clipScale.y;
                float clipMaxX = (command.ClipRect.z - clipOffset.x) * clipScale.x;
                float clipMaxY = (command.ClipRect.w - clipOffset.y) * clipScale.y;

                clipMinX = std::max(clipMinX, 0.0f);
                clipMinY = std::max(clipMinY, 0.0f);
                clipMaxX = std::min(clipMaxX, float(framebufferWidth));
                clipMaxY = std::min(clipMaxY, float(framebufferHeight));
                if (clipMaxX <= clipMinX || clipMaxY <= clipMinY) {
                    continue;
                }

                bgfx::setScissor(uint16_t(clipMinX), uint16_t(clipMinY),
                                 uint16_t(clipMaxX - clipMinX), uint16_t(clipMaxY - clipMinY));

                // Colour only. The back buffer's alpha is the window's opacity to a compositor: blended
                // like the colour, a 95%-opaque panel left it at 0.95 and the desktop showed through the
                // panel. What drew the frame (the renderer, or a program's clear) decides it.
                bgfx::setState(opaque ? (BGFX_STATE_WRITE_RGB | BGFX_STATE_MSAA)
                                      : (BGFX_STATE_WRITE_RGB | BGFX_STATE_MSAA |
                                         BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA)));

                bgfx::TextureHandle texture = textureHandleFromImGuiID(command.GetTexID());
                bgfx::setTexture(0, backendState.textureSampler, texture);

                bgfx::setVertexBuffer(0, &vertexBuffer, command.VtxOffset, vertexCount - command.VtxOffset);
                bgfx::setIndexBuffer(&indexBuffer, command.IdxOffset, command.ElemCount);
                bgfx::submit(backendState.viewID, backendState.shaderProgram);
            }
        }
    }
}
