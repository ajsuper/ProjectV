#include "core/application.h"

#include <chrono>

#include "core/log.h"

namespace projv {
    const char* stageName(Stage stage) {
        switch (stage) {
            case Stage::Startup:     return "Startup";
            case Stage::PreUpdate:   return "PreUpdate";
            case Stage::FixedUpdate: return "FixedUpdate";
            case Stage::Update:      return "Update";
            case Stage::PostUpdate:  return "PostUpdate";
            case Stage::Render:      return "Render";
            case Stage::Shutdown:    return "Shutdown";
        }
        return "?";
    }

    Application::Application() {
        timeResource = &world.ctx().emplace<Time>();
        eventsResource = &world.ctx().emplace<Events>();

        // steady_clock, not system_clock: a wall-clock adjustment must not become a frame of minus
        // an hour.
        clock = [] {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        };

        eventsResource->on<CloseRequested>([this](const CloseRequested&) {
            if (closeOnRequest) closeRequested = true;
        });
    }

    // `world` is the first member, so it is destroyed last: systems -- which may hold references
    // into its resources -- are gone before the resources are.
    Application::~Application() = default;

    void Application::addSystem(Stage stage, std::string name, System system) {
        size_t index = static_cast<size_t>(stage);
        if (!system) {
            core::warn("addSystem: '{}' on {} is empty - ignored", name, stageName(stage));
            return;
        }
        names[index].push_back(name);
        stages[index].push_back({std::move(name), std::move(system)});
    }

    const std::vector<std::string>& Application::systemNames(Stage stage) const {
        return names[static_cast<size_t>(stage)];
    }

    void Application::runStage(Stage stage) {
        // By index, and re-reading the size each step: a system may add another system to the stage
        // it is running in, which then runs this frame, after it.
        std::vector<NamedSystem>& systems = stages[static_cast<size_t>(stage)];
        for (size_t i = 0; i < systems.size(); i++) {
            System run = systems[i].run;   // a copy: addSystem may reallocate the vector under the call
            run(*this);
        }
    }

    void Application::runStartup() {
        if (startupRan) return;
        startupRan = true;
        runStage(Stage::Startup);
    }

    void Application::runFrame() {
        int fixedSteps = timeResource->advance(clock());

        runStage(Stage::PreUpdate);
        for (int step = 0; step < fixedSteps; step++) {
            runStage(Stage::FixedUpdate);
            timeResource->fixedStep++;
        }
        runStage(Stage::Update);
        runStage(Stage::PostUpdate);
        // After PostUpdate and before Render, so a frame's events are handled before it is drawn.
        eventsResource->pump();
        runStage(Stage::Render);
        frameCount++;
    }

    void Application::runShutdown() {
        if (shutdownRan) return;
        shutdownRan = true;
        runStage(Stage::Shutdown);
    }

    void Application::run() {
        runStartup();
        while (!closeRequested) {
            runFrame();
        }
        runShutdown();
    }
}
