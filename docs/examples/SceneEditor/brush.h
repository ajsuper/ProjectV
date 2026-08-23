#ifndef PROJV_EDITOR_BRUSH_H
#define PROJV_EDITOR_BRUSH_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// =============================================================================
// brush — programmable brushes, and the Lua they are written in.
//
// A brush is a file. It declares what it is, what settings it wants the user to have, and what
// materials it can write; then it answers one question per voxel. That is the whole contract, and
// every part of it is deliberate:
//
//   * **The brush never touches the scene.** It is handed a voxel's context and returns a verdict.
//     Writing is the editor's job, and keeping it there is what lets the four things a brush must
//     not break stay unbroken: the stroke journal (a cell is decided once per stroke), symmetry
//     (mirrored cells go through the same journal), the dab budget, and one queue + one GPU flush
//     per frame however many voxels a stroke moved.
//   * **It is a pure function.** Same context, same parameters, same answer, with no state carried
//     between calls and no wall clock. A brush that remembered anything would churn when a stroke
//     crossed its own path, would disagree with its own mirror image, and could not be previewed
//     without applying it.
//   * **It returns a material *index*, never a colour.** See BrushMaterial below -- this is the one
//     part of the design that is forced rather than chosen.
//   * **It says up front what it needs to know.** BrushContextField is a declaration, not a set of
//     callbacks, because the expensive context (skin depth, crevice occupancy) is computed once for
//     a whole dab over a dense box and the box has to be grown before any of it is read. A brush
//     that asked per voxel would pay ~28 tree descents a cell for the same numbers.
//
// This header is the model and the loader. Nothing in it knows what a Scene is: the editor fills a
// BrushContext, calls evaluate, and does something with the verdict. That split is what makes the
// Brush Lab's preview and the Edit tab's real stroke the same evaluation with two different writers.
// =============================================================================

namespace projv::editor {

// What a brush writes, which decides how the editor drives it.
//
// Not "what it is for" -- a rock texture and a grass tint are the same kind here, because both
// recolour voxels that already exist and neither can create one. The kinds differ in exactly two
// things: which cells the editor offers the brush (only solid ones, or every cell of the dab), and
// what it does with the answer.
enum class BrushKind {
    // Recolours what is already there. Offered only solid voxels; cannot add or remove geometry.
    // The safest kind, and the one a first brush should be.
    Material,
    // Decides solidity. Offered every cell in the dab, including empty ones, and its verdict can
    // add, remove, or leave alone.
    Geometry,
    // Places whole objects at points on the surface, each one checked for room before it lands.
    // Unlike the other two it is not asked about cells at all: it is asked about *sites*, and it
    // answers with a shape rather than with one voxel's fate.
    Scatter
};

constexpr int BRUSH_KIND_COUNT = 3;

const char* brushKindName(BrushKind kind);          // "material" -- the spelling a script uses.
const char* brushKindLabel(BrushKind kind);          // "Material" -- the spelling the panel shows.
const char* brushKindHint(BrushKind kind);
bool brushKindFromName(const std::string& name, BrushKind& out);
bool brushKindIsRunnable(BrushKind kind);

// One field of per-voxel context. A brush lists the ones it reads; the editor computes those and
// nothing else, and the two expensive ones decide how far past the dab the snapshot has to reach.
enum class BrushContextField {
    Position,     // x, y, z -- the component's own voxel lattice. Coherent across the whole .data.
    World,        // wx, wy, wz -- world units. Moves with the object; Position does not.
    Material,     // slot, r, g, b, material (name) -- what is there now.
    Solid,        // solid -- whether the cell is occupied. Free; Geometry brushes always get it.
    SkinDepth,    // depth -- 0 on the surface, 1 one voxel in. Costs a margin (see maxSkinDepth).
    Crevice,      // crevice -- 0 exposed .. 1 buried, local solid fraction. Costs a margin too.
    Distance,     // distance -- 0 at the dab centre, 1 at its rim. Free.
    Normal        // nx, ny, nz -- the surface direction, from the local solid gradient.
};

constexpr int BRUSH_CONTEXT_FIELD_COUNT = 8;

const char* brushContextFieldName(BrushContextField field);
const char* brushContextFieldHint(BrushContextField field);
bool brushContextFieldFromName(const std::string& name, BrushContextField& out);

// A set of declared fields, as a bitmask. Small enough to pass by value and cheap enough to test in
// the inner loop, which is where it is tested -- once per voxel per field.
struct BrushContextMask {
    uint32_t bits = 0;

