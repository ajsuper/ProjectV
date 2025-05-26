#ifndef PROJECTV_APPLICATION_H
#define PROJECTV_APPLICATION_H

#include <cstdint>
#include <typeindex>
#include <any>
#include <unordered_map>
#include <functional>

namespace projv {
    using Entity = uint32_t;
    template<typename T>
    using ComponentStorage = std::unordered_map<Entity, T>;

    // Defines our different stages during the game loop.
    enum class SystemStage {
        Startup,
        Update,
        Render,
        Shutdown
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
        std::function<void()> Shutdown;
    };
}

#endif