#include "graphics/disk_io.h"

namespace projv::graphics {
    bgfx::ShaderHandle loadShader(const std::string &path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            fprintf(stderr, "Failed to open shader: %s\n", path.c_str());
            return BGFX_INVALID_HANDLE;
        }

        std::streamsize fileSize = file.tellg();
        if (fileSize <= 0) {
            fprintf(stderr, "Shader file is empty or error reading size: %s\n", path.c_str());
            return BGFX_INVALID_HANDLE;
        }
        file.seekg(0, std::ios::beg);

        std::vector<char> loadedBuffer(fileSize);
        if (!file.read(loadedBuffer.data(), fileSize)) {
            fprintf(stderr, "Failed to read shader: %s\n", path.c_str());
            return BGFX_INVALID_HANDLE;
        }

        const bgfx::Memory *fileMemory = bgfx::copy(loadedBuffer.data(), static_cast<uint32_t>(fileSize));
        return bgfx::createShader(fileMemory);
    }

    std::vector<char> readFile(const std::string &filename) {
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
        std::vector<Uniform> uniforms;
        for (const auto &uniform : resourceData["uniforms"]) {
            std::cout << "Uniform:: name: " << uniform["name"] << ", Type: " << uniform["type"] << std::endl;
            Uniform uniformResource;
            uniformResource.type = getUniformType(uniform["type"]);
            uniformResource.name = uniform["name"];
            uniforms.emplace_back(uniformResource);
        }
        return uniforms;
    }

    std::vector<Shader> loadShaders(nlohmann::json& resourceData, std::string rendererPath) {
        std::vector<Shader> shaders;
        for (const auto &shader : resourceData["shaders"]) {
            std::cout << "Shader:: shaderID: " << shader["shaderID"] << ", path: " << shader["path"] << std::endl;
            Shader shaderResource;
            shaderResource.shaderID = shader["shaderID"];
            if(shader["shaderID"] <= 0) std::cout << "ERROR: ShaderID less than 0! : " << shader["shaderID"] << std::endl;
            shaderResource.filePath = shader["path"];
            shaderResource.shaderFileContents = readFile(rendererPath);
            shaders.emplace_back(shaderResource);
        }
        return shaders;
    }

    FrameBuffer getFrameBuffer(uint frameBufferID, const std::vector<FrameBuffer>& frameBuffers) {
        for (size_t i = 0; i < frameBuffers.size(); i++) {
            if (frameBufferID == frameBuffers[i].frameBufferID) {
                return frameBuffers[i];
            }
        }
        throw std::runtime_error("Failed to find frame buffer");
    }

    bool doesFrameBufferNeedPingPong(uint frameBufferID, const std::vector<RenderPass>& renderPasses) {
        for (size_t i = 0; i < renderPasses.size(); i++) {
            RenderPass renderPass = renderPasses[i];
            uint frameBufferOutputID = renderPass.frameBufferOutputID;
            for (size_t j = 0; j < renderPass.frameBufferInputIDs.size(); j++) {
                if (renderPass.frameBufferInputIDs[i] == frameBufferOutputID) {
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
                        texturesNew[k].pingPongFlag = true;
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
            std::cout << "Uniform:: texID: " << texture["texID"]
                    << ", name: " << texture["name"]
                    << ", format: " << texture["format"] << ", resolution: ("
                    << texture["resX"] << ", " << texture["resY"]
                    << ") , resizable: " << texture["resizable"]
                    << ", origin: " << texture["origin"] << std::endl;
            Texture textureResource;
            textureResource.textureID = texture["texID"];
            if(texture["texID"] <= 0) {std::cout << "ERROR: Invalid textureID! : " << texture["texID"] << std::endl;}
            textureResource.name = texture["name"];
            textureResource.format = getFormat(texture["format"]);
            textureResource.resolutionX = texture["resX"];
            textureResource.resolutionY = texture["resY"];
            textureResource.resizable = texture["resizable"];
            textureResource.origin = getOrigin(texture["origin"]);
            textures.emplace_back(textureResource);
        }
        return textures;
    }

    std::vector<FrameBuffer> loadFrameBuffers(nlohmann::json& resourceData) {
        std::vector<FrameBuffer> frameBuffers;
        for (const auto &frameBuffer : resourceData["framebuffers"]) {
            std::cout << "FrameBuffer:: fboID: " << frameBuffer["fboID"] << ", textureIDs: " << frameBuffer["textureIDs"] << std::endl;
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
        for (const auto &renderPass : dependencyGraphData["renderer"]) {
            std::cout << "RenderPass:: shaderID: " << renderPass["shaderID"]
                    << ", frameBufferInputIDs: " << renderPass["frameBufferInputIDs"]
                    << ", resourceTexturesIDs: " << renderPass["resourceTextures"]
                    << ", frameBufferOutputID: " << renderPass["frameBufferOutputID"]
                    << std::endl;
            RenderPass renderPassDescription;
            renderPassDescription.shaderID = renderPass["shaderID"];

            for (auto &frameBufferInputID : renderPass["frameBufferInputIDs"]) {
                renderPassDescription.frameBufferInputIDs.emplace_back(frameBufferInputID);
            }

            for (auto &resourceTextureID : renderPass["resourceTextures"]) {
                renderPassDescription.textureResourceIDs.emplace_back(resourceTextureID);
            }

            renderPassDescription.frameBufferOutputID = renderPass["frameBufferOutputID"];

            renderPasses.emplace_back(renderPassDescription);
        }
        return renderPasses;
    }

    RendererSpecification loadRendererSpecification(std::string rendererPath) {
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

        return renderer;
    }
}
