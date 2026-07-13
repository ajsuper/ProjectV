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

    // Set the LOCAL position. Does NOT affect children directly — children
    // keep their local transforms; world transforms of the subtree change.
    void setComponentPosition(Scene& scene, ComponentHandle h, const core::vec3& localPos);

    void setComponentRotation(Scene& scene, ComponentHandle h, const core::quat& localRot);

    // Set both position and rotation in one call (avoids double rebake).
    void setComponentTransform(Scene& scene, ComponentHandle h,
                               const core::vec3& localPos, const core::quat& localRot);
}

#endif