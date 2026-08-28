#include "graphics/disk_io.h"

namespace projv::graphics {
    bgfx::ShaderHandle loadShader(const std::string &path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            core::error("Failed to open shader: {}", path);
            return BGFX_INVALID_HANDLE;
        }

        std::streamsize fileSize = file.tellg();
        if (fileSize <= 0) {
            core::error("Shader file is empty or error reading size: {}", path);
            return BGFX_INVALID_HANDLE;
        }
        file.seekg(0, std::ios::beg);

        std::vector<char> loadedBuffer(fileSize);
        if (!file.read(loadedBuffer.data(), fileSize)) {
            core::error("Failed to read shader: {}", path);
            return BGFX_INVALID_HANDLE;
        }

        const bgfx::Memory *fileMemory = bgfx::copy(loadedBuffer.data(), static_cast<uint32_t>(fileSize));
        return bgfx::createShader(fileMemory);
    }

    std::vector<char> readFile(const std::string &filename) {
        core::info("Openning file {}...", filename);
        std::ifstream file(filename, std::ios::ate | std::ios::binary);

        if (!file.is_open()) {
            throw std::runtime_error("failed to open file!");
        }
        // ios::ate means read at the bottom of the file.
        size_t fileSize = (size_t)file.tellg();
        std::vector<char> buffer(fileSize);

        file.seekg(0);
        file.read(buffer.data(), fileSize);

        file.close();

        return buffer;
    }

    std::vector<Uniform> loadUniformTypes(nlohmann::json& resourceData) {
        core::info("Loading uniform types...");
        std::vector<Uniform> uniforms;
        for (const auto &uniform : resourceData["uniforms"]) {
            core::info("Uniform:: name: {} , Type: {}", uniform["name"].dump(), uniform["type"].dump());
            Uniform uniformResource;
            uniformResource.type = getUniformType(uniform["type"]);
            uniformResource.name = uniform["name"];
            uniforms.emplace_back(uniformResource);
        }
        return uniforms;
    }

    std::vector<Shader> loadShaders(nlohmann::json& resourceData, std::string rendererPath) {
        core::info("Loading shaders...");
        std::vector<Shader> shaders;
        for (const auto &shader : resourceData["shaders"]) {
            core::info("Shader:: shaderID: {}, path: {}", shader["shaderID"].dump(), shader["path"].dump());
            Shader shaderResource;
            shaderResource.shaderID = shader["shaderID"];
            if(shader["shaderID"] <= 0) core::error("ShaderID {} is less than 0!", shader["shaderID"].dump());
            shaderResource.filePath = shader["path"];
            shaderResource.shaderFileContents = readFile(shaderResource.filePath);
            shaders.emplace_back(shaderResource);
        }
        return shaders;
    }

    FrameBuffer getFrameBuffer(uint32_t frameBufferID, const std::vector<FrameBuffer>& frameBuffers) {
        for (size_t i = 0; i < frameBuffers.size(); i++) {
            if (frameBufferID == frameBuffers[i].frameBufferID) {
                return frameBuffers[i];
            }
        }
        throw std::runtime_error("Failed to find frame buffer");
    }

    bool doesFrameBufferNeedPingPong(uint32_t frameBufferID, const std::vector<RenderPass>& renderPasses) {
        for (size_t i = 0; i < renderPasses.size(); i++) {
            RenderPass renderPass = renderPasses[i];
            uint32_t frameBufferOutputID = renderPass.frameBufferOutputID;
            for (size_t j = 0; j < renderPass.frameBufferInputIDs.size(); j++) {
                if (frameBufferID == frameBufferOutputID && (renderPass.frameBufferInputIDs[j] == frameBufferOutputID)) {
                    core::info("{} detected as a ping pong frame buffer.", frameBufferID);
                    return true;
                }
            }
        }
        return false;
    }

    std::vector<Texture> setPingPongTextures(const std::vector<Texture>& texturesOld, const std::vector<RenderPass>& renderPasses, const std::vector<FrameBuffer>& frameBuffers) {
        std::vector<Texture> texturesNew = texturesOld;
        for (size_t i = 0; i < frameBuffers.size(); i++) {
            FrameBuffer frameBuffer = frameBuffers[i];
            if (frameBuffer.pingPongFBO) {
                for (size_t j = 0; j < frameBuffer.TextureIDs.size(); j++) {
                    for (size_t k = 0; k < texturesNew.size(); k++) {
                        if (frameBuffer.TextureIDs[j] == texturesNew[k].textureID) {
                            core::info("{} detected as a ping pong texture.", texturesNew[k].textureID);
                            texturesNew[k].pingPongFlag = true;
                        }
                    }
                }
            }
        }
        return texturesNew;
    }

    std::vector<FrameBuffer> setPingPongFrameBuffers(const std::vector<FrameBuffer>& frameBuffersOld, const std::vector<RenderPass>& renderPasses) {
        std::vector<FrameBuffer> frameBuffersNew = frameBuffersOld;
        for (size_t i = 0; i < frameBuffersNew.size(); i++) {
            frameBuffersNew[i].pingPongFBO = doesFrameBufferNeedPingPong(frameBuffersNew[i].frameBufferID, renderPasses);    
        }
        return frameBuffersNew;
    }

    std::vector<Texture> loadTextures(nlohmann::json& resourceData) {
        std::vector<Texture> textures;
        for (const auto &texture : resourceData["textures"]) {
            const std::string textureLabel =
                "texture " + texture.value("texID", nlohmann::json(-1)).dump() +
                " (" + texture.value("name", std::string("unnamed")) + ")";

            // Deliberately fatal, and deliberately specific. `resizable` was replaced rather than
            // extended, so a file still using it would otherwise load under a default rule and render
            // at the wrong resolution -- the failure would be a picture, not an error message.
            if (texture.contains("resizable")) {
                throw std::runtime_error(
                    "resources.json " + textureLabel + " still declares \"resizable\", which has been "
                    "replaced by \"sizeMode\". Migrate it: a render target that followed the window "
                    "becomes \"sizeMode\": \"relative\" (optionally with \"scale\", default 1.0); "
                    "anything with a size of its own -- an uploaded image, a lookup table -- becomes "
                    "\"sizeMode\": \"fixed\" keeping its resX/resY. Note that a CPUBuffer texture "
                    "declared resizable was never actually resized, so those are \"fixed\"."
                );
            }
            if (!texture.contains("sizeMode")) {
                throw std::runtime_error(
                    "resources.json " + textureLabel + " has no \"sizeMode\". Use \"relative\" for a "
                    "render target that follows the render resolution, or \"fixed\" for a texture "
                    "whose resX/resY are its real size."
                );
            }

            Texture textureResource;
            textureResource.textureID = texture["texID"];
            if(texture["texID"] <= 0) core::error("TextureID {} is less than 0!", texture["texID"].dump());
            textureResource.name = texture["name"];
            textureResource.format = getFormat(texture["format"]);
            textureResource.origin = getOrigin(texture["origin"]);
            textureResource.readBack = texture.value("readBack", false);

            const std::string sizeMode = texture["sizeMode"];
            if (sizeMode == "relative") {
                textureResource.sizeMode = TextureSizeMode::Relative;
                textureResource.scale = texture.value("scale", 1.0f);
                if (!(textureResource.scale > 0.0f)) {
                    throw std::runtime_error("resources.json " + textureLabel + " has \"scale\": " +
                        std::to_string(textureResource.scale) + ". A scale must be greater than zero.");
                }
                // Zeroed rather than carried: a relative texture's size is the render resolution times
                // its scale, and leaving a declared size here invites something reading the stale one.
                textureResource.resolutionX = 0;
                textureResource.resolutionY = 0;
            } else if (sizeMode == "fixed") {
                textureResource.sizeMode = TextureSizeMode::Fixed;
                if (!texture.contains("resX") || !texture.contains("resY")) {
                    throw std::runtime_error("resources.json " + textureLabel +
                        " is \"sizeMode\": \"fixed\" but has no resX/resY to be fixed at.");
                }
                textureResource.resolutionX = texture["resX"];
                textureResource.resolutionY = texture["resY"];
            } else {
                throw std::runtime_error("resources.json " + textureLabel + " has unknown \"sizeMode\": \"" +
                    sizeMode + "\". Expected \"relative\" or \"fixed\".");
            }

            core::info("Texture:: texID: {} , name: {} , format: {} , sizeMode: {} , scale: {} , resolution: ({}, {}) , origin: {}",
                        texture["texID"].dump(), texture["name"].dump(), texture["format"].dump(), sizeMode,
                        textureResource.scale, textureResource.resolutionX, textureResource.resolutionY,
                        texture["origin"].dump());
            textures.emplace_back(textureResource);
        }
        return textures;
    }

    // A framebuffer has no size of its own -- it is a list of texture IDs, and its size is its
    // attachments'. Every graphics API requires those attachments to match, so an inconsistent set is
    // invalid by construction and worth refusing at load rather than meeting later as a bind failure
    // or a partly-written target.
    //
    // Checked on the RULE, not on the current pixel size: two relative attachments with different
    // scales agree at some render resolutions and disagree at others, which is the worst version of
    // this bug to ship.
    void validateFrameBufferAttachmentSizes(const std::vector<FrameBuffer>& frameBuffers, const std::vector<Texture>& textures) {
        for (const FrameBuffer& frameBuffer : frameBuffers) {
            const Texture* reference = nullptr;
            for (uint32_t textureID : frameBuffer.TextureIDs) {
                const Texture* current = nullptr;
                for (const Texture& texture : textures) {
                    if (texture.textureID == textureID) { current = &texture; break; }
                }
                if (current == nullptr) {
                    throw std::runtime_error("resources.json framebuffer " + std::to_string(frameBuffer.frameBufferID) +
                        " lists textureID " + std::to_string(textureID) + ", which no texture declares.");
                }
                if (reference == nullptr) { reference = current; continue; }

                const bool sameRule = reference->sizeMode == current->sizeMode &&
                    (reference->sizeMode == TextureSizeMode::Relative
                        ? reference->scale == current->scale
                        : (reference->resolutionX == current->resolutionX &&
                           reference->resolutionY == current->resolutionY));
                if (!sameRule) {
                    throw std::runtime_error("resources.json framebuffer " + std::to_string(frameBuffer.frameBufferID) +
                        " attaches textures that resolve to different sizes: \"" + reference->name + "\" and \"" +
                        current->name + "\". Every attachment of one framebuffer must share the same sizeMode, and "
                        "the same scale (relative) or the same resX/resY (fixed).");
                }
            }
        }
    }

    std::vector<FrameBuffer> loadFrameBuffers(nlohmann::json& resourceData) {
        std::vector<FrameBuffer> frameBuffers;
        for (const auto &frameBuffer : resourceData["framebuffers"]) {
            const std::string logFormat = "Framebuffer:: fboID: {} , textureIDs: {}";
            core::info(logFormat,
                        frameBuffer["fboID"].dump(), 
                        frameBuffer["textureIDs"].dump());
            FrameBuffer frameBufferResource;
            frameBufferResource.frameBufferID = frameBuffer["fboID"];
            for (auto &textureID : frameBuffer["textureIDs"]) {
                frameBufferResource.TextureIDs.emplace_back(textureID);
            }
            frameBuffers.emplace_back(frameBufferResource);
        }
        return frameBuffers;
    }

    std::vector<RenderPass> loadRenderPasses(nlohmann::json& dependencyGraphData) {
        std::vector<RenderPass> renderPasses;
        uint32_t renderPassID;
        for (const auto &renderPass : dependencyGraphData["renderer"]) {
            const std::string logFormat = "RenderPass:: shaderID: {}, frameBufferInputIDs: {}, resourceTexturesIDs: {}, frameBufferOutputID: {}, multiPass: {}";
            core::info(logFormat,
                        renderPass["shaderID"].dump(), 
                        renderPass["frameBufferInputIDs"].dump(),
                        renderPass["resourceTexturesIDs"].dump(),
                        renderPass["frameBufferOutputID"].dump(),
                        renderPass["multiPass"].dump());
            RenderPass renderPassDescription;
            renderPassDescription.shaderID = renderPass["shaderID"];

            for (auto &frameBufferInputID : renderPass["frameBufferInputIDs"]) {
                renderPassDescription.frameBufferInputIDs.emplace_back(frameBufferInputID);
            }

            for (auto &resourceTextureID : renderPass["resourceTexturesIDs"]) {
                renderPassDescription.textureResourceIDs.emplace_back(resourceTextureID);
            }

            renderPassDescription.frameBufferOutputID = renderPass["frameBufferOutputID"];
            renderPassDescription.multiPass = renderPass["multiPass"];
            renderPassDescription.multiPassPassNumber = 1; // 0 Unless specified below.
            
            renderPasses.emplace_back(renderPassDescription);
            // i = 1 because multiPassPassNumber should be 1 for the second pass of the shader.
            for(size_t i = 1; i <= renderPassDescription.multiPass; i++) {
                renderPassDescription.multiPassPassNumber = i;
                renderPasses.emplace_back(renderPassDescription);
            }
        }
        return renderPasses;
    }

    RendererSpecification loadRendererSpecification(std::string rendererPath) {
        core::info("loadRendererSpecification: loading from \"{}\"", rendererPath);
        RendererSpecification renderer;
        std::ifstream resourceJSON(rendererPath + "/resources.json");
        std::ifstream dependencyGraphJSON(rendererPath + "/render.json");

        nlohmann::json resourceData = nlohmann::json::parse(resourceJSON);
        nlohmann::json dependencyGraphData = nlohmann::json::parse(dependencyGraphJSON);

        renderer.resources.uniforms = loadUniformTypes(resourceData);
        renderer.resources.shaders = loadShaders(resourceData, rendererPath);
        renderer.resources.textures = loadTextures(resourceData);
        renderer.resources.FrameBuffers = loadFrameBuffers(resourceData);
        renderer.dependencyGraph.renderPasses = loadRenderPasses(dependencyGraphData);
        renderer.resources.FrameBuffers = setPingPongFrameBuffers(renderer.resources.FrameBuffers, renderer.dependencyGraph.renderPasses);
        renderer.resources.textures = setPingPongTextures(renderer.resources.textures, renderer.dependencyGraph.renderPasses, renderer.resources.FrameBuffers); // Ping pong textures depend on other resources to be specified.

        // Refused here rather than at bind time: a framebuffer whose attachments resolve to different
        // sizes cannot be created correctly at any resolution.
        validateFrameBufferAttachmentSizes(renderer.resources.FrameBuffers, renderer.resources.textures);

        core::render("loadRendererSpecification: {} shaders {} textures {} framebuffers {} renderpasses",
                     renderer.resources.shaders.size(),
                     renderer.resources.textures.size(),
                     renderer.resources.FrameBuffers.size(),
                     renderer.dependencyGraph.renderPasses.size());

        return renderer;
    }
}
