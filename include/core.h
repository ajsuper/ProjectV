#ifndef PROJECTV_CORE_H
#define PROJECTV_CORE_H

#include "core.h"
#include "camera.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <functional>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
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

        // Creats an application with default startup, update, and render functions.
        Application createApp();
    
        // Adds an entity to our world.
        Entity createEntity(World& world);
    
        // fetches the component storage for a specific type, creates it if it doesn't exist.
        template<typename T>
        ComponentStorage<T>& getOrCreateStorage(World& world) {
            std::type_index type = std::type_index(typeid(T));
            if (world.componentStorages.find(type) == world.componentStorages.end()) {
                world.componentStorages[type] = ComponentStorage<T>();
            }
            return std::any_cast<ComponentStorage<T>&>(world.componentStorages[type]);
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
}

#endif
