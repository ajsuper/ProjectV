-- Cracked Rock -- a procedural rock texture.
--
-- Two things make this coherent rather than a per-dab pattern:
--
--   * it is a function of ctx.x/y/z, the component's own voxel lattice, so the same voxel gets the
--     same answer however many strokes reach it and whichever direction they come from; and
--   * it is a pure function -- no state, no clock -- so the second dab over a spot agrees with the
--     first, which is what stops a drag from churning.
--
-- The crack network is Worley's f2 - f1: that difference is near zero exactly on the boundary between
-- two cells, and cell boundaries in 3D are a connected sheet of thin lines through the volume. Which
-- is what a crack is.
return {
    name = "Cracked Rock",
    kind = "material",
    description = "Worley cracks that only bite near the surface, over a grain of fBm.",
    author = "ProjectV",

    -- Only these are computed. skinDepth costs a margin around the brush (maxSkinDepth on every
    -- side), so it is declared rather than assumed.
    needs = { "position", "skinDepth", "material" },
    maxSkinDepth = 6,

    params = {
        { name = "frequency", label = "Crack frequency", type = "float",
          default = 0.09, min = 0.01, max = 0.5, step = 0.005,
          tooltip = "Size of the crack cells, in voxels^-1. 0.09 is a crack every ~11 voxels." },
        { name = "width", label = "Crack width", type = "float",
          default = 0.18, min = 0.01, max = 0.6,
          tooltip = "How wide the dark line is. Past ~0.35 the cracks meet and it reads as rubble." },
        { name = "depth", label = "Crack depth", type = "int", default = 3, min = 0, max = 6,
          tooltip = "How many voxels below the surface a crack still darkens. 0 marks the skin\n"
                 .. "only, which is the honest setting for a hairline crack." },
        { name = "grain", label = "Grain", type = "float", default = 0.35, min = 0.0, max = 1.0,
          tooltip = "How much fBm mottling the unbroken rock face gets." },
        { name = "grainScale", label = "Grain size", type = "float",
          default = 0.28, min = 0.02, max = 1.0 },
        -- The material filter. Compared against the name of the palette entry already in the voxel,
        -- so "only paint the stone" is a setting rather than a second brush. Empty means everything.
        { name = "onlyMaterial", label = "Only this material", type = "text", default = "",
          tooltip = "Leave empty to paint everything. Otherwise a voxel is painted only when the\n"
                 .. "name of the material already in it contains this text -- 'stone' catches\n"
                 .. "stone_wall and darkstone alike." },
        { name = "seed", type = "seed", default = 8891 },
    },

    -- Twelve greys as one ramp: interned once when the stroke begins, and the brush picks one per
    -- voxel. Returning a colour instead would spend a palette entry per distinct value and run a
    -- 255-slot palette dry in one stroke.
    materials = {
        { name = "rock", steps = 12,
          color = { 0.60, 0.58, 0.54 }, colorTo = { 0.10, 0.095, 0.09 } },
    },

    apply = function(ctx, p)
        -- The filter first: it is the cheapest test here and it rejects most voxels on a mixed model.
        if p.onlyMaterial ~= "" then
            if not ctx.material then return nil end
            if not string.find(ctx.material, p.onlyMaterial, 1, true) then return nil end
        end

        local f1, f2 = pv.worley(ctx.x * p.frequency, ctx.y * p.frequency, ctx.z * p.frequency, p.seed)
        -- 1 on a crack, 0 away from one.
        local crack = 1.0 - pv.smoothstep(0.0, p.width, f2 - f1)

        -- A crack is a surface feature: it is darkest where it breaks the skin and fades as it goes
        -- in, which is why this brush asks for skin depth at all. Without it the cracks paint right
        -- through the interior and the model looks like it is made of cracked jelly rather than
        -- cracked rock -- and nobody sees the interior, so the palette spend is wasted too.
        if p.depth > 0 then
            crack = crack * pv.clamp(1.0 - ctx.depth / (p.depth + 1), 0.0, 1.0)
        elseif ctx.depth > 0 then
            crack = 0.0
        end

        local grain = pv.fbm(ctx.x * p.grainScale, ctx.y * p.grainScale, ctx.z * p.grainScale, 3, p.seed + 1)
        -- Cracks own the dark end of the ramp; the grain mottles the rest.
        local shade = crack * 0.85 + grain * p.grain * 0.4

        return 1 + math.floor(pv.clamp(shade, 0.0, 0.999) * 12)
    end,
}
