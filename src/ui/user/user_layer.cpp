// The user layer (ui/user.h, ui/user_layer.h): an RmlUi context as a ui::Layer. Fed from the engine's
// Input (D4), drawn in USER_VIEW under the dev layer, its documents beside the program (D6), values
// in through data models and actions out as UiAction events (D7).

#include "ui/user.h"
#include "ui/user_layer.h"
#include "ui/ui.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <RmlUi/Core.h>

#include "interfaces.h"

#include "core/application.h"
#include "core/log.h"
#include "core/time.h"
#include "graphics/input.h"
#include "graphics/render_instance.h"

namespace projv::ui::user {
    // ---- The system interface ------------------------------------------------------------------

    bool EngineSystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message) {
        switch (type) {
            case Rml::Log::LT_ERROR:
            case Rml::Log::LT_ASSERT: core::error("ui: {}", message); break;
            case Rml::Log::LT_WARNING: core::warn("ui: {}", message); break;
            case Rml::Log::LT_INFO: core::info("ui: {}", message); break;
            default: core::trace("ui: {}", message); break;
        }
        return true;
    }

    void EngineSystemInterface::SetClipboardText(const Rml::String& text) {
        if (window) glfwSetClipboardString(window, text.c_str());
    }

    void EngineSystemInterface::GetClipboardText(Rml::String& text) {
        const char* clip = window ? glfwGetClipboardString(window) : nullptr;
        text = clip ? clip : "";
    }

    namespace {
        // RmlUi is one library-wide instance: initialised with the first layer, shut down with the last.
        int g_layers = 0;
        EngineSystemInterface& systemInterface() {
            static EngineSystemInterface instance;
            return instance;
        }
        // Each layer's context, so an element's `on<event>` listener can find the layer it is in.
        std::map<Rml::Context*, UserLayer::Impl*>& layersByContext() {
            static std::map<Rml::Context*, UserLayer::Impl*> map;
            return map;
        }

        struct KeyMapping { Key key; Rml::Input::KeyIdentifier rml; };
        constexpr KeyMapping KEYS[] = {
            {Key::A, Rml::Input::KI_A}, {Key::B, Rml::Input::KI_B}, {Key::C, Rml::Input::KI_C}, {Key::D, Rml::Input::KI_D},
            {Key::E, Rml::Input::KI_E}, {Key::F, Rml::Input::KI_F}, {Key::G, Rml::Input::KI_G}, {Key::H, Rml::Input::KI_H},
            {Key::I, Rml::Input::KI_I}, {Key::J, Rml::Input::KI_J}, {Key::K, Rml::Input::KI_K}, {Key::L, Rml::Input::KI_L},
            {Key::M, Rml::Input::KI_M}, {Key::N, Rml::Input::KI_N}, {Key::O, Rml::Input::KI_O}, {Key::P, Rml::Input::KI_P},
            {Key::Q, Rml::Input::KI_Q}, {Key::R, Rml::Input::KI_R}, {Key::S, Rml::Input::KI_S}, {Key::T, Rml::Input::KI_T},
            {Key::U, Rml::Input::KI_U}, {Key::V, Rml::Input::KI_V}, {Key::W, Rml::Input::KI_W}, {Key::X, Rml::Input::KI_X},
            {Key::Y, Rml::Input::KI_Y}, {Key::Z, Rml::Input::KI_Z},
            {Key::Num0, Rml::Input::KI_0}, {Key::Num1, Rml::Input::KI_1}, {Key::Num2, Rml::Input::KI_2}, {Key::Num3, Rml::Input::KI_3},
            {Key::Num4, Rml::Input::KI_4}, {Key::Num5, Rml::Input::KI_5}, {Key::Num6, Rml::Input::KI_6}, {Key::Num7, Rml::Input::KI_7},
            {Key::Num8, Rml::Input::KI_8}, {Key::Num9, Rml::Input::KI_9},
            {Key::Space, Rml::Input::KI_SPACE}, {Key::Enter, Rml::Input::KI_RETURN}, {Key::Escape, Rml::Input::KI_ESCAPE},
            {Key::Tab, Rml::Input::KI_TAB}, {Key::Backspace, Rml::Input::KI_BACK}, {Key::Delete, Rml::Input::KI_DELETE},
            {Key::Insert, Rml::Input::KI_INSERT}, {Key::Home, Rml::Input::KI_HOME}, {Key::End, Rml::Input::KI_END},
            {Key::PageUp, Rml::Input::KI_PRIOR}, {Key::PageDown, Rml::Input::KI_NEXT},
            {Key::Left, Rml::Input::KI_LEFT}, {Key::Right, Rml::Input::KI_RIGHT}, {Key::Up, Rml::Input::KI_UP}, {Key::Down, Rml::Input::KI_DOWN},
            {Key::LeftShift, Rml::Input::KI_LSHIFT}, {Key::RightShift, Rml::Input::KI_RSHIFT},
            {Key::LeftControl, Rml::Input::KI_LCONTROL}, {Key::RightControl, Rml::Input::KI_RCONTROL},
            {Key::LeftAlt, Rml::Input::KI_LMENU}, {Key::RightAlt, Rml::Input::KI_RMENU},
            {Key::LeftSuper, Rml::Input::KI_LMETA}, {Key::RightSuper, Rml::Input::KI_RMETA},
            {Key::F1, Rml::Input::KI_F1}, {Key::F2, Rml::Input::KI_F2}, {Key::F3, Rml::Input::KI_F3}, {Key::F4, Rml::Input::KI_F4},
            {Key::F5, Rml::Input::KI_F5}, {Key::F6, Rml::Input::KI_F6}, {Key::F7, Rml::Input::KI_F7}, {Key::F8, Rml::Input::KI_F8},
            {Key::F9, Rml::Input::KI_F9}, {Key::F10, Rml::Input::KI_F10}, {Key::F11, Rml::Input::KI_F11}, {Key::F12, Rml::Input::KI_F12},
            {Key::Minus, Rml::Input::KI_OEM_MINUS}, {Key::Equal, Rml::Input::KI_OEM_PLUS},
            {Key::LeftBracket, Rml::Input::KI_OEM_4}, {Key::RightBracket, Rml::Input::KI_OEM_6},
            {Key::Backslash, Rml::Input::KI_OEM_5}, {Key::Semicolon, Rml::Input::KI_OEM_1},
            {Key::Apostrophe, Rml::Input::KI_OEM_7}, {Key::Comma, Rml::Input::KI_OEM_COMMA},
            {Key::Period, Rml::Input::KI_OEM_PERIOD}, {Key::Slash, Rml::Input::KI_OEM_2}, {Key::GraveAccent, Rml::Input::KI_OEM_3},
        };

        int modifiers(const Input& input) {
            int m = 0;
            if (input.down(Key::LeftControl) || input.down(Key::RightControl)) m |= Rml::Input::KM_CTRL;
            if (input.down(Key::LeftShift) || input.down(Key::RightShift)) m |= Rml::Input::KM_SHIFT;
            if (input.down(Key::LeftAlt) || input.down(Key::RightAlt)) m |= Rml::Input::KM_ALT;
            if (input.down(Key::LeftSuper) || input.down(Key::RightSuper)) m |= Rml::Input::KM_META;
            return m;
        }

        bool takesText(Rml::Element* element) {
            if (!element) return false;
            const Rml::String& tag = element->GetTagName();
            if (tag == "textarea") return true;
            if (tag != "input") return false;
            const Rml::String type = element->GetAttribute<Rml::String>("type", "text");
            return type == "text" || type == "password";
        }
    }

    // ---- The layer's state ------------------------------------------------------------------------

    struct UserLayer::Impl {
        struct Document {
            Rml::ElementDocument* element = nullptr;
            std::string path;     // empty: opened from memory
            std::string source;   // the RML, when opened from memory
            DocumentOptions options;
            bool shown = false;
            bool reveal = false;          // add ui-shown after the next update, so a transition starts from hidden
            double fadeUntil = -1.0;      // hidden, but drawn until then (DocumentOptions::fadeOut)
            std::map<std::string, std::filesystem::file_time_type> watched;   // the .rml and the .rcss beside it
        };
        struct Bound {
            enum Kind { Float, Int, Bool, String, List, Rows } kind = Float;
            std::string name;
            void* value = nullptr;
            float f = 0.0f;
            int i = 0;
            bool b = false;
            std::string s;
            std::vector<std::string> list;
            std::vector<std::vector<std::string>> rows;
        };
        struct Model {
            Rml::DataModelConstructor constructor;
            Rml::DataModelHandle handle;
            std::vector<Bound> bound;
            bool listsRegistered = false;   // RmlUi needs the list types registered once a model
        };

        Application* app = nullptr;
        graphics::RenderInstance* renderInstance = nullptr;
        bool reloadOnChange = false;
        std::unique_ptr<BgfxRenderInterface> gpu;
        std::unique_ptr<RecordingRenderInterface> recording;
        Rml::Context* context = nullptr;
        std::map<std::string, Document> documents;
        std::map<std::string, Model> models;
        int frames = 0;
        bool mouseInside = false;

        Rml::RenderInterface* renderInterface() {
            return gpu ? static_cast<Rml::RenderInterface*>(gpu.get()) : static_cast<Rml::RenderInterface*>(recording.get());
        }

        std::string nameOf(const Rml::ElementDocument* element) const {
            for (const auto& [name, d] : documents)
                if (d.element == element) return name;
            return "";
        }

        void watch(Document& d) {
            d.watched.clear();
            if (d.path.empty()) return;
            std::error_code error;
            const std::filesystem::path rml(d.path);
            d.watched[d.path] = std::filesystem::last_write_time(rml, error);
            const std::filesystem::path folder = rml.has_parent_path() ? rml.parent_path() : std::filesystem::path(".");
            for (const auto& entry : std::filesystem::directory_iterator(folder, error))
                if (entry.path().extension() == ".rcss") d.watched[entry.path().string()] = entry.last_write_time(error);
        }

        bool load(const std::string& name, Document d) {
            close(name);
            d.element = d.path.empty() ? context->LoadDocumentFromMemory(d.source, "[" + name + "]")
                                       : context->LoadDocument(d.path);
            if (!d.element) {
                core::error("ui: could not load the document '{}' ({})", name, d.path.empty() ? "from memory" : d.path);
                return false;
            }
            if (d.shown) {
                d.element->Show(d.options.modal ? Rml::ModalFlag::Modal : Rml::ModalFlag::None);
                d.reveal = true;
            }
            watch(d);
            documents[name] = std::move(d);
            return true;
        }

        void close(const std::string& name) {
            auto it = documents.find(name);
            if (it == documents.end()) return;
            if (it->second.element) it->second.element->Close();
            documents.erase(it);
        }

        // D6: a document whose file, or a style sheet beside it, changed is loaded again, shown as it was.
        void reloadChanged() {
            std::vector<std::string> changed;
            for (auto& [name, d] : documents) {
                for (const auto& [file, stamp] : d.watched) {
                    std::error_code error;
                    const auto now = std::filesystem::last_write_time(file, error);
                    if (!error && now != stamp) {
                        changed.push_back(name);
                        break;
                    }
                }
            }
            if (changed.empty()) return;
            Rml::Factory::ClearStyleSheetCache();
            Rml::Factory::ClearTemplateCache();
            for (const std::string& name : changed) {
                Document d = documents[name];
                d.element = nullptr;
                if (load(name, d)) core::info("ui: reloaded '{}' ({})", name, d.path);
            }
        }

        // Bound values that changed since the last frame are marked dirty, so what shows them is redrawn.
        void updateModels() {
            for (auto& [name, model] : models) {
                for (Bound& b : model.bound) {
                    bool changed = false;
                    switch (b.kind) {
                        case Bound::Float: changed = *static_cast<float*>(b.value) != b.f; b.f = *static_cast<float*>(b.value); break;
                        case Bound::Int: changed = *static_cast<int*>(b.value) != b.i; b.i = *static_cast<int*>(b.value); break;
                        case Bound::Bool: changed = *static_cast<bool*>(b.value) != b.b; b.b = *static_cast<bool*>(b.value); break;
                        case Bound::String: changed = *static_cast<std::string*>(b.value) != b.s; b.s = *static_cast<std::string*>(b.value); break;
                        case Bound::List: {
                            const auto& now = *static_cast<std::vector<std::string>*>(b.value);
                            changed = now != b.list;
                            if (changed) b.list = now;
                            break;
                        }
                        case Bound::Rows: {
                            const auto& now = *static_cast<std::vector<std::vector<std::string>>*>(b.value);
                            changed = now != b.rows;
                            if (changed) b.rows = now;
                            break;
                        }
                    }
                    if (changed && model.handle) model.handle.DirtyVariable(b.name);
                }
            }
        }

        bool anyShown() const {
            return std::any_of(documents.begin(), documents.end(), [](const auto& e) { return e.second.shown; });
        }
        // Shown, or hidden and still fading out: what is drawn.
        bool anyDrawn() const {
            return std::any_of(documents.begin(), documents.end(),
                               [](const auto& e) { return e.second.shown || e.second.fadeUntil >= 0.0; });
        }

        // After an update: ui-shown on a document just shown, and a faded-out one hidden for good.
        void updateFades() {
            const double now = systemInterface().seconds;
            for (auto& [name, d] : documents) {
                if (!d.element) continue;
                if (d.reveal) {
                    d.element->SetClass("ui-shown", true);
                    d.reveal = false;
                }
                if (d.fadeUntil >= 0.0 && now >= d.fadeUntil) {
                    d.element->Hide();
                    d.fadeUntil = -1.0;
                }
            }
        }
        bool modalShown() const {
            return std::any_of(documents.begin(), documents.end(), [](const auto& e) { return e.second.shown && e.second.options.modal; });
        }
    };

    namespace {
        // `on<event>="name arg arg"` on an element: a UiAction when that event happens.
        class ActionListener : public Rml::EventListener {
        public:
            explicit ActionListener(const Rml::String& value) {
                std::istringstream words(value);
                std::string word;
                if (words >> word) name = word;
                while (words >> word) args.push_back(word);
            }
            void ProcessEvent(Rml::Event& event) override {
                Rml::Element* element = event.GetCurrentElement();
                if (!element || name.empty()) return;
                auto it = layersByContext().find(element->GetContext());
                if (it == layersByContext().end()) return;
                UserLayer::Impl& layer = *it->second;
                layer.app->events().send(UiAction{layer.nameOf(element->GetOwnerDocument()), name, args, event.GetType()});
            }
            void OnDetach(Rml::Element*) override { delete this; }

        private:
            std::string name;
            std::vector<std::string> args;
        };

        class ActionInstancer : public Rml::EventListenerInstancer {
        public:
            Rml::EventListener* InstanceEventListener(const Rml::String& value, Rml::Element*) override {
                return new ActionListener(value);
            }
        };

        ActionInstancer& actionInstancer() {
            static ActionInstancer instance;
            return instance;
        }

        UserLayer* layerOf(const Application& app) {
            const Host* host = app.world.ctx().find<Host>();
            return host ? static_cast<UserLayer*>(host->layer(Level::User)) : nullptr;
        }

        UserLayer::Impl* implOf(const Application& app, const char* call) {
            UserLayer* layer = layerOf(app);
            if (!layer) {
                core::error("ui: {} needs the user layer: ui::installUi first", call);
                return nullptr;
            }
            return &layer->impl();
        }
    }

    // ---- The layer -------------------------------------------------------------------------------

    UserLayer::UserLayer(Application& app, graphics::RenderInstance* renderInstance, bool reloadOnChange)
        : state(std::make_unique<Impl>()) {
        state->app = &app;
        state->renderInstance = renderInstance;
        state->reloadOnChange = reloadOnChange;
        if (g_layers++ == 0) {
            Rml::SetSystemInterface(&systemInterface());
            Rml::Initialise();
            Rml::Factory::RegisterEventListenerInstancer(&actionInstancer());
        }
        if (renderInstance) {
            state->gpu = std::make_unique<BgfxRenderInterface>();
            if (!state->gpu->ready()) state->gpu.reset();   // logged: documents still work, nothing draws
            systemInterface().window = renderInstance->window;
        }
        if (!state->gpu) state->recording = std::make_unique<RecordingRenderInterface>();
        static int contexts = 0;
        state->context = Rml::CreateContext("projv-" + std::to_string(contexts++), Rml::Vector2i(1280, 720), state->renderInterface());
        layersByContext()[state->context] = state.get();
    }

    UserLayer::~UserLayer() {
        layersByContext().erase(state->context);
        state->documents.clear();
        Rml::RemoveContext(state->context->GetName());
        if (--g_layers == 0) {
            Rml::Shutdown();
            systemInterface().window = nullptr;
        }
        // The render interface goes last: RmlUi releases its textures and geometry through it.
    }

    bool UserLayer::visible() const { return state->anyDrawn(); }

    int UserLayer::drawCallsRecorded() const { return state->recording ? state->recording->drawCalls : 0; }

    namespace {
        Rml::Element* findElement(const UserLayer::Impl& s, const std::string& document, const std::string& id) {
            auto it = s.documents.find(document);
            return it == s.documents.end() || !it->second.element ? nullptr : it->second.element->GetElementById(id);
        }
    }

    std::string UserLayer::elementText(const std::string& document, const std::string& id) const {
        Rml::Element* element = findElement(*state, document, id);
        return element ? element->GetInnerRML() : std::string();
    }

    bool UserLayer::elementBox(const std::string& document, const std::string& id, float& x, float& y, float& width, float& height) const {
        Rml::Element* element = findElement(*state, document, id);
        if (!element) return false;
        const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
        x = offset.x;
        y = offset.y;
        width = size.x;
        height = size.y;
        return true;
    }

    Capture UserLayer::input(const Input& input, Capture taken) {
        Impl& s = *state;
        systemInterface().seconds += double(s.app->time().unscaledDelta);

        int width = 1280, height = 720;
        if (s.gpu) {
            // The back buffer, not GLFW's framebuffer: on a scaled display they differ (the dev layer, U1).
            if (const bgfx::Stats* stats = bgfx::getStats(); stats && stats->width > 0) {
                width = stats->width;
                height = stats->height;
            }
        }
        s.context->SetDimensions(Rml::Vector2i(width, height));

        const int mods = modifiers(input);
        Capture took;
        if (input.cursorCaptured || taken.mouse) {
            // The game's, or the dev layer's: the documents see the cursor leave.
            if (s.mouseInside) s.context->ProcessMouseLeave();
            s.mouseInside = false;
        } else {
            s.context->ProcessMouseMove(int(input.cursor.x), int(input.cursor.y), mods);
            s.mouseInside = true;
            const MouseButton buttons[] = {MouseButton::Left, MouseButton::Right, MouseButton::Middle};
            for (int b = 0; b < 3; b++) {
                if (input.pressed(buttons[b])) s.context->ProcessMouseButtonDown(b, mods);
                if (input.released(buttons[b])) s.context->ProcessMouseButtonUp(b, mods);
            }
            if (input.scroll.y != 0.0f || input.scroll.x != 0.0f)
                s.context->ProcessMouseWheel(Rml::Vector2f(-input.scroll.x, -input.scroll.y), mods);
            took.mouse = s.context->IsMouseInteracting();
        }
        if (!taken.keyboard) {
            for (const KeyMapping& k : KEYS) {
                if (input.pressed(k.key)) s.context->ProcessKeyDown(k.rml, mods);
                if (input.released(k.key)) s.context->ProcessKeyUp(k.rml, mods);
            }
            for (uint32_t c : input.text) s.context->ProcessTextInput(Rml::Character(c));
            took.keyboard = takesText(s.context->GetFocusElement());
        }
        if (s.modalShown()) took = {true, true};
        // D18: while a document is up, the renderer writes the frame to a texture the UI starts from.
        if (s.gpu && s.renderInstance) s.renderInstance->wantFrameTexture(s.anyDrawn());
        return took;
    }

    void UserLayer::draw(bgfx::ViewId view, core::ivec2 size) {
        Impl& s = *state;
        if (s.reloadOnChange && ++s.frames % 30 == 0) s.reloadChanged();
        s.updateModels();
        s.context->Update();
        s.updateFades();
        if (!s.gpu) {
            s.recording->beginFrame();
            s.context->Render();
            return;
        }
        // Views USER_VIEWS_FIRST..view (the host's USER_VIEW). The scene, if the renderer wrote it to the
        // frame texture this frame; the base layer, scene and UI, then replaces the window.
        const bool fromFrame = s.renderInstance && s.renderInstance->frameRedirected();
        const bgfx::ViewId first = view > USER_VIEWS_FIRST ? USER_VIEWS_FIRST : view;
        s.gpu->beginFrame(first, view, size.x, size.y, fromFrame ? s.renderInstance->frameTexture() : bgfx::TextureHandle{bgfx::kInvalidHandle});
        s.context->Render();
        s.gpu->endFrame();
        if (fromFrame) s.renderInstance->markFrameConsumed();
    }

    // ---- ui/user.h -------------------------------------------------------------------------------

    bool open(Application& app, const std::string& name, const std::string& path, DocumentOptions options) {
        UserLayer::Impl* s = implOf(app, "ui::user::open");
        if (!s) return false;
        UserLayer::Impl::Document d;
        d.path = path;
        d.options = options;
        d.shown = options.shown;
        return s->load(name, std::move(d));
    }

    bool openFromMemory(Application& app, const std::string& name, const std::string& rml, DocumentOptions options) {
        UserLayer::Impl* s = implOf(app, "ui::user::openFromMemory");
        if (!s) return false;
        UserLayer::Impl::Document d;
        d.source = rml;
        d.options = options;
        d.shown = options.shown;
        return s->load(name, std::move(d));
    }

    void close(Application& app, const std::string& name) {
        if (UserLayer::Impl* s = implOf(app, "ui::user::close")) s->close(name);
    }

    void show(Application& app, const std::string& name) {
        UserLayer::Impl* s = implOf(app, "ui::user::show");
        if (!s) return;
        auto it = s->documents.find(name);
        if (it == s->documents.end()) return;
        UserLayer::Impl::Document& d = it->second;
        if (d.shown) return;
        d.shown = true;
        d.fadeUntil = -1.0;
        d.element->Show(d.options.modal ? Rml::ModalFlag::Modal : Rml::ModalFlag::None);
        d.reveal = true;
    }

    void hide(Application& app, const std::string& name) {
        UserLayer::Impl* s = implOf(app, "ui::user::hide");
        if (!s) return;
        auto it = s->documents.find(name);
        if (it == s->documents.end()) return;
        UserLayer::Impl::Document& d = it->second;
        if (!d.shown) return;
        d.shown = false;
        d.reveal = false;
        d.element->SetClass("ui-shown", false);
        if (d.options.fadeOut > 0.0f) d.fadeUntil = systemInterface().seconds + d.options.fadeOut;
        else d.element->Hide();
    }

    bool isOpen(const Application& app, const std::string& name) {
        const UserLayer* layer = layerOf(app);
        return layer && layer->impl().documents.count(name) > 0;
    }

    bool isShown(const Application& app, const std::string& name) {
        const UserLayer* layer = layerOf(app);
        if (!layer) return false;
        auto it = layer->impl().documents.find(name);
        return it != layer->impl().documents.end() && it->second.shown;
    }

    bool loadFont(Application& app, const std::string& path, bool fallback) {
        if (!implOf(app, "ui::user::loadFont")) return false;
        if (!Rml::LoadFontFace(path, fallback)) {
            core::error("ui: could not load the font '{}'", path);
            return false;
        }
        return true;
    }

    Model model(Application& app, const std::string& name) {
        UserLayer::Impl* s = implOf(app, "ui::user::model");
        if (!s) return Model(nullptr, name);
        if (!s->models.count(name)) {
            UserLayer::Impl::Model m;
            m.constructor = s->context->CreateDataModel(name);
            if (!m.constructor) {
                core::error("ui: could not make the data model '{}'", name);
                return Model(nullptr, name);
            }
            m.handle = m.constructor.GetModelHandle();
            s->models.emplace(name, std::move(m));
        }
        return Model(&app, name);
    }

    namespace {
        template<typename T>
        void bindValue(Application* app, const std::string& model, const std::string& name, T* value,
                       UserLayer::Impl::Bound::Kind kind) {
            if (!app) return;
            UserLayer::Impl* s = implOf(*app, "ui::user::Model::bind");
            if (!s) return;
            auto it = s->models.find(model);
            if (it == s->models.end()) return;
            if constexpr (std::is_same_v<T, std::vector<std::string>> || std::is_same_v<T, std::vector<std::vector<std::string>>>) {
                if (!it->second.listsRegistered) {
                    it->second.constructor.RegisterArray<std::vector<std::string>>();
                    it->second.constructor.RegisterArray<std::vector<std::vector<std::string>>>();
                    it->second.listsRegistered = true;
                }
            }
            if (!it->second.constructor.Bind(name, value)) {
                core::error("ui: could not bind '{}' in the data model '{}' (bind before opening a document that uses it)", name, model);
                return;
            }
            UserLayer::Impl::Bound b;
            b.kind = kind;
            b.name = name;
            b.value = value;
            if constexpr (std::is_same_v<T, float>) b.f = *value;
            if constexpr (std::is_same_v<T, int>) b.i = *value;
            if constexpr (std::is_same_v<T, bool>) b.b = *value;
            if constexpr (std::is_same_v<T, std::string>) b.s = *value;
            if constexpr (std::is_same_v<T, std::vector<std::string>>) b.list = *value;
            if constexpr (std::is_same_v<T, std::vector<std::vector<std::string>>>) b.rows = *value;
            it->second.bound.push_back(std::move(b));
        }
    }

    Model& Model::bind(const std::string& variable, float* value) { bindValue(app, name, variable, value, UserLayer::Impl::Bound::Float); return *this; }
    Model& Model::bind(const std::string& variable, int* value) { bindValue(app, name, variable, value, UserLayer::Impl::Bound::Int); return *this; }
    Model& Model::bind(const std::string& variable, bool* value) { bindValue(app, name, variable, value, UserLayer::Impl::Bound::Bool); return *this; }
    Model& Model::bind(const std::string& variable, std::string* value) { bindValue(app, name, variable, value, UserLayer::Impl::Bound::String); return *this; }
    Model& Model::bind(const std::string& variable, std::vector<std::string>* value) { bindValue(app, name, variable, value, UserLayer::Impl::Bound::List); return *this; }
    Model& Model::bind(const std::string& variable, std::vector<std::vector<std::string>>* value) { bindValue(app, name, variable, value, UserLayer::Impl::Bound::Rows); return *this; }

    void Model::dirty(const std::string& variable) {
        if (!app) return;
        UserLayer::Impl* s = implOf(*app, "ui::user::Model::dirty");
        if (!s) return;
        auto it = s->models.find(name);
        if (it != s->models.end() && it->second.handle) it->second.handle.DirtyVariable(variable);
    }
}
