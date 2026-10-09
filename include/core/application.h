#ifndef PROJECTV_APPLICATION_H
#define PROJECTV_APPLICATION_H

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/events.h"
#include "core/time.h"
#include "core/world.h"

namespace projv {
    // When a system runs. Every frame runs the loop stages in this order; Startup and Shutdown run
    // once each, either side of the loop.
    enum class Stage {
        Startup,      // once, before the first frame
        PreUpdate,    // poll the platform and fill Input (Time has already advanced)
        FixedUpdate,  // zero or more times per frame, every Time::fixedDelta -- physics lives here
        Update,       // gameplay, once per frame, Time::delta
        PostUpdate,   // transform propagation and Scene sync; events are pumped after it
        Render,       // draw
        Shutdown      // once, after the loop ends
    };
    inline constexpr size_t STAGE_COUNT = 7;

    const char* stageName(Stage stage);

    class Application;
    // A system is handed the application rather than capturing it: a lambda that captured `app` and
    // was stored inside `app` is what made the old Application unmovable (known-latent-issues #1).
    using System = std::function<void(Application&)>;

    // Where a system goes within its stage, relative to others by name. Empty is no constraint.
    // A constraint naming a system not added yet still holds when that system is added later: the
    // engine's installers use this so the order they need does not depend on the order a program
    // happens to install them in (physics writes poses before the Scene bridge reads them).
    struct SystemOrder {
        std::string before;
        std::string after;
    };

    // Owns the world, the stages and the frame loop.
    //
    //     projv::Application app;
    //     app.addSystem(projv::Stage::Startup, "load scene", loadScene);
    //     app.addSystem(projv::Stage::Update,  "move camera", moveCamera);
    //     app.run();
    //
    // **Not copyable or movable, by construction.** It keeps pointers to its own Time and Events and
    // connects a CloseRequested handler to itself, so a moved copy would go on answering for the
    // original. Create it where it will live. Systems are handed the Application rather than
    // capturing it, so nothing else needs it to stay put.
    class Application {
    public:
        Application();
        ~Application();
        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;
        Application(Application&&) = delete;
        Application& operator=(Application&&) = delete;

        World world;

        // Ends the loop at the end of the current frame; Shutdown then runs. Set it directly, or let
        // a CloseRequested event set it (see closeOnRequest).
        bool closeRequested = false;

        // Frames completed. The first frame sees 0 -- the count the old loop kept, which the
        // examples' accumulation shaders are written against. Time::frame counts frames *begun*.
        int frameCount = 0;

        // When true (the default), a CloseRequested event ends the application. An application that
        // wants to ask first -- unsaved changes -- sets this false and handles CloseRequested itself.
        bool closeOnRequest = true;

        // Adds a system to a stage. Within a stage, systems run in the order they were added,
        // except where a SystemOrder -- this system's, or one an earlier system gave relative to
        // this one's name -- says otherwise: then it goes as late as the constraints allow.
        // Constraints that cannot all hold are logged, and the system is appended. The name is what
        // logs and profilers report.
        void addSystem(Stage stage, std::string name, System system, SystemOrder order = {});

        // Startup, the loop until closeRequested, then Shutdown. Returns once Shutdown has run.
        void run();

        // Runs exactly one frame of the loop: advance Time, PreUpdate, FixedUpdate as many times as
        // Time owes, Update, PostUpdate, pump events, Render. run() is this in a loop; tests call it
        // directly. Startup must already have run (runStartup) for a frame to mean anything.
        void runFrame();
        void runStartup();
        void runShutdown();

        // The seconds clock the frame loop reads. steady_clock by default; tests replace it so that
        // Time sees exact deltas.
        std::function<double()> clock;

        // The engine's own resources, which also live in world.ctx().
        Time&   time()   { return *timeResource; }
        Events& events() { return *eventsResource; }

        // Systems registered on a stage, for diagnostics.
        const std::vector<std::string>& systemNames(Stage stage) const;

    private:
        struct NamedSystem {
            std::string name;
            System      run;
            SystemOrder order;
        };
        void runStage(Stage stage);

        std::array<std::vector<NamedSystem>, STAGE_COUNT> stages;
        std::array<std::vector<std::string>, STAGE_COUNT> names;
        Time*   timeResource = nullptr;
        Events* eventsResource = nullptr;
        bool    startupRan = false;
        bool    shutdownRan = false;
    };

    // Sent by the platform when the window asks to close. With Application::closeOnRequest set,
    // the engine answers it by ending the application.
    struct CloseRequested {};

    // Sent by the platform when the window's framebuffer size changes.
    struct WindowResized {
        int width = 0;
        int height = 0;
    };
}

#endif
