#include "graphics/shader.h"

namespace projv::graphics {
    std::string loadShaderSource(const char *filepath) {
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


    GLuint compileVertexAndFragmentShaders(const char *vertexPath, const char *fragmentPath) {
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

    void addShaderToRenderInstance(RenderInstance& renderInstance, GLuint shader, std::string shaderName) {
        renderInstance.shaders[shaderName] = shader;
    }

    void removeShaderFromRenderInstance(RenderInstance& renderInstance, std::string shaderName) {
        renderInstance.shaders.erase(shaderName);
    }

    GLuint getShaderFromRenderInstance(RenderInstance& renderInstance, std::string shaderName) {
        auto it = renderInstance.shaders.find(shaderName);
        if(it != renderInstance.shaders.end()){
            return it->second;
        } else {
            std::cerr << "[getShaderFromRenderInstance] Shader not found: " << shaderName << std::endl;
            return 0;
        }
    }
}