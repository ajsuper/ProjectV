-- Grass Tufts -- a scatter brush: blades and flowers standing up out of the ground.
--
-- A scatter brush is asked about **sites**, not cells. The editor picks them -- one per cell of a
-- lattice fixed to the model, so sweeping back over ground you have already planted does not plant it
-- again -- and `apply` answers with the object to grow there, as a list of voxels offset from the
-- site:
--
--     return { {0,1,0, 1}, {0,2,0, 1}, {0,3,0, 2} }     -- dx, dy, dz, material
--
-- ...or nil to grow nothing, which is the ordinary answer.
--
-- Two settings are read by the editor rather than by this script, by name: `spacing` (how far apart
-- sites are) and `density` (what fraction of them are taken).
--
-- **The whole placement is checked for room before any of it lands.** If a single voxel of it would
-- grow through something, none of it is planted -- which is why a wide flower head simply does not
-- appear against a wall, rather than appearing with half its petals inside it.

-- Material roles, by index into `materials` below. Named because `{0,3,0, 9}` is unreadable and
-- `{0,3,0, POPPY_PETAL}` is not, and because inserting a material into the list would otherwise mean
-- renumbering every literal in the file.
local BLADE_BASE = 1     -- ...through BLADE_BASE + 3, the four steps of the blade ramp.
local STEM       = 5
local CLOVER     = 6
local DAISY      = 7
local DAISY_EYE  = 8
local POPPY      = 9
local POPPY_EYE  = 10
local BUTTERCUP  = 11
local BLUEBELL   = 12
local PUFF       = 13
local DANDELION  = 14

-- How far the voxel at height `i` of a stalk `height` tall is pushed along the lean direction.
--
-- **Quadratic, not linear.** A blade of grass leaves the ground vertical and bends more the higher it
-- goes; a constant lean gives a straight stick at an angle, which is what this brush used to grow and
-- what reads as a field of spikes. Squaring `t` puts almost all the movement in the top third, so the
-- bottom of the blade stands up and the tip curls over.
local function arcOffset(i, height, arc)
    if height <= 1 then return 0 end
    local t = (i - 1) / (height - 1)
    return math.floor(t * t * arc + 0.5)
end

