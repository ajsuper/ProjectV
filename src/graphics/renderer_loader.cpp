#include "graphics/renderer_loader.h"

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

static std::vector<char> readFile(const std::string &filename) {
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
                << ", frameBufferOutputIDs: " << renderPass["frameBufferOutputID"]
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

    return renderer;
}
