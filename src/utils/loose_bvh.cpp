#include "utils/loose_bvh.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <glm/gtc/quaternion.hpp>

#include "utils/voxel_math.h"

namespace projv::utils {
    bool blobContentBounds(const GeometryBlob& blob, core::ivec3& minimum, core::ivec3& maximum) {
        if (blob.contentBoundsValid) {
            minimum = blob.contentMin;
            maximum = blob.contentMax;
            return maximum.x >= minimum.x;
        }

        // Promoted from the scene editor's chunkContentBounds. An explicit stack, because the
        // Z-order accumulated down the descent is what recovers a voxel's position: positions are
        // not stored, only the path to them.
        const std::vector<uint32_t>& geometry = blob.geometry;
        const size_t nodeCount = geometry.size() / 3;
        bool any = false;
        core::ivec3 low(0), high(-1);
        struct Pending { size_t node; uint64_t cellZOrder; };
        std::vector<Pending> stack;
        if (nodeCount > 0) stack.push_back({0, 0});

        while (!stack.empty()) {
            Pending current = stack.back();
            stack.pop_back();
            if (current.node >= nodeCount) continue;
            uint64_t mask = (uint64_t(geometry[current.node * 3]) << 32) | uint64_t(geometry[current.node * 3 + 1]);
            uint32_t data3 = geometry[current.node * 3 + 2];
            if (mask == 0) continue;

            if (tree64IsLeaf(data3)) {
                for (uint32_t child = 0; child < 64; child++) {
                    // Bit 63 is Z-order 0 -- the convention the brick map writes with.
                    if ((mask & (1ull << (63 - child))) == 0) continue;
                    core::ivec3 position = reverseZOrderIndex(current.cellZOrder * 64 + child);
                    if (!any) { low = high = position; any = true; }
                    else { low = core::min(low, position); high = core::max(high, position); }
                }
                continue;
            }
            // Children are contiguous in ascending Z-order from the node's own address.
            size_t firstChild = current.node + (data3 >> 1);
            uint32_t rank = 0;
            for (uint32_t child = 0; child < 64; child++) {
                if ((mask & (1ull << (63 - child))) == 0) continue;
                stack.push_back({firstChild + rank, current.cellZOrder * 64 + child});
                rank++;
            }
        }

        blob.contentMin = low;
        blob.contentMax = high;
        blob.contentBoundsValid = true;
        minimum = low;
        maximum = high;
        return any;
    }

    bool looseChunkWorldBounds(const Scene& scene, ChunkHandle handle, core::vec3& minimum, core::vec3& maximum) {
        if (handle >= scene.chunks.size()) return false;
        const Chunk& chunk = scene.chunks[handle];
        if (!chunk.alive || chunk.header.scale <= 0.0f || chunk.header.resolution == 0) return false;

        // In the chunk's own frame, a voxel v spans [v, v + 1] * voxelSize from the corner at the
        // header position. The cube is the fallback when there is no pooled geometry to measure.
        const float voxelSize = chunk.header.scale / float(chunk.header.resolution);
        core::vec3 localLow(0.0f), localHigh(chunk.header.scale);
        if (chunk.geometryPoolIndex >= 0 && size_t(chunk.geometryPoolIndex) < scene.geometryPool.size()) {
            core::ivec3 low, high;
            if (!blobContentBounds(scene.geometryPool[chunk.geometryPoolIndex], low, high)) return false;
            localLow = core::vec3(low) * voxelSize;
            localHigh = core::vec3(high + core::ivec3(1)) * voxelSize;
        }

        const core::mat3 rotation = glm::mat3_cast(chunk.header.rotation);
        const core::vec3 position = chunk.header.position;
        minimum = core::vec3(std::numeric_limits<float>::max());
        maximum = core::vec3(std::numeric_limits<float>::lowest());
        for (int corner = 0; corner < 8; corner++) {
            core::vec3 local((corner & 1) ? localHigh.x : localLow.x,
                             (corner & 2) ? localHigh.y : localLow.y,
                             (corner & 4) ? localHigh.z : localLow.z);
            core::vec3 world = position + rotation * local;
            minimum = core::min(minimum, world);
            maximum = core::max(maximum, world);
        }
        return true;
    }

    bool rayBoxInterval(core::vec3 origin, core::vec3 inverse, core::vec3 minimum, core::vec3 maximum,
                        float& entry, float& exit) {
        core::vec3 t0 = (minimum - origin) * inverse;
        core::vec3 t1 = (maximum - origin) * inverse;
        core::vec3 lo = core::min(t0, t1), hi = core::max(t0, t1);
        entry = std::max(std::max(lo.x, lo.y), lo.z);
        exit = std::min(std::min(hi.x, hi.y), hi.z);
        if (exit < std::max(entry, 0.0f)) return false;
        entry = std::max(entry, 0.0f);
        return true;
    }

    namespace {
        struct Item { ChunkHandle chunk; core::vec3 minimum, maximum, centre; };

        float surfaceArea(core::vec3 minimum, core::vec3 maximum) {
            core::vec3 d = core::max(maximum - minimum, core::vec3(0.0f));
            return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
        }

        struct Builder {
            std::vector<Item>& items;
            LooseBVH& bvh;
            uint32_t maxLeafSize;

