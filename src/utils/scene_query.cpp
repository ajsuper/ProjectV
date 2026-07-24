#include "utils/scene_query.h"

#include <algorithm>
#include <sstream>

#include "core/log.h"
#include "utils/voxel_management.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace projv::utils {
    namespace {
        std::vector<std::string> split(const std::string& s, char delim) {
            std::vector<std::string> parts;
            std::istringstream ss(s);
            std::string item;
            while (std::getline(ss, item, delim)) {
                if (!item.empty()) parts.push_back(item);
            }
            return parts;
        }

        std::string join(const std::vector<std::string>& parts, const std::string& delim) {
            std::string result;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) result += delim;
                result += parts[i];
            }
            return result;
        }

        ComponentHandle findChildByPath(const Scene& scene, ComponentHandle parent,
                                         const std::vector<std::string>& parts, size_t idx) {
            if (idx >= parts.size()) return parent;
            for (ComponentHandle child : scene.components[parent].children) {
                if (scene.components[child].name == parts[idx]) {
                    return findChildByPath(scene, child, parts, idx + 1);
                }
            }
            return INVALID_COMPONENT_HANDLE;
        }

        void rebakeSubtree(Scene& scene, ComponentHandle h, const core::mat4& world) {
            ComponentRecord& c = scene.components[h];

            if (c.kind == ComponentKind::Chunk) {
                Chunk& chunk = scene.chunks[c.chunkHandle];
                chunk.header.position = core::vec3(world * core::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                core::mat3 rotMat(world);
                chunk.header.rotation = glm::quat_cast(rotMat);
                chunk.headerDirty = true;
            } else if (c.kind == ComponentKind::Grid) {
                SceneGrid& grid = scene.grids[c.gridIndex];
                grid.origin = core::vec3(world * core::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                core::mat3 rotMat(world);
                grid.rotation = glm::quat_cast(rotMat);
                for (int32_t ci = 0; ci < static_cast<int32_t>(grid.cellToChunk.size()); ++ci) {
                    int32_t chIdx = grid.cellToChunk[ci];
                    if (chIdx < 0) continue;
                    int iz = ci / (grid.dims.x * grid.dims.y);
                    int iy = (ci / grid.dims.x) % grid.dims.y;
                    int ix = ci % grid.dims.x;
                    core::vec3 cellOffset = glm::mat3_cast(grid.rotation) *
                                            (core::vec3(ix, iy, iz) * grid.cellSize);
                    Chunk& chunk = scene.chunks[chIdx];
                    chunk.header.position = grid.origin + cellOffset;
                    chunk.header.rotation = grid.rotation;
                    chunk.headerDirty = true;
                }
            }
            // Asset or others: recurse into children.
            for (ComponentHandle child : c.children) {
                const ComponentRecord& childRec = scene.components[child];
                core::mat4 childLocal = glm::translate(core::mat4(1.0f), childRec.localPosition)
                                      * glm::mat4_cast(childRec.localRotation)
                                      * glm::scale(core::mat4(1.0f), core::vec3(childRec.localScale));
                rebakeSubtree(scene, child, world * childLocal);
            }
        }

        void listTree(const Scene& scene, ComponentHandle h, const std::string& parentPath,
                      std::vector<ComponentInfo>& out) {
            const ComponentRecord& c = scene.components[h];
            std::string fullPath = parentPath.empty() ? c.name : parentPath + "/" + c.name;
            ComponentInfo info;
            info.handle      = h;
            info.name        = c.name;
            info.fullPath    = fullPath;
            info.kind        = c.kind;
            info.sourcePath  = c.sourcePath;
            info.worldPosition = getComponentWorldPosition(scene, h);
            info.voxelCount  = getComponentVoxelCount(scene, h);
            out.push_back(info);
            for (ComponentHandle child : c.children) {
                listTree(scene, child, fullPath, out);
            }
        }
    } // anon

    std::string getComponentPath(const Scene& scene, ComponentHandle h) {
        std::vector<std::string> parts;
        while (h != INVALID_COMPONENT_HANDLE && h < scene.components.size()) {
            parts.push_back(scene.components[h].name);
            h = scene.components[h].parent;
        }
        std::reverse(parts.begin(), parts.end());
        return join(parts, "/");
    }

    ComponentHandle findComponentByPath(const Scene& scene, const std::string& path) {
        if (path.empty() || path == "/") return INVALID_COMPONENT_HANDLE;
        auto parts = split(path, '/');
        if (parts.empty()) return INVALID_COMPONENT_HANDLE;

        for (ComponentHandle h = 0; h < scene.components.size(); ++h) {
            if (scene.components[h].parent != INVALID_COMPONENT_HANDLE) continue;
            if (scene.components[h].name == parts[0]) {
                return findChildByPath(scene, h, parts, 1);
            }
        }
        return INVALID_COMPONENT_HANDLE;
    }

    std::vector<ComponentHandle> findComponentsByName(const Scene& scene,
                                                        const std::string& localName) {
        std::vector<ComponentHandle> result;
        for (ComponentHandle h = 0; h < scene.components.size(); ++h) {
            if (scene.components[h].name == localName) {
                result.push_back(h);
            }
        }
        return result;
    }

    std::vector<ComponentInfo> listComponents(const Scene& scene) {
        std::vector<ComponentInfo> result;
        for (ComponentHandle h = 0; h < scene.components.size(); ++h) {
            if (scene.components[h].parent == INVALID_COMPONENT_HANDLE) {
                listTree(scene, h, "", result);
            }
        }
        return result;
    }

    uint32_t getComponentVoxelCount(const Scene& scene, ComponentHandle h) {
        if (h >= scene.components.size()) return 0;
        const ComponentRecord& c = scene.components[h];
        if (c.kind == ComponentKind::Asset) return 0;
        if (c.kind == ComponentKind::Chunk) {
            if (c.chunkHandle >= scene.chunks.size()) return 0;
            int32_t poolIdx = scene.chunks[c.chunkHandle].geometryPoolIndex;
            if (poolIdx < 0 || static_cast<size_t>(poolIdx) >= scene.geometryPool.size()) return 0;
            return static_cast<uint32_t>(scene.geometryPool[poolIdx].materialIDs.size());
        }
        // Grid: sum voxels across all populated cells.
        uint32_t total = 0;
        int32_t gridIdx = c.gridIndex;
        if (gridIdx < 0 || static_cast<size_t>(gridIdx) >= scene.grids.size()) return 0;
        for (int32_t ci : scene.grids[gridIdx].cellToChunk) {
            if (ci < 0) continue;
            int32_t poolIdx = scene.chunks[ci].geometryPoolIndex;
            if (poolIdx < 0 || static_cast<size_t>(poolIdx) >= scene.geometryPool.size()) continue;
            total += static_cast<uint32_t>(scene.geometryPool[poolIdx].materialIDs.size());
        }
        return total;
    }

    core::mat4 getComponentWorldMatrix(const Scene& scene, ComponentHandle h) {
        core::mat4 m(1.0f);
        while (h != INVALID_COMPONENT_HANDLE && h < scene.components.size()) {
            const ComponentRecord& c = scene.components[h];
            core::mat4 local = glm::translate(core::mat4(1.0f), c.localPosition)
                             * glm::mat4_cast(c.localRotation)
                             * glm::scale(core::mat4(1.0f), core::vec3(c.localScale));
            m = local * m;
            h = c.parent;
        }
        return m;
    }

    core::vec3 getComponentWorldPosition(const Scene& scene, ComponentHandle h) {
        return core::vec3(getComponentWorldMatrix(scene, h) * core::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    }

    core::quat getComponentWorldRotation(const Scene& scene, ComponentHandle h) {
        core::mat3 rotMat(getComponentWorldMatrix(scene, h));
        return glm::quat_cast(rotMat);
    }

    void setComponentPosition(Scene& scene, ComponentHandle h, const core::vec3& localPos) {
        if (h >= scene.components.size()) return;
        setComponentTransform(scene, h, localPos, scene.components[h].localRotation);
    }

    void setComponentRotation(Scene& scene, ComponentHandle h, const core::quat& localRot) {
        if (h >= scene.components.size()) return;
        setComponentTransform(scene, h, scene.components[h].localPosition, localRot);
    }

    void setComponentTransform(Scene& scene, ComponentHandle h,
                               const core::vec3& localPos, const core::quat& localRot) {
        if (h >= scene.components.size()) return;
        ComponentRecord& c = scene.components[h];
        c.localPosition = localPos;
        c.localRotation = localRot;

        // Compute world transform for this component by walking parents.
        core::mat4 world = core::mat4(1.0f);
        ComponentHandle parent = c.parent;
        while (parent != INVALID_COMPONENT_HANDLE && parent < scene.components.size()) {
            const ComponentRecord& p = scene.components[parent];
            core::mat4 parentLocal = glm::translate(core::mat4(1.0f), p.localPosition)
                                   * glm::mat4_cast(p.localRotation)
                                   * glm::scale(core::mat4(1.0f), core::vec3(p.localScale));
            world = parentLocal * world;
            parent = p.parent;
        }

        core::mat4 myLocal = glm::translate(core::mat4(1.0f), localPos)
                           * glm::mat4_cast(localRot)
                           * glm::scale(core::mat4(1.0f), core::vec3(c.localScale));
        rebakeSubtree(scene, h, world * myLocal);
    }
}