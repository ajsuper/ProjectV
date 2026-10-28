#ifndef PROJV_ENGINE_SHADERS_H
#define PROJV_ENGINE_SHADERS_H

#include <cstdint>
#include <span>
#include <string_view>

#include <bgfx/bgfx.h>

// The engine's own shaders (shaders/engine/), compiled into the library for each profile the build
// targets (cmake/ProjectVEngineShaders.cmake). Nothing is staged or read from disk.
//
//     bgfx::ShaderHandle vs = projv::graphics::loadEngineShader("vs_quad");   // the fullscreen quad
//
// Plan: docs/plans/shader-library.md, D3.
namespace projv::graphics {
    /**
     * The compiled bytes of an engine shader for a renderer type, ready for bgfx::createShader.
     * @param name The shader's file base name ("vs_quad").
     * @param type The renderer it is for.
     * @return The binary, or an empty span when the library has no such shader for that renderer.
     */
    std::span<const uint8_t> engineShader(std::string_view name, bgfx::RendererType::Enum type);

    /**
     * engineShader for the running renderer (bgfx::getRendererType()); bgfx must be initialised.
     */
    std::span<const uint8_t> engineShader(std::string_view name);

    /**
     * Creates a shader from an engine shader for the running renderer.
     * @return The handle, or BGFX_INVALID_HANDLE (logged) when there is no such shader.
     */
    bgfx::ShaderHandle loadEngineShader(std::string_view name);

    namespace detail {
        struct EngineShaderEntry {
            const char* name;                  // nullptr ends the table
            bgfx::RendererType::Enum type;
            const uint8_t* data;
            uint32_t size;
        };
        extern const EngineShaderEntry engineShaderTable[];
    }
}

#endif
