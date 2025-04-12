#ifndef PROJECTV_CORE_H
#define PROJECTV_CORE_H


#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "core.h"
#include "camera.h"
#include "data_structures/renderInstance.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <functional>
#include <bitset>
#include <chrono>
#include <unordered_map>
#include <any>
#include <typeindex>
#include <vector>
#include <cmath>


namespace projv{
    namespace ECS {    

        using Entity = uint32_t;
        template<typename T>
        using ComponentStorage = std::unordered_map<Entity, T>;

        // Defines our different stages during the game loop.
        enum class SystemStage {
            Startup,
            Update,
            Render
        };

        // Main world object that stores the state of the world.
        struct World {  
                Entity nextEntityID = 0;
                std::unordered_map<std::type_index, std::any> globalResources;
                std::unordered_map<std::type_index, std::any> componentStorages;
        };

        // Links and controls the data and logic of the engine.
        struct Application {
            bool closeAppFlag = false;
            int frameCount = 0;
            World world;

            std::function<void()> Startup;
            std::function<void()> Update;
            std::function<void()> Render;
        };

        World createWorld();

        // Creats an application with default startup, update, and render functions.
        Application createApp();
    
        // Adds an entity to our world.
        Entity createEntity(World& world);

        template<typename T>
        T& createGlobalResource(World& world) {
            std::type_index typeIndex = std::type_index(typeid(T));
            auto it = world.globalResources.find(typeIndex);
            if (it == world.globalResources.end()) {
                T resource;
                auto [insertedIt, _] = world.globalResources.emplace(typeIndex, std::move(resource));
                return std::any_cast<T&>(insertedIt->second);
            } else {
                std::cout << "Resource already exists!" << std::endl;
                return std::any_cast<T&>(it->second);
            }
        }
        
        template<typename T>
        void deleteGlobalResource(World& world) {
            std::type_index typeIndex = std::type_index(typeid(T));
            world.globalResources.erase(typeIndex);
        }
    
        // fetches the component storage for a specific type, creates it if it doesn't exist.
        template<typename T>
        ComponentStorage<T>& getOrCreateStorage(World& world) {
            std::type_index type = std::type_index(typeid(T));
            auto it = world.componentStorages.find(type);
            if (it == world.componentStorages.end()) {
                auto [newIt, _] = world.componentStorages.emplace(type, ComponentStorage<T>());
                return std::any_cast<ComponentStorage<T>&>(newIt->second);
                world.componentStorages[type] = ComponentStorage<T>();
            } else {
                return std::any_cast<ComponentStorage<T>&>(it->second);
            }
        }
    
        // Attempts to get the component storage for  specific type, returns nullptr if it doesn't exist.
        template<typename T>
        ComponentStorage<T>* tryGetStorage(World& world) {
            auto it = world.componentStorages.find(std::type_index(typeid(T)));
            if (it == world.componentStorages.end()) {
                return nullptr;
            }
            return std::any_cast<ComponentStorage<T>>(&it->second);
        }
        // Adds a component to an entity.
        template<typename T>
        void addComponent(World& world, Entity& entity, T component){
            auto& storage = getOrCreateStorage<T>(world);
            storage[entity] = std::move(component);
        }
    
        // Adds a component to an entity.
        template<typename T>
        void removeComponent(World& world, Entity& entity){
            auto& storage = getOrCreateStorage<T>(world);
            storage.erase(entity);
        }
    
        // Checks if an entity has a component.
        template<typename T>
        bool hasComponent(World& world, Entity entity) {
            auto* storage = tryGetStorage<T>(world);
            return storage && storage->find(entity) != storage->end();
        }
    
        // Fetches a component from an entity.
        template<typename T>
        T& getComponent(World& world, Entity entity) {
            auto& storage = getOrCreateStorage<T>(world);
            return storage.at(entity);
        }

        template<typename T>
        T& getGlobalResource(World& world) {
            auto it = world.globalResources.find(std::type_index(typeid(T)));
            if (it == world.globalResources.end()) {
                std::cout << "Resource not found!" << std::endl;
            }
            return std::any_cast<T&>(it->second);
        }
    
        // Loops over all the entities with the specified components and calls the function.
        template<typename... Components, typename Func>
        void forEachEntityWith(ECS::World& world, Func func) {
            auto& primaryStorage = getOrCreateStorage<std::tuple_element_t<0, std::tuple<Components...>>>(world);
            for (auto& [entity, _] : primaryStorage) {
                if ((hasComponent<Components>(world, entity) && ...)) {
                    func(entity, getComponent<Components>(world, entity)...);
                }
            }
        }
    
