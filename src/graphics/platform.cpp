#include "graphics/input.h"

#include <memory>
#include <unordered_map>

#include "graphics/render_instance.h"

namespace projv::graphics {
    namespace {
        // The wheel arrives through a callback, between polls. Kept per window, and chained to
        // whatever callback was installed before ours so an example's own handler still runs.
        struct ScrollState {
            core::vec2 accumulated{0.0f};
            GLFWscrollfun previous = nullptr;
        };
        std::unordered_map<GLFWwindow*, ScrollState>& scrollStates() {
            static std::unordered_map<GLFWwindow*, ScrollState> states;
            return states;
        }
        void onScroll(GLFWwindow* window, double x, double y) {
            auto it = scrollStates().find(window);
            if (it == scrollStates().end()) return;
            it->second.accumulated += core::vec2(float(x), float(y));
            if (it->second.previous) it->second.previous(window, x, y);
        }

        struct KeyMapping { Key key; int glfw; };
        constexpr KeyMapping KEY_MAP[] = {
            {Key::A, GLFW_KEY_A}, {Key::B, GLFW_KEY_B}, {Key::C, GLFW_KEY_C}, {Key::D, GLFW_KEY_D},
            {Key::E, GLFW_KEY_E}, {Key::F, GLFW_KEY_F}, {Key::G, GLFW_KEY_G}, {Key::H, GLFW_KEY_H},
            {Key::I, GLFW_KEY_I}, {Key::J, GLFW_KEY_J}, {Key::K, GLFW_KEY_K}, {Key::L, GLFW_KEY_L},
            {Key::M, GLFW_KEY_M}, {Key::N, GLFW_KEY_N}, {Key::O, GLFW_KEY_O}, {Key::P, GLFW_KEY_P},
            {Key::Q, GLFW_KEY_Q}, {Key::R, GLFW_KEY_R}, {Key::S, GLFW_KEY_S}, {Key::T, GLFW_KEY_T},
            {Key::U, GLFW_KEY_U}, {Key::V, GLFW_KEY_V}, {Key::W, GLFW_KEY_W}, {Key::X, GLFW_KEY_X},
            {Key::Y, GLFW_KEY_Y}, {Key::Z, GLFW_KEY_Z},
            {Key::Num0, GLFW_KEY_0}, {Key::Num1, GLFW_KEY_1}, {Key::Num2, GLFW_KEY_2},
            {Key::Num3, GLFW_KEY_3}, {Key::Num4, GLFW_KEY_4}, {Key::Num5, GLFW_KEY_5},
            {Key::Num6, GLFW_KEY_6}, {Key::Num7, GLFW_KEY_7}, {Key::Num8, GLFW_KEY_8},
            {Key::Num9, GLFW_KEY_9},
            {Key::Space, GLFW_KEY_SPACE}, {Key::Enter, GLFW_KEY_ENTER}, {Key::Escape, GLFW_KEY_ESCAPE},
            {Key::Tab, GLFW_KEY_TAB}, {Key::Backspace, GLFW_KEY_BACKSPACE}, {Key::Delete, GLFW_KEY_DELETE},
            {Key::Insert, GLFW_KEY_INSERT}, {Key::Home, GLFW_KEY_HOME}, {Key::End, GLFW_KEY_END},
            {Key::PageUp, GLFW_KEY_PAGE_UP}, {Key::PageDown, GLFW_KEY_PAGE_DOWN},
            {Key::Left, GLFW_KEY_LEFT}, {Key::Right, GLFW_KEY_RIGHT}, {Key::Up, GLFW_KEY_UP},
            {Key::Down, GLFW_KEY_DOWN},
            {Key::LeftShift, GLFW_KEY_LEFT_SHIFT}, {Key::RightShift, GLFW_KEY_RIGHT_SHIFT},
            {Key::LeftControl, GLFW_KEY_LEFT_CONTROL}, {Key::RightControl, GLFW_KEY_RIGHT_CONTROL},
            {Key::LeftAlt, GLFW_KEY_LEFT_ALT}, {Key::RightAlt, GLFW_KEY_RIGHT_ALT},
            {Key::LeftSuper, GLFW_KEY_LEFT_SUPER}, {Key::RightSuper, GLFW_KEY_RIGHT_SUPER},
            {Key::F1, GLFW_KEY_F1}, {Key::F2, GLFW_KEY_F2}, {Key::F3, GLFW_KEY_F3}, {Key::F4, GLFW_KEY_F4},
            {Key::F5, GLFW_KEY_F5}, {Key::F6, GLFW_KEY_F6}, {Key::F7, GLFW_KEY_F7}, {Key::F8, GLFW_KEY_F8},
            {Key::F9, GLFW_KEY_F9}, {Key::F10, GLFW_KEY_F10}, {Key::F11, GLFW_KEY_F11},
            {Key::F12, GLFW_KEY_F12},
            {Key::Minus, GLFW_KEY_MINUS}, {Key::Equal, GLFW_KEY_EQUAL},
            {Key::LeftBracket, GLFW_KEY_LEFT_BRACKET}, {Key::RightBracket, GLFW_KEY_RIGHT_BRACKET},
            {Key::Backslash, GLFW_KEY_BACKSLASH}, {Key::Semicolon, GLFW_KEY_SEMICOLON},
            {Key::Apostrophe, GLFW_KEY_APOSTROPHE}, {Key::Comma, GLFW_KEY_COMMA},
            {Key::Period, GLFW_KEY_PERIOD}, {Key::Slash, GLFW_KEY_SLASH},
            {Key::GraveAccent, GLFW_KEY_GRAVE_ACCENT},
        };
    }

