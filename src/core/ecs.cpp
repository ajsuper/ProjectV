#include <iostream>

#include "core/ecs.h"

namespace projv::core {
    World createWorld() {
        World world;
        return world;
    }

    // Creates an application with default startup, update, and render functions.
    Application createApp() {
        Application app;
        app.Startup = []() {
            core::info("Default Startup!");
        };
        app.Update = []() {
            core::info("Default Update!");
        };
        app.Render = []() {
            core::info("Default Render!");
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
    void addSystem(Application& app, SystemStage stage, std::function<void(Application&)> system) {
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