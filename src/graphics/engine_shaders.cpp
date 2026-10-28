#include "graphics/engine_shaders.h"

#include "core/log.h"

namespace projv::graphics {
    std::span<const uint8_t> engineShader(std::string_view name, bgfx::RendererType::Enum type) {
        for (const detail::EngineShaderEntry* entry = detail::engineShaderTable; entry->name; ++entry) {
            if (entry->type == type && name == entry->name) {
                return {entry->data, entry->size};
            }
        }
        return {};
    }

    std::span<const uint8_t> engineShader(std::string_view name) {
        return engineShader(name, bgfx::getRendererType());
    }

    bgfx::ShaderHandle loadEngineShader(std::string_view name) {
        std::span<const uint8_t> bytes = engineShader(name);
        if (bytes.empty()) {
            core::error("No engine shader '{}' for renderer {}", name, bgfx::getRendererName(bgfx::getRendererType()));
            return BGFX_INVALID_HANDLE;
        }
        // makeRef: the bytes live in the library for the life of the process.
        return bgfx::createShader(bgfx::makeRef(bytes.data(), static_cast<uint32_t>(bytes.size())));
    }
}
