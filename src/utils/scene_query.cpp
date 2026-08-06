#include "utils/scene_query.h"

#include <algorithm>
#include <cassert>
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

        // Extracts (position, rotation, uniform scale) from a composed T*R*S world matrix. The scale
        // has to come out first: the columns of the upper-left 3x3 are the rotation basis vectors
        // each stretched by the accumulated scale (that is what T*R*S composes to), and quat_cast
        // expects an orthonormal matrix -- feeding it the still-scaled columns produces a wrong
        // quaternion, silently, with no assertion to catch it. Mirrors the exact same extraction
        // loadComposeFromDisk does when a compose.json leaf's baked transform is first decomposed.
        struct WorldDecomposition {
            core::vec3 position;
            core::quat rotation;
            float      scale;
        };
        WorldDecomposition decomposeWorld(const core::mat4& world) {
            core::vec3 col0(world[0]), col1(world[1]), col2(world[2]);
            float scale = glm::length(col0);
            if (scale < 1e-8f) scale = 1e-8f;   // Guard a degenerate transform rather than divide by 0.
            core::mat3 rotationBasis(col0 / scale, col1 / scale, col2 / scale);
            return WorldDecomposition{
                core::vec3(world * core::vec4(0.0f, 0.0f, 0.0f, 1.0f)),
                glm::quat_cast(rotationBasis),
                scale
            };
        }

        void rebakeSubtree(Scene& scene, ComponentHandle h, const core::mat4& world) {
            ComponentRecord& c = scene.components[h];

            if (c.kind == ComponentKind::Chunk) {
                WorldDecomposition d = decomposeWorld(world);
                Chunk& chunk = scene.chunks[c.chunkHandle];
                chunk.header.position = d.position;
                chunk.header.rotation = d.rotation;
                chunk.header.scale = chunk.nativeScale * d.scale;
                chunk.headerDirty = true;
            } else if (c.kind == ComponentKind::Grid) {
                WorldDecomposition d = decomposeWorld(world);
                SceneGrid& grid = scene.grids[c.gridIndex];
                grid.rotation = d.rotation;
                grid.cellSize = grid.nativeCellSize * d.scale;
                // **grid.origin is the corner of cell *index* zero, and the transform positions
                // absolute cell zero -- the two are the same point only while originCellCoord is
                // still (0,0,0).** Assigning d.position straight across dropped the offset that a
                // downward expansion had put there (see expandGridToInclude), which teleported every
                // cell of the grid by originCellCoord * cellSize the first time the component was
                // moved: one cell of displacement per cell the grid had ever grown downward, so a
                // component that had been extruded past its low edge jumped hundreds of voxels out
                // of the frame the moment it was dragged. The gizmo and the selection outline are
                // driven from the transform rather than from the grid, so they stayed where the user
                // put them and only the geometry left -- which is what makes it read as the object
                // vanishing rather than as it moving.
                //
                // The invariant, maintained here, by expandGridToInclude, and by the loader (which
                // starts at originCellCoord == 0, where the correction is nothing):
                //     grid.origin == worldPosition + rotation * (originCellCoord * cellSize)
                grid.origin = d.position + glm::mat3_cast(grid.rotation) *
                                           (core::vec3(grid.originCellCoord) * grid.cellSize);
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
                    chunk.header.scale = grid.cellSize;
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

    namespace {
        // Voxels in a blob = set child bits across its leaf nodes. materialIDs.size() is NOT a
        // substitute: uniform leaves store a single material byte for all their voxels.
        uint32_t blobVoxelCount(const Scene& scene, int32_t poolIdx) {
            if (poolIdx < 0 || static_cast<size_t>(poolIdx) >= scene.geometryPool.size()) return 0;
            const std::vector<uint32_t>& geom = scene.geometryPool[poolIdx].geometry;
            uint32_t total = 0;
            for (size_t n = 0, nodes = geom.size() / 3; n < nodes; ++n) {
                if (!tree64IsLeaf(geom[n * 3 + 2])) continue;
                total += static_cast<uint32_t>(__builtin_popcount(geom[n * 3 + 0]) +
                                               __builtin_popcount(geom[n * 3 + 1]));
            }
            return total;
        }
    }

    uint32_t getComponentVoxelCount(const Scene& scene, ComponentHandle h) {
        if (h >= scene.components.size()) return 0;
        const ComponentRecord& c = scene.components[h];
        if (c.kind == ComponentKind::Asset) return 0;
        if (c.kind == ComponentKind::Chunk) {
            if (c.chunkHandle >= scene.chunks.size()) return 0;
            return blobVoxelCount(scene, scene.chunks[c.chunkHandle].geometryPoolIndex);
        }
        // Grid: sum voxels across all populated cells.
        uint32_t total = 0;
        int32_t gridIdx = c.gridIndex;
        if (gridIdx < 0 || static_cast<size_t>(gridIdx) >= scene.grids.size()) return 0;
        for (int32_t ci : scene.grids[gridIdx].cellToChunk) {
            if (ci < 0) continue;
            total += blobVoxelCount(scene, scene.chunks[ci].geometryPoolIndex);
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
        const ComponentRecord& c = scene.components[h];
        setComponentTransform(scene, h, localPos, c.localRotation, c.localScale);
    }

    void setComponentRotation(Scene& scene, ComponentHandle h, const core::quat& localRot) {
        if (h >= scene.components.size()) return;
        const ComponentRecord& c = scene.components[h];
        setComponentTransform(scene, h, c.localPosition, localRot, c.localScale);
    }

    void setComponentScale(Scene& scene, ComponentHandle h, float localScale) {
        if (h >= scene.components.size()) return;
        const ComponentRecord& c = scene.components[h];
        setComponentTransform(scene, h, c.localPosition, c.localRotation, localScale);
    }

    void setComponentTransform(Scene& scene, ComponentHandle h, const core::vec3& localPos,
                               const core::quat& localRot, float localScale) {
        if (h >= scene.components.size()) return;
        ComponentRecord& c = scene.components[h];
        c.localPosition = localPos;
        c.localRotation = localRot;
        c.localScale = localScale;

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
                           * glm::scale(core::mat4(1.0f), core::vec3(localScale));
        rebakeSubtree(scene, h, world * myLocal);
    }

    namespace {
        // Where a freshly created data component's voxels will be written.
        //
        // It needs a path from the moment it exists, and a *unique* one: editing.cpp's
        // ensureDataReference dedupes DataReferences on (sourceDataPath, resolution, voxelScale), so
        // two components created back to back with the same grid settings and an empty path would
        // silently share one DataReference -- and with it, one .data destination. The component
        // handle supplies the uniqueness; handles are never recycled, because deleting a component
        // tombstones its slot rather than erasing it.
        //
        // Relative on purpose. It names a file inside the scene folder, which addComponent knows
        // nothing about. Paths that came from disk are absolute (loadComposeFromDisk resolves them),
        // so generated and loaded paths cannot collide, and a writer resolves this one against
        // whichever directory it is saving into.
        std::string makeDefaultDataPath(const std::string& name, ComponentHandle handle) {
            std::string stem;
            stem.reserve(name.size());
            for (char c : name) {
                bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_';
                stem += safe ? c : '_';
            }
            if (stem.empty()) stem = "data";
            return "data/" + stem + "-" + std::to_string(handle) + ".data";
        }
    }

    bool isValidChunkResolution(uint32_t resolution) {
        // 4 is the floor, not 1: a 1^3 chunk is a single voxel with a zero-depth tree, and it is what
        // this function's caller used to hard-code before the grid became a creation-time choice.
        // Admitting it would keep that degenerate case reachable through the front door.
        if (resolution < 4u) return false;
        if ((resolution & (resolution - 1u)) != 0u) return false;   // Not a power of two.
        // A power of four has its single set bit at an even index; 0x55555555 is exactly those bits,
        // which is what separates 4/16/64/256 from 2/8/32/128.
        return (resolution & 0x55555555u) != 0u;
    }

    ComponentHandle addComponent(Scene& scene, ComponentKind kind, const std::string& name,
                                  ComponentHandle parent, uint32_t resolution, float voxelScale) {
        if (kind == ComponentKind::Chunk) {
            if (!isValidChunkResolution(resolution)) {
                core::error("addComponent: resolution {} is not a power of four; refusing to create "
                            "a chunk whose tree64 depth the renderer would misread.", resolution);
                return INVALID_COMPONENT_HANDLE;
            }
            if (!(voxelScale > 0.0f)) {
                core::error("addComponent: voxelScale must be positive, got {}.", voxelScale);
                return INVALID_COMPONENT_HANDLE;
            }
        }

        ComponentHandle handle = static_cast<ComponentHandle>(scene.components.size());
        ComponentRecord component;
        component.kind = kind;
        component.name = name;
        component.parent = parent;
        component.localPosition = core::vec3(0.0f);
        component.localRotation = core::quat(1.0f, 0.0f, 0.0f, 0.0f);
        component.localScale = 1.0f;

        if (kind == ComponentKind::Chunk) {
            // Unique from creation, not left blank until a save invents one -- see makeDefaultDataPath.
            component.sourcePath = makeDefaultDataPath(name, handle);

            Chunk chunk;
            chunk.header.chunkID = static_cast<uint32_t>(scene.chunks.size());
            chunk.header.position = core::vec3(0.0f);
            chunk.header.voxelScale = voxelScale;
            chunk.header.resolution = resolution;
            chunk.alive = true;
            // Untransformed world size of the volume. header.scale is seeded to match and then
            // recomputed from the ancestor chain by the rebake at the end of this function.
            chunk.nativeScale = voxelScale * float(resolution);
            chunk.header.scale = chunk.nativeScale;
            chunk.componentHandle = handle;
            chunk.geometryPoolIndex = poolInsertBlob(scene, GeometryBlob{});
            scene.geometryPool[chunk.geometryPoolIndex].refCount = 1;
            scene.geometryPool[chunk.geometryPoolIndex].dirty = true;
            // The blob carries the same destination, and owns it: this is a fresh volume, not a
            // copy-on-write fork of someone else's read-only file, so the first persist writes here.
            scene.geometryPool[chunk.geometryPoolIndex].sourceDataPath = component.sourcePath;
            scene.geometryPool[chunk.geometryPoolIndex].ownsSourceFile = true;
            scene.chunks.push_back(std::move(chunk));
            component.chunkHandle = static_cast<ChunkHandle>(scene.chunks.size() - 1);
            scene.looseChunks.push_back(component.chunkHandle);
            scene.looseChunkCount = static_cast<uint32_t>(scene.looseChunks.size());

            Material defaultMat;
            defaultMat.packedColor = 0x3FFFFFFF; // white
            defaultMat.name = "default";
            component.materialPalette.push_back(defaultMat);
            component.paletteVersion = 1;
        }

        // Attach to parent if valid. Parent must be an Asset folder --
        // a Chunk or Grid leaf must never acquire children.
        if (parent != INVALID_COMPONENT_HANDLE) {
            assert(parent < scene.components.size());
            assert(scene.components[parent].kind == ComponentKind::Asset);
            scene.components[parent].children.push_back(handle);
            component.parent = parent;
        } else {
            component.parent = INVALID_COMPONENT_HANDLE;
        }

        scene.components.push_back(std::move(component));

        // Re-bake the new component so its world transform is correct. getComponentWorldMatrix
        // includes `handle`'s own local transform, which is the whole point: rebakeSubtree bakes the
        // matrix it is given onto the node itself, so handing it the *parent's* world drops the
        // node's own placement. That is harmless here -- a component is created at identity and
        // setComponentTransform follows -- and it was not harmless in duplicateComponent, which
        // copies the source's transform in.
        rebakeSubtree(scene, handle, getComponentWorldMatrix(scene, handle));

        return handle;
    }

    ComponentHandle duplicateComponent(Scene& scene, ComponentHandle source,
                                        ComponentHandle parent) {
        if (source >= scene.components.size()) return INVALID_COMPONENT_HANDLE;

        ComponentHandle handle = static_cast<ComponentHandle>(scene.components.size());

        // **Everything the source is read for is read here, into values, and no reference to it
        // outlives this block.**
        //
        // `scene.components` is a vector and this function appends to it below -- which can move the
        // buffer and invalidate every reference into it. The recursion at the bottom needs the
        // source's kind and child list *after* that append, and it used to reach them through a
        // reference taken up here. That is a use-after-free, and a quiet one: a freed block usually
        // still holds the old bytes, so it read correctly whenever the vector had spare capacity
        // (most of the time) and whenever it did not (the rest of the time, mostly). Duplicating a
        // nested asset -- a temple holding a pillar, which is what the contents list is made of once
        // anything has been moved into anything -- is the case that walks furthest down it.
        //
        // Scoping the reference is what makes the mistake unavailable rather than merely absent: a
        // later `src.` after the append does not compile.
        ComponentKind srcKind;
        ChunkHandle srcChunkHandle;
        std::vector<ComponentHandle> srcChildren;
        ComponentRecord component;
        {
            const ComponentRecord& src = scene.components[source];
            srcKind = src.kind;
            srcChunkHandle = src.chunkHandle;
            srcChildren = src.children;

            component.kind = src.kind;
            component.name = src.name + " (#" + std::to_string(handle) + ")";
            component.localPosition = src.localPosition;
            component.localRotation = src.localRotation;
            component.localScale = src.localScale;
            component.sourcePath = src.sourcePath;
            component.materialPalette = src.materialPalette;
            component.paletteVersion = src.paletteVersion;
            // Both are part of what the record *is*, and both were being dropped. Without `op` a
            // duplicated child of a boolean stack came back as a plain placement -- the copy of a
            // subtracted window filled the hole it was cut from. Without `externalSource` a
            // duplicated link quietly became a copy, which is the one distinction the two modes
            // exist to make.
            component.op = src.op;
            component.externalSource = src.externalSource;
        }

        if (srcKind == ComponentKind::Chunk) {
            Chunk chunk;
            const Chunk& srcChunk = scene.chunks[srcChunkHandle];
            chunk.header = srcChunk.header;
            chunk.header.chunkID = static_cast<uint32_t>(scene.chunks.size());
            chunk.nativeScale = srcChunk.nativeScale;
            chunk.alive = true;
            chunk.componentHandle = handle;

            // Fork the geometry blob (shared until the copy is edited).
            //
            // The increment belongs only to the sharing branch. forkBlob has a sole-owner fast path
            // that returns the source index without copying -- that is a genuine second owner and
            // has to be counted -- but when it does copy, the fresh blob already carries refCount 1
            // for this chunk, and counting it again pinned the blob for the life of the scene.
            int32_t forkedIdx = forkBlob(scene, srcChunk.geometryPoolIndex);
            if (forkedIdx >= 0 && forkedIdx == srcChunk.geometryPoolIndex) {
                scene.geometryPool[forkedIdx].refCount++;
            }
            chunk.geometryPoolIndex = forkedIdx;

            scene.chunks.push_back(std::move(chunk));
            component.chunkHandle = static_cast<ChunkHandle>(scene.chunks.size() - 1);
            scene.looseChunks.push_back(component.chunkHandle);
            scene.looseChunkCount = static_cast<uint32_t>(scene.looseChunks.size());
        }

        if (srcKind == ComponentKind::Asset) {
            component.children.reserve(srcChildren.size());
        }

        scene.components.push_back(std::move(component));

        // Attach to parent. Parent must be an Asset folder.
        if (parent != INVALID_COMPONENT_HANDLE) {
            assert(parent < scene.components.size());
            assert(scene.components[parent].kind == ComponentKind::Asset);
            scene.components[parent].children.push_back(handle);
            scene.components[handle].parent = parent;
        } else {
            scene.components[handle].parent = INVALID_COMPONENT_HANDLE;
        }

        // Recursively duplicate children (only for Asset kind). Driven from the copy taken above --
        // both because the reference it used to come from is invalid by now, and because each
        // recursion appends to scene.components again, so a live view of the source's child list
        // would be walked while the container it lives in is repeatedly moved.
        if (srcKind == ComponentKind::Asset) {
            for (ComponentHandle child : srcChildren) {
                duplicateComponent(scene, child, handle);
            }
        }

        // Re-bake, **including the duplicate's own local transform**.
        //
        // This walked the parent chain and stopped there, so the matrix baked onto the copy was its
        // parent's world rather than its own. A duplicate carries the source's localPosition, so the
        // copy's chunk headers were written at the parent's origin instead: its transform fields
        // agreed with the original while its geometry -- and the selection outline and gizmo pivot,
        // which are derived from the chunk headers -- sat somewhere else entirely. Duplicate a
        // component that was any distance from its parent and the copy was simply not where you were
        // looking.
        //
        // The same mistake, at a third site, as the one setComponentParent had. getComponentWorldMatrix
        // is the form that cannot make it: it ends at `handle` inclusive.
        rebakeSubtree(scene, handle, getComponentWorldMatrix(scene, handle));

        return handle;
    }

    bool setComponentParent(Scene& scene, ComponentHandle child, ComponentHandle newParent) {
        if (child >= scene.components.size()) return false;
        if (child == newParent) return false;

        // Reject cycles: newParent must not be a descendant of child.
        ComponentHandle walker = newParent;
        while (walker != INVALID_COMPONENT_HANDLE && walker < scene.components.size()) {
            if (walker == child) return false;
            walker = scene.components[walker].parent;
        }

        ComponentRecord& comp = scene.components[child];

        // Remove from old parent.
        ComponentHandle oldParent = comp.parent;
        if (oldParent < scene.components.size()) {
            std::vector<ComponentHandle>& siblings = scene.components[oldParent].children;
            siblings.erase(std::remove(siblings.begin(), siblings.end(), child), siblings.end());
        }

        // Attach to new parent. A Chunk/Grid leaf must never acquire children.
        if (newParent != INVALID_COMPONENT_HANDLE) {
            assert(newParent < scene.components.size());
            assert(scene.components[newParent].kind == ComponentKind::Asset);
            scene.components[newParent].children.push_back(child);
            comp.parent = newParent;
        } else {
            comp.parent = INVALID_COMPONENT_HANDLE;
        }

        // Re-bake the moved subtree.
        core::mat4 world = core::mat4(1.0f);
        ComponentHandle p = comp.parent;
        while (p != INVALID_COMPONENT_HANDLE && p < scene.components.size()) {
            const ComponentRecord& pc = scene.components[p];
            core::mat4 local = glm::translate(core::mat4(1.0f), pc.localPosition)
                             * glm::mat4_cast(pc.localRotation)
                             * glm::scale(core::mat4(1.0f), core::vec3(pc.localScale));
            world = local * world;
            p = pc.parent;
        }
        // The loop above accumulates the *ancestors'* transforms, and rebakeSubtree takes the world
        // matrix of the node it is handed rather than of that node's parent. So the moved
        // component's own local transform has to be applied here -- without it, a reparent silently
        // snaps the component to its new parent's origin and discards a placement the user set
        // deliberately. Every descendant was already correct: rebakeSubtree multiplies in each
        // child's local as it recurses. It was only the root of the moved subtree that lost its own.
        core::mat4 childLocal = glm::translate(core::mat4(1.0f), comp.localPosition)
                              * glm::mat4_cast(comp.localRotation)
                              * glm::scale(core::mat4(1.0f), core::vec3(comp.localScale));
        rebakeSubtree(scene, child, world * childLocal);

        return true;
    }
}