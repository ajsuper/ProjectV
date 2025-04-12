// Last edited on: 31-12-2024.

#include "graphics.h"

namespace projv
{
    int frameCount = 0;
    // Global variables
    
    void addTextureToFrameBuffer(FrameBuffer &frameBuffer, GLuint internalFormat, std::string textureName)
    {
        int width = frameBuffer.width; // Get framebuffer dimensions
        int height = frameBuffer.height;

        Texture customTexture;

        // Generate and bind the texture
        glGenTextures(1, &customTexture.textureID);
        glBindTexture(GL_TEXTURE_2D, customTexture.textureID);

        // Determine the appropriate base format and type
        GLenum baseFormat;
        GLenum type;

        switch (internalFormat)
        {
        case GL_R8:
        case GL_R16:
            baseFormat = GL_RED;
            type = GL_UNSIGNED_BYTE;
            break;

        case GL_R16F:
        case GL_R32F:
            baseFormat = GL_RED;
            type = GL_FLOAT;
            break;

        case GL_RGB8:
        case GL_RGB16:
            baseFormat = GL_RGB;
            type = GL_UNSIGNED_BYTE;
            break;

        case GL_RGB16F:
        case GL_RGB32F:
            baseFormat = GL_RGB;
            type = GL_FLOAT;
            break;

        case GL_RGBA8:
        case GL_RGBA16:
            baseFormat = GL_RGBA;
            type = GL_UNSIGNED_BYTE;
            break;

        case GL_RGBA16F:
        case GL_RGBA32F:
            baseFormat = GL_RGBA;
            type = GL_FLOAT;
            break;

        default:
            std::cerr << "Unsupported format: " << internalFormat << ". Supported formats are: GL_R8, GL_R16F, GL_R32F, GL_RGB8, GL_RGB16, GL_RGB16F, GL_RGB32F, GL_RGBA8, GL_RGBA16, GL_RGBA16F, GL_RGBA32F." << std::endl;
            return; // Exit the function on unsupported format
        }

        // Specify the texture image
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, baseFormat, type, nullptr);

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Assign properties to the customTexture object and add it to the buffers texture list.
        customTexture.name = textureName;
        customTexture.format = internalFormat;
        frameBuffer.textures.push_back(customTexture);

        // Get the number of textures in our frame buffer object
        int numberOfTextures = frameBuffer.textures.size();

