#ifndef PROJV_TYPE_MAPPING_H
#define PROJV_TYPE_MAPPING_H

#include <string>
#include <vector>
#include <iostream>

#include "data_structures/uniform.h"
#include "data_structures/texture.h"
#include "data_structures/framebuffer.h"
#include "data_structures/dependencyGraph.h"
#include "bgfx/bgfx.h"

#include "core/log.h"

namespace projv::graphics {
    /**
     * Maps a string-based UniformType to its corresponding bgfx::UniformType enum value. 
     * Currently only supports: (Vec4, Mat3x3, Mat4x4) for inputs. Anything else will throw an error since bgfx doesn't support it.
     * @param uniformType A string representing the type of uniform.
     * @return The corresponding bgfx::UniformType enum value.
     */
    bgfx::UniformType::Enum mapUniformType(UniformType uniformType);

    /**
     * Converts a string representation of a Uniform type to its corresponding enum value. 
     * Currently only supports: (vec4, mat3x3, mat4x4) for inputs. Anything else will throw an error since bgfx doesn't support it.
     * @param type A string representing the uniform type.
     * @return The corresponding UniformType enum value.
     */
    UniformType getUniformType(std::string type);

    /**
     * Converts a string representation of a texture format to its corresponding bgfx::TextureFormat. 
     * Please see PUTITHERE for a full list of supported formats.
     * @param format A string representing the texture format. Please see PUTITHERE for all formats.
     * @return The corresponding bgfx::TextureFormat enum value.
     */
    bgfx::TextureFormat::Enum getFormat(std::string format);

    /**
     * Converts a string representation of texture origin to it's enum value.
     * Both origins are created automatically. CPUBuffer is set manually on the CPU while CreateNew is used in frame buffers.
     * @param origin A string representing the texture origin. "CPUBuffer" or "CreateNew".
     * @return The corresponding TextureOrigin enum value.
     */
    TextureOrigin getOrigin(std::string origin);

    /**
     * Provides a list of the sampler handles for the texture dependencies for the given render pass.
     * @param frameBuffers All of the frame buffers in a renderer.
     * @param textureSamplerHandles A map of all of the textureIDs in a renderer to their texture samplers.
     * @param renderPass The RenderPass object that defines the rendering operation and its dependencies.
     * @return A vector of texture samplers and their matching textureID.
     */
    std::vector<std::pair<bgfx::UniformHandle, uint>> getDependenciesList(const std::vector<FrameBuffer>& frameBuffers, const std::unordered_map<uint, bgfx::UniformHandle>& textureSamplerHandles, const RenderPass &renderPass);
}

#endif
