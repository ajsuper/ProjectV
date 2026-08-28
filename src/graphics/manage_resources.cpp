#include "graphics/manage_resources.h"
#include "data_structures/texture.h"
#include <bgfx/bgfx.h>

namespace projv::graphics {
    std::vector<bgfx::Attachment> getTextureAttachments(const std::unordered_map<uint32_t, bgfx::TextureHandle>& textureHandles, std::vector<uint32_t> textureIDs) {
        std::vector<bgfx::TextureHandle> textures;
        for (size_t i = 0; i < textureIDs.size(); i++) {
            textures.emplace_back(textureHandles.at(textureIDs[i]));
        }

        std::vector<bgfx::Attachment> attachments;
        for (auto texture : textures) {
            bgfx::Attachment attachment;
            attachment.init(texture);
            attachments.emplace_back(attachment);
        }

        return attachments;
    }

    bgfx::ProgramHandle createShaderProgram(const bgfx::ShaderHandle& vertexShader, const bgfx::ShaderHandle& fragmentShaderHandle) {
        bgfx::ProgramHandle shaderProgram = bgfx::createProgram(vertexShader, fragmentShaderHandle, true);
        return shaderProgram;
    }

    ConstructedTextures constructTextures(const std::vector<Texture>& textures) {
        ConstructedTextures constructedTextures;
        for (size_t i = 0; i < textures.size(); i++) {
            Texture texture = textures[i];

            // Presence in relativeTextureScales is the sizing rule -- see ConstructedTextures.
            if (texture.sizeMode == TextureSizeMode::Relative) {
                constructedTextures.relativeTextureScales[texture.textureID] = texture.scale;
            }

            // A relative texture is created 1x1 and takes its real size from the driver's first
            // resizeRenderTargets, which every driver calls before it renders. Deliberately not
            // seeded from a declared resolution: the render resolution is the single source of truth
            // for these, and a plausible-looking placeholder is how a target ends up quietly stuck at
            // a size nobody asked for.
            uint16_t textureWidth = 1;
            uint16_t textureHeight = 1;
            if (texture.sizeMode == TextureSizeMode::Fixed) {
                textureWidth = uint16_t(std::max(1, texture.resolutionX));
                textureHeight = uint16_t(std::max(1, texture.resolutionY));
            }
            // ---- CLAMP AT THE EDGES, ALWAYS ---------------------------------------------------
            // bgfx's default addressing is REPEAT, and for a render target that is never what anyone
            // means. A render target holds a picture of the screen, so a filter tap that runs off one
            // side does not find "more image" over there -- it finds the OPPOSITE side, and whatever
            // was there gets blended in as if it were adjacent. Every screen-space filter in the tree
            // has this bug by default: blurs, bloom, god rays, temporal reprojection, the lot. It
            // shows up as a bright feature at the top of the frame reappearing along the bottom,
            // which is exactly how it was found.
            //
            // Clamping is the right default rather than a workaround. It makes an off-screen tap read
            // the nearest edge texel, which is the standard assumption every one of these filters is
            // written against -- and it removes a whole class of bug that is invisible until some
            // effect happens to be bright near an edge.
            //
            // Set at CREATION rather than per-bind: bgfx::setTexture's 4-argument form passes
            // UINT32_MAX for its flags, which means "use the texture's own", so this reaches every
            // pass without touching a single call site. It is also stored in
            // ConstructedTextures::textureFlags below, so a resize rebuilds the target the same way.
            //
            // Opt-in (Texture::readBack): lets this texture be blitted into and read back to the CPU
            // via bgfx::readTexture. Off by default so every other texture is unaffected.
            uint64_t textureFlags = BGFX_TEXTURE_RT
                                  | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                                  | (texture.readBack ? (BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST) : 0);
            // Remembered so a resize can rebuild this texture as the same kind of texture. See
            // ConstructedTextures::textureFlags.
            constructedTextures.textureFlags[texture.textureID] = textureFlags;
            constructedTextures.textureHandles[texture.textureID] = bgfx::createTexture2D(textureWidth, textureHeight, false, 1, texture.format, textureFlags);
            constructedTextures.textureSamplerHandles[texture.textureID] = bgfx::createUniform(texture.name.c_str(), bgfx::UniformType::Sampler);
            constructedTextures.textureHandlesAlternate[texture.textureID] = BGFX_INVALID_HANDLE;
            constructedTextures.textureSamplerHandlesAlternate[texture.textureID] = BGFX_INVALID_HANDLE;
            constructedTextures.pingPongFlags[texture.textureID] = false;
            if (texture.pingPongFlag == true) {
                constructedTextures.textureHandlesAlternate[texture.textureID] = bgfx::createTexture2D(textureWidth, textureHeight, false, 1, texture.format, textureFlags);
                constructedTextures.textureSamplerHandlesAlternate[texture.textureID] = bgfx::createUniform(texture.name.c_str(), bgfx::UniformType::Sampler);
                constructedTextures.pingPongFlags[texture.textureID] = true;
            }
            constructedTextures.textureFormats[texture.textureID] = texture.format;
            // The size the handle actually has, not the size that was declared. These two used to be
            // allowed to drift; resolveTargetSize reads this to decide a pass's view rect, so it has to
            // be what the texture is, always.
            constructedTextures.textureResolutions[texture.textureID] = projv::core::ivec2(int(textureWidth), int(textureHeight));
        }

        return constructedTextures;
    }
    