    void set(BrushContextField field) { bits |= 1u << uint32_t(field); }
    bool has(BrushContextField field) const { return (bits & (1u << uint32_t(field))) != 0; }
    bool any() const { return bits != 0; }
};

// The type of one user-facing setting, which is also which widget the Parameters panel draws.
//
// Deliberately a short list. Every entry here is a control the panel knows how to draw, a value the
// sidecar knows how to store, and a Lua value the script receives -- three things that have to agree,
// so a type is not free. Anything more exotic than these is a Float with a tooltip.
enum class BrushParamType {
    Float,     // A drag/slider. min, max, step.
    Int,       // Likewise, whole numbers.
    Bool,      // Checkbox.
    Color,     // RGB swatch. Arrives as three 0..1 numbers.
    Enum,      // One of `options`. Arrives as the option's string.
    Text,      // A short string.
    Asset,     // A path, with a folder hint. Arrives as a string.
    Seed       // An integer with a re-roll button beside it. Separate from Int because "give me a
               // different arrangement" is a distinct verb from "set this number", and a brush that
               // takes a seed wants the button.
};

constexpr int BRUSH_PARAM_TYPE_COUNT = 8;

const char* brushParamTypeName(BrushParamType type);
const char* brushParamTypeLabel(BrushParamType type);
bool brushParamTypeFromName(const std::string& name, BrushParamType& out);

// One declared setting. `name` is what the script reads it by and what the sidecar stores it under;
// everything else is for the human looking at the panel.
struct BrushParam {
    std::string name;
    std::string label;       // Falls back to `name` when the script does not give one.
    std::string tooltip;
    BrushParamType type = BrushParamType::Float;

    double minimum = 0.0;
    double maximum = 1.0;
    double step = 0.01;

    // Defaults, by type. Only the one the type calls for is read.
    double defaultNumber = 0.0;
    bool defaultBoolean = false;
    float defaultColor[3] = { 1.0f, 1.0f, 1.0f };
    std::string defaultText;

    std::vector<std::string> options;   // Enum only.

    // True for a parameter the user added in the Brush Lab rather than one the script declared. The
    // script cannot tell the difference -- it reads both out of the same table -- which is the point:
    // "define it in the script, or in the editor as data" has to mean the brush behaves identically
    // either way, or it is two features wearing one name.
    bool editorDefined = false;
};

// One setting's current value. All four slots exist rather than a union or a variant because this is
// serialised, edited by ImGui (which wants a stable address per widget), and read in an inner loop;
// a variant would cost a branch at every one of those and buy eight bytes.
struct BrushParamValue {
    double number = 0.0;
    bool boolean = false;
    float color[3] = { 1.0f, 1.0f, 1.0f };
    std::string text;
};

// One entry of a brush's declared output palette.
//
// **This is the part of the design that is forced.** A component's palette is at most 255 entries
// (MAX_MATERIALS_PER_COMPONENT, and material IDs are uint8_t), the edit queue carries colours rather
// than slots, and internMaterial adds an entry for every colour it has not seen. So a brush that
// returned free-form RGB would spend the palette -- a few thousand voxels of a noise texture is 255
// entries and then a hard failure with nothing painted.
//
// A brush therefore declares the materials it can write, the editor interns them once when a stroke
// begins, and the brush returns one of them by index. Three things fall out of that, all of them
// wanted: the palette cost of a brush is known before it runs and is its declared count; a brush can
// set glossiness and emission per voxel (a colour could not); and re-running the same brush later
// lands on the same slots, because they are interned by name.
//
// A ramp is the same thing with the entries generated: `steps` colours interpolated from `color` to
// `colorTo`, named "<name>.1" .. "<name>.N". A crack that darkens with depth wants sixteen greys and
// should not have to write them out.
//
// ---- The colour here is a *default*, not the material ----
//
// The component's palette is the single source of truth for what materials exist, what colour they
// are, and what their surface properties are -- there is one place to create and edit a material and
// it is the Palette panel. A brush does not own colours; it owns *roles*, and each role is bound to a
// palette entry (see BrushDefinition::materialBindings). Everything below is what a role is called and
// what colour to give the entry **on the one occasion it has to be created because nothing is bound
// and no entry of that name exists yet**.
//
// Which is why a brush still declares colours at all: a brush dropped into a fresh scene has to do
// something visible without a setup ritual first, and "here is a sensible starting colour" is the
// cheapest way to get there. Once the entry exists it belongs to the palette, and editing it there is
// what changes it -- the script's number stops mattering.
struct BrushMaterial {
    std::string name;
    float color[3] = { 1.0f, 1.0f, 1.0f };

