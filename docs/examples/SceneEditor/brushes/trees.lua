-- Trees -- a scatter brush, and the one that leans hardest on the fit test.
--
-- A blade of grass is three voxels and fits almost anywhere. A tree is a trunk and a canopy that may
-- be a thousand voxels across, and the interesting question stops being "what does it look like" and
-- becomes **"is there room for it here"**. Three different answers to that are used below, cheapest
-- first, because most candidate sites are rejected and the cheap tests are what keep a dab affordable:
--
--   1. the ground itself -- slope, and whether the base is standing on anything (`footing`);
--   2. a thin column straight up, which catches the common case of a low ceiling for the cost of a
--      dozen cells;
--   3. the whole canopy box plus `clearance`, via pv.fits, and only when clearance is asked for.
--
-- On top of all three, the editor refuses any placement whose voxels would land in occupied cells --
-- all or nothing, so a tree never appears with half its canopy inside a wall. That check is exact and
-- automatic; the ones here exist to reject a site *before* a few thousand voxels are generated for it,
-- and to demand room the placement itself does not occupy.

local BARK    = 1     -- ...through 3: root to crown.
local LEAF    = 4     -- ...through 7: inside to outside.
local NEEDLE  = 8     -- ...through 10.
local BLOSSOM = 11
local DEAD    = 12    -- ...through 13.

