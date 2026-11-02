// The user layer's RmlUi interfaces (src/ui/user/): the bgfx render interface, its headless stand-in,
// and the system interface. Not a public header: only src/ui/user/ includes RmlUi (D2).
#ifndef PROJV_UI_USER_INTERFACES_H
#define PROJV_UI_USER_INTERFACES_H

#include <cstdint>
#include <string>
#include <unordered_map>

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <bgfx/bgfx.h>

struct GLFWwindow;

namespace projv::ui::user {
    // RmlUi on bgfx, in one view (USER_VIEW), into the back buffer (D5). U3: geometry, textures,
    // scissor, clip masks (the back buffer's stencil) and transforms. Layers, filters and shaders are
    // U4's, and RmlUi draws without them until then.
    class BgfxRenderInterface : public Rml::RenderInterface {
    public:
        BgfxRenderInterface();
        ~BgfxRenderInterface() override;
        bool ready() const { return bgfx::isValid(program); }

        // Before a context's Render: the view, its projection, a clear of the stencil.
        void beginFrame(bgfx::ViewId view, int width, int height);

        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

        Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
        void ReleaseTexture(Rml::TextureHandle texture) override;

        void EnableScissorRegion(bool enable) override;
        void SetScissorRegion(Rml::Rectanglei region) override;

        void EnableClipMask(bool enable) override;
        void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;

        void SetTransform(const Rml::Matrix4f* transform) override;

    private:
        struct Geometry {
            bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
            bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
        };
        void submit(const Geometry& geometry, Rml::Vector2f translation, bgfx::TextureHandle texture,
                    uint64_t state, uint32_t stencil);
        void fullscreen(uint8_t stencilValue);   // writes the stencil everywhere, no colour

        bgfx::ViewId view = 0;
        int width = 0, height = 0;
        bgfx::VertexLayout layout;
        bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle white = BGFX_INVALID_HANDLE;
        std::unordered_map<uintptr_t, Geometry> geometries;
        uintptr_t nextGeometry = 1;
        float transform[16];
        bool transformed = false;
        bool scissorOn = false;
        int scissor[4] = {0, 0, 0, 0};
        bool clipMaskOn = false;
        uint8_t clipValue = 0;   // the stencil value inside the clip mask
    };

    // Headless (D11): keeps nothing on a GPU, counts what it is asked to draw.
    class RecordingRenderInterface : public Rml::RenderInterface {
    public:
        int drawCalls = 0;        // RenderGeometry calls since the last beginFrame
        int clipMaskCalls = 0;
        int textures = 0;         // alive

        void beginFrame() { drawCalls = 0; clipMaskCalls = 0; }

        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return next++; }
        void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override { drawCalls++; }
        void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
        Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String&) override {
            dimensions = {1, 1};
            textures++;
            return next++;
        }
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { textures++; return next++; }
        void ReleaseTexture(Rml::TextureHandle) override { textures--; }
        void EnableScissorRegion(bool) override {}
        void SetScissorRegion(Rml::Rectanglei) override {}
        void EnableClipMask(bool) override {}
        void RenderToClipMask(Rml::ClipMaskOperation, Rml::CompiledGeometryHandle, Rml::Vector2f) override { clipMaskCalls++; }
        void SetTransform(const Rml::Matrix4f*) override {}

    private:
        uintptr_t next = 1;
    };

    // Time and logging from the engine; the clipboard from GLFW, when there is a window.
    class EngineSystemInterface : public Rml::SystemInterface {
    public:
        double seconds = 0.0;          // advanced by the layer, unscaled
        GLFWwindow* window = nullptr;

        double GetElapsedTime() override { return seconds; }
        bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
        void SetClipboardText(const Rml::String& text) override;
        void GetClipboardText(Rml::String& text) override;
    };
}

#endif