    // Ramp: 0 or 1 means a single entry (and colorTo is ignored).
    int steps = 1;
    float colorTo[3] = { 0.0f, 0.0f, 0.0f };

    // The non-colour half of a palette entry, in the units the panel shows rather than the packed
    // words -- see Material in scene.h. All zero is an opaque, fully rough dielectric, which is what
    // a brush that says nothing about them means.
    float glossiness = 0.0f;
    float metallic = 0.0f;
    float transparency = 0.0f;
    float ior = 1.0f;
    float emissiveStrength = 0.0f;
    float emission[3] = { 0.0f, 0.0f, 0.0f };
    bool hasEmissionColor = false;   // False means "emit in the albedo's colour" -- the zero sentinel.
};

// One line of a brush's output log: an error from Lua, or something the script printed.
struct BrushMessage {
    bool isError = false;
    std::string text;
};

// Everything one brush file amounts to, loaded.
//
// The Lua state is per brush, not per library. A brush is reloaded on its own while the others keep
// their compiled chunks; one bad file cannot take the rest down; and a script that scribbles on its
// own globals can only reach its own.
struct BrushScript;   // Opaque: the Lua state and the registry references into it.

struct BrushDefinition {
    // --- Identity ---
    std::string id;            // Stem of the file: "cracked_rock". Unique within a library.
    std::string sourcePath;    // The .lua file.
    std::string sidecarPath;   // The .params.json beside it, whether or not it exists yet.

    // --- Declaration ---
    std::string name;          // What the library list shows. Defaults to the id.
    std::string description;
    std::string author;
    BrushKind kind = BrushKind::Material;
    BrushContextMask context;
    // How far a SkinDepth brush is willing to have measured. The snapshot grows by this much on
    // every side, so it is a cost knob and not a detail: the box is (2r + 2m + 1)^3.
    int maxSkinDepth = 8;
    // Radius of the ball Crevice is averaged over, in voxels. Same story.
    int creviceRadius = 3;

    // Material name prefixes a Scatter brush's placement is allowed to grow through, matched against
    // the *target component's* palette entry names. Empty -- the default -- means the placement must
    // land in cells that are entirely empty, which is what every scatter brush used to require.
    //
    // **This exists because "occupied" and "in the way" are not the same question, and the fit test
    // could only ask the first one.** Plant a field of grass and then try to plant trees in it and
    // every site is refused: the trunk's first voxel is inside a blade, the headroom column starts
    // inside a blade, and the brush cannot tell that from a wall. The user's report was "it isn't
    // placing anywhere", and there is nothing a script can do about it -- the refusal happens in the
    // host, after the placement comes back.
    //
    // A prefix rather than an exact name, and matched against the palette rather than against the
    // brush's own materials, because the thing being displaced belongs to whatever put it there: a
    // grass brush writes `grass.blade`, `grass.tip` and any ramp step in between, and a tree wants to
    // grow through all of them without knowing how that brush names its ramp. Bound by name for the
    // same reason material roles are (see BrushMaterialSlot): a slot index is a fact about one
    // component's palette, and a name survives being planted in a different one.
    //
    // Displacement is a real edit and is journalled like any other -- the cell's old solidity and
    // colour are remembered, so a revert or an undo puts the grass back.
    std::vector<std::string> displaces;

