-- Grass Tint -- the painting half of a grass brush.
--
-- A grass brush does two things: it tints the ground green, and it plants blades and flowers standing
-- up out of it. Those are two different kinds of brush -- one recolours voxels that exist, one creates
-- them at sampled surface points -- and this is the first. The second is a scatter brush
-- (grass_tufts.lua), and the two are meant to be run over the same ground.
--
-- Only the top of the ground is tinted, which is what ctx.depth is for: grass grows on the surface,
-- and a green interior is both invisible and a waste of palette. ctx.normal keeps it off the
-- undersides of things -- moss on a ceiling is a different brush.
return {
    name = "Grass Tint",
    kind = "material",
    description = "Tints the top surface of the ground green, with dry patches.",
    author = "ProjectV",

    needs = { "position", "skinDepth", "normal", "crevice" },
    maxSkinDepth = 3,
    creviceRadius = 2,

    params = {
        -- The two colours are not settings: they are the ends of the two ramps below, and a ramp's
        -- entries live in the palette once they exist. Recolour them there -- the Palette panel is
        -- where a material's colour is decided, and a pair of pickers here would be a second answer.
        { name = "patchScale", label = "Patch size", type = "float",
          default = 0.06, min = 0.005, max = 0.4, step = 0.005,
          tooltip = "How big the lush and dry patches are, in voxels^-1. Small numbers make broad\n"
                 .. "sweeps of one or the other; large numbers make it look moth-eaten." },
        { name = "dryness", label = "Dryness", type = "float", default = 0.35, min = 0.0, max = 1.0,
          tooltip = "How much of the ground the dry colour claims." },
        { name = "depth", label = "Tint depth", type = "int", default = 1, min = 0, max = 3,
          tooltip = "How far below the surface the tint reaches. 0 is the exposed skin only." },
        { name = "upOnly", label = "Upward faces only", type = "bool", default = true,
          tooltip = "Grass grows on the top of things. Turn this off for moss, which does not care." },
        { name = "shade", label = "Shade creases", type = "bool", default = true,
          tooltip = "Darken where the ground is enclosed -- grass in a crease sits in shadow." },
        { name = "seed", type = "seed", default = 271 },
    },

    -- Two ramps, one per colour, so the brush can choose lush or dry per voxel and still shade within
    -- each. Indices 1..8 are lush, 9..16 are dry -- which is why the arithmetic below adds an offset
    -- rather than picking between two single entries.
    materials = {
        { name = "grass.lush", steps = 8, color = { 0.30, 0.56, 0.18 }, colorTo = { 0.10, 0.22, 0.07 } },
        { name = "grass.dry",  steps = 8, color = { 0.58, 0.54, 0.26 }, colorTo = { 0.24, 0.21, 0.10 } },
    },

    apply = function(ctx, p)
        if ctx.depth > p.depth then return nil end
        -- The surface has to face up. On a voxel model the normal is a gradient of the local solid
        -- mask, so it is roughly unit length on a surface voxel and near zero inside; 0.35 is loose
        -- enough to catch a shallow slope and tight enough to reject a wall.
        if p.upOnly and ctx.ny < 0.35 then return nil end

        local patch = pv.fbm(ctx.x * p.patchScale, ctx.y * p.patchScale, ctx.z * p.patchScale, 2, p.seed)
        local isDry = patch < p.dryness
        local base = isDry and 8 or 0

        -- Within the chosen ramp: a little noise for variation, plus crease shading if asked.
        local shade = pv.hash(ctx.x, ctx.y, ctx.z, p.seed + 5) * 0.45
        if p.shade then shade = shade + pv.clamp(ctx.crevice, 0.0, 1.0) * 0.5 end
        if ctx.depth > 0 then shade = shade + 0.3 end

        return 1 + base + math.floor(pv.clamp(shade, 0.0, 0.999) * 8)
    end,
}