        // Adds a system to our application for a specific stage.
        void addSystem(ECS::Application& app, SystemStage stage, std::function<void(ECS::Application&)> system);
    
        // Runs the specified application and starts the loop.
        void runApplication(Application& app);
    }
    /**
     * Move/Rotate the camera from WASD QE SPACE/SHIFT (R and F to change movement speeds).
     * 
     * @param window The GLFW window to get user input from.
     */
    void moveCameraFromUserInput(GLFWwindow* window);

    /**
     * Initializes GLFW and GLEW, and creates a window.
     * 
     * @param window A reference to the GLFWwindow pointer that will be initialized.
     * @param windowWidth The width of the window to be created.
     * @param windowHeight The height of the window to be created.
     * @param windowName The title of the window to be created.
     * @return true if initialization and window creation were successful, false otherwise.
     */
    bool initializeGLFWandGLEWWindow(GLFWwindow*& window, int windowWidth, int windowHeight, const std::string& windowName);

    /**
     * Initializes GLFW, GLEW, our fullscreen render quad, and creates a window.
     * 
     * @param windowWidth The width of the window.
     * @param windowHeight The height of the window.
     * @param windowName The name of the window.
     * @return Returns a RenderInstance with the initialized variables.
     */
    RenderInstance initializeRenderInstance(int windowWidth, int windowHeight, const std::string& windowName);

    /**
     * Sets the error callback(what happens when an OpenGL error occurs) that OpenGL will use
     * 
     * @param renderInstance The renderInstance that we want to set the callback for.
     * @param error_callback A function pointer that stores the user defined function to handle error's.
     */
    void setErrorCallback(RenderInstance& renderInstance, void (*error_callback)(int error_code, const char* description));

    /**
     * Sets the frame buffer size callback(what happens on window resize) that OpenGL will use.
     */
    void setFrameBufferSizeCallback(RenderInstance& renderInstance, void (*framebuffer_callback)(GLFWwindow* window, int width, int height));

    /**
     * Adds an OpenGL shader to our render instance. Useful when using the ECS system to pass shaders globally.
     * 
     * @param renderInstance The RenderInstance to which the shader will be added.
     * @param shader The OpenGL shader ID to be added.
     * @param shaderName The name of the shader to be used as a key for retrieval.
     */
    void addShaderToRenderInstance(RenderInstance& renderInstance, GLuint shader, std::string shaderName);

    /**
     * Removes an OpenGL shader from our render instance.
     * 
     * @param renderInstance The RenderInstance from which the shader will be removed.
     * @param shaderName The name of the shader to be removed.
     */
    void removeShaderFromRenderInstance(RenderInstance& renderInstance, std::string shaderName);

    /**
     * Retrieves an OpenGL shader from our render instance.
     * 
     * @param renderInstance The RenderInstance from which the shader will be retrieved.
     * @param shaderName The name of the shader to retrieve.
     * @return The OpenGL shader ID associated with the given shader name.
     */
    GLuint getShaderFromRenderInstance(RenderInstance& renderInstance, std::string shaderName);

    /**
     * Adds a framebuffer to our render instance.
     * 
     * @param renderInstance The RenderInstance to which the framebuffer will be added.
     * @param framebuffer The FrameBuffer object to be added.
     * @param name The name of the framebuffer to be used as a key for retrieval.
     */
    void addFramebufferToRenderInstance(RenderInstance& renderInstance, const FrameBuffer& framebuffer, const std::string& name);

    /**
     * Removes a framebuffer from our render instance.
     * 
     * @param renderInstance The RenderInstance from which the framebuffer will be removed.
     * @param name The name of the framebuffer to be removed.
     */
    void removeFramebufferFromRenderInstance(RenderInstance& renderInstance, const std::string& name);

    /**
     * Retrieves a framebuffer from our render instance.
     * 
     * @param renderInstance The RenderInstance from which the framebuffer will be retrieved.
     * @param name The name of the framebuffer to retrieve.
     * @return The FrameBuffer object associated with the given name.
     */
    FrameBuffer getFramebufferFromRenderInstance(RenderInstance& renderInstance, const std::string& name);
}

#endif
