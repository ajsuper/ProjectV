-- Cracked Stone -- a geometry brush: it opens the cracks rather than drawing them.
--
-- The difference from Cracked Rock is what the two can do about a crack. A material brush can only
-- paint one; this one can *open* it, because a geometry brush is offered every cell of the dab --
-- empty ones included -- and its answer decides whether the cell is solid.
--
--   return an index -> fill the cell with that material
--   return false    -> empty the cell
--   return nil      -> leave it exactly as it is
--
-- Note what that means for a brush like this: it must return nil for the cells it does not care
-- about, not false. Returning false everywhere the pattern is absent would not crack the surface, it
-- would delete the whole dab.
--
-- ---- Why there is only one Worley term here ----
--
-- Worley gives two useful numbers from one evaluation, and they are very different shapes:
--
--   f1        distance to the nearest feature point -- small near a cell's *centre*, so thresholding
--             it carves a **round blob per cell**. A field of circles.
--   f2 - f1   how close the two nearest feature points are to equal -- near zero exactly on the
--             boundary between two cells, which in 3D is a connected sheet of thin lines. A crack.
--
-- This brush used to take max(pit, crack) of both, and the f1 term is what put a circle inside every
-- crack cell. Only the boundary term is a crack, so only the boundary term is here.
return {
    name = "Cracked Stone",
    kind = "geometry",
    description = "Opens a network of cracks into a surface, and colours the fresh faces.",
    author = "ProjectV",

    -- `solid` is always present for a geometry brush; it is listed anyway because a declaration that
    -- says what it reads is easier to read back than one that relies on a default.
    needs = { "position", "solid", "skinDepth", "distance", "material" },
    maxSkinDepth = 8,

    params = {
        { name = "frequency", label = "Crack frequency", type = "float",
          default = 0.09, min = 0.02, max = 0.6, step = 0.005,
          tooltip = "Size of the crack cells, in voxels^-1. 0.09 is a crack every ~11 voxels." },
        { name = "width", label = "Crack width", type = "float",
          default = 0.10, min = 0.01, max = 0.5,
          tooltip = "How wide the opened line is. Past ~0.3 the cracks meet and the surface breaks\n"
                 .. "up into loose blocks." },
        { name = "maxDepth", label = "Crack depth", type = "int", default = 3, min = 1, max = 8,
          tooltip = "How many voxels the deepest part of a crack cuts. The cut tapers off towards\n"
                 .. "the edges of the line, so a crack has a V section rather than a slot." },
        { name = "falloff", label = "Soft edge", type = "float", default = 0.4, min = 0.0, max = 1.0,
          tooltip = "How much the effect fades towards the rim of the dab, so a stroke blends into\n"
                 .. "the untouched surface instead of ending on a circle." },
        { name = "recolour", label = "Colour fresh faces", type = "bool", default = true,
          tooltip = "A broken surface is a different colour from a weathered one. With this off the\n"
                 .. "brush only removes, and what is underneath keeps its own colour." },
        -- The material filter. Compared against the name of the palette entry already in the voxel, so
        -- "crack the stone and leave the timber" is a setting rather than a second brush.
        { name = "onlyMaterial", label = "Only this material", type = "text", default = "",
          tooltip = "Leave empty to crack everything. Otherwise a voxel is cut only when the name of\n"
                 .. "the material already in it contains this text -- 'stone' catches stone_wall and\n"
                 .. "darkstone alike.\n\n"
                 .. "It gates the whole brush, not just the cutting: the fresh faces it colours are\n"
                 .. "voxels of the same filtered material, so a crack cannot run out of the stone and\n"
                 .. "repaint the beam beside it." },
        { name = "seed", type = "seed", default = 606 },
    },

    materials = {
        { name = "stone.fresh", steps = 6, color = { 0.55, 0.53, 0.50 }, colorTo = { 0.30, 0.29, 0.27 } },
    },

    apply = function(ctx, p)
        -- Only ever eat into what is already there: a crack in mid-air is nothing, and returning
        -- false for empty cells would queue a removal per cell for no change.
        if not ctx.solid then return nil end
        if ctx.depth > p.maxDepth then return nil end

        -- The filter, before the noise: it is the cheapest test here and on a mixed model it rejects
        -- most cells. Note that this gates *both* halves of the brush -- a cell that is not the chosen
        -- material is neither cut nor coloured -- which is what stops a crack that starts in stone
        -- from carving on through the timber it runs into.
        if p.onlyMaterial ~= "" then
            if not ctx.material then return nil end
            if not string.find(ctx.material, p.onlyMaterial, 1, true) then return nil end
        end

        local f1, f2 = pv.worley(ctx.x * p.frequency, ctx.y * p.frequency, ctx.z * p.frequency, p.seed)
        -- 1 along the middle of a crack, falling to 0 at its edges. f1 is deliberately unused.
        local line = 1.0 - pv.smoothstep(0.0, p.width, f2 - f1)
        if line <= 0.0 then return nil end

        -- Softened towards the rim of the dab, then turned into a depth. The cut being proportional
        -- to `line` is what gives the crack a V section: deepest along its middle, shallowing out to
        -- nothing at its edges, instead of a slot with vertical walls.
        local reach = line * (1.0 - ctx.distance * p.falloff)
        local cut = reach * p.maxDepth

        if ctx.depth < cut then return false end
        -- The voxel just under the cut is the wall of the crack -- the face that is now exposed, and
        -- the only thing this brush paints.
        if p.recolour and ctx.depth < cut + 1.0 then
            return 1 + math.floor(pv.clamp(reach, 0.0, 0.999) * 6)
        end
        return nil
    end,
}
