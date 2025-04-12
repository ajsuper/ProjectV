#include "core.h"
#include "camera.h"
#include "data_structures/renderInstance.h"

namespace projv{
    namespace ECS{
        World createWorld() {
            World world;
            return world;
        }

        // Creats an application with default startup, update, and render functions.
        Application createApp() {
            Application app;
            app.Startup = []() {
                std::cout << "Default Startup!" << std::endl;
            };
            app.Update = []() {
                std::cout << "Default Update!" << std::endl;
            };
            app.Render = []() {
                std::cout << "Default Render!" << std::endl;
            };
            app.world = createWorld();
            return app;
        }
    
        // Adds an entity to our world.
        Entity createEntity(World& world) {
            Entity id = world.nextEntityID++;
            return id;
        }
        
        // Adds a system to our application for a specific stage.
        void addSystem(ECS::Application& app, SystemStage stage, std::function<void(ECS::Application&)> system) {
            switch (stage) {
                case SystemStage::Startup:
                    app.Startup = [=, &app]() { system(app); };
                    break;
                case SystemStage::Update:
                    app.Update = [=, &app]() { system(app); };
                    break;
                case SystemStage::Render:
                    app.Render = [=, &app]() { system(app); };
                    break;
            }
        }
    
        // Runs the specified application and starts the loop.
        void runApplication(Application& app) {
            app.Startup();
            while(!app.closeAppFlag) {
                app.Update();
                app.Render();
                app.frameCount++;
            }
            return;
        }
    }
    /**
     * Move/Rotate the camera from WASD QE SPACE/SHIFT (R and F to change movement speeds).
     * 
     * @param window The GLFW window to get user input from.
     */
    void moveCameraFromUserInput(GLFWwindow* window) {
        float scaledDownCameraMovementSpeed = cam.movementSpeed / 100.0f;
        // Up vector remains constant.
        float up[3] = {0.0f, 1.0f, 0.0f};

        // Handle Rotations First.
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) {
            float angle = cam.rotationSpeed;
            float oldDirX = cam.direction[0];
            float oldDirZ = cam.direction[2];

            // Rotate around the Y-axis.
            cam.direction[0] = oldDirX * cos(angle) - oldDirZ * sin(angle);
            cam.direction[2] = oldDirX * sin(angle) + oldDirZ * cos(angle);
            // Keep Y-component zero to prevent unintended vertical movement.
            cam.direction[1] = 0.0f;

            // Normalize cam.direction after rotation.
            float dirNorm = sqrt(cam.direction[0] * cam.direction[0] + cam.direction[2] * cam.direction[2]);
            if (dirNorm != 0.0f) {
                cam.direction[0] /= dirNorm;
                cam.direction[2] /= dirNorm;
            }
        }
        
        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
            float angle = -cam.rotationSpeed;
            float oldDirX = cam.direction[0];
            float oldDirZ = cam.direction[2];

            // Rotate around the Y-axis.
            cam.direction[0] = oldDirX * cos(angle) - oldDirZ * sin(angle);
            cam.direction[2] = oldDirX * sin(angle) + oldDirZ * cos(angle);
            cam.direction[1] = 0.0f;

