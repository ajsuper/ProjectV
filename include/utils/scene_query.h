#ifndef PROJECTV_SCENE_QUERY_H
#define PROJECTV_SCENE_QUERY_H

#include <vector>
#include <string>
#include <cstdint>

#include "data_structures/scene.h"

namespace projv::utils {

    // --- Path-based lookup ---

    // Walk parents to build the full path: "bird_A/wing/leftWing"
    std::string getComponentPath(const Scene& scene, ComponentHandle h);

    // Parse a "/"-separated path and walk the tree to find the component.
    // Returns INVALID_COMPONENT_HANDLE if not found.
    ComponentHandle findComponentByPath(const Scene& scene, const std::string& path);

    // Find all components whose local name matches (fast linear scan).
    std::vector<ComponentHandle> findComponentsByName(const Scene& scene,
                                                       const std::string& localName);

    // --- Enumeration ---

    struct ComponentInfo {
        ComponentHandle handle;
        std::string     name;
        std::string     fullPath;
        ComponentKind   kind;
        std::string     sourcePath;
        core::vec3      worldPosition;
        uint32_t        voxelCount;  // 0 for Asset components
    };

    // List all components in tree order (depth-first), with metadata.
    std::vector<ComponentInfo> listComponents(const Scene& scene);

    // --- Per-component queries ---

    uint32_t getComponentVoxelCount(const Scene& scene, ComponentHandle h);

    // --- World transform computation ---

    core::mat4 getComponentWorldMatrix(const Scene& scene, ComponentHandle h);

    core::vec3 getComponentWorldPosition(const Scene& scene, ComponentHandle h);

    core::quat getComponentWorldRotation(const Scene& scene, ComponentHandle h);

    // --- Transform mutation ---
    //
    // All four take effect immediately: they rebake this component's whole subtree (world-space
    // position/rotation/scale of every Chunk/Grid it reaches), no separate "apply" step. Scale is
    // uniform only (v0.0) -- matches ComponentRecord::localScale and the compose.json loader, which
    // rejects a non-uniform scale on a data leaf outright.

    // Set the LOCAL position. Does NOT affect children directly — children
    // keep their local transforms; world transforms of the subtree change.
    void setComponentPosition(Scene& scene, ComponentHandle h, const core::vec3& localPos);

    void setComponentRotation(Scene& scene, ComponentHandle h, const core::quat& localRot);

    void setComponentScale(Scene& scene, ComponentHandle h, float localScale);

// Set position, rotation, and scale in one call (avoids a rebake per field).
    void setComponentTransform(Scene& scene, ComponentHandle h, const core::vec3& localPos,
                                const core::quat& localRot, float localScale);

    // --- Component lifecycle ---

    // Creates a new component. For kind==Chunk, allocates an empty chunk+blob and registers it.
    // For kind==Asset, creates a folder node only. Returns the handle of the new component, or
    // INVALID_COMPONENT_HANDLE on failure. If parent is valid and its kind == Asset, the new
    // component is attached as a child; otherwise it is a root.
    //
    // `resolution` and `voxelScale` describe the voxel grid of a kind==Chunk component, and are
    // ignored for kind==Asset. Neither is defaulted, deliberately: both are fixed for the lifetime of
    // the component, and a wrong default would be baked in silently.
    //
    // Resolution must be a power of FOUR (4, 16, 64, 256, 1024, ...), not merely a power of two --
    // the tree64 depth is derived from it exactly as the shader derives it, so anything else builds a
    // tree the renderer misreads. It also participates in blob identity (DataReference is keyed on
    // path + resolution + voxelScale), and together with voxelScale it fixes the component's
    // untransformed world size as nativeScale = voxelScale * resolution. Changing either after voxels
    // exist is a resample of the whole volume rather than an edit, which is why neither has a setter.
    //
    // Returns INVALID_COMPONENT_HANDLE for a Chunk whose resolution is not a power of four or whose
    // voxelScale is not positive.
    ComponentHandle addComponent(Scene& scene, ComponentKind kind, const std::string& name,
                                  ComponentHandle parent, uint32_t resolution, float voxelScale);

    // True for 4, 16, 64, 256, ... -- exactly the resolutions addComponent accepts. Exposed so a UI
    // can reject a bad entry up front instead of discovering it through a failed creation.
    bool isValidChunkResolution(uint32_t resolution);

    // Deep-copies a component and its entire subtree. For data components the geometry blob is
    // forked (shared until the copy is edited). Returns the handle of the top-level copy, or
    // INVALID_COMPONENT_HANDLE on failure. The copy is a root unless `parent` is given.
    ComponentHandle duplicateComponent(Scene& scene, ComponentHandle source,
                                        ComponentHandle parent = INVALID_COMPONENT_HANDLE);

    // Moves `child` from its current parent to `newParent`. Pass INVALID_COMPONENT_HANDLE to
    // make it a root. Re-bakes the subtree world transforms. `newParent` must not be a descendant
    // of `child` (checked; returns false on cycle).
    bool setComponentParent(Scene& scene, ComponentHandle child, ComponentHandle newParent);
}

#endif