    ConstructedFramebuffers constructFramebuffers(const std::vector<FrameBuffer>& frameBuffers, const ConstructedTextures& constructedTextures) {
        ConstructedFramebuffers constructedFramebuffers;
        for (size_t i = 0; i < frameBuffers.size(); i++) {
            FrameBuffer frameBuffer = frameBuffers[i];
            std::vector<bgfx::Attachment> attachments = getTextureAttachments(constructedTextures.textureHandles, frameBuffer.TextureIDs);
            constructedFramebuffers.frameBufferHandles[frameBuffer.frameBufferID] = bgfx::createFrameBuffer(uint16_t(frameBuffer.TextureIDs.size()), attachments.data(), false); //Bindings in GLSL are determined by the texture order.
            constructedFramebuffers.frameBufferTextureMapping[frameBuffer.frameBufferID] = frameBuffer.TextureIDs;
            constructedFramebuffers.pingPongFBOs[frameBuffer.frameBufferID] = false;
            constructedFramebuffers.frameBufferTextureMapping[frameBuffer.frameBufferID] = frameBuffer.TextureIDs;
            core::info("Created frame buffer");
            constructedFramebuffers.frameBufferHandlesAlternate[frameBuffer.frameBufferID] = BGFX_INVALID_HANDLE;
            constructedFramebuffers.primaryWasLastRenderedToo[frameBuffer.frameBufferID] = true;
            if (frameBuffer.pingPongFBO == true) {
                std::vector<bgfx::Attachment> attachmentsAlternate = getTextureAttachments(constructedTextures.textureHandlesAlternate, frameBuffer.TextureIDs);
                core::info("Got alternate texture attachments");
                constructedFramebuffers.frameBufferHandlesAlternate[frameBuffer.frameBufferID] = bgfx::createFrameBuffer(uint16_t(frameBuffer.TextureIDs.size()), attachmentsAlternate.data(), false); //Bindings in GLSL are determined by the texture order.
                constructedFramebuffers.frameBufferTextureMappingAlternate[frameBuffer.frameBufferID] = frameBuffer.TextureIDs;               
                constructedFramebuffers.pingPongFBOs[frameBuffer.frameBufferID] = true;
                core::info("Created alternate frame buffer");
            }
        }
        constructedFramebuffers.frameBufferHandles[-1] = BGFX_INVALID_HANDLE;
        constructedFramebuffers.pingPongFBOs[-1] = false;
        return constructedFramebuffers;
    }