-- Grows a stalk, returning the tip's offset so a flower head can be put on it.
--
-- Every voxel between one height and the next is filled in when the arc moves by more than one cell,
-- which happens near the tip of a strongly curved blade. Without it the blade comes apart into a
-- dotted line of floating voxels -- and floating voxels in a voxel model do not read as motion blur,
-- they read as a bug.
local function growStalk(voxels, height, arc, dirX, dirZ, material, ramp)
    local previousX, previousZ = 0, 0
    local x, z = 0, 0
    for i = 1, height do
        local reach = arcOffset(i, height, arc)
        x = math.floor(dirX * reach + 0.5)
        z = math.floor(dirZ * reach + 0.5)

        -- Which step of the ramp this height sits on, dark at the root and pale at the tip.
        local shade = material
        if ramp then
            local t = (height > 1) and ((i - 1) / (height - 1)) or 0
            shade = BLADE_BASE + math.min(3, math.floor(t * 4))
        end

        if x == previousX and z == previousZ then
            voxels[#voxels + 1] = { x, i, z, shade }
        else
            -- Walk from where the stalk was to where it now is, one axis at a time, so the bend is a
            -- connected elbow rather than a diagonal jump.
            local cx, cz = previousX, previousZ
            while cx ~= x do
                cx = cx + ((x > cx) and 1 or -1)
                voxels[#voxels + 1] = { cx, i, cz, shade }
            end
            while cz ~= z do
                cz = cz + ((z > cz) and 1 or -1)
                voxels[#voxels + 1] = { cx, i, cz, shade }
            end
        end
        previousX, previousZ = x, z
    end
    return x, height, z
end

return {
    name = "Grass Tufts",
    kind = "scatter",
    description = "Blades that curl over, and a meadow's worth of flowers to mix into them.",
    author = "ProjectV",

    needs = { "position", "normal", "crevice", "material" },
    creviceRadius = 2,

    params = {
        -- `spacing` and `density` are the editor's, read by name.
        { name = "spacing", label = "Spacing", type = "int", default = 2, min = 1, max = 32,
          tooltip = "How far apart plants are, in voxels. One per cell of a lattice this size -- and\n"
                 .. "the lattice belongs to the model, not to the brush, so a second pass over the\n"
                 .. "same ground plants nothing new." },
        { name = "density", label = "Coverage", type = "float", default = 0.55, min = 0.0, max = 1.0,
          tooltip = "What fraction of those cells get a plant." },

        -- What may grow. Tick any combination: a site picks from whatever is on.
        { name = "blades", label = "Blades", type = "bool", default = true },
        { name = "clover", label = "Clover", type = "bool", default = true,
          tooltip = "A flat rosette on the ground. The only one with no stalk, so it is the one that\n"
                 .. "still fits under a low ceiling." },
        { name = "daisies", label = "Daisies", type = "bool", default = true,
          tooltip = "White petals round a yellow eye, on a short stalk." },
        { name = "poppies", label = "Poppies", type = "bool", default = false,
          tooltip = "Tall, red, and wide at the head -- the first to be refused for want of room." },
        { name = "buttercups", label = "Buttercups", type = "bool", default = false,
          tooltip = "One or three yellow voxels on a short stalk. The cheapest flower here." },
        { name = "bluebells", label = "Bluebells", type = "bool", default = false,
          tooltip = "Bells hanging down one side of a leaning stalk." },
        { name = "dandelions", label = "Dandelions", type = "bool", default = false,
          tooltip = "Half of them the yellow flower, half the seed head." },

        { name = "flowerChance", label = "Flowers among the grass", type = "float",
          default = 0.18, min = 0.0, max = 1.0,
          tooltip = "How often a site takes a flower rather than a blade, when blades are on. At 1\n"
                 .. "every site is a flower; at 0 the flowers you have ticked never appear." },

        { name = "height", label = "Blade height", type = "int", default = 4, min = 1, max = 16 },
        { name = "curve", label = "Curve", type = "float", default = 0.55, min = 0.0, max = 1.0,
          tooltip = "How far a blade curls over. The bend is quadratic, so the blade stands up out of\n"
                 .. "the ground and does its leaning near the tip. 0 is a field of spikes." },

        { name = "maxSlope", label = "Steepest ground", type = "float", default = 0.55, min = 0.0, max = 1.0,
          tooltip = "Nothing is planted where the surface is steeper than this. 0 is level ground\n"
                 .. "only; 1 plants on walls and ceilings." },
        { name = "enclosure", label = "Skip ground more enclosed than", type = "float",
          default = 0.72, min = 0.5, max = 1.0,
          tooltip = "Skips ground that is boxed in -- the inside of a corner is where grass is worn\n"
                 .. "away, not where it is thickest.\n\n"
                 .. "`crevice` is the fraction of nearby cells that are solid, and **flat open ground\n"
                 .. "already reads about 0.6** -- half the ball around a surface voxel is the ground\n"
                 .. "it stands on. A threshold below that rejects the field you were aiming at." },
        { name = "seed", type = "seed", default = 99 },
    },

    -- Index 1..4 is the blade ramp, root to tip; 5 onward are the named roles at the top of this file.
    -- Any of them can be pointed at an entry that already exists in the model's palette: click its
    -- swatch under "What it writes", then click the entry.
    materials = {
        { name = "grass.blade", steps = 4, color = { 0.16, 0.34, 0.10 }, colorTo = { 0.48, 0.70, 0.26 } },
        { name = "grass.stem",      color = { 0.24, 0.44, 0.16 } },
        { name = "clover.leaf",     color = { 0.28, 0.55, 0.22 } },
        { name = "daisy.petal",     color = { 0.95, 0.95, 0.92 } },
        { name = "daisy.eye",       color = { 0.96, 0.82, 0.25 } },
        { name = "poppy.petal",     color = { 0.78, 0.13, 0.11 } },
        { name = "poppy.eye",       color = { 0.10, 0.09, 0.09 } },
        { name = "buttercup",       color = { 0.98, 0.85, 0.20 } },
        { name = "bluebell",        color = { 0.38, 0.34, 0.80 } },
        { name = "dandelion.puff",  color = { 0.91, 0.91, 0.87 } },
        { name = "dandelion.head",  color = { 0.98, 0.80, 0.15 } },
    },

    apply = function(ctx, p)
        -- Grass grows on the top of things. ctx.ny is the surface's own up-ness: 1 on level ground,
        -- 0 on a vertical wall, negative underneath.
        if ctx.ny < 1.0 - p.maxSlope then return nil end
        if ctx.crevice > p.enclosure then return nil end

        -- Everything below is a pure function of the site's coordinate, so the same plant grows in the
        -- same place every time this ground is asked about -- which is what lets a preview be a
        -- preview, and what stops a second dab reshuffling the first one's meadow.
        local r1 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed)
        local r2 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed + 101)
        local r3 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed + 202)
        local r4 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed + 303)

        -- Which flowers are ticked. Gathered into a list rather than tested one by one further down,
        -- so "whatever combination" costs one uniform pick instead of a chain of special cases -- and
        -- so turning one off cannot change which flower the *others* grow into.
        local flowers = {}
        if p.clover     then flowers[#flowers + 1] = "clover" end
        if p.daisies    then flowers[#flowers + 1] = "daisy" end
        if p.poppies    then flowers[#flowers + 1] = "poppy" end
        if p.buttercups then flowers[#flowers + 1] = "buttercup" end
        if p.bluebells  then flowers[#flowers + 1] = "bluebell" end
        if p.dandelions then flowers[#flowers + 1] = "dandelion" end

        -- What grows here. With blades on, `flowerChance` decides how often a site takes a flower
        -- instead; with blades off, every site that grows anything grows a flower.
        local kind = "blade"
        local wantsFlower = #flowers > 0 and (not p.blades or r1 < p.flowerChance)
        if wantsFlower then
            -- min() rather than a bare floor: pv.hash can return exactly 1.0, which would index one
            -- past the end of the list and hand the script a nil to concatenate.
            kind = flowers[math.min(#flowers, 1 + math.floor(r2 * #flowers))]
        elseif not p.blades then
            return nil
        end

        -- The lean, shared by blades and stalks: a direction on the compass and how hard it curls.
        local angle = r3 * 6.2831853
        local dirX, dirZ = math.cos(angle), math.sin(angle)

        local voxels = {}

        if kind == "clover" then
            -- No stalk. A rosette flat on the ground, which is also the only plant here that fits
            -- under something low.
            voxels[#voxels + 1] = { 0, 1, 0, CLOVER }
            if r1 > 0.35 then voxels[#voxels + 1] = {  1, 1,  0, CLOVER } end
            if r2 > 0.45 then voxels[#voxels + 1] = {  0, 1,  1, CLOVER } end
            if r3 > 0.60 then voxels[#voxels + 1] = { -1, 1,  0, CLOVER } end
            if r4 > 0.75 then voxels[#voxels + 1] = {  0, 1, -1, CLOVER } end
            return voxels
        end

        if kind == "blade" then
            local height = 2 + math.floor(r4 * p.height)
            -- The arc is scaled by the blade's own height, so a tall blade curls further than a short
            -- one instead of every blade tipping by the same absolute amount.
            local arc = p.curve * (0.5 + r1) * height * 0.5
            growStalk(voxels, height, arc, dirX, dirZ, nil, true)
            return voxels
        end

        -- --- Flowers: a stalk, then a head on the end of it ---

        local stalkHeight, arc
        if kind == "poppy" then
            stalkHeight = 4 + math.floor(r4 * 3)
            arc = p.curve * (0.3 + r1 * 0.5) * stalkHeight * 0.4
        elseif kind == "bluebell" then
            stalkHeight = 4 + math.floor(r4 * 2)
            arc = p.curve * (0.5 + r1 * 0.6) * stalkHeight * 0.5
        elseif kind == "dandelion" then
            stalkHeight = 3 + math.floor(r4 * 3)
            arc = p.curve * (0.2 + r1 * 0.3) * stalkHeight * 0.3
        else
            stalkHeight = 2 + math.floor(r4 * 2)
            arc = p.curve * (0.2 + r1 * 0.4) * stalkHeight * 0.4
        end

        local tipX, tipY, tipZ = growStalk(voxels, stalkHeight, arc, dirX, dirZ, STEM, false)

        if kind == "daisy" then
            voxels[#voxels + 1] = { tipX, tipY + 1, tipZ, DAISY_EYE }
            voxels[#voxels + 1] = { tipX + 1, tipY + 1, tipZ, DAISY }
            voxels[#voxels + 1] = { tipX - 1, tipY + 1, tipZ, DAISY }
            voxels[#voxels + 1] = { tipX, tipY + 1, tipZ + 1, DAISY }
            voxels[#voxels + 1] = { tipX, tipY + 1, tipZ - 1, DAISY }

        elseif kind == "poppy" then
            -- Wider and one taller than a daisy, which is why it is the first flower to be refused
            -- when the ground is crowded.
            voxels[#voxels + 1] = { tipX, tipY + 1, tipZ, POPPY_EYE }
            voxels[#voxels + 1] = { tipX + 1, tipY + 1, tipZ, POPPY }
            voxels[#voxels + 1] = { tipX - 1, tipY + 1, tipZ, POPPY }
            voxels[#voxels + 1] = { tipX, tipY + 1, tipZ + 1, POPPY }
            voxels[#voxels + 1] = { tipX, tipY + 1, tipZ - 1, POPPY }
            voxels[#voxels + 1] = { tipX, tipY + 2, tipZ, POPPY }

        elseif kind == "buttercup" then
            voxels[#voxels + 1] = { tipX, tipY + 1, tipZ, BUTTERCUP }
            if r2 > 0.5 then
                voxels[#voxels + 1] = { tipX + 1, tipY + 1, tipZ, BUTTERCUP }
                voxels[#voxels + 1] = { tipX - 1, tipY + 1, tipZ, BUTTERCUP }
            end

        elseif kind == "bluebell" then
            -- Bells hang off one side of the stalk, below its tip -- which is what makes a bluebell
            -- read as a bluebell rather than as a blue daisy.
            local sideX = (dirX >= 0) and 1 or -1
            local sideZ = (math.abs(dirZ) > math.abs(dirX)) and ((dirZ >= 0) and 1 or -1) or 0
            if sideZ ~= 0 then sideX = 0 end
            voxels[#voxels + 1] = { tipX + sideX, tipY, tipZ + sideZ, BLUEBELL }
            voxels[#voxels + 1] = { tipX + sideX, tipY - 1, tipZ + sideZ, BLUEBELL }
            if r2 > 0.5 and tipY >= 3 then
                voxels[#voxels + 1] = { tipX + sideX, tipY - 2, tipZ + sideZ, BLUEBELL }
            end
            voxels[#voxels + 1] = { tipX, tipY + 1, tipZ, BLUEBELL }

        elseif kind == "dandelion" then
            if r2 > 0.5 then
                -- The seed head: a little ball, pale all through.
                voxels[#voxels + 1] = { tipX, tipY + 1, tipZ, PUFF }
                voxels[#voxels + 1] = { tipX + 1, tipY + 1, tipZ, PUFF }
                voxels[#voxels + 1] = { tipX - 1, tipY + 1, tipZ, PUFF }
                voxels[#voxels + 1] = { tipX, tipY + 1, tipZ + 1, PUFF }
                voxels[#voxels + 1] = { tipX, tipY + 1, tipZ - 1, PUFF }
                voxels[#voxels + 1] = { tipX, tipY + 2, tipZ, PUFF }
            else
                voxels[#voxels + 1] = { tipX, tipY + 1, tipZ, DANDELION }
                voxels[#voxels + 1] = { tipX + 1, tipY + 1, tipZ, DANDELION }
                voxels[#voxels + 1] = { tipX - 1, tipY + 1, tipZ, DANDELION }
            end
        end

        return voxels
    end,
}
