-- Rocks -- a scatter brush: one boulder per site, shaped and coloured as a piece of a named rock.
--
-- Three kinds, from the Colorado Plateau's cliff-formers (docs/plans/biome-canyonlands.md):
--
--   Wingate block   a piece of a jointed wall: a box whose faces are the joint planes, slightly out of
--                   true, corners and edges chipped, a spall scar on one face. Red-orange, with dark
--                   desert-varnish streaks running down the sides from the top edge.
--   Navajo boulder  dune sandstone weathered round: a squashed superellipsoid, lumpy, pitted on top.
--                   Cream to tan, crossed by salmon cross-bed bands that dip, rather than lie level.
--   Kayenta slab    a flat plate of thin beds, each bed's edge set in or out a little, so the rim
--                   is stepped. Maroon, red-brown and lavender by bed.
--
-- **The shape is a signed distance**, evaluated over the boulder's box once, then coloured from the
-- grid: a voxel is solid where the distance is negative, it is a surface voxel where a neighbour is
-- not, and its normal is the distance's gradient. Colour is albedo only -- what the rock is, not how
-- it is lit -- so a renderer with real light (the terrain's ReSTIR) does the shading.
--
-- Solid all through, so a boulder broken open is rock inside. Only a boulder too big for the
-- placement budget is hollowed, to a shell four voxels thick.

local MATERIALS = {
    { name = "rock.wingate", colors = {
        { 0.36, 0.15, 0.09 }, { 0.48, 0.21, 0.12 }, { 0.60, 0.29, 0.16 }, { 0.70, 0.37, 0.21 },
        { 0.78, 0.47, 0.29 }, { 0.85, 0.58, 0.40 } } },
    -- Desert varnish: manganese and iron, near black on red rock.
    { name = "rock.varnish", colors = {
        { 0.20, 0.11, 0.08 }, { 0.28, 0.16, 0.11 }, { 0.36, 0.21, 0.14 } } },
    -- Warm: a pale sandstone under a blue sky reads grey unless its albedo leans orange.
    { name = "rock.navajo", colors = {
        { 0.60, 0.42, 0.30 }, { 0.71, 0.52, 0.37 }, { 0.80, 0.62, 0.46 }, { 0.86, 0.71, 0.55 },
        { 0.90, 0.79, 0.64 }, { 0.93, 0.86, 0.73 } } },
    { name = "rock.navajoband", colors = {
        { 0.74, 0.40, 0.25 }, { 0.82, 0.49, 0.31 }, { 0.88, 0.58, 0.39 } } },
    { name = "rock.kayenta", colors = {
        { 0.26, 0.14, 0.13 }, { 0.36, 0.19, 0.17 }, { 0.46, 0.25, 0.21 }, { 0.52, 0.33, 0.31 },
        { 0.58, 0.36, 0.29 }, { 0.66, 0.46, 0.40 } } },
    -- Orange crustose lichen, in patches on the tops of the sandstones.
    { name = "rock.lichen", color = { 0.80, 0.50, 0.15 } },
}

local RAMP_START, RAMP_STEPS = {}, {}
do
    local index = 1
    for _, material in ipairs(MATERIALS) do
        local steps = material.colors and #material.colors or (material.steps or 1)
        RAMP_START[material.name], RAMP_STEPS[material.name] = index, steps
        index = index + steps
    end
end

local floor, sqrt, cos, sin, max, min, abs = math.floor, math.sqrt, math.cos, math.sin,
                                             math.max, math.min, math.abs
local TAU = 6.28318530718
local BUDGET = 100000

local function clamp01(v) return v < 0 and 0 or (v > 1 and 1 or v) end

-- A ramp step for `v` (0..1), dithered so neighbouring steps interleave in patches rather than meet
-- at a line (the tree brush's ditheredStep).
local function step(ramp, v, x, y, z, seed)
    local steps = RAMP_STEPS[ramp]
    local jitter = (pv.hash(x, y, z, seed) - 0.5) * 0.7 + (pv.noise(x * 0.3, y * 0.3, z * 0.3, seed + 3) - 0.5) * 0.6
    return RAMP_START[ramp] + max(0, min(steps - 1, floor(v * steps + jitter)))
end

local function normalize(x, y, z)
    local l = sqrt(x * x + y * y + z * z)
    if l < 1e-6 then return 0, 1, 0 end
    return x / l, y / l, z / l
end

-- ---------------------------------------------------------------------------------------------
-- Shapes: each returns a distance function over (x, y, z) relative to the boulder's centre on the
-- ground, and the boulder's half extents.
-- ---------------------------------------------------------------------------------------------

-- A convex box from six planes, each tilted a little off its axis -- joint faces are planes but never
-- square to each other -- with chips: planes cutting off a corner or an edge.
local function wingateShape(p, seed, H)
    local W = H * (1.0 + 0.8 * pv.hash(1, 2, 3, seed)) * 0.5 * p.width
    local D = H * (0.8 + 0.6 * pv.hash(4, 5, 6, seed)) * 0.5 * p.width
    local h = H * 0.5
    local planes = {}
    local axes = { { 1, 0, 0, W }, { -1, 0, 0, W }, { 0, 1, 0, h }, { 0, -1, 0, h }, { 0, 0, 1, D }, { 0, 0, -1, D } }
    for i, a in ipairs(axes) do
        local tilt = (a[2] ~= 0) and 0.05 or 0.14
        local nx, ny, nz = normalize(a[1] + (pv.hash(i, 1, 0, seed) - 0.5) * tilt * 2,
                                     a[2] + (pv.hash(i, 2, 0, seed) - 0.5) * tilt * 2,
                                     a[3] + (pv.hash(i, 3, 0, seed) - 0.5) * tilt * 2)
        planes[#planes + 1] = { nx, ny, nz, a[4] }
    end
    -- Chips: two to four, each a clean break plane off a corner or an edge, at its own odd angle --
    -- a fresh fracture, not a bevel. Symmetric bevels trimmed deep (the first version) rounded every
    -- block into a loaf.
    local chips = 2 + floor(pv.hash(7, 7, 7, seed) * 3)
    for i = 1, chips do
        local sx = pv.hash(i, 11, 0, seed) < 0.5 and -1 or 1
        local sy = pv.hash(i, 12, 0, seed) < 0.6 and 1 or -1      -- mostly the top: what fell, broke
        local sz = pv.hash(i, 13, 0, seed) < 0.5 and -1 or 1
        local edge = pv.hash(i, 14, 0, seed)
        local cx, cy, cz = sx * W, sy * h, sz * D
        if edge < 0.33 then cx = 0 elseif edge < 0.5 then cz = 0 end    -- an edge rather than a corner
        local nx, ny, nz = normalize(cx / max(W, 1) + (pv.hash(i, 16, 0, seed) - 0.5) * 0.9,
                                     cy / max(h, 1) + (pv.hash(i, 17, 0, seed) - 0.5) * 0.9,
                                     cz / max(D, 1) + (pv.hash(i, 18, 0, seed) - 0.5) * 0.9)
        local reach = nx * cx + ny * cy + nz * cz
        planes[#planes + 1] = { nx, ny, nz, reach * (0.80 + 0.14 * pv.hash(i, 15, 0, seed)) }
    end
    -- One spall scar: a shallow rectangular recess in a side face, where a slab has come away.
    local face = 1 + floor(pv.hash(9, 9, 9, seed) * 4)
    local scar = ({ { 1, 0, W }, { -1, 0, W }, { 0, 1, D }, { 0, -1, D } })[face]
    local scarDepth = max(1.0, H * 0.07)
    local scarU = (scar[1] ~= 0) and D or W
    local function dist(x, y, z)
        local d = -1e9
        for i = 1, #planes do
            local q = planes[i]
            local v = q[1] * x + q[2] * y + q[3] * z - q[4]
            if v > d then d = v end
        end
        -- The spall: carve where inside the face's rectangle and within scarDepth of it.
        local along = scar[1] * x + scar[2] * z
        local u = (scar[1] ~= 0) and z or x
        if along > scar[3] - scarDepth and abs(u) < scarU * 0.55 and y > -h * 0.3 and y < h * 0.7 then
            d = max(d, (scar[3] - scarDepth) - along)
        end
        -- Far from the surface the noise cannot change the answer: skip it (most of the box).
        if abs(d) > 1.0 * p.roughness + 1.0 then return d end
        -- Weathering: the faces are planes, but not machined ones -- only just: more than a voxel of
        -- this rounds the edges, and sharp edges are what say "broken off a jointed wall".
        d = d + (pv.fbm(x * 0.12, y * 0.12, z * 0.12, 3, seed + 21) - 0.5) * 0.7 * p.roughness
              + (pv.noise(x * 0.6, y * 0.6, z * 0.6, seed + 23) - 0.5) * 0.25 * p.roughness
        return d
    end
    return dist, W * 1.2, h * 1.1, D * 1.2, { scar = scar, scarDepth = scarDepth }
end

-- A squashed superellipsoid, lumpy at the scale of the boulder, pitted on top.
local function navajoShape(p, seed, H)
    local a = H * (0.65 + 0.35 * pv.hash(1, 2, 3, seed)) * p.width
    local c = H * (0.55 + 0.35 * pv.hash(4, 5, 6, seed)) * p.width
    local b = H * 0.5
    local e = 2.6
    local function dist(x, y, z)
        local u = (abs(x) / a) ^ e + (abs(y) / b) ^ e + (abs(z) / c) ^ e
        local r = min(a, b, c)
        local d = (u ^ (1 / e) - 1.0) * r
        if abs(d) > r * 0.3 * p.roughness + 1.5 then return d end
        d = d + (pv.fbm(x / (r * 1.1) + 7, y / (r * 1.1), z / (r * 1.1), 3, seed + 31) - 0.5) * r * 0.45 * p.roughness
              + (pv.noise(x * 0.35, y * 0.35, z * 0.35, seed + 33) - 0.5) * 0.35 * p.roughness
        -- Weathering pits, where water stands on the top.
        if y > b * 0.45 then
            local f1 = pv.worley(x * 0.18, 0.0, z * 0.18, seed + 35)
            if f1 < 0.28 then d = d + (0.28 - f1) * 7.0 * clamp01((y - b * 0.45) / (b * 0.4)) end
        end
        return d
    end
    return dist, a * 1.35, b * 1.2, c * 1.35, {}
end

-- A plate of two to four beds, each with its own rim set in or out.
local function kayentaShape(p, seed, H)
    local W = H * (1.6 + 1.0 * pv.hash(1, 2, 3, seed)) * p.width
    local D = H * (1.1 + 0.8 * pv.hash(4, 5, 6, seed)) * p.width
    local h = H * 0.5
    local beds = 2 + floor(pv.hash(7, 8, 9, seed) * 3)
    local insets = {}
    for k = 1, beds do insets[k] = pv.hash(k, 3, 3, seed) * max(1.0, H * 0.35) end
    -- The plan outline: a rounded rectangle, irregular.
    local function dist(x, y, z)
        local k = min(beds, max(1, floor((y + h) / (2 * h) * beds) + 1))
        local inset = insets[k]
        local qx, qz = abs(x) - (W - inset), abs(z) - (D - inset)
        local corner = min(W, D) * 0.35
        qx, qz = qx + corner, qz + corner
        local outside = sqrt(max(qx, 0) ^ 2 + max(qz, 0) ^ 2) + min(max(qx, qz), 0) - corner
        local d = max(outside, abs(y) - h)
        if abs(d) > 1.6 * p.roughness + 1.0 then return d, k end
        d = d + (pv.fbm(x * 0.12, y * 0.4, z * 0.12, 3, seed + 41) - 0.5) * 2.4 * p.roughness
              + (pv.noise(x * 0.6, y * 0.6, z * 0.6, seed + 43) - 0.5) * 0.6 * p.roughness
        return d, k
    end
    return dist, W * 1.15, h * 1.1, D * 1.15, { beds = beds }
end

-- ---------------------------------------------------------------------------------------------
-- Colour
-- ---------------------------------------------------------------------------------------------

local function wingateColour(p, seed, x, y, z, nx, ny, nz, H, info, ax, ay, az)
    local h = H * 0.5
    local t = (y + h) / (2 * h)
    -- Each joint face its own tone: the faces broke at different times and weathered differently.
    local faceKey = (abs(nx) > abs(nz)) and (nx > 0 and 1 or 2) or (nz > 0 and 3 or 4)
    if ny > 0.7 then faceKey = 5 elseif ny < -0.7 then faceKey = 6 end
    local v = 0.48 + (pv.hash(faceKey, 1, 1, seed + 51) - 0.5) * 0.26
    -- Faint laminations, nearly level: the rock's own bedding.
    v = v + sin(y * 0.9 + (pv.noise(x * 0.08, y * 0.08, z * 0.08, seed + 53) - 0.5) * 5.0) * 0.06
    v = v + (pv.fbm(x * 0.12, y * 0.12, z * 0.12, 3, seed + 55) - 0.5) * 0.3
    if ny < -0.5 then v = v - 0.2 end                      -- the underside, in the dirt
    -- The spall scar is fresh rock: paler.
    local scar = info.scar
    local along = scar[1] * x + scar[2] * z
    if along > scar[3] - info.scarDepth - 0.8 and along < scar[3] - info.scarDepth + 1.2 then v = v + 0.22 end
    -- Varnish: on the sides, streaks running down from the top edge -- where water ran off it.
    if abs(ny) < 0.6 and ny > -0.5 then
        local u = (abs(nx) > abs(nz)) and z or x
        -- Narrow (a few voxels across), long, and thinning out toward the bottom -- dithered into
        -- the rock there rather than ending in a hard line.
        local streak = pv.noise(u * 0.7 + faceKey * 17.0, y * 0.025, 0.5, seed + 57)
        local fromTop = clamp01(1.0 - (h - y) / (2 * h) * 1.2)
        local strength = (streak - 0.6) / 0.25 * (0.4 + 0.8 * fromTop)
        if strength > 0 and pv.hash(ax, ay, az, seed + 58) < strength then
            return step("rock.varnish", clamp01(1.0 - strength), ax, ay, az, seed + 59)
        end
    end
    -- Lichen in patches on top.
    if ny > 0.75 and pv.noise(x * 0.2, 0, z * 0.2, seed + 61) > 0.8 then
        return RAMP_START["rock.lichen"]
    end
    return step("rock.wingate", clamp01(v + 0.06 * t), ax, ay, az, seed + 65)
end

local function navajoColour(p, seed, x, y, z, nx, ny, nz, H, info, ax, ay, az)
    -- Cross-bedding: dune laminae, planes dipping ~25 degrees one way, gently curved, cut through.
    local az0 = pv.hash(1, 1, 1, seed + 71) * TAU
    local dip = 0.42
    local c = y * cos(dip) + (x * cos(az0) + z * sin(az0)) * sin(dip)
    c = c + 0.012 * (x * x + z * z) / max(1, H) * 3.0
        + (pv.noise(x * 0.07, y * 0.07, z * 0.07, seed + 73) - 0.5) * 3.0
    local period = max(2.2, H * 0.16)
    local f = c / period - floor(c / period)
    local band = f < 0.34 and pv.noise(x * 0.08, y * 0.08, z * 0.08, seed + 75) > 0.3
    if band then
        return step("rock.navajoband", clamp01(0.5 + (pv.noise(x * 0.2, y * 0.2, z * 0.2, seed + 77) - 0.5)), ax, ay, az, seed + 79)
    end
    local v = 0.55 + (pv.fbm(x * 0.1, y * 0.1, z * 0.1, 3, seed + 81) - 0.5) * 0.4 + 0.12 * ny
    if ny > 0.75 and pv.noise(x * 0.2, 0, z * 0.2, seed + 83) > 0.82 then
        return RAMP_START["rock.lichen"]
    end
    return step("rock.navajo", clamp01(v), ax, ay, az, seed + 87)
end

local function kayentaColour(p, seed, x, y, z, nx, ny, nz, H, info, ax, ay, az, bed)
    -- A tone per bed, then thin laminations and mottling within it.
    local v = 0.2 + 0.65 * pv.hash(bed or 1, 4, 4, seed + 91)
    v = v + sin(y * 2.1 + (pv.noise(x * 0.1, 0, z * 0.1, seed + 93) - 0.5) * 3.0) * 0.07
    v = v + (pv.fbm(x * 0.15, y * 0.15, z * 0.15, 3, seed + 95) - 0.5) * 0.25
    if ny < -0.5 then v = v - 0.15 end
    return step("rock.kayenta", clamp01(v), ax, ay, az, seed + 97)
end

local KINDS = { "Wingate block", "Navajo boulder", "Kayenta slab" }

return {
    name = "Rocks",
    kind = "scatter",
    description = "Boulders of three canyon-country rocks: jointed Wingate blocks with desert varnish, "
               .. "rounded cross-bedded Navajo boulders, stepped Kayenta slabs. Solid, coloured as "
               .. "albedo (no baked light). Force plants one exactly where you click.",
    author = "ProjectV",

    needs = { "position", "normal", "material" },

    params = {
        { name = "force", label = "Force (one, exactly where I click)", type = "bool", default = false,
          tooltip = "Plant one boulder on the voxel under the cursor, skipping the spacing grid and\n"
                 .. "the slope test." },
        { name = "spacing", label = "Spacing", type = "int", default = 40, min = 2, max = 128,
          tooltip = "How far apart boulders are, in voxels." },
        { name = "density", label = "Coverage", type = "float", default = 0.4, min = 0.0, max = 1.0 },
        { name = "rock", label = "Rock", type = "enum", default = "Wingate block", options = KINDS,
          tooltip = "  Wingate block   jointed, chipped, red-orange with dark varnish streaks\n"
                 .. "  Navajo boulder  rounded, pitted on top, cream with salmon cross-bed bands\n"
                 .. "  Kayenta slab    a flat plate of beds with stepped rims, maroon to lavender" },
        { name = "height", label = "Height", type = "int", default = 14, min = 2, max = 48,
          tooltip = "How tall the boulder is, in voxels. Each takes 85-115% of it. The other\n"
                 .. "dimensions follow from it and the kind." },
        { name = "width", label = "Width", type = "float", default = 1.0, min = 0.4, max = 2.5,
          tooltip = "Width and depth as a multiple of what the kind gives for this height." },
        { name = "roughness", label = "Roughness", type = "float", default = 1.0, min = 0.0, max = 2.0,
          tooltip = "How far the faces are weathered off true. 0 is a clean solid." },
        { name = "maxSlope", label = "Steepest ground", type = "float", default = 0.6, min = 0.0, max = 1.0 },
        { name = "seed", type = "seed", default = 3 },
    },

    materials = MATERIALS,

    apply = function(ctx, p)
        if not ctx.force and ctx.ny < 1.0 - p.maxSlope - 0.03 then return nil, "too steep" end
        local seed = p.seed + floor(pv.hash(ctx.x, ctx.y, ctx.z, p.seed) * 100000)
        local H = max(2, p.height * (0.85 + 0.3 * pv.hash(ctx.x, ctx.y, ctx.z, p.seed + 1)))

        local dist, ex, ey, ez, info, colour
        if p.rock == "Navajo boulder" then
            dist, ex, ey, ez, info = navajoShape(p, seed, H); colour = navajoColour
        elseif p.rock == "Kayenta slab" then
            dist, ex, ey, ez, info = kayentaShape(p, seed, H); colour = kayentaColour
        else
            dist, ex, ey, ez, info = wingateShape(p, seed, H); colour = wingateColour
        end
        -- Its centre sits half its height up, so it rests on the ground (y >= 1).
        local cy = H * 0.5 + 0.5
        local X, Y, Z = floor(ex + 1), floor(ey + 1), floor(ez + 1)

        -- The distance over the box, once.
        local grid, bedOf = {}, {}
        local function key(i, j, k) return (i + 128) + (j + 128) * 256 + (k + 128) * 65536 end
        local solid = 0
        for j = 1, floor(cy + Y) do
            local y = j - cy
            for k = -Z, Z do
                for i = -X, X do
                    local d, bed = dist(i, y, k)
                    if d < 0 then
                        grid[key(i, j, k)] = d
                        bedOf[key(i, j, k)] = bed
                        solid = solid + 1
                    end
                end
            end
        end
        -- Too big for one placement: hollow it, keeping a shell four voxels thick.
        local shell = solid > BUDGET and -4.0 or -1e9

        local voxels = {}
        for kk, d in pairs(grid) do
            local i = kk % 256 - 128
            local j = floor(kk / 256) % 256 - 128
            local k = floor(kk / 65536) - 128
            local open = not grid[key(i + 1, j, k)] or not grid[key(i - 1, j, k)] or
                         not grid[key(i, j + 1, k)] or not grid[key(i, j - 1, k)] or
                         not grid[key(i, j, k + 1)] or not grid[key(i, j, k - 1)]
            if open or d > shell then
                local material
                local ax, ay, az = ctx.x + i, ctx.y + j, ctx.z + k
                if open then
                    -- The surface normal: the distance's gradient, from its neighbours.
                    local y = j - cy
                    local nx, ny, nz = normalize(dist(i + 1, y, k) - dist(i - 1, y, k),
                                                 dist(i, y + 1, k) - dist(i, y - 1, k),
                                                 dist(i, y, k + 1) - dist(i, y, k - 1))
                    material = colour(p, seed, i, y, k, nx, ny, nz, H, info, ax, ay, az, bedOf[kk])
                else
                    -- Buried: the kind's mid tone. Seen only if the boulder is broken open.
                    local ramp = (colour == navajoColour) and "rock.navajo"
                              or (colour == kayentaColour) and "rock.kayenta" or "rock.wingate"
                    material = RAMP_START[ramp] + floor(RAMP_STEPS[ramp] / 2)
                end
                voxels[#voxels + 1] = { i, j, k, material }
            end
        end
        -- In a fixed order, so the same site always gives the same placement.
        table.sort(voxels, function(a, b)
            if a[2] ~= b[2] then return a[2] < b[2] end
            if a[3] ~= b[3] then return a[3] < b[3] end
            return a[1] < b[1]
        end)
        return voxels
    end,
}