    ConstructedTextures assignTexturesToTheirFramebuffers(const ConstructedFramebuffers& frameBuffers, const ConstructedTextures& textures) {
        ConstructedTextures texturesNew = textures;
        for (auto& frameBuffer : frameBuffers.frameBufferTextureMapping) {
            for (size_t i = 0; i < frameBuffer.second.size(); i++) {
                uint32_t textureID = frameBuffer.second[i];
                texturesNew.textureIDToFrameBufferID[textureID] = frameBuffer.first;
            }
        }
        return texturesNew;
    }

    std::unordered_map<std::string, bgfx::UniformHandle> constructUniforms(const std::vector<Uniform>& uniforms) {
        std::unordered_map<std::string, bgfx::UniformHandle> constructedUniformHandles;
        for (size_t i = 0; i < uniforms.size(); i++) {
            Uniform uniform = uniforms[i];
            constructedUniformHandles[uniform.name] = bgfx::createUniform(uniform.name.c_str(), mapUniformType(uniform.type));
        }
        return constructedUniformHandles;
    }

    std::unordered_map<uint32_t, bgfx::ShaderHandle> constructShaders(const std::vector<Shader>& shaders) {
        std::unordered_map<uint32_t, bgfx::ShaderHandle> constructedShaderHandles;
        for (size_t i = 0; i < shaders.size(); i++) {
            Shader shader = shaders[i];
            constructedShaderHandles[shader.shaderID] = loadShader(shader.filePath);
        }
        return constructedShaderHandles;
    }

    std::vector<BGFXDependencyGraph> constructRenderPasses(const BGFXResources& constructedResources, const std::vector<FrameBuffer>& frameBuffers, const std::vector<RenderPass>& renderPasses) {
        std::vector<BGFXDependencyGraph> dependencyGraphs;
        for (size_t i = 0; i < renderPasses.size(); i++) {
            const RenderPass &renderPass = renderPasses[i];
            if (constructedResources.framebuffers
                    .frameBufferHandles.at(renderPass.frameBufferOutputID)
                    .idx != bgfx::kInvalidHandle) {
            }

            BGFXDependencyGraph dependencyGraph;
            dependencyGraph.depdendencies = getDependenciesList(frameBuffers, constructedResources.textures.textureSamplerHandles, renderPass); // Should be removed. We should only store the ID's.
            dependencyGraph.targetFrameBufferID = renderPass.frameBufferOutputID;
            dependencyGraph.shaderProgram = createShaderProgram(constructedResources.defaultVertexShader, constructedResources.shaderHandles.at(renderPass.shaderID));
            dependencyGraph.renderPassID = uint32_t(i);
            dependencyGraph.multiPassPassNumber = renderPass.multiPassPassNumber;
            dependencyGraph.multiPassPassNumberUniform = bgfx::createUniform(("multiPassPassNumber" + std::to_string(i)).c_str(), bgfx::UniformType::Vec4);

            dependencyGraphs.push_back(dependencyGraph);
        }
        return dependencyGraphs;
    }