    std::vector<BrushParam> params;
    std::vector<BrushMaterial> materials;

    // Which palette entry each of the brush's material roles is bound to, keyed by the role's declared
    // name (a ramp's expanded "rock.3", or a colour parameter's name) and holding the *palette entry's*
    // name. Absent means "the entry called what the role is called", which is what a brush that has
    // never been rebound does.
    //
    // Bound by name rather than by slot index on purpose. A slot index is a fact about one component's
    // palette and means something else in the next one, while a name is what internMaterial already
    // matches on first -- so a binding made while working on the castle still means the right thing
    // when the same brush is taken to the terrain. It also survives the palette being reordered by a
    // removal, which renumbers every slot above it.
    std::unordered_map<std::string, std::string> materialBindings;

    // --- Live state ---
    std::vector<BrushParamValue> values;    // Parallel to `params`.
    std::vector<BrushMessage> messages;     // Cleared on reload; appended to by print() and errors.
    std::string loadError;                  // Non-empty when the file did not produce a usable brush.
    bool valuesDirty = false;               // A value changed and the sidecar has not been written.
    int64_t sourceModifiedTime = 0;         // For hot reload.
    size_t sourceSize = 0;

    std::shared_ptr<BrushScript> script;    // Null when loadError is set.

    bool usable() const { return script != nullptr && loadError.empty(); }
    // Every palette entry this brush can write, ramps expanded. This is the number that has to fit
    // in what is left of the target's palette.
    int totalMaterialSlots() const;
};

// What the editor knows about one voxel, filled per call. Plain data, and every field is set
// unconditionally cheap or gated on the declaration -- see BrushContextField.
struct BrushContext {
    int32_t x = 0, y = 0, z = 0;              // Component voxel lattice.
    float wx = 0.0f, wy = 0.0f, wz = 0.0f;    // World.
    bool solid = false;
    int slot = -1;                            // Palette slot present, or -1 for an empty cell.
    float r = 0.0f, g = 0.0f, b = 0.0f;       // Colour present.
    const char* materialName = nullptr;       // Name of the slot present, or null.
    int depth = -1;                           // Skin depth; -1 when not solid or not declared.
    float crevice = 0.0f;
    float distance = 0.0f;                    // 0 at the dab centre .. 1 at the rim.
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
};

// What a brush said about one voxel.
enum class BrushAction {
    Leave,    // nil, or false for a Material brush. Nothing happens to this cell.
    Write,    // A material index: paint it (Material) or fill it (Geometry).
    Erase     // Geometry only: false. Make the cell empty.
};

struct BrushVerdict {
    BrushAction action = BrushAction::Leave;
    int materialIndex = 0;   // 0-based into the *expanded* material list. Only for Write.
};

// One voxel of a scatter brush's placement, offset from the site it was planted on.
//
// A placement is a *list* of these rather than a shape the host understands, because there is no
// vocabulary of shapes that covers a blade of grass, a clover leaf and a tree without becoming a
// modelling language. A list of offsets covers all three and needs no vocabulary at all -- the script
// already has loops.
struct BrushPlacementVoxel {
    int32_t dx = 0, dy = 0, dz = 0;
    int materialIndex = 0;   // 0-based into the expanded material list, as BrushVerdict's is.
};

// "Is there a voxel here?", in the component's own coordinates, answered by the editor.
//
// This is the one place a brush reaches back into the scene, and it exists for the fit test: a tree
// has to know whether it has headroom, and only the scene knows. Installed per stroke by the editor
// (brushSetSolidQuery) so this header stays ignorant of what a Scene is -- the same split that lets
// the whole brush model be tested without one.
using BrushSolidQuery = std::function<bool(int32_t, int32_t, int32_t)>;

// A brush's parameter table, prepared once per stroke and reused for every voxel of it. Holding it
// open is what keeps the per-voxel cost to a call rather than a table build, and it is also the
// honest expression of the rule that parameters cannot change mid-stroke.
class BrushInvocation {
    public:
        BrushInvocation() = default;
        ~BrushInvocation();
        BrushInvocation(BrushInvocation&&) noexcept;
        BrushInvocation& operator=(BrushInvocation&&) noexcept;
        BrushInvocation(const BrushInvocation&) = delete;
        BrushInvocation& operator=(const BrushInvocation&) = delete;