-- The trunk, leaning on the same quadratic arc the grass blades use: vertical where it leaves the
-- ground, bending more the higher it goes. Returns the tip so the canopy can sit on it.
local function growTrunk(voxels, height, arc, dirX, dirZ, base, steps)
    local previousX, previousZ = 0, 0
    local x, z = 0, 0
    for i = 1, height do
        local t = (height > 1) and ((i - 1) / (height - 1)) or 0
        local reach = math.floor(t * t * arc + 0.5)
        x = math.floor(dirX * reach + 0.5)
        z = math.floor(dirZ * reach + 0.5)
        local shade = base + math.min(steps - 1, math.floor(t * steps))

        if x == previousX and z == previousZ then
            voxels[#voxels + 1] = { x, i, z, shade }
        else
            -- Fill the elbow, or the trunk comes apart into floating segments where the lean moves it
            -- by more than one cell between levels.
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

-- A few short arms off the upper trunk. Cheap, and the difference between a tree and a lollipop when
-- the canopy is thin enough to see through.
local function growBranches(voxels, ctx, tipX, tipY, tipZ, count, reach, material, seed)
    for branch = 1, count do
        local angle = pv.hash(ctx.x, ctx.y + branch, ctx.z, seed) * 6.2831853
        local dx, dz = math.cos(angle), math.sin(angle)
        local level = tipY - branch
        if level > 1 then
            for step = 1, reach do
                voxels[#voxels + 1] = {
                    tipX + math.floor(dx * step + 0.5),
                    level + math.floor(step * 0.5),
                    tipZ + math.floor(dz * step + 0.5),
                    material
                }
            end
        end
    end
end

-- A ragged ball of foliage. The raggedness is hashed on the voxel's **absolute** coordinate, so a
-- canopy is the same shape every time this tree is asked about -- and two trees whose canopies touch
-- do not share an edge pattern.
local function growCanopy(voxels, ctx, cx, cy, cz, radius, ragged, base, steps, seed)
    local extent = math.floor(radius + 0.5)
    local radiusSquared = radius * radius
    for dy = -extent, extent do
        for dz = -extent, extent do
            for dx = -extent, extent do
                local distanceSquared = dx * dx + dy * dy + dz * dz
                if distanceSquared <= radiusSquared then
                    local edge = math.sqrt(distanceSquared) / radius
                    -- Thinner towards the outside: solid in the middle, broken at the rim.
                    local n = pv.hash(ctx.x + cx + dx, ctx.y + cy + dy, ctx.z + cz + dz, seed)
                    if n > edge * edge * ragged then
                        local shade = base + math.min(steps - 1, math.floor(edge * steps))
                        voxels[#voxels + 1] = { cx + dx, cy + dy, cz + dz, shade }
                    end
                end
            end
        end
    end
end

return {
    name = "Trees",
    kind = "scatter",
    description = "Broadleaf, conifer and bare trees, each checked for room before it lands.",
    author = "ProjectV",

    needs = { "position", "normal", "crevice", "material" },
    creviceRadius = 2,

    params = {
        -- The editor's two, read by name.
        { name = "spacing", label = "Spacing", type = "int", default = 14, min = 1, max = 64,
          tooltip = "How far apart trees are, in voxels. Far larger than a grass brush wants: this is\n"
                 .. "roughly the width of a canopy, and below it every tree is refused for want of\n"
                 .. "room by the one beside it." },
        { name = "density", label = "Coverage", type = "float", default = 0.6, min = 0.0, max = 1.0,
          tooltip = "What fraction of the eligible cells get a tree. Refusals come out of this: a\n"
                 .. "wooded slope plants far fewer than a flat clearing at the same setting." },

        -- What may grow, in any combination.
        { name = "broadleaf", label = "Broadleaf", type = "bool", default = true,
          tooltip = "A leaning trunk, a few branches and a ragged ball of leaves." },
        { name = "conifer", label = "Conifer", type = "bool", default = true,
          tooltip = "A straight trunk under stacked rings that taper to a point. The narrowest, so\n"
                 .. "the one that still fits where a broadleaf will not." },
        { name = "bare", label = "Bare", type = "bool", default = false,
          tooltip = "Trunk and branches, no foliage. Fits almost anywhere, which makes it the useful\n"
                 .. "one for a crowded or a dead landscape." },
        { name = "blossom", label = "Blossom", type = "bool", default = false,
          tooltip = "Scatters petals through a broadleaf canopy. Costs one more palette entry and\n"
                 .. "nothing else." },

        { name = "height", label = "Trunk height", type = "int", default = 8, min = 2, max = 40,
          tooltip = "The tallest a trunk may be. Each tree takes a share of it, so this is the top of\n"
                 .. "a range rather than a fixed height." },
        { name = "canopy", label = "Canopy radius", type = "int", default = 4, min = 1, max = 12,
          tooltip = "How wide the foliage is. Cost is cubic in this -- a radius of 8 is two thousand\n"
                 .. "voxels of tree -- and so is how often a tree is refused for want of room." },
        { name = "ragged", label = "Raggedness", type = "float", default = 0.8, min = 0.0, max = 1.0,
          tooltip = "How broken the outside of a canopy is. 0 is a billiard ball." },
        { name = "curve", label = "Lean", type = "float", default = 0.4, min = 0.0, max = 1.0,
          tooltip = "How far a trunk leans off vertical. Quadratic, so the tree stands up out of the\n"
                 .. "ground and does its leaning near the crown." },

        -- The fit test's own settings.
        { name = "clearance", label = "Clearance", type = "int", default = 1, min = 0, max = 8,
          tooltip = "Empty space demanded *around* the canopy, in voxels, on top of the room the tree\n"
                 .. "itself takes. 0 lets canopies touch; 2 or more keeps them apart and refuses far\n"
                 .. "more sites. This is the one check the editor cannot do for the brush -- it knows\n"
                 .. "what the tree occupies, not what it wants to be left alone." },
        { name = "footing", label = "Footing", type = "int", default = 6, min = 0, max = 9,
          tooltip = "How many of the nine cells under the base must be solid ground. 9 demands a flat\n"
                 .. "slab; 0 will plant a tree on a single voxel sticking out of a cliff." },
        { name = "maxSlope", label = "Steepest ground", type = "float", default = 0.45, min = 0.0, max = 1.0 },
        { name = "enclosure", label = "Skip ground more enclosed than", type = "float",
          default = 0.75, min = 0.5, max = 1.0,
          tooltip = "Skips ground that is boxed in -- the bottom of a narrow gully is where a canopy\n"
                 .. "has nowhere to go.\n\n"
                 .. "Note where the useful range starts. `crevice` is the fraction of nearby cells\n"
                 .. "that are solid, and **flat open ground already reads about 0.6** -- half the ball\n"
                 .. "around a surface voxel is the ground it stands on. So a threshold below that\n"
                 .. "rejects everything, including the field you were aiming at." },
        { name = "seed", type = "seed", default = 7 },
    },

    materials = {
        { name = "tree.bark",    steps = 3, color = { 0.26, 0.18, 0.12 }, colorTo = { 0.44, 0.33, 0.22 } },
        { name = "tree.leaf",    steps = 4, color = { 0.10, 0.26, 0.09 }, colorTo = { 0.38, 0.62, 0.22 } },
        { name = "tree.needle",  steps = 3, color = { 0.07, 0.20, 0.13 }, colorTo = { 0.20, 0.40, 0.22 } },
        { name = "tree.blossom",            color = { 0.94, 0.76, 0.82 } },
        { name = "tree.dead",    steps = 2, color = { 0.30, 0.26, 0.22 }, colorTo = { 0.46, 0.42, 0.37 } },
    },

    apply = function(ctx, p)
        -- ---- 1. The ground ----
        --
        -- The cheapest tests, and the ones that reject the most sites. Everything below them costs
        -- either a scene query or a few thousand voxels of generated tree.
        if ctx.ny < 1.0 - p.maxSlope then return nil end
        if ctx.crevice > p.enclosure then return nil end

        local kinds = {}
        if p.broadleaf then kinds[#kinds + 1] = "broadleaf" end
        if p.conifer   then kinds[#kinds + 1] = "conifer" end
        if p.bare      then kinds[#kinds + 1] = "bare" end
        if #kinds == 0 then return nil end

        local r1 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed)
        local r2 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed + 101)
        local r3 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed + 202)
        local kind = kinds[math.min(#kinds, 1 + math.floor(r1 * #kinds))]

        -- Is the base standing on anything? A tree rooted on a one-voxel spur reads as a mistake, and
        -- nothing else here would reject it -- the site is a surface voxel, so it is solid by
        -- definition; what is in question is its neighbours.
        if p.footing > 0 then
            local firm = 0
            for dz = -1, 1 do
                for dx = -1, 1 do
                    if pv.solid(ctx.x + dx, ctx.y, ctx.z + dz) then firm = firm + 1 end
                end
            end
            if firm < p.footing then return nil end
        end

        -- ---- 2. Headroom ----
        --
        -- A thin column straight up, before any geometry is built. This is the cheap version of the
        -- box test below -- a dozen cells against a few thousand -- and on a scene with a ceiling it
        -- rejects almost everything the expensive test would have.
        local trunkHeight = math.max(2, math.floor(p.height * (0.55 + r2 * 0.45)))
        local canopyRadius = p.canopy
        if kind == "conifer" then canopyRadius = math.max(1, p.canopy - 1) end
        -- The crown sits `canopyRadius - 1` above the trunk's tip and reaches `canopyRadius` above
        -- that, so the tallest voxel of the tree is trunk + 2*radius - 1. Worth deriving rather than
        -- guessing: a headroom check that stops short of the canopy passes sites where the top of the
        -- tree is inside a ceiling, and the editor then refuses the placement anyway -- so the brush
        -- pays for generating a whole tree to have it thrown away.
        local topY = ctx.y + trunkHeight + canopyRadius * 2
        if not pv.fits(ctx.x, ctx.y + 1, ctx.z, ctx.x, topY, ctx.z) then return nil end

        -- ---- 3. Room to spread ----
        --
        -- The whole canopy box plus the clearance margin. Skipped entirely when no clearance is asked
        -- for, because with clearance 0 this is asking the same question the editor already answers
        -- exactly, and answering it twice is the most expensive thing in the brush.
        local arc = p.curve * (0.4 + r3) * trunkHeight * 0.35
        local angle = r2 * 6.2831853
        local dirX, dirZ = math.cos(angle), math.sin(angle)
        if p.clearance > 0 then
            local lean = math.floor(arc + 0.5)
            local reach = canopyRadius + p.clearance + math.abs(lean)
            if not pv.fits(ctx.x - reach, ctx.y + 1, ctx.z - reach,
                           ctx.x + reach, topY + p.clearance, ctx.z + reach) then
                return nil
            end
        end

        -- ---- The tree itself ----

        local voxels = {}

        if kind == "conifer" then
            -- A straight-ish trunk, then rings that taper to a point. Built as discs rather than as a
            -- ball because a conifer's silhouette is the whole of what makes it one.
            local bareTrunk = math.max(1, math.floor(trunkHeight * 0.35))
            local tipX, tipY, tipZ = growTrunk(voxels, trunkHeight, arc * 0.4, dirX, dirZ, BARK, 3)
            local skirt = trunkHeight - bareTrunk + canopyRadius
            for level = 0, skirt do
                local t = level / skirt
                local radius = (1.0 - t) * canopyRadius + 0.4
                local extent = math.floor(radius + 0.5)
                local y = bareTrunk + level
                for dz = -extent, extent do
                    for dx = -extent, extent do
                        if dx * dx + dz * dz <= radius * radius then
                            local edge = math.sqrt(dx * dx + dz * dz) / math.max(radius, 0.001)
                            local n = pv.hash(ctx.x + dx, ctx.y + y, ctx.z + dz, p.seed + 7)
                            if n > edge * edge * p.ragged * 0.7 then
                                local shade = NEEDLE + math.min(2, math.floor(edge * 3))
                                voxels[#voxels + 1] = { dx, y, dz, shade }
                            end
                        end
                    end
                end
            end
            -- One voxel at the very top, so the cone comes to a point rather than a plateau.
            voxels[#voxels + 1] = { tipX, bareTrunk + skirt + 1, tipZ, NEEDLE }
            return voxels
        end

        if kind == "bare" then
            local tipX, tipY, tipZ = growTrunk(voxels, trunkHeight, arc, dirX, dirZ, DEAD, 2)
            growBranches(voxels, ctx, tipX, tipY, tipZ, 4, math.max(2, canopyRadius - 1),
                         DEAD + 1, p.seed + 11)
            return voxels
        end

        -- Broadleaf: trunk, a few branches, and a ball of leaves sitting on the crown.
        local tipX, tipY, tipZ = growTrunk(voxels, trunkHeight, arc, dirX, dirZ, BARK, 3)
        growBranches(voxels, ctx, tipX, tipY, tipZ, 3, math.max(1, canopyRadius - 2), BARK + 1,
                     p.seed + 11)
        growCanopy(voxels, ctx, tipX, tipY + math.max(1, canopyRadius - 1), tipZ,
                   canopyRadius, p.ragged, LEAF, 4, p.seed + 3)

        if p.blossom then
            -- Sprinkled through the canopy afterwards rather than chosen per leaf inside it: the
            -- canopy is already written, and a second pass over its voxels is cheaper than a branch
            -- inside the triple loop that generated them.
            for i = 1, #voxels do
                local voxel = voxels[i]
                if voxel[4] >= LEAF and voxel[4] < LEAF + 4 then
                    local n = pv.hash(ctx.x + voxel[1], ctx.y + voxel[2], ctx.z + voxel[3], p.seed + 5)
                    if n > 0.82 then voxel[4] = BLOSSOM end
                end
            end
        end

        return voxels
    end,
}
