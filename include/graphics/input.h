#ifndef PROJV_INPUT_H
#define PROJV_INPUT_H

#include <array>
#include <cstdint>

#include "core/application.h"
#include "core/math.h"

namespace projv {
    // ProjectV's own key names, mapped once from GLFW, so gameplay code never includes GLFW.
    enum class Key : uint16_t {
        Unknown,
        A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
        Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
        Space, Enter, Escape, Tab, Backspace, Delete, Insert, Home, End, PageUp, PageDown,
        Left, Right, Up, Down,
        LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt, LeftSuper, RightSuper,
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        Minus, Equal, LeftBracket, RightBracket, Backslash, Semicolon, Apostrophe, Comma, Period,
        Slash, GraveAccent,
        Count
    };

    enum class MouseButton : uint8_t { Left, Right, Middle, Count };

    // The keyboard and mouse as of this frame: an engine resource, filled in PreUpdate by the
    // platform system (graphics::installPlatform). Read it anywhere after PreUpdate:
    //
    //     const auto& input = app.world.ctx().get<projv::Input>();
    //     if (input.down(projv::Key::W)) ...
    //     if (input.pressed(projv::MouseButton::Left)) ...
    //
    // `pressed` and `released` are true for exactly the frame the change was seen.
    //
    // An application that draws ImGui should copy io.WantCaptureKeyboard / WantCaptureMouse into
    // the two `want` flags after ImGui's NewFrame, so gameplay can yield to a focused text field. The
    // engine cannot do that itself: it does not depend on ImGui.
    struct Input {
        bool down(Key key) const         { return bits(key) & DOWN; }
        bool pressed(Key key) const      { return bits(key) & PRESSED; }
        bool released(Key key) const     { return bits(key) & RELEASED; }
        bool down(MouseButton b) const     { return mouse[static_cast<size_t>(b)] & DOWN; }
        bool pressed(MouseButton b) const  { return mouse[static_cast<size_t>(b)] & PRESSED; }
        bool released(MouseButton b) const { return mouse[static_cast<size_t>(b)] & RELEASED; }

        core::vec2 cursor{0.0f};        // window coordinates, in screen units
        core::vec2 cursorDelta{0.0f};   // since the previous frame; zero on the first
        core::vec2 scroll{0.0f};        // this frame's wheel movement (x, y)
        bool cursorCaptured = false;    // set through graphics::setCursorCaptured

        bool wantCaptureKeyboard = false;
        bool wantCaptureMouse = false;

        // Written by the platform system. A key's state this frame from whether it is down now.
        void setKey(Key key, bool isDown)               { update(keys[static_cast<size_t>(key)], isDown); }
        void setMouse(MouseButton button, bool isDown)  { update(mouse[static_cast<size_t>(button)], isDown); }

    private:
        static constexpr uint8_t DOWN = 1, PRESSED = 2, RELEASED = 4;
        uint8_t bits(Key key) const { return keys[static_cast<size_t>(key)]; }
        static void update(uint8_t& state, bool isDown) {
            bool wasDown = state & DOWN;
            state = (isDown ? DOWN : 0) | (isDown && !wasDown ? PRESSED : 0) | (!isDown && wasDown ? RELEASED : 0);
        }
        std::array<uint8_t, static_cast<size_t>(Key::Count)> keys{};
        std::array<uint8_t, static_cast<size_t>(MouseButton::Count)> mouse{};
    };
}

namespace projv::graphics {
    class RenderInstance;

    // Connects the application to the window: adds a PreUpdate system that polls GLFW, fills the
    // Input resource (emplacing it), and sends CloseRequested and WindowResized.
    //
    // **It takes over event polling.** renderConstructedRenderer stops calling glfwPollEvents once
    // this is installed, so events are seen at the top of the frame rather than in the middle of
    // drawing. The close button becomes a CloseRequested event rather than a flag: with
    // Application::closeOnRequest (the default) that ends the application, and an application that
    // wants to ask first turns closeOnRequest off and handles the event. RenderInstance::shouldClose
    // is still set, for code that reads it.
    //
    // Install it after setting any GLFW scroll callback of your own: it chains to the one it finds.
    // `renderInstance` must outlive the application's loop; keeping it in world.ctx() does that.
    void installPlatform(Application& app, RenderInstance& renderInstance);

    // Hides and locks the cursor (mouse-look) or releases it, and records which in Input.
    void setCursorCaptured(Application& app, RenderInstance& renderInstance, bool captured);
}

#endif
