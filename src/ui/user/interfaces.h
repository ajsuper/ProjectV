// The user layer's RmlUi interfaces (src/ui/user/): the bgfx render interface, its headless stand-in,
// and the system interface. Not a public header: only src/ui/user/ includes RmlUi (D2).
#ifndef PROJV_UI_USER_INTERFACES_H
#define PROJV_UI_USER_INTERFACES_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <bgfx/bgfx.h>

struct GLFWwindow;

namespace projv::ui::user {
    // RmlUi on bgfx (D5, D19, D20): geometry, textures, scissor, clip masks (a stencil shared by the
    // layers), transforms, layers, filters and gradients. Everything draws into window-sized layers --
    // the base one starting from the frame texture when there is one (D18) -- in a range of views, a
    // new one at each framebuffer switch; endFrame writes the base layer to the window.
    class BgfxRenderInterface : public Rml::RenderInterface {
    public:
        BgfxRenderInterface();
        ~BgfxRenderInterface() override;
        bool ready() const { return bgfx::isValid(programs[0]); }

        // Around a context's Render: views from `firstView` to `lastView`; `frame` the scene to start
        // from (invalid: none, and the UI is blended over the window instead).
        void beginFrame(bgfx::ViewId firstView, bgfx::ViewId lastView, int width, int height, bgfx::TextureHandle frame);
        void endFrame();

        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

        Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
        void ReleaseTexture(Rml::TextureHandle texture) override;
        // GenerateTexture with extra sampler flags (BGFX_SAMPLER_POINT for pixel art).
        Rml::TextureHandle createTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions, uint64_t sampler);

        void EnableScissorRegion(bool enable) override;
        void SetScissorRegion(Rml::Rectanglei region) override;

        void EnableClipMask(bool enable) override;
        void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;

        void SetTransform(const Rml::Matrix4f* transform) override;

        Rml::LayerHandle PushLayer() override;
        void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blendMode,
                             Rml::Span<const Rml::CompiledFilterHandle> filters) override;
        void PopLayer() override;
        Rml::TextureHandle SaveLayerAsTexture() override;
        Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;
        Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
        void ReleaseFilter(Rml::CompiledFilterHandle filter) override;
        Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
        void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                          Rml::TextureHandle texture) override;
        void ReleaseShader(Rml::CompiledShaderHandle shader) override;

        int viewsUsed() const { return int(view - firstView) + 1; }

    private:
        enum Program { Geometry, Pass, Matrix, Mask, Blur, Shadow, Gradient, ProgramCount };
        struct Mesh {
            bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
            bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
        };
        struct Target {
            bgfx::TextureHandle colour = BGFX_INVALID_HANDLE;
            bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
        };
        struct Rect { int x = 0, y = 0, w = 0, h = 0; };

        // Views: a new one each time the target changes.
        bool open(bgfx::FrameBufferHandle framebuffer, int w, int h, uint16_t clear = BGFX_CLEAR_NONE);
        bool target(const Target& t);   // the current view if it writes t at full size, else the next
        const Target& top() const { return layers[size_t(layerTop)]; }
        Target& layer(int index);
        Target& post(int index);
        void releaseTargets();

        void submit(const Mesh& mesh, Rml::Vector2f translation, bgfx::TextureHandle texture, uint64_t state,
                    uint32_t stencil, Program program);
        void fullscreen(uint8_t stencilValue);
        // A quad over the current view's [0,w]x[0,h], sampling `source` from uv0 to uv1.
        void quad(bgfx::TextureHandle source, int w, int h, float u0, float v0, float u1, float v1, Program program,
                  uint64_t state, const Rect* scissor, uint32_t stencil = BGFX_STENCIL_NONE);
        Rect region() const;   // the scissor, or the whole window
        void copy(const Target& from, const Target& to);
        void renderBlur(float sigma, Target& sourceDestination, Target& temp, Rect window);
        void renderFilters(Rml::Span<const Rml::CompiledFilterHandle> filters);

        bgfx::ViewId firstView = 0, lastView = 0, view = 0;
        uint16_t current = UINT16_MAX;   // the framebuffer the current view writes (its idx; UINT16_MAX: the window)
        int currentWidth = 0;            // and its width: a half-size downsample view is not the full target
        bool newView = true;
        bool exhausted = false, warnedExhausted = false;
        int width = 0, height = 0;
        bool startedFromFrame = false;

        bgfx::VertexLayout vertexLayout;
        bgfx::ProgramHandle programs[ProgramCount];
        bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE, maskSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle uColour = BGFX_INVALID_HANDLE, uMatrix = BGFX_INVALID_HANDLE, uTexel = BGFX_INVALID_HANDLE,
                            uLimits = BGFX_INVALID_HANDLE, uWeights = BGFX_INVALID_HANDLE, uGradient = BGFX_INVALID_HANDLE,
                            uGradientPV = BGFX_INVALID_HANDLE, uStopColours = BGFX_INVALID_HANDLE,
                            uStopPositions = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle white = BGFX_INVALID_HANDLE;

        std::unordered_map<uintptr_t, Mesh> meshes;
        uintptr_t nextMesh = 1;
        std::vector<Target> layers;       // [0] the base
        int layerTop = 0;
        Target postprocess[4];            // primary, secondary, tertiary, blend mask
        bgfx::TextureHandle stencil = BGFX_INVALID_HANDLE;   // shared by every layer
        int targetsWidth = 0, targetsHeight = 0;

        float transform[16];
        bool scissorOn = false;
        Rect scissor;
        bool clipMaskOn = false;
        uint8_t clipValue = 0;
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