        bool valid() const { return definition != nullptr; }
        // Set once the script has raised an error: the rest of the stroke is skipped rather than
        // reporting the same failure once per voxel.
        bool failed() const { return failedFlag; }
        const std::string& error() const { return errorText; }
        size_t callCount() const { return calls; }
        // How many palette entries the brush's returned index may address. Resolved once here rather
        // than re-expanded per voxel: the ramp expansion allocates, and a per-voxel allocation in the
        // one function that runs a quarter of a million times a dab is the whole cost of the brush.
        int materialSlotCount() const { return slotCount; }

    private:
        friend BrushInvocation brushBeginStroke(BrushDefinition&, std::string&);
        friend BrushVerdict brushEvaluate(BrushInvocation&, const BrushContext&);
        friend bool brushEvaluateScatter(BrushInvocation&, const BrushContext&,
                                         std::vector<BrushPlacementVoxel>&);

        BrushDefinition* definition = nullptr;
        std::shared_ptr<BrushScript> script;   // Kept alive for the stroke even across a reload.
        bool failedFlag = false;
        std::string errorText;
        size_t calls = 0;
        int slotCount = 0;
};

// A library is a folder of .lua files, loaded and watched.
//
// Definitions are held by pointer, not by value, and that is load-bearing rather than tidy: the Lab
// keeps hold of the selected brush across frames, an invocation holds one for the length of a stroke,
// and the Lua state's own hook reaches its definition to append a message. A vector of values would
// invalidate all three the moment a New Brush grew the vector.
//
// shared_ptr rather than unique_ptr, and not for shared ownership -- nothing here shares a definition.
// A library lives in EditorState, which the ECS stores in a std::any, and std::any's in-place
// constructor is SFINAE-constrained on is_copy_constructible_v as a *static* requirement even though
// it never copies. One unique_ptr member makes the whole editor state uncopyable and the resource
// impossible to create. Scene has the same scar; see its hand-written copy constructor.
struct BrushLibrary {
    std::string folder;
    std::vector<std::shared_ptr<BrushDefinition>> brushes;
    std::string folderError;   // The folder could not be read at all.

