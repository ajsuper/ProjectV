// RmlUi on bgfx (interfaces.h; docs/plans/ui.md D5, D18-D20).
//
// RmlUi draws into a stack of window-sized layers. The base layer starts from the frame texture when
// there is one (the scene, so a backdrop filter blurs it) and is written to the window at the end:
// replacing it then, blended over it otherwise. Filters run through four postprocess targets, as in
// RmlUi's GL3 backend, which this follows step for step.
//
// bgfx gives each view one framebuffer and runs views in order, so every change of target opens the
// next view of the layer's range. Geometry is compiled into static buffers once, as RmlUi expects.
// Colours are premultiplied throughout (RmlUi's convention): blended ONE, INV_SRC_ALPHA.
//
// Not handled: OpenGL, whose render targets are bottom-up (ProjectV runs Vulkan, Metal and D3D).

#include "interfaces.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Log.h>
#include <bimg/decode.h>
#include <bx/allocator.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "core/log.h"
#include "graphics/engine_shaders.h"

namespace projv::ui::user {
    namespace {
        // Handles RmlUi holds for textures: the bgfx index, plus one (0 is "none" to RmlUi).
        Rml::TextureHandle toRml(bgfx::TextureHandle handle) { return Rml::TextureHandle(handle.idx) + 1; }
        bgfx::TextureHandle fromRml(Rml::TextureHandle handle) { return bgfx::TextureHandle{uint16_t(handle - 1)}; }

        constexpr uint64_t PREMULTIPLIED = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                           BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
        constexpr uint64_t REPLACE = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
        constexpr uint32_t STENCIL_KEEP = BGFX_STENCIL_OP_FAIL_S_KEEP | BGFX_STENCIL_OP_FAIL_Z_KEEP;
        constexpr uint64_t TARGET_FLAGS = BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;

        // The layers are multisampled where the GPU can render and resolve RGBA8 and the stencil so:
        // RmlUi's rounded geometry and its clip masks have hard edges, which RmlUi's own backends smooth
        // the same way. bgfx resolves a multisampled target when it is next sampled.
        uint64_t layerSamples() {
            const bgfx::Caps* caps = bgfx::getCaps();
            const bool d24s8 = caps->formats[bgfx::TextureFormat::D24S8] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER;
            const uint16_t colour = caps->formats[bgfx::TextureFormat::RGBA8];
            const uint16_t depth = caps->formats[d24s8 ? bgfx::TextureFormat::D24S8 : bgfx::TextureFormat::D0S8];
            const bool msaa = (colour & BGFX_CAPS_FORMAT_TEXTURE_MSAA) && (depth & BGFX_CAPS_FORMAT_TEXTURE_MSAA);
            return msaa ? BGFX_TEXTURE_RT_MSAA_X4 : BGFX_TEXTURE_RT;
        }

        uint32_t stencilTest(uint8_t value) {
            return BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(value) | BGFX_STENCIL_FUNC_RMASK(0xff) |
                   STENCIL_KEEP | BGFX_STENCIL_OP_PASS_Z_KEEP;
        }
        uint32_t stencilWrite(uint8_t value) {
            return BGFX_STENCIL_TEST_ALWAYS | BGFX_STENCIL_FUNC_REF(value) | BGFX_STENCIL_FUNC_RMASK(0xff) |
                   STENCIL_KEEP | BGFX_STENCIL_OP_PASS_Z_REPLACE;
        }

        constexpr const char* FRAGMENT_SHADERS[] = {"rmlui", "rmlui_pass", "rmlui_matrix", "rmlui_mask",
                                                    "rmlui_blur", "rmlui_shadow", "rmlui_gradient"};

        // ---- Filters and shaders, as compiled for RmlUi (D20) ------------------------------------
        enum class FilterType { Invalid, Passthrough, Blur, DropShadow, ColorMatrix, MaskImage };
        struct Filter {
            FilterType type = FilterType::Invalid;
            float blendFactor = 1.0f;          // Passthrough (opacity)
            float sigma = 0.0f;                // Blur, DropShadow
            Rml::Vector2f offset{0.0f, 0.0f};  // DropShadow
            Rml::ColourbPremultiplied colour;  // DropShadow
            Rml::Matrix4f matrix = Rml::Matrix4f::Identity();   // ColorMatrix
        };

        // Must match rmlui_gradient.frag.
        enum class GradientFunction { Linear, Radial, Conic, RepeatingLinear, RepeatingRadial, RepeatingConic };
        constexpr int MAX_STOPS = 16;
        struct Shader {
            GradientFunction function = GradientFunction::Linear;
            Rml::Vector2f p{0.0f, 0.0f}, v{0.0f, 0.0f};
            std::vector<float> positions;
            std::vector<Rml::Colourf> colours;   // premultiplied
        };

