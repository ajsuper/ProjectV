// RmlUi on bgfx (interfaces.h). Geometry is compiled into static buffers once, as RmlUi expects;
// every draw is one submit in the user layer's view, in sequential order. Colours are premultiplied
// (RmlUi's convention), blended ONE, INV_SRC_ALPHA, and only RGB is written: the back buffer's alpha
// is the window's opacity to a compositor, as the dev layer found (docs/plans/ui.md, U1).

#include "interfaces.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

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

        constexpr uint64_t DRAW_STATE = BGFX_STATE_WRITE_RGB | BGFX_STATE_MSAA |
                                        BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
        constexpr uint32_t STENCIL_KEEP = BGFX_STENCIL_OP_FAIL_S_KEEP | BGFX_STENCIL_OP_FAIL_Z_KEEP;

        uint32_t stencilTest(uint8_t value) {
            return BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(value) | BGFX_STENCIL_FUNC_RMASK(0xff) |
                   STENCIL_KEEP | BGFX_STENCIL_OP_PASS_Z_KEEP;
        }
        uint32_t stencilWrite(uint8_t value) {
            return BGFX_STENCIL_TEST_ALWAYS | BGFX_STENCIL_FUNC_REF(value) | BGFX_STENCIL_FUNC_RMASK(0xff) |
                   STENCIL_KEEP | BGFX_STENCIL_OP_PASS_Z_REPLACE;
        }
    }

    BgfxRenderInterface::BgfxRenderInterface() {
        layout.begin()
            .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        static_assert(sizeof(Rml::Vertex) == 20, "Rml::Vertex is two floats, four bytes, two floats");

        bgfx::ShaderHandle vs = graphics::loadEngineShader("vs_rmlui");
        bgfx::ShaderHandle fs = graphics::loadEngineShader("rmlui");
        if (!bgfx::isValid(vs) || !bgfx::isValid(fs)) {
            // No shaders for this renderer, or no bgfx at all: nothing else is made (ready() is false).
            core::error("ui: no RmlUi shaders for {}: the user layer will not draw", bgfx::getRendererName(bgfx::getRendererType()));
            if (bgfx::isValid(vs)) bgfx::destroy(vs);
            if (bgfx::isValid(fs)) bgfx::destroy(fs);
            return;
        }
        program = bgfx::createProgram(vs, fs, true);
        sampler = bgfx::createUniform("s_rmluiTexture", bgfx::UniformType::Sampler);
        const uint32_t whitePixel = 0xffffffffu;
        white = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, bgfx::copy(&whitePixel, 4));
        std::memcpy(transform, glm::value_ptr(glm::mat4(1.0f)), sizeof(transform));
    }

    BgfxRenderInterface::~BgfxRenderInterface() {
        for (auto& [id, g] : geometries) {
            bgfx::destroy(g.vertices);
            bgfx::destroy(g.indices);
        }
        if (!bgfx::isValid(program)) return;
        bgfx::destroy(program);
        bgfx::destroy(sampler);
        bgfx::destroy(white);
    }

    void BgfxRenderInterface::beginFrame(bgfx::ViewId v, int w, int h) {
        view = v;
        width = w;
        height = h;
        bgfx::setViewName(view, "user ui");
        bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
        bgfx::setViewRect(view, 0, 0, uint16_t(w), uint16_t(h));
        // Stencil only: the frame under the UI is kept. The clip masks start from nothing each frame.
        bgfx::setViewClear(view, BGFX_CLEAR_STENCIL, 0, 1.0f, 0);
        bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
        const glm::mat4 projection = glm::ortho(0.0f, float(w), float(h), 0.0f, -1.0f, 1.0f);
        bgfx::setViewTransform(view, nullptr, glm::value_ptr(projection));
        bgfx::touch(view);   // so the stencil clear happens even on a frame with nothing drawn
        clipMaskOn = false;
        clipValue = 0;
    }

    Rml::CompiledGeometryHandle BgfxRenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
        if (vertices.empty() || indices.empty()) return 0;
        Geometry g;
        g.vertices = bgfx::createVertexBuffer(bgfx::copy(vertices.data(), uint32_t(vertices.size() * sizeof(Rml::Vertex))), layout);
        g.indices = bgfx::createIndexBuffer(bgfx::copy(indices.data(), uint32_t(indices.size() * sizeof(int))), BGFX_BUFFER_INDEX32);
        const uintptr_t id = nextGeometry++;
        geometries.emplace(id, g);
        return id;
    }

    void BgfxRenderInterface::submit(const Geometry& g, Rml::Vector2f translation, bgfx::TextureHandle texture,
                                     uint64_t state, uint32_t stencil) {
        glm::mat4 model = glm::make_mat4(transform);
        model = glm::translate(model, glm::vec3(translation.x, translation.y, 0.0f));
        bgfx::setTransform(glm::value_ptr(model));
        bgfx::setVertexBuffer(0, g.vertices);
        bgfx::setIndexBuffer(g.indices);
        bgfx::setTexture(0, sampler, bgfx::isValid(texture) ? texture : white);
        if (scissorOn) bgfx::setScissor(uint16_t(scissor[0]), uint16_t(scissor[1]), uint16_t(scissor[2]), uint16_t(scissor[3]));
        bgfx::setState(state);
        bgfx::setStencil(stencil);
        bgfx::submit(view, program);
    }

    void BgfxRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) {
        auto it = geometries.find(handle);
        if (it == geometries.end() || !ready()) return;
        submit(it->second, translation, texture ? fromRml(texture) : white, DRAW_STATE,
               clipMaskOn ? stencilTest(clipValue) : BGFX_STENCIL_NONE);
    }

    void BgfxRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
        auto it = geometries.find(handle);
        if (it == geometries.end()) return;
        bgfx::destroy(it->second.vertices);
        bgfx::destroy(it->second.indices);
        geometries.erase(it);
    }

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

    void BgfxRenderInterface::EnableScissorRegion(bool enable) { scissorOn = enable; }

    void BgfxRenderInterface::SetScissorRegion(Rml::Rectanglei region) {
        const int x = std::clamp(region.Left(), 0, width), y = std::clamp(region.Top(), 0, height);
        scissor[0] = x;
        scissor[1] = y;
        scissor[2] = std::clamp(region.Left() + region.Width(), 0, width) - x;
        scissor[3] = std::clamp(region.Top() + region.Height(), 0, height) - y;
    }

    void BgfxRenderInterface::EnableClipMask(bool enable) { clipMaskOn = enable; }

    // The clip mask is the back buffer's stencil, as RmlUi's GL3 backend keeps it: inside the mask the
    // stencil equals clipValue. Set and SetInverse rewrite all of it; Intersect raises the value where
    // the new geometry falls inside the old mask.
    void BgfxRenderInterface::fullscreen(uint8_t value) {
        if (bgfx::getAvailTransientVertexBuffer(4, layout) < 4 || bgfx::getAvailTransientIndexBuffer(6, true) < 6) return;
        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;
        bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
        bgfx::allocTransientIndexBuffer(&tib, 6, true);
        Rml::Vertex* v = reinterpret_cast<Rml::Vertex*>(tvb.data);
        const float w = float(width), h = float(height);
        const Rml::Vector2f corners[4] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
        for (int i = 0; i < 4; i++) v[i] = Rml::Vertex{corners[i], Rml::ColourbPremultiplied(255, 255, 255, 255), {0, 0}};
        const int quad[6] = {0, 1, 2, 0, 2, 3};
        std::memcpy(tib.data, quad, sizeof(quad));
        bgfx::setTransform(glm::value_ptr(glm::mat4(1.0f)));
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(&tib);
        bgfx::setTexture(0, sampler, white);
        bgfx::setState(0);   // no colour, no depth: the stencil only
        bgfx::setStencil(stencilWrite(value));
        bgfx::submit(view, program);
    }

    void BgfxRenderInterface::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle handle, Rml::Vector2f translation) {
        auto it = geometries.find(handle);
        if (it == geometries.end() || !ready()) return;
        const bool scissor = scissorOn;
        scissorOn = false;   // the mask is the whole of the geometry
        switch (operation) {
            case Rml::ClipMaskOperation::Set:
                fullscreen(0);
                clipValue = 1;
                submit(it->second, translation, white, 0, stencilWrite(1));
                break;
            case Rml::ClipMaskOperation::SetInverse:
                fullscreen(1);
                clipValue = 1;
                submit(it->second, translation, white, 0, stencilWrite(0));
                break;
            case Rml::ClipMaskOperation::Intersect:
                submit(it->second, translation, white, 0,
                       BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(clipValue) | BGFX_STENCIL_FUNC_RMASK(0xff) |
                           STENCIL_KEEP | BGFX_STENCIL_OP_PASS_Z_INCR);
                clipValue++;
                break;
        }
        scissorOn = scissor;
    }

    void BgfxRenderInterface::SetTransform(const Rml::Matrix4f* matrix) {
        transformed = matrix != nullptr;
        if (matrix) std::memcpy(transform, matrix->data(), sizeof(transform));
        else std::memcpy(transform, glm::value_ptr(glm::mat4(1.0f)), sizeof(transform));
    }
}
