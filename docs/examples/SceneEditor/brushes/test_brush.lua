-- Test Brush -- a material brush: it recolours voxels that already exist.
--
-- apply is called once per solid voxel of the dab. Return the index of one of the materials below
-- to paint the voxel that colour, or nil to leave it alone. It cannot add or remove geometry.
return {
    name = "Test Brush",
    kind = "material",
    description = "Darkens what is buried and lightens what is exposed.",

    -- Only what is listed here is computed and handed to apply. skinDepth and crevice both cost a
    -- margin around the brush, so ask for them when you use them and not otherwise.
    needs = { "position", "skinDepth", "crevice" },
    maxSkinDepth = 4,
    creviceRadius = 3,

    params = {
        { name = "scale", label = "Grain size", type = "float",
          default = 0.2, min = 0.02, max = 1.0,
          tooltip = "Noise frequency, in voxels^-1." },
        { name = "shade", label = "Crevice darkening", type = "float",
          default = 0.7, min = 0.0, max = 1.0 },
        { name = "seed", type = "seed", default = 1 },
    },

    -- A ramp: `steps` entries interpolated from color to colorTo. Returning an index into it is how
    -- a brush shades without spending the palette.
    --
    -- The colours here are only what the entries are given **if they have to be created**. Once they
    -- exist they belong to the component's palette, and the Palette panel along the bottom is where
    -- they are recoloured, renamed and removed. Any of these roles can also be pointed at an entry
    -- that already exists: click its swatch under "What it writes", then click the entry.
    materials = {
        { name = "Test Brush", steps = 8,
          color = { 0.68, 0.66, 0.62 }, colorTo = { 0.16, 0.15, 0.14 } },
    },

    apply = function(ctx, p)
        local grain = pv.fbm(ctx.x * p.scale, ctx.y * p.scale, ctx.z * p.scale, 3, p.seed)
        -- Buried voxels and tight creases go darker; exposed faces keep the grain.
        local dark = ctx.crevice * p.shade + grain * 0.4
        if ctx.depth > 0 then dark = dark + 0.25 end
        return 1 + math.floor(pv.clamp(dark, 0.0, 0.999) * 8)
    end,
}