        // Blur kernel of seven taps (four distinct weights), downsampled first for a large sigma.
        constexpr int BLUR_WEIGHTS = 4;
        void sigmaToParameters(float desired, int& passLevel, float& sigma) {
            constexpr int maxPasses = 10;
            constexpr float maxSinglePassSigma = 3.0f;
            const int scaled = int(desired * (2.0f / maxSinglePassSigma));
            int level = 0;
            while ((1 << (level + 1)) <= scaled) level++;   // floor(log2(scaled)), 0 for scaled < 2
            passLevel = std::clamp(level, 0, maxPasses);
            sigma = std::clamp(desired / float(1 << passLevel), 0.0f, maxSinglePassSigma);
        }
        void blurWeights(float sigma, float out[BLUR_WEIGHTS]) {
            float normalisation = 0.0f;
            for (int i = 0; i < BLUR_WEIGHTS; i++) {
                if (std::abs(sigma) < 0.1f) out[i] = float(i == 0);
                else out[i] = std::exp(-float(i * i) / (2.0f * sigma * sigma)) / (std::sqrt(2.0f * 3.14159265f) * sigma);
                normalisation += (i == 0 ? 1.0f : 2.0f) * out[i];
            }
            for (int i = 0; i < BLUR_WEIGHTS; i++) out[i] /= normalisation;
        }

        Rml::Colourf toFloat(Rml::ColourbPremultiplied c) {
            return Rml::Colourf(c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f, c.alpha / 255.0f);
        }
    }

