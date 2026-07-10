#ifndef PROJV_RANGE_ALLOCATOR_H
#define PROJV_RANGE_ALLOCATOR_H

#include <cstdint>
#include <vector>

namespace projv::graphics {
    // A tiny first-fit free-list suballocator over a 1D space [0, capacity) measured in abstract
    // "units". Callers choose the unit: for the tree64 texture a unit is one RGB texel (3 uint32),
    // for the voxelType texture one RGBA texel (4 uint32), for the header texture one row. It hands
    // out contiguous [offset, offset+length) ranges and reclaims them on free, coalescing adjacent
    // free ranges so a stream of alloc/free doesn't fragment into dust. alloc() returns INVALID when
    // no single free range is large enough (the caller then grows the backing texture and re-inits).
    //
    // This is the persistent GPU layout state that createTexturesForScene used to throw away: it lets
    // a blob be added, resized, or freed without repacking every other blob. It is pure CPU
    // bookkeeping (no bgfx), so it is unit-testable headlessly.
    struct RangeAllocator {
        static constexpr uint32_t INVALID = 0xFFFFFFFFu;

        struct FreeRange { uint32_t offset; uint32_t length; };

        uint32_t capacity = 0;
        // Sorted by offset, non-overlapping and non-adjacent (always coalesced). Invariant maintained
        // by alloc()/free().
        std::vector<FreeRange> freeList;

        // Resets to a single free range covering [0, cap). Call after (re)sizing the backing texture.
        void reset(uint32_t cap) {
            capacity = cap;
            freeList.clear();
            if (cap > 0) freeList.push_back({0u, cap});
        }

        // Marks [offset, offset+length) as already in use (used to seed the allocator from an
        // existing packed layout, e.g. the bulk build in createTexturesForScene). Assumes the region
        // is currently free. length 0 is a no-op.
        void reserve(uint32_t offset, uint32_t length) {
            if (length == 0) return;
            for (size_t i = 0; i < freeList.size(); i++) {
                FreeRange& fr = freeList[i];
                if (offset >= fr.offset && offset + length <= fr.offset + fr.length) {
                    uint32_t tailOffset = offset + length;
                    uint32_t tailLength = (fr.offset + fr.length) - tailOffset;
                    uint32_t headLength = offset - fr.offset;
                    // Rewrite this free range as the head remainder, then insert the tail remainder.
                    if (headLength > 0) {
                        fr.length = headLength;
                        if (tailLength > 0) freeList.insert(freeList.begin() + i + 1, {tailOffset, tailLength});
                    } else if (tailLength > 0) {
                        fr.offset = tailOffset;
                        fr.length = tailLength;
                    } else {
                        freeList.erase(freeList.begin() + i);
                    }
                    return;
                }
            }
        }

        // Allocates length units first-fit. Returns the offset, or INVALID if there is no free range
        // large enough. A zero-length allocation returns offset 0 and touches nothing (a chunk with no
        // voxelType data reads an empty [start, start) range, so the offset is never dereferenced).
        uint32_t alloc(uint32_t length) {
            if (length == 0) return 0u;
            for (size_t i = 0; i < freeList.size(); i++) {
                if (freeList[i].length >= length) {
                    uint32_t off = freeList[i].offset;
                    freeList[i].offset += length;
                    freeList[i].length -= length;
                    if (freeList[i].length == 0) freeList.erase(freeList.begin() + i);
                    return off;
                }
            }
            return INVALID;
        }

        // Returns [offset, offset+length) to the free pool, coalescing with neighbours. length 0 is a
        // no-op (mirrors a zero-length alloc).
        void free(uint32_t offset, uint32_t length) {
            if (length == 0) return;
            size_t i = 0;
            while (i < freeList.size() && freeList[i].offset < offset) i++;
            freeList.insert(freeList.begin() + i, {offset, length});
            coalesce();
        }

        // Largest single contiguous free range (0 if full). Handy for deciding when to grow.
        uint32_t largestFreeRun() const {
            uint32_t best = 0;
            for (const FreeRange& fr : freeList) if (fr.length > best) best = fr.length;
            return best;
        }

    private:
        // Merge adjacent free ranges. Assumes freeList is sorted by offset.
        void coalesce() {
            for (size_t i = 0; i + 1 < freeList.size();) {
                if (freeList[i].offset + freeList[i].length == freeList[i + 1].offset) {
                    freeList[i].length += freeList[i + 1].length;
                    freeList.erase(freeList.begin() + i + 1);
                } else {
                    i++;
                }
            }
        }
    };
}

#endif
