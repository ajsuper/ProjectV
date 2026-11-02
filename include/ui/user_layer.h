#ifndef PROJV_UI_USER_LAYER_H
#define PROJV_UI_USER_LAYER_H

#include <memory>
#include <string>

#include "ui/ui.h"

// The user layer as the host holds it: an RmlUi context fed from Input, drawn in USER_VIEW. A program
// does not make one -- ui::installUi does, and ui/user.h is how a program uses it -- but a test can,
// headless. No RmlUi type appears here (D2); its insides are in src/ui/user/.
//
// Plan: docs/plans/ui.md (D5-D7, D11).
namespace projv::ui::user {
    class UserLayer : public Layer {
    public:
        // `renderInstance` nullptr is headless: a recording render interface (no GPU), a 1280x720
        // display. For tests (D11).
        UserLayer(Application& app, graphics::RenderInstance* renderInstance, bool reloadOnChange);
        ~UserLayer() override;
        UserLayer(const UserLayer&) = delete;
        UserLayer& operator=(const UserLayer&) = delete;

        // Whether any document is shown.
        bool visible() const override;
        Capture input(const Input& input, Capture taken) override;
        void draw(bgfx::ViewId view, core::ivec2 size) override;

        // For tests (D11): what the headless render interface drew in the last draw (its geometry
        // calls); an element's text, by document and id, as laid out ("" if there is none); and its
        // border box in window pixels (false if there is none).
        int drawCallsRecorded() const;
        std::string elementText(const std::string& document, const std::string& id) const;
        bool elementBox(const std::string& document, const std::string& id, float& x, float& y, float& width, float& height) const;

        struct Impl;
        Impl& impl() { return *state; }
        const Impl& impl() const { return *state; }

    private:
        std::unique_ptr<Impl> state;
    };
}

#endif