        // Create our color attatchments aray and bind our buffer.
        GLenum drawBuffers[numberOfTextures];
        glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer.buffer);

        // Re-bind all of the textures.
        for (int i = 0; i < numberOfTextures; i++)
        {
            Texture texture = frameBuffer.textures[i];                                                             // Fetch the texture
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, texture.textureID, 0); // Set this textureID to a color attatchment.
            drawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;

            std::cout << "[INFO | addTextureToFrameBuffer] Texture " << texture.name << " added at location " << i << std::endl;
        }

        glDrawBuffers(numberOfTextures, drawBuffers);

        // Unbind the texture and framebuffer.
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        return;
    }

    FrameBuffer createFrameBufferObjectAdvanced(int width, int height)
    {
        FrameBuffer FBO;
        glGenFramebuffers(1, &FBO.buffer);
        FBO.width = width;
        FBO.height = height;
        return FBO;
    }

    void renderFragmentShaderToTargetBuffer(RenderInstance renderInstance, GLuint shaderProgram, FrameBuffer targetBuffer)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, targetBuffer.buffer);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(shaderProgram);

        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            std::cerr << "OpenGL error after shader program usage: " << error << std::endl;
        }

        glBindVertexArray(renderInstance.VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        return;
    }

    void renderFragmentShaderToTargetBufferWithOneInputBuffer(RenderInstance renderInstance, GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer targetBuffer)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, targetBuffer.buffer);

        // Clear the framebuffer
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Use the shader program
        glUseProgram(shaderProgram);

        // Bind the quad VAO
        glBindVertexArray(renderInstance.VAO);

        for (int i = 0; i < inputBuffer1.textures.size(); i++)
        {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, inputBuffer1.textures[i].textureID);
            std::string fullTextureName = "inputBuffer1_" + inputBuffer1.textures[i].name;
            glUniform1i(glGetUniformLocation(shaderProgram, fullTextureName.c_str()), i);
        }

        // Draw the full-screen quad
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Unbind VAO
        glBindVertexArray(0);

        return;
    }

    void renderFragmentShaderToTargetBufferWithTwoInputBuffers(RenderInstance renderInstance, GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer inputBuffer2, FrameBuffer targetBuffer)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, targetBuffer.buffer);

        // Clear the framebuffer
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Use the shader program
        glUseProgram(shaderProgram);

        // Bind the quad VAO
        glBindVertexArray(renderInstance. VAO);

        for (int i = 0; i < inputBuffer1.textures.size(); i++)
        {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, inputBuffer1.textures[i].textureID);
            std::string fullTextureName = "inputBuffer1_" + inputBuffer1.textures[i].name;
            glUniform1i(glGetUniformLocation(shaderProgram, fullTextureName.c_str()), i);
        }

        for (int i = 0; i < inputBuffer2.textures.size(); i++)
        {
            glActiveTexture(GL_TEXTURE0 + i + inputBuffer1.textures.size());
            glBindTexture(GL_TEXTURE_2D, inputBuffer2.textures[i].textureID);
            std::string fullTextureName = "inputBuffer2_" + inputBuffer2.textures[i].name;
            glUniform1i(glGetUniformLocation(shaderProgram, fullTextureName.c_str()), inputBuffer1.textures.size() + i);
        }

        // Draw the full-screen quad
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Unbind VAO
        glBindVertexArray(0);

        return;
    }

    void renderMultipassFragmentShaderToTargetBuffer(RenderInstance renderInstance, int numberOfPasses, GLuint multiPassShaderProgram, FrameBuffer frameBuffer1, FrameBuffer frameBuffer2, FrameBuffer targetBuffer)
    {
        // Ensure the frame buffers have the same texture attatchments.
        if (frameBuffer1.textures.size() != frameBuffer2.textures.size())
        {
            std::cerr << "[ERROR | renderMultipassFragmentShaderToTargetBuffer] Mismatch in number of texture attatchments between inputBuffer1 and inputBuffer2" << std::endl;
        }
        for (int i = 0; i < frameBuffer1.textures.size(); i++)
        {
            if (frameBuffer1.textures[i].name != frameBuffer2.textures[i].name)
            {
                std::cerr << "[ERROR | renderMultipassFragmentShaderToTargetBuffer] Mismatch between textures. frameBuffer1 contains " << frameBuffer1.textures[i].name << " while frameBuffer2 contains" << frameBuffer2.textures[i].name << std::endl;
            }
        }

        // Multi-Pass denoiser.
        for (int pass = 0; pass < numberOfPasses; ++pass)
        {

            // Decide which framebuffer to bind
            if (pass < numberOfPasses - 1)
            {
                // Alternate between FBO and FBO2 for intermediate passes
                if (pass % 2 == 0)
                {
                    glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer2.buffer);
                }
                else
                {
                    glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer1.buffer);
                }
            }
            else
            {
                // For the final pass, render to the default framebuffer
                glBindFramebuffer(GL_FRAMEBUFFER, targetBuffer.buffer);
            }

            // Clear the framebuffer
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Use the multiPassShaderProgram shader program
            glUseProgram(multiPassShaderProgram);

            GLint passLocation = glGetUniformLocation(multiPassShaderProgram, "PassNumber");
            glUniform1i(passLocation, pass);

            // Bind the quad VAO
            glBindVertexArray(renderInstance.VAO);

            // Bind textures
            FrameBuffer activeFrameBuffer = (pass % 2 == 0) ? frameBuffer1 : frameBuffer2;

            for (int i = 0; i < activeFrameBuffer.textures.size(); i++)
            {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, activeFrameBuffer.textures[i].textureID);
                std::string fullTextureName = "inputBuffer1_" + activeFrameBuffer.textures[i].name;
                glUniform1i(glGetUniformLocation(multiPassShaderProgram, fullTextureName.c_str()), i);
            }

            // Draw the full-screen quad
            glDrawArrays(GL_TRIANGLES, 0, 6);

            // Unbind VAO
            glBindVertexArray(0);
        }
    }

    void updateFloatInShader(GLuint shader, float variable, const char variableNameInShader[24])
    {
        // Ensure the correct shader program is bound
        if (glIsProgram(shader))
        {
            glUseProgram(shader);
        }
        else
        {
            std::cerr << "Invalid shader program" << std::endl;
            return;
        }

        // Get uniform location
        GLint floatLoc = glGetUniformLocation(shader, variableNameInShader);

        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            std::cerr << "OpenGL error after glUseProgram: " << error << std::endl;
        }

        // Check if the uniform location is valid
        if (floatLoc == -1)
        {
            std::cerr << "Failed to get uniform location for " << variableNameInShader << std::endl;
            return;
        }

        // Update the uniform
        glUniform1f(floatLoc, variable);

        // Check for OpenGL errors
        error = glGetError();
        if (error != GL_NO_ERROR)
        {
            std::cerr << "OpenGL error after glUniform1f: " << error << std::endl;
        }

        return;
    }

    void updateTimeInShader(GLuint shader)
    {
        // Ensure the correct shader program is bound
        if (glIsProgram(shader))
        {
            glUseProgram(shader);
        }
        else
        {
            std::cerr << "Invalid shader program" << std::endl;
            return;
        }

        // Get uniform location
        GLint timeLoc = glGetUniformLocation(shader, "time");

        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            std::cerr << "OpenGL error after glUseProgram: " << error << std::endl;
        }

        // Check if the uniform location is valid
        if (timeLoc == -1)
        {
            std::cerr << "Failed to get uniform location for time" << std::endl;
            return;
        }

        // Update the uniform
        glUniform1f(timeLoc, float(frameCount));

        // Check for OpenGL errors
        error = glGetError();
        if (error != GL_NO_ERROR)
        {
            std::cerr << "OpenGL error after glUniform1f: " << error << std::endl;
        }

        return;
    }

    void updateCameraInShader(GLuint shader)
    {
        // Ensure the correct shader program is bound
        if (glIsProgram(shader))
        {
            glUseProgram(shader);
        }
        else
        {
            std::cerr << "Invalid shader program" << std::endl;
            return;
        }

        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            std::cerr << "OpenGL error after glUseProgram: " << error << std::endl;
        }

        // Get uniform locations
        GLint cameraPosLoc = glGetUniformLocation(shader, "cameraPos");
        GLint cameraDirLoc = glGetUniformLocation(shader, "cameraDir");

        // Check for OpenGL errors
        error = glGetError();
        if (error != GL_NO_ERROR)
        {
            std::cerr << "OpenGL error after glGetUniformLocation: " << error << std::endl;
        }

        // Check if the uniform locations are valid
        if (cameraPosLoc == -1)
        {
            std::cerr << "Failed to get uniform location for cameraPos" << std::endl;
            return;
        }
        if (cameraDirLoc == -1)
        {
            std::cerr << "Failed to get uniform location for cameraDir" << std::endl;
            return;
        }

        // Update the uniforms
        glUniform3fv(cameraPosLoc, 1, cam.position);
        glUniform3fv(cameraDirLoc, 1, cam.direction);

        // Check for OpenGL errors
        error = glGetError();
        if (error != GL_NO_ERROR)
        {
            std::cerr << "OpenGL error after glUniform3fv: " << error << std::endl;
        }

        return;
    }

    void updateResolutionInShader(GLuint shader, GLFWwindow *window)
    {
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);

        // Pass the resolution uniform
        GLint resolutionLoc = glGetUniformLocation(shader, "resolution");
        float res[2] = {float(width), float(height)};
        glUniform2fv(resolutionLoc, 1, res);

        return;
    }

    std::string loadShaderSource(const char *filepath)
    {
        std::ifstream shaderFile;
        shaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        try
        {
            shaderFile.open(filepath);
            std::stringstream shaderStream;
            shaderStream << shaderFile.rdbuf();
            shaderFile.close();
            return shaderStream.str();
        }
        catch (std::ifstream::failure &e)
        {
            std::cout << "Shader Loading ERROR. Failed to load: " << filepath << std::endl;
            return "";
        }
        return "";
    }

    void passSceneToOpenGL(Scene& sceneToRender) {
        std::vector<GPUChunkHeader> chunkHeaders;
        std::vector<uint32_t> serializedGeometry;
        std::vector<uint32_t> serializedVoxelTypeData;
                    
        chunkHeaders.reserve(sceneToRender.chunks.size());
        size_t totalGeometrySize = 0;
        size_t totalVoxelTypeDataSize = 0;
    
        // Precompute total sizes to reserve memory
        for (const auto &chunk : sceneToRender.chunks) {
            totalGeometrySize += chunk.geometryData.size();
            totalVoxelTypeDataSize += chunk.voxelTypeData.size();
        }
        serializedGeometry.reserve(totalGeometrySize);
        serializedVoxelTypeData.reserve(totalVoxelTypeDataSize);
    
        // Gather data and compute chunk header indices
        for (const auto &chunk : sceneToRender.chunks) {
            if(chunk.geometryData.empty() || chunk.voxelTypeData.empty()) {
                std::cerr << "Chunk " << chunk.header.chunkID << " has empty geometry or voxel type data." << std::endl;
                continue; // Skip empty chunks
            }
            GPUChunkHeader shaderChunkHeader;
            shaderChunkHeader.position = chunk.header.position;
            shaderChunkHeader.scale = chunk.header.scale;
            shaderChunkHeader.resolution = chunk.header.resolution / pow(2, chunk.LOD);
            shaderChunkHeader.geometryStartIndex = serializedGeometry.size();
            shaderChunkHeader.voxelTypeDataStartIndex = serializedVoxelTypeData.size();
    
            serializedGeometry.insert(serializedGeometry.end(), chunk.geometryData.begin(), chunk.geometryData.end());
            serializedVoxelTypeData.insert(serializedVoxelTypeData.end(), chunk.voxelTypeData.begin(), chunk.voxelTypeData.end());
    
            shaderChunkHeader.geometryEndIndex = serializedGeometry.size();
            shaderChunkHeader.voxelTypeDataEndIndex = serializedVoxelTypeData.size();
    
            chunkHeaders.emplace_back(shaderChunkHeader);
        }
        
        // Use static variables for persistent mapped buffers and capacity tracking.
        static GLuint chunkHeaderSSBO = 0, geometrySSBO = 0, voxelTypeDataSSBO = 0;
        static void* chunkHeaderMappedPtr = nullptr;
        static void* geometryMappedPtr = nullptr;
        static void* voxelTypeDataMappedPtr = nullptr;
        static size_t chunkHeaderCapacity = 0;
        static size_t geometryCapacity = 0;
        static size_t voxelTypeDataCapacity = 0;
        
        // Define persistent mapping access flags.
        const GLbitfield storageFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        
        // Helper lambda to update or allocate a buffer with persistent mapping.
        auto updateBuffer = [&](GLuint &ssbo, void* &mappedPtr, size_t &capacity, size_t requiredSize,
                                  GLenum bindingPoint, const void* data) {
            if (ssbo == 0 || capacity < requiredSize) {
                std::cout << "Resizing SSBO" << std::endl;
                // If already created but not large enough, delete it.
                if (ssbo != 0) {
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
                    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);  // Unmap before deletion.
                    glDeleteBuffers(1, &ssbo);
                }
                glGenBuffers(1, &ssbo);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
                capacity = requiredSize * 1.5;
                // Allocate persistent storage once with no initial data.
                glBufferStorage(GL_SHADER_STORAGE_BUFFER, capacity, nullptr, storageFlags);
                // Map the entire buffer persistently.
                mappedPtr = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, capacity, storageFlags);
                if (!mappedPtr) {
                    std::cerr << "Failed to map buffer persistently!" << std::endl;
                    // Handle error appropriately.
                }
            } else {
                // Bind if no reallocation is needed.
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
            }
            // Update the buffer memory using memcpy.
            std::memcpy(mappedPtr, data, requiredSize);
            // Bind the buffer to the desired binding point.
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, bindingPoint, ssbo);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        };
        
        // Calculate sizes (in bytes) for each buffer.
        size_t chunkHeaderSize = chunkHeaders.size() * sizeof(GPUChunkHeader);
        size_t geometrySize = serializedGeometry.size() * sizeof(uint32_t);
        size_t voxelTypeDataSize = serializedVoxelTypeData.size() * sizeof(uint32_t);
        
        // Update each SSBO with the new data using persistent mapping.
        updateBuffer(chunkHeaderSSBO, chunkHeaderMappedPtr, chunkHeaderCapacity,
                     chunkHeaderSize, 3, chunkHeaders.data());
        updateBuffer(geometrySSBO, geometryMappedPtr, geometryCapacity,
                     geometrySize, 4, serializedGeometry.data());
        updateBuffer(voxelTypeDataSSBO, voxelTypeDataMappedPtr, voxelTypeDataCapacity,
                     voxelTypeDataSize, 5, serializedVoxelTypeData.data());
    }
    

    void passVoxelTypeDataToFrag(std::vector<uint32_t> voxelTypeData)
    {
        GLuint ssbo;
        glGenBuffers(1, &ssbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, voxelTypeData.size() * sizeof(uint32_t), voxelTypeData.data(), GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ssbo);

        return;
    }

    GLuint compileVertexAndFragmentShaders(const char *vertexPath, const char *fragmentPath)
    {
        GLuint shader = glCreateProgram();

        std::string vertCode = loadShaderSource(vertexPath);
        std::string fragCode = loadShaderSource(fragmentPath);

        const char *vertShaderSource = vertCode.c_str();
        const char *fragShaderSource = fragCode.c_str();

        GLint success;
        GLchar infoLog[512];

        // Compile vertex shader
        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &vertShaderSource, NULL);
        glCompileShader(vertexShader);
        // Check for compilation errors
        glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
            std::cerr << "Vertex Shader compilation failed:\n"
                      << infoLog << std::endl;
        }

        // Compile fragment shader
        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &fragShaderSource, NULL);
        glCompileShader(fragmentShader);
        // Check for compilation errors
        glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
            std::cerr << "Fragment Shader compilation failed:\n"
                      << infoLog << std::endl;
        }

        // Link shaders into a program
        glAttachShader(shader, vertexShader);
        glAttachShader(shader, fragmentShader);
        glLinkProgram(shader);
        // Check for linking errors
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success)
        {
            glGetProgramInfoLog(shader, 512, NULL, infoLog);
            std::cerr << "Shader Program linking failed:\n"
                      << infoLog << std::endl;
        }

        // Clean up shaders
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        return shader;
    }

}