    void installPlatform(Application& app, RenderInstance& renderInstance) {
        app.world.ctx().emplace<Input>();
        renderInstance.pollEventsInRender = false;

        ScrollState& scroll = scrollStates()[renderInstance.window];
        scroll.previous = glfwSetScrollCallback(renderInstance.window, onScroll);
        // Installing twice would make our callback its own `previous`, and every wheel tick count
        // twice -- then forever.
        if (scroll.previous == onScroll) scroll.previous = nullptr;

        RenderInstance* instance = &renderInstance;
        auto firstFrame = std::make_shared<bool>(true);
        auto lastSize = std::make_shared<core::ivec2>(0);

        app.addSystem(Stage::PreUpdate, "platform", [instance, firstFrame, lastSize](Application& a) {
            GLFWwindow* window = instance->window;
            glfwPollEvents();
            Input& input = a.world.ctx().get<Input>();

            for (const KeyMapping& mapping : KEY_MAP) {
                input.setKey(mapping.key, glfwGetKey(window, mapping.glfw) == GLFW_PRESS);
            }
            input.setMouse(MouseButton::Left,   glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
            input.setMouse(MouseButton::Right,  glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
            input.setMouse(MouseButton::Middle, glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS);

            double x = 0.0, y = 0.0;
            glfwGetCursorPos(window, &x, &y);
            core::vec2 cursor{float(x), float(y)};
            input.cursorDelta = *firstFrame ? core::vec2(0.0f) : cursor - input.cursor;
            input.cursor = cursor;

            ScrollState& state = scrollStates()[window];
            input.scroll = state.accumulated;
            state.accumulated = core::vec2(0.0f);

            // The close button is an edge, not a level: request once, then clear GLFW's flag, so an
            // application that declines the request can be asked again.
            if (glfwWindowShouldClose(window)) {
                glfwSetWindowShouldClose(window, GLFW_FALSE);
                instance->shouldClose = true;
                a.events().send(CloseRequested{});
            }

            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            core::ivec2 size{width, height};
            if (!*firstFrame && size != *lastSize) a.events().send(WindowResized{width, height});
            *lastSize = size;
            *firstFrame = false;
        });
    }

    void setCursorCaptured(Application& app, RenderInstance& renderInstance, bool captured) {
        glfwSetInputMode(renderInstance.window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        if (Input* input = app.world.ctx().find<Input>()) {
            input->cursorCaptured = captured;
            // The jump from wherever the cursor was to wherever capture puts it is not a movement.
            double x = 0.0, y = 0.0;
            glfwGetCursorPos(renderInstance.window, &x, &y);
            input->cursor = core::vec2(float(x), float(y));
        }
    }
}