    std::shared_ptr<ConstructedRenderer> constructRendererSpecification(RendererSpecification &renderer, bgfx::ShaderHandle vertexShader) {
        ConstructedRenderer constructedRenderer;
        constructedRenderer.resources.defaultVertexShader = vertexShader;
        // Engine-owned, so it exists for every renderer whether or not its resources.json mentions it.
        // See BGFXResources::passTargetRes.
        constructedRenderer.resources.passTargetRes = bgfx::createUniform("passTargetRes", bgfx::UniformType::Vec4);
        constructedRenderer.resources.passInputRes = bgfx::createUniform("passInputRes", bgfx::UniformType::Vec4, PROJV_MAX_PASS_INPUTS);
        constructedRenderer.resources.pjvMotionSets = bgfx::createUniform("pjvMotionSets", bgfx::UniformType::Vec4, PROJV_MOTION_SET_VEC4S);
        constructedRenderer.resources.pjvAnimTime = bgfx::createUniform("pjvAnimTime", bgfx::UniformType::Vec4);
        const Resources &resources = renderer.resources;
        const std::vector<RenderPass> &renderPasses = renderer.dependencyGraph.renderPasses;

        constructedRenderer.resources.textures = constructTextures(resources.textures);
        core::info("Constructed textures!");
        constructedRenderer.resources.framebuffers = constructFramebuffers(resources.FrameBuffers, constructedRenderer.resources.textures);
        core::info("Constructed frame buffers!");
        constructedRenderer.resources.uniformHandles = constructUniforms(resources.uniforms);
        core::info("Constructed uniform handles!");
        constructedRenderer.resources.shaderHandles = constructShaders(resources.shaders);
        core::info("Constructed shaders!");
        constructedRenderer.dependencyGraph = constructRenderPasses(constructedRenderer.resources, resources.FrameBuffers, renderPasses);
        core::info("Constructed render passes!");
        constructedRenderer.resources.textures = assignTexturesToTheirFramebuffers(constructedRenderer.resources.framebuffers, constructedRenderer.resources.textures);

        return std::make_shared<ConstructedRenderer>(constructedRenderer);
    }

    // The size a pass writing to this framebuffer has to rasterize at. See the header for why this is
    // authoritative rather than advisory.
    //
    // A framebuffer has no size of its own -- FrameBuffer is a list of texture IDs -- so its size is
    // its attachments'. Reading the first attachment is sufficient because every attachment of a
    // framebuffer is required to be the same size (an API requirement, enforced at load).
    core::ivec2 resolveTargetSize(const ConstructedTextures& textures, const ConstructedFramebuffers& frameBuffers, int frameBufferID, core::ivec2 backBufferSize) {
        if (frameBufferID < 0) {
            return backBufferSize;   // The default framebuffer: whatever the driver last reset to.
        }
        auto mapping = frameBuffers.frameBufferTextureMapping.find(frameBufferID);
        if (mapping == frameBuffers.frameBufferTextureMapping.end() || mapping->second.empty()) {
            return backBufferSize;
        }
        auto resolution = textures.textureResolutions.find(mapping->second.front());
        if (resolution == textures.textureResolutions.end()) {
            return backBufferSize;
        }
        return resolution->second;
    }

