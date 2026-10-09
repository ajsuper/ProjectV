#include "core/application.h"

#include <algorithm>
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

    void Application::addSystem(Stage stage, std::string name, System system, SystemOrder order) {
        size_t index = static_cast<size_t>(stage);
        if (!system) {
            core::warn("addSystem: '{}' on {} is empty - ignored", name, stageName(stage));
            return;
        }
        std::vector<NamedSystem>& systems = stages[index];

        // The new system must come after every system it names in `after` or that names it in
        // `before`, and before every system it names in `before` or that names it in `after`.
        size_t earliest = 0, latest = systems.size();
        for (size_t i = 0; i < systems.size(); i++) {
            const NamedSystem& existing = systems[i];
            bool mustFollow = (!order.after.empty() && existing.name == order.after) ||
                              existing.order.before == name;
            bool mustPrecede = (!order.before.empty() && existing.name == order.before) ||
                               existing.order.after == name;
            if (mustFollow) earliest = std::max(earliest, i + 1);
            if (mustPrecede) latest = std::min(latest, i);
        }
        size_t at = latest;   // as late as allowed: unconstrained systems keep their added order
        if (earliest > latest) {
            core::warn("addSystem: '{}' on {} cannot be both before and after the systems its order "
                       "names - appended", name, stageName(stage));
            at = systems.size();
        }
        systems.insert(systems.begin() + static_cast<std::ptrdiff_t>(at), {name, std::move(system), std::move(order)});
        names[index].insert(names[index].begin() + static_cast<std::ptrdiff_t>(at), std::move(name));
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
