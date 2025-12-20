#include "graphics/type_mapping.h"

namespace projv::graphics {
    bgfx::UniformType::Enum mapUniformType(UniformType uniformType) {
        const std::unordered_map<UniformType, bgfx::UniformType::Enum> uniformMap = {
            {UniformType::Vec4, bgfx::UniformType::Vec4},
            {UniformType::Mat3x3, bgfx::UniformType::Mat3},
            {UniformType::Mat4x4, bgfx::UniformType::Mat4}
        };
        return uniformMap.at(uniformType);
    };

    UniformType getUniformType(std::string type) {
        const std::unordered_map<std::string, UniformType> typeMap = {
            {"vec4", UniformType::Vec4}, {"mat3x3", UniformType::Mat3x3},
            {"mat4x4", UniformType::Mat4x4}
        };

        auto it = typeMap.find(type);
        if (it != typeMap.end()) {
            return it->second;
        }
        throw std::invalid_argument("Unkown type: " + type + ". Supports: vec4, mat3x3, mat4x4");
    }

    bgfx::TextureFormat::Enum getFormat(std::string format) {
        const std::unordered_map<std::string, bgfx::TextureFormat::Enum> formatMap = {
            // Single-channel formats (R)
            {"R1_INT", bgfx::TextureFormat::R1},
            {"R8_INT", bgfx::TextureFormat::R8I},
            {"R8_UINT", bgfx::TextureFormat::R8U},
            {"R16_INT", bgfx::TextureFormat::R16I},
            {"R16_UINT", bgfx::TextureFormat::R16U},
            {"R16_FLOAT", bgfx::TextureFormat::R16F},
            {"R32_INT", bgfx::TextureFormat::R32I},
            {"R32_UINT", bgfx::TextureFormat::R32U},
            {"R32_FLOAT", bgfx::TextureFormat::R32F},

            // Two-channel formats (RG)
            {"RG8_INT", bgfx::TextureFormat::RG8I},
            {"RG8_UINT", bgfx::TextureFormat::RG8U},
            {"RG16_INT", bgfx::TextureFormat::RG16I},
            {"RG16_UINT", bgfx::TextureFormat::RG16U},
            {"RG16_FLOAT", bgfx::TextureFormat::RG16F},
            {"RG32_INT", bgfx::TextureFormat::RG32I},
            {"RG32_UINT", bgfx::TextureFormat::RG32U},
            {"RG32_FLOAT", bgfx::TextureFormat::RG32F},

            // Three-channel formats (RGB)
            {"RGB8_INT", bgfx::TextureFormat::RGB8I},
            {"RGB8_UINT", bgfx::TextureFormat::RGB8U},

            // Four-channel formats (RGBA)
            {"RGBA8", bgfx::TextureFormat::RGBA8},
            {"RGBA8_INT", bgfx::TextureFormat::RGBA8I},
            {"RGBA8_UINT", bgfx::TextureFormat::RGBA8U},
            {"RGBA16_INT", bgfx::TextureFormat::RGBA16I},
            {"RGBA16_UINT", bgfx::TextureFormat::RGBA16U},
            {"RGBA16_FLOAT", bgfx::TextureFormat::RGBA16F},
            {"RGBA32_INT", bgfx::TextureFormat::RGBA32I},
            {"RGBA32_UINT", bgfx::TextureFormat::RGBA32U},
            {"RGBA32_FLOAT", bgfx::TextureFormat::RGBA32F}};

        auto it = formatMap.find(format);
        if (it != formatMap.end()) {
            return it->second;
        }
        throw std::invalid_argument("Unkown format: " + format);
    }

    TextureOrigin getOrigin(std::string origin) {
        const std::unordered_map<std::string, TextureOrigin> originMap = {
            {"CPUBuffer", TextureOrigin::CPUBuffer},
            {"CreateNew", TextureOrigin::CreateNew}};

        auto it = originMap.find(origin);
        if (it != originMap.end()) {
            return it->second;
        }
        throw std::invalid_argument("Unkown origin: " + origin);
    }

    std::vector<std::pair<bgfx::UniformHandle, uint>> getDependenciesList(const std::vector<FrameBuffer>& frameBuffers, const std::unordered_map<uint, bgfx::UniformHandle>& textureSamplerHandles, const RenderPass &renderPass) {
        core::info("type_mapping: Loading resource dependencies...");
        std::vector<std::pair<bgfx::UniformHandle, uint>> dependencies;

        if (renderPass.frameBufferInputIDs.size() == 0) {
            return dependencies;
        }

        // Adds the CreateNew/frame buffer texture ID's to the textureIDs.
        std::vector<uint> textureIDs;
        for (size_t j = 0; j < renderPass.frameBufferInputIDs.size(); j++) {
            uint frameBufferInputID = renderPass.frameBufferInputIDs[j];
            for (size_t i = 0; i < frameBuffers.size(); i++) {
                if (frameBuffers[i].frameBufferID == frameBufferInputID) {
                    for (size_t k = 0; k < frameBuffers[i].TextureIDs.size(); k++) {
                        textureIDs.emplace_back(frameBuffers[i].TextureIDs[k]);
                    }
                }
            }
        }
        
        // Adds the CPUBuffer/resourceTexture ID's to the textureIDs.
        for (size_t i = 0; i < renderPass.textureResourceIDs.size(); i++) {
            textureIDs.emplace_back(renderPass.textureResourceIDs[i]);
        }

        // Adds our texture handle and it's textureID as the std::pair. Actually getting the texture handles the pass depends on.
        for (size_t i = 0; i < textureIDs.size(); i++) {
            dependencies.emplace_back(textureSamplerHandles.at(textureIDs[i]), textureIDs[i]);
        }

        core::info("type_mapping: Successfully acquired all dependencies");
        return dependencies;
    } 
}