            // Normalize cam.direction after rotation.
            float dirNorm = sqrt(cam.direction[0] * cam.direction[0] + cam.direction[2] * cam.direction[2]);
            if (dirNorm != 0.0f) {
                cam.direction[0] /= dirNorm;
                cam.direction[2] /= dirNorm;
            }
        }

        // Recalculate Right Vector After Rotation.
        float right[3];
        // Cross product: right = up x cam.direction.
        right[0] = up[1] * cam.direction[2] - up[2] * cam.direction[1];
        right[1] = up[2] * cam.direction[0] - up[0] * cam.direction[2];
        right[2] = up[0] * cam.direction[1] - up[1] * cam.direction[0];
        // Normalize right vector.
        float norm = sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
        if (norm != 0.0f) {
            right[0] /= norm;
            right[1] /= norm;
            right[2] /= norm;
        }

        // Movement Logic.

        if(glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS){
            cam.position[0] += cam.direction[0] * scaledDownCameraMovementSpeed;
            cam.position[1] += cam.direction[1] * scaledDownCameraMovementSpeed;
            cam.position[2] += cam.direction[2] * scaledDownCameraMovementSpeed;
        }
        if(glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS){
            cam.position[0] -= right[0] * scaledDownCameraMovementSpeed;
            cam.position[1] -= right[1] * scaledDownCameraMovementSpeed;
            cam.position[2] -= right[2] * scaledDownCameraMovementSpeed;
        }
        if(glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS){
            cam.position[0] -= cam.direction[0] * scaledDownCameraMovementSpeed;
            cam.position[1] -= cam.direction[1] * scaledDownCameraMovementSpeed;
            cam.position[2] -= cam.direction[2] * scaledDownCameraMovementSpeed;
        }
        if(glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS){
            cam.position[0] += right[0] * scaledDownCameraMovementSpeed;
            cam.position[1] += right[1] * scaledDownCameraMovementSpeed;
            cam.position[2] += right[2] * scaledDownCameraMovementSpeed;
        }

        // Vertical movement with SPACE and LEFT SHIFT
        if(glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS){
            cam.position[1] -= scaledDownCameraMovementSpeed;
        }
        if(glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS){
            cam.position[1] += scaledDownCameraMovementSpeed;
        }

        // Adjust movement speed
        if(glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS){
            cam.movementSpeed += 0.1f;
        }
        if(glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS){
            cam.movementSpeed -= 0.1f;
            if(cam.movementSpeed <= 0.001f){
                cam.movementSpeed = 0.001f;
            }
        }

        return;
    }

    /**
     * @brief Initializes GLFW and GLEW, and creates a window.
     * 
     * @param window A reference to the GLFWwindow pointer that will be initialized.
     * @param windowWidth The width of the window to be created.
     * @param windowHeight The height of the window to be created.
     * @param windowName The title of the window to be created.
     * @return true if initialization and window creation were successful, false otherwise.
     */
    bool initializeGLFWandGLEWWindow(GLFWwindow*& window, int windowWidth, int windowHeight, const std::string& windowName){ //CORE
        if (!glfwInit()) {
            std::cerr << "[CORE] Function: initializeGLFWandGLEWWindow, Failed to initialize GLFW!" << std::endl;
            return false;
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        window = glfwCreateWindow(windowWidth, windowHeight, windowName.c_str(), NULL, NULL);
        if (!window) {
            std::cerr << "[CORE] Function: initializeGLFWandGLEWWindow, Failed to create GLFW window!" << std::endl;
            glfwTerminate();
            return false;
        }
        glfwMakeContextCurrent(window);

        glewExperimental = GL_TRUE;  // Enable modern OpenGL features
        if (glewInit() != GLEW_OK) {
            std::cerr << "[CORE] Function: initializeGLFWandGLEWWindow, Failed to initialize GLEW!" << std::endl;
            return false;
        } 
        return true;
    }

    void createRenderQuad(RenderInstance& renderInstance)
    {
        // Define vertices for a window filling quad
        float vertices[] = {
            // positions   // texCoords
            -1.0f, -1.0f, 0.0f, 0.0f,
            1.0f, -1.0f, 1.0f, 0.0f,
            -1.0f, 1.0f, 0.0f, 1.0f,
            -1.0f, 1.0f, 0.0f, 1.0f,
            1.0f, -1.0f, 1.0f, 0.0f,
            1.0f, 1.0f, 1.0f, 1.0f};

        glGenVertexArrays(1, &renderInstance.VAO);
        glGenBuffers(1, &renderInstance.VBO);

        // Bind and set buffer data
        glBindVertexArray(renderInstance.VAO);

        glBindBuffer(GL_ARRAY_BUFFER, renderInstance.VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        // Position attribute (location = 0)
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(0);

        // Texture Coord attribute (location = 1)
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glBindVertexArray(0);

        return;
    }

    RenderInstance initializeRenderInstance(int windowWidth, int windowHeight, const std::string& windowName){ //CORE
        RenderInstance renderInstance;
        if (!glfwInit()) {
            std::cerr << "[initializeRenderInstance] Failed to initialize GLFW!" << std::endl;
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        renderInstance.window = glfwCreateWindow(windowWidth, windowHeight, windowName.c_str(), NULL, NULL);
        if (!renderInstance.window) {
            std::cerr << "[initializeRenderInstance] Failed to create GLFW window!" << std::endl;
            glfwTerminate();
        }
        glfwMakeContextCurrent(renderInstance.window);

        glewExperimental = GL_TRUE;  // Enable modern OpenGL features
        if (glewInit() != GLEW_OK) {
            std::cerr << "[initializeRenderInstance] Failed to initialize GLEW!" << std::endl;
        } 

        createRenderQuad(renderInstance);

        return renderInstance;
    }

    void setErrorCallback(RenderInstance renderInstance, void (*error_callback)(int error_code, const char* description)){
        glfwSetErrorCallback(error_callback);
    }

    void setFrameBufferSizeCallback(RenderInstance renderInstance, void (*framebuffer_callback)(GLFWwindow* window, int width, int height)){
        void framebuffer_size_callback(GLFWwindow *window, int width, int height);
    }
}
