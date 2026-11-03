#ifndef PROJV_UI_USER_H
#define PROJV_UI_USER_H

#include <string>
#include <vector>

// The user layer: what the player sees -- HUDs, menus, prompts -- as RmlUi documents (.rml, .rcss)
// beside the program, drawn over the frame under the dev layer. No RmlUi type appears here (D2).
//
//     ui::user::loadFont(app, "ui/fonts/LatoLatin-Regular.ttf");
//     ui::user::model(app, "hud").bind("score", &score).bind("health", &health);   // before opening
//     ui::user::open(app, "hud", "ui/hud.rml");
//
//     <body data-model="hud"> <p>Score {{score}}</p> <button onclick="pause">Pause</button> </body>
//
//     app.events().on<ui::UiAction>([](const ui::UiAction& a) { if (a.name == "pause") ...; });
//
// - **Values in, through a model.** A bound variable is the program's; the layer reads it each frame
//   and redraws what shows it when it changes. A control bound to one (a checkbox, a slider) writes it.
// - **Actions out, as events.** `on<event>="name arg arg"` on any element sends UiAction{document,
//   name, args, event} when that event happens -- `onclick`, `onchange`, `onsubmit`. Handled in
//   Update like any other event: the UI never runs inside a step (D7).
// - **Input it takes.** The mouse while it is over an element that takes pointer events (a HUD's
//   body sets `pointer-events: none` to let the game have the rest), the keyboard while a text field
//   has focus, and everything while a `modal` document is shown (a pause menu).
// - **Fades are style.** A shown document's body has the class `ui-shown`, added the frame after it is
//   shown and removed when it is hidden, so a style sheet can transition between the two:
//   `body { opacity: 0; transition: opacity 0.3s quadratic-out; } body.ui-shown { opacity: 1; }`.
//   DocumentOptions::fadeOut keeps it drawn while it fades out. A fading document is not modal; one
//   that sets `pointer-events: none` without ui-shown ignores the mouse too.
// - **Edits apply live.** In a dev build a document is reloaded when its .rml, or an .rcss beside
//   it, changes on disk; what it showed comes back as it was.
//
// Needs ui::installUi. Plan: docs/plans/ui.md (D5-D7, U3).
namespace projv {
    class Application;
}

namespace projv::ui {
    // What an element's `on<event>` attribute sends. `onclick="buy sword 2"` on a button in the
    // document opened as "shop" sends {"shop", "buy", {"sword", "2"}, "click"}.
    struct UiAction {
        std::string document;
        std::string name;
        std::vector<std::string> args;
        std::string event;
    };
}

namespace projv::ui::user {
    struct DocumentOptions {
        bool shown = true;
        // While shown, it takes all of the mouse and keyboard, over an element or not: a pause menu.
        bool modal = false;
        // Seconds it is still drawn after hide, for its style sheet's transition out. 0: gone at once.
        float fadeOut = 0.0f;
    };

    // Loads an .rml document under `name`, which UiAction::document carries and the calls below take.
    // Paths inside it (style sheets, images, templates) are relative to it. A name already open is
    // closed first. False (logged) if it cannot be loaded.
    bool open(Application& app, const std::string& name, const std::string& path, DocumentOptions options = {});
    // The same, from RML text: generated UI, and tests. Relative paths resolve against the working
    // directory.
    bool openFromMemory(Application& app, const std::string& name, const std::string& rml, DocumentOptions options = {});
    void close(Application& app, const std::string& name);
    void show(Application& app, const std::string& name);
    void hide(Application& app, const std::string& name);
    bool isOpen(const Application& app, const std::string& name);
    bool isShown(const Application& app, const std::string& name);

    // How many pixels one `dp` is in every document: 1 unless set. A program can follow the display's
    // scale with it or, for pixel art, keep it a whole number so 1dp is whole pixels. Images whose name
    // ends ".pixel" ("pickaxe.pixel.png") are sampled nearest, so they stay crisp drawn larger.
    void setDpRatio(Application& app, float ratio);
    float dpRatio(const Application& app);

    // A font face for every document (TTF or OTF). `fallback`: used for characters the others lack.
    bool loadFont(Application& app, const std::string& path, bool fallback = false);

    // A data model: the values documents show with {{name}} and data-* attributes. Make it, and bind
    // its values, before opening a document that names it (data-model="hud"). Variables are the
    // program's and must outlive the model; it lasts until the UI does.
    class Model {
    public:
        Model& bind(const std::string& name, float* value);
        Model& bind(const std::string& name, int* value);
        Model& bind(const std::string& name, bool* value);
        Model& bind(const std::string& name, std::string* value);
        // Lists (D21): `data-for="item : name"`, and rows of text, `{{row[0]}}`. Compared each frame.
        Model& bind(const std::string& name, std::vector<std::string>* value);
        Model& bind(const std::string& name, std::vector<std::vector<std::string>>* value);
        // Bound values are compared each frame, so this is only for a change the comparison cannot see:
        // there is none for these types; it is here for types to come.
        void dirty(const std::string& variable);
        bool valid() const { return app != nullptr; }

    private:
        friend Model model(Application& app, const std::string& name);
        Model(Application* app, std::string name) : app(app), name(std::move(name)) {}
        Application* app;
        std::string name;
    };

    // The model `name`, made on first use. An invalid Model (logged) without the user layer.
    Model model(Application& app, const std::string& name);
}

#endif