    // Brings every window-relative render target to `renderWidth` x `renderHeight`, and rebuilds the
    // framebuffers that point at the ones which actually changed. Returns whether anything was
    // rebuilt, which is what a caller averaging frames needs: an accumulation or a reprojection
    // history cannot be carried across a resolution change, so the driver folds this into the same
    // condition a camera move goes through.
    //
    // Deliberately does NOT touch the back buffer. bgfx::reset used to live here, which is correct
    // only when the render target *is* the window and wrong for any renderer drawing into a panel --
    // it shrinks the back buffer, and the interface with it. That coupling is the whole reason the
    // scene editor grew a private copy of this function; the back buffer belongs to the driver.
    //
    // Ownership is uniform: textures belong to ConstructedTextures and framebuffers to
    // ConstructedFramebuffers. Every createFrameBuffer here passes destroyTexture=false to match
    // constructFramebuffers -- passing true made the framebuffer co-own attachments this function
    // destroys itself.
    bool resizeRenderTargets(ConstructedTextures& textures, ConstructedFramebuffers& frameBuffers, int renderWidth, int renderHeight) {
        renderWidth = std::max(1, renderWidth);
        renderHeight = std::max(1, renderHeight);

        // Which textures actually moved. Only the framebuffers holding one of these need rebuilding;
        // the old code rebuilt every framebuffer in the renderer on any window change, including
        // fixed-size ones whose attachments had not been touched.
        std::vector<uint32_t> resizedTextureIDs;
        for (const auto& relativeTexture : textures.relativeTextureScales) {
            uint32_t textureID = relativeTexture.first;
            float scale = relativeTexture.second;

            // Rounded UP, in one place, so a half-resolution target of an odd-width image covers it
            // rather than falling a column short. Consumers must not re-derive a size by scaling --
            // they read the real one from passTargetRes, because ceil(w*s) is not w*s.
            int scaledWidth  = std::max(1, int(std::ceil(float(renderWidth)  * scale)));
            int scaledHeight = std::max(1, int(std::ceil(float(renderHeight) * scale)));

            core::ivec2 current = textures.textureResolutions.at(textureID);
            if (current.x == scaledWidth && current.y == scaledHeight) continue;

            bgfx::TextureFormat::Enum format = textures.textureFormats.at(textureID);
            // The flags this texture was created with, not a fresh BGFX_TEXTURE_RT -- see
            // ConstructedTextures::textureFlags for what recreating with the wrong ones silently costs.
            uint64_t flags = textures.textureFlags.at(textureID);

            if (bgfx::isValid(textures.textureHandles.at(textureID))) {
                bgfx::destroy(textures.textureHandles.at(textureID));
            }
            textures.textureHandles.at(textureID) =
                bgfx::createTexture2D(uint16_t(scaledWidth), uint16_t(scaledHeight), false, 1, format, flags);

            // A ping-pong texture exists twice and both copies have to follow, or the pass reads one
            // resolution and writes another on alternating frames.
            if (textures.pingPongFlags.at(textureID)) {
                if (bgfx::isValid(textures.textureHandlesAlternate.at(textureID))) {
                    bgfx::destroy(textures.textureHandlesAlternate.at(textureID));
                }
                textures.textureHandlesAlternate.at(textureID) =
                    bgfx::createTexture2D(uint16_t(scaledWidth), uint16_t(scaledHeight), false, 1, format, flags);
            }

            // Kept in step with the handles. This map is what resolveTargetSize reads to decide a
            // pass's view rect, so leaving it at the declared size (as this function used to) makes
            // every pass rasterize at a resolution its target no longer has.
            textures.textureResolutions.at(textureID) = core::ivec2(scaledWidth, scaledHeight);
            resizedTextureIDs.push_back(textureID);
        }

        if (resizedTextureIDs.empty()) {
            return false;
        }

        for (auto& frameBuffer : frameBuffers.frameBufferTextureMapping) {
            int frameBufferID = frameBuffer.first;
            const std::vector<uint32_t>& textureIDs = frameBuffer.second;

            bool holdsAResizedTexture = false;
            for (uint32_t textureID : textureIDs) {
                for (uint32_t resizedID : resizedTextureIDs) {
                    if (textureID == resizedID) { holdsAResizedTexture = true; break; }
                }
                if (holdsAResizedTexture) break;
            }
            if (!holdsAResizedTexture) continue;

            // Destroyed before being overwritten. Assigning over a live handle leaks the framebuffer
            // object, and a renderer resized on every frame of a splitter drag exhausts bgfx's
            // framebuffer pool in seconds.
            if (bgfx::isValid(frameBuffers.frameBufferHandles.at(frameBufferID))) {
                bgfx::destroy(frameBuffers.frameBufferHandles.at(frameBufferID));
            }
            std::vector<bgfx::Attachment> attachments = getTextureAttachments(textures.textureHandles, textureIDs);
            frameBuffers.frameBufferHandles.at(frameBufferID) =
                bgfx::createFrameBuffer(uint16_t(textureIDs.size()), attachments.data(), false); // Bindings in GLSL are determined by the textureID order.

            if (!frameBuffers.pingPongFBOs.at(frameBufferID)) continue;
            if (bgfx::isValid(frameBuffers.frameBufferHandlesAlternate.at(frameBufferID))) {
                bgfx::destroy(frameBuffers.frameBufferHandlesAlternate.at(frameBufferID));
            }
            std::vector<bgfx::Attachment> alternateAttachments = getTextureAttachments(textures.textureHandlesAlternate, textureIDs);
            frameBuffers.frameBufferHandlesAlternate.at(frameBufferID) =
                bgfx::createFrameBuffer(uint16_t(textureIDs.size()), alternateAttachments.data(), false);
        }

        return true;
    }
}
