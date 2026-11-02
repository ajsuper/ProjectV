# Add a dev panel

A window of your own in the dev layer: stats to watch, values to tweak. Header:
[`ui/dev.h`](../../include/ui/dev.h). Concepts: [The UI](../concepts/ui.md).

## Install the UI

```cpp
#include "ui/ui.h"

int main(int argc, char** argv) {
    projv::ui::setCommandLine(argc, argv);   // --dev-ui, for release builds
    ...
}

void startup(Application& app) {
    ...
    graphics::installPlatform(app, window);
    ui::installUi(app, window);              // F1 shows the dev layer
}
```

## Add the panel

```cpp
#include "ui/dev.h"

ui::dev::addPanel(app, "Tuning", [](Application& app) {
    auto& tuning = app.world.ctx().get<Tuning>();
    ImGui::SliderFloat("blast radius", &tuning.radius, 1.0f, 20.0f);
    ImGui::Text("%d bodies", int(app.world.view<Body>().size()));
});
```

- **The body of an ImGui window** named after the panel, built every frame the layer is shown, in
  the Render stage. Any ImGui widget works: `imgui.h` comes with `ui/dev.h`.
- **Hidden, it costs nothing.** A build or a run without the dev layer registers the panel and never
  calls it.
- **Changes apply next frame**, from Render: fine for tuning, not for anything a step must agree on
  across machines (send a command instead).

## When it is on

| Build | Dev layer |
|---|---|
| `dev` preset | installed; F1 shows it |
| `release` preset (`PROJV_DEV_UI_DEFAULT=OFF`) | only with `--dev-ui` |
| `UiOptions{.devLayer = false}` | never |
| `PROJV_DEV_UI_SHOWN=1` | starts shown (captures, unattended runs) |

The engine's own panels, always there: **Frame** (CPU and GPU time, stacked graphs by stage and by
render pass, each with its average, min and max) and **Log** (the last 4000 lines, by category,
filterable). [Reference](../reference/ui.md#uidev_layerh).

## Let the Renderer panel edit a value

The Renderer panel shows every uniform read-only. To make one editable, bind the variable the program
writes it from -- never the uniform itself, which the program overwrites each frame:

```cpp
// once, when the renderer is built; g_fast outlives it
ui::dev::editable(app, *renderer, "fastSettings", &g_fast.aoRadius, {.label = "AO radius", .min = 0.5f, .max = 16.0f});
ui::dev::editable(app, *renderer, "jitterParams", &g_fast.taa, {.label = "TAA"});
```

An edit changes the program's variable, so anything the program derives from it (a history reset
when the light moves) stays right. `forgetEditables(app, *renderer)` before the renderer or the
variables go.

## A reading that holds still

A number redrawn every frame flickers past reading. `Meter` averages it over a period and keeps the
period's least and greatest:

```cpp
static ui::dev::Meter step;   // 0.5 s; setPeriod to change
step.add(stepMs, app.time().unscaledDelta);
if (step.ready()) ImGui::Text("step %.2f ms  (%.2f .. %.2f)", step.average(), step.min(), step.max());
```