            // Builds [begin, end) of `items` into node `index`, which already exists.
            void build(uint32_t index, uint32_t begin, uint32_t end, int depth) {
                core::vec3 low(std::numeric_limits<float>::max()), high(std::numeric_limits<float>::lowest());
                core::vec3 centreLow = low, centreHigh = high;
                for (uint32_t i = begin; i < end; i++) {
                    low = core::min(low, items[i].minimum);
                    high = core::max(high, items[i].maximum);
                    centreLow = core::min(centreLow, items[i].centre);
                    centreHigh = core::max(centreHigh, items[i].centre);
                }
                bvh.nodes[index].minimum = low;
                bvh.nodes[index].maximum = high;

                uint32_t count = end - begin;
                // The shader's traversal stack holds 32 nodes (PJV_LOOSE_BVH_STACK), and a depth-d
                // walk needs at most d + 1; past 30, stop splitting rather than ever overflow it.
                if (count <= maxLeafSize || depth >= 30) { makeLeaf(index, begin, count); return; }

                // Binned SAH along the longest centroid axis.
                core::vec3 extent = centreHigh - centreLow;
                int axis = extent.x > extent.y ? (extent.x > extent.z ? 0 : 2) : (extent.y > extent.z ? 1 : 2);
                if (extent[axis] <= 1e-6f) { splitMedian(index, begin, end, axis, depth); return; }

                constexpr int BINS = 12;
                struct Bin { core::vec3 low{std::numeric_limits<float>::max()}, high{std::numeric_limits<float>::lowest()}; uint32_t count = 0; };
                std::array<Bin, BINS> bins{};
                auto binOf = [&](const Item& item) {
                    int b = int(float(BINS) * (item.centre[axis] - centreLow[axis]) / extent[axis]);
                    return std::clamp(b, 0, BINS - 1);
                };
                for (uint32_t i = begin; i < end; i++) {
                    Bin& bin = bins[binOf(items[i])];
                    bin.low = core::min(bin.low, items[i].minimum);
                    bin.high = core::max(bin.high, items[i].maximum);
                    bin.count++;
                }
                float bestCost = std::numeric_limits<float>::max();
                int bestSplit = -1;
                for (int split = 1; split < BINS; split++) {
                    Bin left, right;
                    for (int b = 0; b < split; b++) { left.low = core::min(left.low, bins[b].low); left.high = core::max(left.high, bins[b].high); left.count += bins[b].count; }
                    for (int b = split; b < BINS; b++) { right.low = core::min(right.low, bins[b].low); right.high = core::max(right.high, bins[b].high); right.count += bins[b].count; }
                    if (left.count == 0 || right.count == 0) continue;
                    float cost = surfaceArea(left.low, left.high) * left.count + surfaceArea(right.low, right.high) * right.count;
                    if (cost < bestCost) { bestCost = cost; bestSplit = split; }
                }
                if (bestSplit < 0) { splitMedian(index, begin, end, axis, depth); return; }

                Item* first = items.data() + begin;
                Item* middle = std::partition(first, items.data() + end, [&](const Item& item) { return binOf(item) < bestSplit; });
                uint32_t mid = uint32_t(middle - items.data());
                if (mid == begin || mid == end) { splitMedian(index, begin, end, axis, depth); return; }
                makeInterior(index, begin, mid, end, depth);
            }

            void splitMedian(uint32_t index, uint32_t begin, uint32_t end, int axis, int depth) {
                uint32_t mid = begin + (end - begin) / 2;
                std::nth_element(items.data() + begin, items.data() + mid, items.data() + end,
                                 [axis](const Item& a, const Item& b) { return a.centre[axis] < b.centre[axis]; });
                makeInterior(index, begin, mid, end, depth);
            }

            void makeInterior(uint32_t index, uint32_t begin, uint32_t mid, uint32_t end, int depth) {
                uint32_t left = uint32_t(bvh.nodes.size());
                bvh.nodes.emplace_back();
                uint32_t right = uint32_t(bvh.nodes.size());
                bvh.nodes.emplace_back();
                bvh.nodes[index].a = left;
                bvh.nodes[index].b = right;
                build(left, begin, mid, depth + 1);
                build(right, mid, end, depth + 1);
            }

            void makeLeaf(uint32_t index, uint32_t begin, uint32_t count) {
                bvh.nodes[index].a = LooseBVHNode::LEAF | uint32_t(bvh.chunks.size());
                bvh.nodes[index].b = count;
                for (uint32_t i = begin; i < begin + count; i++) {
                    bvh.chunks.push_back(items[i].chunk);
                    bvh.chunkMinimum.push_back(items[i].minimum);
                    bvh.chunkMaximum.push_back(items[i].maximum);
                }
            }
        };
    }

    LooseBVH buildLooseBVH(const Scene& scene, const std::vector<ChunkHandle>& chunks, uint32_t maxLeafSize) {
        LooseBVH bvh;
        std::vector<Item> items;
        items.reserve(chunks.size());
        for (ChunkHandle chunk : chunks) {
            Item item;
            item.chunk = chunk;
            if (!looseChunkWorldBounds(scene, chunk, item.minimum, item.maximum)) continue;
            item.centre = (item.minimum + item.maximum) * 0.5f;
            items.push_back(item);
        }
        if (items.empty()) return bvh;
        bvh.nodes.reserve(items.size() * 2);
        bvh.chunks.reserve(items.size());
        bvh.nodes.emplace_back();
        Builder builder{items, bvh, std::max(1u, maxLeafSize)};
        builder.build(0, 0, uint32_t(items.size()), 0);
        return bvh;
    }

    bool looseBVHEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("PROJV_LOOSE_BVH");
            return !(value && std::strcmp(value, "0") == 0);
        }();
        return enabled;
    }
}