    BgfxRenderInterface::BgfxRenderInterface() {
        for (auto& p : programs) p = BGFX_INVALID_HANDLE;
        vertexLayout.begin()
            .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        static_assert(sizeof(Rml::Vertex) == 20, "Rml::Vertex is two floats, four bytes, two floats");
        std::memcpy(transform, glm::value_ptr(glm::mat4(1.0f)), sizeof(transform));

        bgfx::ShaderHandle vs = graphics::loadEngineShader("vs_rmlui");
        if (!bgfx::isValid(vs)) {
            // No shaders for this renderer, or no bgfx at all: nothing else is made (ready() is false).
            core::error("ui: no RmlUi shaders for {}: the user layer will not draw", bgfx::getRendererName(bgfx::getRendererType()));
            return;
        }
        for (int i = 0; i < ProgramCount; i++) {
            bgfx::ShaderHandle fs = graphics::loadEngineShader(FRAGMENT_SHADERS[i]);
            if (!bgfx::isValid(fs)) {
                core::error("ui: no engine shader '{}': the user layer will not draw", FRAGMENT_SHADERS[i]);
                for (int j = 0; j < i; j++) bgfx::destroy(programs[j]), programs[j] = BGFX_INVALID_HANDLE;
                bgfx::destroy(vs);
                return;
            }
            programs[i] = bgfx::createProgram(vs, fs, false);
            bgfx::destroy(fs);
        }
        bgfx::destroy(vs);

        sampler = bgfx::createUniform("s_rmluiTexture", bgfx::UniformType::Sampler);
        maskSampler = bgfx::createUniform("s_rmluiMask", bgfx::UniformType::Sampler);
        uColour = bgfx::createUniform("u_rmluiColour", bgfx::UniformType::Vec4);
        uMatrix = bgfx::createUniform("u_rmluiMatrix", bgfx::UniformType::Mat4);
        uTexel = bgfx::createUniform("u_rmluiTexel", bgfx::UniformType::Vec4);
        uLimits = bgfx::createUniform("u_rmluiLimits", bgfx::UniformType::Vec4);
        uWeights = bgfx::createUniform("u_rmluiWeights", bgfx::UniformType::Vec4);
        uGradient = bgfx::createUniform("u_rmluiGradient", bgfx::UniformType::Vec4);
        uGradientPV = bgfx::createUniform("u_rmluiGradientPV", bgfx::UniformType::Vec4);
        uStopColours = bgfx::createUniform("u_rmluiStopColours", bgfx::UniformType::Vec4, MAX_STOPS);
        uStopPositions = bgfx::createUniform("u_rmluiStopPositions", bgfx::UniformType::Vec4, MAX_STOPS / 4);
        const uint32_t whitePixel = 0xffffffffu;
        white = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, bgfx::copy(&whitePixel, 4));
    }

    BgfxRenderInterface::~BgfxRenderInterface() {
        if (!ready()) return;
        for (auto& [id, m] : meshes) {
            bgfx::destroy(m.vertices);
            bgfx::destroy(m.indices);
        }
        releaseTargets();
        for (auto& p : programs) bgfx::destroy(p);
        for (bgfx::UniformHandle u : {sampler, maskSampler, uColour, uMatrix, uTexel, uLimits, uWeights, uGradient,
                                      uGradientPV, uStopColours, uStopPositions})
            bgfx::destroy(u);
        bgfx::destroy(white);
    }

    // ---- Targets and views ---------------------------------------------------------------------

    void BgfxRenderInterface::releaseTargets() {
        for (Target& t : layers) {
            if (bgfx::isValid(t.framebuffer)) bgfx::destroy(t.framebuffer);
            if (bgfx::isValid(t.colour)) bgfx::destroy(t.colour);
        }
        layers.clear();
        for (Target& t : postprocess) {
            if (bgfx::isValid(t.framebuffer)) bgfx::destroy(t.framebuffer);
            if (bgfx::isValid(t.colour)) bgfx::destroy(t.colour);
            t = Target{};
        }
        if (bgfx::isValid(stencil)) bgfx::destroy(stencil);
        stencil = BGFX_INVALID_HANDLE;
    }

    BgfxRenderInterface::Target& BgfxRenderInterface::layer(int index) {
        while (int(layers.size()) <= index) layers.emplace_back();
        Target& t = layers[size_t(index)];
        if (!bgfx::isValid(t.framebuffer)) {
            if (!bgfx::isValid(stencil)) {
                // D24S8 where the GPU renders to it (not every AMD part does), else stencil alone.
                const bool d24s8 = bgfx::getCaps()->formats[bgfx::TextureFormat::D24S8] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER;
                stencil = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1,
                                                d24s8 ? bgfx::TextureFormat::D24S8 : bgfx::TextureFormat::D0S8,
                                                layerSamples() | BGFX_TEXTURE_RT_WRITE_ONLY);   // the colour's sample count
            }
            // Multisampled (no blits into or out of it: what it holds is drawn in and sampled out).
            t.colour = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::RGBA8,
                                             layerSamples() | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            const bgfx::TextureHandle attachments[2] = {t.colour, stencil};
            t.framebuffer = bgfx::createFrameBuffer(2, attachments, false);
        }
        return t;
    }

    BgfxRenderInterface::Target& BgfxRenderInterface::post(int index) {
        Target& t = postprocess[index];
        if (!bgfx::isValid(t.framebuffer)) {
            t.colour = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::RGBA8, TARGET_FLAGS);
            t.framebuffer = bgfx::createFrameBuffer(1, &t.colour, false);
        }
        return t;
    }

    // The next view of the range, writing `framebuffer` (invalid: the window) at w x h. False once the
    // range is spent: the frame draws what it has (D19).
    bool BgfxRenderInterface::open(bgfx::FrameBufferHandle framebuffer, int w, int h, uint16_t clear) {
        if (exhausted) return false;
        if (!newView && view >= lastView) {
            exhausted = true;
            if (!warnedExhausted) {
                core::warn("ui: the user layer ran out of views ({}-{}): the rest of this frame's UI is not drawn",
                           firstView, lastView);
                warnedExhausted = true;
            }
            return false;
        }
        view = newView ? firstView : bgfx::ViewId(view + 1);
        newView = false;
        bgfx::setViewName(view, "user ui");
        bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
        bgfx::setViewFrameBuffer(view, framebuffer);
        bgfx::setViewRect(view, 0, 0, uint16_t(w), uint16_t(h));
        bgfx::setViewScissor(view, 0, 0, 0, 0);
        bgfx::setViewClear(view, clear, 0, 1.0f, 0);
        const glm::mat4 projection = glm::ortho(0.0f, float(w), float(h), 0.0f, -1.0f, 1.0f);
        bgfx::setViewTransform(view, nullptr, glm::value_ptr(projection));
        bgfx::touch(view);
        current = framebuffer.idx;
        currentWidth = w;
        return true;
    }

    bool BgfxRenderInterface::target(const Target& t) {
        if (exhausted) return false;
        if (!newView && current == t.framebuffer.idx && currentWidth == width) return true;
        return open(t.framebuffer, width, height);
    }

    BgfxRenderInterface::Rect BgfxRenderInterface::region() const {
        return scissorOn ? scissor : Rect{0, 0, width, height};
    }

    void BgfxRenderInterface::beginFrame(bgfx::ViewId first, bgfx::ViewId last, int w, int h, bgfx::TextureHandle frame) {
        if (w != targetsWidth || h != targetsHeight) {
            releaseTargets();
            targetsWidth = w;
            targetsHeight = h;
        }
        firstView = first;
        lastView = last;
        newView = true;
        exhausted = false;
        width = w;
        height = h;
        layerTop = 0;
        scissorOn = false;
        clipMaskOn = false;
        clipValue = 0;
        std::memcpy(transform, glm::value_ptr(glm::mat4(1.0f)), sizeof(transform));

        // The base layer: the scene, when there is a frame texture (D18), else nothing.
        Target& base = layer(0);
        startedFromFrame = bgfx::isValid(frame);
        if (startedFromFrame) {
            if (open(base.framebuffer, w, h, BGFX_CLEAR_STENCIL)) {
                const float one[4] = {1, 1, 1, 1};
                bgfx::setUniform(uColour, one);
                quad(frame, w, h, 0, 0, 1, 1, Pass, REPLACE, nullptr);
            }
        } else {
            open(base.framebuffer, w, h, BGFX_CLEAR_COLOR | BGFX_CLEAR_STENCIL);
        }
    }

    void BgfxRenderInterface::endFrame() {
        // The base layer to the window: all of it (the scene came from it), or the UI over what is there.
        exhausted = false;   // the last view is kept for this
        if (!newView && view >= lastView) view = bgfx::ViewId(lastView - 1);
        if (!open(BGFX_INVALID_HANDLE, width, height)) return;
        const float one[4] = {1, 1, 1, 1};
        bgfx::setUniform(uColour, one);
        quad(layers[0].colour, width, height, 0, 0, 1, 1, Pass,
             startedFromFrame ? REPLACE
                              : (BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA)),
             nullptr);
    }

    // ---- Drawing -------------------------------------------------------------------------------

    void BgfxRenderInterface::submit(const Mesh& m, Rml::Vector2f translation, bgfx::TextureHandle texture, uint64_t state,
                                     uint32_t stencilState, Program program) {
        glm::mat4 model = glm::make_mat4(transform);
        model = glm::translate(model, glm::vec3(translation.x, translation.y, 0.0f));
        bgfx::setTransform(glm::value_ptr(model));
        bgfx::setVertexBuffer(0, m.vertices);
        bgfx::setIndexBuffer(m.indices);
        bgfx::setTexture(0, sampler, bgfx::isValid(texture) ? texture : white);
        if (scissorOn) bgfx::setScissor(uint16_t(scissor.x), uint16_t(scissor.y), uint16_t(scissor.w), uint16_t(scissor.h));
        bgfx::setState(state);
        bgfx::setStencil(stencilState);
        bgfx::submit(view, programs[program]);
    }

    void BgfxRenderInterface::quad(bgfx::TextureHandle source, int w, int h, float u0, float v0, float u1, float v1,
                                   Program program, uint64_t state, const Rect* clip, uint32_t stencilState) {
        if (bgfx::getAvailTransientVertexBuffer(4, vertexLayout) < 4 || bgfx::getAvailTransientIndexBuffer(6, true) < 6) return;
        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;
        bgfx::allocTransientVertexBuffer(&tvb, 4, vertexLayout);
        bgfx::allocTransientIndexBuffer(&tib, 6, true);
        Rml::Vertex* v = reinterpret_cast<Rml::Vertex*>(tvb.data);
        const Rml::ColourbPremultiplied solid(255, 255, 255, 255);
        v[0] = Rml::Vertex{{0.0f, 0.0f}, solid, {u0, v0}};
        v[1] = Rml::Vertex{{float(w), 0.0f}, solid, {u1, v0}};
        v[2] = Rml::Vertex{{float(w), float(h)}, solid, {u1, v1}};
        v[3] = Rml::Vertex{{0.0f, float(h)}, solid, {u0, v1}};
        const int indices[6] = {0, 1, 2, 0, 2, 3};
        std::memcpy(tib.data, indices, sizeof(indices));
        bgfx::setTransform(glm::value_ptr(glm::mat4(1.0f)));
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(&tib);
        bgfx::setTexture(0, sampler, bgfx::isValid(source) ? source : white);
        if (clip && clip->w > 0 && clip->h > 0) bgfx::setScissor(uint16_t(clip->x), uint16_t(clip->y), uint16_t(clip->w), uint16_t(clip->h));
        bgfx::setState(state);
        bgfx::setStencil(stencilState);
        bgfx::submit(view, programs[program]);
    }

    Rml::CompiledGeometryHandle BgfxRenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
        if (vertices.empty() || indices.empty()) return 0;
        Mesh m;
        m.vertices = bgfx::createVertexBuffer(bgfx::copy(vertices.data(), uint32_t(vertices.size() * sizeof(Rml::Vertex))), vertexLayout);
        m.indices = bgfx::createIndexBuffer(bgfx::copy(indices.data(), uint32_t(indices.size() * sizeof(int))), BGFX_BUFFER_INDEX32);
        const uintptr_t id = nextMesh++;
        meshes.emplace(id, m);
        return id;
    }

    void BgfxRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) {
        auto it = meshes.find(handle);
        if (it == meshes.end() || !target(top())) return;
        submit(it->second, translation, texture ? fromRml(texture) : white, PREMULTIPLIED,
               clipMaskOn ? stencilTest(clipValue) : BGFX_STENCIL_NONE, Geometry);
    }

    void BgfxRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
        auto it = meshes.find(handle);
        if (it == meshes.end()) return;
        bgfx::destroy(it->second.vertices);
        bgfx::destroy(it->second.indices);
        meshes.erase(it);
    }

    // ---- Textures ------------------------------------------------------------------------------

    Rml::TextureHandle BgfxRenderInterface::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) {
        std::ifstream file(source, std::ios::binary);
        if (!file) {
            core::warn("ui: no image at '{}'", source);
            return 0;
        }
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        static bx::DefaultAllocator allocator;
        bimg::ImageContainer* image = bimg::imageParse(&allocator, bytes.data(), uint32_t(bytes.size()), bimg::TextureFormat::RGBA8);
        if (!image) {
            core::warn("ui: '{}' is not an image bimg can read", source);
            return 0;
        }
        // RmlUi draws textures as premultiplied; files are not.
        std::vector<Rml::byte> pixels(static_cast<const Rml::byte*>(image->m_data),
                                      static_cast<const Rml::byte*>(image->m_data) + size_t(image->m_width) * image->m_height * 4);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            const unsigned a = pixels[i + 3];
            for (int c = 0; c < 3; c++) pixels[i + c] = Rml::byte((pixels[i + c] * a + 127) / 255);
        }
        dimensions = {int(image->m_width), int(image->m_height)};
        bimg::imageFree(image);
        return GenerateTexture(pixels, dimensions);
    }

    Rml::TextureHandle BgfxRenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
        if (dimensions.x <= 0 || dimensions.y <= 0 || source.size() < size_t(dimensions.x) * dimensions.y * 4) return 0;
        bgfx::TextureHandle handle = bgfx::createTexture2D(uint16_t(dimensions.x), uint16_t(dimensions.y), false, 1,
                                                           bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                                                           bgfx::copy(source.data(), uint32_t(size_t(dimensions.x) * dimensions.y * 4)));
        return bgfx::isValid(handle) ? toRml(handle) : 0;
    }

    void BgfxRenderInterface::ReleaseTexture(Rml::TextureHandle texture) {
        if (texture) bgfx::destroy(fromRml(texture));
    }

    // ---- Scissor, clip mask, transform ---------------------------------------------------------

    void BgfxRenderInterface::EnableScissorRegion(bool enable) { scissorOn = enable; }

    void BgfxRenderInterface::SetScissorRegion(Rml::Rectanglei r) {
        const int x = std::clamp(r.Left(), 0, width), y = std::clamp(r.Top(), 0, height);
        scissor = {x, y, std::clamp(r.Left() + r.Width(), 0, width) - x, std::clamp(r.Top() + r.Height(), 0, height) - y};
    }

    void BgfxRenderInterface::EnableClipMask(bool enable) { clipMaskOn = enable; }

    // The clip mask is the layers' shared stencil, as RmlUi's GL3 backend keeps it: inside the mask the
    // stencil equals clipValue. Set and SetInverse rewrite all of it; Intersect raises the value where
    // the new geometry falls inside the old mask.
    void BgfxRenderInterface::fullscreen(uint8_t value) {
        if (bgfx::getAvailTransientVertexBuffer(4, vertexLayout) < 4 || bgfx::getAvailTransientIndexBuffer(6, true) < 6) return;
        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;
        bgfx::allocTransientVertexBuffer(&tvb, 4, vertexLayout);
        bgfx::allocTransientIndexBuffer(&tib, 6, true);
        Rml::Vertex* v = reinterpret_cast<Rml::Vertex*>(tvb.data);
        const float w = float(width), h = float(height);
        const Rml::Vector2f corners[4] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
        for (int i = 0; i < 4; i++) v[i] = Rml::Vertex{corners[i], Rml::ColourbPremultiplied(255, 255, 255, 255), {0, 0}};
        const int indices[6] = {0, 1, 2, 0, 2, 3};
        std::memcpy(tib.data, indices, sizeof(indices));
        bgfx::setTransform(glm::value_ptr(glm::mat4(1.0f)));
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(&tib);
        bgfx::setTexture(0, sampler, white);
        bgfx::setState(0);   // no colour, no depth: the stencil only
        bgfx::setStencil(stencilWrite(value));
        bgfx::submit(view, programs[Geometry]);
    }

    void BgfxRenderInterface::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle handle, Rml::Vector2f translation) {
        auto it = meshes.find(handle);
        if (it == meshes.end() || !target(top())) return;
        const bool wasScissored = scissorOn;
        scissorOn = false;   // the mask is the whole of the geometry
        switch (operation) {
            case Rml::ClipMaskOperation::Set:
                fullscreen(0);
                clipValue = 1;
                submit(it->second, translation, white, 0, stencilWrite(1), Geometry);
                break;
            case Rml::ClipMaskOperation::SetInverse:
                fullscreen(1);
                clipValue = 1;
                submit(it->second, translation, white, 0, stencilWrite(0), Geometry);
                break;
            case Rml::ClipMaskOperation::Intersect:
                submit(it->second, translation, white, 0,
                       BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(clipValue) | BGFX_STENCIL_FUNC_RMASK(0xff) |
                           STENCIL_KEEP | BGFX_STENCIL_OP_PASS_Z_INCR,
                       Geometry);
                clipValue++;
                break;
        }
        scissorOn = wasScissored;
    }

    void BgfxRenderInterface::SetTransform(const Rml::Matrix4f* matrix) {
        if (matrix) std::memcpy(transform, matrix->data(), sizeof(transform));
        else std::memcpy(transform, glm::value_ptr(glm::mat4(1.0f)), sizeof(transform));
    }

    // ---- Layers --------------------------------------------------------------------------------

    void BgfxRenderInterface::copy(const Target& from, const Target& to) {
        // Drawn, not blitted: `from` may be a multisampled layer, which a blit cannot read; sampling it
        // resolves it.
        if (!open(to.framebuffer, width, height)) return;
        const float one[4] = {1, 1, 1, 1};
        bgfx::setUniform(uColour, one);
        quad(from.colour, width, height, 0, 0, 1, 1, Pass, REPLACE, nullptr);
    }

    Rml::LayerHandle BgfxRenderInterface::PushLayer() {
        layerTop++;
        Target& t = layer(layerTop);
        open(t.framebuffer, width, height, BGFX_CLEAR_COLOR);   // its colour only: the stencil is shared
        return Rml::LayerHandle(layerTop);
    }

    void BgfxRenderInterface::PopLayer() {
        if (layerTop > 0) layerTop--;
    }

    void BgfxRenderInterface::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode mode,
                                              Rml::Span<const Rml::CompiledFilterHandle> filters) {
        copy(layer(int(source)), post(0));
        renderFilters(filters);
        if (!target(layer(int(destination)))) return;
        const float one[4] = {1, 1, 1, 1};
        bgfx::setUniform(uColour, one);
        const Rect clip = region();
        // Through the clip mask when one is on: a backdrop is composited back through the element's
        // rounded border this way (RmlUi's GL3 backend leaves its stencil test on for this draw).
        quad(post(0).colour, width, height, 0, 0, 1, 1, Pass, mode == Rml::BlendMode::Replace ? REPLACE : PREMULTIPLIED,
             scissorOn ? &clip : nullptr, clipMaskOn ? stencilTest(clipValue) : BGFX_STENCIL_NONE);
    }

    Rml::TextureHandle BgfxRenderInterface::SaveLayerAsTexture() {
        const Rect bounds = region();
        if (bounds.w <= 0 || bounds.h <= 0) return 0;
        bgfx::TextureHandle texture = bgfx::createTexture2D(uint16_t(bounds.w), uint16_t(bounds.h), false, 1, bgfx::TextureFormat::RGBA8,
                                                            BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (!bgfx::isValid(texture)) return 0;
        // Resolved into a postprocess target first (a layer is multisampled), then blitted out of it in
        // a view of its own, after the copy.
        copy(top(), post(0));
        if (!open(post(1).framebuffer, width, height)) return toRml(texture);
        bgfx::blit(view, texture, 0, 0, post(0).colour, uint16_t(bounds.x), uint16_t(bounds.y), uint16_t(bounds.w), uint16_t(bounds.h));
        return toRml(texture);
    }

    Rml::CompiledFilterHandle BgfxRenderInterface::SaveLayerAsMaskImage() {
        copy(top(), post(3));
        Filter* filter = new Filter;
        filter->type = FilterType::MaskImage;
        return reinterpret_cast<Rml::CompiledFilterHandle>(filter);
    }

    // ---- Filters -------------------------------------------------------------------------------

    Rml::CompiledFilterHandle BgfxRenderInterface::CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) {
        Filter f;
        if (name == "opacity") {
            f.type = FilterType::Passthrough;
            f.blendFactor = Rml::Get(parameters, "value", 1.0f);
        } else if (name == "blur") {
            f.type = FilterType::Blur;
            f.sigma = Rml::Get(parameters, "sigma", 1.0f);
        } else if (name == "drop-shadow") {
            f.type = FilterType::DropShadow;
            f.sigma = Rml::Get(parameters, "sigma", 0.0f);
            f.colour = Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
            f.offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.0f));
        } else if (name == "brightness") {
            f.type = FilterType::ColorMatrix;
            const float value = Rml::Get(parameters, "value", 1.0f);
            f.matrix = Rml::Matrix4f::Diag(value, value, value, 1.0f);
        } else if (name == "contrast") {
            f.type = FilterType::ColorMatrix;
            const float value = Rml::Get(parameters, "value", 1.0f);
            const float grey = 0.5f - 0.5f * value;
            f.matrix = Rml::Matrix4f::Diag(value, value, value, 1.0f);
            f.matrix.SetColumn(3, Rml::Vector4f(grey, grey, grey, 1.0f));
        } else if (name == "invert") {
            f.type = FilterType::ColorMatrix;
            const float value = std::clamp(Rml::Get(parameters, "value", 1.0f), 0.0f, 1.0f);
            const float inverted = 1.0f - 2.0f * value;
            f.matrix = Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.0f);
            f.matrix.SetColumn(3, Rml::Vector4f(value, value, value, 1.0f));
        } else if (name == "grayscale") {
            f.type = FilterType::ColorMatrix;
            const float value = Rml::Get(parameters, "value", 1.0f), rev = 1.0f - value;
            const Rml::Vector3f g = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
            f.matrix = Rml::Matrix4f::FromRows({g.x + rev, g.y, g.z, 0.0f}, {g.x, g.y + rev, g.z, 0.0f},
                                               {g.x, g.y, g.z + rev, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
        } else if (name == "sepia") {
            f.type = FilterType::ColorMatrix;
            const float value = Rml::Get(parameters, "value", 1.0f), rev = 1.0f - value;
            const Rml::Vector3f r = value * Rml::Vector3f(0.393f, 0.769f, 0.189f);
            const Rml::Vector3f g = value * Rml::Vector3f(0.349f, 0.686f, 0.168f);
            const Rml::Vector3f b = value * Rml::Vector3f(0.272f, 0.534f, 0.131f);
            f.matrix = Rml::Matrix4f::FromRows({r.x + rev, r.y, r.z, 0.0f}, {g.x, g.y + rev, g.z, 0.0f},
                                               {b.x, b.y, b.z + rev, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
        } else if (name == "hue-rotate") {
            // https://www.w3.org/TR/filter-effects-1/#attr-valuedef-type-huerotate
            f.type = FilterType::ColorMatrix;
            const float value = Rml::Get(parameters, "value", 1.0f);
            const float s = std::sin(value), c = std::cos(value);
            f.matrix = Rml::Matrix4f::FromRows(
                {0.213f + 0.787f * c - 0.213f * s, 0.715f - 0.715f * c - 0.715f * s, 0.072f - 0.072f * c + 0.928f * s, 0.0f},
                {0.213f - 0.213f * c + 0.143f * s, 0.715f + 0.285f * c + 0.140f * s, 0.072f - 0.072f * c - 0.283f * s, 0.0f},
                {0.213f - 0.213f * c - 0.787f * s, 0.715f - 0.715f * c + 0.715f * s, 0.072f + 0.928f * c + 0.072f * s, 0.0f},
                {0.0f, 0.0f, 0.0f, 1.0f});
        } else if (name == "saturate") {
            f.type = FilterType::ColorMatrix;
            const float value = Rml::Get(parameters, "value", 1.0f);
            f.matrix = Rml::Matrix4f::FromRows(
                {0.213f + 0.787f * value, 0.715f - 0.715f * value, 0.072f - 0.072f * value, 0.0f},
                {0.213f - 0.213f * value, 0.715f + 0.285f * value, 0.072f - 0.072f * value, 0.0f},
                {0.213f - 0.213f * value, 0.715f - 0.715f * value, 0.072f + 0.928f * value, 0.0f},
                {0.0f, 0.0f, 0.0f, 1.0f});
        }
        if (f.type == FilterType::Invalid) {
            Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported filter type '%s'.", name.c_str());
            return 0;
        }
        return reinterpret_cast<Rml::CompiledFilterHandle>(new Filter(f));
    }

    void BgfxRenderInterface::ReleaseFilter(Rml::CompiledFilterHandle filter) {
        delete reinterpret_cast<Filter*>(filter);
    }

    // RmlUi's GL3 RenderBlur: halve down to a sigma of at most 3, blur vertically then horizontally
    // with seven taps clamped to the region, then scale back up into the region.
    void BgfxRenderInterface::renderBlur(float desiredSigma, Target& sd, Target& temp, Rect window) {
        int passLevel = 0;
        float sigma = 0.0f;
        sigmaToParameters(desiredSigma, passLevel, sigma);
        const float one[4] = {1, 1, 1, 1};

        // Halving downsamples, alternating between the two targets, each into a half-size view.
        int x0 = window.x, y0 = window.y, x1 = window.x + window.w, y1 = window.y + window.h;
        const float uvScaleX = (width % 2 == 1) ? (1.0f - 1.0f / float(width)) : 1.0f;
        const float uvScaleY = (height % 2 == 1) ? (1.0f - 1.0f / float(height)) : 1.0f;
        for (int i = 0; i < passLevel; i++) {
            x0 = (x0 + 1) / 2;
            y0 = (y0 + 1) / 2;
            x1 = std::max(x1 / 2, x0);
            y1 = std::max(y1 / 2, y0);
            const bool fromSource = i % 2 == 0;
            Target& from = fromSource ? sd : temp;
            Target& to = fromSource ? temp : sd;
            if (!open(to.framebuffer, width / 2, height / 2)) return;
            bgfx::setUniform(uColour, one);
            const Rect clip{x0, y0, x1 - x0, y1 - y0};
            quad(from.colour, width / 2, height / 2, 0, 0, uvScaleX, uvScaleY, Pass, REPLACE, &clip);
        }
        const Rect region{x0, y0, x1 - x0, y1 - y0};
        if (passLevel % 2 == 0) {   // the image must end up in temp
            if (!open(temp.framebuffer, width, height)) return;
            bgfx::setUniform(uColour, one);
            quad(sd.colour, width, height, 0, 0, 1, 1, Pass, REPLACE, &region);
        }

        float weights[BLUR_WEIGHTS];
        blurWeights(sigma, weights);
        const float limits[4] = {(float(x0) + 0.5f) / float(width), (float(y0) + 0.5f) / float(height),
                                 (float(x1) - 0.5f) / float(width), (float(y1) - 0.5f) / float(height)};

        // Vertical, temp to sd.
        if (!open(sd.framebuffer, width, height)) return;
        const float vertical[4] = {0.0f, 1.0f / float(height), 0.0f, 0.0f};
        bgfx::setUniform(uWeights, weights);
        bgfx::setUniform(uLimits, limits);
        bgfx::setUniform(uTexel, vertical);
        quad(temp.colour, width, height, 0, 0, 1, 1, Blur, REPLACE, &region);

        // Horizontal, sd to temp, inside a cleared one-pixel border (so the upscale does not pull in stale texels).
        if (!open(temp.framebuffer, width, height)) return;
        const float zero[4] = {0, 0, 0, 0};
        bgfx::setUniform(uColour, zero);
        const Rect border{std::max(x0 - 1, 0), std::max(y0 - 1, 0), std::min(x1 + 1, width) - std::max(x0 - 1, 0),
                          std::min(y1 + 1, height) - std::max(y0 - 1, 0)};
        quad(white, width, height, 0, 0, 1, 1, Pass, REPLACE, &border);
        const float horizontal[4] = {1.0f / float(width), 0.0f, 0.0f, 0.0f};
        bgfx::setUniform(uWeights, weights);
        bgfx::setUniform(uLimits, limits);
        bgfx::setUniform(uTexel, horizontal);
        quad(sd.colour, width, height, 0, 0, 1, 1, Blur, REPLACE, &region);

        // Up, temp's reduced image to the window region of sd, bilinear.
        if (!open(sd.framebuffer, width, height)) return;
        bgfx::setUniform(uColour, one);
        const float scale = 1.0f / float(1 << passLevel);
        quad(temp.colour, width, height, 0, 0, scale, scale, Pass, REPLACE, &window);
    }

    void BgfxRenderInterface::renderFilters(Rml::Span<const Rml::CompiledFilterHandle> handles) {
        const float one[4] = {1, 1, 1, 1};
        for (Rml::CompiledFilterHandle handle : handles) {
            const Filter& f = *reinterpret_cast<const Filter*>(handle);
            const Rect clip = region();
            const Rect* scissored = scissorOn ? &clip : nullptr;
            switch (f.type) {
                case FilterType::Passthrough: {
                    if (!open(post(1).framebuffer, width, height)) return;
                    const float factor[4] = {f.blendFactor, f.blendFactor, f.blendFactor, f.blendFactor};
                    bgfx::setUniform(uColour, factor);
                    quad(post(0).colour, width, height, 0, 0, 1, 1, Pass, REPLACE, scissored);
                    std::swap(postprocess[0], postprocess[1]);
                    break;
                }
                case FilterType::Blur:
                    renderBlur(f.sigma, post(0), post(1), clip);
                    break;
                case FilterType::DropShadow: {
                    // The shadow (the image's alpha in the shadow's colour, moved by its offset) into
                    // secondary, blurred, then the image over it.
                    if (!open(post(1).framebuffer, width, height)) return;
                    const Rml::Colourf c = toFloat(f.colour);
                    const float colour[4] = {c.red, c.green, c.blue, c.alpha};
                    const float limits[4] = {(float(clip.x) + 0.5f) / float(width), (float(clip.y) + 0.5f) / float(height),
                                             (float(clip.x + clip.w) - 0.5f) / float(width),
                                             (float(clip.y + clip.h) - 0.5f) / float(height)};
                    bgfx::setUniform(uColour, colour);
                    bgfx::setUniform(uLimits, limits);
                    const float du = -f.offset.x / float(width), dv = -f.offset.y / float(height);
                    quad(post(0).colour, width, height, du, dv, 1.0f + du, 1.0f + dv, Shadow, REPLACE, scissored);
                    if (f.sigma >= 0.5f) renderBlur(f.sigma, post(1), post(2), clip);
                    if (!open(post(1).framebuffer, width, height)) return;
                    bgfx::setUniform(uColour, one);
                    quad(post(0).colour, width, height, 0, 0, 1, 1, Pass, PREMULTIPLIED, scissored);
                    std::swap(postprocess[0], postprocess[1]);
                    break;
                }
                case FilterType::ColorMatrix: {
                    if (!open(post(1).framebuffer, width, height)) return;
                    bgfx::setUniform(uMatrix, f.matrix.data());
                    quad(post(0).colour, width, height, 0, 0, 1, 1, Matrix, REPLACE, scissored);
                    std::swap(postprocess[0], postprocess[1]);
                    break;
                }
                case FilterType::MaskImage: {
                    if (!open(post(1).framebuffer, width, height)) return;
                    bgfx::setTexture(1, maskSampler, post(3).colour);
                    quad(post(0).colour, width, height, 0, 0, 1, 1, Mask, REPLACE, scissored);
                    std::swap(postprocess[0], postprocess[1]);
                    break;
                }
                case FilterType::Invalid:
                    break;
            }
        }
    }

    // ---- Shaders: gradients --------------------------------------------------------------------

    Rml::CompiledShaderHandle BgfxRenderInterface::CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) {
        Shader s;
        const bool repeating = Rml::Get(parameters, "repeating", false);
        if (name == "linear-gradient") {
            s.function = repeating ? GradientFunction::RepeatingLinear : GradientFunction::Linear;
            s.p = Rml::Get(parameters, "p0", Rml::Vector2f(0.0f));
            s.v = Rml::Get(parameters, "p1", Rml::Vector2f(0.0f)) - s.p;
        } else if (name == "radial-gradient") {
            s.function = repeating ? GradientFunction::RepeatingRadial : GradientFunction::Radial;
            s.p = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
            s.v = Rml::Vector2f(1.0f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.0f));
        } else if (name == "conic-gradient") {
            s.function = repeating ? GradientFunction::RepeatingConic : GradientFunction::Conic;
            s.p = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
            const float angle = Rml::Get(parameters, "angle", 0.0f);
            s.v = {std::cos(angle), std::sin(angle)};
        } else {
            Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported shader type '%s'.", name.c_str());
            return 0;
        }
        auto it = parameters.find("color_stop_list");
        if (it == parameters.end() || it->second.GetType() != Rml::Variant::COLORSTOPLIST) return 0;
        const Rml::ColorStopList& stops = it->second.GetReference<Rml::ColorStopList>();
        for (size_t i = 0; i < stops.size() && i < size_t(MAX_STOPS); i++) {
            s.positions.push_back(stops[i].position.number);
            s.colours.push_back(toFloat(stops[i].color));
        }
        return reinterpret_cast<Rml::CompiledShaderHandle>(new Shader(std::move(s)));
    }

    void BgfxRenderInterface::RenderShader(Rml::CompiledShaderHandle handle, Rml::CompiledGeometryHandle geometry,
                                           Rml::Vector2f translation, Rml::TextureHandle) {
        auto it = meshes.find(geometry);
        if (!handle || it == meshes.end() || !target(top())) return;
        const Shader& s = *reinterpret_cast<const Shader*>(handle);
        float colours[MAX_STOPS][4] = {};
        float positions[MAX_STOPS] = {};
        for (size_t i = 0; i < s.colours.size(); i++) {
            colours[i][0] = s.colours[i].red;
            colours[i][1] = s.colours[i].green;
            colours[i][2] = s.colours[i].blue;
            colours[i][3] = s.colours[i].alpha;
            positions[i] = s.positions[i];
        }
        const float gradient[4] = {float(int(s.function)), float(s.colours.size()), 0.0f, 0.0f};
        const float pv[4] = {s.p.x, s.p.y, s.v.x, s.v.y};
        bgfx::setUniform(uGradient, gradient);
        bgfx::setUniform(uGradientPV, pv);
        bgfx::setUniform(uStopColours, colours, MAX_STOPS);
        bgfx::setUniform(uStopPositions, positions, MAX_STOPS / 4);
        submit(it->second, translation, white, PREMULTIPLIED, clipMaskOn ? stencilTest(clipValue) : BGFX_STENCIL_NONE, Gradient);
    }

    void BgfxRenderInterface::ReleaseShader(Rml::CompiledShaderHandle handle) {
        delete reinterpret_cast<Shader*>(handle);
    }
}