    BrushDefinition* find(const std::string& id);
    const BrushDefinition* find(const std::string& id) const;
};

// --- Loading -----------------------------------------------------------------------------------

/**
 * Loads every .lua in `folder` (non-recursive), sorted by id. Brushes that fail to load are still
 * listed, with loadError set -- a broken brush has to be visible in the Lab or there is nowhere to
 * see why it broke.
 * @param folder Directory to scan. Created if it does not exist and `create` is true.
 */
void brushLibraryLoad(BrushLibrary& library, const std::string& folder, bool create);

/**
 * Re-reads one brush from disk, preserving the values of any parameter whose name and type survived
 * the edit. That preservation is the whole feel of the Lab: a script is saved every few seconds
 * while a number is being tuned, and a reload that reset every slider would make the loop useless.
 * @return False if the file did not produce a usable brush (loadError carries why).
 */
bool brushReload(BrushDefinition& brush);

/**
 * Reloads any brush whose file has changed on disk since it was read. Cheap enough to call every
 * frame: one stat per brush.
 * @return The number reloaded.
 */
int brushLibraryPollForChanges(BrushLibrary& library);

/** Reads a brush's source off disk. Empty on failure. */
std::string brushReadSource(const BrushDefinition& brush);

/**
 * Writes `source` to the brush's file and reloads it.
 * @return False if the write failed (loadError is left alone) or the reload did.
 */
bool brushWriteSource(BrushDefinition& brush, const std::string& source);

/**
 * Creates `folder`/`id`.lua from a starter template for `kind` and appends it to the library.
 * @return The new brush, or null if the file could not be written (or the id was taken).
 */
BrushDefinition* brushCreate(BrushLibrary& library, const std::string& id, BrushKind kind,
                             const std::string& displayName, std::string& error);

/** Copies `source` to a new id within the same library, sidecar included. */
BrushDefinition* brushDuplicate(BrushLibrary& library, const BrushDefinition& source,
                                const std::string& newID, std::string& error);

/** The starter script for a kind -- what a New Brush contains before it is edited. */
std::string brushTemplateSource(BrushKind kind, const std::string& displayName);

// --- Parameters ---------------------------------------------------------------------------------

/** Resets one parameter to its declared default. */
void brushResetParam(BrushDefinition& brush, size_t index);

/** Resets every parameter. */
void brushResetParams(BrushDefinition& brush);

/**
 * Adds a parameter the script did not declare -- the "or in the editor as data" half of the design.
 * The script reads it exactly as it reads its own, so a brush can be tuned into shape in the Lab and
 * the declaration written down afterwards.
 * @return False if the name is empty or already taken.
 */
bool brushAddEditorParam(BrushDefinition& brush, const BrushParam& param);

/** Removes an editor-defined parameter. Script-declared ones cannot be removed here. */
bool brushRemoveEditorParam(BrushDefinition& brush, size_t index);

/**
 * Writes the sidecar: parameter values, and the schema of any editor-defined parameters. Called on a
 * value change (coalesced by the caller), on leaving the Lab, and on exit.
 * @return False if the file could not be written.
 */
bool brushSaveSidecar(const BrushDefinition& brush);

// --- Evaluation ---------------------------------------------------------------------------------

/**
 * Prepares `brush` for a run of voxels: builds the parameter table, resets the error latch, and
 * arms the instruction watchdog. One call per stroke (or per preview dab).
 * @param error Set when the brush cannot be run at all -- unusable, or the wrong kind.
 */
BrushInvocation brushBeginStroke(BrushDefinition& brush, std::string& error);

/**
 * One voxel. Returns Leave on any error, and latches the error so the rest of the stroke is skipped
 * rather than reporting the same thing thousands of times.
 */
BrushVerdict brushEvaluate(BrushInvocation& invocation, const BrushContext& context);

/**
 * One site, for a Scatter brush. `context` describes the surface voxel the site landed on; `out`
 * receives the placement's voxels, relative to it.
 *
 * @return True when something was planted. False for `nil`, for an empty list, and on any error --
 *         all three mean "nothing here", which is the common case for a scatter brush and not a
 *         failure.
 */
bool brushEvaluateScatter(BrushInvocation& invocation, const BrushContext& context,
                          std::vector<BrushPlacementVoxel>& out);

/**
 * Installs the fit query a scatter script reaches through `pv.solid(x, y, z)`. Cleared by passing an
 * empty function; while it is empty `pv.solid` answers false, which is the safe direction -- a fit
 * test that cannot see the scene should not report the space occupied and refuse every placement.
 */
void brushSetSolidQuery(BrushDefinition& brush, BrushSolidQuery query);

/**
 * One material role of a brush: ramps expanded, colour parameters appended, in exactly the order
 * brushEvaluate's returned indices address (1-based on the Lua side).
 *
 * Colour parameters are in this list rather than beside it because they *are* material roles -- a
 * setting that says "paint this bit with that" is the same kind of thing as a declared output, and
 * giving it its own parallel mechanism would mean two ways to bind, two ways to intern, and two
 * places for the palette ceiling to be counted. Their index is what lets a script paint with one:
 * `p.tint.index` is a value `apply` can return.
 */
struct BrushMaterialSlot {
    // The role's identity, and the key its binding is stored under: "rock.3" for a ramp step, or the
    // parameter's name. Stable across a rebinding, unlike `name`.
    std::string declaredName;
    // The palette entry this role writes: the binding if it has one, otherwise `declaredName`.
    std::string name;
    // The colour to give the entry **if it has to be created**. Once an entry of that name exists,
    // the palette's copy wins and this is not consulted -- see BrushMaterial.
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float glossiness = 0.0f;
    float metallic = 0.0f;
    float transparency = 0.0f;
    float ior = 1.0f;
    float emissiveStrength = 0.0f;
    float emission[3] = { 0.0f, 0.0f, 0.0f };
    bool hasEmissionColor = false;

