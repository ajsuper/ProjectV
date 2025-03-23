// This demonstration is capable of loading the sibenik render test model as a scene and rendering it.

#include "utils.h" // For generating data types.
#include "graphics.h" // For handling shaders and passing data to the GPU.
#include "core.h" // For handling user input and window creation.
#include "camera.h"

// Define our frame buffer to output to
projv::FrameBuffer outputFrameBuffer;

void error_callback(int error, const char* description) {
    std::cerr << "GLFW Error (" << error << "): " << description << std::endl;
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    int width1, height1;
    glfwGetFramebufferSize(window, &width1, &height1);
    width1 = width1;
    height1 = height1;
    glViewport(0, 0, width1, height1);
}

void renderFrame(GLuint rayMarcher, GLFWwindow* window){
    GLuint query1;
    
    glGenQueries(1, &query1);

    // Start timer query for rayMarcher
    glBeginQuery(GL_TIME_ELAPSED, query1);        
    
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);

    // Perform rayMarcher rendering
    glViewport(0, 0, width, height);
    projv::renderFragmentShaderToTargetBuffer(rayMarcher, outputFrameBuffer);

    
    // End timer query for rayMarcher
    glEndQuery(GL_TIME_ELAPSED);

    // Get query result for rayMarcher
    GLuint elapsedTime1;
    glGetQueryObjectuiv(query1, GL_QUERY_RESULT, &elapsedTime1);

    std::cout << "RayMarcher GPU Time: " << elapsedTime1 / 1e6 << " ms" << std::endl;

    glDeleteQueries(1, &query1);     
    
    glfwSwapInterval(1);
    glfwSwapBuffers(window);

    return;
}

int main(){
    // Set the bufferID to 0, the default buffer ID for rendering to an OpenGL window.
    outputFrameBuffer.buffer = 0;

    // Initialize GLFW and create a
    GLFWwindow* window; // The window that our program will use.
    if(!projv::initializeGLFWandGLEWWindow(window, 1920, 1080, "window!!")){ // Initializes GLFW and creates a window. Prints if initialization fails.
        std::cerr << "Failed to initialize GLFW and GLEW" << std::endl;
        return -1;
    }

    // Set up callback's
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetErrorCallback(error_callback); // Allows for GLFW to print errors to the console.

    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    glViewport(0, 0, width, height);

    GLuint rayMarchShader = projv::compileVertexAndFragmentShaders("./shaders/vertexShader.glsl", "./shaders/fragmentShader.frag"); // Compiles the shaders so they can be run.

    projv::createRenderQuad(); // Creates a quad (2 triangles that cover the whole screen) to render to. ///home/andrew/Documents/3DModels/san-miguel-low-poly.binvox ///home/andrew/Documents/3DModels/sponza_256x108x158.binvox

    // Set the camera position and direction
    projv::cam.position[0] = 31.6;
    projv::cam.position[1] = 11.7;
    projv::cam.position[2] = -7.9;

    projv::cam.direction[0] = 0;
    projv::cam.direction[2] = 1;

    // Load the scene
    projv::scene SceneToRender;
    SceneToRender = projv::readScene("/home/andrew/Development/CPP/Simple Renderer/Sibenik-Scene");
    projv::writeScene(SceneToRender, "/home/andrew/Development/CPP/Simple Renderer/Sibenik-Scene");
    std::cout << SceneToRender.version << std::endl;
    projv::passSceneToFrag(SceneToRender);

    while(!glfwWindowShouldClose(window)){
        projv::frameCount++;
        GLenum error;
        glfwPollEvents(); // Checks for events to allow for user input.

        // Move the camera based on user input
        projv::moveCameraFromUserInput(window);

        // Update the current camera position and direction in the shader
        projv::updateCameraInShader(rayMarchShader); // Updates the camera position in the shader.
        projv::updateResolutionInShader(rayMarchShader, window); // Updates the resolution in the shader based off of the size of the window.

        // Render the frame
        renderFrame(rayMarchShader, window);

        // Check for OpenGL errors
        GLenum err; // Creates a variable to store OpenGL errors.
        while ((err = glGetError()) != GL_NO_ERROR) { // Detects and stores OpenGL errors in err.
            std::cout << "OpenGL ERROR: " << err << std::endl; // If there is an error, print it to the console.
            return -1; // Return -1 to close the program.
        }
    }
    glfwTerminate(); // Terminates GLFW and closes the window.
    return 0;
}
