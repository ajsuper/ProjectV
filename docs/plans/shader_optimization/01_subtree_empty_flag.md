# Plan 01: Subtree-Empty Flag in Tree64 Node Data

## Objective

Add a "subtree empty" bit to the tree64 node data structure so the DDA march
can skip descent into empty subtrees without fetching child nodes. This is
expected to be the **highest-impact optimization** for sparse scenes.

## Instructions

1. **Copy** `include/pjv_utils_DDA.sc` to `include/pjv_utils_DDA_emptyflag.sc`.
   Make ALL changes only in the copy.

2. **Modify `Tree64NodeData`** (around line 55):
   - Currently: `data1` (valid mask low), `data2` (valid mask high), `data3`
     (leaf bit in bit 0, child pointer in bits 1..31).
   - Add a "subtree empty" flag. Two options — pick whichever is cleaner:
     - **Option A**: Steal bit 31 of `data3` (the MSB of the child pointer).
       Child pointer becomes 30 bits (still ~1 billion addressable nodes).
       Bit 31 = 1 means "all children empty — skip descent entirely."
     - **Option B**: Use the combination "valid mask == 0 AND leaf bit == 0"
       as an implicit empty signal. This requires no bit stealing but may
       not distinguish "empty" from "not yet loaded."
   - Use Option A unless you find a reason to prefer B.

3. **Modify `marchRayThroughTree64_DDA`** (around line 901):
   - In the descent loop (the `while (candidateNodeLevel >= 0)` loop), before
     descending into a child, check the subtree-empty bit.
   - If the bit is set, treat this node as a miss for this descent — `break`
     out of the descent loop and continue DDA stepping.
   - The check should go right after `checkZOrderInValidMasks` returns true
     and before the leaf check (or after the leaf check, before computing
     the child pointer).

4. **Do NOT modify CPU-side code.** This is a shader-only change. The
   subtree-empty bit will be 0 in existing data (no CPU writer sets it),
   so this change is a **no-op on existing scenes** — it only helps once
   the CPU build pass populates the bit (a separate future task).

5. **Important**: Ensure the change is purely additive — if the bit is 0,
   behavior must be identical to the original. This is your correctness
   invariant.

## How to test

1. Change the include in
   `docs/examples/terrain_generator/fastRenderer/pathTracerShaders/albedo.frag`
   to `#include <pjv_utils_DDA_emptyflag.sc>`.
2. Run `bash compFast.sh` from
   `docs/examples/terrain_generator/`.
3. Run `./docs/examples/terrain_generator/terrain_generator` and verify the
   image is identical to the baseline (since the bit is 0 in existing data).
4. Frame time should be ~identical to baseline (the win only comes when the
   CPU populates the bit).

## What to report back

- Which bit-stealing option you chose and why.
- The exact code change (diff or line numbers in the copy).
- Confirmation that the image is identical to baseline with existing data.
- Any issues encountered with the Metal compiler (bitfield extraction).