    bool bound = false;      // True when materialBindings named an entry for this role.
    int paramIndex = -1;     // >= 0 when the role is a colour parameter, indexing BrushDefinition::params.
};

void brushExpandMaterials(const BrushDefinition& brush, std::vector<BrushMaterialSlot>& out);

/**
 * Points a material role at a palette entry. Pass an empty `paletteEntryName` to unbind it, which
 * returns the role to writing the entry named after itself.
 * @param declaredName The role's identity -- BrushMaterialSlot::declaredName.
 */
void brushSetMaterialBinding(BrushDefinition& brush, const std::string& declaredName,
                             const std::string& paletteEntryName);

/** The entry a role is bound to, or an empty string when it is unbound. */
std::string brushMaterialBinding(const BrushDefinition& brush, const std::string& declaredName);

// There is deliberately no "cache the resolved colour" call here. What colour a role ends up being is
// a question about the palette, and the palette is right there -- the panel looks it up live, so a
// swatch cannot go stale against an entry the user has just recoloured. The one exception is a colour
// parameter, whose resolved colour has to reach the script: the editor writes it into that param's own
// value before the stroke begins, which is where a parameter's value already lives.

// The two settings the *editor* reads off a scatter brush rather than the script: how far apart
// placements may be, and how many of the eligible sites are taken.
//
// Read by convention from parameters of these names when the brush declares them, so that a scatter
// brush's spacing is tuned by the same panel everything else is tuned by, with no second mechanism
// and no declaration naming which parameter means what. A brush that declares neither gets the
// defaults and is simply not tunable in those two respects.
extern const char* const BRUSH_SPACING_PARAM;   // "spacing", in voxels.
extern const char* const BRUSH_DENSITY_PARAM;   // "density", 0..1 of eligible sites, or per 100 voxels.

// The runaway guard. `while true do end` in a file the user is actively editing must not be able to
// hang the editor, and a brush is saved and reloaded every few seconds while it is being written, so
// this is a normal event rather than an exotic one.
//
// Measured in *time per call*, checked from a count hook. A pure instruction budget was the first
// attempt and is wrong: Lua's count hook fires on a counter that runs across the whole state, not
// per call, so a stroke of a hundred thousand honest thirty-instruction calls trips a two-million
// instruction budget exactly as reliably as one infinite loop does -- and kills the stroke at a
// random voxel. Elapsed time since the call began has no such accumulation.
//
// **Raised from 0.25s, which was measured against the wrong unit of work.** A material brush's call
// decides one voxel and is over in microseconds, and against that a quarter second is enormous. A
// scatter brush's call builds a whole object: one call to the tree brush generates a trunk, a
// branching skeleton and every leaf on it, tens of thousands of voxels at a large trunk height, and
// that is a legitimate call that simply takes longer than a tenth of a second. At 0.25 the guard
// stopped catching runaways and started setting a ceiling on how big a scatter brush's subject could
// be -- and it enforced that ceiling by killing the stroke with "it looks like an endless loop",
// which sends the reader hunting for a bug in a script that does not have one.
//
// The number to size this against is how long a person will sit through before deciding the editor
// has hung, not how long the smallest call takes. Two seconds is comfortably inside that and is still
// three orders of magnitude away from any honest per-voxel call, so `while true do end` is caught
// just as surely -- only after two seconds rather than a quarter of one, which nobody will notice
// against the reload that follows it.
//
// Note what this does and does not bound: it is per *call*, and a scatter dab makes one call per
// site, so it has never bounded the length of a dab. What bounds that is the site ceiling
// (BRUSH_MAX_SCATTER_SITES) and the dab's cell ceiling, and raising this does not touch either.
constexpr int BRUSH_HOOK_INTERVAL = 100000;     // Lua instructions between deadline checks.
constexpr double BRUSH_CALL_TIMEOUT_SECONDS = 2.0;

} // namespace projv::editor

#endif
