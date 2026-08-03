// ProjectV Scene Editor — Phase 1: the dockable shell around a live scene view.
//
// This is the first slice of the editor/modelling tool: a window whose interface is made of ImGui
// dock panels, one of which is the scene rendered live by the engine, and a File menu that loads any
// Compose scene folder off disk at runtime. Nothing here edits voxels yet — that is the next slice,
// and the engine already has the pieces for it (utils::queueVoxelAdd / updateScene /
// flushSceneUpdates). What it establishes is the frame those tools will hang off:
//
//   * one bgfx frame carries both the scene and the interface. The scene's render passes draw into
//     an offscreen texture (the editor's renderer targets FBO 3 where the previewer targeted the
//     back buffer), and ImGui draws that texture as an image inside the Viewport panel. That is what
//     lets the scene live in a dock node that the user can move, resize, and tab like any other.
//   * the scene is a runtime, replaceable resource rather than something loaded once at startup, so
//     File ▸ Load Scene… tears down the GPU state and uploads a different scene without a restart.
//
// The renderer itself is the ScenePreviewer's: one primary ray per pixel, pure albedo, no lighting.
// It is the right viewport renderer to start from because it shows what is actually *in* the scene
// (a material that reads wrong is wrong in the data) and it costs one ray per pixel, which leaves
// the frame budget for interface and, later, editing.
//
// Usage:
//   ./scene_editor [scene-directory]
//
// With no argument it opens the previewer's bundled StonehillCastle if it is there, so the editor
// starts on something rather than a black panel. Either way File ▸ Load Scene… switches scenes.
//
// Controls:
//   Right-mouse drag in Viewport — fly the camera (cursor is captured for the duration)
//   W/S, A/D, R/F               — forward/back, strafe, up/down (while flying)
//   Scroll wheel                — movement speed (while flying or hovering the Viewport)
//   H                           — re-frame the camera on the scene
//
// Panels dock, tear off, and tab by dragging their title bars; the layout is saved to imgui.ini next
// to the executable and restored on the next run. View ▸ Reset Layout puts it back.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/ecs.h"
#include "core/math.h"
#include "core/log.h"
#include "graphics/render_instance.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/type_mapping.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/picking.h"
#include "utils/scene_query.h"
#include "utils/voxel_math.h"

#include <glm/gtc/quaternion.hpp>

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder — building the default dock layout is not in the public API.
#include "imgui_impl_glfw.h"
#include "imgui_impl_bgfx.h"
#include "edit_history.h"

// The interface is drawn into a bgfx view above every scene pass. bgfx submits views in ascending ID
// order, and the scene renderer takes 0..N-1 (one per render pass), so any comfortably higher number
// puts the interface on top.
static constexpr bgfx::ViewId EDITOR_IMGUI_VIEW_ID = 200;

// Where the file browser starts, and what is opened when the editor is run without an argument. The
// previewer's scene folder is shared rather than copied — the .data files are tens of megabytes.
static const char* DEFAULT_SCENE_DIRECTORY = "../ScenePreviewer/scenes/";
static const char* DEFAULT_SCENE_PATH = "../ScenePreviewer/scenes/StonehillCastle/";

// Mouse wheel. GLFW scroll callbacks are plain C function pointers, so the accumulated offset lives
// at file scope. This callback is installed *before* ImGui's, so ImGui chains into it and both see
// every scroll event; the editor then decides per frame whether the scroll was meant for the camera
// or for whatever panel is under the cursor.
static double g_scrollOffsetThisFrame = 0.0;
static void scrollCallback(GLFWwindow* /*window*/, double /*xoffset*/, double yoffset) {
    g_scrollOffsetThisFrame += yoffset;
}

// =============================================================================
// Editor state
// =============================================================================

// How a scene is framed on open, and the movement scale that goes with it. Both are derived from the
// scene's own size so the controls feel the same whether the subject is a tree or a city.
struct CameraFraming {
    projv::core::vec3 position;
    float yaw;          // Radians; matches the cameraYaw convention below.
    float pitch;
    float moveSpeed;    // World units per frame at the default scroll setting.
};

// =============================================================================
// Tools
// =============================================================================
//
// The active tool is what a left-click inside the Viewport means. Without it every new interaction
// would have to negotiate for the same button: the gizmo already wants a drag, picking wants a
// click, and a sculpt stroke wants a drag over the same pixels the gizmo is drawn on. A mode makes
// that a choice the user states once rather than a modifier they have to remember per action, and it
// is what lets the right-hand Tool panel show one tool's settings instead of every tool's at once.
//
// Ctrl is part of the shortcut because the bare letters are already spoken for: W/A/S/D/R/F fly the
// camera while the right button is held, and taking Q/W/E/R unqualified would mean a fly-through
// that silently changed tool on the way past.
enum class EditorTool {
    Select,   // Ctrl+Q — click a voxel to select the component that owns it.
    Move,     // Ctrl+W — the transform gizmo, plus selection.
    Sculpt,   // Ctrl+E — add and remove voxels with a brush.
    Paint     // Ctrl+R — recolour voxels with the palette's current entry.
};

static constexpr int EDITOR_TOOL_COUNT = 4;

static const char* editorToolLabel(EditorTool tool) {
    switch (tool) {
        case EditorTool::Select: return "Select";
        case EditorTool::Move:   return "Move";
        case EditorTool::Sculpt: return "Sculpt";
        case EditorTool::Paint:  return "Paint";
    }
    return "?";
}

static const char* editorToolShortcut(EditorTool tool) {
    switch (tool) {
        case EditorTool::Select: return "Ctrl+Q";
        case EditorTool::Move:   return "Ctrl+W";
        case EditorTool::Sculpt: return "Ctrl+E";
        case EditorTool::Paint:  return "Ctrl+R";
    }
    return "";
}

// One line, shown under the tool's name in the Tool panel and in its toolbar tooltip: what the left
// mouse button does right now. Every tool answers that question in the same place.
static const char* editorToolHint(EditorTool tool) {
    switch (tool) {
        case EditorTool::Select: return "Click a voxel to select its component.";
        case EditorTool::Move:   return "Drag a gizmo handle to transform; click a voxel to select.";
        case EditorTool::Sculpt: return "Drag to add or remove voxels. Alt+click samples a material.";
        case EditorTool::Paint:  return "Drag to recolour voxels. Alt+click samples an entry.";
    }
    return "";
}

// What the ray cast after the interface is built is being cast *for*. The click is noticed while the
// Viewport panel is laid out but the scene cannot be touched there, so the intent is parked here and
// acted on between the panels — see processVoxelPick.
enum class PickPurpose {
    None,
    SelectComponent,
    SampleMaterial,
    PaintVoxel,
    SculptVoxel   // Re-armed every frame of a drag, not once per click — see the sculpt stroke below.
};

// =============================================================================
// Sculpt brushes
// =============================================================================
//
// What a single dab of the brush covers. Unlike Paint, which only ever recolours voxels that already
// exist, all three of these create and destroy geometry -- that is the whole distinction between the
// two tools, and the reason a component can grow past its current bounds while sculpting (a loose
// Chunk becomes a Grid, and a Grid expands; see applyComponentQueue).
enum class SculptBrush {
    // --- Brushes that place a shape ---
    Sphere,    // A ball of voxels centred on the placement cell.
    Cube,      // A box of voxels centred on it, axis-aligned in the component's voxel space.

    // --- Brushes that reshape what is already there ---
    //
    // These two do not stamp a shape; they run an operator over the surface inside the brush, once per
    // tick, for as long as the button is held. So they keep working with the cursor standing still --
    // which is the point of both -- and they are paced by the clock rather than by mouse movement.
    // Both take their colour from the geometry they are reshaping rather than from the palette.
    Smooth,    // Rounds the surface off. No Add/Remove: there is only one direction for smoother.
    Bump,      // Pushes the surface out (Add) or in (Remove), following its shape.

    // Not a brush at all, and it does not run along a drag the way the others do: it selects the whole
    // face you click on and moves that face. See the Extrude section below.
    Extrude
};

enum class SculptMode {
    Add,
    Remove
};

static constexpr int SCULPT_BRUSH_COUNT = 5;

static const char* SculptBrushLabel(SculptBrush brush) {
    switch (brush) {
        case SculptBrush::Sphere:  return "Sphere";
        case SculptBrush::Cube:    return "Cube";
        case SculptBrush::Smooth:  return "Smooth";
        case SculptBrush::Bump:    return "Bump";
        case SculptBrush::Extrude: return "Extrude";
    }
    return "?";
}

static const char* SculptBrushHint(SculptBrush brush) {
    switch (brush) {
        case SculptBrush::Sphere:  return "A ball of voxels centred on the cell under the cursor.";
        case SculptBrush::Cube:    return "A box of voxels centred on it, aligned to the component's axes.";
        case SculptBrush::Smooth:  return "Rounds the surface off. Hold to keep smoothing.";
        case SculptBrush::Bump:    return "Pushes the surface out or in, following its shape.";
        case SculptBrush::Extrude: return "Drags the whole face you click on outward or into the shape.";
    }
    return "";
}

// Whether the brush runs an operator on a tick rather than stamping a shape -- see the enum. These
// keep working while the cursor is still, take no palette colour, and are paced by the clock.
static bool sculptBrushIsIterative(SculptBrush brush) {
    return brush == SculptBrush::Smooth || brush == SculptBrush::Bump;
}

// Bump's two directions are the same choice as Add/Remove and share the enum, but "Add a bump" is not
// what the tool does -- it moves a surface that is already there. Naming them for what they do to that
// surface is worth the one indirection.
static const char* SculptModeLabel(SculptMode mode, SculptBrush brush) {
    if (brush == SculptBrush::Bump) {
        return mode == SculptMode::Add ? "Pull" : "Push";
    }
    return mode == SculptMode::Add ? "Add" : "Remove";
}

// =============================================================================
// Selection scope
// =============================================================================
//
// Both of the tools that spread a selection outward from the voxel you clicked -- the Extrude face
// gather and the Paint fill -- answer the same question, so they answer it the same way: does a
// change of material stop the spread?
//
//   Material    stops at one. This is the tool picking out the brick in a brick-and-mortar wall.
//   Everything  ignores it and is bounded only by the geometry -- the whole face, the whole shape.
//
// It has to be a choice rather than a default, because on a surface of one material the two are
// identical and on one built from several neither answer is more obviously right. It also depends
// enormously on how the scene was made, which is not something either tool can infer: a flat-coloured
// model has large single-material regions, while a photo-textured voxelisation gives nearly every
// voxel its own palette entry, and there Material picks out a handful of voxels and Everything is the
// only useful setting.
//
// What each tool spreads *over* still differs, and so do their labels -- a face for Extrude, a
// connected body for Fill -- which is why the second label is supplied at the call site.
enum class SelectionScope {
    Material,
    Everything
};

static constexpr int SELECTION_SCOPE_COUNT = 2;

// Ceilings on the brush, for the same reason the paint brush has them: every candidate coordinate
// costs a tree64 descent to learn whether a voxel is there, and the *scanned box* -- not the placed
// count -- is what sets the cost. A dab happens every frame of a drag rather than once per click, so
// the ceiling here is tighter than Paint's: radius 24 is a 49^3 box, ~118k descents, which is about
// as much as belongs inside a frame that also has to rebuild the chunk and re-upload it.
static constexpr float SCULPT_MAX_RADIUS = 24.0f;
static constexpr float SCULPT_MAX_CUBE_SIDE = 48.0f;

// How far apart two consecutive dabs of a stroke may be, in voxels, before the gap between them is
// filled in with dabs of its own -- and how many of those one frame will pay for.
//
// A drag is sampled once per frame, so how far the cursor travels between two dabs depends on the
// frame rate and on how fast the user is moving the mouse. Without interpolation a quick flick lays
// down a dotted line of disconnected blobs. Stepping along the segment at half the brush's radius
// keeps consecutive dabs overlapping, which is what makes a stroke read as one continuous ridge.
//
// The cap is what a fast flick across a large scene degrades into: past 48 steps the stroke thins out
// rather than the frame stalling on thousands of dabs. It is a deliberate trade -- a dropped frame
// mid-stroke is far more disruptive than a slightly sparse one.
static constexpr int SCULPT_MAX_INTERPOLATED_STEPS = 48;

// How often Smooth and Bump take a step, in seconds.
//
// These run for as long as the button is held, so the frame rate must not be what decides how fast
// they work -- at 60 fps an unthrottled Bump would push the surface out sixty layers in a second, and
// the same edit would land differently on a slower machine. A tick is a fixed unit of effect: ten
// smoothing passes or ten layers of bump per second, whatever the frame rate, so holding for "about a
// second" means the same thing everywhere. It is also the granularity of the brush -- there is no
// point running a pass that changes nothing between two frames 16 ms apart.
static constexpr double SCULPT_ITERATION_SECONDS = 0.1;

// Smooth's rule, over the 3x3x3 neighbourhood including the cell itself: solid if strictly more than
// this many of the 27 are solid. Thirteen is the majority, and a majority filter is exactly a
// smoothing operator on a voxel grid -- it is stable on a flat surface (a surface cell sees 18 solid
// of 27 and stays; the cell above it sees 9 and stays empty), shaves anything that protrudes, and
// fills anything that is dented in. Rounding convex edges and filling concave ones is what "smoother"
// means here, and it falls out of the same test rather than needing two.
static constexpr int SCULPT_SMOOTH_SOLID_THRESHOLD = 13;

// Smooth's strength, and what it means on a grid where a cell cannot move half a voxel.
//
// Everywhere else "strength" is how far each point slides toward the smoothed result, which needs a
// continuous surface to slide along. Here a cell is solid or it is not, so the only thing a strength
// can choose is *which* cells the filter is allowed to flip -- and the honest ordering is by how
// badly each one disagrees with its own neighbourhood. Distance from the threshold is exactly that
// number, and it is symmetric: a solid cell flips at 13 neighbours or fewer, an empty one at 14 or
// more, so both sides run from 1 (a hair over the line) to 13 (an isolated voxel, or a hole with
// solid on every side). A cutoff on it is the discrete form of the same idea -- the weaker the
// setting, the more out of place a cell has to be before the brush touches it.
//
// What the levels come out as on real geometry, since the number is meaningless without them:
//
//   1  Everything the majority filter says. Shaves edges and rounds corners -- the aggressive end,
//      and what the brush did before this setting existed.
//   2  Spares a clean 90-degree edge (about 2), which is the first thing users notice being eaten.
//   4  Spares staircase steps and gentle curvature; still fills a one-voxel pit and shaves a
//      one-voxel spike (both about 4).
//   6  Noise only: an isolated voxel (13) and a fully enclosed hole (13), and very little else.
//
// Six is where the top of the useful range sits, so that is where the weakest setting maps. Past it
// the control would spend most of its travel doing nothing new.
static constexpr int SCULPT_SMOOTH_MAX_CUTOFF = 6;

// Past 1 the cutoff has nothing left to give -- at 1 the filter already flips every cell it can -- so
// the strength widens the *neighbourhood* instead, and that is a different operator rather than more
// of the same one.
//
// The distinction is the whole reason the scale goes past 1. Holding the button reruns the 3x3x3
// filter, and a filter run twice on its own output eventually stops changing anything: a two-layer
// slab, a large sphere, a shallow dome spread over a dozen voxels are all **fixed points** of it, and
// no amount of holding will touch them, because at the 1-voxel scale they are already flat. Widening
// the kernel is what sees them. A 5x5x5 majority (63 of 125) and a 7x7x7 one (172 of 343) ask the
// same question over a larger piece of the surface, so they smooth features the small kernel cannot
// perceive at all.
//
// Two consequences, both worth knowing before reaching for it:
//
//   * **A body thinner than the kernel dissolves rather than smooths.** A two-layer slab shows the
//     5x5x5 filter 50 solid of 125 where it wants 63, so the whole slab goes. That is not a bug in
//     the operator, it is what "smooth at a five-voxel scale" means for something two voxels thick --
//     but voxelised meshes are usually hollow shells a few voxels thick, so a wide setting aimed at
//     one takes it out. The Tool panel says so where the setting is.
//   * **It costs the kernel's volume per cell.** 343 array reads against 27 is a real multiple, which
//     is why the colour vote below stays on the inner 3x3x3 and runs only for cells actually being
//     filled. See the timing in runSculptOperatorSelfTest.
static constexpr int   SCULPT_SMOOTH_MAX_KERNEL = 3;    // Radius, so 7x7x7 at the top.
static constexpr float SCULPT_MAX_SMOOTH_STRENGTH = float(SCULPT_SMOOTH_MAX_KERNEL);

// The strength slider as the two numbers the operator actually takes.
//
// Below 1 the kernel is the usual 3x3x3 and the strength is a floor on how far out of place a cell
// has to be; at and above 1 the cutoff is 1 (everything the filter says) and each whole step widens
// the kernel by one. Full strength -- 1.00 -- is 3x3x3 with cutoff 1, which is what the brush did
// before either half of this setting existed, so the default still changes nothing.
static int sculptSmoothKernelRadius(float strength) {
    if (strength <= 1.0f) return 1;
    float clamped = std::min(strength, SCULPT_MAX_SMOOTH_STRENGTH);
    return std::min(1 + int(std::ceil(clamped - 1.0f)), SCULPT_SMOOTH_MAX_KERNEL);
}

static int sculptSmoothCutoff(float strength) {
    if (strength >= 1.0f) return 1;
    float clamped = std::max(strength, 0.0f);
    return 1 + int(std::lround((1.0f - clamped) * float(SCULPT_SMOOTH_MAX_CUTOFF - 1)));
}

// The majority for a kernel of this radius: solid if strictly more than half its cells are. Radius 1
// gives SCULPT_SMOOTH_SOLID_THRESHOLD, which is where the rule and its justification are written out.
static int sculptSmoothThreshold(int kernelRadius) {
    int side = 2 * kernelRadius + 1;
    return (side * side * side) / 2;
}

// Bump's blend: how many smoothing passes follow each push or pull, and how far past the brush they
// reach.
//
// Dilating everything inside a sphere gives a disc with a cliff around it -- the bump is a layer
// thick in the middle and a layer thick right up to the brush's edge, where it stops dead against
// untouched surface. That edge is what makes an unblended bump read as "a sphere was stamped here"
// rather than as the surface being pushed. Smoothing the result is what turns the cliff into a
// shoulder, and it has to reach *past* the brush to do it, because the join it is rounding off is
// exactly at the brush's rim -- hence the margin.
static constexpr float SCULPT_MAX_BLEND = 4.0f;
static constexpr float SCULPT_BLEND_MARGIN = 2.0f;

// Extrude's two ceilings. The face is gathered by a flood fill with no bound of its own -- a terrain's
// ground plane is one enormous face of one material -- and the voxels it moves are the face times the
// depth, so the two multiply. A million-voxel face pulled out sixty-four layers is not an edit anyone
// meant to make, and the depth cap is what keeps a single flick of the mouse from being one.
static constexpr size_t EXTRUDE_MAX_FACE_VOXELS = 1000000;
static constexpr int    EXTRUDE_MAX_DEPTH = 64;

// =============================================================================
// Paint shapes
// =============================================================================
//
// What a single Paint click covers. All four recolour voxels that are already there and never create
// one -- adding geometry is the Sculpt tool's job, and a paint brush that quietly filled the empty
// space it passed over would be the most surprising tool in the editor.
enum class PaintShape {
    Voxel,       // Just the one under the cursor.
    Sphere,      // Every solid voxel within a radius of it.
    Cube,        // Every solid voxel in a box centred on it.
    // The two floods, and they are kept apart deliberately -- the difference between them is the
    // difference between recolouring a wall and recolouring the building.
    //
    // A volume fill spreads in three dimensions through anything it touches, so it reaches the
    // *inside* of a shape as readily as the outside, and it does not stop at a corner the way a
    // person looking at the surface expects. Combined with the Everything scope that is a single
    // click that repaints an entire connected model, which is occasionally what is wanted and never
    // what is wanted by accident. A face fill stays on the surface you pointed at.
    FillFace,    // Across the one surface: same plane, face exposed, 4-connected.
    FillVolume   // Through the solid: 6-connected in three dimensions.
};

static constexpr int PAINT_SHAPE_COUNT = 5;

static const char* PaintShapeLabel(PaintShape shape) {
    switch (shape) {
        case PaintShape::Voxel:      return "Voxel";
        case PaintShape::Sphere:     return "Sphere";
        case PaintShape::Cube:       return "Cube";
        case PaintShape::FillFace:   return "Fill face";
        case PaintShape::FillVolume: return "Fill volume";
    }
    return "?";
}

static const char* PaintShapeHint(PaintShape shape) {
    switch (shape) {
        case PaintShape::Voxel:      return "Recolours the single voxel under the cursor.";
        case PaintShape::Sphere:     return "Recolours every solid voxel within the radius.";
        case PaintShape::Cube:       return "Recolours every solid voxel in a box around it.";
        case PaintShape::FillFace:   return "Spreads across the surface you clicked, staying on it.";
        case PaintShape::FillVolume: return "Spreads through the solid in every direction, inside included.";
    }
    return "";
}

// Ceilings on the brush settings, and on how far a flood fill will spread.
//
// Both are about a click staying a click. Every candidate coordinate costs a tree64 descent to find
// out whether a voxel is there at all, so the scanned box -- not the painted count -- is what sets
// the cost: radius 32 is a 65^3 box, a quarter of a million descents, which is a perceptible pause
// and about as much as belongs on the end of a mouse button.
//
// The fill limit is a different worry: a fill has no bound of its own, and one started on a terrain's
// ground material would otherwise walk the entire component. It counts voxels *of the region* -- the
// ones a fill would paint -- and not coordinates looked at, which is roughly seven times larger and
// is what an earlier version measured, cutting every large fill off at a seventh of its budget. Four
// million is comfortably past a terrain's ground layer and still a fraction of a second, now that the
// visited set is a bitset rather than a hash of packed coordinates.
static constexpr float PAINT_MAX_RADIUS = 32.0f;
static constexpr int   PAINT_MAX_CUBE_SIDE = 64;
static constexpr size_t PAINT_FILL_LIMIT = 4000000;

// How many dabs one frame of a paint drag will pay for, filling in the gap the cursor crossed since
// the last frame. Same reasoning as the sculpt brush's cap, and the same trade: past this the stroke
// thins out rather than the frame stalling. Lower than sculpt's, because a paint dab is the more
// expensive of the two at equal size -- the ceiling on the paint radius is a 65^3 box against the
// brush's 49^3, and a stroke that is a few voxels sparse is far less noticeable on a recolour than on
// geometry, where it leaves a visible hole.
static constexpr int PAINT_MAX_INTERPOLATED_STEPS = 32;

// Whether a shape spreads on its own rather than covering a fixed neighbourhood. The two fills do,
// and it is what keeps them out of a drag: they are click operations, like a paint bucket everywhere
// else, and re-running one every frame would re-walk a region of up to PAINT_FILL_LIMIT voxels to
// discover that the previous frame already painted all of it.
static bool paintShapeIsFill(PaintShape shape) {
    return shape == PaintShape::FillFace || shape == PaintShape::FillVolume;
}

// Everything the editor owns that is neither the Scene nor its GPU mirror. One global resource, so
// the ECS stages can reach it without file-scope statics.
struct EditorState {
    // --- Loaded scene ---
    std::string scenePath;
    bool sceneLoaded = false;

    // --- Camera ---
    CameraFraming framing;
    projv::core::vec3 cameraPosition = projv::core::vec3(0.0f);
    float cameraYaw = 0.0f;
    float cameraPitch = 0.0f;
    float speedScrollSteps = 0.0f;      // Scroll notches; applied geometrically to framing.moveSpeed.
    bool cameraIsFlying = false;        // Right mouse held inside the viewport: cursor captured.
    bool cameraMovedByInterface = false; // A panel or menu moved the camera; consumed by the render
                                         // loop, which has to drop the accumulated image when it did.
    double cursorAnchorX = 0.0;         // Where the cursor was when the fly-through started, so it
    double cursorAnchorY = 0.0;         // can be put back when the button is released.
    double lastCursorX = 0.0;
    double lastCursorY = 0.0;
    int frameCameraLastMovedOn = 0;     // Drives the accumulate pass's reset.

    // --- Viewport panel ---
    // The size the panel had when it was last laid out, in framebuffer pixels. The render targets are
    // resized to match at the top of the *next* frame — see the note in renderEditor().
    int viewportWidth = 1280;
    int viewportHeight = 720;
    int requestedViewportWidth = 1280;
    int requestedViewportHeight = 720;
    bool viewportHovered = false;

    // --- Render settings ---
    // The viewport's readability toggles, driven by the icon bar along the bottom of the scene image
    // and read by shade.frag. Both are aids for judging *shape*; the renderer underneath them shows
    // stored albedo with no lighting, and with both off that is exactly what reaches the screen.
    //
    // Normal shading is on by default and ambient occlusion is not: the first is free (one dot
    // product per pixel) and makes an otherwise flat-looking scene legible, the second costs 16
    // texture taps per pixel and is grainy until the accumulation settles, which is a surprising
    // thing to meet before you have asked for it.
    bool ambientOcclusionEnabled = false;
    bool normalShadingEnabled = true;
    // A toggle invalidates the accumulated image the same way a camera move does — without this the
    // new setting fades in over the history's 64 frames instead of appearing.
    bool renderSettingsChanged = false;

    // --- Interface ---
    bool dockLayoutBuilt = false;
    bool resetDockLayoutRequested = false;
    bool loadSceneDialogOpen = false;
    std::string browserDirectory = DEFAULT_SCENE_DIRECTORY;
    std::string browserSelection;       // Folder highlighted in the browser, if any.
    char browserPathBuffer[1024] = {};  // The editable path field. Owned by the state, not rebuilt
                                        // per frame, so what the user types survives to the next one.
    std::string pendingScenePath;       // Set by the dialog, consumed once the interface is built.
    // Frames still to wait after releasing the old scene's GPU data before building the new one.
    // Zero means "not mid-load"; see the two-phase load in render().
    int sceneTeardownFramesRemaining = 0;
    std::string statusMessage;
    bool statisticsWindowOpen = false;   // The detailed numbers; the status bar carries the summary.

    // --- Status toast ---
    // statusMessage is written from twenty-odd places, so rather than have each of them also stamp a
    // time, the viewport notices the string has changed and starts the fade itself. A message that is
    // set twice with the same text does not re-show, which is the right answer for the repeated ones
    // ("Nothing under the cursor.") -- and the status bar is still carrying it regardless.
    std::string toastMessage;
    double toastShownAt = 0.0;

    // --- Tools ---
    EditorTool activeTool = EditorTool::Select;

    // --- Library ---
    // The left column's persistent disk browser. Separate from the modal Load Scene dialog's
    // browserDirectory: the dialog is opened at the scene that is loaded, while the library is a
    // place the user navigates to and expects to still be there next time they look.
    std::string libraryDirectory = DEFAULT_SCENE_DIRECTORY;
    std::string librarySelection;         // Absolute path of the highlighted folder, if any.
    char libraryPathBuffer[1024] = {};
    // The compose.json of librarySelection, parsed but NOT loaded -- no geometry is read and nothing
    // reaches the GPU. Re-parsed only when the selection changes.
    std::string libraryPreviewPath;
    projv::ComposeDoc libraryPreview;
    bool libraryPreviewValid = false;

    // --- Selection ---
    projv::ComponentHandle selectedComponent = projv::INVALID_COMPONENT_HANDLE;
    projv::ComponentHandle lastHierarchySelection = projv::INVALID_COMPONENT_HANDLE;
    uint32_t selectedVoxelCount = 0;    // Cached: counting walks the component's geometry.
    // The .data leaves (Chunk/Grid) reachable from the selection -- itself if it is one, every leaf
    // of its subtree if it is an Asset folder. What the yellow viewport outline is drawn around, and
    // what the Inspector's "Boxes" count reports.
    std::vector<projv::ChunkHandle> selectionOutlineChunks;
    bool selectionOutlineValid = false;
    // Bumped every time the leaf set above is rebuilt. Anything caching a derived quantity (the
    // Inspector's pivot) compares against its own copy rather than watching selectionOutlineValid
    // go false -- the viewport draws first and always wins the race to rebuild, so by the time a
    // panel further down the frame looks, the flag is already back to true.
    uint32_t selectionOutlineGeneration = 0;

    // --- Inspector: transform editing ---
    // A quaternion has no natural "drag this number" representation, so rotation is edited as Euler
    // degrees and converted on every change. The buffer is refreshed from the component's quaternion
    // only when the selection changes (lastInspectorSelection stale) -- recomputing it every frame
    // would fight the user's own drag, and at gimbal lock there is more than one valid Euler triple
    // for the same quaternion, so a fresh reconstruction can visibly jump.
    projv::ComponentHandle lastInspectorSelection = projv::INVALID_COMPONENT_HANDLE;
    float inspectorEulerDegrees[3] = {0.0f, 0.0f, 0.0f};

    // Centre of the selection's bounding box, expressed in the component's own untransformed local
    // space -- the point the rotation and scale drags pivot about when pivotAtCenter is set. Local
    // rather than world so it is invariant to the very transform being edited: recomputed only when
    // the selection or its leaf set changes, never mid-drag. See computeLocalPivot.
    projv::core::vec3 inspectorPivotLocal = projv::core::vec3(0.0f);
    bool inspectorPivotValid = false;
    uint32_t inspectorPivotGeneration = 0xFFFFFFFFu;   // Never equals a real generation, so first use computes.
    bool pivotAtCenter = true;    // Off = the raw transform, pivoting about the local origin.

    // --- Viewport transform gizmo ---
    // Handle indices: 0/1/2 are the translate arrows along world X/Y/Z, 3/4/5 the rotate rings about
    // the same three axes. -1 is none.
    // The gizmo belongs to the Move tool. It used to appear whenever the Inspector was the selected
    // tab, which was the best signal available while Inspector/Palette/Sculpt shared one dock node --
    // now that they are stacked and all three are visible at once, that signal is gone and the tool
    // is the honest answer: handles are drawn when the user has said they are transforming.
    bool gizmoEnabled = true;
    int  gizmoHoveredHandle = -1;
    int  gizmoActiveHandle = -1;      // >= 0 for the whole duration of a drag.
    // The component's transform at the instant the drag began. A gizmo drag computes its result
    // absolutely from the mouse's start position, so every frame re-derives from these rather than
    // accumulating onto the previous frame -- accumulation drifts, and makes the undo record wrong.
    projv::core::vec3 gizmoDragStartPosition = projv::core::vec3(0.0f);
    projv::core::quat gizmoDragStartRotation = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);
    float gizmoDragStartScale = 1.0f;
    // Where on the axis (translate) or around the ring (rotate) the drag first grabbed, so the
    // handle stays under the cursor instead of snapping to it.
    float gizmoDragStartAxisT = 0.0f;
    projv::core::vec3 gizmoDragStartSpoke = projv::core::vec3(0.0f);
    projv::core::vec3 gizmoAnchorWorld = projv::core::vec3(0.0f);   // Pivot, in world space.

    // --- Viewport interaction ---
    // Screen rectangle the scene image occupies, so a click in it can be turned back into a ray.
    ImVec2 viewportImageMin = ImVec2(0.0f, 0.0f);
    ImVec2 viewportImageMax = ImVec2(0.0f, 0.0f);
    bool materialPickerActive = false;      // Eyedropper armed from the Palette panel: the next
                                            // viewport click samples, whatever the tool is.
    PickPurpose pendingPick = PickPurpose::None;   // A click landed this frame; the ray is cast after
    projv::core::vec2 pickUV = {0.0f, 0.0f};       // the interface is built, where the scene is reachable.

    // --- Hierarchy ---
    // Set when something outside the hierarchy chooses the selection (a viewport click, most of the
    // time). The tree then opens every ancestor of the selection and scrolls it into view once --
    // selecting a component you cannot see in the panel that is meant to show it is not a selection.
    bool revealSelectionInHierarchy = false;

    // --- Palette ---
    // Palettes are per component, so the panel edits one component's palette at a time. It follows
    // the hierarchy selection when that component has a palette, and can be pointed elsewhere.
    projv::ComponentHandle paletteComponent = projv::INVALID_COMPONENT_HANDLE;
    int selectedMaterialSlot = -1;

    // --- New data component ---
    // Resolution and voxel scale are fixed for a component's whole life (see utils::addComponent),
    // so they are chosen once here, at creation, and are read-only everywhere afterwards. Held on the
    // editor rather than passed around because two separate creation paths need the same answer.
    int   createResolutionIndex = 3;      // Into CHUNK_RESOLUTION_CHOICES; 3 == 256, what the shipped scenes use.
    float createVoxelScale = 1.0f;
    char materialNameBuffer[64] = {};
    // Voxels per slot. Counting walks every blob the component owns, so it is cached and recomputed
    // only when the palette or the chosen component changes.
    std::vector<uint32_t> materialUsage;
    bool materialUsageValid = false;
    // Per-chunk breakdown for the selected slot — "where is this material", not just "how much".
    std::vector<projv::utils::MaterialChunkUsage> materialChunkUsage;
    bool materialChunkUsageValid = false;
    int materialRemovalReplacement = 0;   // Slot chosen in the reassign dialog.
    // Set when a palette entry is added or removed. Those move every later component's palette
    // offset, so they need the full flushSceneUpdates rather than the single-texel colour path.
    bool gpuFlushNeeded = false;

    // --- Undo ---
    projv::editor::EditHistory history;

    // --- Component management ---
    projv::ComponentHandle renamingComponent = projv::INVALID_COMPONENT_HANDLE;
    char renameBuffer[256] = {};
    char createNameBuffer[256] = {};

    // --- Paint ---
    PaintShape paintShape = PaintShape::Voxel;
    float paintRadius = 3.0f;          // Voxels, for the sphere.
    int   paintCubeSize[3] = {4, 4, 4}; // Voxels, for the cube: width, height, depth.
    SelectionScope paintFillScope = SelectionScope::Material;   // Both fills.

    // --- Paint stroke in progress ---
    //
    // Painting is a drag, not a click, for the same reason every other paint program's brush is: a
    // wall is recoloured by sweeping over it, and clicking once per brush-width is not a tool, it is a
    // chore. The stroke is the unit of undo -- one entry for the whole sweep -- and of the target
    // component, locked on the press so a drag that wanders over a second object does not start
    // writing into it halfway through.
    //
    // There is no equivalent of the sculpt stroke's solidity override here, and there does not need to
    // be: painting never changes what is solid, so the ray sees the same surface on the last frame of
    // the stroke as on the first. Nor is there a journal of original state -- collectPaintTargets
    // already refuses to collect a voxel that is the colour being painted, so a voxel this stroke has
    // already covered is skipped on every later frame and each one appears in the record exactly once.
    // That leaves the accumulated lists below as both the record and the dedupe.
    bool paintStrokeActive = false;
    bool paintStrokeSampled = false;   // False until the press's own sample has been taken, which is
                                       // what tells "clicked on nothing" from "the drag ran off the
                                       // edge of the object" -- the first is worth a message and the
                                       // second happens on every stroke.
    projv::ComponentHandle paintStrokeComponent = projv::INVALID_COMPONENT_HANDLE;
    PaintShape paintStrokeShape = PaintShape::Voxel;   // Frozen for the stroke, like the sculpt brush.
    uint32_t paintStrokeColor = 0;                     // Likewise: the entry the stroke began on.
    bool paintStrokeHasAnchor = false;                 // False until the first dab lands, and again
                                                       // whenever the cursor leaves the geometry, so
                                                       // the gap is not bridged with a straight line
                                                       // through the air.
    projv::core::ivec3 paintStrokeLastCenter = projv::core::ivec3(0);
    // Shared with the history closures at the end of the stroke rather than copied into them; see
    // recordPaintStep for why a fill makes that worth doing.
    std::shared_ptr<std::vector<projv::core::ivec3>> paintStrokeCoords;
    std::shared_ptr<std::vector<uint32_t>> paintStrokePreviousColors;
    bool paintStrokeFillTruncated = false;   // A fill ran into PAINT_FILL_LIMIT.
    bool paintStrokeTruncated = false;       // A frame hit PAINT_MAX_INTERPOLATED_STEPS.

    // --- Sculpt ---
    SculptBrush sculptBrush = SculptBrush::Sphere;
    SculptMode   sculptMode   = SculptMode::Add;
    float sculptRadius = 5.0f;
    float sculptCubeWidth  = 4.0f;
    float sculptCubeHeight = 4.0f;
    float sculptCubeDepth  = 4.0f;
    // Where a stroke lands when it begins with the cursor over nothing at all. There is no surface to
    // measure from in that case, so the dab goes this many world units down the ray -- which is the
    // only way to put the first voxel into a component that is still empty. Once the stroke has an
    // anchor every later dab follows the geometry, so this distance only ever decides where a stroke
    // *starts*.
    float sculptPlaceDistance = 20.0f;
    SelectionScope extrudeFaceScope = SelectionScope::Material;
    // Bump only. Zero is the bare operator -- a disc with a hard rim; each step up adds a smoothing
    // pass over the result, so the bump sits into the surface instead of on top of it.
    float sculptBlendStrength = 1.0f;
    // Smooth only, and two settings in one number: below 1 it is how far out of place a cell has to
    // be before a pass will flip it, at and above 1 it is the width of the neighbourhood the filter
    // asks about. See sculptSmoothCutoff and sculptSmoothKernelRadius. One is the plain 3x3x3
    // majority filter, which is what the brush has always done, so the default changes nothing.
    // Bump's own blend passes ignore it entirely and always run at 1.
    float sculptSmoothStrength = 1.0f;

    // --- Sculpt stroke in progress ---
    //
    // A stroke is one press-drag-release of the left button, and it is the unit of almost everything:
    // one undo entry, one target component, one set of voxels hidden from the ray.
    //
    // Two of those need saying. The component is locked at the moment the button goes down, because an
    // edit queue belongs to a component and a drag that wandered over a second object would otherwise
    // start writing into it halfway through -- one gesture, two undo entries, and geometry in a place
    // the user was not pointing at.
    //
    // And `sculptStrokeOriginal` is the scene as it stood when the button went down, remembered one
    // cell at a time as the stroke first touches each. It does two jobs at once:
    //
    //   * It is what the ray is made to believe. For every cell in it the override reports the
    //     *original* solidity, so for as long as the button is held the ray sees the surface the
    //     stroke started against rather than the geometry the stroke is laying down. Without that an
    //     additive drag climbs its own deposits straight back toward the camera. See
    //     VoxelSolidityOverride.
    //   * It is the undo record. At the end of the stroke each remembered cell is compared against
    //     what it holds now, and the difference is the whole edit -- however many times the stroke
    //     changed its mind about that cell along the way.
    //
    // Remembering the *original* rather than accumulating a list of changes is what lets a brush both
    // add and remove within one stroke. Smooth does that by nature (it fills dents and shaves bumps in
    // the same pass) and Bump does it whenever a drag crosses a surface twice, and neither could have
    // been expressed by the pair of "added" and "removed" lists this replaced.
    bool sculptStrokeActive = false;
    projv::ComponentHandle sculptStrokeComponent = projv::INVALID_COMPONENT_HANDLE;
    SculptMode sculptStrokeMode = SculptMode::Add;   // Frozen for the stroke; the panel cannot be
                                                     // reached while the button is held anyway.
    SculptBrush sculptStrokeBrush = SculptBrush::Sphere;   // Likewise, and it decides which of the
                                                           // interactions below the drag is.
    uint32_t sculptStrokeColor = 0;                  // Likewise: the palette entry the stroke began on.
    bool sculptStrokeHasAnchor = false;              // False until the first dab lands.
    projv::core::ivec3 sculptStrokeLastCenter = projv::core::ivec3(0);

    // What one cell held before this stroke first touched it.
    struct StrokeVoxel {
        projv::core::ivec3 coord;
        bool wasSolid = false;
        uint32_t oldColor = 0;
    };
    // Keyed by packed coordinate. Looked up a few hundred times per frame by the ray override (once
    // per DDA step of one ray) and once per candidate cell by the brush, so a hash map is the right
    // shape; see packVoxelKey for the coordinate range it is exact over.
    std::unordered_map<uint64_t, StrokeVoxel> sculptStrokeOriginal;
    bool sculptStrokeTruncated = false;              // A dab hit SCULPT_MAX_INTERPOLATED_STEPS.
    // When the last iteration of a held-in-place brush ran. Smooth and Bump keep working while the
    // cursor sits still, so they are paced by the clock rather than by mouse movement.
    double sculptStrokeLastIteration = 0.0;

    // --- Extrude drag in progress ---
    //
    // The other kind of sculpt drag, and it works nothing like the brush. The face is chosen once, on
    // the press, and after that the mouse means one number: how many layers to move it. So there is no
    // per-frame ray cast into the geometry to be confused by the geometry the drag is creating —
    // depth comes from projecting the cursor onto the face's own normal, the way a translate gizmo
    // handle reads a drag along its axis.
    //
    // `extrudeAppliedDepth` is signed and is the whole committed state: positive layers have been
    // added outward, negative layers carved inward, zero is untouched. Every frame walks it toward
    // whatever the cursor now says, one layer at a time, so dragging out and back in undoes itself as
    // you go rather than piling edits up.
    std::vector<projv::core::ivec3> extrudeFace;   // The clicked face, in component voxel space.
    projv::core::ivec3 extrudeNormal = projv::core::ivec3(0);   // Its outward normal, same space.
    // One colour per face voxel, carried up each column as the face moves. The voxels pulled out
    // inherit the material of the voxel they came from rather than the palette's current selection --
    // extruding a wall extends the wall, in the wall's own colour. Per voxel rather than one for the
    // whole face because a WholeFace selection spans materials, and pulling a patterned surface out
    // has to keep its pattern rather than flatten it to whichever entry happened to be clicked.
    std::vector<uint32_t> extrudeFaceColors;
    projv::core::vec3 extrudeAxisWorld = projv::core::vec3(0.0f);   // The normal, in world space.
    projv::core::vec3 extrudeAnchorWorld = projv::core::vec3(0.0f);
    float extrudeStartAlongAxis = 0.0f;   // Where the cursor sat on that axis when the drag began.
    int extrudeAppliedDepth = 0;
    // What each applied layer displaced, so that reversing it puts back exactly what was there.
    //
    // The naive version -- "pulling a face out fills empty space, so retracting empties it again" --
    // is wrong whenever a face is extruded *into* geometry that was already there: one step of a
    // staircase pushed into the step above it, or a wall pushed into the floor. Retracting would then
    // delete voxels the drag never created, and so would undo. Splitting the record by what the cell
    // held before makes reversing a layer one rule for both directions: remove what this layer added,
    // and put back what it displaced.
    struct ExtrudeLayerRecord {
        std::vector<projv::core::ivec3> addedCoords;      // Were empty. Reverse by removing.
        std::vector<projv::core::ivec3> restoreCoords;    // Held something. Reverse by writing it back...
        std::vector<uint32_t> restoreColors;              // ...in the colour it held.
    };
    std::unordered_map<int, ExtrudeLayerRecord> extrudeLayers;
    bool extrudeFaceTruncated = false;   // The face hit EXTRUDE_MAX_FACE_VOXELS.
};

// =============================================================================
// Automatic framing (from the ScenePreviewer, unchanged)
// =============================================================================

// Measures the world-space bounding box of every live chunk. A chunk header carries its world
// position (minimum corner) and its scale, which is all a bounding box needs — no geometry is
// touched, so this stays instant on a large scene.
static bool measureSceneBounds(const projv::Scene& scene, projv::core::vec3& boundsMin,
                               projv::core::vec3& boundsMax) {
    bool found = false;
    for (const projv::Chunk& chunk : scene.chunks) {
        if (!chunk.alive || chunk.header.scale <= 0.0f) continue;

        projv::core::vec3 chunkMin = chunk.header.position;
        projv::core::vec3 chunkMax = chunk.header.position + projv::core::vec3(chunk.header.scale);
        if (!found) {
            boundsMin = chunkMin;
            boundsMax = chunkMax;
            found = true;
        } else {
            boundsMin = projv::core::min(boundsMin, chunkMin);
            boundsMax = projv::core::max(boundsMax, chunkMax);
        }
    }
    return found;
}

// Places the camera outside the scene's bounding sphere, looking at its centre from a raised
// three-quarter angle — the view that shows the most of an unfamiliar object.
static CameraFraming frameScene(const projv::Scene& scene) {
    using namespace projv::core;

    CameraFraming framing;
    vec3 boundsMin, boundsMax;
    if (!measureSceneBounds(scene, boundsMin, boundsMax)) {
        projv::core::warn("Scene has no live chunks — the viewport will be empty.");
        framing.position = vec3(0.0f, 0.0f, -100.0f);
        framing.yaw = 3.14159f / 2.0f;
        framing.pitch = 0.0f;
        framing.moveSpeed = 1.0f;
        return framing;
    }

    vec3 center = (boundsMin + boundsMax) * 0.5f;
    vec3 extents = boundsMax - boundsMin;
    float radius = length(extents) * 0.5f;
    if (radius <= 0.0f) radius = 1.0f;

    projv::core::info("Scene bounds: ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f})",
        boundsMin.x, boundsMin.y, boundsMin.z, boundsMax.x, boundsMax.y, boundsMax.z);

    // Pull back far enough that the bounding sphere fits the 60-degree vertical FOV the albedo pass
    // uses, with a margin so the subject is not jammed against the frame edge.
    const float FOV_RADIANS = 60.0f * 3.14159265f / 180.0f;
    float distance = (radius / std::tan(FOV_RADIANS * 0.5f)) * 1.25f;

    framing.yaw = 3.14159265f * 0.25f;  // Looking along +X/+Z, so the camera sits on the -X/-Z side.
    framing.pitch = -0.35f;             // Slightly above, looking down onto the subject.

    vec3 viewDirection = {
        std::cos(framing.pitch) * std::cos(framing.yaw),
        std::sin(framing.pitch),
        std::cos(framing.pitch) * std::sin(framing.yaw)
    };
    framing.position = center - viewDirection * distance;

    // Roughly two seconds to cross the scene at 60fps, which feels the same at any scale.
    framing.moveSpeed = std::max(radius * 2.0f / 120.0f, 0.01f);
    return framing;
}

// The camera's forward vector from its yaw/pitch. Every consumer of the fly-through camera derives
// this the same way; kept in one place for the outline projection below to share.
static projv::core::vec3 computeCameraDirection(const EditorState& editor) {
    return {
        std::cos(editor.cameraPitch) * std::cos(editor.cameraYaw),
        std::sin(editor.cameraPitch),
        std::cos(editor.cameraPitch) * std::sin(editor.cameraYaw)
    };
}

// Snaps the camera back to the framing computed for the current scene.
static void applyFraming(EditorState& editor) {
    editor.cameraPosition = editor.framing.position;
    editor.cameraYaw = editor.framing.yaw;
    editor.cameraPitch = editor.framing.pitch;
    editor.speedScrollSteps = 0.0f;
    editor.cameraMovedByInterface = true;
}

// =============================================================================
// Scene loading
// =============================================================================

// A Compose scene is any folder holding a compose.json — that is exactly what loadComposeFromDisk
// opens, and what the browser below offers as loadable.
static bool directoryHoldsScene(const std::filesystem::path& directory) {
    std::error_code errorCode;
    return std::filesystem::is_regular_file(directory / "compose.json", errorCode);
}

// Defined with the palette panel below; loading a scene has to reset the palette selection, which is
// the same job.
static void selectPaletteComponent(EditorState& editor, projv::ComponentHandle componentHandle);

// Points the browser at a folder, keeping its editable path field in step with it.
static void setBrowserDirectory(EditorState& editor, const std::string& directory) {
    editor.browserDirectory = directory;
    editor.browserSelection.clear();
    std::snprintf(editor.browserPathBuffer, sizeof(editor.browserPathBuffer), "%s", directory.c_str());
}

// Builds the scene in folderPath into the editor and uploads it. The *previous* scene's GPU data
// must already have been released — see the two-phase load in render(), which exists because
// holding two scenes' textures at once is more VRAM than a large scene leaves spare.
static bool loadScene(projv::Scene& scene, projv::GPUData& gpuData, EditorState& editor,
                      const std::string& folderPath) {
    std::string normalizedPath = folderPath;
    if (!normalizedPath.empty() && normalizedPath.back() != '/') normalizedPath += '/';

    if (!directoryHoldsScene(normalizedPath)) {
        editor.statusMessage = "No compose.json in " + normalizedPath;
        projv::core::warn("Load failed: {}", editor.statusMessage);
        return false;
    }

    projv::core::info("Loading scene: {}", normalizedPath);
    projv::Scene loadedScene = projv::utils::loadComposeFromDisk(normalizedPath);

    scene = std::move(loadedScene);
    gpuData = projv::graphics::createTexturesForScene(scene);

    editor.scenePath = normalizedPath;
    editor.sceneLoaded = true;
    // The browser opens next to the scene that is loaded, where its siblings are. normalizedPath
    // ends in a separator, so its last path element is empty and the folder holding the scene is two
    // parents up rather than one.
    std::error_code parentErrorCode;
    std::filesystem::path absoluteScenePath = std::filesystem::absolute(normalizedPath, parentErrorCode);
    setBrowserDirectory(editor, absoluteScenePath.parent_path().parent_path().string());
    editor.selectedComponent = projv::INVALID_COMPONENT_HANDLE;
    editor.lastHierarchySelection = projv::INVALID_COMPONENT_HANDLE;
    editor.selectedVoxelCount = 0;
    editor.selectionOutlineChunks.clear();
    editor.selectionOutlineValid = false;
    // Palette state belongs to the scene that is going away: handles, slots and the cached usage
    // counts all mean something different in the incoming one.
    selectPaletteComponent(editor, projv::INVALID_COMPONENT_HANDLE);
    editor.materialUsage.clear();
    editor.materialChunkUsage.clear();
    editor.materialPickerActive = false;
    // A click parked for the ray cast that has not happened yet refers to the outgoing scene's
    // camera and geometry, and would be cast into the incoming one.
    editor.pendingPick = PickPurpose::None;
    // Likewise a stroke caught mid-drag: its component handle and every coordinate it has collected
    // belong to the scene being replaced. Dropped rather than committed — there is nothing left to
    // commit it to, and the history is cleared just below anyway.
    editor.sculptStrokeActive = false;
    editor.sculptStrokeComponent = projv::INVALID_COMPONENT_HANDLE;
    editor.sculptStrokeHasAnchor = false;
    editor.sculptStrokeOriginal.clear();
    editor.paintStrokeActive = false;
    editor.paintStrokeSampled = false;
    editor.paintStrokeComponent = projv::INVALID_COMPONENT_HANDLE;
    editor.paintStrokeHasAnchor = false;
    editor.paintStrokeCoords.reset();
    editor.paintStrokePreviousColors.reset();
    editor.extrudeFace.clear();
    editor.extrudeLayers.clear();
    editor.extrudeAppliedDepth = 0;
    editor.revealSelectionInHierarchy = false;
    // Every recorded edit refers to slots and blobs of the scene being replaced.
    editor.history.clear();
    editor.renamingComponent = projv::INVALID_COMPONENT_HANDLE;
    editor.renameBuffer[0] = '\0';
    editor.framing = frameScene(scene);
    applyFraming(editor);
    editor.statusMessage = "Loaded " + normalizedPath + " - " + std::to_string(scene.chunks.size()) +
                           " chunk(s), " + std::to_string(scene.components.size()) + " component(s)";
    projv::core::info("{}", editor.statusMessage);
    return true;
}

// =============================================================================
// Viewport render targets
// =============================================================================

// Resizes the scene renderer's offscreen textures (and the framebuffers pointing at them) to the
// Viewport panel's size, so the scene is rendered at exactly the resolution it is displayed at.
//
// The engine's resizeFramebuffersAndTheirTexturesIfNeeded cannot be used here: it also calls
// bgfx::reset with the size it is given, which is right when the render target *is* the window, and
// wrong here — it would shrink the back buffer to the panel and the interface with it. This is that
// function's texture half, minus the reset, plus destruction of the framebuffer handles it replaces
// (the editor resizes on every frame of a splitter drag, so leaked handles would exhaust bgfx's
// framebuffer pool within seconds).
static void resizeViewportTargets(const std::shared_ptr<projv::ConstructedRenderer>& renderer,
                                  int width, int height) {
    projv::ConstructedTextures& textures = renderer->resources.textures;
    projv::ConstructedFramebuffers& framebuffers = renderer->resources.framebuffers;

    // bgfx defers the actual release until the frame that last referenced a handle has completed, so
    // destroying up front is safe even though this runs mid-frame-loop.
    for (const auto& framebuffer : framebuffers.frameBufferTextureMapping) {
        uint frameBufferID = framebuffer.first;
        if (bgfx::isValid(framebuffers.frameBufferHandles.at(frameBufferID))) {
            bgfx::destroy(framebuffers.frameBufferHandles.at(frameBufferID));
        }
        if (framebuffers.pingPongFBOs.at(frameBufferID) &&
            bgfx::isValid(framebuffers.frameBufferHandlesAlternate.at(frameBufferID))) {
            bgfx::destroy(framebuffers.frameBufferHandlesAlternate.at(frameBufferID));
        }
    }

    for (const auto& resizableTexture : textures.texturesResizedWithWindow) {
        uint textureID = resizableTexture.first;
        bgfx::TextureFormat::Enum format = textures.textureFormats.at(textureID);

        if (bgfx::isValid(textures.textureHandles.at(textureID))) {
            bgfx::destroy(textures.textureHandles.at(textureID));
        }
        textures.textureHandles.at(textureID) =
            bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, format, BGFX_TEXTURE_RT);
        textures.textureResolutions[textureID] = projv::core::ivec2(width, height);

        // The accumulation target is a ping-pong pair: it is read and written by the same pass, so it
        // exists twice and both copies have to follow the panel.
        if (!textures.pingPongFlags.at(textureID)) continue;
        if (bgfx::isValid(textures.textureHandlesAlternate.at(textureID))) {
            bgfx::destroy(textures.textureHandlesAlternate.at(textureID));
        }
        textures.textureHandlesAlternate.at(textureID) =
            bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, format, BGFX_TEXTURE_RT);
    }

    for (const auto& framebuffer : framebuffers.frameBufferTextureMapping) {
        uint frameBufferID = framebuffer.first;
        const std::vector<uint>& textureIDs = framebuffer.second;

        // false: the framebuffer does not take ownership of its attachments. The textures are
        // destroyed by the loop above, so ownership here would mean destroying them twice.
        std::vector<bgfx::Attachment> attachments =
            projv::graphics::getTextureAttachments(textures.textureHandles, textureIDs);
        framebuffers.frameBufferHandles.at(frameBufferID) =
            bgfx::createFrameBuffer(uint16_t(textureIDs.size()), attachments.data(), false);

        if (!framebuffers.pingPongFBOs.at(frameBufferID)) continue;
        std::vector<bgfx::Attachment> alternateAttachments =
            projv::graphics::getTextureAttachments(textures.textureHandlesAlternate, textureIDs);
        framebuffers.frameBufferHandlesAlternate.at(frameBufferID) =
            bgfx::createFrameBuffer(uint16_t(textureIDs.size()), alternateAttachments.data(), false);
    }
}

// The texture the display pass writes, which is what the Viewport panel shows. Texture 3 in the
// editor renderer's resources.json.
static bgfx::TextureHandle getViewportTexture(const std::shared_ptr<projv::ConstructedRenderer>& renderer) {
    const std::unordered_map<uint, bgfx::TextureHandle>& handles = renderer->resources.textures.textureHandles;
    auto it = handles.find(3);
    return it == handles.end() ? bgfx::TextureHandle(BGFX_INVALID_HANDLE) : it->second;
}

// =============================================================================
// Camera
// =============================================================================

// Fly-through camera, active only while the right mouse button is held inside the Viewport panel.
// Gating on the panel is what lets the same mouse serve both the interface and the camera: a drag
// that starts over a dock panel belongs to ImGui, one that starts over the scene belongs to us.
// Returns whether the camera changed this frame, which the accumulate pass needs in order to drop
// its history.
static bool updateCamera(GLFWwindow* window, EditorState& editor) {
    ImGuiIO& io = ImGui::GetIO();
    bool cameraMoved = false;

    if (!editor.cameraIsFlying && editor.viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        editor.cameraIsFlying = true;
        glfwGetCursorPos(window, &editor.cursorAnchorX, &editor.cursorAnchorY);
        editor.lastCursorX = editor.cursorAnchorX;
        editor.lastCursorY = editor.cursorAnchorY;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        // A captured cursor reports unbounded virtual motion, which would drag ImGui's hover and
        // click state around the interface behind the viewport. NoMouse makes ImGui ignore the mouse
        // for the duration; the fly-through reads GLFW directly instead.
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    }

    if (editor.cameraIsFlying && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) != GLFW_PRESS) {
        editor.cameraIsFlying = false;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        // Put the cursor back where the drag started, so releasing does not leave it somewhere else
        // in the interface.
        glfwSetCursorPos(window, editor.cursorAnchorX, editor.cursorAnchorY);
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }

    // Scroll scales the framing-derived speed geometrically, so one notch is a consistent
    // proportional change whether the scene is 64 or 8192 voxels across. Scroll that lands on any
    // other panel belongs to that panel, so it is discarded rather than applied.
    if (editor.cameraIsFlying || editor.viewportHovered) {
        editor.speedScrollSteps += float(g_scrollOffsetThisFrame);
    }
    float moveSpeed = editor.framing.moveSpeed * std::pow(1.2f, editor.speedScrollSteps);

    if (!editor.cameraIsFlying) {
        // H re-frames on the scene, for when navigation has left the subject behind. Ignored while a
        // text field has the keyboard, or typing a path would steer the camera.
        if (editor.viewportHovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_H)) {
            applyFraming(editor);
            return true;
        }
        return false;
    }

    double cursorX, cursorY;
    glfwGetCursorPos(window, &cursorX, &cursorY);
    double mouseDeltaX = cursorX - editor.lastCursorX;
    double mouseDeltaY = cursorY - editor.lastCursorY;
    editor.lastCursorX = cursorX;
    editor.lastCursorY = cursorY;

    const float MOUSE_SENSITIVITY = 0.0025f;
    if (mouseDeltaX != 0.0 || mouseDeltaY != 0.0) {
        editor.cameraYaw += float(mouseDeltaX) * MOUSE_SENSITIVITY;
        editor.cameraPitch -= float(mouseDeltaY) * MOUSE_SENSITIVITY;
        const float PITCH_LIMIT = 1.55f;   // ~89 degrees, just shy of gimbal flip.
        editor.cameraPitch = std::clamp(editor.cameraPitch, -PITCH_LIMIT, PITCH_LIMIT);
        cameraMoved = true;
    }

    // Horizontal forward from yaw only, so W/S flies level regardless of pitch.
    projv::core::vec3 forwardDirection = { std::cos(editor.cameraYaw), 0.0f, std::sin(editor.cameraYaw) };
    // +90 degrees, not -90: with the yaw convention above (forward = cos/sin of yaw across X/Z), the
    // screen-right vector is forward rotated the *other* way, or A/D strafe backwards.
    projv::core::vec3 rightDirection = { std::cos(editor.cameraYaw + 3.14159265f / 2.0f), 0.0f,
                                         std::sin(editor.cameraYaw + 3.14159265f / 2.0f) };

    if (glfwGetKey(window, GLFW_KEY_W)) { editor.cameraPosition += forwardDirection * moveSpeed; cameraMoved = true; }
    if (glfwGetKey(window, GLFW_KEY_S)) { editor.cameraPosition -= forwardDirection * moveSpeed; cameraMoved = true; }
    if (glfwGetKey(window, GLFW_KEY_A)) { editor.cameraPosition -= rightDirection * moveSpeed; cameraMoved = true; }
    if (glfwGetKey(window, GLFW_KEY_D)) { editor.cameraPosition += rightDirection * moveSpeed; cameraMoved = true; }
    if (glfwGetKey(window, GLFW_KEY_R)) { editor.cameraPosition.y += moveSpeed; cameraMoved = true; }
    if (glfwGetKey(window, GLFW_KEY_F)) { editor.cameraPosition.y -= moveSpeed; cameraMoved = true; }
    if (glfwGetKey(window, GLFW_KEY_H)) { applyFraming(editor); cameraMoved = true; }

    return cameraMoved;
}

// =============================================================================
// Interface
// =============================================================================

// The window name of the per-tool settings panel. The part after ### is the window's identity and
// the part before it is what the title bar shows, so the panel can be called "Sculpt" one moment and
// "Paint" the next without ImGui treating it as a different window (and without the dock layout
// having to be rebuilt every time the tool changes).
static const char* TOOL_PANEL_ID = "Tool###ToolPanel";

// The layout the editor opens with the first time it is run (and whenever View ▸ Reset Layout is
// used). ImGui saves any rearrangement to imgui.ini and restores it on the next run — this only runs
// when there is nothing saved to restore.
//
// Two columns and a strip, which is the shape every editor of this kind converges on for a reason:
//
//   left    what exists — the scene's own contents on top, the library of things that could be
//           brought into it underneath. Navigation and organisation.
//   centre  the scene itself, with the tool strip and the breadcrumb that says what is being edited.
//   right   what is being done to the selection, stacked rather than tabbed: the Inspector's
//           transform, the active tool's settings, and the palette. Those three are not alternatives
//           — sculpting wants a brush, a colour, and the target's transform in view at once — and
//           tabbing them is what forced the tool panel to carry its own copy of the palette grid.
//   bottom  the edit history. The numbers that used to share it now live in the status bar, which is
//           what a line of read-only counters actually deserves.
static void buildDefaultDockLayout(ImGuiID dockspaceID, ImVec2 dockspaceSize) {
    ImGui::DockBuilderRemoveNode(dockspaceID);
    ImGui::DockBuilderAddNode(dockspaceID, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceID, dockspaceSize);

    ImGuiID centerNode = dockspaceID;
    ImGuiID leftNode = ImGui::DockBuilderSplitNode(centerNode, ImGuiDir_Left, 0.20f, nullptr, &centerNode);
    ImGuiID rightNode = ImGui::DockBuilderSplitNode(centerNode, ImGuiDir_Right, 0.25f, nullptr, &centerNode);
    ImGuiID bottomNode = ImGui::DockBuilderSplitNode(centerNode, ImGuiDir_Down, 0.22f, nullptr, &centerNode);

    // Left column: the scene's contents above the disk they came from.
    ImGuiID libraryNode = ImGui::DockBuilderSplitNode(leftNode, ImGuiDir_Down, 0.40f, nullptr, &leftNode);

    // Right column, split twice from the bottom up so the fractions are of the whole column: palette
    // 30%, tool 35%, inspector the remaining 35%.
    ImGuiID paletteNode = ImGui::DockBuilderSplitNode(rightNode, ImGuiDir_Down, 0.30f, nullptr, &rightNode);
    ImGuiID toolNode = ImGui::DockBuilderSplitNode(rightNode, ImGuiDir_Down, 0.50f, nullptr, &rightNode);

    ImGui::DockBuilderDockWindow("Viewport", centerNode);
    ImGui::DockBuilderDockWindow("Scene Hierarchy", leftNode);
    ImGui::DockBuilderDockWindow("Library", libraryNode);
    ImGui::DockBuilderDockWindow("Inspector", rightNode);
    ImGui::DockBuilderDockWindow(TOOL_PANEL_ID, toolNode);
    ImGui::DockBuilderDockWindow("Palette", paletteNode);
    ImGui::DockBuilderDockWindow("History", bottomNode);
    ImGui::DockBuilderFinish(dockspaceID);
}

// File ▸ Load Scene…: a directory browser over the filesystem. Folders that hold a compose.json are
// scenes and are marked as such; anything else is just a step on the way to one.
static void drawLoadSceneDialog(EditorState& editor) {
    if (editor.loadSceneDialogOpen) {
        // Open on the folder the current scene came from, which is nearly always where the next one
        // is too.
        std::error_code openErrorCode;
        std::filesystem::path startDirectory = std::filesystem::absolute(editor.browserDirectory, openErrorCode);
        setBrowserDirectory(editor, startDirectory.string());
        ImGui::OpenPopup("Load Scene");
        editor.loadSceneDialogOpen = false;
    }

    ImGui::SetNextWindowSize(ImVec2(640.0f, 460.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Load Scene", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    std::error_code errorCode;
    std::filesystem::path currentDirectory = editor.browserDirectory;

    ImGui::TextUnformatted("Folder");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##browserPath", editor.browserPathBuffer, sizeof(editor.browserPathBuffer),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (std::filesystem::is_directory(editor.browserPathBuffer, errorCode)) {
            setBrowserDirectory(editor, editor.browserPathBuffer);
            currentDirectory = editor.browserDirectory;
        } else {
            editor.statusMessage = std::string("Not a folder: ") + editor.browserPathBuffer;
        }
    }

    ImGui::Separator();

    // The listing. Collected first so it can be sorted: the filesystem hands directory entries back
    // in whatever order it stores them, which is not an order anyone can scan.
    std::vector<std::filesystem::path> subdirectories;
    if (std::filesystem::is_directory(currentDirectory, errorCode)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(currentDirectory, std::filesystem::directory_options::skip_permission_denied, errorCode)) {
            if (entry.is_directory(errorCode)) {
                subdirectories.push_back(entry.path());
            }
        }
    }
    std::sort(subdirectories.begin(), subdirectories.end());

    std::string directoryToOpen;
    std::string sceneToLoad;

    ImGui::BeginChild("##browserListing", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.0f), ImGuiChildFlags_Borders);
    if (currentDirectory.has_parent_path()) {
        if (ImGui::Selectable("..")) {
            directoryToOpen = currentDirectory.parent_path().string();
        }
    }
    for (const std::filesystem::path& subdirectory : subdirectories) {
        bool isScene = directoryHoldsScene(subdirectory);
        std::string label = (isScene ? "[scene]  " : "[dir]    ") + subdirectory.filename().string();

        // Scene folders are tinted, so the thing being looked for stands out from the folders that
        // are only on the way to it.
        if (isScene) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 1.00f, 1.00f));
        }
        bool isSelected = (editor.browserSelection == subdirectory.string());
        if (ImGui::Selectable(label.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
            // A click selects; a double click commits — loading a scene folder, or stepping into an
            // ordinary one. Committing on a single click would mean a scene folder navigated away
            // from under the second half of a double click.
            editor.browserSelection = subdirectory.string();
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (isScene) {
                    sceneToLoad = subdirectory.string();
                } else {
                    directoryToOpen = subdirectory.string();
                }
            }
        }
        if (isScene) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    // Load acts on the selected scene folder, or on the folder being browsed when that is itself a
    // scene — so both "point at it from outside" and "walk into it" end at the same button.
    bool selectionIsScene = !editor.browserSelection.empty() && directoryHoldsScene(editor.browserSelection);
    bool currentIsScene = directoryHoldsScene(currentDirectory);
    ImGui::TextDisabled("%s", selectionIsScene  ? "Press Load to open the selected scene."
                            : currentIsScene    ? "This folder is a scene — press Load."
                                                : "Double-click to open a folder, or a [scene] to load it.");

    ImGui::BeginDisabled(!selectionIsScene && !currentIsScene);
    if (ImGui::Button("Load", ImVec2(120.0f, 0.0f))) {
        sceneToLoad = selectionIsScene ? editor.browserSelection : currentDirectory.string();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }

    if (!directoryToOpen.empty()) {
        setBrowserDirectory(editor, directoryToOpen);
    }
    if (!sceneToLoad.empty()) {
        // The load itself happens after the interface is built — it destroys and rebuilds GPU
        // resources, which has no business running in the middle of laying out a window.
        editor.pendingScenePath = sceneToLoad;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// =============================================================================
// Selection outline
// =============================================================================
//
// Compose components form a tree, but only the leaves -- Chunk and Grid -- own geometry; an Asset
// node is a folder. Selecting a folder in the hierarchy means "everything under here", so the
// outline is drawn around every .data leaf its subtree reaches, not just the folder's own (empty)
// bounds.

// The resolutions utils::addComponent accepts, in the order they are offered. Powers of four only --
// anything else builds a tree64 whose depth the shader derives differently, so there is no "custom"
// entry here by design. 1024 and up are offered but cost 1024^3 addressable cells, so the default
// sits at 256, which is what every scene shipped so far uses.
static constexpr uint32_t CHUNK_RESOLUTION_CHOICES[] = { 4, 16, 64, 256, 1024 };
static constexpr int CHUNK_RESOLUTION_CHOICE_COUNT =
    int(sizeof(CHUNK_RESOLUTION_CHOICES) / sizeof(CHUNK_RESOLUTION_CHOICES[0]));

// The width a field should ask for when a text label follows it on the same row.
//
// ImGui's -1.0f means "fill to the right edge of the content region", which leaves exactly nothing
// for a trailing SameLine label -- it is laid out past the edge and clipped away, so the field ends
// up unlabelled. Asking for the row minus the label keeps both on screen. Negative widths are
// measured back from the right edge, which is what makes this one expression rather than a
// GetContentRegionAvail subtraction that would have to be redone per row.
static float fieldWidthBeside(const char* label) {
    return -(ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemInnerSpacing.x * 2.0f);
}

// The resolution/voxel-scale controls shared by both creation paths. Drawn inside whatever popup or
// menu is asking, so the two can never drift apart on what they offer.
//
// Widths are explicit rather than the -1.0f "fill the row" the docked panels use. Both callers are
// auto-resizing popups, where -1.0f is measured against a content region that is itself derived from
// the content -- the item asks for everything available, which widens the window, which makes more
// available, and the popup runs off the screen. A fixed width matches the name field beside it.
static void drawNewDataComponentOptions(EditorState& editor) {
    const float FIELD_WIDTH = 200.0f;

    ImGui::TextDisabled("Resolution (fixed at creation)");
    ImGui::SetNextItemWidth(FIELD_WIDTH);
    char label[32];
    std::snprintf(label, sizeof(label), "%u", CHUNK_RESOLUTION_CHOICES[editor.createResolutionIndex]);
    if (ImGui::BeginCombo("##createResolution", label)) {
        for (int i = 0; i < CHUNK_RESOLUTION_CHOICE_COUNT; i++) {
            char entry[32];
            std::snprintf(entry, sizeof(entry), "%u", CHUNK_RESOLUTION_CHOICES[i]);
            if (ImGui::Selectable(entry, editor.createResolutionIndex == i)) {
                editor.createResolutionIndex = i;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(FIELD_WIDTH);
    ImGui::DragFloat("##createVoxelScale", &editor.createVoxelScale, 0.01f, 0.001f, 1000.0f, "%.3f");
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextDisabled("Voxel scale");

    // The one number a user actually pictures, so it is spelled out rather than left to be inferred
    // from a multiplication.
    ImGui::TextDisabled("World size %.2f",
                        editor.createVoxelScale * float(CHUNK_RESOLUTION_CHOICES[editor.createResolutionIndex]));
}

// Collects the live chunk(s) a component's selection covers: itself if it is a Chunk, every occupied
// cell if it is a Grid, or the same recursively for every child if it is an Asset folder.
static void collectLeafChunks(const projv::Scene& scene, projv::ComponentHandle handle,
                              std::vector<projv::ChunkHandle>& out) {
    if (handle >= scene.components.size()) return;
    const projv::ComponentRecord& component = scene.components[handle];

    if (component.kind == projv::ComponentKind::Chunk) {
        if (component.chunkHandle < scene.chunks.size() && scene.chunks[component.chunkHandle].alive) {
            out.push_back(component.chunkHandle);
        }
    } else if (component.kind == projv::ComponentKind::Grid) {
        if (component.gridIndex >= 0 && size_t(component.gridIndex) < scene.grids.size()) {
            for (int32_t chunkIndex : scene.grids[component.gridIndex].cellToChunk) {
                if (chunkIndex >= 0 && size_t(chunkIndex) < scene.chunks.size() &&
                    scene.chunks[chunkIndex].alive) {
                    out.push_back(projv::ChunkHandle(chunkIndex));
                }
            }
        }
    } else {
        for (projv::ComponentHandle child : component.children) {
            collectLeafChunks(scene, child, out);
        }
    }
}

// Projects a world-space point into a pixel position within the viewport image, matching the camera
// model the albedo shader builds its rays from (rayStartDirection in pjv_utils_DDA.sc — the same
// basis utils::rayDirectionThroughImage uses for picking). This is that construction inverted: given
// a world point instead of a screen UV, recover the screen UV it would have projected from.
//
// Returns false for a point behind the camera (dot product with forward <= 0), which has no sane
// forward projection — the caller drops any box edge touching such a corner rather than clip it.
static bool worldToViewportPixel(projv::core::vec3 worldPos, projv::core::vec3 cameraPos,
                                 projv::core::vec3 cameraDirection, ImVec2 imageMin, ImVec2 imageMax,
                                 ImVec2& outScreenPos, float verticalFovDegrees = 60.0f) {
    using namespace projv::core;
    vec3 forward = glm::normalize(cameraDirection);
    vec3 worldUp = std::abs(forward.y) > 0.999f ? vec3(0.0f, 0.0f, 1.0f) : vec3(0.0f, 1.0f, 0.0f);
    vec3 right = glm::normalize(glm::cross(forward, worldUp));
    vec3 up = glm::normalize(glm::cross(right, forward));

    vec3 toPoint = worldPos - cameraPos;
    float depth = glm::dot(toPoint, forward);
    if (depth <= 1.0e-3f) return false;

    float imageWidth = std::max(1.0f, imageMax.x - imageMin.x);
    float imageHeight = std::max(1.0f, imageMax.y - imageMin.y);
    float aspectRatio = imageWidth / imageHeight;
    float scale = std::tan(glm::radians(verticalFovDegrees * 0.5f));

    float ndcX = (glm::dot(toPoint, right) / depth) / (scale * aspectRatio);
    float ndcY = (glm::dot(toPoint, up) / depth) / scale;

    // Inverse of rayStartDirection's `vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;`.
    float u = (ndcX + 1.0f) * 0.5f;
    float v = (1.0f - ndcY) * 0.5f;
    outScreenPos = ImVec2(imageMin.x + u * imageWidth, imageMin.y + v * imageHeight);
    return true;
}

// Draws one chunk's world-space OBB as a yellow wireframe box. header.rotation is about the chunk's
// own minimum corner (position), matching the convention fetchVoxelColor uses on the GPU side and
// utils::pickVoxel mirrors on the CPU side: worldCorner = position + rotation * (localOffset).
static void drawChunkOutline(ImDrawList* drawList, const projv::Chunk& chunk, projv::core::vec3 cameraPos,
                             projv::core::vec3 cameraDirection, ImVec2 imageMin, ImVec2 imageMax) {
    using namespace projv::core;
    if (chunk.header.scale <= 0.0f) return;

    mat3 rotation = glm::mat3_cast(chunk.header.rotation);
    vec3 corners[8];
    for (int i = 0; i < 8; i++) {
        vec3 localOffset((i & 1) ? chunk.header.scale : 0.0f,
                         (i & 2) ? chunk.header.scale : 0.0f,
                         (i & 4) ? chunk.header.scale : 0.0f);
        corners[i] = chunk.header.position + rotation * localOffset;
    }

    ImVec2 screen[8];
    bool visible[8];
    for (int i = 0; i < 8; i++) {
        visible[i] = worldToViewportPixel(corners[i], cameraPos, cameraDirection, imageMin, imageMax, screen[i]);
    }

    // The cube's 12 edges, as pairs of corner indices sharing exactly one bit-axis flip.
    static const int EDGES[12][2] = {
        {0,1}, {0,2}, {0,4}, {1,3}, {1,5}, {2,3}, {2,6}, {3,7}, {4,5}, {4,6}, {5,7}, {6,7}
    };
    const ImU32 outlineColor = IM_COL32(255, 220, 40, 255);
    for (const int (&edge)[2] : EDGES) {
        // An edge with an endpoint behind the camera is dropped rather than clipped to the near
        // plane -- simple, and sufficient for a selection hint that only needs to read as "here" once
        // any part of the box is in front of the camera.
        if (!visible[edge[0]] || !visible[edge[1]]) continue;
        drawList->AddLine(screen[edge[0]], screen[edge[1]], outlineColor, 2.0f);
    }
}

// Whether `handle` is `descendant` itself or one of its ancestors — that is, whether it lies on the
// path from the root down to it. Walked upward from the descendant, which is O(depth) and needs no
// scratch storage; walking downward from the handle would visit the whole subtree instead.
static bool isSelfOrAncestorOf(const projv::Scene& scene, projv::ComponentHandle handle,
                               projv::ComponentHandle descendant) {
    if (handle == projv::INVALID_COMPONENT_HANDLE) return false;
    for (projv::ComponentHandle walker = descendant;
         walker != projv::INVALID_COMPONENT_HANDLE && walker < scene.components.size();
         walker = scene.components[walker].parent) {
        if (walker == handle) return true;
    }
    return false;
}

// One node of the scene tree. Asset components are the folders of a Compose scene (they hold
// children and no geometry of their own); Chunk and Grid components are its data leaves.
static void drawHierarchyNode(projv::Scene& scene, EditorState& editor, projv::ComponentHandle handle) {
    if (handle >= scene.components.size()) return;
    const projv::ComponentRecord& component = scene.components[handle];

    bool isFolder = component.kind == projv::ComponentKind::Asset;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    // Data components (Chunk, Grid) are always leaves — they can never have children.
    if (!isFolder) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    } else if (component.children.empty()) {
        // Empty folder: looks like a leaf but MUST push an ID — drag-drop can add children.
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (handle == editor.selectedComponent) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const char* kindLabel = isFolder ? "folder" : "data";
    ImVec4 kindColor = isFolder ? ImVec4(0.75f, 0.60f, 0.20f, 0.85f)   // amber — folder
                                : ImVec4(0.35f, 0.55f, 0.80f, 0.70f);  // blue  — data

    // --- Rename in place ---
    if (editor.renamingComponent == handle) {
        std::string label = component.name.empty() ? ("component " + std::to_string(handle)) : component.name;
        bool nodeOpen = ImGui::TreeNodeEx((void*)(uintptr_t)handle, flags, "%s", "");
        if (nodeOpen && isFolder) { ImGui::TreePop(); }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::InputText("##rename", editor.renameBuffer, sizeof(editor.renameBuffer),
                              ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (std::strlen(editor.renameBuffer) > 0) {
                scene.components[handle].name = editor.renameBuffer;
            }
            editor.renamingComponent = projv::INVALID_COMPONENT_HANDLE;
        }
        if (ImGui::IsItemDeactivated()) {
            editor.renamingComponent = projv::INVALID_COMPONENT_HANDLE;
        }
        return;
    }

    projv::Scene* scenePtr = &scene;
    EditorState* editorPtr = &editor;

    std::string label = component.name.empty() ? ("component " + std::to_string(handle)) : component.name;

    // Something outside the tree chose the selection (a viewport click, the breadcrumb): open every
    // folder on the way down to it so the row exists to be scrolled to. Only forced open, never
    // forced shut -- a reveal should not close folders the user opened for their own reasons.
    bool onPathToSelection = isSelfOrAncestorOf(scene, handle, editor.selectedComponent);
    if (editor.revealSelectionInHierarchy && onPathToSelection && isFolder) {
        ImGui::SetNextItemOpen(true);
    }

    bool nodeOpen = ImGui::TreeNodeEx((void*)(uintptr_t)handle, flags, "%s", label.c_str());
    // ALL item queries must happen immediately after TreeNodeEx, before anything else touches the
    // "last item" (ImGui's drag-drop and click APIs are meaningless after a SameLine or TextDisabled).
    bool nodeClicked = ImGui::IsItemClicked();
    bool nodeToggledOpen = ImGui::IsItemToggledOpen();
    bool nodeRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

    // Centred rather than merely brought into view: a row scrolled to the very bottom edge is
    // technically visible and still reads as "not there".
    if (editor.revealSelectionInHierarchy && handle == editor.selectedComponent) {
        ImGui::SetScrollHereY(0.5f);
    }

    // --- Drag source ---
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("COMPONENT", &handle, sizeof(handle));
        ImGui::Text("Move %s", label.c_str());
        ImGui::EndDragDropSource();
    }

    // --- Drop target (only Asset nodes accept drops) ---
    if (component.kind == projv::ComponentKind::Asset && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("COMPONENT")) {
            projv::ComponentHandle draggedHandle = *(const projv::ComponentHandle*)payload->Data;
            if (draggedHandle != handle) {
                projv::ComponentHandle oldParent = scenePtr->components[draggedHandle].parent;
                if (projv::utils::setComponentParent(*scenePtr, draggedHandle, handle)) {
                    if (editorPtr->selectedComponent == draggedHandle) {
                        editorPtr->selectionOutlineValid = false;
                    }
                    editorPtr->gpuFlushNeeded = true;
                    editorPtr->cameraMovedByInterface = true;

                    projv::editor::EditRecord record;
                    record.label = "Reparent " + scenePtr->components[draggedHandle].name;
                    record.undo = [scenePtr, draggedHandle, oldParent] {
                        projv::utils::setComponentParent(*scenePtr, draggedHandle, oldParent);
                    };
                    record.redo = [scenePtr, draggedHandle, handle] {
                        projv::utils::setComponentParent(*scenePtr, draggedHandle, handle);
                    };
                    editorPtr->history.record(std::move(record), ImGui::GetTime());
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // --- Context menu popup trigger ---
    char popupID[32];
    std::snprintf(popupID, sizeof(popupID), "##ctx%u", handle);
    if (nodeRightClicked && !nodeToggledOpen) {
        ImGui::OpenPopup(popupID);
    }

    ImGui::SameLine();
    ImGui::TextColored(kindColor, "(%s)", kindLabel);

    if (nodeClicked && !nodeToggledOpen) {
        editor.selectedComponent = handle;
        editor.selectedVoxelCount = projv::utils::getComponentVoxelCount(scene, handle);
        editor.selectionOutlineValid = false;
    }

    // --- Context menu popup content ---
    if (ImGui::BeginPopup(popupID)) {
        if (ImGui::MenuItem("Duplicate")) {
            projv::ComponentHandle parent = scenePtr->components[handle].parent;
            projv::ComponentHandle dup = projv::utils::duplicateComponent(*scenePtr, handle, parent);
            if (dup != projv::INVALID_COMPONENT_HANDLE) {
                editorPtr->gpuFlushNeeded = true;
                editorPtr->statusMessage = "Duplicated " + scenePtr->components[dup].name;
            }
        }
        if (ImGui::MenuItem("Rename")) {
            editorPtr->renamingComponent = handle;
            std::strncpy(editorPtr->renameBuffer, scenePtr->components[handle].name.c_str(),
                         sizeof(editorPtr->renameBuffer) - 1);
        }
        if (component.kind == projv::ComponentKind::Asset && ImGui::BeginMenu("Add Child")) {
            // The grid is fixed once the component exists, so it is chosen here rather than being
            // silently defaulted and discovered later.
            drawNewDataComponentOptions(*editorPtr);
            ImGui::Separator();
            if (ImGui::MenuItem("Data")) {
                uint32_t resolution = CHUNK_RESOLUTION_CHOICES[editorPtr->createResolutionIndex];
                projv::ComponentHandle child = projv::utils::addComponent(
                    *scenePtr, projv::ComponentKind::Chunk, "New Data", handle,
                    resolution, editorPtr->createVoxelScale);
                if (child != projv::INVALID_COMPONENT_HANDLE) {
                    editorPtr->gpuFlushNeeded = true;
                    editorPtr->selectedComponent = child;
                    editorPtr->selectionOutlineValid = false;
                    editorPtr->selectedVoxelCount = projv::utils::getComponentVoxelCount(*scenePtr, child);
                    editorPtr->statusMessage = "Created Data component at resolution " +
                                               std::to_string(resolution);
                } else {
                    editorPtr->statusMessage = "Could not create the Data component (see log).";
                }
            }
            if (ImGui::MenuItem("Folder")) {
                projv::ComponentHandle child = projv::utils::addComponent(
                    *scenePtr, projv::ComponentKind::Asset, "New Folder", handle,
                    CHUNK_RESOLUTION_CHOICES[0], 1.0f);   // Ignored for an Asset; it owns no voxels.
                if (child != projv::INVALID_COMPONENT_HANDLE) {
                    editorPtr->selectedComponent = child;
                    editorPtr->selectionOutlineValid = false;
                    editorPtr->statusMessage = "Created Folder";
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) {
            projv::ComponentHandle toDelete = handle;
            projv::ComponentHandle oldParent = scenePtr->components[toDelete].parent;
            std::string deletedName = scenePtr->components[toDelete].name;

            if (oldParent < scenePtr->components.size()) {
                std::vector<projv::ComponentHandle>& siblings = scenePtr->components[oldParent].children;
                siblings.erase(std::remove(siblings.begin(), siblings.end(), toDelete), siblings.end());
            }

            auto disableSubtree = [scenePtr](projv::ComponentHandle h, auto& self) -> void {
                if (h >= scenePtr->components.size()) return;
                if (scenePtr->components[h].kind == projv::ComponentKind::Chunk) {
                    projv::ChunkHandle ch = scenePtr->components[h].chunkHandle;
                    if (ch < scenePtr->chunks.size()) {
                        scenePtr->chunks[ch].alive = false;
                        projv::releaseBlob(*scenePtr, scenePtr->chunks[ch].geometryPoolIndex);
                    }
                    auto& loose = scenePtr->looseChunks;
                    loose.erase(std::remove(loose.begin(), loose.end(), ch), loose.end());
                    scenePtr->looseChunkCount = static_cast<uint32_t>(loose.size());
                }
                for (projv::ComponentHandle child : scenePtr->components[h].children) {
                    self(child, self);
                }
                scenePtr->components[h].children.clear();
            };
            disableSubtree(toDelete, disableSubtree);

            scenePtr->components[toDelete].name = "__deleted__";
            scenePtr->components[toDelete].parent = projv::INVALID_COMPONENT_HANDLE;

            if (editorPtr->selectedComponent == toDelete) {
                editorPtr->selectedComponent = projv::INVALID_COMPONENT_HANDLE;
                editorPtr->selectedVoxelCount = 0;
                editorPtr->selectionOutlineValid = false;
            }
            editorPtr->gpuFlushNeeded = true;
            editorPtr->cameraMovedByInterface = true;
            editorPtr->statusMessage = "Deleted " + deletedName;
        }
        ImGui::EndPopup();
    }

    // Folders always push to the ID stack (never NoTreePushOnOpen), so they always need TreePop.
    // Data components use NoTreePushOnOpen and skip TreePop entirely.
    bool showChildren = isFolder ? nodeOpen : (nodeOpen && !component.children.empty());
    if (showChildren) {
        for (projv::ComponentHandle child : component.children) {
            drawHierarchyNode(scene, editor, child);
        }
        ImGui::TreePop();
    }
}

static void drawHierarchyPanel(projv::Scene& scene, EditorState& editor) {
    ImGui::Begin("Scene Hierarchy");

    if (!editor.sceneLoaded) {
        ImGui::TextDisabled("No scene loaded.");
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("%s", editor.scenePath.c_str());

    // --- Deselect on Escape or click on empty space ---
    if (ImGui::IsWindowFocused() && editor.selectedComponent != projv::INVALID_COMPONENT_HANDLE &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        editor.selectedComponent = projv::INVALID_COMPONENT_HANDLE;
        editor.selectedVoxelCount = 0;
        editor.selectionOutlineValid = false;
    }

    // --- Toolbar row ---
    if (ImGui::Button("Create", ImVec2(64.0f, 0.0f))) {
        editor.createNameBuffer[0] = '\0';
        ImGui::OpenPopup("##createComponent");
    }
    if (ImGui::BeginPopup("##createComponent")) {
        // Pick parent: selected folder → child; selected data → sibling; nothing → root.
        projv::ComponentHandle parent = projv::INVALID_COMPONENT_HANDLE;
        if (editor.selectedComponent < scene.components.size()) {
            if (scene.components[editor.selectedComponent].kind == projv::ComponentKind::Asset) {
                parent = editor.selectedComponent;
                ImGui::TextDisabled("Child of %s", scene.components[parent].name.c_str());
            } else {
                parent = scene.components[editor.selectedComponent].parent;
                ImGui::TextDisabled("Sibling of %s", scene.components[editor.selectedComponent].name.c_str());
            }
        } else {
            ImGui::TextDisabled("Root component");
        }

        auto createWithName = [&](projv::ComponentKind kind) {
            std::string name = editor.createNameBuffer[0] ? editor.createNameBuffer : "New Component";
            // Ignored for an Asset, which owns no voxels, but still passed: addComponent takes both
            // without a default so a data component can never be created without a considered grid.
            uint32_t resolution = CHUNK_RESOLUTION_CHOICES[editor.createResolutionIndex];
            projv::ComponentHandle child = projv::utils::addComponent(scene, kind, name, parent,
                                                                      resolution, editor.createVoxelScale);
            if (child != projv::INVALID_COMPONENT_HANDLE) {
                editor.gpuFlushNeeded = true;
                editor.selectedComponent = child;
                editor.selectionOutlineValid = false;
                if (kind == projv::ComponentKind::Chunk) {
                    editor.selectedVoxelCount = projv::utils::getComponentVoxelCount(scene, child);
                    editor.statusMessage = "Created " + name + " at resolution " + std::to_string(resolution);
                } else {
                    editor.statusMessage = "Created " + name;
                }
            } else {
                editor.statusMessage = "Could not create " + name + " (see log).";
            }
        };

        ImGui::SetNextItemWidth(200.0f);
        bool enterPressed = ImGui::InputTextWithHint("##createName", "New Component",
            editor.createNameBuffer, sizeof(editor.createNameBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (enterPressed) {
            createWithName(projv::ComponentKind::Chunk);
            ImGui::CloseCurrentPopup();
        }

        ImGui::Separator();
        drawNewDataComponentOptions(editor);
        ImGui::Separator();

        if (ImGui::MenuItem("Data", "Enter", false, true)) { createWithName(projv::ComponentKind::Chunk); ImGui::CloseCurrentPopup(); }
        if (ImGui::MenuItem("Folder", nullptr, false, true))  { createWithName(projv::ComponentKind::Asset); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::Separator();

    // Drop target on empty space (for reparenting to root).
    float availableHeight = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##hierarchyDropTarget", ImVec2(0.0f, availableHeight));

    for (projv::ComponentHandle handle = 0; handle < scene.components.size(); handle++) {
        if (scene.components[handle].parent == projv::INVALID_COMPONENT_HANDLE &&
            scene.components[handle].name != "__deleted__") {
            drawHierarchyNode(scene, editor, handle);
        }
    }

    // Empty space drop target for reparenting to root.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("COMPONENT")) {
            projv::ComponentHandle draggedHandle = *(const projv::ComponentHandle*)payload->Data;
            projv::ComponentHandle oldParent = scene.components[draggedHandle].parent;
            if (projv::utils::setComponentParent(scene, draggedHandle, projv::INVALID_COMPONENT_HANDLE)) {
                if (editor.selectedComponent == draggedHandle) {
                    editor.selectionOutlineValid = false;
                }
                editor.gpuFlushNeeded = true;
                editor.cameraMovedByInterface = true;

                projv::Scene* sp = &scene;
                EditorState* ep = &editor;
                projv::editor::EditRecord record;
                record.label = "Reparent " + scene.components[draggedHandle].name + " to root";
                record.undo = [sp, draggedHandle, oldParent] {
                    projv::utils::setComponentParent(*sp, draggedHandle, oldParent);
                };
                record.redo = [sp, draggedHandle] {
                    projv::utils::setComponentParent(*sp, draggedHandle, projv::INVALID_COMPONENT_HANDLE);
                };
                ep->history.record(std::move(record), ImGui::GetTime());

                editor.statusMessage = "Moved to root";
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::EndChild();

    // The reveal is consumed whether or not a node acted on it: if the selection no longer exists
    // there is nothing to scroll to, and leaving the flag set would fire on some unrelated node the
    // next time the tree happened to contain the same handle.
    editor.revealSelectionInHierarchy = false;

    ImGui::End();
}

// =============================================================================
// Library
// =============================================================================
//
// The left column's second half: a persistent browser over the folders scenes and assets live in.
//
// It is deliberately separate from the Scene Hierarchy above it rather than being extra roots in the
// same tree. The hierarchy answers "what is in my world" and its rows are renamed, reparented and
// deleted; the library answers "what could be" and its rows are searched and opened. Merging them
// would give one tree two sets of operations that mean different things on rows that look alike.
//
// Nothing here loads geometry. A folder's compose.json is parsed to list what is inside it —
// parseComposeJson reads the JSON and stops — so browsing a 3 GB scene costs a file read, not a GPU
// upload. That is also why "bring this into the open scene" is not offered yet: see the note the
// panel itself carries.

static void setLibraryDirectory(EditorState& editor, const std::string& directory) {
    editor.libraryDirectory = directory;
    editor.librarySelection.clear();
    editor.libraryPreviewValid = false;
    editor.libraryPreview = projv::ComposeDoc();
    editor.libraryPreviewPath.clear();
    std::snprintf(editor.libraryPathBuffer, sizeof(editor.libraryPathBuffer), "%s", directory.c_str());
}

static void drawLibraryPanel(EditorState& editor) {
    ImGui::Begin("Library");

    std::error_code errorCode;
    std::filesystem::path currentDirectory = editor.libraryDirectory;

    // --- Path row ---
    if (ImGui::Button("Up") && currentDirectory.has_parent_path()) {
        setLibraryDirectory(editor, currentDirectory.parent_path().string());
        currentDirectory = editor.libraryDirectory;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##libraryPath", editor.libraryPathBuffer, sizeof(editor.libraryPathBuffer),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (std::filesystem::is_directory(editor.libraryPathBuffer, errorCode)) {
            setLibraryDirectory(editor, editor.libraryPathBuffer);
            currentDirectory = editor.libraryDirectory;
        } else {
            editor.statusMessage = std::string("Not a folder: ") + editor.libraryPathBuffer;
        }
    }

    ImGui::Separator();

    // --- Listing ---
    // Collected first so it can be sorted: the filesystem hands entries back in whatever order it
    // stores them, which is not an order anyone can scan.
    std::vector<std::filesystem::path> subdirectories;
    if (std::filesystem::is_directory(currentDirectory, errorCode)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(currentDirectory,
                                                 std::filesystem::directory_options::skip_permission_denied,
                                                 errorCode)) {
            if (entry.is_directory(errorCode)) {
                subdirectories.push_back(entry.path());
            }
        }
    }
    std::sort(subdirectories.begin(), subdirectories.end());

    std::string directoryToOpen;
    std::string sceneToLoad;

    // The listing takes what is left after the preview pane below it, which is given a fixed share
    // rather than being allowed to grow: the list is the part being scanned.
    float previewHeight = ImGui::GetFrameHeightWithSpacing() * 5.0f;
    ImGui::BeginChild("##libraryListing", ImVec2(0.0f, -previewHeight), ImGuiChildFlags_Borders);
    if (subdirectories.empty()) {
        ImGui::TextDisabled("No folders here.");
    }
    for (const std::filesystem::path& subdirectory : subdirectories) {
        bool isScene = directoryHoldsScene(subdirectory);
        std::string label = (isScene ? "[scene]  " : "[dir]    ") + subdirectory.filename().string();

        // Scene folders are tinted, so the thing being looked for stands out from the folders that
        // are only on the way to it.
        if (isScene) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 1.00f, 1.00f));
        }
        bool isSelected = (editor.librarySelection == subdirectory.string());
        if (ImGui::Selectable(label.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
            // A click selects (and previews); a double click commits — loading a scene folder, or
            // stepping into an ordinary one. Same contract as the Load Scene dialog, so the two
            // browsers do not disagree about what a click does.
            editor.librarySelection = subdirectory.string();
            editor.libraryPreviewValid = false;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (isScene) {
                    sceneToLoad = subdirectory.string();
                } else {
                    directoryToOpen = subdirectory.string();
                }
            }
        }
        if (isScene) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    // --- Preview of the selected folder ---
    bool selectionIsScene = !editor.librarySelection.empty() &&
                            directoryHoldsScene(editor.librarySelection);
    if (selectionIsScene && (!editor.libraryPreviewValid ||
                             editor.libraryPreviewPath != editor.librarySelection)) {
        editor.libraryPreview = projv::utils::parseComposeJson(editor.librarySelection + "/compose.json");
        editor.libraryPreviewPath = editor.librarySelection;
        editor.libraryPreviewValid = true;
    }

    if (!selectionIsScene) {
        ImGui::TextDisabled("Select a [scene] to see what is in it.");
        ImGui::TextDisabled("Double-click a folder to open it.");
    } else {
        std::filesystem::path selectionPath(editor.librarySelection);
        ImGui::Text("%s", selectionPath.filename().string().c_str());
        ImGui::TextDisabled("%zu top-level component(s)", editor.libraryPreview.components.size());

        ImGui::BeginChild("##libraryPreview", ImVec2(0.0f, ImGui::GetFrameHeightWithSpacing() * 2.0f));
        for (const projv::ComposeComponent& component : editor.libraryPreview.components) {
            const char* kind = component.type == projv::ComponentType::Asset ? "folder" : "data";
            ImGui::BulletText("%s", component.name.empty() ? component.source.c_str()
                                                           : component.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", kind);
        }
        ImGui::EndChild();

        if (ImGui::Button("Load scene")) {
            sceneToLoad = editor.librarySelection;
        }
        ImGui::SameLine();
        // Stated rather than hidden, because "why can I see it but not use it" is the question the
        // panel would otherwise leave hanging. Copying a component between scenes means appending
        // another Scene's geometry blobs, palette and chunk records into this one and remapping every
        // handle they carry — engine work that does not exist yet, and not something to fake here.
        ImGui::BeginDisabled(true);
        ImGui::Button("Import into scene");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Not yet: copying a component between scenes needs a cross-scene\n"
                              "geometry merge in the engine (blobs, palette, and handle remapping).\n"
                              "Loading the scene works today.");
        }
    }

    if (!directoryToOpen.empty()) {
        setLibraryDirectory(editor, directoryToOpen);
    }
    if (!sceneToLoad.empty()) {
        // Deferred exactly as the dialog defers it: the load destroys and rebuilds GPU resources,
        // which has no business running in the middle of laying out a window.
        editor.pendingScenePath = sceneToLoad;
    }

    ImGui::End();
}

// Applies a local transform and marks the GPU mirror for a header-only flush. Shared by the drag
// controls below and their undo/redo closures, so an undone drag takes exactly the path the drag
// took -- and matches applyMaterialColor's shape for the same reason.
static void applyComponentTransform(projv::Scene* scene, EditorState* editor, projv::ComponentHandle component,
                                    projv::core::vec3 localPos, projv::core::quat localRot, float localScale) {
    projv::utils::setComponentTransform(*scene, component, localPos, localRot, localScale);
    editor->gpuFlushNeeded = true;   // Only header rows change (position/rotation/scale), not geometry
                                     // -- flushSceneUpdates' updateDirtyHeaders is what's cheap here.
    editor->cameraMovedByInterface = true;   // The accumulated image is of the old geometry position.
}

// Finds the centre of the selection's bounding box in the component's own *untransformed* local
// space -- the frame its geometry sits in before localPosition/localRotation/localScale are applied.
//
// Local rather than world on purpose. A chunk's transform origin is its minimum corner (see
// drawChunkOutline), so the world bounding box slides and swells as the component is rotated and
// scaled; a pivot derived from it would drift under the user's own drag. The local box does not move
// at all under a transform edit, which is exactly the invariant a pivot needs.
//
// Returns false when the selection has no live geometry to measure (an empty Asset folder, say),
// in which case the caller should fall back to the origin pivot.
static bool computeLocalPivot(const projv::Scene& scene, projv::ComponentHandle handle,
                              const std::vector<projv::ChunkHandle>& leafChunks,
                              projv::core::vec3& outPivot) {
    using namespace projv::core;
    if (leafChunks.empty()) return false;

    mat4 worldToLocal = glm::inverse(projv::utils::getComponentWorldMatrix(scene, handle));

    vec3 boundsMin(0.0f), boundsMax(0.0f);
    bool haveBounds = false;
    for (projv::ChunkHandle chunkHandle : leafChunks) {
        if (chunkHandle >= scene.chunks.size()) continue;
        const projv::Chunk& chunk = scene.chunks[chunkHandle];
        if (!chunk.alive || chunk.header.scale <= 0.0f) continue;

        // The same eight world corners the yellow outline is drawn through, pulled back into local
        // space. All eight are needed rather than just the two extremes: a rotated chunk's corners
        // do not stay axis-aligned on the way through the inverse.
        mat3 rotation = glm::mat3_cast(chunk.header.rotation);
        for (int i = 0; i < 8; i++) {
            vec3 localOffset((i & 1) ? chunk.header.scale : 0.0f,
                             (i & 2) ? chunk.header.scale : 0.0f,
                             (i & 4) ? chunk.header.scale : 0.0f);
            vec3 corner = vec3(worldToLocal * vec4(chunk.header.position + rotation * localOffset, 1.0f));
            if (!haveBounds) {
                boundsMin = corner;
                boundsMax = corner;
                haveBounds = true;
            } else {
                boundsMin = glm::min(boundsMin, corner);
                boundsMax = glm::max(boundsMax, corner);
            }
        }
    }
    if (!haveBounds) return false;

    outPivot = (boundsMin + boundsMax) * 0.5f;
    return true;
}

// Rewrites localPosition so that changing a component's rotation and/or scale leaves `pivot` (a
// point in the component's untransformed local space) sitting exactly where it already was.
//
// The local transform maps a local point p into parent space as
//     L(p) = localPosition + localScale * (localRotation * p)
// and the whole trick is to solve L_new(pivot) == L_old(pivot) for the one unknown:
//     localPosition_new = L_old(pivot) - scale_new * (rotation_new * pivot)
//
// So an object rotates and scales in place instead of swinging around, and grows out of, its minimum
// corner. Nothing about the underlying transform convention changes -- this is purely the position
// the user would otherwise have had to dial in by hand afterwards.
//
// The base transform is passed explicitly rather than read off the component because the two callers
// need different bases. An Inspector field drag compensates against the *current* values, one small
// delta per frame, which telescopes exactly. A gizmo drag computes its rotation absolutely from where
// the drag started, so it must compensate against the drag-start values too -- feeding it the current
// ones would apply each frame's compensation on top of the last and walk the component away.
static projv::core::vec3 pivotCompensatedPosition(projv::core::vec3 basePosition,
                                                  projv::core::quat baseRotation, float baseScale,
                                                  projv::core::vec3 pivot,
                                                  projv::core::quat newRotation, float newScale) {
    projv::core::vec3 pivotInParent = basePosition + baseScale * (baseRotation * pivot);
    return pivotInParent - newScale * (newRotation * pivot);
}

// Rebuilds whatever the selection's derived caches need, for any panel that is about to read them.
// Called by the viewport and the Inspector both, because the viewport draws first and would
// otherwise be reading a pivot the Inspector had not computed yet on the frame after a selection
// change (and, being first, it is also the one that rebuilds the leaf set).
static void refreshSelectionCaches(const projv::Scene& scene, EditorState& editor) {
    if (editor.selectedComponent == projv::INVALID_COMPONENT_HANDLE ||
        editor.selectedComponent >= scene.components.size()) {
        return;
    }
    if (!editor.selectionOutlineValid) {
        editor.selectionOutlineChunks.clear();
        collectLeafChunks(scene, editor.selectedComponent, editor.selectionOutlineChunks);
        editor.selectionOutlineValid = true;
        editor.selectionOutlineGeneration++;
    }
    if (editor.inspectorPivotGeneration != editor.selectionOutlineGeneration) {
        editor.inspectorPivotGeneration = editor.selectionOutlineGeneration;
        editor.inspectorPivotValid = computeLocalPivot(scene, editor.selectedComponent,
                                                       editor.selectionOutlineChunks,
                                                       editor.inspectorPivotLocal);
    }
}

static void drawInspectorPanel(projv::Scene& scene, EditorState& editor) {
    ImGui::Begin("Inspector");

    if (!editor.sceneLoaded || editor.selectedComponent == projv::INVALID_COMPONENT_HANDLE ||
        editor.selectedComponent >= scene.components.size()) {
        ImGui::TextDisabled("Select a component in the Scene Hierarchy.");
        ImGui::End();
        return;
    }

    projv::ComponentRecord& component = scene.components[editor.selectedComponent];
    projv::core::vec3 worldPosition = projv::utils::getComponentWorldPosition(scene, editor.selectedComponent);
    projv::core::quat worldRotation = projv::utils::getComponentWorldRotation(scene, editor.selectedComponent);

    ImGui::Text("%s", component.name.c_str());
    ImGui::TextDisabled("%s", projv::utils::getComponentPath(scene, editor.selectedComponent).c_str());
    ImGui::Separator();

    ImGui::Text("Handle       %u", editor.selectedComponent);
    ImGui::Text("Kind         %s", component.kind == projv::ComponentKind::Asset ? "Asset"
                                 : component.kind == projv::ComponentKind::Grid ? "Grid" : "Chunk");
    if (!component.sourcePath.empty()) {
        ImGui::TextWrapped("Source       %s", component.sourcePath.c_str());
    }
    if (component.kind != projv::ComponentKind::Asset) {
        ImGui::Text("Voxels       %u", editor.selectedVoxelCount);
        // Read-only, and not merely because there is no setter: resolution fixes the tree64 depth and
        // voxelScale fixes the world size of a cell, so changing either after the fact is a resample
        // of the whole volume. Both are chosen once, at creation. See utils::addComponent.
        if (component.kind == projv::ComponentKind::Chunk &&
            component.chunkHandle < scene.chunks.size()) {
            const projv::Chunk& chunk = scene.chunks[component.chunkHandle];
            ImGui::Text("Resolution   %u", chunk.header.resolution);
            ImGui::Text("Voxel scale  %.3f", chunk.header.voxelScale);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Fixed when the component was created - changing either would\n"
                                  "resample the volume rather than edit it.");
            }
        }
    }
    // Same set the yellow viewport outline is drawn around -- one .data box for a Chunk, one per
    // live cell for a Grid, every leaf beneath an Asset folder.
    refreshSelectionCaches(scene, editor);
    ImGui::Text("Boxes        %zu", editor.selectionOutlineChunks.size());

    ImGui::Separator();
    ImGui::TextDisabled("Local transform");

    // Rotation is edited as Euler degrees; refreshed from the quaternion only on a selection change
    // (see the field's comment in EditorState) so a live drag isn't fighting a per-frame rebuild.
    bool selectionChanged = editor.lastInspectorSelection != editor.selectedComponent;
    if (selectionChanged) {
        editor.lastInspectorSelection = editor.selectedComponent;
        projv::core::vec3 radians = glm::eulerAngles(component.localRotation);
        editor.inspectorEulerDegrees[0] = glm::degrees(radians.x);
        editor.inspectorEulerDegrees[1] = glm::degrees(radians.y);
        editor.inspectorEulerDegrees[2] = glm::degrees(radians.z);
    }
    bool usePivot = editor.pivotAtCenter && editor.inspectorPivotValid;

    projv::Scene* scenePointer = &scene;
    EditorState* editorPointer = &editor;
    projv::ComponentHandle handle = editor.selectedComponent;

    ImGui::SetNextItemWidth(-1.0f);
    projv::core::vec3 pos = component.localPosition;
    if (ImGui::DragFloat3("##localPos", &pos.x, 0.05f, 0.0f, 0.0f, "%.3f")) {
        projv::core::vec3 previousPos = component.localPosition;
        applyComponentTransform(&scene, &editor, handle, pos, component.localRotation, component.localScale);
        projv::editor::EditRecord record;
        record.label = "Move " + component.name;
        record.coalesceKey = "transform:pos:" + std::to_string(handle);
        record.undo = [=] { applyComponentTransform(scenePointer, editorPointer, handle, previousPos,
                                                     scenePointer->components[handle].localRotation,
                                                     scenePointer->components[handle].localScale); };
        record.redo = [=] { applyComponentTransform(scenePointer, editorPointer, handle, pos,
                                                     scenePointer->components[handle].localRotation,
                                                     scenePointer->components[handle].localScale); };
        editor.history.record(std::move(record), ImGui::GetTime());
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextDisabled("Position");

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::DragFloat3("##localRot", editor.inspectorEulerDegrees, 0.5f, 0.0f, 0.0f, "%.1f")) {
        projv::core::quat previousRot = component.localRotation;
        projv::core::vec3 previousPos = component.localPosition;
        projv::core::vec3 eulerRadians(glm::radians(editor.inspectorEulerDegrees[0]),
                                       glm::radians(editor.inspectorEulerDegrees[1]),
                                       glm::radians(editor.inspectorEulerDegrees[2]));
        projv::core::quat newRot(eulerRadians);
        // Position rides along with the rotation when pivoting about the centre, so both are
        // captured and both are restored -- an undo that put the rotation back but left the
        // compensated position would leave the component somewhere it has never been.
        projv::core::vec3 newPos = usePivot
            ? pivotCompensatedPosition(previousPos, previousRot, component.localScale,
                                       editor.inspectorPivotLocal, newRot, component.localScale)
            : previousPos;
        applyComponentTransform(&scene, &editor, handle, newPos, newRot, component.localScale);
        projv::editor::EditRecord record;
        record.label = "Rotate " + component.name;
        record.coalesceKey = "transform:rot:" + std::to_string(handle);
        record.undo = [=] { applyComponentTransform(scenePointer, editorPointer, handle, previousPos,
                                                     previousRot, scenePointer->components[handle].localScale); };
        record.redo = [=] { applyComponentTransform(scenePointer, editorPointer, handle, newPos,
                                                     newRot, scenePointer->components[handle].localScale); };
        editor.history.record(std::move(record), ImGui::GetTime());
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextDisabled("Rotation (deg)");

    ImGui::SetNextItemWidth(-1.0f);
    float scale = component.localScale;
    // Uniform scale only (v0.0) -- matches ComponentRecord::localScale and the compose.json loader,
    // which rejects a non-uniform scale on a data leaf outright.
    if (ImGui::DragFloat("##localScale", &scale, 0.01f, 0.001f, 1000.0f, "%.3f")) {
        float previousScale = component.localScale;
        projv::core::vec3 previousPos = component.localPosition;
        projv::core::vec3 newPos = usePivot
            ? pivotCompensatedPosition(previousPos, component.localRotation, previousScale,
                                       editor.inspectorPivotLocal, component.localRotation, scale)
            : previousPos;
        applyComponentTransform(&scene, &editor, handle, newPos, component.localRotation, scale);
        projv::editor::EditRecord record;
        record.label = "Scale " + component.name;
        record.coalesceKey = "transform:scale:" + std::to_string(handle);
        record.undo = [=] { applyComponentTransform(scenePointer, editorPointer, handle, previousPos,
                                                     scenePointer->components[handle].localRotation, previousScale); };
        record.redo = [=] { applyComponentTransform(scenePointer, editorPointer, handle, newPos,
                                                     scenePointer->components[handle].localRotation, scale); };
        editor.history.record(std::move(record), ImGui::GetTime());
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextDisabled("Scale");

    ImGui::Checkbox("Pivot at center", &editor.pivotAtCenter);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Rotate and scale about the selection's bounding-box center.\n"
                          "Off: about the component's local origin, which for a chunk is its\n"
                          "minimum corner -- the raw transform.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("World transform (read-only)");
    ImGui::Text("Position  %.2f, %.2f, %.2f", worldPosition.x, worldPosition.y, worldPosition.z);
    ImGui::Text("Rotation  %.3f, %.3f, %.3f, %.3f",
                worldRotation.w, worldRotation.x, worldRotation.y, worldRotation.z);

    ImGui::Separator();
    if (ImGui::Button("Look at this component")) {
        // Keep the current viewing angle and distance, and slide the camera so the component sits
        // where the camera is already pointing.
        projv::core::vec3 viewDirection = computeCameraDirection(editor);
        float distance = std::max(editor.framing.moveSpeed * 60.0f, 1.0f);
        editor.cameraPosition = worldPosition - viewDirection * distance;
        editor.cameraMovedByInterface = true;
    }
    if (ImGui::Button("Reset transform")) {
        // Captured before the reset overwrites them -- component is a live reference into
        // scene.components[handle], so this order matters.
        projv::core::vec3 previousPos = component.localPosition;
        projv::core::quat previousRot = component.localRotation;
        float previousScale = component.localScale;
        applyComponentTransform(&scene, &editor, handle, projv::core::vec3(0.0f),
                                projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f);
        editor.lastInspectorSelection = projv::INVALID_COMPONENT_HANDLE;   // Force the Euler cache to refresh.
        projv::editor::EditRecord record;
        record.label = "Reset transform of " + component.name;
        record.undo = [=] { applyComponentTransform(scenePointer, editorPointer, handle, previousPos, previousRot, previousScale); };
        record.redo = [=] { applyComponentTransform(scenePointer, editorPointer, handle, projv::core::vec3(0.0f),
                                                     projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f); };
        editor.history.record(std::move(record), ImGui::GetTime());
    }

    ImGui::End();
}

// =============================================================================
// Palette editing
// =============================================================================

// The palette stores R10G10B10 — ten bits per channel, which is what the shader decodes and what a
// voxelizer writes. voxel.h's packColor takes 8-bit channels and multiplies by four, so round-
// tripping an edit through it would quantize every colour to 8 bits and lose the low two bits of
// whatever the voxelizer originally sampled. These two keep the full ten.
static uint32_t packColorFromFloats(const float color[3]) {
    uint32_t red   = uint32_t(std::lround(std::clamp(color[0], 0.0f, 1.0f) * 1023.0f));
    uint32_t green = uint32_t(std::lround(std::clamp(color[1], 0.0f, 1.0f) * 1023.0f));
    uint32_t blue  = uint32_t(std::lround(std::clamp(color[2], 0.0f, 1.0f) * 1023.0f));
    return (red << 20) | (green << 10) | blue;
}

static void unpackColorToFloats(uint32_t packedColor, float color[3]) {
    color[0] = float((packedColor >> 20) & 0x3FF) / 1023.0f;
    color[1] = float((packedColor >> 10) & 0x3FF) / 1023.0f;
    color[2] = float((packedColor >>  0) & 0x3FF) / 1023.0f;
}

static ImVec4 materialSwatchColor(uint32_t packedColor) {
    float color[3];
    unpackColorToFloats(packedColor, color);
    return ImVec4(color[0], color[1], color[2], 1.0f);
}

// Voxel counts run to seven digits and the grid cell is 34 pixels wide, so they are shortened to
// fit. The exact figure is a hover away.
static std::string formatCompactCount(uint32_t count) {
    char buffer[16];
    if (count < 1000) {
        std::snprintf(buffer, sizeof(buffer), "%u", count);
    } else if (count < 1000000) {
        std::snprintf(buffer, sizeof(buffer), "%.0fk", double(count) / 1000.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1fM", double(count) / 1000000.0);
    }
    return buffer;
}

// The eyedropper, drawn rather than glyphed: ImGui's default font is ASCII, and pulling in an icon
// font for one button is not worth the atlas.
static void drawEyedropperIcon(ImDrawList* drawList, ImVec2 topLeft, float size, ImU32 color) {
    float unit = size / 20.0f;
    ImVec2 tip     = ImVec2(topLeft.x + 3.0f * unit,  topLeft.y + 17.0f * unit);
    ImVec2 shaftLo = ImVec2(topLeft.x + 7.0f * unit,  topLeft.y + 13.0f * unit);
    ImVec2 shaftHi = ImVec2(topLeft.x + 14.0f * unit, topLeft.y + 6.0f * unit);
    ImVec2 bulb    = ImVec2(topLeft.x + 15.5f * unit, topLeft.y + 4.5f * unit);

    drawList->AddLine(shaftLo, shaftHi, color, 2.5f * unit);
    drawList->AddCircleFilled(bulb, 3.0f * unit, color, 12);
    drawList->AddTriangleFilled(tip,
                                ImVec2(shaftLo.x + 1.0f * unit, shaftLo.y + 1.0f * unit),
                                ImVec2(shaftLo.x - 2.0f * unit, shaftLo.y - 2.0f * unit), color);
}

// The cached usage counts and per-chunk breakdowns describe a palette that just changed, so every
// edit — and every undo of one — drops them.
static void invalidatePaletteCaches(EditorState* editor) {
    editor->materialUsageValid = false;
    editor->materialChunkUsageValid = false;
}

// Applies a colour to a slot and pushes it to the GPU by the cheap route. Shared by the picker and
// by the undo/redo closures, so an undone drag takes exactly the path the drag took.
static void applyMaterialColor(projv::Scene* scene, projv::GPUData* gpuData, EditorState* editor,
                               projv::ComponentHandle component, uint8_t slot, uint32_t packedColor) {
    if (projv::utils::setMaterialColor(*scene, component, slot, packedColor)) {
        projv::graphics::updatePaletteEntry(*scene, *gpuData, component, slot);
        editor->cameraMovedByInterface = true;
    }
}

static void applyMaterialName(projv::Scene* scene, EditorState* editor,
                              projv::ComponentHandle component, uint8_t slot, const std::string& name) {
    projv::utils::setMaterialName(*scene, component, slot, name);
    if (component == editor->paletteComponent && int(slot) == editor->selectedMaterialSlot) {
        std::snprintf(editor->materialNameBuffer, sizeof(editor->materialNameBuffer), "%s", name.c_str());
    }
}

// Undo of an "add": the entry is always the last one, and nothing can reference it — a slot only
// acquires voxels through operations that would themselves be undone first.
static void popLastMaterial(projv::Scene* scene, EditorState* editor, projv::ComponentHandle component) {
    if (component >= scene->components.size()) return;
    {
        std::lock_guard<std::mutex> lock(scene->materialPaletteMutex);
        projv::ComponentRecord& record = scene->components[component];
        if (record.materialPalette.empty()) return;
        record.materialPalette.pop_back();
        record.paletteVersion++;
    }
    if (editor->selectedMaterialSlot >= int(scene->components[component].materialPalette.size())) {
        editor->selectedMaterialSlot = -1;
    }
    editor->gpuFlushNeeded = true;
    invalidatePaletteCaches(editor);
}

// Points the panel at a component's palette and drops the cached usage counts, which belong to
// whichever palette was being shown before.
static void selectPaletteComponent(EditorState& editor, projv::ComponentHandle componentHandle) {
    editor.paletteComponent = componentHandle;
    editor.selectedMaterialSlot = -1;
    editor.materialUsageValid = false;
    editor.materialChunkUsageValid = false;
    editor.materialNameBuffer[0] = '\0';
}

// Removes a palette slot and records the undo step for it. The undo is a snapshot rather than an
// inverse operation: the removal renumbers every slot above it and rewrites the material byte of
// every voxel that referenced them, and — where geometry was shared with another component — forks
// blobs and repoints chunks. None of that can be reconstructed from the palette afterwards.
static bool removeMaterialWithUndo(projv::Scene& scene, EditorState& editor, uint8_t slot,
                                   uint8_t replacementSlot) {
    projv::ComponentHandle component = editor.paletteComponent;
    auto snapshot = std::make_shared<projv::editor::PaletteSnapshot>(
        projv::editor::capturePaletteSnapshot(scene, component, true));

    if (!projv::utils::removeMaterial(scene, component, slot, replacementSlot)) {
        return false;
    }

    editor.selectedMaterialSlot = -1;
    invalidatePaletteCaches(&editor);
    editor.gpuFlushNeeded = true;

    projv::Scene* scenePointer = &scene;
    EditorState* editorPointer = &editor;
    projv::editor::EditRecord record;
    record.label = "Remove entry " + std::to_string(slot);
    record.memoryCost = snapshot->memoryCost();
    record.undo = [=] {
        projv::editor::restorePaletteSnapshot(*scenePointer, *snapshot);
        editorPointer->gpuFlushNeeded = true;
        editorPointer->selectedMaterialSlot = -1;
        invalidatePaletteCaches(editorPointer);
    };
    record.redo = [=] {
        projv::utils::removeMaterial(*scenePointer, component, slot, replacementSlot);
        editorPointer->gpuFlushNeeded = true;
        editorPointer->selectedMaterialSlot = -1;
        invalidatePaletteCaches(editorPointer);
    };
    editor.history.record(std::move(record), ImGui::GetTime());
    return true;
}

// The removed-entry dialog. Reachable only when the slot is actually used: with nothing referencing
// it, removal needs no decision from anyone.
static void drawMaterialRemovalDialog(projv::Scene& scene, EditorState& editor) {
    if (!ImGui::BeginPopupModal("Reassign voxels", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    projv::ComponentRecord& component = scene.components[editor.paletteComponent];
    int slot = editor.selectedMaterialSlot;
    if (slot < 0 || size_t(slot) >= component.materialPalette.size()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    uint32_t usage = size_t(slot) < editor.materialUsage.size() ? editor.materialUsage[slot] : 0;
    ImGui::Text("%u voxel(s) use slot %d (%s).", usage, slot, component.materialPalette[slot].name.c_str());
    ImGui::TextDisabled("Removing it renumbers every slot above it. Those voxels become:");
    ImGui::Spacing();

    // The replacement cannot be the slot being removed, so the first different slot is the default.
    if (editor.materialRemovalReplacement == slot) {
        editor.materialRemovalReplacement = (slot == 0) ? 1 : 0;
    }

    const projv::Material& replacement = component.materialPalette[editor.materialRemovalReplacement];
    std::string preview = std::to_string(editor.materialRemovalReplacement) + "  " + replacement.name;
    ImGui::ColorButton("##replacementSwatch", materialSwatchColor(replacement.packedColor),
                       ImGuiColorEditFlags_NoTooltip, ImVec2(20.0f, 20.0f));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::BeginCombo("##replacementSlot", preview.c_str())) {
        for (size_t candidate = 0; candidate < component.materialPalette.size(); candidate++) {
            if (int(candidate) == slot) continue;
            const projv::Material& material = component.materialPalette[candidate];
            ImGui::ColorButton(("##candidate" + std::to_string(candidate)).c_str(),
                               materialSwatchColor(material.packedColor),
                               ImGuiColorEditFlags_NoTooltip, ImVec2(16.0f, 16.0f));
            ImGui::SameLine();
            std::string label = std::to_string(candidate) + "  " + material.name;
            if (ImGui::Selectable(label.c_str(), int(candidate) == editor.materialRemovalReplacement)) {
                editor.materialRemovalReplacement = int(candidate);
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    if (ImGui::Button("Remove", ImVec2(120.0f, 0.0f))) {
        if (removeMaterialWithUndo(scene, editor, uint8_t(slot), uint8_t(editor.materialRemovalReplacement))) {
            editor.statusMessage = "Removed palette slot " + std::to_string(slot) + " (" +
                                   std::to_string(usage) + " voxel(s) reassigned)";
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

static void drawPalettePanel(projv::Scene& scene, projv::GPUData& gpuData, EditorState& editor) {
    ImGui::Begin("Palette");

    if (!editor.sceneLoaded) {
        ImGui::TextDisabled("No scene loaded.");
        ImGui::End();
        return;
    }

    // Selecting a component in the hierarchy points the palette panel at it, so the two panels agree
    // without the user having to say so twice. An explicit choice in the combo below still holds
    // until the hierarchy selection changes again.
    if (editor.selectedComponent != editor.lastHierarchySelection) {
        editor.lastHierarchySelection = editor.selectedComponent;
        if (editor.selectedComponent < scene.components.size() &&
            !scene.components[editor.selectedComponent].materialPalette.empty()) {
            selectPaletteComponent(editor, editor.selectedComponent);
        }
    }
    if (editor.paletteComponent >= scene.components.size() ||
        scene.components[editor.paletteComponent].materialPalette.empty()) {
        // Fall back to the first component that has a palette — in a single-model scene that is the
        // only sensible target, and it means the panel is never blank for no reason.
        projv::ComponentHandle fallback = projv::INVALID_COMPONENT_HANDLE;
        for (projv::ComponentHandle handle = 0; handle < scene.components.size(); handle++) {
            if (scene.components[handle].name == "__deleted__") continue;
            if (!scene.components[handle].materialPalette.empty()) { fallback = handle; break; }
        }
        if (fallback == projv::INVALID_COMPONENT_HANDLE) {
            ImGui::TextDisabled("No component in this scene has a material palette.");
            ImGui::End();
            return;
        }
        if (fallback != editor.paletteComponent) selectPaletteComponent(editor, fallback);
    }

    projv::ComponentRecord& component = scene.components[editor.paletteComponent];

    // --- Which component's palette ---
    std::string componentPreview = component.name.empty()
        ? ("component " + std::to_string(editor.paletteComponent)) : component.name;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##paletteComponent", componentPreview.c_str())) {
        for (projv::ComponentHandle handle = 0; handle < scene.components.size(); handle++) {
            const projv::ComponentRecord& candidate = scene.components[handle];
            if (candidate.name == "__deleted__") continue;
            if (candidate.materialPalette.empty()) continue;
            std::string label = (candidate.name.empty() ? ("component " + std::to_string(handle)) : candidate.name) +
                                "  (" + std::to_string(candidate.materialPalette.size()) + ")";
            if (ImGui::Selectable(label.c_str(), handle == editor.paletteComponent)) {
                selectPaletteComponent(editor, handle);
                editor.selectedComponent = handle;
                editor.lastHierarchySelection = handle;
                editor.selectedVoxelCount = projv::utils::getComponentVoxelCount(scene, handle);
                editor.selectionOutlineValid = false;
            }
        }
        ImGui::EndCombo();
    }

    if (!editor.materialUsageValid) {
        editor.materialUsage = projv::utils::countMaterialUsage(scene, editor.paletteComponent);
        editor.materialUsageValid = true;
    }

    // 255, not 256: material IDs are uint8_t and 255 is the "no material" sentinel.
    ImGui::TextDisabled("%zu / %u entries", component.materialPalette.size(),
                        projv::MAX_MATERIALS_PER_COMPONENT - 1);

    // --- Eyedropper ---
    // Armed here, fired by the next click in the viewport (see drawViewportPanel / processVoxelPick).
    ImGui::SameLine();
    float toolbarSize = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - toolbarSize + ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
    ImVec2 pickerButtonPos = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##materialPicker", ImVec2(toolbarSize, toolbarSize))) {
        editor.materialPickerActive = !editor.materialPickerActive;
    }
    bool pickerHovered = ImGui::IsItemHovered();
    ImU32 pickerBackground = editor.materialPickerActive ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                           : pickerHovered               ? ImGui::GetColorU32(ImGuiCol_ButtonHovered)
                                                         : ImGui::GetColorU32(ImGuiCol_Button);
    ImGui::GetWindowDrawList()->AddRectFilled(pickerButtonPos,
        ImVec2(pickerButtonPos.x + toolbarSize, pickerButtonPos.y + toolbarSize), pickerBackground, 3.0f);
    drawEyedropperIcon(ImGui::GetWindowDrawList(), pickerButtonPos, toolbarSize,
                       ImGui::GetColorU32(ImGuiCol_Text));
    if (pickerHovered) {
        ImGui::SetTooltip("Pick material from a voxel\nClick a voxel in the viewport to select its palette entry.");
    }

    ImGui::Separator();

    // --- The entries, as a grid of swatches ---
    // A palette is a set of colours, and colours are what the eye scans for — so the grid shows the
    // colour at size with only its usage count under it, and leaves names to the detail pane below.
    float editorHeight = ImGui::GetFrameHeightWithSpacing() * 3.0f + 270.0f;
    ImGui::BeginChild("##paletteEntries", ImVec2(0.0f, -editorHeight), ImGuiChildFlags_Borders);

    const float SWATCH_SIZE = 34.0f;
    float cellWidth = SWATCH_SIZE + ImGui::GetStyle().ItemSpacing.x;
    int columns = std::max(1, int(ImGui::GetContentRegionAvail().x / cellWidth));

    for (size_t slot = 0; slot < component.materialPalette.size(); slot++) {
        const projv::Material& material = component.materialPalette[slot];
        uint32_t usage = slot < editor.materialUsage.size() ? editor.materialUsage[slot] : 0;

        ImGui::PushID(int(slot));
        ImGui::BeginGroup();

        ImGui::ColorButton("##swatch", materialSwatchColor(material.packedColor),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(SWATCH_SIZE, SWATCH_SIZE));
        if (ImGui::IsItemClicked()) {
            editor.selectedMaterialSlot = int(slot);
            editor.materialChunkUsageValid = false;
            std::snprintf(editor.materialNameBuffer, sizeof(editor.materialNameBuffer), "%s", material.name.c_str());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%zu  %s\n%u voxel(s)", slot,
                              material.name.empty() ? "(unnamed)" : material.name.c_str(), usage);
        }
        // The selection ring is drawn rather than styled: ColorButton has no selected state, and a
        // ring reads at a glance across a grid of sixty swatches.
        if (int(slot) == editor.selectedMaterialSlot) {
            ImVec2 ringMin = ImGui::GetItemRectMin();
            ImVec2 ringMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(ImVec2(ringMin.x - 2.0f, ringMin.y - 2.0f),
                                                ImVec2(ringMax.x + 2.0f, ringMax.y + 2.0f),
                                                IM_COL32(255, 255, 255, 230), 3.0f, 0, 2.0f);
        }

        // Count under the swatch, centred, dimmed when nothing uses the entry.
        std::string countText = formatCompactCount(usage);
        float countWidth = ImGui::CalcTextSize(countText.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (SWATCH_SIZE - countWidth) * 0.5f));
        if (usage == 0) {
            ImGui::TextDisabled("%s", countText.c_str());
        } else {
            ImGui::TextUnformatted(countText.c_str());
        }

        ImGui::EndGroup();
        ImGui::PopID();

        if (int((slot + 1) % size_t(columns)) != 0 && slot + 1 < component.materialPalette.size()) {
            ImGui::SameLine();
        }
    }
    ImGui::EndChild();

    // --- Add / remove ---
    bool paletteIsFull = component.materialPalette.size() >= projv::MAX_MATERIALS_PER_COMPONENT - 1;
    ImGui::BeginDisabled(paletteIsFull);
    if (ImGui::Button("Add entry")) {
        std::string name = "material " + std::to_string(component.materialPalette.size());
        uint8_t newSlot = projv::utils::addMaterial(scene, editor.paletteComponent, name, 0x3FFFFFFF /* white */);
        if (newSlot != projv::INVALID_MATERIAL) {
            editor.selectedMaterialSlot = int(newSlot);
            std::snprintf(editor.materialNameBuffer, sizeof(editor.materialNameBuffer), "%s", name.c_str());
            invalidatePaletteCaches(&editor);
            editor.gpuFlushNeeded = true;

            projv::Scene* scenePointer = &scene;
            EditorState* editorPointer = &editor;
            projv::ComponentHandle component = editor.paletteComponent;
            projv::editor::EditRecord record;
            record.label = "Add entry " + std::to_string(newSlot);
            record.undo = [=] { popLastMaterial(scenePointer, editorPointer, component); };
            record.redo = [=] {
                projv::utils::addMaterial(*scenePointer, component, name, 0x3FFFFFFF);
                editorPointer->gpuFlushNeeded = true;
                invalidatePaletteCaches(editorPointer);
            };
            editor.history.record(std::move(record), ImGui::GetTime());
        }
    }
    ImGui::EndDisabled();
    if (paletteIsFull && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Palette is full: material IDs are 8-bit, so 255 entries is the ceiling.");
    }

    ImGui::SameLine();
    bool canRemove = editor.selectedMaterialSlot >= 0 && component.materialPalette.size() > 1;
    ImGui::BeginDisabled(!canRemove);
    if (ImGui::Button("Remove entry")) {
        uint32_t usage = size_t(editor.selectedMaterialSlot) < editor.materialUsage.size()
                       ? editor.materialUsage[editor.selectedMaterialSlot] : 0;
        if (usage == 0) {
            // Nothing references it, so the only work is renumbering the slots above it.
            int removedSlot = editor.selectedMaterialSlot;
            if (removeMaterialWithUndo(scene, editor, uint8_t(removedSlot), 0)) {
                editor.statusMessage = "Removed unused palette slot " + std::to_string(removedSlot);
            }
        } else {
            editor.materialRemovalReplacement = editor.selectedMaterialSlot == 0 ? 1 : 0;
            ImGui::OpenPopup("Reassign voxels");
        }
    }
    ImGui::EndDisabled();

    drawMaterialRemovalDialog(scene, editor);

    ImGui::Separator();

    // --- The selected entry ---
    if (editor.selectedMaterialSlot < 0 || size_t(editor.selectedMaterialSlot) >= component.materialPalette.size()) {
        ImGui::TextDisabled("Select an entry to edit it.");
        ImGui::End();
        return;
    }

    uint8_t slot = uint8_t(editor.selectedMaterialSlot);
    projv::Material& material = component.materialPalette[slot];

    ImGui::ColorButton("##selectedSwatch", materialSwatchColor(material.packedColor),
                       ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##materialName", editor.materialNameBuffer, sizeof(editor.materialNameBuffer))) {
        std::string previousName = material.name;
        std::string newName = editor.materialNameBuffer;
        projv::utils::setMaterialName(scene, editor.paletteComponent, slot, newName);

        projv::Scene* scenePointer = &scene;
        EditorState* editorPointer = &editor;
        projv::ComponentHandle component = editor.paletteComponent;
        projv::editor::EditRecord record;
        record.label = "Rename slot " + std::to_string(slot) + " to \"" + newName + "\"";
        // Typing emits one of these per keystroke; they merge into a single "rename" step.
        record.coalesceKey = "name:" + std::to_string(component) + ":" + std::to_string(slot);
        record.undo = [=] { applyMaterialName(scenePointer, editorPointer, component, slot, previousName); };
        record.redo = [=] { applyMaterialName(scenePointer, editorPointer, component, slot, newName); };
        editor.history.record(std::move(record), ImGui::GetTime());
    }

    if (ImGui::BeginTabBar("##materialDetail")) {
        if (ImGui::BeginTabItem("Colour")) {
            float color[3];
            unpackColorToFloats(material.packedColor, color);
            if (ImGui::ColorPicker3("##materialColor", color,
                                    ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_DisplayHex |
                                    ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoLabel)) {
                // Every frame of a drag lands here. Recolouring cannot move a palette offset, so it
                // takes the single-texel path rather than a full flush — and the image it changes is
                // the one the accumulate pass has been averaging, so that history has to go.
                uint32_t previousColor = material.packedColor;
                uint32_t newColor = packColorFromFloats(color);
                if (previousColor != newColor) {
                    applyMaterialColor(&scene, &gpuData, &editor, editor.paletteComponent, slot, newColor);

                    // One drag is one undo step: every frame of it coalesces into the entry the drag
                    // opened, whose undo still holds the colour the user started from.
                    projv::Scene* scenePointer = &scene;
                    projv::GPUData* gpuPointer = &gpuData;
                    EditorState* editorPointer = &editor;
                    projv::ComponentHandle component = editor.paletteComponent;
                    projv::editor::EditRecord record;
                    record.label = "Recolour slot " + std::to_string(slot);
                    record.coalesceKey = "colour:" + std::to_string(component) + ":" + std::to_string(slot);
                    record.undo = [=] { applyMaterialColor(scenePointer, gpuPointer, editorPointer, component, slot, previousColor); };
                    record.redo = [=] { applyMaterialColor(scenePointer, gpuPointer, editorPointer, component, slot, newColor); };
                    editor.history.record(std::move(record), ImGui::GetTime());
                }
            }
            // What is actually stored, as opposed to what the picker's 8-bit display suggests.
            ImGui::TextDisabled("slot %u   R%u G%u B%u  (10-bit)", slot,
                                (material.packedColor >> 20) & 0x3FF,
                                (material.packedColor >> 10) & 0x3FF,
                                material.packedColor & 0x3FF);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Voxels")) {
            // The breakdown walks every blob of the component, so it is computed when the selection
            // changes rather than every frame this tab is open.
            if (!editor.materialChunkUsageValid) {
                editor.materialChunkUsage = projv::utils::findMaterialChunks(scene, editor.paletteComponent, slot);
                editor.materialChunkUsageValid = true;
            }

            uint32_t usage = slot < editor.materialUsage.size() ? editor.materialUsage[slot] : 0;
            uint32_t componentVoxels = 0;
            for (uint32_t count : editor.materialUsage) componentVoxels += count;
            float share = componentVoxels > 0 ? 100.0f * float(usage) / float(componentVoxels) : 0.0f;

            ImGui::Text("%u voxel(s)", usage);
            ImGui::TextDisabled("%.2f%% of this component's %u voxels", share, componentVoxels);
            ImGui::TextDisabled("in %zu chunk(s)", editor.materialChunkUsage.size());
            ImGui::Separator();

            if (editor.materialChunkUsage.empty()) {
                ImGui::TextDisabled("Nothing uses this entry - it is safe to remove.");
            } else {
                ImGui::TextDisabled("Click a chunk to look at it.");
                ImGui::BeginChild("##materialChunks", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
                for (const projv::utils::MaterialChunkUsage& chunkUsage : editor.materialChunkUsage) {
                    std::string label = "chunk " + std::to_string(chunkUsage.chunk) + "   " +
                                        formatCompactCount(chunkUsage.voxelCount) + " voxels";
                    if (ImGui::Selectable(label.c_str())) {
                        // Put the camera where it can see that chunk, keeping the current angle.
                        projv::core::vec3 viewDirection = computeCameraDirection(editor);
                        float distance = std::max(editor.framing.moveSpeed * 60.0f, 1.0f);
                        projv::core::vec3 target = chunkUsage.chunkPosition;
                        if (chunkUsage.chunk < scene.chunks.size()) {
                            target += projv::core::vec3(scene.chunks[chunkUsage.chunk].header.scale * 0.5f);
                        }
                        editor.cameraPosition = target - viewDirection * distance;
                        editor.cameraMovedByInterface = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%u voxels at (%.0f, %.0f, %.0f)", chunkUsage.voxelCount,
                                          chunkUsage.chunkPosition.x, chunkUsage.chunkPosition.y,
                                          chunkUsage.chunkPosition.z);
                    }
                }
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// =============================================================================
// Icon buttons
// =============================================================================

// Two strips of icon buttons float over the scene image rather than living in docked panels: the
// tool strip down the left edge, and the render toggles along the bottom. Both change what a click
// in the viewport does or what the viewport shows, so they belong where the user is already looking,
// and a docked panel for six buttons would cost more screen than it is worth. The strips themselves
// are further down, with the Viewport panel they are drawn into; this is the chrome and the icons,
// which sit up here because the Tool panel below repeats the same four buttons.
//
// The icons are drawn from primitives instead of glyphs because ImGui's default font is ASCII-only
// -- there is no symbol in it that reads as "ambient occlusion" -- and shipping an icon font is a
// font dependency, an atlas rebuild, and a licence for what fits in a screenful of ImDrawList calls.

static constexpr float SETTINGS_BAR_ICON_SIZE = 30.0f;   // Button, square, in screen pixels.
static constexpr float SETTINGS_BAR_SPACING = 6.0f;
static constexpr float SETTINGS_BAR_PADDING = 6.0f;      // Backing panel, around the buttons.
static constexpr float SETTINGS_BAR_MARGIN = 10.0f;      // Gap from the edge of the image.

// The chrome shared by both strips: an invisible button of the given size at the current cursor
// position, a rounded background that fills in when the button is on and lights up under the cursor
// when it is not, and the caller's icon centred in it. Tooltips are the caller's job — the two
// strips have different things to say.
static bool drawViewportIconButton(const char* id, float size, bool active,
                                   void (*drawIcon)(ImDrawList*, ImVec2, float, bool),
                                   bool& outHovered) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 buttonMin = ImGui::GetCursorScreenPos();
    ImVec2 buttonMax = ImVec2(buttonMin.x + size, buttonMin.y + size);

    ImGui::InvisibleButton(id, ImVec2(size, size));
    outHovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    ImU32 background = IM_COL32(0, 0, 0, 0);
    if (active) {
        background = ImGui::IsItemActive() ? IM_COL32(80, 140, 205, 255)
                   : outHovered            ? IM_COL32(70, 126, 190, 235)
                                           : IM_COL32(58, 108, 165, 210);
    } else if (outHovered) {
        background = ImGui::IsItemActive() ? IM_COL32(90, 96, 110, 235) : IM_COL32(72, 78, 92, 200);
    }
    if (background != IM_COL32(0, 0, 0, 0)) {
        drawList->AddRectFilled(buttonMin, buttonMax, background, 5.0f);
    }

    drawIcon(drawList, ImVec2((buttonMin.x + buttonMax.x) * 0.5f, (buttonMin.y + buttonMax.y) * 0.5f),
             size, active);
    return clicked;
}

// The two shades every icon below is drawn in. Selected icons sit on a filled blue background and
// are drawn bright; unselected ones are dimmed, so the strip reads as one row of the same objects
// with exactly one of them lit rather than as five unrelated pictures.
static ImU32 iconInk(bool active) {
    return active ? IM_COL32(238, 242, 250, 255) : IM_COL32(152, 158, 170, 205);
}
static ImU32 iconInkDim(bool active) {
    return active ? IM_COL32(190, 205, 230, 235) : IM_COL32(120, 126, 138, 175);
}

// --- Tool icons -------------------------------------------------------------
//
// Each is drawn inside a `size` box centred on `center`, in units of size/20 so the shapes scale
// with the button. Nothing here is convex as a whole, so the filled shapes are decomposed into
// triangles and rectangles rather than handed to AddConvexPolyFilled, which would fold the
// concave ones inside out.

// The mouse arrow, which is what "this click chooses something" looks like everywhere.
static void drawSelectToolIcon(ImDrawList* drawList, ImVec2 center, float size, bool active) {
    float unit = size / 20.0f;
    ImVec2 origin = ImVec2(center.x - 5.0f * unit, center.y - 7.0f * unit);
    auto point = [&](float x, float y) { return ImVec2(origin.x + x * unit, origin.y + y * unit); };
    ImU32 ink = iconInk(active);

    // Head: tip at the top-left, down the left edge, out to the right shoulder.
    drawList->AddTriangleFilled(point(0.0f, 0.0f), point(0.0f, 13.5f), point(9.8f, 9.6f), ink);
    // Tail: the quad running from the notch down to the bottom-right, as two triangles.
    ImVec2 tail[4] = { point(4.0f, 10.2f), point(6.6f, 9.1f), point(9.6f, 15.6f), point(7.0f, 16.7f) };
    drawList->AddTriangleFilled(tail[0], tail[1], tail[2], ink);
    drawList->AddTriangleFilled(tail[0], tail[2], tail[3], ink);
}

// A four-way arrow: the standard "this drags things around" mark, and the shape of the translate
// handles the tool actually puts on screen.
static void drawMoveToolIcon(ImDrawList* drawList, ImVec2 center, float size, bool active) {
    float unit = size / 20.0f;
    ImU32 ink = iconInk(active);
    float shaft = 1.6f * unit;     // Half-width of the cross bars.
    float reach = 5.6f * unit;     // Where the arrowheads begin.
    float head = 2.9f * unit;      // Half-width of an arrowhead's base.
    float tip = 8.4f * unit;

    drawList->AddRectFilled(ImVec2(center.x - reach, center.y - shaft),
                            ImVec2(center.x + reach, center.y + shaft), ink);
    drawList->AddRectFilled(ImVec2(center.x - shaft, center.y - reach),
                            ImVec2(center.x + shaft, center.y + reach), ink);

    drawList->AddTriangleFilled(ImVec2(center.x - tip, center.y),
                                ImVec2(center.x - reach, center.y - head),
                                ImVec2(center.x - reach, center.y + head), ink);
    drawList->AddTriangleFilled(ImVec2(center.x + tip, center.y),
                                ImVec2(center.x + reach, center.y + head),
                                ImVec2(center.x + reach, center.y - head), ink);
    drawList->AddTriangleFilled(ImVec2(center.x, center.y - tip),
                                ImVec2(center.x + head, center.y - reach),
                                ImVec2(center.x - head, center.y - reach), ink);
    drawList->AddTriangleFilled(ImVec2(center.x, center.y + tip),
                                ImVec2(center.x - head, center.y + reach),
                                ImVec2(center.x + head, center.y + reach), ink);
}

// A round brush over a stepped voxel surface -- the two halves of what sculpting is here: a smooth
// falloff applied to something that can only answer in cubes.
static void drawSculptToolIcon(ImDrawList* drawList, ImVec2 center, float size, bool active) {
    float unit = size / 20.0f;
    ImU32 ink = iconInk(active);
    ImU32 surface = iconInkDim(active);

    // The staircase, three cubes descending left to right along the bottom.
    float cube = 4.2f * unit;
    float baseY = center.y + 8.2f * unit;
    for (int step = 0; step < 3; step++) {
        float left = center.x - 8.4f * unit + float(step) * cube;
        float top = baseY - cube * float(3 - step) * 0.42f - cube;
        drawList->AddRectFilled(ImVec2(left, top), ImVec2(left + cube - unit * 0.4f, baseY), surface, 1.0f);
    }
    // The brush: an outlined sphere with its centre marked, sitting into the top step.
    ImVec2 brushCenter = ImVec2(center.x + 1.2f * unit, center.y - 3.6f * unit);
    drawList->AddCircle(brushCenter, 5.4f * unit, ink, 20, 1.8f * unit);
    drawList->AddCircleFilled(brushCenter, 1.1f * unit, ink, 8);
}

// A brush loaded with paint. The dab of colour is the point: this tool does not change what is
// there, only what colour it is.
static void drawPaintToolIcon(ImDrawList* drawList, ImVec2 center, float size, bool active) {
    float unit = size / 20.0f;
    ImU32 ink = iconInk(active);
    ImU32 ferrule = iconInkDim(active);
    ImU32 paint = active ? IM_COL32(255, 205, 90, 255) : IM_COL32(170, 145, 85, 210);

    // Handle, running from the lower left to the upper right.
    drawList->AddLine(ImVec2(center.x - 5.6f * unit, center.y + 6.4f * unit),
                      ImVec2(center.x + 4.4f * unit, center.y - 3.6f * unit), ink, 2.6f * unit);
    // Ferrule: a short thick band across the handle where the bristles are clamped.
    drawList->AddLine(ImVec2(center.x + 3.4f * unit, center.y - 2.6f * unit),
                      ImVec2(center.x + 5.8f * unit, center.y - 5.0f * unit), ferrule, 4.4f * unit);
    // Bristle tip, and the dab it has just laid down.
    drawList->AddTriangleFilled(ImVec2(center.x + 5.2f * unit, center.y - 6.6f * unit),
                                ImVec2(center.x + 8.4f * unit, center.y - 3.4f * unit),
                                ImVec2(center.x + 8.6f * unit, center.y - 7.6f * unit), paint);
    drawList->AddCircleFilled(ImVec2(center.x - 6.4f * unit, center.y + 7.2f * unit), 1.8f * unit, paint, 10);
}

static void (*const TOOL_ICONS[EDITOR_TOOL_COUNT])(ImDrawList*, ImVec2, float, bool) = {
    drawSelectToolIcon, drawMoveToolIcon, drawSculptToolIcon, drawPaintToolIcon
};

// =============================================================================
// Tool panel
// =============================================================================
//
// One panel, whose contents are the active tool's settings. It replaces the old Sculpt panel, and
// the change is not only cosmetic: that panel had to carry its own copy of the palette grid, because
// it shared a dock node with the Palette and so was never on screen at the same time as it. Three
// stacked panels means the palette is always visible, which means the tool panel can show the one
// line that matters -- which entry is loaded -- and point at the panel that owns the rest.

// The material every painting tool works in: the Palette panel's current entry, shown here as a
// swatch and a name so the tool can be used without looking away, but owned there. Deliberately not
// a second "current material" of its own -- three notions of which colour is active is how you end
// up sculpting in a colour you were not looking at.
static void drawToolMaterialRow(projv::Scene& scene, EditorState& editor, bool enabled) {
    ImGui::TextDisabled("Material");

    if (editor.paletteComponent >= scene.components.size() ||
        scene.components[editor.paletteComponent].materialPalette.empty()) {
        ImGui::TextWrapped("No palette in view. Select a component that has one.");
        return;
    }

    const projv::ComponentRecord& source = scene.components[editor.paletteComponent];
    if (editor.selectedMaterialSlot < 0 ||
        size_t(editor.selectedMaterialSlot) >= source.materialPalette.size()) {
        ImGui::TextWrapped("No entry selected - click a swatch in the Palette panel.");
        return;
    }

    ImGui::BeginDisabled(!enabled);
    const projv::Material& active = source.materialPalette[editor.selectedMaterialSlot];
    ImGui::ColorButton("##toolActiveSwatch", materialSwatchColor(active.packedColor),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                       ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
    ImGui::SameLine();
    ImGui::Text("%d  %s", editor.selectedMaterialSlot,
                active.name.empty() ? "(unnamed)" : active.name.c_str());

    // The eyedropper, sharing the one armed flag the Palette panel and the viewport already
    // cooperate on. Kept here as well as there because arming it is a thing you do mid-stroke.
    ImGui::SameLine();
    float toolbarSize = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - toolbarSize + ImGui::GetCursorPosX() -
                    ImGui::GetStyle().ItemSpacing.x);
    ImVec2 pickerPos = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##toolPicker", ImVec2(toolbarSize, toolbarSize))) {
        editor.materialPickerActive = !editor.materialPickerActive;
    }
    bool pickerHovered = ImGui::IsItemHovered();
    ImU32 pickerBackground = editor.materialPickerActive ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                           : pickerHovered               ? ImGui::GetColorU32(ImGuiCol_ButtonHovered)
                                                         : ImGui::GetColorU32(ImGuiCol_Button);
    ImGui::GetWindowDrawList()->AddRectFilled(pickerPos,
        ImVec2(pickerPos.x + toolbarSize, pickerPos.y + toolbarSize), pickerBackground, 3.0f);
    drawEyedropperIcon(ImGui::GetWindowDrawList(), pickerPos, toolbarSize,
                       ImGui::GetColorU32(ImGuiCol_Text));
    if (pickerHovered) {
        ImGui::SetTooltip("Pick material from a voxel\nClick a voxel in the viewport to load its entry.");
    }
    ImGui::EndDisabled();
}

// The selection, named once at the top of the panel, so every tool says what it is about to act on
// in the same place. The breadcrumb over the viewport says the same thing; this is the version that
// is next to the settings being changed.
static void drawToolTargetRow(const projv::Scene& scene, const EditorState& editor) {
    if (editor.selectedComponent >= scene.components.size()) {
        ImGui::TextDisabled("Target");
        ImGui::TextWrapped("Nothing selected. Click a voxel in the viewport, or a node in the Scene "
                           "Hierarchy.");
        return;
    }
    const projv::ComponentRecord& component = scene.components[editor.selectedComponent];
    ImGui::TextDisabled("Target");
    ImGui::TextWrapped("%s", component.name.empty() ? "(unnamed)" : component.name.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", projv::utils::getComponentPath(scene, editor.selectedComponent).c_str());
    }
}

static void drawSelectToolSettings(projv::Scene& scene, EditorState& editor) {
    drawToolTargetRow(scene, editor);
    ImGui::Separator();
    ImGui::TextWrapped("Clicking a voxel selects the component that owns it, and clicking past "
                       "everything clears the selection.");
    ImGui::Spacing();
    ImGui::BeginDisabled(editor.selectedComponent >= scene.components.size());
    if (ImGui::Button("Clear selection")) {
        editor.selectedComponent = projv::INVALID_COMPONENT_HANDLE;
        editor.selectedVoxelCount = 0;
        editor.selectionOutlineValid = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reveal in hierarchy")) {
        editor.revealSelectionInHierarchy = true;
    }
    ImGui::EndDisabled();
}

static void drawMoveToolSettings(projv::Scene& scene, EditorState& editor) {
    drawToolTargetRow(scene, editor);
    ImGui::Separator();

    ImGui::Checkbox("Show gizmo", &editor.gizmoEnabled);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Three arrows to translate along the world axes and three rings to rotate\n"
                          "about them, drawn at the selection's pivot.");
    }
    ImGui::Checkbox("Pivot at center", &editor.pivotAtCenter);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Rotate and scale about the selection's bounding-box center.\n"
                          "Off: about the component's local origin, which for a chunk is its\n"
                          "minimum corner -- the raw transform.");
    }

    ImGui::Spacing();
    ImGui::TextWrapped("Drag a handle to transform, or type exact values into the Inspector's "
                       "Position, Rotation and Scale fields - the two drive the same transform.");
}

// World units per voxel for a component, or 0 when it has no voxel space to ask about. What both
// brush panels need to translate a size in voxels into the size it will look like in the scene.
static float componentVoxelScale(const projv::Scene& scene, projv::ComponentHandle handle) {
    if (handle >= scene.components.size()) return 0.0f;
    const projv::ComponentRecord& component = scene.components[handle];

    if (component.kind == projv::ComponentKind::Chunk && component.chunkHandle < scene.chunks.size()) {
        return scene.chunks[component.chunkHandle].header.voxelScale;
    }
    if (component.kind == projv::ComponentKind::Grid && component.dataRefID >= 0 &&
        size_t(component.dataRefID) < scene.dataReferences.size()) {
        return scene.dataReferences[component.dataRefID].voxelScale;
    }
    return 0.0f;
}

static void drawSculptToolSettings(projv::Scene& scene, EditorState& editor) {
    drawToolTargetRow(scene, editor);
    ImGui::Separator();

    // Brush type.
    ImGui::TextDisabled("Brush");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##sculptBrush", SculptBrushLabel(editor.sculptBrush))) {
        for (int i = 0; i < SCULPT_BRUSH_COUNT; i++) {
            SculptBrush brush = static_cast<SculptBrush>(i);
            if (ImGui::Selectable(SculptBrushLabel(brush), editor.sculptBrush == brush)) {
                editor.sculptBrush = brush;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", SculptBrushHint(brush));
            }
        }
        ImGui::EndCombo();
    }

    bool extruding = editor.sculptBrush == SculptBrush::Extrude;
    bool iterative = sculptBrushIsIterative(editor.sculptBrush);

    // Only what the chosen brush actually uses is drawn. A setting that cannot do anything is not
    // shown greyed out -- a disabled control still reads as part of the tool and still has to be
    // ruled out. The panel is short enough that its shape changing is easier to follow than a column
    // of dead rows.
    //
    // So: Extrude has no Mode (which way it goes is which way you drag) and Smooth has none either
    // (there is only one direction for "smoother"); Bump keeps the same choice under the names for
    // what it actually does to a surface. Only the shape brushes place the palette's colour, so only
    // they show a Material row -- Extrude takes the colour of the face it pulls and the other two take
    // theirs from the surface they are reshaping.
    bool hasMode = !extruding && editor.sculptBrush != SculptBrush::Smooth;
    if (hasMode) {
        ImGui::Spacing();
        ImGui::TextDisabled("Mode");
        for (int i = 0; i < 2; i++) {
            SculptMode mode = static_cast<SculptMode>(i);
            if (i > 0) ImGui::SameLine();
            if (ImGui::RadioButton(SculptModeLabel(mode, editor.sculptBrush), editor.sculptMode == mode)) {
                editor.sculptMode = mode;
            }
        }
    }

    if (!extruding && !iterative && editor.sculptMode != SculptMode::Remove) {
        ImGui::Separator();
        drawToolMaterialRow(scene, editor, true);
    }

    ImGui::Separator();
    // Sizes are in voxels for the same reason the paint brush's are: this tool addresses the voxel
    // grid, and a radius in world units would cover a different number of voxels per component. The
    // world size is spelled out below, which is the number a user actually pictures.
    switch (editor.sculptBrush) {
        case SculptBrush::Sphere: {
            ImGui::SetNextItemWidth(fieldWidthBeside("Radius (voxels)"));
            ImGui::DragFloat("##sculptRadius", &editor.sculptRadius, 0.1f, 0.5f, SCULPT_MAX_RADIUS, "%.1f");
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::TextDisabled("Radius (voxels)");
            editor.sculptRadius = std::clamp(editor.sculptRadius, 0.5f, SCULPT_MAX_RADIUS);
            break;
        }

        case SculptBrush::Cube: {
            float* sides[3] = { &editor.sculptCubeWidth, &editor.sculptCubeHeight, &editor.sculptCubeDepth };
            static const char* AXIS_LABELS[3] = { "Width", "Height", "Depth" };
            static const char* AXIS_IDS[3] = { "##sculptCubeW", "##sculptCubeH", "##sculptCubeD" };
            for (int axis = 0; axis < 3; axis++) {
                // All three reserve the widest label's width so the fields line up.
                ImGui::SetNextItemWidth(fieldWidthBeside("Height (voxels)"));
                ImGui::DragFloat(AXIS_IDS[axis], sides[axis], 0.2f, 1.0f, SCULPT_MAX_CUBE_SIDE, "%.0f");
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::TextDisabled("%s (voxels)", AXIS_LABELS[axis]);
                *sides[axis] = std::clamp(*sides[axis], 1.0f, SCULPT_MAX_CUBE_SIDE);
            }
            break;
        }

        case SculptBrush::Smooth:
        case SculptBrush::Bump: {
            // How much of the surface one pass reaches. How *far* it goes is how long you hold.
            ImGui::SetNextItemWidth(fieldWidthBeside("Radius (voxels)"));
            ImGui::DragFloat("##sculptOperatorRadius", &editor.sculptRadius, 0.1f, 0.5f,
                             SCULPT_MAX_RADIUS, "%.1f");
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::TextDisabled("Radius (voxels)");
            editor.sculptRadius = std::clamp(editor.sculptRadius, 0.5f, SCULPT_MAX_RADIUS);

            if (editor.sculptBrush == SculptBrush::Bump) {
                ImGui::SetNextItemWidth(fieldWidthBeside("Radius (voxels)"));
                ImGui::DragFloat("##sculptBlend", &editor.sculptBlendStrength, 0.05f, 0.0f,
                                 SCULPT_MAX_BLEND, "%.0f");
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::TextDisabled("Blend");
                editor.sculptBlendStrength =
                    std::clamp(editor.sculptBlendStrength, 0.0f, SCULPT_MAX_BLEND);
                ImGui::TextDisabled("0 leaves a hard edge; higher\nsettles it into the surface.");
            }

            if (editor.sculptBrush == SculptBrush::Smooth) {
                ImGui::SetNextItemWidth(fieldWidthBeside("Radius (voxels)"));
                ImGui::SliderFloat("##sculptSmoothStrength", &editor.sculptSmoothStrength,
                                   0.0f, SCULPT_MAX_SMOOTH_STRENGTH, "%.2f");
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::TextDisabled("Strength");
                editor.sculptSmoothStrength =
                    std::clamp(editor.sculptSmoothStrength, 0.0f, SCULPT_MAX_SMOOTH_STRENGTH);

                // Named, not numbered. Neither the cutoff nor the kernel radius means anything on its
                // own; what the user is choosing is which end of the scale they are on -- how much of
                // the surface counts as rough below 1, and how large a feature the brush can even see
                // above it. The 1 in the middle is the line between the two, and is the default.
                int kernel = sculptSmoothKernelRadius(editor.sculptSmoothStrength);
                if (kernel > 1) {
                    int side = 2 * kernel + 1;
                    ImGui::TextDisabled("%dx%dx%d neighbourhood: smooths whole\n"
                                        "features, not just rough voxels.", side, side, side);
                    // The one thing about this setting that cannot be discovered safely: at a wide
                    // kernel a shell is not smoothed, it is removed, and most voxelised meshes are
                    // shells. Said as a warning rather than as a description for that reason.
                    ImGui::TextDisabled("Anything thinner than %d voxels\ndissolves.", side);
                } else {
                    int cutoff = sculptSmoothCutoff(editor.sculptSmoothStrength);
                    const char* spares = cutoff <= 1 ? "Everything the filter says: rounds edges\nand corners as well as noise."
                                       : cutoff <= 2 ? "Spares clean 90-degree edges."
                                       : cutoff <= 4 ? "Spares steps and gentle curves; still\nfills pits and shaves spikes."
                                                     : "Noise only: lone voxels and\nenclosed holes.";
                    ImGui::TextDisabled("%s", spares);
                }
            }
            break;
        }

        case SculptBrush::Extrude: {
            // How far the face selection spreads. The difference only shows on a surface built from
            // more than one material, where it is the difference between moving the brick and moving
            // the whole wall -- so it is a choice, not a default worth guessing at.
            ImGui::TextDisabled("Face");
            for (int i = 0; i < SELECTION_SCOPE_COUNT; i++) {
                SelectionScope scope = static_cast<SelectionScope>(i);
                if (i > 0) ImGui::SameLine();
                if (ImGui::RadioButton(scope == SelectionScope::Material ? "Material" : "Whole face", editor.extrudeFaceScope == scope)) {
                    editor.extrudeFaceScope = scope;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", scope == SelectionScope::Material ? "Only the part of the face made of the material you clicked." : "Everything on that face, whatever it is made of. Each voxel keeps its own material.");
                }
            }
            break;
        }
    }

    // The world size of the brush, against the component the stroke would land in.
    float voxelScale = extruding ? 0.0f : componentVoxelScale(scene, editor.selectedComponent);
    if (voxelScale > 0.0f) {
        if (editor.sculptBrush == SculptBrush::Cube) {
            ImGui::TextDisabled("World size %.2f x %.2f x %.2f",
                                editor.sculptCubeWidth * voxelScale,
                                editor.sculptCubeHeight * voxelScale,
                                editor.sculptCubeDepth * voxelScale);
        } else {
            ImGui::TextDisabled("World diameter %.2f", 2.0f * editor.sculptRadius * voxelScale);
        }
    }

    if (!extruding && !iterative) {
        ImGui::Separator();
        ImGui::SetNextItemWidth(fieldWidthBeside("Place distance"));
        ImGui::DragFloat("##sculptPlaceDistance", &editor.sculptPlaceDistance, 0.5f, 1.0f, 10000.0f, "%.1f");
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TextDisabled("Place distance");
        editor.sculptPlaceDistance = std::clamp(editor.sculptPlaceDistance, 1.0f, 10000.0f);
    }

    ImGui::Separator();
    const char* footer = "Drag to sculpt. Alt+click samples a material.";
    if (extruding) footer = "Drag a face out to extend, in to carve.";
    else if (editor.sculptBrush == SculptBrush::Smooth) footer = "Hold over a surface to keep smoothing it.\nStrength decides what counts as rough; holding decides how far it gets.";
    else if (editor.sculptBrush == SculptBrush::Bump) footer = "Hold to keep moving the surface.";
    ImGui::TextDisabled("%s", footer);
}

static void drawPaintToolSettings(projv::Scene& scene, EditorState& editor) {
    drawToolTargetRow(scene, editor);
    ImGui::Separator();

    // --- Shape ---
    ImGui::TextDisabled("Mode");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##paintShape", PaintShapeLabel(editor.paintShape))) {
        for (int i = 0; i < PAINT_SHAPE_COUNT; i++) {
            PaintShape shape = static_cast<PaintShape>(i);
            if (ImGui::Selectable(PaintShapeLabel(shape), editor.paintShape == shape)) {
                editor.paintShape = shape;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", PaintShapeHint(shape));
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("%s", PaintShapeHint(editor.paintShape));

    // --- Per-shape settings ---
    // Sizes are in voxels, not world units: this is a tool that addresses the voxel grid, and a
    // radius in world units would mean a different number of voxels per component depending on each
    // one's voxelScale. The world size is spelled out underneath for the one number a user pictures.
    switch (editor.paintShape) {
        case PaintShape::Voxel:
            break;

        case PaintShape::Sphere: {
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##paintRadius", &editor.paintRadius, 0.1f, 0.5f, PAINT_MAX_RADIUS, "%.1f");
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::TextDisabled("Radius (voxels)");
            editor.paintRadius = std::clamp(editor.paintRadius, 0.5f, PAINT_MAX_RADIUS);
            break;
        }

        case PaintShape::Cube: {
            ImGui::Spacing();
            static const char* AXIS_LABELS[3] = { "Width", "Height", "Depth" };
            static const char* AXIS_IDS[3] = { "##paintCubeW", "##paintCubeH", "##paintCubeD" };
            for (int axis = 0; axis < 3; axis++) {
                // All three rows reserve the widest label's worth, so the fields line up rather than
                // each ending where its own name happens to start.
                ImGui::SetNextItemWidth(fieldWidthBeside("Height (voxels)"));
                ImGui::DragInt(AXIS_IDS[axis], &editor.paintCubeSize[axis], 0.2f, 1, PAINT_MAX_CUBE_SIDE);
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::TextDisabled("%s (voxels)", AXIS_LABELS[axis]);
                editor.paintCubeSize[axis] = std::clamp(editor.paintCubeSize[axis], 1, PAINT_MAX_CUBE_SIDE);
            }
            break;
        }

        case PaintShape::FillFace:
        case PaintShape::FillVolume: {
            ImGui::Spacing();
            ImGui::TextDisabled("Spread");
            const char* everythingLabel = editor.paintShape == PaintShape::FillFace ? "Whole face"
                                                                                    : "Whole volume";
            for (int i = 0; i < SELECTION_SCOPE_COUNT; i++) {
                SelectionScope scope = static_cast<SelectionScope>(i);
                if (i > 0) ImGui::SameLine();
                const char* label = scope == SelectionScope::Material ? "Material" : everythingLabel;
                if (ImGui::RadioButton(label, editor.paintFillScope == scope)) {
                    editor.paintFillScope = scope;
                }
            }
            // Said here rather than left to be discovered, because it is the one combination that can
            // repaint far more than was pointed at in a single click.
            if (editor.paintShape == PaintShape::FillVolume &&
                editor.paintFillScope == SelectionScope::Everything) {
                ImGui::TextDisabled("Reaches every connected voxel,\ninside included.");
            }
            break;
        }
    }

    // The world size of the brush, which is what it will look like against the scene. Read off the
    // selected component when there is one, since voxelScale is a per-component number.
    if (editor.paintShape == PaintShape::Sphere || editor.paintShape == PaintShape::Cube) {
        float voxelScale = componentVoxelScale(scene, editor.selectedComponent);
        if (voxelScale > 0.0f) {
            if (editor.paintShape == PaintShape::Sphere) {
                ImGui::TextDisabled("World diameter %.2f", 2.0f * editor.paintRadius * voxelScale);
            } else {
                ImGui::TextDisabled("World size %.2f x %.2f x %.2f",
                                    float(editor.paintCubeSize[0]) * voxelScale,
                                    float(editor.paintCubeSize[1]) * voxelScale,
                                    float(editor.paintCubeSize[2]) * voxelScale);
            }
        }
    }

    ImGui::Separator();
    drawToolMaterialRow(scene, editor, true);

    ImGui::Separator();
    // The boundary against the Sculpt tool, which is the one thing about this tool that is not
    // visible from its controls -- and, for the two fills, the one thing about them that is not
    // visible from the drag either.
    ImGui::TextDisabled("%s\nAlt+click samples. Recolours only; never adds a voxel.",
                        paintShapeIsFill(editor.paintShape)
                            ? "Click to fill. One fill per click, not per drag."
                            : "Drag to paint a stroke; the whole sweep is one undo step.");
}

static void drawToolPanel(projv::Scene& scene, EditorState& editor) {
    // The visible title tracks the tool; the identity after ### does not, so the dock layout and the
    // imgui.ini entry survive a tool switch. See TOOL_PANEL_ID.
    std::string title = std::string(editorToolLabel(editor.activeTool)) + " Tool###ToolPanel";
    ImGui::Begin(title.c_str());

    if (!editor.sceneLoaded) {
        ImGui::TextDisabled("No scene loaded.");
        ImGui::End();
        return;
    }

    // The tool strip in the viewport, repeated here as a row of the same icons: the panel is where
    // the user is when they are adjusting settings, and having to travel back to the viewport to
    // change tool is a trip the panel can save.
    ImGui::PushID("ToolPanelStrip");
    for (int i = 0; i < EDITOR_TOOL_COUNT; i++) {
        EditorTool tool = static_cast<EditorTool>(i);
        if (i > 0) ImGui::SameLine();
        bool hovered = false;
        char id[8];
        std::snprintf(id, sizeof(id), "tool%d", i);
        if (drawViewportIconButton(id, ImGui::GetFrameHeight() * 1.4f, editor.activeTool == tool,
                                   TOOL_ICONS[i], hovered)) {
            editor.activeTool = tool;
        }
        if (hovered) {
            ImGui::SetTooltip("%s  (%s)", editorToolLabel(tool), editorToolShortcut(tool));
        }
    }
    ImGui::PopID();

    ImGui::TextDisabled("%s", editorToolHint(editor.activeTool));
    ImGui::Separator();

    switch (editor.activeTool) {
        case EditorTool::Select: drawSelectToolSettings(scene, editor); break;
        case EditorTool::Move:   drawMoveToolSettings(scene, editor);   break;
        case EditorTool::Sculpt: drawSculptToolSettings(scene, editor); break;
        case EditorTool::Paint:  drawPaintToolSettings(scene, editor);  break;
    }

    ImGui::End();
}

// A pick reports the voxel's coordinate *inside the chunk it hit*; the edit queue takes coordinates
// in the component's own continuous voxel space. For a loose Chunk those are the same space. For a
// Grid they are not: the chunk is one cell of it, and applyComponentQueue buckets an op back out to
// a cell with floorDiv(position, resolution) against grid.originCellCoord. This is that bucketing
// run backwards, so a coordinate handed to queueVoxelAdd lands in the cell it was picked from.
//
// False when the mapping is not defined — an Asset folder owns no voxels, and a chunk whose grid or
// cell index is out of range is not somewhere an edit can be aimed.
// Also false when the chunk is not one of `component`'s: the ray override asks about whatever chunk
// the traversal walked into, which is routinely somebody else's.
static bool chunkVoxelToComponentCoord(const projv::Scene& scene, projv::ComponentHandle componentHandle,
                                       projv::ChunkHandle chunkHandle, projv::core::ivec3 localCoord,
                                       projv::core::ivec3& outCoord) {
    if (componentHandle >= scene.components.size() || chunkHandle >= scene.chunks.size()) return false;
    const projv::ComponentRecord& component = scene.components[componentHandle];
    const projv::Chunk& chunk = scene.chunks[chunkHandle];

    if (component.kind == projv::ComponentKind::Chunk) {
        if (component.chunkHandle != chunkHandle) return false;
        outCoord = localCoord;
        return true;
    }
    if (component.kind != projv::ComponentKind::Grid) return false;
    if (chunk.gridIndex < 0 || chunk.gridIndex != component.gridIndex) return false;
    if (size_t(chunk.gridIndex) >= scene.grids.size()) return false;

    const projv::SceneGrid& grid = scene.grids[chunk.gridIndex];
    if (grid.dims.x <= 0 || grid.dims.y <= 0 || chunk.cellIndex < 0) return false;

    // Linear cell index back to the (ix, iy, iz) within the grid, exactly as the Grid path unpacks
    // it when it creates a new cell.
    int linear = chunk.cellIndex;
    int iz = linear / (grid.dims.x * grid.dims.y);
    int iy = (linear / grid.dims.x) % grid.dims.y;
    int ix = linear % grid.dims.x;

    int32_t resolution = int32_t(chunk.header.resolution);
    if (resolution <= 0) return false;

    outCoord = (grid.originCellCoord + projv::core::ivec3(ix, iy, iz)) * resolution + localCoord;
    return true;
}

static bool pickToComponentVoxelCoord(const projv::Scene& scene, const projv::utils::VoxelPick& pick,
                                      projv::core::ivec3& outCoord) {
    return chunkVoxelToComponentCoord(scene, pick.component, pick.chunk, pick.voxelCoord, outCoord);
}

// Points every palette-facing piece of state at a component and a slot within it. Shared by the
// eyedropper and by painting, which both have to answer "which palette, which entry" the same way.
static void adoptPickedMaterial(projv::Scene& scene, EditorState& editor,
                                projv::ComponentHandle component, uint8_t slot) {
    if (component != editor.paletteComponent) {
        selectPaletteComponent(editor, component);
    }
    editor.selectedMaterialSlot = int(slot);
    editor.materialChunkUsageValid = false;

    const projv::ComponentRecord& record = scene.components[component];
    std::string name = slot < record.materialPalette.size() ? record.materialPalette[slot].name
                                                            : std::string();
    std::snprintf(editor.materialNameBuffer, sizeof(editor.materialNameBuffer), "%s", name.c_str());
}

// =============================================================================
// Painting
// =============================================================================

// A component-space coordinate as one integer, keying the sets both brushes build over voxels: the
// sculpt stroke's record of the original surface, and the paint stroke's per-frame dedupe.
//
// 21 bits per axis, signed, so the set is exact for coordinates within +/-1,048,575 of the origin --
// at the usual 256^3 cells that is +/-4096 cells in every direction, far past any scene that fits in
// memory. Beyond it two coordinates could alias, which would hide a voxel from the ray that the
// stroke never touched: a cosmetic slip in an unreachable scene, not a corruption of any geometry.
static uint64_t packVoxelKey(projv::core::ivec3 coord) {
    return (uint64_t(uint32_t(coord.x) & 0x1FFFFFu) << 42) |
           (uint64_t(uint32_t(coord.y) & 0x1FFFFFu) << 21) |
           (uint64_t(uint32_t(coord.z) & 0x1FFFFFu));
}

// Everything needed to turn a component-space voxel coordinate back into the chunk that holds it,
// resolved once per stroke rather than per voxel.
//
// Per-stroke matters: a brush asks about every coordinate in its bounding box, and for a Grid the
// resolution has to be recovered by finding a populated cell. Doing that inside the inner loop would
// put a scan of cellToChunk under every one of a quarter-million probes.
struct ComponentVoxelSpace {
    bool valid = false;
    bool isGrid = false;
    int32_t resolution = 0;
    projv::ChunkHandle chunk = 0;         // Loose Chunk only.
    // Grid only -- and a pointer *into* scene.grids, so it does not survive an edit.
    //
    // **Resolve, use, then mutate; never the other way round.** applyComponentQueue can push a new
    // SceneGrid (convertChunkToGrid) and reallocate the vector, leaving this dangling, and it can
    // move a grid's origin, leaving latticeOrigin/coordOrigin describing a lattice that has shifted.
    // Every caller here resolves once per frame, reads whatever it needs, and only then queues -- the
    // one place that got this wrong was a self-test that held a space across a dozen edits and read
    // freed memory back as geometry.
    const projv::SceneGrid* grid = nullptr;

    // --- The world lattice ---
    //
    // A component's voxels form one continuous grid in world space, and these four fields are it. A
    // Grid's cells are laid out at `origin + R * (cellIJK * cellSize)` (see applyComponentQueue) and
    // each cell subdivides into `resolution` voxels per axis, so the cells fall on the same lattice
    // their voxels do — there is a single mapping for the whole component, not one per cell, and a
    // coordinate does not need its cell to exist yet for the mapping to be defined. That last part is
    // what additive sculpting needs: the cell is created *because* something was written there.
    //
    //     centre(coord) = latticeOrigin + R * ((vec3(coord - coordOrigin) + 0.5) * voxelSize)
    //
    // For a loose Chunk this is just the chunk's own header, with coordOrigin zero, which is exactly
    // the mapping utils::pickVoxel inverts to report worldPosition.
    projv::core::vec3 latticeOrigin = projv::core::vec3(0.0f);
    projv::core::quat rotation = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);
    float voxelSize = 0.0f;
    // The component-space coordinate sitting at latticeOrigin. Non-zero only after a Grid has been
    // expanded downward, which moves grid.origin without moving any existing geometry.
    projv::core::ivec3 coordOrigin = projv::core::ivec3(0);
};

// Centre of a component-space voxel, in world space.
static projv::core::vec3 componentVoxelToWorld(const ComponentVoxelSpace& space,
                                               projv::core::ivec3 coord) {
    projv::core::vec3 local = (projv::core::vec3(coord - space.coordOrigin) + 0.5f) * space.voxelSize;
    return space.latticeOrigin + glm::mat3_cast(space.rotation) * local;
}

// The exact inverse: which voxel of the component contains a world point. Defined everywhere, whether
// or not a chunk exists there — "which cell would hold this" is a question about the lattice, not
// about what has been built on it.
static projv::core::ivec3 worldToComponentVoxel(const ComponentVoxelSpace& space,
                                                projv::core::vec3 world) {
    projv::core::mat3 inverseRotation = glm::transpose(glm::mat3_cast(space.rotation));
    projv::core::vec3 local = (inverseRotation * (world - space.latticeOrigin)) / space.voxelSize;
    return projv::core::ivec3(int(std::floor(local.x)), int(std::floor(local.y)),
                              int(std::floor(local.z))) + space.coordOrigin;
}

// A direction in world space, expressed as the component-axis it most nearly points along. Used to
// carry a pick's face normal into a component whose rotation is not the one the ray hit — and to give
// the Extrude brush a plane to lie in when the stroke began over empty space and there is no face.
static projv::core::ivec3 worldDirectionToComponentAxis(const ComponentVoxelSpace& space,
                                                        projv::core::vec3 direction) {
    if (glm::length(direction) <= 0.0f) return projv::core::ivec3(0);
    projv::core::mat3 inverseRotation = glm::transpose(glm::mat3_cast(space.rotation));
    projv::core::vec3 local = inverseRotation * direction;

    int axis = 0;
    for (int candidate = 1; candidate < 3; candidate++) {
        if (std::abs(local[candidate]) > std::abs(local[axis])) axis = candidate;
    }
    projv::core::ivec3 result(0);
    result[axis] = local[axis] >= 0.0f ? 1 : -1;
    return result;
}

static ComponentVoxelSpace resolveComponentVoxelSpace(const projv::Scene& scene,
                                                      projv::ComponentHandle component) {
    ComponentVoxelSpace space;
    if (component >= scene.components.size()) return space;
    const projv::ComponentRecord& record = scene.components[component];

    if (record.kind == projv::ComponentKind::Chunk) {
        if (record.chunkHandle >= scene.chunks.size()) return space;
        const projv::Chunk& chunk = scene.chunks[record.chunkHandle];
        if (!chunk.alive || chunk.header.resolution == 0) return space;
        space.valid = true;
        space.chunk = record.chunkHandle;
        space.resolution = int32_t(chunk.header.resolution);
        space.latticeOrigin = chunk.header.position;
        space.rotation = chunk.header.rotation;
        space.voxelSize = chunk.header.scale / float(space.resolution);
        return space;
    }

    if (record.kind != projv::ComponentKind::Grid) return space;
    if (record.gridIndex < 0 || size_t(record.gridIndex) >= scene.grids.size()) return space;
    const projv::SceneGrid& grid = scene.grids[record.gridIndex];

    // The resolution is read off the first populated cell, NOT off dataReferences[dataRefID]:
    // dataRefID is -1 until the component's first edit assigns one (see ensureDataReference), so on a
    // freshly loaded scene -- which is every scene, before anything is painted -- that route reports
    // nothing and the brush finds an empty world. Every cell of a grid shares one resolution, so the
    // first one that exists is the answer. (This is the same fallback ensureDataReference itself uses.)
    for (int32_t chunkIndex : grid.cellToChunk) {
        if (chunkIndex < 0 || size_t(chunkIndex) >= scene.chunks.size()) continue;
        if (!scene.chunks[chunkIndex].alive) continue;
        space.resolution = int32_t(scene.chunks[chunkIndex].header.resolution);
        break;
    }
    if (space.resolution <= 0) return space;

    space.valid = true;
    space.isGrid = true;
    space.grid = &grid;
    space.latticeOrigin = grid.origin;
    space.rotation = grid.rotation;
    space.voxelSize = grid.cellSize / float(space.resolution);
    space.coordOrigin = grid.originCellCoord * space.resolution;
    return space;
}

// The other direction of pickToComponentVoxelCoord: a coordinate in the component's continuous voxel
// space back to the chunk that holds it and the coordinate within that chunk. Needed because a brush
// spans coordinates nobody picked -- the ray hit one voxel, and the sphere around it has to be asked
// about voxel by voxel.
//
// False when nothing holds that coordinate: outside a loose Chunk's resolution, outside a Grid's
// current extent, or in a Grid cell that has no chunk. All three mean "empty", which is what a paint
// brush does nothing about.
static bool componentVoxelToChunk(const projv::Scene& scene, const ComponentVoxelSpace& space,
                                  projv::core::ivec3 coord, projv::ChunkHandle& outChunk,
                                  projv::core::ivec3& outLocal) {
    if (!space.valid) return false;

    if (!space.isGrid) {
        if (coord.x < 0 || coord.y < 0 || coord.z < 0 ||
            coord.x >= space.resolution || coord.y >= space.resolution || coord.z >= space.resolution) {
            return false;
        }
        outChunk = space.chunk;
        outLocal = coord;
        return true;
    }

    const projv::SceneGrid& grid = *space.grid;
    // Exactly applyComponentQueue's bucketing: floorDiv, not integer division, because a negative
    // coordinate has to round toward the cell below it rather than toward zero.
    projv::core::ivec3 cell(projv::utils::floorDiv(coord.x, space.resolution),
                            projv::utils::floorDiv(coord.y, space.resolution),
                            projv::utils::floorDiv(coord.z, space.resolution));
    projv::core::ivec3 offset = cell - grid.originCellCoord;
    if (offset.x < 0 || offset.y < 0 || offset.z < 0 ||
        offset.x >= grid.dims.x || offset.y >= grid.dims.y || offset.z >= grid.dims.z) {
        return false;
    }
    int linear = offset.x + grid.dims.x * (offset.y + grid.dims.y * offset.z);
    if (linear < 0 || size_t(linear) >= grid.cellToChunk.size()) return false;
    int32_t chunkIndex = grid.cellToChunk[linear];
    if (chunkIndex < 0 || size_t(chunkIndex) >= scene.chunks.size()) return false;
    if (!scene.chunks[chunkIndex].alive) return false;

    outChunk = projv::ChunkHandle(chunkIndex);
    outLocal = projv::core::ivec3(projv::utils::floorMod(coord.x, space.resolution),
                                  projv::utils::floorMod(coord.y, space.resolution),
                                  projv::utils::floorMod(coord.z, space.resolution));
    return true;
}

// Whether a solid voxel exists at a component-space coordinate, and which palette entry it uses.
// Reads the tree64 directly, the same descent utils::pickVoxel makes once it has chosen a chunk.
static bool queryComponentVoxel(const projv::Scene& scene, const ComponentVoxelSpace& space,
                                projv::core::ivec3 coord, uint8_t& outSlot) {
    projv::ChunkHandle chunkHandle;
    projv::core::ivec3 local;
    if (!componentVoxelToChunk(scene, space, coord, chunkHandle, local)) return false;

    const projv::Chunk& chunk = scene.chunks[chunkHandle];
    if (chunk.geometryPoolIndex < 0 || size_t(chunk.geometryPoolIndex) >= scene.geometryPool.size()) {
        return false;
    }
    return projv::utils::queryVoxelMaterial(scene.geometryPool[chunk.geometryPoolIndex],
                                            chunk.header.resolution, local, outSlot);
}

// A voxel's colour, via its palette entry. Two entries can hold the same colour, which matters for
// undo: restoring by colour lands on whichever entry internMaterial finds for it, not necessarily the
// entry number the voxel started on. The voxel looks identical either way, and the alternative --
// undoing by slot number -- would be wrong the moment an entry is added or removed in between.
static uint32_t componentVoxelColor(const projv::Scene& scene, projv::ComponentHandle component,
                                    uint8_t slot) {
    const projv::ComponentRecord& record = scene.components[component];
    return slot < record.materialPalette.size() ? record.materialPalette[slot].packedColor : 0u;
}

// "Have I already looked at this voxel", for the flood fill, as one bit per voxel of each chunk the
// fill reaches rather than a hash set of coordinates.
//
// A chunk's voxels are a dense cube of known size, so a bit per voxel is the natural set: 256^3 is
// 2 MB, allocated only for chunks the fill actually enters, and a test is an index rather than a
// hash. The coordinate-hashing version this replaced cost ~40 bytes and a hash per voxel, which put
// a hard ceiling on how large a fill could be before it had to give up.
class VisitedVoxels {
public:
    VisitedVoxels(const projv::Scene& scene, const ComponentVoxelSpace& space)
        : scene_(scene), space_(space) {}

    bool isMarked(projv::core::ivec3 coord) {
        size_t bit;
        std::vector<uint64_t>* bits = locate(coord, bit);
        // Outside the component entirely: nothing there to visit, and reporting it as already seen
        // keeps it out of the frontier without a second lookup.
        if (!bits) return true;
        return ((*bits)[bit >> 6] >> (bit & 63)) & 1ull;
    }

    void mark(projv::core::ivec3 coord) {
        size_t bit;
        std::vector<uint64_t>* bits = locate(coord, bit);
        if (!bits) return;
        (*bits)[bit >> 6] |= 1ull << (bit & 63);
    }

private:
    // The chunk holding `coord` and the bit index within it, allocating that chunk's bitset the
    // first time the fill reaches it.
    std::vector<uint64_t>* locate(projv::core::ivec3 coord, size_t& outBit) {
        projv::ChunkHandle chunk;
        projv::core::ivec3 local;
        if (!componentVoxelToChunk(scene_, space_, coord, chunk, local)) return nullptr;

        size_t resolution = size_t(space_.resolution);
        outBit = (size_t(local.z) * resolution + size_t(local.y)) * resolution + size_t(local.x);

        std::vector<uint64_t>& bits = chunks_[chunk];
        if (bits.empty()) {
            bits.assign((resolution * resolution * resolution + 63) / 64, 0ull);
        }
        return &bits;
    }

    const projv::Scene& scene_;
    const ComponentVoxelSpace& space_;
    std::unordered_map<projv::ChunkHandle, std::vector<uint64_t>> chunks_;
};

// Every voxel of one face of a surface: the flat region you get by putting a finger on a wall and
// sliding it until the wall stops. Shared by the Extrude tool, which moves such a region, and the
// Paint tool's Fill face mode, which recolours one.
//
// A voxel belongs to the face when it is solid, lies in the same plane as the seed (same coordinate
// along the normal axis), has its face open in the same direction -- so it is part of the *surface*
// rather than buried behind it -- and is reachable from the seed through face neighbours within that
// plane. Staying in the plane is automatic: every step is along one of the two axes the normal does
// not use.
//
// `scope` decides whether a change of material stops the spread; see SelectionScope. Connectivity is
// 4-way, for the same reason the volume fill is 6-way -- diagonal connectivity leaks across the gap
// where two surfaces touch only at a corner.
//
// `outColors` reports each voxel's own colour, which Extrude carries up its columns and Paint ignores.
// `truncated` reports a face that ran into `maxVoxels`, which callers say out loud: a selection that
// silently stopped halfway looks like a broken tool rather than a limit.
static void gatherFaceRegion(const projv::Scene& scene, const ComponentVoxelSpace& space,
                             projv::ComponentHandle component, projv::core::ivec3 seed,
                             projv::core::ivec3 normal, uint8_t slot, SelectionScope scope,
                             size_t maxVoxels, std::vector<projv::core::ivec3>& outCoords,
                             std::vector<uint32_t>& outColors, bool& truncated) {
    using projv::core::ivec3;
    truncated = false;
    if (normal == ivec3(0)) return;

    int normalAxis = normal.x != 0 ? 0 : (normal.y != 0 ? 1 : 2);
    ivec3 stepA(0), stepB(0);
    stepA[(normalAxis + 1) % 3] = 1;
    stepB[(normalAxis + 2) % 3] = 1;
    const ivec3 NEIGHBOURS[4] = { stepA, -stepA, stepB, -stepB };
    bool matchMaterial = scope == SelectionScope::Material;

    auto belongsToFace = [&](ivec3 coord, uint32_t& colorOut) {
        uint8_t found = 0;
        if (!queryComponentVoxel(scene, space, coord, found)) return false;
        if (matchMaterial && found != slot) return false;
        uint8_t buried = 0;
        if (queryComponentVoxel(scene, space, coord + normal, buried)) return false;
        colorOut = componentVoxelColor(scene, component, found);
        return true;
    };

    uint32_t seedColor = 0;
    if (!belongsToFace(seed, seedColor)) return;

    VisitedVoxels visited(scene, space);
    std::vector<ivec3> queue;
    size_t head = 0;

    visited.mark(seed);
    queue.push_back(seed);
    outCoords.push_back(seed);
    outColors.push_back(seedColor);

    while (head < queue.size()) {
        ivec3 current = queue[head++];
        if (outCoords.size() >= maxVoxels) {
            truncated = true;
            break;
        }
        for (const ivec3& step : NEIGHBOURS) {
            ivec3 neighbour = current + step;
            if (visited.isMarked(neighbour)) continue;
            visited.mark(neighbour);   // Marked either way: a rejected voxel is tested once.
            uint32_t color = 0;
            if (!belongsToFace(neighbour, color)) continue;
            queue.push_back(neighbour);
            outCoords.push_back(neighbour);
            outColors.push_back(color);
        }
    }
}

// Every voxel one click of the current shape should recolour, with the colour each one has now.
//
// Only voxels that exist are collected, and only ones whose colour would actually change -- a brush
// dragged over a wall it has already painted queues nothing, so the undo step is the set of voxels
// that really moved rather than every voxel the brush covered.
//
// `truncated` reports a fill that ran into PAINT_FILL_LIMIT, which the caller says out loud: a fill
// that silently stopped halfway looks like a bug in the fill.
// `seedFaceNormal` is the face the ray entered through, which only Fill face uses -- it is the plane
// that fill is confined to. Zero (a ray that began inside the geometry) means there is no face to
// spread over, and that mode collects nothing.
static void collectPaintTargets(const projv::Scene& scene, projv::ComponentHandle component,
                                projv::core::ivec3 seed, uint8_t seedSlot,
                                projv::core::ivec3 seedFaceNormal, const EditorState& editor,
                                uint32_t newColor, std::vector<projv::core::ivec3>& outCoords,
                                std::vector<uint32_t>& outPreviousColors, bool& truncated) {
    using projv::core::ivec3;
    truncated = false;

    ComponentVoxelSpace space = resolveComponentVoxelSpace(scene, component);
    if (!space.valid) return;

    auto consider = [&](ivec3 coord) {
        uint8_t slot;
        if (!queryComponentVoxel(scene, space, coord, slot)) return;
        uint32_t previous = componentVoxelColor(scene, component, slot);
        if (previous == newColor) return;
        outCoords.push_back(coord);
        outPreviousColors.push_back(previous);
    };

    switch (editor.paintShape) {
        case PaintShape::Voxel:
            consider(seed);
            break;

        case PaintShape::Sphere: {
            // Scanned as a box and rejected by squared distance -- no square roots, and the box is
            // the smallest one that can contain the sphere anyway.
            int radius = int(std::floor(std::min(editor.paintRadius, PAINT_MAX_RADIUS)));
            float radiusSquared = editor.paintRadius * editor.paintRadius;
            for (int z = -radius; z <= radius; z++) {
                for (int y = -radius; y <= radius; y++) {
                    for (int x = -radius; x <= radius; x++) {
                        if (float(x * x + y * y + z * z) > radiusSquared) continue;
                        consider(seed + ivec3(x, y, z));
                    }
                }
            }
            break;
        }

        case PaintShape::Cube: {
            // Centred on the clicked voxel, which for an even side means one more voxel on the high
            // side than the low. Splitting the difference the other way would be just as arbitrary;
            // what matters is that the clicked voxel is always inside the box.
            int size[3];
            int low[3];
            for (int axis = 0; axis < 3; axis++) {
                size[axis] = std::clamp(editor.paintCubeSize[axis], 1, PAINT_MAX_CUBE_SIDE);
                low[axis] = -(size[axis] / 2);
            }
            for (int z = low[2]; z < low[2] + size[2]; z++) {
                for (int y = low[1]; y < low[1] + size[1]; y++) {
                    for (int x = low[0]; x < low[0] + size[0]; x++) {
                        consider(seed + ivec3(x, y, z));
                    }
                }
            }
            break;
        }

        case PaintShape::FillFace: {
            // The surface you pointed at, and no further -- the same region the Extrude tool moves,
            // recoloured instead. Confined to one plane, so unlike the volume fill below it cannot
            // reach around a corner or into the inside of a shape.
            std::vector<ivec3> face;
            std::vector<uint32_t> faceColors;
            gatherFaceRegion(scene, space, component, seed, seedFaceNormal, seedSlot,
                             editor.paintFillScope, PAINT_FILL_LIMIT, face, faceColors, truncated);
            for (const ivec3& coord : face) {
                consider(coord);
            }
            break;
        }

        case PaintShape::FillVolume: {
            // Six-connected flood through the solid. Face neighbours only: two voxels touching at an
            // edge or a corner are not somewhere paint would run, and diagonal connectivity leaks a
            // fill through the gap between two touching walls.
            //
            // Under the Material scope, compared by slot rather than by colour so that two entries
            // which happen to hold the same colour stay separate regions -- they are separate
            // materials, and a fill that merged them would recolour more than the user pointed at.
            // Under Everything the test is dropped and the flood is bounded only by the geometry,
            // which on a connected model means the whole model. That is a real thing to want and a
            // terrible thing to arrive at by accident, which is why it takes two deliberate choices
            // to reach: this mode rather than Fill face, and that scope rather than Material.
            //
            // Breadth-first, over a bitset of visited voxels, with the budget counting voxels of the
            // region rather than coordinates looked at. Each of those three was a bug once:
            //
            //   * A hash set of packed coordinates spent ~40 bytes and a hash on every voxel and,
            //     worse, was also what the budget was measured against -- and it holds every
            //     *rejected* neighbour too, roughly six per voxel of the region. So a nominal one
            //     million stopped the fill at about 160k voxels of actual region.
            //   * Depth-first (a stack) meant that stopping early did not leave a compact partial
            //     region: DFS runs to the end of one axis before it takes the first step along
            //     another, so a truncated fill came out as long tendrils with unpainted material
            //     between them. That is what "misses stripes, but no pattern I can discern" was --
            //     the pattern was the traversal order, and it is invisible from the outside.
            //
            // Together they made every large fill stop at a fraction of the region and made the part
            // it did paint look arbitrary. Neither shows up on a small region, which is why the first
            // round of tests passed.
            VisitedVoxels visited(scene, space);
            std::vector<ivec3> queue;
            size_t head = 0;
            size_t regionSize = 0;

            visited.mark(seed);
            queue.push_back(seed);
            regionSize++;

            static const ivec3 NEIGHBOURS[6] = {
                ivec3(1, 0, 0), ivec3(-1, 0, 0), ivec3(0, 1, 0),
                ivec3(0, -1, 0), ivec3(0, 0, 1), ivec3(0, 0, -1)
            };

            while (head < queue.size()) {
                ivec3 current = queue[head++];
                consider(current);

                if (regionSize >= PAINT_FILL_LIMIT) {
                    // Stop growing, but drain what is already queued so the painted set stays a
                    // closed shell rather than ending mid-layer.
                    truncated = true;
                    continue;
                }

                for (const ivec3& step : NEIGHBOURS) {
                    ivec3 neighbour = current + step;
                    if (visited.isMarked(neighbour)) continue;
                    uint8_t slot;
                    // Marked whatever the answer, so a rejected neighbour is tested once rather than
                    // once per voxel that touches it.
                    visited.mark(neighbour);
                    if (!queryComponentVoxel(scene, space, neighbour, slot)) continue;
                    if (editor.paintFillScope == SelectionScope::Material && slot != seedSlot) continue;
                    queue.push_back(neighbour);
                    regionSize++;
                }
            }
            break;
        }
    }
}

// Writes a set of voxel colours through the engine's edit pipeline (queue -> updateScene -> the GPU
// flush the render loop performs when gpuFlushNeeded is set).
//
// The queue works in packed colours rather than slot numbers, and internMaterial resolves a colour
// back to a slot on the way in — so painting with the palette's current entry writes that entry's
// colour and lands back on that entry, without the editor having to reason about slot numbering.
//
// One call per stroke, not per voxel: updateScene forks and rebuilds every chunk a queue touches, so
// a thousand voxels queued together cost one rebuild of each chunk they fall in, and a thousand
// separate calls would cost a thousand.
static void applyVoxelPaint(projv::Scene* scene, EditorState* editor, projv::ComponentHandle component,
                            const std::vector<projv::core::ivec3>& coords,
                            const std::vector<uint32_t>& colors) {
    if (coords.empty() || coords.size() != colors.size()) return;

    std::vector<projv::PendingVoxelOp> ops;
    ops.reserve(coords.size());
    for (size_t index = 0; index < coords.size(); index++) {
        projv::PendingVoxelOp op;
        op.isAdd = true;
        op.position = coords[index];
        op.packedColor = colors[index];
        ops.push_back(op);
    }

    if (!projv::utils::queueVoxelAdd(*scene, component, ops)) return;
    projv::utils::updateScene(*scene);
    editor->gpuFlushNeeded = true;
    editor->cameraMovedByInterface = true;   // The accumulated image is of the old colours.
}

// The undo/redo pair for a stroke. The coordinate list is shared between the two closures and with
// the caller rather than copied into each -- a fill can run to a million voxels, and three copies of
// that list is 36 MB the history would be charged for and would have to carry.
static void recordPaintStep(projv::Scene& scene, EditorState& editor, projv::ComponentHandle component,
                            std::shared_ptr<std::vector<projv::core::ivec3>> coords,
                            std::shared_ptr<std::vector<uint32_t>> previousColors,
                            uint32_t newColor, const std::string& label) {
    projv::Scene* scenePointer = &scene;
    EditorState* editorPointer = &editor;
    // One entry per voxel, so redo does not have to rebuild the uniform list every time.
    auto newColors = std::make_shared<std::vector<uint32_t>>(coords->size(), newColor);

    projv::editor::EditRecord record;
    record.label = label;
    record.memoryCost = coords->size() * (sizeof(projv::core::ivec3) + 2 * sizeof(uint32_t));
    record.undo = [=] { applyVoxelPaint(scenePointer, editorPointer, component, *coords, *previousColors); };
    record.redo = [=] { applyVoxelPaint(scenePointer, editorPointer, component, *coords, *newColors); };
    editor.history.record(std::move(record), ImGui::GetTime());
}

// --- The paint stroke ---
//
// A press, a drag, and a release, sampled once per frame the cursor moves. Everything above is per
// dab; this is what turns a run of dabs into one gesture: one locked component, one colour, one entry
// in the history, and a run of interpolated dabs filling the gap the cursor crossed between frames so
// a quick sweep is a stripe rather than a row of dots.
//
// The sculpt stroke below has a third problem this one does not -- its own deposits get in the way of
// its own ray -- because painting never changes what is solid. That is also why there is no journal of
// original state here: a voxel this stroke has already painted is the colour being painted, and
// collectPaintTargets drops those, so the accumulated list picks up each voxel exactly once.

// How far apart two dabs of the current brush may be and still overlap, in voxels. Half the shape's
// smallest extent, on the same reasoning as sculptDabSpacing; for the single-voxel brush it is one
// cell, which is the spacing at which a diagonal sweep leaves no gaps.
static float paintDabSpacing(const EditorState& editor) {
    switch (editor.paintStrokeShape) {
        case PaintShape::Sphere:
            return std::max(1.0f, std::min(editor.paintRadius, PAINT_MAX_RADIUS) * 0.5f);
        case PaintShape::Cube: {
            int smallest = PAINT_MAX_CUBE_SIDE;
            for (int axis = 0; axis < 3; axis++) {
                smallest = std::min(smallest, std::clamp(editor.paintCubeSize[axis], 1, PAINT_MAX_CUBE_SIDE));
            }
            return std::max(1.0f, 0.5f * float(smallest));
        }
        default:
            return 1.0f;
    }
}

// Drops the repeats out of one frame's collected targets.
//
// Consecutive dabs of an interpolated run overlap by design -- that is what makes the run continuous
// -- so the same voxel is collected several times within a frame. The colour test that keeps a voxel
// out of every *later* frame cannot see them, because the scene is not written until the end of this
// one. Left in, each repeat would cost an entry in the edit queue and another in the undo record, and
// an interpolated sphere is mostly overlap.
static void dropDuplicatePaintTargets(std::vector<projv::core::ivec3>& coords,
                                      std::vector<uint32_t>& previousColors) {
    if (coords.size() < 2) return;
    std::unordered_set<uint64_t> seen;
    seen.reserve(coords.size() * 2);
    size_t kept = 0;
    for (size_t index = 0; index < coords.size(); index++) {
        if (!seen.insert(packVoxelKey(coords[index])).second) continue;
        coords[kept] = coords[index];
        previousColors[kept] = previousColors[index];
        kept++;
    }
    coords.resize(kept);
    previousColors.resize(kept);
}

// Opens a stroke. Nothing is painted here -- the ray has not been cast yet -- so this only resets the
// accumulators. The component and the colour are settled by the first sample.
static void beginPaintStroke(EditorState& editor) {
    editor.paintStrokeActive = true;
    editor.paintStrokeSampled = false;
    editor.paintStrokeComponent = projv::INVALID_COMPONENT_HANDLE;
    editor.paintStrokeShape = editor.paintShape;
    editor.paintStrokeColor = 0;
    editor.paintStrokeHasAnchor = false;
    editor.paintStrokeFillTruncated = false;
    editor.paintStrokeTruncated = false;
    editor.paintStrokeCoords = std::make_shared<std::vector<projv::core::ivec3>>();
    editor.paintStrokePreviousColors = std::make_shared<std::vector<uint32_t>>();
}

// Closes a stroke and hands the whole sweep to the history as one entry, which is what makes undo put
// the scene back where the button went down rather than unpainting it a frame at a time.
static void endPaintStroke(projv::Scene& scene, EditorState& editor) {
    if (!editor.paintStrokeActive) return;
    editor.paintStrokeActive = false;

    auto coords = std::move(editor.paintStrokeCoords);
    auto previousColors = std::move(editor.paintStrokePreviousColors);
    editor.paintStrokeCoords.reset();
    editor.paintStrokePreviousColors.reset();

    projv::ComponentHandle component = editor.paintStrokeComponent;
    if (!coords || component == projv::INVALID_COMPONENT_HANDLE ||
        component >= scene.components.size()) {
        return;   // The stroke never found anything to paint into; it has already said so.
    }

    const std::string& name = scene.components[component].name;
    if (coords->empty()) {
        // Nothing to do is not the same as nothing found: the brush covered voxels, they were simply
        // already this colour. Said plainly so a stroke that changes nothing does not read as one that
        // failed.
        editor.statusMessage = editor.paintStrokeShape == PaintShape::Voxel
                             ? "Already this colour."
                             : std::string("Nothing to repaint - the ") +
                               PaintShapeLabel(editor.paintStrokeShape) +
                               " covered no voxels of a different colour.";
        return;
    }

    std::string label = std::string("Paint ") + PaintShapeLabel(editor.paintStrokeShape) + " in " + name;
    recordPaintStep(scene, editor, component, coords, previousColors, editor.paintStrokeColor, label);

    editor.statusMessage = "Painted " + std::to_string(coords->size()) + " voxel(s) in " + name +
                           (editor.paintStrokeFillTruncated
                                ? " (fill stopped at the " + std::to_string(PAINT_FILL_LIMIT) +
                                  " voxel limit)" : "") +
                           (editor.paintStrokeTruncated
                                ? " (stroke moved faster than the brush could follow)" : "");
}

// One frame of a stroke: decide which voxel the brush sits on, stamp from the last one to this one,
// and write the frame's worth of colour in a single queue. Called from processVoxelPick, which owns
// the ray -- paint needs no solidity override, so it shares the pick every other tool uses.
static void processPaintSample(projv::Scene& scene, EditorState& editor,
                               const projv::utils::VoxelPick& pick) {
    using projv::core::ivec3;
    using projv::core::vec3;

    if (!editor.paintStrokeActive) return;
    bool firstSample = !editor.paintStrokeSampled;
    editor.paintStrokeSampled = true;

    // --- Which component, and in what colour ---
    //
    // Both settled on the first sample and held for the rest of the stroke. The component is locked
    // because an edit queue belongs to one, and a sweep that wandered over a second object would
    // otherwise become two undo entries; the colour is locked because the palette panel is the one
    // notion of "the current colour" and reading it once is what makes the whole sweep one edit.
    if (editor.paintStrokeComponent == projv::INVALID_COMPONENT_HANDLE) {
        projv::ComponentHandle source = editor.paletteComponent;
        if (source >= scene.components.size() || editor.selectedMaterialSlot < 0 ||
            size_t(editor.selectedMaterialSlot) >= scene.components[source].materialPalette.size()) {
            editor.statusMessage = "No palette entry selected - pick one in the Palette panel first.";
            editor.paintStrokeActive = false;   // Nothing painted yet, so there is nothing to commit.
            return;
        }
        if (!resolveComponentVoxelSpace(scene, pick.component).valid) {
            editor.statusMessage = "Cannot paint that component (no voxel space to aim at).";
            editor.paintStrokeActive = false;
            return;
        }
        editor.paintStrokeColor =
            scene.components[source].materialPalette[editor.selectedMaterialSlot].packedColor;
        editor.paintStrokeComponent = pick.component;
    } else if (pick.component != editor.paintStrokeComponent) {
        // The sweep crossed onto something else. It stays locked to its own component, and the anchor
        // is dropped so that coming back does not draw a line through whatever was in between.
        editor.paintStrokeHasAnchor = false;
        return;
    }

    projv::ComponentHandle component = editor.paintStrokeComponent;
    ivec3 centre;
    if (!pickToComponentVoxelCoord(scene, pick, centre)) {
        editor.paintStrokeHasAnchor = false;
        return;
    }

    std::vector<ivec3> frameCoords;
    std::vector<uint32_t> framePreviousColors;
    bool interpolated = false;

    if (paintShapeIsFill(editor.paintStrokeShape)) {
        // A fill runs on the press and the rest of the drag is ignored -- see paintShapeIsFill.
        if (!firstSample) return;
        bool truncated = false;
        collectPaintTargets(scene, component, centre, pick.materialSlot, pick.faceNormal, editor,
                            editor.paintStrokeColor, frameCoords, framePreviousColors, truncated);
        editor.paintStrokeFillTruncated = editor.paintStrokeFillTruncated || truncated;
    } else if (!editor.paintStrokeHasAnchor) {
        bool truncated = false;
        collectPaintTargets(scene, component, centre, pick.materialSlot, pick.faceNormal, editor,
                            editor.paintStrokeColor, frameCoords, framePreviousColors, truncated);
    } else if (centre != editor.paintStrokeLastCenter) {
        // Fill the gap the cursor crossed since the last frame, so a quick sweep is a stripe rather
        // than a row of dots. Starts at step 1: step 0 is the previous frame's dab, already painted.
        vec3 from(editor.paintStrokeLastCenter);
        vec3 to(centre);
        float span = glm::length(to - from);
        int steps = std::max(1, int(std::ceil(span / paintDabSpacing(editor))));
        if (steps > PAINT_MAX_INTERPOLATED_STEPS) {
            steps = PAINT_MAX_INTERPOLATED_STEPS;
            editor.paintStrokeTruncated = true;
        }
        interpolated = steps > 1;
        for (int step = 1; step <= steps; step++) {
            vec3 point = from + (to - from) * (float(step) / float(steps));
            ivec3 dabCentre(int(std::lround(point.x)), int(std::lround(point.y)),
                            int(std::lround(point.z)));
            bool truncated = false;
            collectPaintTargets(scene, component, dabCentre, pick.materialSlot, pick.faceNormal, editor,
                                editor.paintStrokeColor, frameCoords, framePreviousColors, truncated);
        }
    } else {
        return;   // The cursor moved, but not off the voxel it was already on.
    }

    editor.paintStrokeLastCenter = centre;
    editor.paintStrokeHasAnchor = true;

    if (interpolated) dropDuplicatePaintTargets(frameCoords, framePreviousColors);
    if (frameCoords.empty()) return;

    // Kept for the history, which is handed the whole sweep at once when the button comes up.
    editor.paintStrokeCoords->insert(editor.paintStrokeCoords->end(),
                                     frameCoords.begin(), frameCoords.end());
    editor.paintStrokePreviousColors->insert(editor.paintStrokePreviousColors->end(),
                                             framePreviousColors.begin(), framePreviousColors.end());

    // One queue and one chunk rebuild for everything this frame produced, however many dabs that was.
    applyVoxelPaint(&scene, &editor, component, frameCoords,
                    std::vector<uint32_t>(frameCoords.size(), editor.paintStrokeColor));
}

// =============================================================================
// Sculpting
// =============================================================================
//
// A stroke is a press, a drag, and a release. Every frame of the drag casts one ray, turns where it
// landed into a cell of the target component, and stamps the brush there — so the geometry appears
// under the cursor as it is drawn rather than at the end.
//
// The whole difficulty is in that "every frame": the scene the ray is cast into already contains what
// the earlier frames of the same stroke put there. Left alone, an additive drag hits its own last dab
// (which is nearer the camera than the surface was), places the next one on top of that, and walks a
// column of voxels back up the ray in a few frames. Removal has the mirror version: the ray falls
// through the hole it just made and eats a tunnel straight through the object.
//
// Both are fixed by the same thing, and it is the reason utils::pickVoxel takes a solidity override:
// for as long as the button is held, the ray is shown the surface the stroke *started* against.
// EditorState::sculptStrokeOriginal remembers that surface a cell at a time, and
// makeSculptStrokeOverride is the predicate over it.

// The lie the ray is told while a stroke is in progress; see the section note and the doc comment on
// utils::VoxelSolidityOverride. Empty (and so ignored) until the stroke has locked onto a component.
//
// Only voxels of the stroke's own component are overridden. Everything else in the scene answers
// normally, which is what lets a stroke that wanders across a second object still land somewhere
// sensible instead of falling through it.
static projv::utils::VoxelSolidityOverride makeSculptStrokeOverride(const projv::Scene& scene,
                                                                    const EditorState& editor) {
    projv::ComponentHandle component = editor.sculptStrokeComponent;
    if (!editor.sculptStrokeActive || component == projv::INVALID_COMPONENT_HANDLE) {
        return projv::utils::VoxelSolidityOverride();
    }
    const std::unordered_map<uint64_t, EditorState::StrokeVoxel>* original = &editor.sculptStrokeOriginal;

    return [&scene, component, original](projv::ChunkHandle chunk, projv::core::ivec3 local,
                                         bool solidInScene) {
        projv::core::ivec3 coord;
        if (!chunkVoxelToComponentCoord(scene, component, chunk, local, coord)) return solidInScene;
        auto remembered = original->find(packVoxelKey(coord));
        if (remembered == original->end()) return solidInScene;
        // Whatever the stroke has since done here, the ray is shown what was here to begin with.
        return remembered->second.wasSolid;
    };
}

// Every coordinate one dab of the brush covers, before anything is asked about what is already there.
static void collectSculptDab(const EditorState& editor, projv::core::ivec3 centre,
                             std::vector<projv::core::ivec3>& out) {
    using projv::core::ivec3;

    switch (editor.sculptBrush) {
        case SculptBrush::Sphere: {
            // Scanned as a box and rejected by squared distance, like the paint sphere: no square
            // roots, and the box is the smallest one that can hold the ball anyway.
            float radius = std::min(editor.sculptRadius, SCULPT_MAX_RADIUS);
            int extent = int(std::floor(radius));
            float radiusSquared = radius * radius;
            for (int z = -extent; z <= extent; z++) {
                for (int y = -extent; y <= extent; y++) {
                    for (int x = -extent; x <= extent; x++) {
                        if (float(x * x + y * y + z * z) > radiusSquared) continue;
                        out.push_back(centre + ivec3(x, y, z));
                    }
                }
            }
            break;
        }

        case SculptBrush::Cube: {
            // Same centring convention as the paint cube: an even side puts one more voxel on the
            // high side than the low, and the cell under the cursor is always inside the box.
            const float sides[3] = { editor.sculptCubeWidth, editor.sculptCubeHeight,
                                     editor.sculptCubeDepth };
            int size[3];
            int low[3];
            for (int axis = 0; axis < 3; axis++) {
                size[axis] = std::clamp(int(std::lround(sides[axis])), 1, int(SCULPT_MAX_CUBE_SIDE));
                low[axis] = -(size[axis] / 2);
            }
            for (int z = low[2]; z < low[2] + size[2]; z++) {
                for (int y = low[1]; y < low[1] + size[1]; y++) {
                    for (int x = low[0]; x < low[0] + size[0]; x++) {
                        out.push_back(centre + ivec3(x, y, z));
                    }
                }
            }
            break;
        }

        case SculptBrush::Smooth:
        case SculptBrush::Bump:
        case SculptBrush::Extrude:
            // None of these stamp a shape, so none reaches here: Smooth and Bump run an operator over
            // the sphere (see applySculptIteration) and Extrude is a face drag, and processSculptSample
            // sends all three down their own paths long before this. Listed so the switch is exhaustive.
            break;
    }
}

// How far apart two dabs of the current brush may be and still overlap. Half the brush's smallest
// extent: at that spacing consecutive dabs always share voxels, so an interpolated run of them is a
// solid ridge rather than a string of beads.
static float sculptDabSpacing(const EditorState& editor) {
    float extent = std::min(editor.sculptRadius, SCULPT_MAX_RADIUS);
    if (editor.sculptBrush == SculptBrush::Cube) {
        extent = 0.5f * std::min({ editor.sculptCubeWidth, editor.sculptCubeHeight,
                                   editor.sculptCubeDepth });
    }
    return std::max(1.0f, extent * 0.5f);
}

// Writes a set of voxels through the engine's edit pipeline, in one queue and one updateScene for the
// whole set -- updateScene forks and rebuilds every chunk a queue touches, so this is called once per
// frame of a stroke with everything that frame's dabs produced, never once per dab and never once per
// voxel.
//
// `colors` is read only when adding; a removal does not need one.
static void applyVoxelSculpt(projv::Scene* scene, EditorState* editor, projv::ComponentHandle component,
                             const std::vector<projv::core::ivec3>& coords,
                             const std::vector<uint32_t>& colors, bool add) {
    if (coords.empty()) return;
    if (add && colors.size() != coords.size()) return;

    std::vector<projv::PendingVoxelOp> ops;
    ops.reserve(coords.size());
    for (size_t index = 0; index < coords.size(); index++) {
        projv::PendingVoxelOp op;
        op.isAdd = add;
        op.position = coords[index];
        op.packedColor = add ? colors[index] : 0u;
        ops.push_back(op);
    }

    bool queued = add ? projv::utils::queueVoxelAdd(*scene, component, ops)
                      : projv::utils::queueVoxelRemove(*scene, component, ops);
    if (!queued) return;

    projv::utils::updateScene(*scene);
    editor->gpuFlushNeeded = true;
    editor->cameraMovedByInterface = true;   // The accumulated image is of the old geometry.
}

// Remembers what a cell held before this stroke first touched it, and answers whether the stroke has
// seen it before. The single point at which sculptStrokeOriginal grows -- both the ray override and
// the undo record are downstream of exactly this.
static const EditorState::StrokeVoxel& rememberOriginal(projv::Scene& scene, EditorState& editor,
                                                        const ComponentVoxelSpace& space,
                                                        projv::core::ivec3 coord, uint64_t key) {
    auto existing = editor.sculptStrokeOriginal.find(key);
    if (existing != editor.sculptStrokeOriginal.end()) return existing->second;

    EditorState::StrokeVoxel record;
    record.coord = coord;
    uint8_t slot = 0;
    record.wasSolid = queryComponentVoxel(scene, space, coord, slot);
    if (record.wasSolid) {
        record.oldColor = componentVoxelColor(scene, editor.sculptStrokeComponent, slot);
    }
    return editor.sculptStrokeOriginal.emplace(key, record).first->second;
}

// One dab of a shape brush: the brush's coordinates, filtered down to the ones that actually change,
// applied, and recorded.
//
// The filter is what keeps the tool from doing pointless work and the undo record honest. In Add mode
// a cell that is already solid is left alone -- writing it would change nothing, and recording it
// would leave a cell in the stroke's journal that the stroke never altered. Remove mode is the mirror.
// A cell the stroke has already changed is skipped for the same reason: a dab that overlaps the
// previous one must not queue the same voxel twice.
static void stampSculptDab(projv::Scene& scene, EditorState& editor, const ComponentVoxelSpace& space,
                           projv::core::ivec3 centre,
                           std::vector<projv::core::ivec3>& frameCoords,
                           std::vector<uint32_t>& frameColors) {
    static std::vector<projv::core::ivec3> candidates;   // Reused; a dab can be 100k coordinates.
    candidates.clear();
    collectSculptDab(editor, centre, candidates);

    bool addMode = editor.sculptStrokeMode == SculptMode::Add;
    for (const projv::core::ivec3& coord : candidates) {
        uint64_t key = packVoxelKey(coord);
        if (editor.sculptStrokeOriginal.count(key) != 0) continue;   // Already changed by this stroke.

        uint8_t slot = 0;
        bool solid = queryComponentVoxel(scene, space, coord, slot);
        if (addMode == solid) continue;   // Adding into stone, or removing from air: nothing to do.

        rememberOriginal(scene, editor, space, coord, key);
        frameCoords.push_back(coord);
        if (addMode) frameColors.push_back(editor.sculptStrokeColor);
    }
}

// =============================================================================
// Smooth and Bump
// =============================================================================
//
// The two brushes that reshape rather than place. Neither stamps anything: each runs one pass of an
// operator over the cells inside the brush sphere, and repeats it on a tick for as long as the button
// is held. That is what makes them behave the way a physical tool does -- hold longer, get more --
// and it is why they work with the cursor standing still, unlike the shape brushes.
//
// Both read the *current* geometry, not the stroke's starting state: each pass must build on the last
// or holding the button would do the same thing forever. That is a deliberate exception to how the
// stroke sees the world, and it is safe because neither of them takes its position from the geometry.
// The ray still finds the original surface (that is what keeps the brush parked where the user aimed
// it), and only the operator inside the sphere looks at what is actually there now.

// What one pass does. Separate from the brush because a single Bump tick runs a pull or a push and
// then, if the blend calls for it, one or more Smooth passes over the same place.
enum class SculptOperator {
    Smooth,
    BumpPull,
    BumpPush
};

// A dense copy of the neighbourhood the tick works in, and the reason these brushes are affordable.
//
// **The cost of these two brushes is reading the geometry, not writing it.** Both operators ask about
// a cell and then about the cells around it, and neighbouring cells overlap heavily -- a 3x3x3 count
// re-reads 18 of the 27 cells its neighbour just read. Asked of the tree64 one at a time that is ~28
// descents per cell: half a million per tick of a radius-10 Smooth, five million a second at ten
// ticks, and at the largest radius far more than the machine can do in the time available. Copying
// the box into a flat array first makes it one descent per cell and turns every pass into array
// indexing -- the same tick drops to ~17k descents.
//
// Writing is not the problem and never was: the engine's edit path handles large geometry edits
// comfortably. Gathering the whole tick into a single difference is worth doing anyway -- it is
// simpler than applying each pass, and it keeps the intermediate states of a blend off the screen --
// but it is a tidiness, not the fix.
//
// Buffers are kept between ticks (this runs ten times a second, and the box is up to ~830 KB at the
// largest radius) and this is single-threaded UI code.
struct SculptScratch {
    projv::core::ivec3 origin = projv::core::ivec3(0);   // Component coord of the box's low corner.
    int side = 0;
    std::vector<uint8_t> solid;
    std::vector<uint32_t> color;
    std::vector<uint8_t> initialSolid;
    std::vector<uint32_t> initialColor;

    int index(projv::core::ivec3 coord) const {
        projv::core::ivec3 local = coord - origin;
        if (local.x < 0 || local.y < 0 || local.z < 0 ||
            local.x >= side || local.y >= side || local.z >= side) {
            return -1;
        }
        return (local.z * side + local.y) * side + local.x;
    }
    bool solidAt(projv::core::ivec3 coord) const {
        int at = index(coord);
        // Outside the box reads as empty. Nothing is ever written within one cell of the boundary --
        // see how boxRadius is chosen -- so no decision ever depends on this.
        return at >= 0 && solid[size_t(at)] != 0;
    }
};

// One descent per cell, into the flat array.
static void snapshotSculptScratch(const projv::Scene& scene, projv::ComponentHandle component,
                                  const ComponentVoxelSpace& space, projv::core::ivec3 centre,
                                  int boxRadius, SculptScratch& scratch) {
    using projv::core::ivec3;

    scratch.side = 2 * boxRadius + 1;
    scratch.origin = centre - ivec3(boxRadius);
    size_t cells = size_t(scratch.side) * size_t(scratch.side) * size_t(scratch.side);
    scratch.solid.assign(cells, 0);
    scratch.color.assign(cells, 0);

    size_t at = 0;
    for (int z = 0; z < scratch.side; z++) {
        for (int y = 0; y < scratch.side; y++) {
            for (int x = 0; x < scratch.side; x++, at++) {
                uint8_t slot = 0;
                if (!queryComponentVoxel(scene, space, scratch.origin + ivec3(x, y, z), slot)) continue;
                scratch.solid[at] = 1;
                scratch.color[at] = componentVoxelColor(scene, component, slot);
            }
        }
    }
    scratch.initialSolid = scratch.solid;
    scratch.initialColor = scratch.color;
}

// One pass over a sphere, entirely within the scratch array.
//
// Every cell is decided against the state at the *start* of the pass: the changes are gathered first
// and written to the array afterwards. Deciding against a half-updated array would make the result
// depend on the order of the loop -- a pass sweeping in +x would smooth differently from one sweeping
// in -x, and a bump would run away across the sphere in a single tick instead of growing one layer.
//
// `smoothCutoff` and `smoothKernelRadius` are the two halves of Smooth's strength, and are ignored by
// the two Bump operators; see SCULPT_SMOOTH_MAX_CUTOFF and SCULPT_SMOOTH_MAX_KERNEL.
static void runSculptPass(SculptScratch& scratch, projv::core::ivec3 centre, SculptOperator op,
                          float radius, int smoothCutoff, int smoothKernelRadius) {
    using projv::core::ivec3;

    static std::vector<int> changedIndices;
    static std::vector<uint8_t> changedSolid;
    static std::vector<uint32_t> changedColor;
    changedIndices.clear();
    changedSolid.clear();
    changedColor.clear();

    int extent = int(std::floor(radius));
    float radiusSquared = radius * radius;
    bool pulling = op == SculptOperator::BumpPull;
    int smoothThreshold = sculptSmoothThreshold(smoothKernelRadius);

    static const ivec3 FACE_NEIGHBOURS[6] = {
        ivec3(1, 0, 0), ivec3(-1, 0, 0), ivec3(0, 1, 0),
        ivec3(0, -1, 0), ivec3(0, 0, 1), ivec3(0, 0, -1)
    };

    for (int z = -extent; z <= extent; z++) {
        for (int y = -extent; y <= extent; y++) {
            for (int x = -extent; x <= extent; x++) {
                if (float(x * x + y * y + z * z) > radiusSquared) continue;
                ivec3 coord = centre + ivec3(x, y, z);
                int at = scratch.index(coord);
                if (at < 0) continue;
                bool solid = scratch.solid[size_t(at)] != 0;

                if (op == SculptOperator::Smooth) {
                    // Solid cells in the kernel, including this one. Counting is all this loop does:
                    // the colour vote below is worth its own pass over a much smaller box, and only
                    // for the cells that turn out to be changing.
                    int neighbours = 0;
                    for (int nz = -smoothKernelRadius; nz <= smoothKernelRadius; nz++) {
                        for (int ny = -smoothKernelRadius; ny <= smoothKernelRadius; ny++) {
                            for (int nx = -smoothKernelRadius; nx <= smoothKernelRadius; nx++) {
                                int neighbour = scratch.index(coord + ivec3(nx, ny, nz));
                                if (neighbour >= 0 && scratch.solid[size_t(neighbour)]) neighbours++;
                            }
                        }
                    }
                    bool shouldBeSolid = neighbours > smoothThreshold;
                    if (shouldBeSolid == solid) continue;
                    // How far past the line this cell sits, and the cutoff is a floor on it. Both
                    // sides start at 1 -- a solid cell flips at the threshold or fewer, an empty one
                    // at one more -- so a single comparison covers shaving and filling alike, and a
                    // surface with nothing out of place is left untouched at every setting.
                    int disagreement = solid ? (smoothThreshold + 1 - neighbours)
                                             : (neighbours - smoothThreshold);
                    if (disagreement < smoothCutoff) continue;

                    // The colour a *filled* cell takes: the one that turns up most often among the
                    // cells it actually touches, so a dent healed in the middle of a wall comes back
                    // as that wall rather than as whatever happened to be nearest. Deliberately the
                    // inner 3x3x3 whatever the kernel is -- widening the kernel widens what counts as
                    // rough, not what a cell is made of, and a vote over 343 cells would be both
                    // wrong (colours from three voxels away are not what this cell adjoins) and, with
                    // a per-voxel palette, the most expensive thing in the tool.
                    uint32_t bestColor = 0;
                    if (shouldBeSolid) {
                        int bestCount = 0;
                        uint32_t seenColors[27];
                        int seenCounts[27];
                        int seenTotal = 0;
                        for (int nz = -1; nz <= 1; nz++) {
                            for (int ny = -1; ny <= 1; ny++) {
                                for (int nx = -1; nx <= 1; nx++) {
                                    int neighbour = scratch.index(coord + ivec3(nx, ny, nz));
                                    if (neighbour < 0 || !scratch.solid[size_t(neighbour)]) continue;
                                    uint32_t found = scratch.color[size_t(neighbour)];
                                    int slot = 0;
                                    for (; slot < seenTotal; slot++) {
                                        if (seenColors[slot] == found) break;
                                    }
                                    if (slot == seenTotal) {
                                        seenColors[seenTotal] = found;
                                        seenCounts[seenTotal] = 0;
                                        seenTotal++;
                                    }
                                    if (++seenCounts[slot] > bestCount) {
                                        bestCount = seenCounts[slot];
                                        bestColor = found;
                                    }
                                }
                            }
                        }
                    }

                    changedIndices.push_back(at);
                    changedSolid.push_back(shouldBeSolid ? 1 : 0);
                    changedColor.push_back(bestColor);
                    continue;
                }

                // Bump. Pulling grows the surface outward by one layer -- every empty cell that
                // touches solid becomes solid, in that neighbour's own colour, so the bump is made of
                // whatever it is growing out of. Pushing is the exact mirror, peeling one layer off.
                // Face neighbours only, for the same reason every other spread in the editor uses
                // them: growing diagonally would round the bump into a ball no matter what shape the
                // surface under it had.
                if (pulling == solid) continue;   // Pulling only fills air; pushing only eats stone.

                bool touchesOpposite = false;
                uint32_t sourceColor = 0;
                for (const ivec3& step : FACE_NEIGHBOURS) {
                    int neighbour = scratch.index(coord + step);
                    if (neighbour < 0) continue;
                    if ((scratch.solid[size_t(neighbour)] != 0) == pulling) {
                        touchesOpposite = true;
                        sourceColor = scratch.color[size_t(neighbour)];
                        break;
                    }
                }
                if (!touchesOpposite) continue;

                changedIndices.push_back(at);
                changedSolid.push_back(pulling ? 1 : 0);
                changedColor.push_back(sourceColor);
            }
        }
    }

    for (size_t change = 0; change < changedIndices.size(); change++) {
        size_t at = size_t(changedIndices[change]);
        scratch.solid[at] = changedSolid[change];
        if (changedSolid[change]) scratch.color[at] = changedColor[change];
    }
}

// One tick of an iterative brush: snapshot, run every pass in memory, write the difference once.
//
// Smooth is one pass and nothing else. Bump is a pull or a push followed by however many smoothing
// passes the blend asks for, over a slightly wider sphere so the join at the brush's rim is included
// -- that join is the whole reason the blend exists. Blend zero skips them and leaves the bare
// operator, which is a disc with a cliff around it.
static void applySculptIteration(projv::Scene& scene, EditorState& editor, projv::core::ivec3 centre) {
    using projv::core::ivec3;

    projv::ComponentHandle component = editor.sculptStrokeComponent;
    ComponentVoxelSpace space = resolveComponentVoxelSpace(scene, component);
    if (!space.valid) return;

    bool smoothing = editor.sculptStrokeBrush == SculptBrush::Smooth;
    float radius = std::min(editor.sculptRadius, SCULPT_MAX_RADIUS);
    int blendPasses = smoothing ? 0
        : int(std::lround(std::clamp(editor.sculptBlendStrength, 0.0f, SCULPT_MAX_BLEND)));
    int smoothKernel = smoothing ? sculptSmoothKernelRadius(editor.sculptSmoothStrength) : 1;

    // The furthest a pass may write, and a kernel further for the neighbourhood it reads there -- so
    // every decision the tick makes is taken against real geometry rather than against the box's edge.
    // Bump reads face neighbours only and its blend passes run at the default width, so one cell is
    // the margin for everything except a widened Smooth.
    float writeRadius = radius + (blendPasses > 0 ? SCULPT_BLEND_MARGIN : 0.0f);
    int boxRadius = int(std::floor(writeRadius)) + smoothKernel;

    static SculptScratch scratch;
    snapshotSculptScratch(scene, component, space, centre, boxRadius, scratch);

    if (smoothing) {
        runSculptPass(scratch, centre, SculptOperator::Smooth, radius,
                      sculptSmoothCutoff(editor.sculptSmoothStrength), smoothKernel);
    } else {
        bool pulling = editor.sculptStrokeMode == SculptMode::Add;
        runSculptPass(scratch, centre, pulling ? SculptOperator::BumpPull : SculptOperator::BumpPush,
                      radius, 1, 1);
        // Full strength and the default width, whatever Smooth's own slider says: the blend exists to
        // round off the cliff at the brush's rim, and either a weakened or a widened pass would do
        // something other than that -- one would spare the cliff, the other would reach well past it.
        for (int pass = 0; pass < blendPasses; pass++) {
            runSculptPass(scratch, centre, SculptOperator::Smooth, radius + SCULPT_BLEND_MARGIN, 1, 1);
        }
    }

    // The whole tick as one difference: two queue calls and two chunk rebuilds however many passes ran.
    std::vector<ivec3> toAdd, toRemove;
    std::vector<uint32_t> addColors;
    size_t at = 0;
    for (int z = 0; z < scratch.side; z++) {
        for (int y = 0; y < scratch.side; y++) {
            for (int x = 0; x < scratch.side; x++, at++) {
                bool before = scratch.initialSolid[at] != 0;
                bool after = scratch.solid[at] != 0;
                if (before == after && (!after || scratch.color[at] == scratch.initialColor[at])) {
                    continue;
                }
                ivec3 coord = scratch.origin + ivec3(x, y, z);
                if (after) {
                    toAdd.push_back(coord);
                    addColors.push_back(scratch.color[at]);
                } else {
                    toRemove.push_back(coord);
                }
            }
        }
    }
    if (toAdd.empty() && toRemove.empty()) return;

    for (const ivec3& coord : toAdd) rememberOriginal(scene, editor, space, coord, packVoxelKey(coord));
    for (const ivec3& coord : toRemove) rememberOriginal(scene, editor, space, coord, packVoxelKey(coord));

    // Removals first, so a tick that does both never shows the intermediate state for a frame.
    applyVoxelSculpt(&scene, &editor, component, toRemove, std::vector<uint32_t>(), false);
    applyVoxelSculpt(&scene, &editor, component, toAdd, addColors, true);
}

// =============================================================================
// Extrude
// =============================================================================
//
// Click a face, drag, and that whole face moves. Not a brush: nothing is stamped along the path of
// the cursor, the shape of the result is the shape of the face, and the cursor supplies exactly one
// number -- how far.
//
// Which voxels count as "the face you clicked on": every solid voxel that
//
//   * lies in the same plane as the one under the cursor (same coordinate along the normal axis),
//   * has its face open in the same direction -- so it is part of the *surface*, not buried behind it,
//   * uses the same palette entry, and
//   * is reachable from the clicked voxel through face neighbours within that plane.
//
// The material test is what makes this per-material: a wall of stone meeting a wall of brick at the
// same height gives you the stone, not both. Entries are compared by slot rather than by colour, for
// the same reason the paint fill does -- two entries holding the same colour are different materials.
// Connectivity is 4-way within the plane, again like the fill, because diagonal connectivity leaks
// across the gap where two faces touch only at a corner.
//
// The pulled-out voxels take the face's own material, not the palette's current entry. Extruding a
// wall extends that wall; being handed whatever colour happened to be selected in another panel is
// not what "pull this face out" means.
//
// Note what is *absent* here: a per-frame ray cast into the geometry. The brush needs one every frame
// and therefore needs the solidity override to keep from chasing its own deposits (see above). Extrude
// fixes its face and its axis on the press and reads depth by projecting the cursor onto that axis, so
// the geometry it creates was never in a position to mislead it.

// Defined further down with the transform gizmo -- the same projection a translate handle uses to
// turn a drag into a distance along one axis. Declared here rather than moved, so the gizmo's helpers
// stay together where they are used most.
static bool closestPointOnAxis(projv::core::vec3 axisOrigin, projv::core::vec3 axisDirection,
                               projv::core::vec3 rayOrigin, projv::core::vec3 rayDirection,
                               float& outT);


// Moves the committed depth to `target`, one layer at a time, and writes the difference.
//
// Walking layer by layer rather than jumping is what makes a drag reversible in place: pulling out to
// five and back to two reverses layers five, four and three, leaving exactly the state a drag straight
// to two would have produced. A whole frame's worth of layers goes out in at most one add and one
// remove, however many the cursor crossed.
static void applyExtrudeDepth(projv::Scene& scene, EditorState& editor,
                              const ComponentVoxelSpace& space, int target) {
    using projv::core::ivec3;

    target = std::clamp(target, -EXTRUDE_MAX_DEPTH, EXTRUDE_MAX_DEPTH);
    if (target == editor.extrudeAppliedDepth) return;

    projv::ComponentHandle component = editor.sculptStrokeComponent;
    const ivec3& normal = editor.extrudeNormal;

    // Accumulated across every layer this call crosses. Removals are issued before additions, which
    // matters when reversing a layer: the cells it filled have to go before the cells it displaced
    // come back. Layers never share coordinates, so the order between them does not matter.
    std::vector<ivec3> toRemove;
    std::vector<ivec3> toAdd;
    std::vector<uint32_t> addColors;

    // Undo one layer: take away what it put there, put back what it pushed out of the way.
    auto reverseLayer = [&](int layer) {
        auto record = editor.extrudeLayers.find(layer);
        if (record == editor.extrudeLayers.end()) return;
        toRemove.insert(toRemove.end(), record->second.addedCoords.begin(),
                        record->second.addedCoords.end());
        toAdd.insert(toAdd.end(), record->second.restoreCoords.begin(),
                     record->second.restoreCoords.end());
        addColors.insert(addColors.end(), record->second.restoreColors.begin(),
                         record->second.restoreColors.end());
        editor.extrudeLayers.erase(record);
    };

    // Record what a layer's cells hold right now, before touching them.
    auto captureLayer = [&](int layer) {
        EditorState::ExtrudeLayerRecord record;
        for (const ivec3& faceVoxel : editor.extrudeFace) {
            ivec3 coord = faceVoxel + normal * layer;
            uint8_t slot = 0;
            if (queryComponentVoxel(scene, space, coord, slot)) {
                record.restoreCoords.push_back(coord);
                record.restoreColors.push_back(componentVoxelColor(scene, component, slot));
            } else {
                record.addedCoords.push_back(coord);
            }
        }
        return record;
    };

    while (editor.extrudeAppliedDepth < target) {
        int layer = editor.extrudeAppliedDepth + 1;
        if (layer >= 1) {
            // Outward: a new layer standing off the face, in the face's own material. Every cell of
            // it is written, including any that already held something -- that is what displacing
            // means -- and captureLayer has just recorded which were which.
            EditorState::ExtrudeLayerRecord record = captureLayer(layer);
            for (size_t index = 0; index < editor.extrudeFace.size(); index++) {
                toAdd.push_back(editor.extrudeFace[index] + normal * layer);
                addColors.push_back(editor.extrudeFaceColors[index]);
            }
            editor.extrudeLayers[layer] = std::move(record);
        } else {
            reverseLayer(layer);   // Coming back out of a carve.
        }
        editor.extrudeAppliedDepth = layer;
    }

    while (editor.extrudeAppliedDepth > target) {
        int layer = editor.extrudeAppliedDepth;
        if (layer >= 1) {
            reverseLayer(layer);   // Retracting an outward layer.
        } else {
            // Carving into the shape. The columns are not all the same depth -- a shape can be
            // thinner in places than the face is wide -- so only the cells that actually hold
            // something are removed, and what they held is kept for the way back.
            EditorState::ExtrudeLayerRecord record = captureLayer(layer);
            toRemove.insert(toRemove.end(), record.restoreCoords.begin(), record.restoreCoords.end());
            record.addedCoords.clear();   // Carving adds nothing; empty cells here stay empty.
            editor.extrudeLayers[layer] = std::move(record);
        }
        editor.extrudeAppliedDepth = layer - 1;
    }

    applyVoxelSculpt(&scene, &editor, component, toRemove, std::vector<uint32_t>(), false);
    applyVoxelSculpt(&scene, &editor, component, toAdd, addColors, true);
}

// The press: choose the face, and set up the axis the drag will be measured along.
static bool beginExtrudeDrag(projv::Scene& scene, EditorState& editor, const ComponentVoxelSpace& space,
                             const projv::utils::VoxelPick& pick, projv::core::ivec3 faceCoord,
                             projv::core::ivec3 faceNormal, projv::core::vec3 rayDirection) {
    if (faceNormal == projv::core::ivec3(0)) {
        editor.statusMessage = "Point at a face to extrude it.";
        return false;
    }

    editor.extrudeFace.clear();
    editor.extrudeFaceColors.clear();
    editor.extrudeLayers.clear();
    editor.extrudeAppliedDepth = 0;
    editor.extrudeNormal = faceNormal;
    gatherFaceRegion(scene, space, editor.sculptStrokeComponent, faceCoord, faceNormal,
                     pick.materialSlot, editor.extrudeFaceScope, EXTRUDE_MAX_FACE_VOXELS,
                     editor.extrudeFace, editor.extrudeFaceColors, editor.extrudeFaceTruncated);

    if (editor.extrudeFace.empty()) {
        editor.statusMessage = "No face to extrude there.";
        return false;
    }

    editor.extrudeAxisWorld = glm::normalize(glm::mat3_cast(space.rotation) *
                                             projv::core::vec3(faceNormal));
    editor.extrudeAnchorWorld = componentVoxelToWorld(space, faceCoord);
    editor.extrudeStartAlongAxis = 0.0f;
    closestPointOnAxis(editor.extrudeAnchorWorld, editor.extrudeAxisWorld, editor.cameraPosition,
                              rayDirection, editor.extrudeStartAlongAxis);

    editor.statusMessage = "Extruding " + std::to_string(editor.extrudeFace.size()) + " voxel face" +
                           (editor.extrudeFaceTruncated
                                ? " (stopped at the " + std::to_string(EXTRUDE_MAX_FACE_VOXELS) +
                                  " voxel limit)" : "");
    return true;
}

// Every later frame: where has the cursor slid along the face's normal, in whole voxels?
static void updateExtrudeDrag(projv::Scene& scene, EditorState& editor,
                              const ComponentVoxelSpace& space, projv::core::vec3 rayDirection) {
    float along = 0.0f;
    // False when the axis is edge-on to the view, where the projection is meaningless and would jump.
    // Holding the last depth is the right answer: the user cannot aim along an axis they cannot see,
    // and the drag stays live for when the camera or the cursor moves off it again.
    if (!closestPointOnAxis(editor.extrudeAnchorWorld, editor.extrudeAxisWorld,
                            editor.cameraPosition, rayDirection, along)) {
        return;
    }
    float layers = (along - editor.extrudeStartAlongAxis) / space.voxelSize;
    applyExtrudeDepth(scene, editor, space, int(std::lround(layers)));
}

// The release. One history entry for the whole drag, holding only its net effect -- the intermediate
// depths a wavering cursor passed through were already undone on the way, so what is left is exactly
// the difference between the shape before the press and the shape now.
static void endExtrudeDrag(projv::Scene& scene, EditorState& editor) {
    using projv::core::ivec3;

    projv::ComponentHandle component = editor.sculptStrokeComponent;
    int depth = editor.extrudeAppliedDepth;
    if (depth == 0 || component == projv::INVALID_COMPONENT_HANDLE) {
        editor.extrudeFace.clear();
        editor.extrudeLayers.clear();
        return;
    }
    bool grew = depth > 0;

    // The net effect, gathered from the layer records rather than recomputed. Undoing is the same two
    // rules one layer's reversal uses, applied to every layer at once: remove what the drag added,
    // and put back what it displaced.
    auto undoRemove = std::make_shared<std::vector<ivec3>>();
    auto undoRestore = std::make_shared<std::vector<ivec3>>();
    auto undoColors = std::make_shared<std::vector<uint32_t>>();
    // Redo writes the end state again: outward layers get each column's own colour back, and a carve
    // takes back out exactly the cells it removed.
    auto redoAdd = std::make_shared<std::vector<ivec3>>();
    auto redoAddColors = std::make_shared<std::vector<uint32_t>>();
    auto redoRemove = std::make_shared<std::vector<ivec3>>();

    int first = grew ? 1 : depth + 1;
    int last = grew ? depth : 0;
    for (int layer = first; layer <= last; layer++) {
        auto record = editor.extrudeLayers.find(layer);
        if (record == editor.extrudeLayers.end()) continue;
        undoRemove->insert(undoRemove->end(), record->second.addedCoords.begin(),
                           record->second.addedCoords.end());
        undoRestore->insert(undoRestore->end(), record->second.restoreCoords.begin(),
                            record->second.restoreCoords.end());
        undoColors->insert(undoColors->end(), record->second.restoreColors.begin(),
                           record->second.restoreColors.end());
        if (grew) {
            for (size_t index = 0; index < editor.extrudeFace.size(); index++) {
                redoAdd->push_back(editor.extrudeFace[index] + editor.extrudeNormal * layer);
                redoAddColors->push_back(editor.extrudeFaceColors[index]);
            }
        } else {
            redoRemove->insert(redoRemove->end(), record->second.restoreCoords.begin(),
                               record->second.restoreCoords.end());
        }
    }

    size_t count = undoRemove->size() + undoRestore->size();
    editor.extrudeFace.clear();
    editor.extrudeFaceColors.clear();
    editor.extrudeLayers.clear();
    if (count == 0) return;

    projv::Scene* scenePointer = &scene;
    EditorState* editorPointer = &editor;

    projv::editor::EditRecord record;
    record.label = std::string(grew ? "Extrude out " : "Extrude in ") + std::to_string(std::abs(depth)) +
                   " in " + scene.components[component].name;
    record.memoryCost = count * (sizeof(ivec3) + sizeof(uint32_t)) +
                        redoAdd->size() * sizeof(ivec3);
    // Removals before additions, the same ordering applyExtrudeDepth uses: a cell that was displaced
    // has to be emptied of what replaced it before its own contents go back.
    record.undo = [=] {
        applyVoxelSculpt(scenePointer, editorPointer, component, *undoRemove,
                         std::vector<uint32_t>(), false);
        applyVoxelSculpt(scenePointer, editorPointer, component, *undoRestore, *undoColors, true);
    };
    record.redo = [=] {
        applyVoxelSculpt(scenePointer, editorPointer, component, *redoRemove,
                         std::vector<uint32_t>(), false);
        applyVoxelSculpt(scenePointer, editorPointer, component, *redoAdd, *redoAddColors, true);
    };
    editor.history.record(std::move(record), ImGui::GetTime());

    if (component == editor.selectedComponent) {
        editor.selectedVoxelCount = projv::utils::getComponentVoxelCount(scene, component);
        editor.selectionOutlineValid = false;
    }
    editor.materialUsageValid = false;
    editor.materialChunkUsageValid = false;

    editor.statusMessage = std::string(grew ? "Extruded out " : "Extruded in ") +
                           std::to_string(std::abs(depth)) + " layer(s), " + std::to_string(count) +
                           " voxel(s) in " + scene.components[component].name;
}

// Opens a stroke. Nothing is placed here -- the ray has not been cast yet -- so this only resets the
// per-stroke accumulators. The target component and the anchor are settled by the first sample.
static void beginSculptStroke(EditorState& editor) {
    editor.sculptStrokeActive = true;
    editor.sculptStrokeComponent = projv::INVALID_COMPONENT_HANDLE;
    editor.sculptStrokeMode = editor.sculptMode;
    editor.sculptStrokeBrush = editor.sculptBrush;
    editor.sculptStrokeHasAnchor = false;
    editor.sculptStrokeTruncated = false;
    editor.sculptStrokeOriginal.clear();
    // Zero, not "now": the first tick of an iterative brush should land on the frame the button goes
    // down rather than one interval later, so a quick click still does something.
    editor.sculptStrokeLastIteration = 0.0;

    editor.extrudeFace.clear();
    editor.extrudeLayers.clear();
    editor.extrudeAppliedDepth = 0;
    editor.extrudeFaceTruncated = false;
}

// Closes a stroke and hands the whole thing to the history as one entry.
//
// One entry per stroke, not per dab: a drag is a single gesture and undoing it should put the scene
// back where the button went down. The coordinate list is shared with the closures by shared_ptr for
// the same reason the paint tool shares its own -- a long stroke is a large list, and the history is
// charged for it once rather than three times.
static void endSculptStroke(projv::Scene& scene, EditorState& editor) {
    if (!editor.sculptStrokeActive) return;
    editor.sculptStrokeActive = false;

    // Extrude keeps its own accounting -- its net effect is a depth, not a list of dabs.
    if (editor.sculptStrokeBrush == SculptBrush::Extrude) {
        endExtrudeDrag(scene, editor);
        return;
    }

    projv::ComponentHandle component = editor.sculptStrokeComponent;
    if (editor.sculptStrokeOriginal.empty() || component == projv::INVALID_COMPONENT_HANDLE) {
        editor.sculptStrokeOriginal.clear();
        return;
    }

    // The stroke's net effect, read out of the journal: what each remembered cell held when the stroke
    // began, against what it holds now. Cells the stroke changed and then changed back contribute
    // nothing, which is exactly right -- a Smooth pass that filled a dent and a later one that shaved
    // it flat again should leave no trace in the history.
    //
    // Four lists rather than two, because a single stroke can both add and remove: undo has to delete
    // what appeared *and* restore what vanished, and redo is the mirror.
    ComponentVoxelSpace space = resolveComponentVoxelSpace(scene, component);
    auto undoRemove = std::make_shared<std::vector<projv::core::ivec3>>();
    auto undoRestore = std::make_shared<std::vector<projv::core::ivec3>>();
    auto undoColors = std::make_shared<std::vector<uint32_t>>();
    auto redoAdd = std::make_shared<std::vector<projv::core::ivec3>>();
    auto redoColors = std::make_shared<std::vector<uint32_t>>();
    auto redoRemove = std::make_shared<std::vector<projv::core::ivec3>>();

    size_t added = 0, removed = 0;
    for (const auto& entry : editor.sculptStrokeOriginal) {
        const EditorState::StrokeVoxel& before = entry.second;
        uint8_t slot = 0;
        bool nowSolid = space.valid && queryComponentVoxel(scene, space, before.coord, slot);
        uint32_t nowColor = nowSolid ? componentVoxelColor(scene, component, slot) : 0u;

        if (nowSolid == before.wasSolid && (!nowSolid || nowColor == before.oldColor)) continue;

        if (nowSolid) {
            redoAdd->push_back(before.coord);
            redoColors->push_back(nowColor);
        } else {
            redoRemove->push_back(before.coord);
        }
        if (before.wasSolid) {
            undoRestore->push_back(before.coord);
            undoColors->push_back(before.oldColor);
        } else {
            undoRemove->push_back(before.coord);
        }
        if (nowSolid && !before.wasSolid) added++;
        if (!nowSolid && before.wasSolid) removed++;
    }
    editor.sculptStrokeOriginal.clear();

    size_t count = undoRemove->size() + undoRestore->size();
    if (count == 0) return;

    projv::Scene* scenePointer = &scene;
    EditorState* editorPointer = &editor;

    projv::editor::EditRecord record;
    record.label = std::string("Sculpt ") + SculptBrushLabel(editor.sculptStrokeBrush) + " in " +
                   scene.components[component].name;
    record.memoryCost = count * (sizeof(projv::core::ivec3) + sizeof(uint32_t)) +
                        redoAdd->size() * (sizeof(projv::core::ivec3) + sizeof(uint32_t));
    // Removals before additions in both directions, so a cell that changed hands is emptied before it
    // is refilled rather than the two writes racing over one coordinate.
    //
    // Undoing an addition does leave behind any Grid cell the stroke caused to be created -- an empty
    // chunk, drawing nothing and costing a header row, which the next save drops.
    record.undo = [=] {
        applyVoxelSculpt(scenePointer, editorPointer, component, *undoRemove,
                         std::vector<uint32_t>(), false);
        applyVoxelSculpt(scenePointer, editorPointer, component, *undoRestore, *undoColors, true);
    };
    record.redo = [=] {
        applyVoxelSculpt(scenePointer, editorPointer, component, *redoRemove,
                         std::vector<uint32_t>(), false);
        applyVoxelSculpt(scenePointer, editorPointer, component, *redoAdd, *redoColors, true);
    };
    editor.history.record(std::move(record), ImGui::GetTime());

    // The component may have grown new cells, and its voxel count certainly changed. Both are cached.
    if (component == editor.selectedComponent) {
        editor.selectedVoxelCount = projv::utils::getComponentVoxelCount(scene, component);
        editor.selectionOutlineValid = false;
    }
    editor.materialUsageValid = false;
    editor.materialChunkUsageValid = false;

    std::string summary;
    if (added > 0) summary += "+" + std::to_string(added);
    if (removed > 0) summary += (summary.empty() ? "" : " ") + std::string("-") + std::to_string(removed);
    if (summary.empty()) summary = std::to_string(count) + " recoloured";
    editor.statusMessage = std::string(SculptBrushLabel(editor.sculptStrokeBrush)) + ": " + summary +
                           " voxel(s) in " + scene.components[component].name +
                           (editor.sculptStrokeTruncated
                                ? " (stroke moved faster than the brush could follow)" : "");
}

// One frame of a stroke: cast, decide which cell the brush sits on, and stamp from the last cell to
// this one. Called from processVoxelPick, which owns the ray.
static void processSculptSample(projv::Scene& scene, EditorState& editor,
                                projv::core::vec3 rayDirection) {
    using projv::core::ivec3;
    using projv::core::vec3;

    if (!editor.sculptStrokeActive) return;
    bool extruding = editor.sculptStrokeBrush == SculptBrush::Extrude;

    // Once an extrude has its face, the cursor means a distance along that face's normal and nothing
    // else. Handled before the pick, so the ray is genuinely not cast rather than cast and ignored --
    // which is also why extrude never needed the solidity override the brush depends on.
    if (extruding && editor.sculptStrokeHasAnchor) {
        ComponentVoxelSpace anchored = resolveComponentVoxelSpace(scene, editor.sculptStrokeComponent);
        if (anchored.valid) updateExtrudeDrag(scene, editor, anchored, rayDirection);
        return;
    }

    // The stroke's own edits are hidden from its own ray; see makeSculptStrokeOverride. On the first
    // sample there is no stroke component yet, so this is empty and the ray sees the scene as it is.
    projv::utils::VoxelPick pick = projv::utils::pickVoxel(scene, editor.cameraPosition, rayDirection,
                                                          1.0e6f, makeSculptStrokeOverride(scene, editor));

    // --- Which component is being sculpted ---
    //
    // Settled once, on the first sample, and held for the rest of the stroke. What the ray hit is the
    // natural answer and matches the Paint tool; with nothing under the cursor there is no answer to
    // read off the scene, so the stroke goes to whatever is selected -- which is also the only way to
    // put the first voxel into a component that is still empty.
    if (editor.sculptStrokeComponent == projv::INVALID_COMPONENT_HANDLE) {
        // Extrude has no empty-space fallback and cannot have one: it moves a face, and a miss means
        // there is no face to move. The brush's fallback exists to let an empty component be filled;
        // extrude has nothing to work from until something is there.
        if (extruding && !pick.hit) {
            editor.statusMessage = "Point at a face to extrude it.";
            editor.sculptStrokeActive = false;
            return;
        }
        projv::ComponentHandle target = pick.hit ? pick.component : editor.selectedComponent;
        if (target == projv::INVALID_COMPONENT_HANDLE || target >= scene.components.size()) {
            editor.statusMessage = "Nothing under the cursor - select a component to sculpt into it.";
            editor.sculptStrokeActive = false;
            return;
        }
        if (!resolveComponentVoxelSpace(scene, target).valid) {
            editor.statusMessage = "Cannot sculpt " + scene.components[target].name +
                                   " (no voxel space to aim at).";
            editor.sculptStrokeActive = false;
            return;
        }
        // Only the shape brushes place the palette's colour. Extrude takes the colour of the face it
        // pulls, and Smooth and Bump take theirs from the surface they are reshaping, so none of the
        // three needs a palette selection and none should be blocked for want of one.
        bool needsPaletteColor = !extruding && !sculptBrushIsIterative(editor.sculptStrokeBrush) &&
                                 editor.sculptStrokeMode == SculptMode::Add;
        if (needsPaletteColor) {
            projv::ComponentHandle source = editor.paletteComponent;
            if (source >= scene.components.size() || editor.selectedMaterialSlot < 0 ||
                size_t(editor.selectedMaterialSlot) >= scene.components[source].materialPalette.size()) {
                editor.statusMessage = "No palette entry selected - pick one in the Palette panel first.";
                editor.sculptStrokeActive = false;
                return;
            }
            editor.sculptStrokeColor =
                scene.components[source].materialPalette[editor.selectedMaterialSlot].packedColor;
        }
        editor.sculptStrokeComponent = target;
    }

    projv::ComponentHandle component = editor.sculptStrokeComponent;
    ComponentVoxelSpace space = resolveComponentVoxelSpace(scene, component);
    if (!space.valid) return;
    bool addMode = editor.sculptStrokeMode == SculptMode::Add;

    // --- Where the brush sits this frame ---
    ivec3 centre;
    ivec3 faceAxis(0);
    if (pick.hit && pick.component == component) {
        // The common case, and the exact one: the pick is already a coordinate of this component's
        // grid, and its face normal is already along this component's axes.
        ivec3 hitCoord;
        if (!pickToComponentVoxelCoord(scene, pick, hitCoord)) return;
        faceAxis = pick.faceNormal;
        // Additive work goes in the empty cell outside the face; removal takes the cell that is there.
        centre = addMode ? hitCoord + faceAxis : hitCoord;
    } else if (pick.hit) {
        // The stroke wandered onto a different object. It stays locked to its own component, so the
        // hit is carried across through world space -- approximate where the two disagree on voxel
        // size or rotation, but well defined, and far better than a stroke that stalls at the seam.
        const projv::Chunk& hitChunk = scene.chunks[pick.chunk];
        vec3 worldNormal = glm::mat3_cast(hitChunk.header.rotation) * vec3(pick.faceNormal);
        faceAxis = worldDirectionToComponentAxis(space, worldNormal);
        centre = worldToComponentVoxel(space, pick.worldPosition) + (addMode ? faceAxis : ivec3(0));
    } else {
        // Nothing under the cursor: the dab goes a fixed distance down the ray. This is what makes an
        // empty component sculptable at all, and it is also how a stroke keeps flowing when the drag
        // runs off the edge of the object it started on.
        vec3 worldPoint = editor.cameraPosition + glm::normalize(rayDirection) * editor.sculptPlaceDistance;
        centre = worldToComponentVoxel(space, worldPoint);
        faceAxis = ivec3(0);
    }

    // --- Extrude: the press picks the face, and that is all this frame does ---
    //
    // `centre` above already stepped off the surface for an additive brush, which is not what extrude
    // wants: it moves the face itself, so it needs the solid voxel that was hit.
    if (extruding) {
        ivec3 faceCoord = addMode ? centre - faceAxis : centre;
        if (!beginExtrudeDrag(scene, editor, space, pick, faceCoord, faceAxis, rayDirection)) {
            editor.sculptStrokeActive = false;
            return;
        }
        editor.sculptStrokeHasAnchor = true;
        return;
    }

    // --- Smooth and Bump: run a pass on the clock, wherever the cursor is now ---
    //
    // Centred on the surface cell itself rather than the empty one beside it: these reshape what is
    // there, so the sphere has to be over the geometry, not floating off its face.
    if (sculptBrushIsIterative(editor.sculptStrokeBrush)) {
        ivec3 surfaceCentre = addMode && pick.hit ? centre - faceAxis : centre;
        editor.sculptStrokeLastCenter = surfaceCentre;
        editor.sculptStrokeHasAnchor = true;

        double now = ImGui::GetTime();
        if (now - editor.sculptStrokeLastIteration < SCULPT_ITERATION_SECONDS) return;
        editor.sculptStrokeLastIteration = now;

        applySculptIteration(scene, editor, surfaceCentre);
        return;
    }

    std::vector<ivec3> frameCoords;
    std::vector<uint32_t> framePreviousColors;

    if (!editor.sculptStrokeHasAnchor) {
        stampSculptDab(scene, editor, space, centre, frameCoords, framePreviousColors);
    } else if (centre != editor.sculptStrokeLastCenter) {
        // Fill the gap the cursor crossed since the last frame, so the stroke is a ridge rather than
        // a row of dots. Starts at step 1: step 0 is the previous frame's dab, already placed.
        vec3 from(editor.sculptStrokeLastCenter);
        vec3 to(centre);
        float span = glm::length(to - from);
        int steps = std::max(1, int(std::ceil(span / sculptDabSpacing(editor))));
        if (steps > SCULPT_MAX_INTERPOLATED_STEPS) {
            steps = SCULPT_MAX_INTERPOLATED_STEPS;
            editor.sculptStrokeTruncated = true;
        }
        for (int step = 1; step <= steps; step++) {
            vec3 point = from + (to - from) * (float(step) / float(steps));
            ivec3 dabCentre(int(std::lround(point.x)), int(std::lround(point.y)),
                            int(std::lround(point.z)));
            stampSculptDab(scene, editor, space, dabCentre, frameCoords, framePreviousColors);
        }
    }

    editor.sculptStrokeLastCenter = centre;
    editor.sculptStrokeHasAnchor = true;

    // One queue and one rebuild for everything this frame produced, however many dabs that was.
    if (!frameCoords.empty()) {
        applyVoxelSculpt(&scene, &editor, component, frameCoords, framePreviousColors, addMode);
    }
}

// Casts the ray the user clicked on into the scene and does whatever the click was for. Runs after
// the Viewport panel has been laid out (that is where the click is noticed) but before the panels
// that show the result, so a selection or a sample appears on the same frame it was made.
static void processVoxelPick(projv::Scene& scene, EditorState& editor) {
    PickPurpose purpose = editor.pendingPick;
    if (purpose == PickPurpose::None) return;
    editor.pendingPick = PickPurpose::None;

    projv::core::vec3 cameraDirection = computeCameraDirection(editor);
    projv::core::vec2 viewportResolution = { float(editor.viewportWidth), float(editor.viewportHeight) };
    projv::core::vec3 rayDirection =
        projv::utils::rayDirectionThroughImage(editor.pickUV, viewportResolution, cameraDirection);

    // Sculpting casts its own ray: it needs the stroke's solidity override, and a miss means
    // something to it (place down the ray) rather than nothing.
    if (purpose == PickPurpose::SculptVoxel) {
        processSculptSample(scene, editor, rayDirection);
        return;
    }

    projv::utils::VoxelPick pick = projv::utils::pickVoxel(scene, editor.cameraPosition, rayDirection);
    if (!pick.hit) {
        // A paint drag is expected to run off the geometry -- a sweep along a wall leaves its edge
        // every time. The anchor is dropped so the dab that lands when the cursor comes back does not
        // draw a line across the gap, and only the press itself has anything to say: one message per
        // frame of a drag would be noise.
        if (purpose == PickPurpose::PaintVoxel && editor.paintStrokeActive) {
            bool firstSample = !editor.paintStrokeSampled;
            editor.paintStrokeSampled = true;
            editor.paintStrokeHasAnchor = false;
            if (firstSample) editor.statusMessage = "Nothing under the cursor.";
            return;
        }
        // A miss deselects only for the Select tool, which is the one whose whole job is choosing:
        // clicking past everything is how you say "nothing". Under the other tools a near miss is a
        // slip of the hand, and losing the selection (and with it the gizmo, or the brush's target)
        // would cost far more than it saves.
        if (purpose == PickPurpose::SelectComponent && editor.activeTool == EditorTool::Select &&
            editor.selectedComponent != projv::INVALID_COMPONENT_HANDLE) {
            editor.selectedComponent = projv::INVALID_COMPONENT_HANDLE;
            editor.selectedVoxelCount = 0;
            editor.selectionOutlineValid = false;
        }
        editor.statusMessage = "Nothing under the cursor.";
        editor.materialPickerActive = false;
        return;
    }

    switch (purpose) {
        case PickPurpose::SelectComponent: {
            editor.selectedComponent = pick.component;
            editor.selectedVoxelCount = projv::utils::getComponentVoxelCount(scene, pick.component);
            editor.selectionOutlineValid = false;
            // The hierarchy is the panel that is meant to show what is selected, and the component
            // just picked may be inside a collapsed folder well off the top of it.
            editor.revealSelectionInHierarchy = true;
            editor.statusMessage = "Selected " + projv::utils::getComponentPath(scene, pick.component);
            break;
        }

        case PickPurpose::SampleMaterial: {
            adoptPickedMaterial(scene, editor, pick.component, pick.materialSlot);
            editor.materialPickerActive = false;   // One-shot, like every other eyedropper.

            const projv::ComponentRecord& picked = scene.components[pick.component];
            std::string name = pick.materialSlot < picked.materialPalette.size()
                             ? picked.materialPalette[pick.materialSlot].name : std::string();
            editor.statusMessage = "Picked slot " + std::to_string(pick.materialSlot) +
                                   (name.empty() ? "" : " (" + name + ")") +
                                   " from voxel (" + std::to_string(pick.voxelCoord.x) + ", " +
                                   std::to_string(pick.voxelCoord.y) + ", " +
                                   std::to_string(pick.voxelCoord.z) + ") in chunk " +
                                   std::to_string(pick.chunk);
            break;
        }

        case PickPurpose::PaintVoxel:
            // The press and every frame of the drag after it come through here; the stroke owns what
            // a sample means, and the history entry is written when the button comes up.
            processPaintSample(scene, editor, pick);
            break;

        case PickPurpose::SculptVoxel:   // Handled above, before the shared ray cast.
        case PickPurpose::None:
            break;
    }
}

// The edit log, listed oldest first, with everything after the cursor greyed out — those are the
// edits that have been undone and are waiting to be redone. Clicking any entry walks the scene to
// the state just after it.
static void drawHistoryPanel(EditorState& editor) {
    ImGui::Begin("History");

    const std::vector<projv::editor::EditRecord>& entries = editor.history.entries();

    ImGui::BeginDisabled(!editor.history.canUndo());
    if (ImGui::Button("Undo")) editor.history.undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!editor.history.canRedo());
    if (ImGui::Button("Redo")) editor.history.redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu edit(s), %.1f MB", entries.size(), double(editor.history.memoryUsed()) / (1024.0 * 1024.0));

    ImGui::Separator();

    if (entries.empty()) {
        ImGui::TextDisabled("No edits yet.");
        ImGui::End();
        return;
    }

    ImGui::BeginChild("##historyEntries", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    // The base state is an entry of its own: clicking it undoes everything, which is the one target
    // the list of edits cannot otherwise express.
    if (ImGui::Selectable("(scene as loaded)", editor.history.position() < 0)) {
        editor.history.jumpTo(-1);
    }
    for (size_t index = 0; index < entries.size(); index++) {
        bool isApplied = int(index) <= editor.history.position();
        bool isCurrent = int(index) == editor.history.position();

        if (!isApplied) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        std::string label = std::to_string(index + 1) + ". " + entries[index].label;
        if (ImGui::Selectable(label.c_str(), isCurrent)) {
            editor.history.jumpTo(int(index));
        }
        if (!isApplied) ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::End();
}

// One line along the bottom of the window: frame time, what is loaded, which tool is live, and the
// last thing the editor had to say.
//
// It replaces the docked Statistics panel, which spent a fifth of the bottom row on five read-only
// numbers -- and, worse, was where statusMessage was displayed, so the answer to a click in the
// viewport appeared in a tabbed panel at the far corner of the screen. The numbers that do not fit
// here are a floating window away (View ▸ Statistics), and the messages are also floated over the
// viewport itself where the click happened (drawViewportToast).
static void drawStatusBar(const projv::Scene& scene, const EditorState& editor,
                          const ImGuiViewport* mainViewport) {
    // BeginViewportSideBar reserves the strip out of the viewport's work area, which is what the
    // dock host is sized from -- so the dockspace gives up exactly this much on the next frame.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##StatusBar", const_cast<ImGuiViewport*>(mainViewport),
                                    ImGuiDir_Down, ImGui::GetFrameHeight(), flags)) {
        if (ImGui::BeginMenuBar()) {
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::Text("%.1f ms  (%.0f fps)", 1000.0f / io.Framerate, io.Framerate);
            ImGui::TextDisabled("|");
            ImGui::Text("%d x %d", editor.viewportWidth, editor.viewportHeight);
            ImGui::TextDisabled("|");
            if (editor.sceneLoaded) {
                ImGui::Text("%zu chunks, %zu components", scene.chunks.size(), scene.components.size());
            } else {
                ImGui::TextDisabled("no scene");
            }
            ImGui::TextDisabled("|");
            ImGui::Text("%s", editorToolLabel(editor.activeTool));

            // The message is last and takes whatever room is left, because it is the only part whose
            // length is not known in advance -- the counters ahead of it must not be pushed off the
            // bar by a long path in a load message.
            if (!editor.statusMessage.empty()) {
                ImGui::TextDisabled("|");
                ImGui::TextDisabled("%s", editor.statusMessage.c_str());
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

// The numbers the status bar has no room for. A floating window, opened from View ▸ Statistics and
// closed with its own title-bar cross, rather than a permanent dock panel: these are looked at while
// something is being diagnosed and ignored the rest of the time.
static void drawStatisticsWindow(const projv::Scene& scene, EditorState& editor, int frameCount) {
    if (!editor.statisticsWindowOpen) return;

    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Statistics", &editor.statisticsWindowOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Frame        %.2f ms  (%.0f fps)", 1000.0f / io.Framerate, io.Framerate);
    ImGui::Text("Viewport     %d x %d", editor.viewportWidth, editor.viewportHeight);

    if (editor.sceneLoaded) {
        ImGui::TextWrapped("Scene        %s", editor.scenePath.c_str());
        ImGui::Text("             %zu chunk(s), %zu component(s)", scene.chunks.size(), scene.components.size());
        ImGui::Text("Camera       %.1f, %.1f, %.1f  (speed %.3f/frame)",
                    editor.cameraPosition.x, editor.cameraPosition.y, editor.cameraPosition.z,
                    editor.framing.moveSpeed * std::pow(1.2f, editor.speedScrollSteps));
        // The albedo pass is jittered per frame and the accumulate pass averages the results, so
        // this is how supersampled the image currently is. It resets to 0 whenever the camera moves.
        ImGui::Text("Accumulated  %d frame(s)", std::min(frameCount - editor.frameCameraLastMovedOn, 64));
    } else {
        ImGui::TextDisabled("No scene loaded.");
    }

    ImGui::Separator();
    ImGui::Text("Tool         %s  (%s)", editorToolLabel(editor.activeTool),
                editorToolShortcut(editor.activeTool));
    ImGui::Text("History      %zu edit(s), %.1f MB", editor.history.entries().size(),
                double(editor.history.memoryUsed()) / (1024.0 * 1024.0));

    if (!editor.statusMessage.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", editor.statusMessage.c_str());
    }

    ImGui::End();
}

// =============================================================================
// Transform gizmo
// =============================================================================
//
// Three arrows and three rings, drawn at the selection's pivot -- the same point the Inspector's
// rotation and scale drags turn about, so the two agree about where "the middle" is.
//
// The axes are the *world* axes, always, and never follow the model. That is a deliberate choice
// with one consequence worth stating plainly, because it is not obvious: glm composes
// quat(vec3 euler) as qZ * qY * qX, so only the Z field rotates about a fixed axis -- Y is tilted by
// Z, and X by both. A ring locked to world Z therefore cannot be "the X field plus an angle". So a
// ring drag applies a true axis-angle delta to the rotation and lets all three Euler numbers fall
// out of the result, rather than driving one field directly. The orientation always matches the ring
// the user grabbed, which is the property a gizmo has to have; the numbers in the Inspector are a
// readout of it. (This is exactly Blender's Global orientation mode, and its Gimbal mode is the
// other side of the same trade.)
namespace gizmo {
    constexpr float ARROW_LENGTH_PIXELS = 72.0f;   // Arrows sit inside the rings so the two do not fight.
    constexpr float RING_RADIUS_PIXELS  = 96.0f;
    constexpr float GRAB_RADIUS_PIXELS  = 9.0f;
    constexpr int   RING_SEGMENTS       = 64;
    constexpr int   HANDLE_COUNT        = 6;

    // A ring seen close to edge-on projects to a sliver, and the ray-plane intersection its drag
    // depends on blows up as the plane turns parallel to the view. Below this |cos| between the view
    // ray and the ring's axis, the ring is drawn dimmed and refuses to start a drag.
    constexpr float MIN_RING_FACING = 0.08f;
}

// The world-space rotation and uniform scale of a component's *parent* -- the frame localPosition and
// localRotation are expressed in. Identity for a root component, which is why a gizmo on a root reads
// as world-aligned in the obvious way and one on a child of a rotated asset still writes the right
// numbers.
struct ParentFrame {
    projv::core::quat rotation = projv::core::quat(1.0f, 0.0f, 0.0f, 0.0f);
    float scale = 1.0f;
};

static ParentFrame getParentFrame(const projv::Scene& scene, projv::ComponentHandle handle) {
    ParentFrame frame;
    if (handle >= scene.components.size()) return frame;
    projv::ComponentHandle parent = scene.components[handle].parent;
    if (parent == projv::INVALID_COMPONENT_HANDLE) return frame;

    projv::core::mat4 world = projv::utils::getComponentWorldMatrix(scene, parent);
    float scale = glm::length(projv::core::vec3(world[0]));
    if (scale < 1e-8f) scale = 1e-8f;
    projv::core::mat3 basis(projv::core::vec3(world[0]) / scale,
                            projv::core::vec3(world[1]) / scale,
                            projv::core::vec3(world[2]) / scale);
    frame.rotation = glm::quat_cast(basis);
    frame.scale = scale;
    return frame;
}

// World-space size of one screen pixel at `point`'s depth, so the gizmo can be drawn at a constant
// on-screen size however far away the component is. Mirrors worldToViewportPixel's camera model.
static float worldUnitsPerPixel(const EditorState& editor, projv::core::vec3 point,
                                projv::core::vec3 cameraDirection, ImVec2 imageMin, ImVec2 imageMax,
                                float verticalFovDegrees = 60.0f) {
    float imageHeight = std::max(1.0f, imageMax.y - imageMin.y);
    float depth = glm::dot(point - editor.cameraPosition, glm::normalize(cameraDirection));
    depth = std::max(depth, 1.0e-3f);
    return (2.0f * std::tan(glm::radians(verticalFovDegrees * 0.5f)) * depth) / imageHeight;
}

// Closest point along an infinite line to a ray, as the line's parameter t. False when the two are
// close to parallel, where t is unbounded and dragging would fling the component off.
static bool closestPointOnAxis(projv::core::vec3 axisOrigin, projv::core::vec3 axisDirection,
                               projv::core::vec3 rayOrigin, projv::core::vec3 rayDirection, float& outT) {
    using namespace projv::core;
    vec3 u = glm::normalize(axisDirection);
    vec3 d = glm::normalize(rayDirection);
    vec3 w = axisOrigin - rayOrigin;
    float b = glm::dot(u, d);
    float denominator = 1.0f - b * b;
    if (std::abs(denominator) < 1.0e-5f) return false;
    // From minimising |w + t*u - s*d|^2 over both parameters and eliminating s.
    outT = (b * glm::dot(w, d) - glm::dot(w, u)) / denominator;
    return true;
}

// Where a ray meets the plane through `planeOrigin` with normal `planeNormal`. False when the ray
// runs along the plane, or the hit is behind the camera.
static bool intersectRayPlane(projv::core::vec3 planeOrigin, projv::core::vec3 planeNormal,
                              projv::core::vec3 rayOrigin, projv::core::vec3 rayDirection,
                              projv::core::vec3& outPoint) {
    float denominator = glm::dot(rayDirection, planeNormal);
    if (std::abs(denominator) < 1.0e-6f) return false;
    float t = glm::dot(planeOrigin - rayOrigin, planeNormal) / denominator;
    if (t <= 0.0f) return false;
    outPoint = rayOrigin + rayDirection * t;
    return true;
}

static float distancePointToSegment(ImVec2 point, ImVec2 a, ImVec2 b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float lengthSquared = dx * dx + dy * dy;
    if (lengthSquared < 1.0e-6f) {
        float px = point.x - a.x, py = point.y - a.y;
        return std::sqrt(px * px + py * py);
    }
    float t = std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSquared, 0.0f, 1.0f);
    float cx = a.x + t * dx - point.x, cy = a.y + t * dy - point.y;
    return std::sqrt(cx * cx + cy * cy);
}

// Everything about the gizmo's on-screen shape for this frame, built once and then used for both
// drawing and hit testing so the two can never disagree about where a handle is.
struct GizmoLayout {
    bool   valid = false;
    ImVec2 centerScreen = ImVec2(0.0f, 0.0f);
    projv::core::vec3 axes[3];                  // World X/Y/Z, unit length.
    bool   arrowVisible[3] = {false, false, false};
    ImVec2 arrowTipScreen[3];
    bool   ringFacingOK[3] = {false, false, false};
    std::vector<ImVec2> ringScreen[3];          // Projected polyline, empty where off-screen.
    float  worldRadius = 1.0f;
    float  arrowWorldLength = 1.0f;
};

static GizmoLayout buildGizmoLayout(const EditorState& editor, projv::core::vec3 anchor,
                                    projv::core::vec3 cameraDirection) {
    using namespace projv::core;
    GizmoLayout layout;
    ImVec2 imageMin = editor.viewportImageMin;
    ImVec2 imageMax = editor.viewportImageMax;

    if (!worldToViewportPixel(anchor, editor.cameraPosition, cameraDirection, imageMin, imageMax,
                              layout.centerScreen)) {
        return layout;   // Pivot is behind the camera; there is no sane place to draw.
    }

    float unitsPerPixel = worldUnitsPerPixel(editor, anchor, cameraDirection, imageMin, imageMax);
    layout.worldRadius = gizmo::RING_RADIUS_PIXELS * unitsPerPixel;
    layout.arrowWorldLength = gizmo::ARROW_LENGTH_PIXELS * unitsPerPixel;

    layout.axes[0] = vec3(1.0f, 0.0f, 0.0f);
    layout.axes[1] = vec3(0.0f, 1.0f, 0.0f);
    layout.axes[2] = vec3(0.0f, 0.0f, 1.0f);

    vec3 viewDirection = glm::normalize(anchor - editor.cameraPosition);
    for (int axis = 0; axis < 3; axis++) {
        layout.arrowVisible[axis] = worldToViewportPixel(anchor + layout.axes[axis] * layout.arrowWorldLength,
                                                         editor.cameraPosition, cameraDirection,
                                                         imageMin, imageMax, layout.arrowTipScreen[axis]);

        layout.ringFacingOK[axis] = std::abs(glm::dot(viewDirection, layout.axes[axis])) >= gizmo::MIN_RING_FACING;

        // Two axes perpendicular to this one span the ring's plane.
        vec3 spokeA = layout.axes[(axis + 1) % 3];
        vec3 spokeB = layout.axes[(axis + 2) % 3];
        layout.ringScreen[axis].clear();
        layout.ringScreen[axis].reserve(gizmo::RING_SEGMENTS + 1);
        for (int i = 0; i <= gizmo::RING_SEGMENTS; i++) {
            float angle = (float(i) / float(gizmo::RING_SEGMENTS)) * 2.0f * 3.14159265f;
            vec3 point = anchor + (spokeA * std::cos(angle) + spokeB * std::sin(angle)) * layout.worldRadius;
            ImVec2 screen;
            if (worldToViewportPixel(point, editor.cameraPosition, cameraDirection, imageMin, imageMax, screen)) {
                layout.ringScreen[axis].push_back(screen);
            } else if (!layout.ringScreen[axis].empty()) {
                break;   // Ring crosses behind the camera; keep the front arc and stop.
            }
        }
    }

    layout.valid = true;
    return layout;
}

// Which handle the cursor is over, or -1. Arrows win ties against rings: they are drawn inside the
// rings and are the smaller target, so preferring them makes the crowded middle usable.
static int hitTestGizmo(const GizmoLayout& layout, ImVec2 mouse) {
    int best = -1;
    float bestDistance = gizmo::GRAB_RADIUS_PIXELS;

    for (int axis = 0; axis < 3; axis++) {
        if (!layout.arrowVisible[axis]) continue;
        float distance = distancePointToSegment(mouse, layout.centerScreen, layout.arrowTipScreen[axis]);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = axis;
        }
    }
    if (best >= 0) return best;

    for (int axis = 0; axis < 3; axis++) {
        if (!layout.ringFacingOK[axis]) continue;
        const std::vector<ImVec2>& ring = layout.ringScreen[axis];
        for (size_t i = 1; i < ring.size(); i++) {
            float distance = distancePointToSegment(mouse, ring[i - 1], ring[i]);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = 3 + axis;
            }
        }
    }
    return best;
}

static void drawGizmo(ImDrawList* drawList, const GizmoLayout& layout, int hovered, int active) {
    const ImU32 axisColors[3] = {
        IM_COL32(230,  70,  70, 255),   // X
        IM_COL32( 90, 210,  90, 255),   // Y
        IM_COL32( 90, 140, 240, 255),   // Z
    };
    const ImU32 highlightColor = IM_COL32(255, 225, 80, 255);
    int emphasised = active >= 0 ? active : hovered;

    for (int axis = 0; axis < 3; axis++) {
        if (!layout.ringFacingOK[axis]) continue;
        const std::vector<ImVec2>& ring = layout.ringScreen[axis];
        if (ring.size() < 2) continue;
        bool lit = emphasised == 3 + axis;
        ImU32 color = lit ? highlightColor : axisColors[axis];
        // Dimmed while another handle is being dragged, so the live one reads clearly.
        if (active >= 0 && !lit) color = (color & 0x00FFFFFFu) | 0x50000000u;
        drawList->AddPolyline(ring.data(), int(ring.size()), color, ImDrawFlags_None, lit ? 3.5f : 2.0f);
    }

    for (int axis = 0; axis < 3; axis++) {
        if (!layout.arrowVisible[axis]) continue;
        bool lit = emphasised == axis;
        ImU32 color = lit ? highlightColor : axisColors[axis];
        if (active >= 0 && !lit) color = (color & 0x00FFFFFFu) | 0x50000000u;

        ImVec2 tip = layout.arrowTipScreen[axis];
        ImVec2 center = layout.centerScreen;
        drawList->AddLine(center, tip, color, lit ? 3.5f : 2.0f);

        // A screen-space arrowhead: the shaft direction rotated 90 degrees gives the base.
        float dx = tip.x - center.x, dy = tip.y - center.y;
        float length = std::sqrt(dx * dx + dy * dy);
        if (length > 1.0e-3f) {
            dx /= length; dy /= length;
            const float head = lit ? 13.0f : 11.0f;
            const float halfWidth = head * 0.45f;
            ImVec2 base(tip.x - dx * head, tip.y - dy * head);
            drawList->AddTriangleFilled(tip,
                                        ImVec2(base.x - dy * halfWidth, base.y + dx * halfWidth),
                                        ImVec2(base.x + dy * halfWidth, base.y - dx * halfWidth),
                                        color);
        }
    }

    drawList->AddCircleFilled(layout.centerScreen, 4.0f, IM_COL32(240, 240, 240, 230));
}

// Runs the gizmo for one frame: places it, hit tests it, advances any drag, and draws it. Returns
// true while a drag is live, so the viewport can keep the click away from anything else.
static bool updateAndDrawTransformGizmo(projv::Scene& scene, EditorState& editor, bool imageHovered) {
    using namespace projv::core;

    if (!editor.gizmoEnabled || editor.activeTool != EditorTool::Move || editor.materialPickerActive ||
        editor.selectedComponent == projv::INVALID_COMPONENT_HANDLE ||
        editor.selectedComponent >= scene.components.size()) {
        editor.gizmoActiveHandle = -1;
        editor.gizmoHoveredHandle = -1;
        return false;
    }
    // Flying takes the whole viewport: the cursor is captured and relative, so screen-space hit
    // testing is meaningless until it is released.
    if (editor.cameraIsFlying) {
        editor.gizmoActiveHandle = -1;
        editor.gizmoHoveredHandle = -1;
        return false;
    }

    projv::ComponentHandle handle = editor.selectedComponent;
    projv::ComponentRecord& component = scene.components[handle];

    // The gizmo sits on whatever point the transform edits actually turn about, so the two agree.
    vec3 pivotLocal = (editor.pivotAtCenter && editor.inspectorPivotValid)
                    ? editor.inspectorPivotLocal : vec3(0.0f);
    mat4 world = projv::utils::getComponentWorldMatrix(scene, handle);
    vec3 anchor = vec3(world * vec4(pivotLocal, 1.0f));
    editor.gizmoAnchorWorld = anchor;

    vec3 cameraDirection = computeCameraDirection(editor);
    GizmoLayout layout = buildGizmoLayout(editor, anchor, cameraDirection);
    if (!layout.valid) {
        editor.gizmoActiveHandle = -1;
        editor.gizmoHoveredHandle = -1;
        return false;
    }

    ImVec2 mouse = ImGui::GetIO().MousePos;
    // Hover is frozen for the duration of a drag: the cursor routinely leaves both the handle and the
    // image while dragging, and re-testing would drop the highlight off the handle being used.
    if (editor.gizmoActiveHandle < 0) {
        editor.gizmoHoveredHandle = imageHovered ? hitTestGizmo(layout, mouse) : -1;
    }

    // The mouse ray, built exactly the way the shader built the ray for that pixel.
    float imageWidth = std::max(1.0f, editor.viewportImageMax.x - editor.viewportImageMin.x);
    float imageHeight = std::max(1.0f, editor.viewportImageMax.y - editor.viewportImageMin.y);
    vec2 uv((mouse.x - editor.viewportImageMin.x) / imageWidth,
            (mouse.y - editor.viewportImageMin.y) / imageHeight);
    vec3 rayDirection = projv::utils::rayDirectionThroughImage(uv, vec2(imageWidth, imageHeight),
                                                               cameraDirection);
    vec3 rayOrigin = editor.cameraPosition;

    ParentFrame parentFrame = getParentFrame(scene, handle);

    // --- Begin a drag ---
    if (editor.gizmoActiveHandle < 0 && imageHovered && editor.gizmoHoveredHandle >= 0 &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int candidate = editor.gizmoHoveredHandle;
        bool started = false;

        if (candidate < 3) {
            float t = 0.0f;
            if (closestPointOnAxis(anchor, layout.axes[candidate], rayOrigin, rayDirection, t)) {
                editor.gizmoDragStartAxisT = t;
                started = true;
            }
        } else {
            int axis = candidate - 3;
            vec3 hit;
            if (layout.ringFacingOK[axis] &&
                intersectRayPlane(anchor, layout.axes[axis], rayOrigin, rayDirection, hit)) {
                vec3 spoke = hit - anchor;
                if (glm::length(spoke) > 1.0e-6f) {
                    editor.gizmoDragStartSpoke = glm::normalize(spoke);
                    started = true;
                }
            }
        }

        if (started) {
            editor.gizmoActiveHandle = candidate;
            editor.gizmoDragStartPosition = component.localPosition;
            editor.gizmoDragStartRotation = component.localRotation;
            editor.gizmoDragStartScale = component.localScale;
        }
    }

    // --- Advance a live drag ---
    if (editor.gizmoActiveHandle >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            editor.gizmoActiveHandle = -1;
        } else {
            int active = editor.gizmoActiveHandle;
            projv::Scene* scenePointer = &scene;
            EditorState* editorPointer = &editor;
            vec3 previousPosition = editor.gizmoDragStartPosition;
            quat previousRotation = editor.gizmoDragStartRotation;
            float dragScale = editor.gizmoDragStartScale;

            if (active < 3) {
                float t = 0.0f;
                if (closestPointOnAxis(anchor, layout.axes[active], rayOrigin, rayDirection, t)) {
                    // World-space slide along the axis, expressed in the parent's frame because that
                    // is what localPosition is measured in.
                    vec3 worldDelta = layout.axes[active] * (t - editor.gizmoDragStartAxisT);
                    vec3 localDelta = (glm::inverse(parentFrame.rotation) * worldDelta) / parentFrame.scale;
                    vec3 newPosition = editor.gizmoDragStartPosition + localDelta;

                    applyComponentTransform(&scene, &editor, handle, newPosition,
                                            component.localRotation, component.localScale);
                    projv::editor::EditRecord record;
                    record.label = "Move " + component.name;
                    record.coalesceKey = "transform:pos:" + std::to_string(handle);
                    record.undo = [=] { applyComponentTransform(scenePointer, editorPointer, handle, previousPosition,
                                                                 scenePointer->components[handle].localRotation,
                                                                 scenePointer->components[handle].localScale); };
                    record.redo = [=] { applyComponentTransform(scenePointer, editorPointer, handle, newPosition,
                                                                 scenePointer->components[handle].localRotation,
                                                                 scenePointer->components[handle].localScale); };
                    editor.history.record(std::move(record), ImGui::GetTime());
                }
            } else {
                int axis = active - 3;
                vec3 hit;
                if (intersectRayPlane(anchor, layout.axes[axis], rayOrigin, rayDirection, hit)) {
                    vec3 spoke = hit - anchor;
                    if (glm::length(spoke) > 1.0e-6f) {
                        spoke = glm::normalize(spoke);
                        // Signed angle from the spoke the drag grabbed to the current one, measured
                        // about the ring's axis -- the cross product's component along the axis gives
                        // the sine and settles the direction, which a bare acos cannot.
                        vec3 start = editor.gizmoDragStartSpoke;
                        float sine = glm::dot(glm::cross(start, spoke), layout.axes[axis]);
                        float cosine = glm::dot(start, spoke);
                        float angle = std::atan2(sine, cosine);

                        // A world-space delta, converted into the parent frame the stored rotation
                        // lives in: worldRotation_new = delta * worldRotation_old, and
                        // worldRotation = parentRotation * localRotation, so
                        //   local_new = inverse(parent) * delta * parent * local_old.
                        quat delta = glm::angleAxis(angle, layout.axes[axis]);
                        quat newRotation = glm::inverse(parentFrame.rotation) * delta *
                                           parentFrame.rotation * editor.gizmoDragStartRotation;
                        newRotation = glm::normalize(newRotation);

                        // Turn about the gizmo's own anchor, which is what the user is pointing at.
                        vec3 newPosition = (editor.pivotAtCenter && editor.inspectorPivotValid)
                            ? pivotCompensatedPosition(editor.gizmoDragStartPosition, editor.gizmoDragStartRotation,
                                                       dragScale, pivotLocal, newRotation, dragScale)
                            : editor.gizmoDragStartPosition;

                        applyComponentTransform(&scene, &editor, handle, newPosition, newRotation, dragScale);

                        // The Inspector's Euler buffer is a readout of the quaternion, not the source
                        // of this edit -- push the result into it so the fields track the drag.
                        vec3 radians = glm::eulerAngles(newRotation);
                        editor.inspectorEulerDegrees[0] = glm::degrees(radians.x);
                        editor.inspectorEulerDegrees[1] = glm::degrees(radians.y);
                        editor.inspectorEulerDegrees[2] = glm::degrees(radians.z);

                        projv::editor::EditRecord record;
                        record.label = "Rotate " + component.name;
                        record.coalesceKey = "transform:rot:" + std::to_string(handle);
                        record.undo = [=] { applyComponentTransform(scenePointer, editorPointer, handle,
                                                                     previousPosition, previousRotation, dragScale); };
                        record.redo = [=] { applyComponentTransform(scenePointer, editorPointer, handle,
                                                                     newPosition, newRotation, dragScale); };
                        editor.history.record(std::move(record), ImGui::GetTime());
                    }
                }
            }
        }
    }

    drawGizmo(ImGui::GetWindowDrawList(), layout, editor.gizmoHoveredHandle, editor.gizmoActiveHandle);
    return editor.gizmoActiveHandle >= 0;
}

// =============================================================================
// Viewport overlay bars
// =============================================================================
//
// The two strips of icon buttons that float over the scene image, built from the chrome and icons
// defined further up. Both follow the same contract: a rectangle function the Viewport panel can ask
// for *before* the strip is drawn (so a click on an icon is not also a click on the scene behind
// it), and a draw function submitted last so the buttons sit on top of the outline and the gizmo.

// Where the tool strip sits: down the left edge of the scene image, vertically centred. Same
// contract as viewportSettingsBarRect below -- false when the panel is too small to hold it, and the
// rectangle is needed *before* the strip is drawn so a click on an icon is not also a click on the
// scene behind it.
static bool viewportToolbarRect(const EditorState& editor, ImVec2& barMin, ImVec2& barMax) {
    float barWidth = SETTINGS_BAR_ICON_SIZE + 2.0f * SETTINGS_BAR_PADDING;
    float barHeight = EDITOR_TOOL_COUNT * SETTINGS_BAR_ICON_SIZE +
                      (EDITOR_TOOL_COUNT - 1) * SETTINGS_BAR_SPACING + 2.0f * SETTINGS_BAR_PADDING;

    if (editor.viewportImageMax.x - editor.viewportImageMin.x < barWidth + 2.0f * SETTINGS_BAR_MARGIN ||
        editor.viewportImageMax.y - editor.viewportImageMin.y < barHeight + 2.0f * SETTINGS_BAR_MARGIN) {
        return false;
    }

    barMin = ImVec2(editor.viewportImageMin.x + SETTINGS_BAR_MARGIN,
                    (editor.viewportImageMin.y + editor.viewportImageMax.y - barHeight) * 0.5f);
    barMax = ImVec2(barMin.x + barWidth, barMin.y + barHeight);
    return true;
}

// The tool strip. Exactly one button is lit, always -- there is no "no tool", because there is no
// state in which a click in the viewport should mean nothing.
static void drawViewportToolbar(EditorState& editor) {
    ImVec2 barMin;
    ImVec2 barMax;
    if (!viewportToolbarRect(editor, barMin, barMax)) {
        return;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(barMin, barMax, IM_COL32(18, 20, 26, 205), 8.0f);
    drawList->AddRect(barMin, barMax, IM_COL32(255, 255, 255, 28), 8.0f, 0, 1.0f);

    ImGui::PushID("ViewportTools");
    for (int i = 0; i < EDITOR_TOOL_COUNT; i++) {
        EditorTool tool = static_cast<EditorTool>(i);
        // Each button places itself explicitly rather than riding the layout cursor, which an
        // InvisibleButton would otherwise advance horizontally.
        ImGui::SetCursorScreenPos(ImVec2(barMin.x + SETTINGS_BAR_PADDING,
                                         barMin.y + SETTINGS_BAR_PADDING +
                                             float(i) * (SETTINGS_BAR_ICON_SIZE + SETTINGS_BAR_SPACING)));
        bool hovered = false;
        char id[8];
        std::snprintf(id, sizeof(id), "tool%d", i);
        if (drawViewportIconButton(id, SETTINGS_BAR_ICON_SIZE, editor.activeTool == tool,
                                   TOOL_ICONS[i], hovered)) {
            editor.activeTool = tool;
        }
        if (hovered) {
            ImGui::SetTooltip("%s  (%s)\n%s", editorToolLabel(tool), editorToolShortcut(tool),
                              editorToolHint(tool));
        }
    }
    ImGui::PopID();
}

// A sphere resting on a ground plane with the contact shadow AO exists to produce: the one drawing
// that says "darkening where surfaces meet" without a caption. Drawn dim when the setting is off, so
// the icon reads as the same object either way and only the state changes.
static void drawAmbientOcclusionIcon(ImDrawList* drawList, ImVec2 center, float size, bool enabled) {
    float radius = size * 0.30f;
    ImVec2 sphereCenter = ImVec2(center.x, center.y - size * 0.06f);
    float groundY = sphereCenter.y + radius + size * 0.10f;

    ImU32 groundColor = enabled ? IM_COL32(150, 160, 175, 210) : IM_COL32(120, 126, 136, 150);
    ImU32 sphereColor = enabled ? IM_COL32(236, 240, 248, 255) : IM_COL32(150, 156, 166, 190);
    ImU32 shadowColor = enabled ? IM_COL32(20, 24, 32, 235) : IM_COL32(70, 74, 82, 120);

    drawList->AddLine(ImVec2(center.x - size * 0.42f, groundY), ImVec2(center.x + size * 0.42f, groundY),
                      groundColor, 1.5f);
    // Under the sphere and slightly wider than it: the contact shadow, drawn before the sphere so the
    // sphere sits on top of its own darkening rather than beside it.
    drawList->AddEllipseFilled(ImVec2(sphereCenter.x, groundY - 1.0f),
                               ImVec2(radius * 1.15f, radius * 0.34f), shadowColor);
    drawList->AddCircleFilled(sphereCenter, radius, sphereColor, 24);
    // The sphere's own occluded underside. Two shrinking arcs rather than a gradient, which is as much
    // as a 30-pixel icon can resolve anyway.
    drawList->AddCircleFilled(ImVec2(sphereCenter.x, sphereCenter.y + radius * 0.42f), radius * 0.52f,
                              enabled ? IM_COL32(120, 128, 145, 190) : IM_COL32(120, 126, 136, 110), 20);
}

// An isometric cube with its three visible faces at three brightnesses -- which is precisely what the
// setting does to the scene, so the icon is a sample of its own effect.
static void drawNormalShadingIcon(ImDrawList* drawList, ImVec2 center, float size, bool enabled) {
    float halfWidth = size * 0.34f;    // Half the cube's width across the flats.
    float halfHeight = size * 0.19f;   // Half the height of the top face's diamond.
    float depth = size * 0.30f;        // Length of the vertical edges.

    // The six silhouette points of an isometric cube, from the top vertex clockwise.
    ImVec2 top = ImVec2(center.x, center.y - depth * 0.5f - halfHeight);
    ImVec2 right = ImVec2(center.x + halfWidth, center.y - depth * 0.5f);
    ImVec2 bottomOfTopFace = ImVec2(center.x, center.y - depth * 0.5f + halfHeight);
    ImVec2 left = ImVec2(center.x - halfWidth, center.y - depth * 0.5f);
    ImVec2 rightLower = ImVec2(right.x, right.y + depth);
    ImVec2 centerLower = ImVec2(bottomOfTopFace.x, bottomOfTopFace.y + depth);
    ImVec2 leftLower = ImVec2(left.x, left.y + depth);

    // The three face brightnesses mirror shade.frag's SHADE_AXIS: top full, and the two sides
    // different from each other so the icon shows the distinction the setting is there to make.
    ImU32 topColor = enabled ? IM_COL32(238, 242, 250, 255) : IM_COL32(150, 156, 166, 180);
    ImU32 leftColor = enabled ? IM_COL32(150, 158, 176, 255) : IM_COL32(126, 132, 142, 165);
    ImU32 rightColor = enabled ? IM_COL32(96, 104, 122, 255) : IM_COL32(104, 110, 120, 150);

    ImVec2 topFace[4] = { top, right, bottomOfTopFace, left };
    ImVec2 leftFace[4] = { left, bottomOfTopFace, centerLower, leftLower };
    ImVec2 rightFace[4] = { bottomOfTopFace, right, rightLower, centerLower };

    drawList->AddConvexPolyFilled(topFace, 4, topColor);
    drawList->AddConvexPolyFilled(leftFace, 4, leftColor);
    drawList->AddConvexPolyFilled(rightFace, 4, rightColor);
}

// One toggle button, on the shared chrome the tool strip uses. The only thing that differs is what
// it has to say about itself: a toggle reports on/off, a tool reports its shortcut.
static bool drawSettingsBarButton(const char* id, bool enabled, const char* tooltip,
                                  void (*drawIcon)(ImDrawList*, ImVec2, float, bool)) {
    bool hovered = false;
    bool clicked = drawViewportIconButton(id, SETTINGS_BAR_ICON_SIZE, enabled, drawIcon, hovered);
    if (hovered) {
        ImGui::SetTooltip("%s  (%s)", tooltip, enabled ? "on" : "off");
    }
    return clicked;
}

// Where the bar sits: centred along the bottom edge of the scene image. False when the panel is too
// small to hold it — the toggles keep their values and the bar comes back when there is room, which
// is less startling than one overhanging the panel edge.
//
// Split out from the drawing because the viewport needs the rectangle *before* the bar is drawn: the
// bar goes on top of the scene image, so everything the image drives (the eyedropper, the gizmo) has
// to know the mouse is over the bar rather than over the scene, and the bar is drawn last so the
// gizmo's lines cannot be laid across it.
static bool viewportSettingsBarRect(const EditorState& editor, ImVec2& barMin, ImVec2& barMax) {
    const int buttonCount = 2;
    float barWidth = buttonCount * SETTINGS_BAR_ICON_SIZE + (buttonCount - 1) * SETTINGS_BAR_SPACING +
                     2.0f * SETTINGS_BAR_PADDING;
    float barHeight = SETTINGS_BAR_ICON_SIZE + 2.0f * SETTINGS_BAR_PADDING;

    if (editor.viewportImageMax.x - editor.viewportImageMin.x < barWidth + 2.0f * SETTINGS_BAR_MARGIN ||
        editor.viewportImageMax.y - editor.viewportImageMin.y < barHeight + 2.0f * SETTINGS_BAR_MARGIN) {
        return false;
    }

    barMin = ImVec2((editor.viewportImageMin.x + editor.viewportImageMax.x - barWidth) * 0.5f,
                    editor.viewportImageMax.y - SETTINGS_BAR_MARGIN - barHeight);
    barMax = ImVec2(barMin.x + barWidth, barMin.y + barHeight);
    return true;
}

// Draws the bar and runs its buttons. Submitted last inside the Viewport panel, which puts it on top
// of the scene image both in the draw list and in ImGui's within-window hover ordering.
static void drawViewportSettingsBar(EditorState& editor) {
    ImVec2 barMin;
    ImVec2 barMax;
    if (!viewportSettingsBarRect(editor, barMin, barMax)) {
        return;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(barMin, barMax, IM_COL32(18, 20, 26, 205), 8.0f);
    drawList->AddRect(barMin, barMax, IM_COL32(255, 255, 255, 28), 8.0f, 0, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(barMin.x + SETTINGS_BAR_PADDING, barMin.y + SETTINGS_BAR_PADDING));
    ImGui::PushID("ViewportSettings");
    // No ImGui::SameLine between the buttons: it works off the layout cursor, and each button below
    // places its own screen position explicitly, which SameLine would then override.
    if (drawSettingsBarButton("ao", editor.ambientOcclusionEnabled,
                              "Ambient occlusion", drawAmbientOcclusionIcon)) {
        editor.ambientOcclusionEnabled = !editor.ambientOcclusionEnabled;
        editor.renderSettingsChanged = true;
    }
    ImGui::SetCursorScreenPos(ImVec2(barMin.x + SETTINGS_BAR_PADDING + SETTINGS_BAR_ICON_SIZE + SETTINGS_BAR_SPACING,
                                     barMin.y + SETTINGS_BAR_PADDING));
    if (drawSettingsBarButton("normal", editor.normalShadingEnabled,
                              "Normal shading", drawNormalShadingIcon)) {
        editor.normalShadingEnabled = !editor.normalShadingEnabled;
        editor.renderSettingsChanged = true;
    }
    ImGui::PopID();
}

// =============================================================================
// Viewport breadcrumb
// =============================================================================

// The last element of the loaded scene's path, which is the name anyone would call the scene by.
// scenePath always ends in a separator, so the last element is empty and the name is one up.
static std::string sceneDisplayName(const EditorState& editor) {
    if (!editor.sceneLoaded) return "(no scene)";
    std::filesystem::path path(editor.scenePath);
    std::string name = path.filename().string();
    if (name.empty()) name = path.parent_path().filename().string();
    return name.empty() ? editor.scenePath : name;
}

// A strip along the top of the Viewport reading scene ▸ folder ▸ … ▸ selection, every element of it
// clickable.
//
// It exists because the thing an edit lands on is the selection, and until now the only place that
// said so was a highlighted row in a panel that might be scrolled away from it. A tool that is about
// to add or remove voxels needs its target stated where the voxels are, not somewhere else on
// screen — and the ancestors being clickable makes "work on the whole asset instead" one click
// rather than a hunt back up the tree.
static void drawViewportBreadcrumb(const projv::Scene& scene, EditorState& editor) {
    // Nudged in from the content region's own start, not placed absolutely: SetCursorPos is relative
    // to the *window*, not to its content, so an absolute y of a few pixels puts the strip up behind
    // the dock tab bar -- where it is drawn, and invisible.
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + style.ItemSpacing.x);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);

    // Flat buttons: the strip should read as a path with clickable words, not as a row of controls.
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 1.0f));

    // The scene itself is the root of the path, and selects nothing — clicking it is how you get
    // back to "the whole scene, no component chosen".
    if (ImGui::SmallButton(sceneDisplayName(editor).c_str())) {
        editor.selectedComponent = projv::INVALID_COMPONENT_HANDLE;
        editor.selectedVoxelCount = 0;
        editor.selectionOutlineValid = false;
    }

    if (editor.selectedComponent < scene.components.size()) {
        // Walked up and reversed rather than recursed, so the buttons come out root-first.
        std::vector<projv::ComponentHandle> chain;
        for (projv::ComponentHandle walker = editor.selectedComponent;
             walker != projv::INVALID_COMPONENT_HANDLE && walker < scene.components.size();
             walker = scene.components[walker].parent) {
            chain.push_back(walker);
        }
        std::reverse(chain.begin(), chain.end());

        for (projv::ComponentHandle handle : chain) {
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextDisabled(">");
            ImGui::SameLine(0.0f, 2.0f);

            const projv::ComponentRecord& component = scene.components[handle];
            std::string label = component.name.empty() ? ("component " + std::to_string(handle))
                                                       : component.name;
            // The selection itself is the end of the path and is drawn lit; its ancestors are
            // context. Both are clickable — selecting the one you are already on is harmless.
            bool isSelection = handle == editor.selectedComponent;
            if (isSelection) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.86f, 0.16f, 1.00f));
            }
            ImGui::PushID(int(handle));
            if (ImGui::SmallButton(label.c_str())) {
                editor.selectedComponent = handle;
                editor.selectedVoxelCount = projv::utils::getComponentVoxelCount(scene, handle);
                editor.selectionOutlineValid = false;
                editor.revealSelectionInHierarchy = true;
            }
            ImGui::PopID();
            if (isSelection) {
                ImGui::PopStyleColor();
            }
        }
    } else {
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextDisabled("- nothing selected");
    }

    // The active tool, right-aligned: the other half of "what will happen if I click in here".
    std::string toolText = std::string(editorToolLabel(editor.activeTool)) + "  " +
                           editorToolShortcut(editor.activeTool);
    float toolWidth = ImGui::CalcTextSize(toolText.c_str()).x;
    float available = ImGui::GetWindowWidth() - toolWidth - style.ItemSpacing.x * 2.0f;
    if (available > ImGui::GetCursorPosX()) {
        ImGui::SameLine(available);
        ImGui::TextDisabled("%s", toolText.c_str());
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::Separator();
}

// The status message, floated over the top of the scene for a few seconds after it changes. The
// status bar carries the same string permanently; this is for the ones that answer something the
// user did in the viewport ("Picked slot 7...", "Nothing under the cursor.") where looking away to
// the bottom of the window to read the answer is the whole problem.
static void drawViewportToast(EditorState& editor) {
    if (editor.statusMessage != editor.toastMessage) {
        editor.toastMessage = editor.statusMessage;
        editor.toastShownAt = ImGui::GetTime();
    }
    if (editor.toastMessage.empty()) return;

    const double HOLD_SECONDS = 3.0;
    const double FADE_SECONDS = 0.8;
    double age = ImGui::GetTime() - editor.toastShownAt;
    if (age > HOLD_SECONDS + FADE_SECONDS) return;

    float alpha = age <= HOLD_SECONDS ? 1.0f : float(1.0 - (age - HOLD_SECONDS) / FADE_SECONDS);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 textSize = ImGui::CalcTextSize(editor.toastMessage.c_str());
    ImVec2 textPos = ImVec2((editor.viewportImageMin.x + editor.viewportImageMax.x - textSize.x) * 0.5f,
                            editor.viewportImageMin.y + 12.0f);
    drawList->AddRectFilled(ImVec2(textPos.x - 10.0f, textPos.y - 5.0f),
                            ImVec2(textPos.x + textSize.x + 10.0f, textPos.y + textSize.y + 5.0f),
                            IM_COL32(14, 16, 22, int(190.0f * alpha)), 5.0f);
    drawList->AddText(textPos, IM_COL32(226, 232, 244, int(255.0f * alpha)),
                      editor.toastMessage.c_str());
}

// The scene, drawn as an image. The panel is what decides the render resolution: its size is
// recorded here and the render targets follow it at the top of the next frame.
static void drawViewportPanel(projv::Scene& scene, const std::shared_ptr<projv::ConstructedRenderer>& renderer,
                              EditorState& editor, float framebufferScale) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Viewport");

    editor.viewportHovered = ImGui::IsWindowHovered();

    // Above the image, so the scene is rendered into what is left rather than under it.
    drawViewportBreadcrumb(scene, editor);

    ImVec2 panelSize = ImGui::GetContentRegionAvail();
    editor.requestedViewportWidth = std::max(1, int(panelSize.x * framebufferScale));
    editor.requestedViewportHeight = std::max(1, int(panelSize.y * framebufferScale));

    // Escape disarms the eyedropper, matching every other modal tool.
    if (editor.materialPickerActive && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        editor.materialPickerActive = false;
    }

    bgfx::TextureHandle viewportTexture = getViewportTexture(renderer);
    if (editor.sceneLoaded && bgfx::isValid(viewportTexture)) {
        // OpenGL's texture origin is the bottom-left corner and everyone else's is the top-left, so
        // on a GL backend the render target arrives upside down and the V axis is flipped back here.
        bool originBottomLeft = bgfx::getCaps()->originBottomLeft;
        ImVec2 uv0 = originBottomLeft ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
        ImVec2 uv1 = originBottomLeft ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
        ImGui::Image(ImTextureRef(projv::editor::imGuiTextureID(viewportTexture)), panelSize, uv0, uv1);

        // Where the image landed on screen, so a click inside it can be turned into the same ray the
        // shader used for that pixel.
        editor.viewportImageMin = ImGui::GetItemRectMin();
        editor.viewportImageMax = ImGui::GetItemRectMax();
        bool imageHovered = ImGui::IsItemHovered();

        // Both overlay bars float on top of the image, so a click that lands on an icon must not also
        // be a click on the scene behind it. Their rectangles are known here even though they are
        // drawn at the end of the panel — see viewportToolbarRect / viewportSettingsBarRect.
        ImVec2 barMin;
        ImVec2 barMax;
        if (viewportToolbarRect(editor, barMin, barMax) &&
            ImGui::IsMouseHoveringRect(barMin, barMax, false)) {
            imageHovered = false;
        }
        if (viewportSettingsBarRect(editor, barMin, barMax) &&
            ImGui::IsMouseHoveringRect(barMin, barMax, false)) {
            imageHovered = false;
        }

        // The selected component's .data box(es), outlined in yellow. Cache is invalidated by the
        // hierarchy click handler and by loading a new scene; recomputing it is a tree walk, not
        // something to redo every frame.
        //
        // The gizmo runs before the click is interpreted below, and it is drawn over the outline so
        // its handles are never buried by a box edge.
        bool gizmoBusy = false;
        if (editor.selectedComponent != projv::INVALID_COMPONENT_HANDLE) {
            refreshSelectionCaches(scene, editor);
            ImDrawList* outlineDrawList = ImGui::GetWindowDrawList();
            projv::core::vec3 cameraDirection = computeCameraDirection(editor);
            for (projv::ChunkHandle chunkHandle : editor.selectionOutlineChunks) {
                if (chunkHandle >= scene.chunks.size()) continue;
                drawChunkOutline(outlineDrawList, scene.chunks[chunkHandle], editor.cameraPosition,
                                 cameraDirection, editor.viewportImageMin, editor.viewportImageMax);
            }
            // A hovered handle counts as busy, not just a live drag: the click that is about to
            // start the drag arrives on the frame the handle is merely hovered, and letting it also
            // select would change the selection out from under the drag it just began.
            gizmoBusy = updateAndDrawTransformGizmo(scene, editor, imageHovered) ||
                        editor.gizmoHoveredHandle >= 0;
        }

        // --- What a left click in the scene means ---
        //
        // One place decides, rather than each feature reaching for the mouse itself. The eyedropper
        // wins when it is armed (the user asked for exactly one sample and is waiting to give it),
        // then the tool decides, and Alt is the paint tool's own sample modifier — the standard one
        // in every paint program, and the reason Paint does not need the armed mode at all.
        // UV within the image, which is what the ray generator wants — independent of where the panel
        // happens to sit and of the panel-to-pixel scale.
        ImVec2 mousePosition = ImGui::GetIO().MousePos;
        float imageWidth = std::max(1.0f, editor.viewportImageMax.x - editor.viewportImageMin.x);
        float imageHeight = std::max(1.0f, editor.viewportImageMax.y - editor.viewportImageMin.y);
        projv::core::vec2 cursorUV = {
            (mousePosition.x - editor.viewportImageMin.x) / imageWidth,
            (mousePosition.y - editor.viewportImageMin.y) / imageHeight
        };

        if (imageHovered && !gizmoBusy && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            editor.pickUV = cursorUV;

            if (editor.materialPickerActive) {
                editor.pendingPick = PickPurpose::SampleMaterial;
            } else if (editor.activeTool == EditorTool::Paint) {
                if (ImGui::GetIO().KeyAlt) {
                    editor.pendingPick = PickPurpose::SampleMaterial;
                } else {
                    beginPaintStroke(editor);
                    editor.pendingPick = PickPurpose::PaintVoxel;
                }
            } else if (editor.activeTool == EditorTool::Sculpt) {
                if (ImGui::GetIO().KeyAlt) {
                    // Sculpt borrows Paint's Alt-sample: the brush places the palette's current entry,
                    // so "use that colour" has to be reachable without leaving the tool.
                    editor.pendingPick = PickPurpose::SampleMaterial;
                } else {
                    beginSculptStroke(editor);
                    editor.pendingPick = PickPurpose::SculptVoxel;
                }
            } else {
                editor.pendingPick = PickPurpose::SelectComponent;
            }
        }

        // --- The rest of a sculpt drag ---
        //
        // Sampled every frame the cursor actually moves, which is what makes a stroke follow the
        // cursor instead of landing one dab per click. Hover is deliberately not required after the
        // press: the cursor routinely leaves the image partway through a drag, and a stroke that cut
        // out at the edge of the panel would be far more surprising than one that keeps going. The
        // release is watched unconditionally for the same reason — a button let go outside the panel
        // still ends the stroke, rather than leaving it armed for the next time the mouse comes back.
        if (editor.sculptStrokeActive) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                ImVec2 movement = ImGui::GetIO().MouseDelta;
                if (movement.x != 0.0f || movement.y != 0.0f) {
                    editor.pickUV = cursorUV;
                    editor.pendingPick = PickPurpose::SculptVoxel;
                }
            } else {
                endSculptStroke(scene, editor);
            }
        }

        // --- The rest of a paint drag ---
        //
        // The same shape as the sculpt drag, for the same reasons: sampled on movement rather than on
        // clicks, hover no longer required once the button is down (a sweep routinely leaves the panel
        // partway through), and the release watched wherever the cursor happens to be.
        //
        // A fill takes no part in the drag -- it did its work on the press -- so its frames are not
        // even sampled, which spares them a ray cast that could only be thrown away.
        if (editor.paintStrokeActive) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                endPaintStroke(scene, editor);
            } else if (!paintShapeIsFill(editor.paintStrokeShape)) {
                ImVec2 movement = ImGui::GetIO().MouseDelta;
                if (movement.x != 0.0f || movement.y != 0.0f) {
                    editor.pickUV = cursorUV;
                    editor.pendingPick = PickPurpose::PaintVoxel;
                }
            }
        }

        if (editor.materialPickerActive) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            // A hint, because an armed tool with no visible state is a trap.
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRect(editor.viewportImageMin, editor.viewportImageMax,
                              IM_COL32(120, 200, 255, 200), 0.0f, 0, 2.0f);
            const char* hint = "Click a voxel to pick its material  (Esc to cancel)";
            ImVec2 hintSize = ImGui::CalcTextSize(hint);
            ImVec2 hintPos = ImVec2((editor.viewportImageMin.x + editor.viewportImageMax.x - hintSize.x) * 0.5f,
                                    editor.viewportImageMin.y + 12.0f);
            drawList->AddRectFilled(ImVec2(hintPos.x - 8.0f, hintPos.y - 4.0f),
                                    ImVec2(hintPos.x + hintSize.x + 8.0f, hintPos.y + hintSize.y + 4.0f),
                                    IM_COL32(0, 0, 0, 160), 4.0f);
            drawList->AddText(hintPos, IM_COL32(200, 230, 255, 255), hint);
        } else {
            // Shares the top-centre strip with the eyedropper's hint, which takes precedence: an
            // armed tool has more to say than what happened a moment ago.
            drawViewportToast(editor);
        }

        // Last, so they are on top of the outline and the gizmo as well as the scene.
        drawViewportToolbar(editor);
        drawViewportSettingsBar(editor);
    } else {
        const char* message = "No scene loaded - File > Load Scene...";
        ImVec2 textSize = ImGui::CalcTextSize(message);
        ImGui::SetCursorPos(ImVec2((panelSize.x - textSize.x) * 0.5f, (panelSize.y - textSize.y) * 0.5f));
        ImGui::TextDisabled("%s", message);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

// Builds one frame of the whole interface: the host window that owns the dockspace and the menu bar,
// then each panel docked into it.
static void drawEditorInterface(projv::Application& app, projv::Scene& scene, projv::GPUData& gpuData,
                                EditorState& editor,
                                const std::shared_ptr<projv::ConstructedRenderer>& renderer,
                                float framebufferScale) {
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mainViewport->WorkPos);
    ImGui::SetNextWindowSize(mainViewport->WorkSize);
    ImGui::SetNextWindowViewport(mainViewport->ID);

    // A borderless, immovable window filling the OS window. It exists only to host the dockspace and
    // the menu bar; every visible panel is docked inside it.
    ImGuiWindowFlags hostFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##EditorDockHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    // Versioned: the panels and their arrangement changed (Inspector/Tool/Palette stacked rather than
    // tabbed, Library added, Statistics gone), and an imgui.ini written by the previous layout has
    // saved positions for some of those windows but not others. Restoring it would half-migrate the
    // layout — old panels where they were, new ones floating. A new dockspace ID has no saved node,
    // which is exactly the "first run" condition below, so the default layout is rebuilt once.
    ImGuiID dockspaceID = ImGui::GetID("EditorDockSpaceV2");
    // A dockspace with no node behind it is a first run (or a reset): there is no imgui.ini layout to
    // restore, so the default one is built.
    if (!editor.dockLayoutBuilt || editor.resetDockLayoutRequested) {
        if (ImGui::DockBuilderGetNode(dockspaceID) == nullptr || editor.resetDockLayoutRequested) {
            buildDefaultDockLayout(dockspaceID, mainViewport->WorkSize);
        }
        editor.dockLayoutBuilt = true;
        editor.resetDockLayoutRequested = false;
    }
    ImGui::DockSpace(dockspaceID);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Load Scene...", "Ctrl+O")) {
                editor.loadSceneDialogOpen = true;
            }
            // Ctrl+Shift+R, not Ctrl+R: the plain chord is the Paint tool now, and reloading a scene
            // is a rare, expensive action that can afford the extra modifier far better than a tool
            // switch can.
            if (ImGui::MenuItem("Reload Scene", "Ctrl+Shift+R", false, editor.sceneLoaded)) {
                editor.pendingScenePath = editor.scenePath;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                app.closeAppFlag = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            const std::vector<projv::editor::EditRecord>& entries = editor.history.entries();
            std::string undoLabel = editor.history.canUndo()
                                  ? "Undo " + entries[editor.history.position()].label : "Undo";
            std::string redoLabel = editor.history.canRedo()
                                  ? "Redo " + entries[editor.history.position() + 1].label : "Redo";
            if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, editor.history.canUndo())) {
                editor.history.undo();
            }
            if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, editor.history.canRedo())) {
                editor.history.redo();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear History", nullptr, false, !entries.empty())) {
                editor.history.clear();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            for (int i = 0; i < EDITOR_TOOL_COUNT; i++) {
                EditorTool tool = static_cast<EditorTool>(i);
                if (ImGui::MenuItem(editorToolLabel(tool), editorToolShortcut(tool),
                                    editor.activeTool == tool)) {
                    editor.activeTool = tool;
                }
            }
            ImGui::Separator();
            ImGui::MenuItem("Transform Gizmo", nullptr, &editor.gizmoEnabled);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Frame Scene", "H", false, editor.sceneLoaded)) {
                applyFraming(editor);
            }
            if (ImGui::MenuItem("Reset Layout")) {
                editor.resetDockLayoutRequested = true;
            }
            ImGui::Separator();
            // A floating window rather than a dock panel: it is a handful of read-only numbers that
            // are looked at while something is being diagnosed and ignored the rest of the time, and
            // a permanent panel's worth of screen is more than that is worth. The summary is always
            // on screen in the status bar regardless.
            ImGui::MenuItem("Statistics", nullptr, &editor.statisticsWindowOpen);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // The shortcuts the menu items advertise, live anywhere in the editor. All of them are gated on
    // the keyboard not belonging to a text field: Ctrl+Z is the field's own undo while it is being
    // typed in, and a tool switch triggered by typing "Query" into the rename box would be worse.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_O)) {
            editor.loadSceneDialogOpen = true;
        }
        // Shift is what separates a reload from selecting the Paint tool, so each has to exclude the
        // other explicitly — IsKeyPressed says nothing about the modifiers held alongside it.
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_R) && editor.sceneLoaded) {
            editor.pendingScenePath = editor.scenePath;
        }
        if (!io.KeyShift) {
            if (ImGui::IsKeyPressed(ImGuiKey_Q)) editor.activeTool = EditorTool::Select;
            if (ImGui::IsKeyPressed(ImGuiKey_W)) editor.activeTool = EditorTool::Move;
            if (ImGui::IsKeyPressed(ImGuiKey_E)) editor.activeTool = EditorTool::Sculpt;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) editor.activeTool = EditorTool::Paint;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
            if (io.KeyShift) editor.history.redo(); else editor.history.undo();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
            editor.history.redo();
        }
    }

    ImGui::End();

    drawViewportPanel(scene, renderer, editor, framebufferScale);
    processVoxelPick(scene, editor);   // Between the panels: the click is seen above, the result is
                                       // shown below, both in this frame.
    drawHierarchyPanel(scene, editor);
    drawLibraryPanel(editor);
    drawInspectorPanel(scene, editor);
    drawToolPanel(scene, editor);
    drawPalettePanel(scene, gpuData, editor);
    drawHistoryPanel(editor);
    drawStatisticsWindow(scene, editor, app.frameCount);
    drawLoadSceneDialog(editor);
    // Outside the dock host, and last: the side bar takes its strip out of the viewport's work area,
    // which is what the host window is sized from on the next frame.
    drawStatusBar(scene, editor, mainViewport);
}

// Round-trip check on the two coordinate mappings the Paint tool stands on: pickToComponentVoxelCoord
// (chunk-local -> component space, the direction a pick arrives in) and componentVoxelToChunk (back
// again, the direction a brush asks in). They have to be exact inverses, and nothing else in the
// editor would notice if they were not — a brush whose reverse map is off paints a plausible-looking
// region in the wrong cell, which reads as "the tool is weird near chunk edges" rather than as a bug.
//
// Opt-in via EDITOR_SELFTEST=1, and read-only: it probes the tree64 and changes nothing. Cheap to run
// after touching either mapping or anything about how a Grid derives its resolution — the first
// version of this caught exactly that, a reverse map that read the resolution from `dataRefID`, which
// is -1 until a component's first edit and so reported an empty world on every freshly loaded scene.
//
//   EDITOR_SELFTEST=1 ./scene_editor ../ScenePreviewer/scenes/Sibenik
//   ... SELFTEST paint coords: 303918 probes, 9962 solid, 0 mismatches, 0 unmappable
static void runPaintCoordSelfTest(const projv::Scene& scene) {
    size_t checked = 0, solid = 0, mismatch = 0, mapFail = 0;

    for (projv::ComponentHandle component = 0; component < scene.components.size(); component++) {
        const projv::ComponentRecord& record = scene.components[component];
        if (record.kind == projv::ComponentKind::Asset) continue;

        ComponentVoxelSpace space = resolveComponentVoxelSpace(scene, component);
        std::vector<projv::ChunkHandle> leaves;
        collectLeafChunks(scene, component, leaves);
        for (projv::ChunkHandle chunkHandle : leaves) {
            const projv::Chunk& chunk = scene.chunks[chunkHandle];
            if (chunk.geometryPoolIndex < 0) continue;
            const projv::GeometryBlob& blob = scene.geometryPool[chunk.geometryPoolIndex];
            int32_t resolution = int32_t(chunk.header.resolution);

            // Strided so a 256^3 chunk costs ~50k probes rather than 16M.
            const int STRIDE = 7;
            for (int z = 0; z < resolution; z += STRIDE) {
                for (int y = 0; y < resolution; y += STRIDE) {
                    for (int x = 0; x < resolution; x += STRIDE) {
                        projv::core::ivec3 local(x, y, z);
                        uint8_t direct = 0;
                        bool directHit = projv::utils::queryVoxelMaterial(blob, chunk.header.resolution,
                                                                          local, direct);
                        checked++;
                        if (directHit) solid++;

                        // Forward: what a pick on this voxel would report as a component coord.
                        projv::utils::VoxelPick pick;
                        pick.hit = true;
                        pick.chunk = chunkHandle;
                        pick.component = component;
                        pick.voxelCoord = local;
                        projv::core::ivec3 componentCoord;
                        if (!pickToComponentVoxelCoord(scene, pick, componentCoord)) {
                            mapFail++;
                            continue;
                        }

                        // Backward: the brush's own lookup must land on the same voxel.
                        uint8_t viaComponent = 0;
                        bool componentHit = queryComponentVoxel(scene, space, componentCoord,
                                                                viaComponent);
                        if (componentHit != directHit || (directHit && viaComponent != direct)) {
                            if (mismatch < 8) {
                                projv::core::error("SELFTEST mismatch comp={} chunk={} local=({},{},{}) "
                                                   "compCoord=({},{},{}) direct={}/{} viaComponent={}/{}",
                                                   component, chunkHandle, x, y, z,
                                                   componentCoord.x, componentCoord.y, componentCoord.z,
                                                   int(directHit), int(direct),
                                                   int(componentHit), int(viaComponent));
                            }
                            mismatch++;
                        }
                    }
                }
            }
        }
    }
    projv::core::info("SELFTEST paint coords: {} probes, {} solid, {} mismatches, {} unmappable",
                      checked, solid, mismatch, mapFail);
}

// The sculpt tool's mapping, checked the same way and for the same reason: componentVoxelToWorld /
// worldToComponentVoxel claim that a component's voxels form *one* lattice in world space, so that a
// world point can be turned into a voxel coordinate whether or not a chunk exists there yet. That
// claim is what lets a stroke place its first voxel into an empty component and carry a hit across
// from another object -- and it is only true if the lattice agrees with the per-chunk mapping
// utils::pickVoxel inverts to report worldPosition.
//
// So this checks two things per probe:
//
//   1. The lattice puts a voxel where its own chunk puts it. A wrong coordOrigin on a Grid that has
//      been expanded downward fails here and nowhere else -- every chunk stays individually correct,
//      and the whole component's geometry is simply offset by a cell or two from where the tool aims.
//   2. World and voxel round-trip exactly, which is what stops a stroke from drifting by a voxel per
//      frame as it converts back and forth.
//
// Opt-in via EDITOR_SELFTEST=1, read-only, and worth re-running after touching either function or
// anything about how a Grid's origin moves.
//
//   ... SELFTEST sculpt lattice: 40704 probes, 0 lattice mismatches, 0 round-trip failures
static void runSculptLatticeSelfTest(const projv::Scene& scene) {
    size_t checked = 0, latticeMismatch = 0, roundTripFail = 0;

    for (projv::ComponentHandle component = 0; component < scene.components.size(); component++) {
        if (scene.components[component].kind == projv::ComponentKind::Asset) continue;

        ComponentVoxelSpace space = resolveComponentVoxelSpace(scene, component);
        if (!space.valid) continue;

        std::vector<projv::ChunkHandle> leaves;
        collectLeafChunks(scene, component, leaves);
        for (projv::ChunkHandle chunkHandle : leaves) {
            const projv::Chunk& chunk = scene.chunks[chunkHandle];
            int32_t resolution = int32_t(chunk.header.resolution);
            if (resolution <= 0) continue;
            float chunkVoxelSize = chunk.header.scale / float(resolution);
            projv::core::mat3 chunkRotation = glm::mat3_cast(chunk.header.rotation);
            // A generous tolerance in absolute terms, and still a hundredth of a voxel: the failures
            // this is looking for are whole cells out, not rounding.
            float tolerance = 0.01f * chunkVoxelSize;

            const int STRIDE = 23;   // Cheaper than the paint test; the mapping is per-chunk uniform.
            for (int z = 0; z < resolution; z += STRIDE) {
                for (int y = 0; y < resolution; y += STRIDE) {
                    for (int x = 0; x < resolution; x += STRIDE) {
                        projv::core::ivec3 local(x, y, z);
                        projv::core::ivec3 componentCoord;
                        if (!chunkVoxelToComponentCoord(scene, component, chunkHandle, local,
                                                        componentCoord)) {
                            continue;
                        }
                        checked++;

                        // Where the chunk itself says this voxel's centre is -- the same expression
                        // utils::pickVoxel uses to fill in VoxelPick::worldPosition.
                        projv::core::vec3 viaChunk = chunk.header.position +
                            chunkRotation * ((projv::core::vec3(local) + 0.5f) * chunkVoxelSize);
                        projv::core::vec3 viaLattice = componentVoxelToWorld(space, componentCoord);

                        if (glm::length(viaChunk - viaLattice) > tolerance) {
                            if (latticeMismatch < 8) {
                                projv::core::error("SELFTEST lattice comp={} chunk={} local=({},{},{}) "
                                                   "viaChunk=({:.3f},{:.3f},{:.3f}) "
                                                   "viaLattice=({:.3f},{:.3f},{:.3f})",
                                                   component, chunkHandle, x, y, z,
                                                   viaChunk.x, viaChunk.y, viaChunk.z,
                                                   viaLattice.x, viaLattice.y, viaLattice.z);
                            }
                            latticeMismatch++;
                        }

                        if (worldToComponentVoxel(space, viaLattice) != componentCoord) {
                            roundTripFail++;
                        }
                    }
                }
            }
        }
    }
    projv::core::info("SELFTEST sculpt lattice: {} probes, {} lattice mismatches, "
                      "{} round-trip failures", checked, latticeMismatch, roundTripFail);
}

// Drives a whole sculpt stroke with the mouse held still, and measures the one thing about the tool
// that cannot be checked by reading the code: that a drag does not climb its own deposits.
//
// Holding the ray fixed is the sharpest form of the problem. Every frame casts the same ray into a
// scene that now contains the previous frame's dab, so a tool that believed the scene would place the
// next dab on top of the last one and walk a column of voxels back toward the camera, one brush-width
// per frame, for as long as the button is held. That is the failure the solidity override exists to
// prevent, and it is invisible in a screenshot of a *finished* stroke -- it only shows while dragging.
//
// So the test runs the same stroke twice:
//
//   A. As shipped. The nearest voxel placed must stay within a brush radius of the surface the stroke
//      started on, however many frames it runs for.
//   B. With the stroke's touched set cleared between frames, which is exactly what the override reads,
//      so this is the tool with the fix removed. It must climb, and by a lot.
//
// B is there to prove A is measuring something. Without a control, "the voxels stayed near the
// surface" would also be the result if the stroke had quietly placed nothing at all.
//
// **This one mutates the scene** -- in memory, never saved, and undone again through the history
// before it returns (which also checks that undoing a stroke restores the voxel count exactly). It is
// behind its own environment variable rather than EDITOR_SELFTEST for that reason: the read-only
// checks are cheap to leave on, and an editor that edits the scene on startup is not.
//
//   EDITOR_SCULPTTEST=1 ./scene_editor ../ScenePreviewer/scenes/StonehillCastle
static void runSculptStrokeSelfTest(projv::Scene& scene, EditorState& editor) {
    using projv::core::ivec3;
    using projv::core::vec3;

    // --- Find something to sculpt on ---
    projv::ComponentHandle component = projv::INVALID_COMPONENT_HANDLE;
    ComponentVoxelSpace space;
    ivec3 seed(0);
    for (projv::ComponentHandle candidate = 0;
         candidate < scene.components.size() && component == projv::INVALID_COMPONENT_HANDLE; candidate++) {
        if (scene.components[candidate].kind == projv::ComponentKind::Asset) continue;
        if (scene.components[candidate].materialPalette.empty()) continue;
        ComponentVoxelSpace candidateSpace = resolveComponentVoxelSpace(scene, candidate);
        if (!candidateSpace.valid) continue;

        std::vector<projv::ChunkHandle> leaves;
        collectLeafChunks(scene, candidate, leaves);
        for (projv::ChunkHandle chunkHandle : leaves) {
            const projv::Chunk& chunk = scene.chunks[chunkHandle];
            if (chunk.geometryPoolIndex < 0) continue;
            int32_t resolution = int32_t(chunk.header.resolution);
            for (int z = 0; z < resolution && component == projv::INVALID_COMPONENT_HANDLE; z += 3) {
                for (int y = 0; y < resolution && component == projv::INVALID_COMPONENT_HANDLE; y += 3) {
                    for (int x = 0; x < resolution; x += 3) {
                        uint8_t slot = 0;
                        if (!projv::utils::queryVoxelMaterial(scene.geometryPool[chunk.geometryPoolIndex],
                                                              chunk.header.resolution, ivec3(x, y, z), slot)) {
                            continue;
                        }
                        if (!chunkVoxelToComponentCoord(scene, candidate, chunkHandle, ivec3(x, y, z), seed)) {
                            continue;
                        }
                        component = candidate;
                        space = candidateSpace;
                        break;
                    }
                }
            }
            if (component != projv::INVALID_COMPONENT_HANDLE) break;
        }
    }
    if (component == projv::INVALID_COMPONENT_HANDLE) {
        projv::core::warn("SCULPTTEST: no component with geometry and a palette; skipped");
        return;
    }

    // A camera looking down at that voxel from an arbitrary angle, far enough out to be clear of the
    // geometry. Whatever the ray hits first on the way in is the surface the stroke starts against --
    // it does not have to be the voxel that was sought, only something solid.
    vec3 direction = glm::normalize(vec3(0.35f, -1.0f, 0.28f));
    vec3 origin = componentVoxelToWorld(space, seed) - direction * (400.0f * space.voxelSize);
    projv::utils::VoxelPick opening = projv::utils::pickVoxel(scene, origin, direction);
    if (!opening.hit) {
        projv::core::warn("SCULPTTEST: the probe ray missed the scene; skipped");
        return;
    }

    // --- Set the tool up ---
    const int FRAMES = 20;
    SculptBrush savedBrush = editor.sculptBrush;
    SculptMode savedMode = editor.sculptMode;
    float savedRadius = editor.sculptRadius;
    projv::ComponentHandle savedPalette = editor.paletteComponent;
    int savedSlot = editor.selectedMaterialSlot;
    vec3 savedCamera = editor.cameraPosition;

    editor.sculptBrush = SculptBrush::Sphere;
    editor.sculptMode = SculptMode::Add;
    editor.sculptRadius = 3.0f;      // Small: this runs 40 rebuilds of a real chunk.
    editor.paletteComponent = component;
    editor.selectedMaterialSlot = 0;
    editor.cameraPosition = origin;

    uint32_t baselineVoxels = projv::utils::getComponentVoxelCount(scene, component);
    float surfaceDistance = opening.distance;
    // The brush is a ball centred on the surface, so its own near side is legitimately a radius closer
    // to the camera than the surface is. Two voxels of slack past that for the cell-centre rounding.
    float allowedApproach = (editor.sculptRadius + 2.0f) * space.voxelSize;

    // What the stroke did, and — the actual invariant — where the ray still believes the surface is
    // once the stroke has been running for a while.
    //
    // That last number is the one to assert on, and the extent of the placed voxels is not, because
    // the two failures do not look alike from the geometry's side. Climbing piles voxels up toward the
    // camera and shows up in their extent; tunnelling only does if there is something deeper to eat,
    // and a voxelised surface is usually a shell. Aim through a shell after removing a disc from it
    // and the ray does not hit "further in", it exits the scene entirely. Both failures do share one
    // signature, which is the definition of the fix: the ray stopped agreeing with where the stroke
    // started. So that is what is measured, and it holds whatever the object is made of.
    struct StrokeResult {
        size_t touched = 0;
        float nearest = 0.0f;
        float farthest = 0.0f;
        bool finalHit = false;
        float finalDistance = 0.0f;   // Where the ray reports the surface after the last frame.
        bool undoRestored = false;
    };

    // The control's restoration record. The journal is both the ray's memory and the undo record now,
    // so the control cannot simply throw it away -- doing that would take the stroke's undo with it and
    // leave the scene modified for the next case. Instead the journal is *rewritten* each frame to say
    // that the current geometry is what was always there, which is precisely the tool with the fix
    // removed, and the true original of each cell is kept here so the scene can be put back by hand.
    std::unordered_map<uint64_t, EditorState::StrokeVoxel> controlOriginals;

    auto runStroke = [&](SculptMode mode, bool keepOverride) {
        StrokeResult result;
        controlOriginals.clear();
        editor.sculptMode = mode;
        beginSculptStroke(editor);
        for (int frame = 0; frame < FRAMES; frame++) {
            processSculptSample(scene, editor, direction);
            if (!editor.sculptStrokeActive) break;
            if (keepOverride) continue;

            ComponentVoxelSpace live = resolveComponentVoxelSpace(scene, component);
            if (!live.valid) break;
            for (auto& entry : editor.sculptStrokeOriginal) {
                controlOriginals.emplace(entry.first, entry.second);   // First sighting is the truth.
                uint8_t slot = 0;
                bool nowSolid = queryComponentVoxel(scene, live, entry.second.coord, slot);
                entry.second.wasSolid = nowSolid;
                entry.second.oldColor = nowSolid ? componentVoxelColor(scene, component, slot) : 0u;
            }
        }

        result.touched = editor.sculptStrokeOriginal.size();
        result.nearest = std::numeric_limits<float>::infinity();
        result.farthest = -std::numeric_limits<float>::infinity();
        for (const auto& entry : editor.sculptStrokeOriginal) {
            const ivec3& coord = entry.second.coord;
            float along = glm::dot(componentVoxelToWorld(space, coord) - origin, direction);
            result.nearest = std::min(result.nearest, along);
            result.farthest = std::max(result.farthest, along);
        }

        // The same ray the stroke has been using, cast through the same override the stroke sees.
        // Run before endSculptStroke, which clears the set the override reads. In the control that
        // set is already empty, so this probe reads the raw scene — which is the point.
        projv::utils::VoxelPick probe = projv::utils::pickVoxel(scene, origin, direction, 1.0e6f,
                                                                makeSculptStrokeOverride(scene, editor));
        result.finalHit = probe.hit;
        result.finalDistance = probe.distance;

        endSculptStroke(scene, editor);
        if (keepOverride) {
            // Undoing puts the scene back for the next case, and checks the stroke's own undo on the
            // way: an addition must remove exactly what it added, a removal must restore every voxel
            // it took.
            result.undoRestored = editor.history.undo() &&
                                  projv::utils::getComponentVoxelCount(scene, component) == baselineVoxels;
            return result;
        }

        // The control's journal was rewritten as it went, so its undo restores the last frame rather
        // than the start. Put the scene back from the record kept above instead.
        editor.history.undo();
        std::vector<ivec3> restoreRemove, restoreAdd;
        std::vector<uint32_t> restoreColors;
        for (const auto& entry : controlOriginals) {
            if (entry.second.wasSolid) {
                restoreAdd.push_back(entry.second.coord);
                restoreColors.push_back(entry.second.oldColor);
            } else {
                restoreRemove.push_back(entry.second.coord);
            }
        }
        applyVoxelSculpt(&scene, &editor, component, restoreRemove, std::vector<uint32_t>(), false);
        applyVoxelSculpt(&scene, &editor, component, restoreAdd, restoreColors, true);
        editor.history.clear();
        result.undoRestored = projv::utils::getComponentVoxelCount(scene, component) == baselineVoxels;
        return result;
    };

    StrokeResult addKept = runStroke(SculptMode::Add, true);
    StrokeResult addControl = runStroke(SculptMode::Add, false);
    StrokeResult removeKept = runStroke(SculptMode::Remove, true);
    StrokeResult removeControl = runStroke(SculptMode::Remove, false);

    // --- Verdict ---
    // Held: the stroke did something, and the ray still finds the surface where it started.
    // Drifted: the ray lost it — either it now stops somewhere else entirely, or nothing is left to
    // stop it at all (the shell case above).
    auto held = [&](const StrokeResult& result) {
        return result.touched > 0 && result.finalHit &&
               std::abs(result.finalDistance - surfaceDistance) <= allowedApproach;
    };
    auto drifted = [&](const StrokeResult& result) {
        return !result.finalHit || std::abs(result.finalDistance - surfaceDistance) > allowedApproach;
    };

    auto report = [&](const char* what, const StrokeResult& result, bool ok) {
        projv::core::info("SCULPTTEST   {}: {} voxels, span [{:.1f}, {:.1f}], ray now {} -> {}",
                          what, result.touched, result.nearest, result.farthest,
                          result.finalHit ? fmt::format("{:.2f}", result.finalDistance)
                                          : std::string("nothing"),
                          ok ? "PASS" : "FAIL");
    };

    bool undoExact = addKept.undoRestored && addControl.undoRestored &&
                     removeKept.undoRestored && removeControl.undoRestored;

    projv::core::info("SCULPTTEST comp={} surface={:.2f} voxelSize={:.3f} frames={} tolerance={:.2f}",
                      component, surfaceDistance, space.voxelSize, FRAMES, allowedApproach);
    report("add,    override", addKept, held(addKept));
    report("add,    control ", addControl, drifted(addControl));
    report("remove, override", removeKept, held(removeKept));
    report("remove, control ", removeControl, drifted(removeControl));
    projv::core::info("SCULPTTEST   controls must drift and overrides must hold; undo back to {} -> {}",
                      baselineVoxels, undoExact ? "PASS" : "FAIL");

    editor.sculptBrush = savedBrush;
    editor.sculptMode = savedMode;
    editor.sculptRadius = savedRadius;
    editor.paletteComponent = savedPalette;
    editor.selectedMaterialSlot = savedSlot;
    editor.cameraPosition = savedCamera;
    editor.history.clear();
    editor.statusMessage.clear();
}

// Extrude, driven end to end without a mouse. Three properties, each of which fails silently:
//
//   1. **The face is a face.** Every voxel gathered must be in the clicked voxel's plane, use its
//      material, and have that same face exposed. A gather that leaked through the material test or
//      out of the plane still produces a plausible-looking extrusion — of the wrong region.
//   2. **A drag is reversible in place.** Pulling out to five and back to zero must leave the scene
//      exactly as it was, mid-drag, before any undo is involved. This is what the layer-by-layer walk
//      buys, and it is why layers record what they *displaced* rather than assuming empty space: a
//      face pushed into geometry that was already there (a stair tread into the tread above) would
//      otherwise have that geometry deleted on the way back.
//   3. **Depth one is exact.** Every voxel of a face has, by definition, an empty cell in front of it,
//      so pulling out one layer adds precisely one voxel per face voxel. Any other number means the
//      face and the layer disagree about which cells they cover.
//
// Like the stroke test this **edits the scene** and undoes itself; it runs under EDITOR_SCULPTTEST.
static void runExtrudeSelfTest(projv::Scene& scene, EditorState& editor) {
    using projv::core::ivec3;
    using projv::core::vec3;

    // Aim at something, from a direction that gives a face square enough to work with.
    projv::ComponentHandle component = projv::INVALID_COMPONENT_HANDLE;
    ComponentVoxelSpace space;
    projv::utils::VoxelPick pick;
    vec3 direction = glm::normalize(vec3(0.35f, -1.0f, 0.28f));
    vec3 origin(0.0f);

    for (projv::ComponentHandle candidate = 0;
         candidate < scene.components.size() && component == projv::INVALID_COMPONENT_HANDLE; candidate++) {
        if (scene.components[candidate].kind == projv::ComponentKind::Asset) continue;
        ComponentVoxelSpace candidateSpace = resolveComponentVoxelSpace(scene, candidate);
        if (!candidateSpace.valid) continue;

        std::vector<projv::ChunkHandle> leaves;
        collectLeafChunks(scene, candidate, leaves);
        for (projv::ChunkHandle chunkHandle : leaves) {
            const projv::Chunk& chunk = scene.chunks[chunkHandle];
            if (chunk.geometryPoolIndex < 0) continue;
            int32_t resolution = int32_t(chunk.header.resolution);
            for (int z = 0; z < resolution && component == projv::INVALID_COMPONENT_HANDLE; z += 5) {
                for (int y = 0; y < resolution && component == projv::INVALID_COMPONENT_HANDLE; y += 5) {
                    for (int x = 0; x < resolution; x += 5) {
                        uint8_t slot = 0;
                        if (!projv::utils::queryVoxelMaterial(scene.geometryPool[chunk.geometryPoolIndex],
                                                              chunk.header.resolution, ivec3(x, y, z), slot)) {
                            continue;
                        }
                        ivec3 seed;
                        if (!chunkVoxelToComponentCoord(scene, candidate, chunkHandle, ivec3(x, y, z), seed)) {
                            continue;
                        }
                        vec3 probeOrigin = componentVoxelToWorld(candidateSpace, seed) -
                                           direction * (400.0f * candidateSpace.voxelSize);
                        projv::utils::VoxelPick probe =
                            projv::utils::pickVoxel(scene, probeOrigin, direction);
                        if (!probe.hit || probe.faceNormal == ivec3(0)) continue;
                        if (probe.component != candidate) continue;

                        component = candidate;
                        space = candidateSpace;
                        pick = probe;
                        origin = probeOrigin;
                        break;
                    }
                }
            }
            if (component != projv::INVALID_COMPONENT_HANDLE) break;
        }
    }
    if (component == projv::INVALID_COMPONENT_HANDLE) {
        projv::core::warn("EXTRUDETEST: found no face to aim at; skipped");
        return;
    }

    ivec3 faceCoord;
    if (!pickToComponentVoxelCoord(scene, pick, faceCoord)) {
        projv::core::warn("EXTRUDETEST: the pick did not map into component space; skipped");
        return;
    }

    SculptBrush savedBrush = editor.sculptBrush;
    SelectionScope savedScope = editor.extrudeFaceScope;
    vec3 savedCamera = editor.cameraPosition;
    editor.sculptBrush = SculptBrush::Extrude;
    editor.extrudeFaceScope = SelectionScope::Material;
    editor.cameraPosition = origin;

    uint32_t baseline = projv::utils::getComponentVoxelCount(scene, component);

    beginSculptStroke(editor);
    editor.sculptStrokeComponent = component;
    if (!beginExtrudeDrag(scene, editor, space, pick, faceCoord, pick.faceNormal, direction)) {
        projv::core::warn("EXTRUDETEST: the face gather found nothing; skipped");
        editor.sculptStrokeActive = false;
        editor.sculptBrush = savedBrush;
        editor.cameraPosition = savedCamera;
        return;
    }

    // --- 1. The gathered face really is one face of one material ---
    //
    // Snapshotted, because every check below has to survive the drag that follows: endExtrudeDrag
    // clears the editor's copy, and the first version of this test asserted against the cleared one
    // and reported a clean face as a failure.
    std::vector<ivec3> face = editor.extrudeFace;
    std::vector<uint32_t> faceColors = editor.extrudeFaceColors;
    size_t faceSize = face.size();
    int normalAxis = pick.faceNormal.x != 0 ? 0 : (pick.faceNormal.y != 0 ? 1 : 2);
    size_t offPlane = 0, wrongMaterial = 0, buried = 0;
    for (const ivec3& faceVoxel : face) {
        if (faceVoxel[normalAxis] != faceCoord[normalAxis]) offPlane++;
        uint8_t slot = 0;
        if (!queryComponentVoxel(scene, space, faceVoxel, slot) || slot != pick.materialSlot) {
            wrongMaterial++;
        }
        uint8_t ahead = 0;
        if (queryComponentVoxel(scene, space, faceVoxel + pick.faceNormal, ahead)) buried++;
    }

    // --- 2. The other face scope, gathered read-only at the same seed ---
    //
    // Run here, before anything is extruded, so "exposed" is judged against the original surface
    // rather than against geometry this test put there. WholeFace only drops the material test, so it
    // must cover at least what Material did, and every voxel it adds must still be in the plane with
    // its face open. A scope that leaked out of the plane would extrude a region that is not a face.
    std::vector<ivec3> wholeFace;
    std::vector<uint32_t> wholeFaceColors;
    bool wholeTruncated = false;
    gatherFaceRegion(scene, space, component, faceCoord, pick.faceNormal, pick.materialSlot,
                     SelectionScope::Everything, EXTRUDE_MAX_FACE_VOXELS, wholeFace,
                     wholeFaceColors, wholeTruncated);
    size_t wholeOffPlane = 0, wholeBuried = 0;
    for (const ivec3& faceVoxel : wholeFace) {
        if (faceVoxel[normalAxis] != faceCoord[normalAxis]) wholeOffPlane++;
        uint8_t ahead = 0;
        if (queryComponentVoxel(scene, space, faceVoxel + pick.faceNormal, ahead)) wholeBuried++;
    }
    bool wholeSane = wholeFace.size() >= faceSize && wholeOffPlane == 0 && wholeBuried == 0 &&
                     wholeFace.size() == wholeFaceColors.size();

    // --- 3. One layer out adds exactly one voxel per face voxel, each in its own material ---
    applyExtrudeDepth(scene, editor, space, 1);
    uint32_t afterOne = projv::utils::getComponentVoxelCount(scene, component);
    bool exactOne = (afterOne == baseline + uint32_t(faceSize));

    // The colour each new voxel came out in. Under Material these are all the same and getting it
    // wrong is invisible; under WholeFace they are not, and a face that flattened to one entry would
    // erase the pattern of whatever was pulled.
    size_t wrongColor = 0;
    for (size_t index = 0; index < face.size(); index++) {
        uint8_t slot = 0;
        ivec3 pulled = face[index] + pick.faceNormal;
        if (!queryComponentVoxel(scene, space, pulled, slot) ||
            componentVoxelColor(scene, component, slot) != faceColors[index]) {
            wrongColor++;
        }
    }

    // --- 2. Reversible in place, out and back, with no undo involved ---
    applyExtrudeDepth(scene, editor, space, 5);
    uint32_t afterFive = projv::utils::getComponentVoxelCount(scene, component);
    applyExtrudeDepth(scene, editor, space, 2);
    applyExtrudeDepth(scene, editor, space, 0);
    uint32_t backToZero = projv::utils::getComponentVoxelCount(scene, component);

    // And the same going inward, which removes rather than adds.
    applyExtrudeDepth(scene, editor, space, -3);
    uint32_t afterCarve = projv::utils::getComponentVoxelCount(scene, component);
    applyExtrudeDepth(scene, editor, space, 0);
    uint32_t backFromCarve = projv::utils::getComponentVoxelCount(scene, component);

    // --- Extruding *into* something that was already there ---
    //
    // The case the layer records exist for, and the one no natural face in these scenes happens to
    // produce: the counts above show every layer landing in empty space. So it is built on purpose --
    // one voxel of another colour parked two layers out, which the extrusion must overwrite on the
    // way out and hand back, in its own colour, on the way in. Without the records, retracting would
    // delete it; a "restore" that used the face's colour would return it as the wrong material.
    bool displacementTested = false;
    bool displacementSurvived = false;
    bool displacementColorKept = false;
    uint32_t markerColor = 0;
    for (const projv::Material& entry : scene.components[component].materialPalette) {
        if (entry.packedColor != faceColors.front()) {
            markerColor = entry.packedColor;
            displacementTested = true;
            break;
        }
    }
    if (displacementTested) {
        ivec3 marker = face.front() + pick.faceNormal * 2;
        uint8_t occupied = 0;
        if (queryComponentVoxel(scene, space, marker, occupied)) {
            displacementTested = false;   // Something is already there; not a clean experiment.
        } else {
            applyVoxelSculpt(&scene, &editor, component, { marker }, { markerColor }, true);

            applyExtrudeDepth(scene, editor, space, 3);   // Layer 2 lands on the marker.
            applyExtrudeDepth(scene, editor, space, 0);   // ...and must give it back.

            uint8_t slot = 0;
            displacementSurvived = queryComponentVoxel(scene, space, marker, slot);
            displacementColorKept = displacementSurvived &&
                                    componentVoxelColor(scene, component, slot) == markerColor;

            applyVoxelSculpt(&scene, &editor, component, { marker }, std::vector<uint32_t>(), false);
        }
    }
    uint32_t afterDisplacement = projv::utils::getComponentVoxelCount(scene, component);

    // --- Finish a real drag and check undo ---
    applyExtrudeDepth(scene, editor, space, 3);
    uint32_t afterThree = projv::utils::getComponentVoxelCount(scene, component);
    editor.sculptStrokeActive = false;
    editor.sculptStrokeBrush = SculptBrush::Extrude;
    endExtrudeDrag(scene, editor);
    bool undone = editor.history.undo();
    uint32_t afterUndo = projv::utils::getComponentVoxelCount(scene, component);

    bool faceClean = faceSize > 0 && offPlane == 0 && wrongMaterial == 0 && buried == 0 &&
                     faceColors.size() == faceSize;
    bool reversible = backToZero == baseline && backFromCarve == baseline;
    bool carved = afterCarve < baseline;
    bool undoExact = undone && afterUndo == baseline;

    projv::core::info("EXTRUDETEST comp={} face={} voxels normal=({},{},{})",
                      component, faceSize, pick.faceNormal.x, pick.faceNormal.y, pick.faceNormal.z);
    projv::core::info("EXTRUDETEST   face is one plane/material/surface: {} off-plane, {} wrong "
                      "material, {} buried -> {}", offPlane, wrongMaterial, buried,
                      faceClean ? "PASS" : "FAIL");
    projv::core::info("EXTRUDETEST   depth 1 adds one per face voxel: {} -> {} (expected {}) -> {}",
                      baseline, afterOne, baseline + uint32_t(faceSize), exactOne ? "PASS" : "FAIL");
    projv::core::info("EXTRUDETEST   each pulled voxel keeps its own material: {} wrong of {} -> {}",
                      wrongColor, faceSize, wrongColor == 0 ? "PASS" : "FAIL");
    projv::core::info("EXTRUDETEST   whole-face scope: {} voxels (material scope {}), {} off-plane, "
                      "{} buried -> {}", wholeFace.size(), faceSize, wholeOffPlane, wholeBuried,
                      wholeSane ? "PASS" : "FAIL");
    projv::core::info("EXTRUDETEST   reversible mid-drag: out to 5 ({}) and back ({}), in to -3 ({}) "
                      "and back ({}), baseline {} -> {}",
                      afterFive, backToZero, afterCarve, backFromCarve, baseline,
                      (reversible && carved) ? "PASS" : "FAIL");
    projv::core::info("EXTRUDETEST   undo of a depth-3 drag: {} -> {} (baseline {}) -> {}",
                      afterThree, afterUndo, baseline, undoExact ? "PASS" : "FAIL");
    if (displacementTested) {
        projv::core::info("EXTRUDETEST   extruded over an existing voxel, then retracted: survived={} "
                          "colour kept={} count back to {} -> {}",
                          displacementSurvived, displacementColorKept, afterDisplacement,
                          (displacementSurvived && displacementColorKept &&
                           afterDisplacement == baseline) ? "PASS" : "FAIL");
    } else {
        projv::core::warn("EXTRUDETEST   displacement case not exercised (no spare palette colour, "
                          "or the cell was occupied)");
    }

    editor.sculptBrush = savedBrush;
    editor.extrudeFaceScope = savedScope;
    editor.cameraPosition = savedCamera;
    editor.history.clear();
    editor.statusMessage.clear();
}

// Smooth and Bump, checked against a slab built for the purpose rather than against whatever the
// loaded scene happens to contain.
//
// Both operators are defined by what they do to shapes, so the test needs shapes it knows: a flat
// slab with a one-voxel spike on it and a one-voxel dent in it. On a real scene "did it get smoother"
// is not a question with a numeric answer, and the properties that matter would be invisible:
//
//   1. **Smoothing is stable on a flat surface.** A majority filter that drifted would eat a flat wall
//      just for being pointed at, which is the difference between a smooth brush and a delete brush.
//   2. **It removes a spike and fills a dent** -- in the same pass, from the same rule.
//   3. **Bump moves exactly one layer per tick**, and Pull and Push are inverses: pull once, push
//      once, and the slab is back.
//
// Built in a scratch component and torn down again, so it does not care what scene is loaded. Runs
// under EDITOR_SCULPTTEST with the other two, since it edits.
static void runSculptOperatorSelfTest(projv::Scene& scene, EditorState& editor) {
    using projv::core::ivec3;

    // A component to build in, and somewhere empty inside it to build.
    //
    // Inside its *existing* bounds, deliberately: a slab parked far off in space would make the Grid
    // expand to reach it and leave a shell of empty cells behind after the teardown, permanently
    // changing the scene this test is supposed to leave alone.
    projv::ComponentHandle component = projv::INVALID_COMPONENT_HANDLE;
    ivec3 base(0);
    const int SLAB = 13;   // Comfortably wider than the brush, so its rim never enters the result.

    for (projv::ComponentHandle candidate = 0;
         candidate < scene.components.size() && component == projv::INVALID_COMPONENT_HANDLE; candidate++) {
        if (scene.components[candidate].kind == projv::ComponentKind::Asset) continue;
        if (scene.components[candidate].materialPalette.empty()) continue;
        ComponentVoxelSpace probe = resolveComponentVoxelSpace(scene, candidate);
        if (!probe.valid || probe.resolution < SLAB + 8) continue;

        std::vector<projv::ChunkHandle> leaves;
        collectLeafChunks(scene, candidate, leaves);
        if (leaves.empty()) continue;

        // Any cell whose (SLAB+4)^3 box is entirely empty. Scanned coarsely; the first that fits wins.
        ivec3 cellOrigin;
        if (!chunkVoxelToComponentCoord(scene, candidate, leaves.front(), ivec3(0), cellOrigin)) continue;
        for (int z = 2; z + SLAB + 4 < probe.resolution && component == projv::INVALID_COMPONENT_HANDLE; z += 9) {
            for (int y = 2; y + SLAB + 4 < probe.resolution && component == projv::INVALID_COMPONENT_HANDLE; y += 9) {
                for (int x = 2; x + SLAB + 4 < probe.resolution; x += 9) {
                    ivec3 corner = cellOrigin + ivec3(x, y, z);
                    bool clear = true;
                    for (int dz = -1; dz <= SLAB + 2 && clear; dz++) {
                        for (int dy = -1; dy <= SLAB + 2 && clear; dy++) {
                            for (int dx = -1; dx <= SLAB + 2 && clear; dx++) {
                                uint8_t slot = 0;
                                if (queryComponentVoxel(scene, probe, corner + ivec3(dx, dy, dz), slot)) {
                                    clear = false;
                                }
                            }
                        }
                    }
                    if (!clear) continue;
                    component = candidate;
                    base = corner;
                    break;
                }
            }
        }
    }
    if (component == projv::INVALID_COMPONENT_HANDLE) {
        projv::core::warn("OPERATORTEST: found no empty room inside an editable component; skipped");
        return;
    }

    uint32_t color = scene.components[component].materialPalette.front().packedColor;

    // The space is re-resolved on every read, because every write below can invalidate it -- see the
    // note on ComponentVoxelSpace::grid. Holding one across the test is what made the first version of
    // this read freed memory back as geometry and report a working brush as broken.
    auto solidAt = [&](ivec3 coord) {
        ComponentVoxelSpace space = resolveComponentVoxelSpace(scene, component);
        uint8_t slot = 0;
        return space.valid && queryComponentVoxel(scene, space, coord, slot);
    };

    // Two solid layers: one is not enough, since a lone layer is not stable under a majority filter
    // (its cells see only 9 of 27 solid) and the test would be measuring that instead.
    std::vector<ivec3> slab;
    for (int z = 0; z < SLAB; z++) {
        for (int x = 0; x < SLAB; x++) {
            slab.push_back(base + ivec3(x, 0, z));
            slab.push_back(base + ivec3(x, 1, z));
        }
    }
    ivec3 centre = base + ivec3(SLAB / 2, 1, SLAB / 2);
    ivec3 spike = centre + ivec3(0, 1, 0);      // One voxel standing proud of the surface.
    ivec3 dent = centre + ivec3(3, 0, 0);       // One voxel missing from the surface.
    // A voxel floating clear of everything, three cells above the spike -- close enough to be inside
    // the brush, far enough that its 3x3x3 holds nothing but itself. It is the noise end of the scale
    // (disagreement 13) where the spike and the dent are the surface-detail end (4 each), which is
    // what makes the strength setting measurable: a weak pass must take this and leave those.
    ivec3 noise = centre + ivec3(0, 4, 0);

    std::vector<ivec3> build = slab;
    build.push_back(spike);
    build.push_back(noise);
    build.erase(std::remove(build.begin(), build.end(), dent), build.end());
    applyVoxelSculpt(&scene, &editor, component, build, std::vector<uint32_t>(build.size(), color), true);

    // Every cell of the slab's top layer, away from its rim, still solid and still bare -- skipping
    // the two columns the test deliberately deformed, which are not flat and are not supposed to be.
    // Checking them here is what made the first version of this contradict itself: it asserted the
    // surface was bare at the very column it had just put a spike on.
    auto surfaceIntact = [&]() {
        for (int z = 3; z < SLAB - 3; z++) {
            for (int x = 3; x < SLAB - 3; x++) {
                ivec3 top = base + ivec3(x, 1, z);
                if (top.x == spike.x && top.z == spike.z) continue;
                if (top.x == dent.x && top.z == dent.z) continue;
                if (!solidAt(top)) return false;
                if (solidAt(top + ivec3(0, 1, 0))) return false;
            }
        }
        return true;
    };
    auto iterate = [&](ivec3 at) { applySculptIteration(scene, editor, at); };

    SculptBrush savedBrush = editor.sculptBrush;
    SculptMode savedMode = editor.sculptMode;
    float savedRadius = editor.sculptRadius;
    editor.sculptRadius = 5.0f;

    bool builtSpike = solidAt(spike), builtDent = !solidAt(dent), builtSlab = solidAt(centre);
    bool builtNoise = solidAt(noise);

    // --- Smooth at the weakest strength ---
    //
    // Run first, and undone again, so the full-strength case below still meets the shape that was
    // built. What a weak pass has to be is *selective*, not merely slower: four ticks of it must take
    // the floating voxel and still leave the spike and the dent, however long it is held. Held is the
    // word -- these brushes repeat on a tick, so a strength that only slowed the same verdict down
    // would converge on the same result in a second and be no setting at all.
    float savedSmoothStrength = editor.sculptSmoothStrength;
    editor.sculptBrush = SculptBrush::Smooth;
    editor.sculptSmoothStrength = 0.0f;
    beginSculptStroke(editor);
    editor.sculptStrokeComponent = component;
    editor.sculptStrokeBrush = SculptBrush::Smooth;

    for (int pass = 0; pass < 4; pass++) iterate(centre);
    bool weakTookNoise = !solidAt(noise);
    bool weakSparedSpike = solidAt(spike);
    bool weakSparedDent = !solidAt(dent);
    bool weakHeldFlat = surfaceIntact();

    editor.sculptStrokeActive = true;
    endSculptStroke(scene, editor);
    bool weakUndone = editor.history.undo();
    bool restoredAfterWeak = solidAt(noise) && solidAt(spike) && !solidAt(dent);
    editor.sculptSmoothStrength = savedSmoothStrength;

    // --- Smooth ---
    editor.sculptBrush = SculptBrush::Smooth;
    editor.sculptSmoothStrength = 1.0f;   // The full majority filter, which is the default.
    beginSculptStroke(editor);
    editor.sculptStrokeComponent = component;
    editor.sculptStrokeBrush = SculptBrush::Smooth;

    for (int pass = 0; pass < 4; pass++) iterate(centre);
    bool spikeGone = !solidAt(spike);
    bool dentFilled = solidAt(dent);
    bool noiseGone = !solidAt(noise);
    bool flatHeld = surfaceIntact();

    editor.sculptStrokeActive = true;
    endSculptStroke(scene, editor);
    bool smoothUndone = editor.history.undo();
    bool spikeBack = solidAt(spike), dentBack = !solidAt(dent), surfaceBack = surfaceIntact();
    bool restoredAfterSmooth = spikeBack && dentBack && surfaceBack && solidAt(noise);

    // --- Smooth above full strength ---
    //
    // The reason the scale goes past 1 at all, and the one claim that has to be proved rather than
    // asserted: past 1 is a *different operator*, not more of the same one.
    //
    // The slab is the proof. Two solid layers are a fixed point of the 3x3x3 filter -- every cell of
    // the top layer sees 18 of 27 and stays, and the case just above confirms four ticks leave it
    // exactly as it was -- so no amount of holding at strength 1 will ever touch it. One tick at
    // strength 2 sees 50 solid of 125 where it wants 63 and takes the whole thing. That is smoother
    // in a way more passes cannot be, and it is simultaneously the hazard the Tool panel warns about:
    // a body thinner than the kernel does not get smoothed, it dissolves.
    editor.sculptSmoothStrength = 2.0f;
    beginSculptStroke(editor);
    editor.sculptStrokeComponent = component;
    editor.sculptStrokeBrush = SculptBrush::Smooth;

    iterate(centre);
    bool wideTookTheSlab = !solidAt(centre) && !surfaceIntact();

    editor.sculptStrokeActive = true;
    endSculptStroke(scene, editor);
    bool wideUndone = editor.history.undo();
    bool restoredAfterWide = solidAt(centre) && surfaceIntact() && solidAt(spike) &&
                             solidAt(noise) && !solidAt(dent);
    editor.sculptSmoothStrength = savedSmoothStrength;

    // --- Bump, on ground made flat first ---
    //
    // The spike stands directly over the centre, and a pull grows on top of whatever is under it, so
    // "one layer and no more" is not even true of a correct bump while the spike is there. Flattening
    // first is what makes the assertion mean what it says. (The first version of this test asserted it
    // anyway and reported a working brush as broken.)
    applyVoxelSculpt(&scene, &editor, component, { spike, noise }, std::vector<uint32_t>(), false);
    applyVoxelSculpt(&scene, &editor, component, { dent }, { color }, true);
    bool flatGround = !solidAt(spike) && !solidAt(noise) && solidAt(dent) && surfaceIntact();

    // --- Bump ---
    editor.sculptBrush = SculptBrush::Bump;
    editor.sculptMode = SculptMode::Add;   // Pull.
    // Blend off: the "exactly one layer" property is about the bare operator. A blend pass is *meant*
    // to move the result around, and is checked separately below.
    float savedBlend = editor.sculptBlendStrength;
    editor.sculptBlendStrength = 0.0f;
    beginSculptStroke(editor);
    editor.sculptStrokeComponent = component;
    editor.sculptStrokeBrush = SculptBrush::Bump;
    editor.sculptStrokeMode = SculptMode::Add;

    iterate(centre);
    // One tick lifts the surface above the centre by exactly one layer, and no further.
    bool pulledOne = solidAt(centre + ivec3(0, 1, 0)) && !solidAt(centre + ivec3(0, 2, 0));

    editor.sculptStrokeMode = SculptMode::Remove;   // Push, without ending the stroke.
    iterate(centre);
    bool pushedBack = !solidAt(centre + ivec3(0, 1, 0));

    editor.sculptStrokeActive = true;
    endSculptStroke(scene, editor);
    editor.history.undo();

    // --- Blend ---
    //
    // A blended pull must leave no cliff at the brush's rim. "Cliff" has a precise meaning here: a
    // cell of the new layer standing over untouched surface with nothing beside it -- the step the
    // user described as feeling like a stamped sphere. Measured as the tallest single-cell step found
    // anywhere around the bump's edge, with the blend off and then on.
    auto rimStep = [&]() {
        int worst = 0;
        for (int z = 1; z < SLAB - 1; z++) {
            for (int x = 1; x < SLAB - 1; x++) {
                ivec3 top = base + ivec3(x, 1, z);
                int height = 0;
                while (height < 6 && solidAt(top + ivec3(0, height + 1, 0))) height++;
                // How far this column stands above its lowest face neighbour.
                int lowest = 6;
                for (int axis = 0; axis < 2; axis++) {
                    for (int sign = -1; sign <= 1; sign += 2) {
                        ivec3 step(0);
                        step[axis == 0 ? 0 : 2] = sign;
                        int neighbour = 0;
                        while (neighbour < 6 && solidAt(top + step + ivec3(0, neighbour + 1, 0))) neighbour++;
                        lowest = std::min(lowest, neighbour);
                    }
                }
                worst = std::max(worst, height - lowest);
            }
        }
        return worst;
    };

    auto measureBump = [&](float blend) {
        editor.sculptBlendStrength = blend;
        editor.sculptMode = SculptMode::Add;
        beginSculptStroke(editor);
        editor.sculptStrokeComponent = component;
        editor.sculptStrokeBrush = SculptBrush::Bump;
        editor.sculptStrokeMode = SculptMode::Add;
        for (int tick = 0; tick < 3; tick++) iterate(centre);
        int step = rimStep();
        editor.sculptStrokeActive = true;
        endSculptStroke(scene, editor);
        editor.history.undo();
        return step;
    };
    int hardStep = measureBump(0.0f);
    int defaultStep = measureBump(savedBlend);
    int blendedStep = measureBump(SCULPT_MAX_BLEND);
    editor.sculptBlendStrength = savedBlend;

    // --- How long a tick actually takes ---
    //
    // These brushes run ten times a second for as long as the button is held, so a tick has about
    // 100 ms of room and wants far less. Timed at a middling radius on the slab built above; the
    // number is dominated by reading the neighbourhood, which is what the scratch array exists for.
    // Timed at both ends of the strength scale, because they are different amounts of work: the count
    // is the kernel's volume per cell, so the widest setting reads 343 cells where the default reads
    // 27. That multiple is the one thing that could put this brush over its tick.
    double tickMilliseconds = 0.0;
    double wideTickMilliseconds = 0.0;
    {
        editor.sculptBlendStrength = savedBlend;
        editor.sculptBrush = SculptBrush::Smooth;
        editor.sculptRadius = 10.0f;

        auto timeTicks = [&](float strength) {
            editor.sculptSmoothStrength = strength;
            beginSculptStroke(editor);
            editor.sculptStrokeComponent = component;
            editor.sculptStrokeBrush = SculptBrush::Smooth;

            const int TICKS = 10;
            auto started = std::chrono::steady_clock::now();
            for (int tick = 0; tick < TICKS; tick++) iterate(centre);
            auto finished = std::chrono::steady_clock::now();
            double each =
                std::chrono::duration<double, std::milli>(finished - started).count() / double(TICKS);

            editor.sculptStrokeActive = true;
            endSculptStroke(scene, editor);
            editor.history.undo();
            return each;
        };

        tickMilliseconds = timeTicks(1.0f);
        wideTickMilliseconds = timeTicks(SCULPT_MAX_SMOOTH_STRENGTH);

        editor.sculptSmoothStrength = savedSmoothStrength;
        editor.sculptRadius = 5.0f;
    }

    // --- Tear the slab down ---
    std::vector<ivec3> everything = slab;
    everything.push_back(spike);
    everything.push_back(noise);
    applyVoxelSculpt(&scene, &editor, component, everything, std::vector<uint32_t>(), false);
    bool cleanedUp = !solidAt(centre) && !solidAt(spike) && !solidAt(dent) && !solidAt(noise);

    projv::core::info("OPERATORTEST comp={} slab={}x{} at ({},{},{}) radius={:.1f} (built slab={} "
                      "spike={} dent={} noise={})", component, SLAB, SLAB, base.x, base.y, base.z,
                      editor.sculptRadius, builtSlab, builtSpike, builtDent, builtNoise);
    projv::core::info("OPERATORTEST   smooth at full strength removes a spike, fills a dent and "
                      "clears noise: spike gone={} dent filled={} noise gone={} -> {}",
                      spikeGone, dentFilled, noiseGone,
                      (spikeGone && dentFilled && noiseGone) ? "PASS" : "FAIL");
    projv::core::info("OPERATORTEST   smooth at strength 0 (cutoff {}) takes only the noise, however "
                      "long it is held: noise gone={} spike kept={} dent kept={} surface={} -> {}",
                      sculptSmoothCutoff(0.0f), weakTookNoise, weakSparedSpike, weakSparedDent,
                      weakHeldFlat,
                      (weakTookNoise && weakSparedSpike && weakSparedDent && weakHeldFlat)
                          ? "PASS" : "FAIL");
    projv::core::info("OPERATORTEST   smooth at strength 2 ({}x{}x{}) takes a two-layer slab that "
                      "strength 1 cannot: {} -> {}",
                      2 * sculptSmoothKernelRadius(2.0f) + 1, 2 * sculptSmoothKernelRadius(2.0f) + 1,
                      2 * sculptSmoothKernelRadius(2.0f) + 1, wideTookTheSlab,
                      wideTookTheSlab ? "PASS" : "FAIL");
    projv::core::info("OPERATORTEST   smooth leaves a flat surface alone: {} -> {}",
                      flatHeld, flatHeld ? "PASS" : "FAIL");
    projv::core::info("OPERATORTEST   undo restores the shape exactly: spike={} dent={} surface={} "
                      "(weak pass too={}, wide pass too={}) -> {}",
                      spikeBack, dentBack, surfaceBack, (weakUndone && restoredAfterWeak),
                      (wideUndone && restoredAfterWide),
                      (smoothUndone && restoredAfterSmooth && weakUndone && restoredAfterWeak &&
                       wideUndone && restoredAfterWide) ? "PASS" : "FAIL");
    projv::core::info("OPERATORTEST   bump pulls one layer and pushes it back: flat={} pulled={} "
                      "pushed={} -> {}", flatGround, pulledOne, pushedBack,
                      (flatGround && pulledOne && pushedBack) ? "PASS" : "FAIL");
    projv::core::info("OPERATORTEST   blend softens the rim: tallest step {} at blend 0, {} at the "
                      "default {:.0f}, {} at {:.0f} -> {}", hardStep, defaultStep, savedBlend,
                      blendedStep, SCULPT_MAX_BLEND,
                      (defaultStep < hardStep && blendedStep <= defaultStep) ? "PASS" : "FAIL");
    projv::core::info("OPERATORTEST   smooth tick at radius 10: {:.2f} ms at strength 1, {:.2f} ms at "
                      "{:.0f} (budget {:.0f} ms) -> {}",
                      tickMilliseconds, wideTickMilliseconds, SCULPT_MAX_SMOOTH_STRENGTH,
                      SCULPT_ITERATION_SECONDS * 1000.0,
                      (tickMilliseconds < SCULPT_ITERATION_SECONDS * 1000.0 &&
                       wideTickMilliseconds < SCULPT_ITERATION_SECONDS * 1000.0) ? "PASS" : "FAIL");
    projv::core::info("OPERATORTEST   scratch geometry removed: {} -> {}",
                      cleanedUp, cleanedUp ? "PASS" : "FAIL");

    editor.sculptBrush = savedBrush;
    editor.sculptMode = savedMode;
    editor.sculptRadius = savedRadius;
    editor.history.clear();
    editor.statusMessage.clear();
}

// The other half of the paint self-test: a flood fill must be adjacency-closed. If any solid voxel
// of the seed's material touches the gathered set but is not in it, the traversal left part of the
// region behind -- which is exactly what a truncated or mis-ordered fill does, and exactly what no
// amount of looking at the result will tell you, because a fill's shape is supposed to be arbitrary.
//
// This is what caught the striping: 1,147,019 leaks on StonehillCastle, from a budget that counted
// probed coordinates (about seven per voxel of the region) instead of voxels of the region, stopping
// a depth-first traversal a seventh of the way in and leaving tendrils behind.
//
// Read-only: it gathers with a colour no palette holds, so nothing is filtered out as "already this
// colour", and it never applies anything.
static void runFillSelfTest(const projv::Scene& scene, EditorState& editor) {
    using projv::core::ivec3;

    PaintShape savedShape = editor.paintShape;
    SelectionScope savedScope = editor.paintFillScope;
    editor.paintShape = PaintShape::FillVolume;
    // Pinned: adjacency-closure is asserted against the seed's *material*, so the scope that stops at
    // one is the scope this test is about. Under Everything the region is the whole connected body
    // and the property is trivially true.
    editor.paintFillScope = SelectionScope::Material;

    for (projv::ComponentHandle component = 0; component < scene.components.size(); component++) {
        const projv::ComponentRecord& record = scene.components[component];
        if (record.kind == projv::ComponentKind::Asset || record.materialPalette.empty()) continue;
        ComponentVoxelSpace space = resolveComponentVoxelSpace(scene, component);
        if (!space.valid) continue;

        // Seed on the most common material, so the region under test is the largest one available --
        // the small ones were always fine, which is how the bug survived the first round of testing.
        std::vector<projv::ChunkHandle> leaves;
        collectLeafChunks(scene, component, leaves);
        std::vector<size_t> slotCounts(256, 0);
        std::vector<ivec3> slotSeeds(256, ivec3(0));
        std::vector<bool> haveSeed(256, false);
        for (projv::ChunkHandle chunkHandle : leaves) {
            const projv::Chunk& chunk = scene.chunks[chunkHandle];
            if (chunk.geometryPoolIndex < 0) continue;
            int32_t resolution = int32_t(chunk.header.resolution);
            for (int z = 0; z < resolution; z += 2)
              for (int y = 0; y < resolution; y += 2)
                for (int x = 0; x < resolution; x += 2) {
                    uint8_t slot;
                    if (!projv::utils::queryVoxelMaterial(scene.geometryPool[chunk.geometryPoolIndex],
                                                          chunk.header.resolution, ivec3(x, y, z), slot)) {
                        continue;
                    }
                    slotCounts[slot]++;
                    if (haveSeed[slot]) continue;
                    projv::utils::VoxelPick pick;
                    pick.hit = true;
                    pick.chunk = chunkHandle;
                    pick.component = component;
                    pick.voxelCoord = ivec3(x, y, z);
                    if (pickToComponentVoxelCoord(scene, pick, slotSeeds[slot])) haveSeed[slot] = true;
                }
        }
        int best = -1;
        for (int slot = 0; slot < 256; slot++) {
            if (haveSeed[slot] && (best < 0 || slotCounts[slot] > slotCounts[best])) best = slot;
        }
        if (best < 0) continue;

        // A colour no entry holds, so collectPaintTargets drops nothing as already-that-colour and
        // the gathered set is the whole region.
        const uint32_t UNUSED_COLOR = 0x0AAAAAAAu;
        std::vector<ivec3> coords;
        std::vector<uint32_t> previous;
        bool truncated = false;
        // The volume fill ignores the face normal, so any value does; zero is the honest one.
        collectPaintTargets(scene, component, slotSeeds[best], uint8_t(best), ivec3(0), editor,
                            UNUSED_COLOR, coords, previous, truncated);

        VisitedVoxels gathered(scene, space);
        for (const ivec3& coord : coords) gathered.mark(coord);

        static const ivec3 NEIGHBOURS[6] = {
            ivec3(1, 0, 0), ivec3(-1, 0, 0), ivec3(0, 1, 0),
            ivec3(0, -1, 0), ivec3(0, 0, 1), ivec3(0, 0, -1)
        };
        size_t leaks = 0;
        for (const ivec3& coord : coords) {
            for (const ivec3& step : NEIGHBOURS) {
                ivec3 neighbour = coord + step;
                if (gathered.isMarked(neighbour)) continue;   // In the set, or outside the component.
                uint8_t slot;
                if (!queryComponentVoxel(scene, space, neighbour, slot)) continue;
                if (slot != uint8_t(best)) continue;
                if (leaks < 5) {
                    projv::core::error("SELFTEST fill leak: ({},{},{}) touches ({},{},{}), slot {}, "
                                       "not gathered", coord.x, coord.y, coord.z,
                                       neighbour.x, neighbour.y, neighbour.z, int(slot));
                }
                leaks++;
            }
        }
        projv::core::info("SELFTEST fill: comp={} slot={} gathered={} truncated={} leaks={}",
                          component, best, coords.size(), int(truncated), leaks);

        // Fill face against the same seed. It is the volume fill restricted to one plane of the
        // surface, so two things must hold, and both are the point of having the mode at all: it
        // never reaches a voxel the volume fill did not, and it never leaves the plane. A face fill
        // that escaped its plane would be a volume fill wearing the safer name -- exactly the
        // surprise the split exists to prevent.
        for (int axis = 0; axis < 3; axis++) {
            for (int sign = -1; sign <= 1; sign += 2) {
                ivec3 normal(0);
                normal[axis] = sign;
                uint8_t ahead = 0;
                // Only a direction the seed is actually exposed in describes a face it is on.
                if (queryComponentVoxel(scene, space, slotSeeds[best] + normal, ahead)) continue;

                std::vector<ivec3> faceCoords;
                std::vector<uint32_t> faceColors;
                bool faceTruncated = false;
                gatherFaceRegion(scene, space, component, slotSeeds[best], normal, uint8_t(best),
                                 SelectionScope::Material, PAINT_FILL_LIMIT, faceCoords, faceColors,
                                 faceTruncated);

                size_t offPlane = 0, outsideVolume = 0;
                for (const ivec3& coord : faceCoords) {
                    if (coord[axis] != slotSeeds[best][axis]) offPlane++;
                    if (!gathered.isMarked(coord)) outsideVolume++;
                }
                projv::core::info("SELFTEST fill face: comp={} normal=({},{},{}) gathered={} "
                                  "off-plane={} outside-volume={} -> {}",
                                  component, normal.x, normal.y, normal.z, faceCoords.size(),
                                  offPlane, outsideVolume,
                                  (offPlane == 0 && (outsideVolume == 0 || truncated)) ? "PASS" : "FAIL");
                axis = 3;   // One representative face per component is enough.
                break;
            }
        }
    }

    editor.paintShape = savedShape;
    editor.paintFillScope = savedScope;
}

// The Paint tool as a drag rather than a click. Four things have to hold, and only the first is
// visible from a single frame:
//
//   1. Every voxel the stroke recorded is the stroke's colour afterwards, and undoing puts each one
//      back to the colour it had. That is the same property a single click has.
//   2. The whole sweep is *one* entry in the history. One gesture, one undo -- a stroke that recorded
//      an entry per frame would take a dozen presses of Ctrl+Z to take back.
//   3. No voxel appears in the record twice. Consecutive dabs of a run overlap by design and the
//      scene is not written until the end of the frame, so the colour test that keeps a voxel out of
//      later frames cannot see the repeats within one; dropDuplicatePaintTargets is what does.
//   4. The gap between two samples is filled in. This is the point of the whole stroke: a drag is
//      sampled once per frame, so the cursor moves several voxels between samples and an
//      uninterpolated sweep is a dotted line -- which is exactly what clicking repeatedly already
//      gave. It is asserted against the strongest control available: the same two dab centres,
//      collected on the untouched scene with nothing in between. The stroke must paint strictly more
//      than those two dabs do.
//
// **This one mutates the scene**, in memory and undone again before it returns, so it sits behind the
// same environment variable as the sculpt stroke test rather than the read-only EDITOR_SELFTEST.
static void runPaintStrokeSelfTest(projv::Scene& scene, EditorState& editor) {
    using projv::core::ivec3;
    using projv::core::vec3;

    // --- Find something to paint on ---
    //
    // The same search the sculpt stroke test uses, plus a palette of at least two entries: the stroke
    // needs a colour that is not already on the surface, or collectPaintTargets rightly collects
    // nothing and there is no stroke to measure.
    projv::ComponentHandle component = projv::INVALID_COMPONENT_HANDLE;
    ComponentVoxelSpace space;
    ivec3 seed(0);
    for (projv::ComponentHandle candidate = 0;
         candidate < scene.components.size() && component == projv::INVALID_COMPONENT_HANDLE; candidate++) {
        if (scene.components[candidate].kind == projv::ComponentKind::Asset) continue;
        if (scene.components[candidate].materialPalette.size() < 2) continue;
        ComponentVoxelSpace candidateSpace = resolveComponentVoxelSpace(scene, candidate);
        if (!candidateSpace.valid) continue;

        std::vector<projv::ChunkHandle> leaves;
        collectLeafChunks(scene, candidate, leaves);
        for (projv::ChunkHandle chunkHandle : leaves) {
            const projv::Chunk& chunk = scene.chunks[chunkHandle];
            if (chunk.geometryPoolIndex < 0) continue;
            int32_t resolution = int32_t(chunk.header.resolution);
            for (int z = 0; z < resolution && component == projv::INVALID_COMPONENT_HANDLE; z += 3) {
                for (int y = 0; y < resolution && component == projv::INVALID_COMPONENT_HANDLE; y += 3) {
                    for (int x = 0; x < resolution; x += 3) {
                        uint8_t slot = 0;
                        if (!projv::utils::queryVoxelMaterial(scene.geometryPool[chunk.geometryPoolIndex],
                                                              chunk.header.resolution, ivec3(x, y, z), slot)) {
                            continue;
                        }
                        if (!chunkVoxelToComponentCoord(scene, candidate, chunkHandle, ivec3(x, y, z), seed)) {
                            continue;
                        }
                        component = candidate;
                        space = candidateSpace;
                        break;
                    }
                }
            }
            if (component != projv::INVALID_COMPONENT_HANDLE) break;
        }
    }
    if (component == projv::INVALID_COMPONENT_HANDLE) {
        projv::core::warn("PAINTSTROKE: no component with geometry and two palette entries; skipped");
        return;
    }

    vec3 direction = glm::normalize(vec3(0.35f, -1.0f, 0.28f));
    vec3 origin = componentVoxelToWorld(space, seed) - direction * (400.0f * space.voxelSize);
    projv::utils::VoxelPick first = projv::utils::pickVoxel(scene, origin, direction);
    if (!first.hit || first.component != component) {
        projv::core::warn("PAINTSTROKE: the probe ray missed the component; skipped");
        return;
    }

    // --- The two ends of one drag ---
    //
    // A drag is a rotation of the ray, not a translation of the camera, so the second sample is aimed
    // at a point along the surface rather than cast from somewhere else. Several sweep lengths are
    // tried because the first one may run off the object, and both directions along the tangent
    // because which way is "along the wall" depends on the wall.
    vec3 up = std::abs(direction.y) > 0.9f ? vec3(1.0f, 0.0f, 0.0f) : vec3(0.0f, 1.0f, 0.0f);
    vec3 tangent = glm::normalize(glm::cross(direction, up));
    ivec3 startCentre(0);
    if (!pickToComponentVoxelCoord(scene, first, startCentre)) {
        projv::core::warn("PAINTSTROKE: the probe hit does not map into the component; skipped");
        return;
    }

    projv::utils::VoxelPick second;
    ivec3 endCentre(0);
    int sweep = 0;
    for (int voxels : { 24, 16, 8 }) {
        for (int sign = 1; sign >= -1 && sweep == 0; sign -= 2) {
            vec3 target = first.worldPosition + tangent * (float(sign * voxels) * space.voxelSize);
            projv::utils::VoxelPick candidate =
                projv::utils::pickVoxel(scene, origin, glm::normalize(target - origin));
            ivec3 candidateCentre(0);
            if (!candidate.hit || candidate.component != component) continue;
            if (!pickToComponentVoxelCoord(scene, candidate, candidateCentre)) continue;
            ivec3 delta = candidateCentre - startCentre;
            int separation = std::max({ std::abs(delta.x), std::abs(delta.y), std::abs(delta.z) });
            if (separation < 4) continue;   // Too close for the gap to be worth filling.
            second = candidate;
            endCentre = candidateCentre;
            sweep = separation;
        }
        if (sweep != 0) break;
    }
    if (sweep == 0) {
        projv::core::warn("PAINTSTROKE: no sweep along the surface stayed on the component; skipped");
        return;
    }

    // --- A colour the surface is not already ---
    uint8_t startSlot = 0;
    if (!queryComponentVoxel(scene, space, startCentre, startSlot)) {
        projv::core::warn("PAINTSTROKE: the mapped seed is not solid; skipped");
        return;
    }
    uint32_t startColor = componentVoxelColor(scene, component, startSlot);
    int paintSlot = -1;
    const projv::ComponentRecord& record = scene.components[component];
    for (size_t slot = 0; slot < record.materialPalette.size(); slot++) {
        if (record.materialPalette[slot].packedColor != startColor) {
            paintSlot = int(slot);
            break;
        }
    }
    if (paintSlot < 0) {
        projv::core::warn("PAINTSTROKE: every palette entry holds the surface colour; skipped");
        return;
    }
    uint32_t paintColor = record.materialPalette[paintSlot].packedColor;

    // --- Set the tool up ---
    PaintShape savedShape = editor.paintShape;
    projv::ComponentHandle savedPalette = editor.paletteComponent;
    int savedSlot = editor.selectedMaterialSlot;
    vec3 savedCamera = editor.cameraPosition;

    editor.paintShape = PaintShape::Voxel;   // One voxel per dab, so the count *is* the path.
    editor.paletteComponent = component;
    editor.selectedMaterialSlot = paintSlot;
    editor.cameraPosition = origin;

    // The control: the two ends, and nothing between them. Read-only -- collectPaintTargets never
    // applies anything -- so this runs against the same untouched scene the stroke is about to see.
    std::vector<ivec3> endsOnly;
    std::vector<uint32_t> endsOnlyPrevious;
    bool controlTruncated = false;
    collectPaintTargets(scene, component, startCentre, first.materialSlot, first.faceNormal, editor,
                        paintColor, endsOnly, endsOnlyPrevious, controlTruncated);
    collectPaintTargets(scene, component, endCentre, second.materialSlot, second.faceNormal, editor,
                        paintColor, endsOnly, endsOnlyPrevious, controlTruncated);
    dropDuplicatePaintTargets(endsOnly, endsOnlyPrevious);

    // --- The stroke: press at one end, one move, release at the other ---
    size_t historyBefore = editor.history.entries().size();
    beginPaintStroke(editor);
    processPaintSample(scene, editor, first);
    processPaintSample(scene, editor, second);
    // Held past the end of the stroke: endPaintStroke hands these to the history, and this is the
    // same vector the undo closure will read.
    auto painted = editor.paintStrokeCoords;
    auto paintedPrevious = editor.paintStrokePreviousColors;
    endPaintStroke(scene, editor);

    if (!painted || !paintedPrevious) {
        projv::core::error("PAINTSTROKE: the stroke never opened; FAIL");
        editor.paintShape = savedShape;
        editor.paletteComponent = savedPalette;
        editor.selectedMaterialSlot = savedSlot;
        editor.cameraPosition = savedCamera;
        return;
    }

    // --- Verdict ---
    size_t entriesAdded = editor.history.entries().size() - historyBefore;

    std::unordered_set<uint64_t> seen;
    size_t duplicates = 0;
    for (const ivec3& coord : *painted) {
        if (!seen.insert(packVoxelKey(coord)).second) duplicates++;
    }

    ComponentVoxelSpace live = resolveComponentVoxelSpace(scene, component);
    size_t wrongAfterPaint = 0;
    for (const ivec3& coord : *painted) {
        uint8_t slot = 0;
        if (!live.valid || !queryComponentVoxel(scene, live, coord, slot) ||
            componentVoxelColor(scene, component, slot) != paintColor) {
            wrongAfterPaint++;
        }
    }

    bool undone = editor.history.undo();
    live = resolveComponentVoxelSpace(scene, component);
    size_t wrongAfterUndo = 0;
    for (size_t index = 0; index < painted->size(); index++) {
        uint8_t slot = 0;
        if (!live.valid || !queryComponentVoxel(scene, live, (*painted)[index], slot) ||
            componentVoxelColor(scene, component, slot) != (*paintedPrevious)[index]) {
            wrongAfterUndo++;
        }
    }

    bool interpolated = painted->size() > endsOnly.size();
    projv::core::info("SELFTEST paint stroke: comp={} sweep={} voxels painted={} (two dabs alone={}) "
                      "entries={} duplicates={} wrong-after-paint={} wrong-after-undo={}",
                      component, sweep, painted->size(), endsOnly.size(), entriesAdded, duplicates,
                      wrongAfterPaint, wrongAfterUndo);
    projv::core::info("SELFTEST paint stroke: one entry per stroke {} | no repeats {} | "
                      "colour applied {} | undo restored {} | gap filled {}",
                      entriesAdded == 1 ? "PASS" : "FAIL",
                      duplicates == 0 ? "PASS" : "FAIL",
                      wrongAfterPaint == 0 ? "PASS" : "FAIL",
                      (undone && wrongAfterUndo == 0) ? "PASS" : "FAIL",
                      interpolated ? "PASS" : "FAIL");

    editor.paintShape = savedShape;
    editor.paletteComponent = savedPalette;
    editor.selectedMaterialSlot = savedSlot;
    editor.cameraPosition = savedCamera;
    editor.history.clear();
    editor.statusMessage.clear();
}

// =============================================================================
// Application stages
// =============================================================================

void startup(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance =
        projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
    renderInstance.initialize(1600, 900, "ProjectV Scene Editor");

    // Installed before ImGui's callbacks so ImGui chains into it rather than replacing it.
    glfwSetScrollCallback(renderInstance.window, scrollCallback);

    projv::Scene& scene = projv::core::createGlobalResource<projv::Scene>(app.world);
    projv::GPUData& gpuData = projv::core::createGlobalResource<projv::GPUData>(app.world);
    EditorState& editor = projv::core::createGlobalResource<EditorState>(app.world);

    projv::RendererSpecification rendererSpecification =
        projv::graphics::loadRendererSpecification("./editorRenderer/");
    renderInstance.addRendererSpecification(1, rendererSpecification);

    bgfx::ShaderHandle vertexShader =
        projv::graphics::loadShader("./editorRenderer/editorShaders/vs_quad.bin");
    std::shared_ptr<projv::ConstructedRenderer> constructedRenderer =
        projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader);
    renderInstance.setActiveRenderer(constructedRenderer);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigDpiScaleFonts = true;   // Readable text on a scaled display, which most are now.
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // Explicit, rather than letting ImGui add a default font on first use: with no font source and
    // ImGuiBackendFlags_RendererHasTextures set (our backend does, see imgui_impl_bgfx.cpp),
    // AddFontDefault() picks between two embedded fonts by an "expected context font size"
    // heuristic. AddFontDefaultBitmap() pins it to the classic ProggyClean rather than leaving the
    // choice to that heuristic.
    io.Fonts->AddFontDefaultBitmap();

    // Glyphs bake on demand from here: with ImGuiBackendFlags_RendererHasTextures set, that is the
    // path ImGui intends, and the backend uploads each new glyph as it appears. There used to be a
    // force-bake of the whole ASCII + Latin-1 range here, to work around specific glyphs (capital I
    // and W among them) drawing as blank space. The cause was in the backend, not the bake -- it
    // created the atlas immutable, so every glyph rasterized after the first frame was silently
    // dropped on upload (see updateImGuiTexture in imgui_impl_bgfx.cpp). That preload could not have
    // fixed it in any case: it baked at LegacySize, while the interface renders at the DPI-scaled
    // size, so the size actually on screen went on baking lazily regardless.

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;
    ImGui::GetStyle().FrameRounding = 3.0f;

    ImGui_ImplGlfw_InitForOther(renderInstance.window, true);
    if (!projv::editor::initializeImGuiBgfx(EDITOR_IMGUI_VIEW_ID,
                                            "./editorRenderer/imguiShaders/vs_imgui.bin",
                                            "./editorRenderer/imguiShaders/imgui.bin")) {
        projv::core::error("The editor cannot run without its interface shaders. Exiting.");
        app.closeAppFlag = true;
        return;
    }

    // The library browses from an absolute path: it is navigated with an Up button, and walking a
    // relative path upward runs out of parents long before the filesystem does.
    {
        std::error_code libraryErrorCode;
        const char* libraryStart = std::filesystem::is_directory(DEFAULT_SCENE_DIRECTORY, libraryErrorCode)
                                 ? DEFAULT_SCENE_DIRECTORY : ".";
        setLibraryDirectory(editor, std::filesystem::absolute(libraryStart, libraryErrorCode)
                                        .lexically_normal().string());
    }

    // A scene on screen at startup beats an empty panel, but a missing default is not an error: the
    // editor is perfectly usable empty, and File > Load Scene... is the first thing in the menu.
    std::string startupScenePath = editor.scenePath.empty() ? DEFAULT_SCENE_PATH : editor.scenePath;
    if (directoryHoldsScene(startupScenePath)) {
        loadScene(scene, gpuData, editor, startupScenePath);
        if (std::getenv("EDITOR_SELFTEST")) {
            runPaintCoordSelfTest(scene);
            runSculptLatticeSelfTest(scene);
            runFillSelfTest(scene, editor);
        }
        // Separate switch: unlike the three above, these edit the scene (and undo themselves).
        if (std::getenv("EDITOR_SCULPTTEST")) {
            runSculptStrokeSelfTest(scene, editor);
            runExtrudeSelfTest(scene, editor);
            runSculptOperatorSelfTest(scene, editor);
            runPaintStrokeSelfTest(scene, editor);
        }
    } else {
        std::error_code errorCode;
        setBrowserDirectory(editor, std::filesystem::is_directory(DEFAULT_SCENE_DIRECTORY, errorCode)
                                        ? DEFAULT_SCENE_DIRECTORY : ".");
        editor.statusMessage = "No scene loaded. Use File > Load Scene...";
        projv::core::info("{}", editor.statusMessage);
    }
}

void update(projv::Application& app) {
    (void)app;
}

void render(projv::Application& app) {
    projv::graphics::RenderInstance& renderInstance =
        projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    projv::Scene& scene = projv::core::getGlobalResource<projv::Scene>(app.world);
    projv::GPUData& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
    EditorState& editor = projv::core::getGlobalResource<EditorState>(app.world);
    std::shared_ptr<projv::ConstructedRenderer> renderer = renderInstance.getActiveRenderer();

    glfwPollEvents();
    if (glfwWindowShouldClose(renderInstance.window)) {
        app.closeAppFlag = true;
        return;
    }

    // The back buffer follows the OS window; the scene's render targets follow the Viewport panel.
    // Keeping the two independent is the whole reason this loop is written out here instead of
    // calling renderConstructedRenderer.
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(renderInstance.window, &framebufferWidth, &framebufferHeight);
    static int previousFramebufferWidth = 0;
    static int previousFramebufferHeight = 0;
    if (framebufferWidth != previousFramebufferWidth || framebufferHeight != previousFramebufferHeight) {
        bgfx::reset(uint32_t(framebufferWidth), uint32_t(framebufferHeight), BGFX_RESET_NONE, bgfx::TextureFormat::Count);
        previousFramebufferWidth = framebufferWidth;
        previousFramebufferHeight = framebufferHeight;
    }
    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        return;   // Minimized: nothing to draw into.
    }

    bool cameraMoved = false;

    // The panel's size is known one frame late, and that is deliberate: the interface built below
    // hands ImGui the viewport texture's handle, so the handle must not change afterwards. Resizing
    // here — before anything reads it — means the texture ImGui is given is the one this frame's
    // scene passes render into.
    if (editor.requestedViewportWidth != editor.viewportWidth ||
        editor.requestedViewportHeight != editor.viewportHeight) {
        editor.viewportWidth = editor.requestedViewportWidth;
        editor.viewportHeight = editor.requestedViewportHeight;
        resizeViewportTargets(renderer, editor.viewportWidth, editor.viewportHeight);
        cameraMoved = true;   // The accumulated history is the wrong size now; start it again.
    }

    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    float framebufferScale = ImGui::GetIO().DisplayFramebufferScale.x;
    drawEditorInterface(app, scene, gpuData, editor, renderer, framebufferScale);
    cameraMoved |= updateCamera(renderInstance.window, editor);
    // Menu items and panel buttons can move the camera too, and they run inside the interface above.
    cameraMoved |= editor.cameraMovedByInterface;
    editor.cameraMovedByInterface = false;
    // A viewport toggle changes what the shading pass produces, so the frames already in the
    // accumulation buffer were produced under the old setting. Same treatment as a camera move.
    cameraMoved |= editor.renderSettingsChanged;
    editor.renderSettingsChanged = false;
    ImGui::Render();
    g_scrollOffsetThisFrame = 0.0;

    // Loading, in two phases. bgfx frees a destroyed texture only once the frames that might still
    // reference it have been rendered, so building the incoming scene in the same frame the outgoing
    // one is released means both are resident at once. A 3.2 GB scene reloaded that way asks an 8 GB
    // card for 6.4 GB and takes the driver down with it — hence: release, let two frames pass, then
    // build. The viewport shows its empty state for those two frames.
    if (!editor.pendingScenePath.empty()) {
        if (editor.sceneTeardownFramesRemaining > 0) {
            editor.sceneTeardownFramesRemaining--;
        } else if (editor.sceneLoaded) {
            // Validated before anything is torn down, so a bad path still cannot cost the user the
            // scene they have open.
            if (!directoryHoldsScene(editor.pendingScenePath)) {
                editor.statusMessage = "No compose.json in " + editor.pendingScenePath;
                projv::core::warn("Load failed: {}", editor.statusMessage);
                editor.pendingScenePath.clear();
            } else {
                projv::graphics::destroyGPUData(gpuData);
                editor.sceneLoaded = false;
                editor.sceneTeardownFramesRemaining = 8;
            }
        } else {
            loadScene(scene, gpuData, editor, editor.pendingScenePath);
            editor.pendingScenePath.clear();
            cameraMoved = true;
        }
    }

    // An edit that changed the palette's *size* moved every later component's palette offset, so the
    // whole GPU mirror is resynced — headers included. Colour edits never reach this: they take
    // updatePaletteEntry's single texel write while the panel is being dragged.
    if (editor.gpuFlushNeeded && editor.sceneLoaded) {
        projv::graphics::flushSceneUpdates(scene, gpuData);
        editor.gpuFlushNeeded = false;
        cameraMoved = true;
    }

    if (cameraMoved) {
        editor.frameCameraLastMovedOn = app.frameCount;
    }

    if (editor.sceneLoaded) {
        projv::core::vec3 cameraDirection;
        cameraDirection.x = projv::core::cos(editor.cameraPitch) * projv::core::cos(editor.cameraYaw);
        cameraDirection.y = projv::core::sin(editor.cameraPitch);
        cameraDirection.z = projv::core::cos(editor.cameraPitch) * projv::core::sin(editor.cameraYaw);

        projv::core::vec2 viewportResolution = { float(editor.viewportWidth), float(editor.viewportHeight) };
        projv::core::vec4 frameCount = {
            float(app.frameCount), float(cameraMoved), float(editor.frameCameraLastMovedOn), 0.0f
        };
        projv::core::vec2 texelSize = { 1.0f / viewportResolution.x, 1.0f / viewportResolution.y };
        projv::core::vec3 cameraPosition = editor.cameraPosition;
        // The viewport's icon-bar toggles, read by shade.frag. z and w are spare — the obvious next
        // things to put there are the occlusion radius and strength, which are compile-time constants
        // in the shader until something in the interface wants to drive them.
        projv::core::vec4 renderSettings = {
            editor.ambientOcclusionEnabled ? 1.0f : 0.0f,
            editor.normalShadingEnabled ? 1.0f : 0.0f,
            0.0f, 0.0f
        };

        projv::graphics::setUniformToValue(renderer, "cameraPos", cameraPosition);
        projv::graphics::setUniformToValue(renderer, "cameraDir", cameraDirection);
        projv::graphics::setUniformToValue(renderer, "windowRes", viewportResolution);
        projv::graphics::setUniformToValue(renderer, "frameCount", frameCount);
        projv::graphics::setUniformToValue(renderer, "texelSize", texelSize);
        projv::graphics::setUniformToValue(renderer, "renderSettings", renderSettings);
        projv::graphics::updateUniforms(renderer->resources.uniformHandles, renderer->resources.uniformValues);

        // Views 0..2, all rendering offscreen at the panel's resolution. The identity matrices match
        // the renderer's fullscreen-quad vertex shader, which ignores them.
        projv::core::mat4 identityMatrix = projv::core::mat4(1.0f);
        projv::graphics::performRenderPasses(false, renderer, renderInstance,
                                             editor.viewportWidth, editor.viewportHeight,
                                             identityMatrix, identityMatrix, &gpuData);
    }

    // Nothing else touches the back buffer any more — the scene renders offscreen — so the interface
    // view owns the clear.
    bgfx::setViewClear(EDITOR_IMGUI_VIEW_ID, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x14161cff, 1.0f, 0);
    bgfx::touch(EDITOR_IMGUI_VIEW_ID);
    projv::editor::renderImGuiDrawData(ImGui::GetDrawData());

    bgfx::frame();

}


void shutdown(projv::Application& app) {
    projv::GPUData& gpuData = projv::core::getGlobalResource<projv::GPUData>(app.world);
    EditorState& editor = projv::core::getGlobalResource<EditorState>(app.world);

    projv::editor::shutdownImGuiBgfx();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (editor.sceneLoaded && std::getenv("EDITOR_NO_DESTROY") == nullptr) {
        projv::graphics::destroyGPUData(gpuData);
    }

    bgfx::shutdown();
    glfwTerminate();
}

int main(int argc, char** argv) {
    projv::Application app = projv::core::createApp();

    projv::core::assignSystemStage(app, projv::SystemStage::Startup, startup);
    projv::core::assignSystemStage(app, projv::SystemStage::Update, update);
    projv::core::assignSystemStage(app, projv::SystemStage::Render, render);
    projv::core::assignSystemStage(app, projv::SystemStage::Shutdown, shutdown);

    // The scene given on the command line has to survive until startup runs, and the ECS stages take
    // only the Application — so it is parked in the editor state, which startup creates and reads.
    if (argc > 1) {
        EditorState& editor = projv::core::createGlobalResource<EditorState>(app.world);
        editor.scenePath = argv[1];
        if (!editor.scenePath.empty() && editor.scenePath.back() != '/') editor.scenePath += '/';
    }

    projv::core::runApplication(app);
    return 0;
}
