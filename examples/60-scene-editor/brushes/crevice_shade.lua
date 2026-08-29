-- Crevice Shade -- paints the model's own shape.
--
-- The colour comes from the geometry rather than from a noise field: ctx.crevice is the fraction of
-- nearby cells that are solid, so it is near 0 on an exposed corner, ~0.5 on a flat wall, and near 1
-- in the bottom of a crease. Multiply a base colour down by it and every crease in the model goes
-- darker without a single hand-painted voxel.
--
-- This is the "darker in the creases" brush, and it is worth knowing what it is *not*: it is not
-- ambient occlusion from the renderer. It is measured in the voxel data, once, and stored in the
-- voxels. That makes it wrong if the model changes afterwards -- and permanent, and free at render
-- time, which for authored dirt is the better trade.
return {
    name = "Crevice Shade",
    kind = "material",
    description = "Darkens creases and lightens exposed edges, from the local solid density.",
    author = "ProjectV",

    needs = { "position", "crevice", "material" },
    -- The ball crevice is averaged over. 3 catches the corner where two walls meet; 6 starts to
    -- shade whole recesses rather than the crease in them, and costs (2r+1)^3 reads per voxel.
    creviceRadius = 3,

    params = {
        { name = "shadow", label = "Crease darkness", type = "float", default = 0.22, min = 0.0, max = 1.0,
          tooltip = "How dark the deepest crease goes, as a fraction of the base colour." },
        { name = "contrast", label = "Contrast", type = "float", default = 1.4, min = 0.2, max = 4.0,
          tooltip = "Above 1 the shading pushes towards the two ends; below 1 it flattens out." },
        { name = "speckle", label = "Speckle", type = "float", default = 0.06, min = 0.0, max = 0.5,
          tooltip = "A little per-voxel noise, which stops a large flat wall reading as one\n"
                 .. "printed colour. A tiny amount goes a long way." },
        { name = "onlyMaterial", label = "Only this material", type = "text", default = "" },
        { name = "seed", type = "seed", default = 4242 },
    },

    -- Sixteen steps rather than twelve: this brush's whole output is a gradient, and banding shows
    -- far more on smooth shading than it does through a crack pattern.
    materials = {
        { name = "shade", steps = 16, color = { 0.72, 0.70, 0.66 }, colorTo = { 0.16, 0.15, 0.14 } },
    },

    apply = function(ctx, p)
        if p.onlyMaterial ~= "" then
            if not ctx.material then return nil end
            if not string.find(ctx.material, p.onlyMaterial, 1, true) then return nil end
        end

        -- 0 exposed .. 1 buried, pushed through a contrast curve.
        local buried = pv.clamp(ctx.crevice, 0.0, 1.0)
        buried = pv.clamp((buried - 0.5) * p.contrast + 0.5, 0.0, 1.0)

        if p.speckle > 0.0 then
            local n = pv.hash(ctx.x, ctx.y, ctx.z, p.seed) - 0.5
            buried = pv.clamp(buried + n * p.speckle, 0.0, 1.0)
        end

        -- The ramp runs light to dark, so `buried` indexes it directly. The `shadow` parameter clamps
        -- how far down it is allowed to reach.
        local reach = pv.lerp(0.0, 1.0 - p.shadow, 1.0)
        return 1 + math.floor(pv.clamp(buried * reach, 0.0, 0.999) * 16)
    end,
}
