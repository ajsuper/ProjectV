-- Palette Paint -- the simplest brush there is, and the one that shows what a colour setting is.
--
-- It paints one palette entry. Which entry is a setting: click the swatch beside "Paint with" in
-- Settings, then click an entry in the palette along the bottom. That is the whole gesture, and it is
-- how every colour setting in every brush works.
--
-- What a colour setting is *not* is a colour picker. There is one place a material's colour is
-- decided -- the Palette panel -- and a picker here would be a second one, holding a number the
-- palette knows nothing about. So the setting names an entry instead, and the entry's colour is
-- whatever the palette currently says it is: recolour it below and the next dab paints the new colour
-- with nothing else to change.
--
-- The script's side of it is one field. `p.paint.index` is the material index of the entry the user
-- assigned, and a material index is exactly what `apply` returns.
return {
    name = "Palette Paint",
    kind = "material",
    description = "Paints one palette entry, with a falloff and an optional material filter.",
    author = "ProjectV",

    needs = { "material", "distance", "position" },

    params = {
        -- A "color" parameter is a reference to a palette entry. Its `default` is only the colour to
        -- give the entry if it has to be created -- once it exists, the palette owns it.
        { name = "paint", label = "Paint with", type = "color", default = { 0.80, 0.24, 0.20 },
          tooltip = "Click, then click an entry in the palette below. The entry's own colour is what\n"
                 .. "gets painted -- change it there and this brush follows." },
        { name = "coverage", label = "Coverage", type = "float", default = 1.0, min = 0.0, max = 1.0,
          tooltip = "1 paints the whole dab. Below that the edge breaks up, so overlapping strokes\n"
                 .. "blend into each other instead of leaving a circle." },
        { name = "onlyMaterial", label = "Only this material", type = "text", default = "",
          tooltip = "Leave empty to paint everything. Otherwise only voxels whose current material's\n"
                 .. "name contains this text are painted." },
        { name = "seed", type = "seed", default = 17 },
    },

    -- No `materials` list at all: the colour setting above is this brush's only material role, and a
    -- role is a role however it was declared.
    materials = {},

    apply = function(ctx, p)
        if p.onlyMaterial ~= "" then
            if not ctx.material then return nil end
            if not string.find(ctx.material, p.onlyMaterial, 1, true) then return nil end
        end

        -- A dithered edge rather than a hard one. The threshold is a hash of the voxel's own
        -- coordinate, so it is the same every time this voxel is asked -- a random number here would
        -- make the edge crawl as the stroke passed back over it.
        if p.coverage < 1.0 then
            local edge = ctx.distance * ctx.distance
            if edge > p.coverage then return nil end
            if pv.hash(ctx.x, ctx.y, ctx.z, p.seed) < edge / math.max(p.coverage, 0.0001) then
                return nil
            end
        end

        return p.paint.index
    end,
}
