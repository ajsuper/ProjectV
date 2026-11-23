-- Boulders -- a scatter brush: one boulder, or a group of them, of any of six shapes in any of eight
-- rocks, with rubble at the foot and moss and lichen where they would grow.
-- Plan: docs/plans/boulder-brush.md.
--
-- **Shape and rock are separate axes.** The shape is how the stone broke or weathered -- rounded,
-- angular, a slab, split in two, a tor of stacked blocks, a clump of basalt columns -- and the rock is
-- what it is made of: its colours, its sheen and metal, and its *relief*, the small geometry its
-- weathering leaves (basalt's vesicles, sandstone's honeycomb, limestone's runnels, slate's ridges).
-- Relief is geometry, not paint, because the renderers light voxels: a pit is a pit.
--
-- **A group is one placement** -- the host's fit test is all or nothing, so a group lands whole or not
-- at all, and one undo takes it back. Each boulder is **dropped**: it rests on the ground under its own
-- footprint (found with pv.solid) or on the boulders placed before it, and sinks by `Sunk`. Cells at or
-- below the ground are clipped rather than written, so nothing is placed inside the terrain and every
-- boulder looks bedded rather than set down.
--
-- **Moss comes last**, from the assembled group: on tops, on the shaded (-z) side, low down where it
-- is damp. Dense moss is a cushion of extra voxels standing off the stone, not paint alone.
--
-- Pipeline, per call:
--   1. pick the group: sizes, shapes, rock, layout                      (cheap)
--   2. per boulder, march its signed distance column by column, skipping far from the surface
--   3. drop it onto the ground and the boulders already placed, clip it at the ground, add it
--   4. colour every open cell: rock, veins, minerals, stains, lichen, moss; moss cushions
--   5. hollow to a shell if over budget, sort, return

-- ---------------------------------------------------------------------------------------------
-- Materials
-- ---------------------------------------------------------------------------------------------
--
-- Every colour differs from every other role's: interning falls back to matching by colour, so a
-- duplicate would bind to the wrong entry. Sheen (glossiness) and metal are per role, so a rock's are
-- shared by its whole ramp; the accents that need more -- mica, pyrite, haematite, quartz -- are roles
-- of their own. 71 roles, interned on the first stroke.
local MATERIALS = {
    -- Granite: grey with a warm cast, a little sheen off its feldspar faces.
    { name = "boulder.granite", glossiness = 0.12, colors = {
        { 0.30, 0.29, 0.28 }, { 0.40, 0.39, 0.37 }, { 0.50, 0.48, 0.46 }, { 0.60, 0.57, 0.54 },
        { 0.69, 0.66, 0.62 }, { 0.78, 0.75, 0.71 } } },
    -- Biotite mica: black, flaky, a glitter in the sun.
    { name = "boulder.mica", color = { 0.09, 0.09, 0.10 }, glossiness = 0.8, metallic = 0.55 },
    -- Feldspar crystals: pink in some granites, white in others.
    { name = "boulder.feldspar", glossiness = 0.35, colors = { { 0.79, 0.62, 0.55 }, { 0.89, 0.87, 0.83 } } },
    -- Basalt: near black, faintly glassy.
    { name = "boulder.basalt", glossiness = 0.2, colors = {
        { 0.10, 0.10, 0.11 }, { 0.15, 0.15, 0.16 }, { 0.21, 0.21, 0.22 }, { 0.28, 0.28, 0.29 },
        { 0.36, 0.35, 0.35 } } },
    -- Its weathering rind, iron going to rust on the exposed top.
    { name = "boulder.rind", colors = { { 0.36, 0.22, 0.14 }, { 0.50, 0.33, 0.20 } } },
    -- Sandstone: tan to buff, matte.
    { name = "boulder.sandstone", colors = {
        { 0.42, 0.30, 0.20 }, { 0.53, 0.39, 0.27 }, { 0.63, 0.49, 0.34 }, { 0.72, 0.58, 0.42 },
        { 0.80, 0.67, 0.50 }, { 0.87, 0.76, 0.60 } } },
    -- Its iron bands.
    { name = "boulder.sandband", colors = { { 0.55, 0.30, 0.18 }, { 0.66, 0.39, 0.24 }, { 0.75, 0.48, 0.31 } } },
    -- Limestone: pale cream-grey, a soft sheen where water has worked it.
    { name = "boulder.limestone", glossiness = 0.08, colors = {
        { 0.40, 0.39, 0.35 }, { 0.51, 0.50, 0.46 }, { 0.61, 0.60, 0.55 }, { 0.70, 0.69, 0.64 },
        { 0.78, 0.77, 0.72 }, { 0.85, 0.84, 0.79 } } },
    -- Slate: blue-grey, the satin sheen of its cleavage faces.
    { name = "boulder.slate", glossiness = 0.45, metallic = 0.05, colors = {
        { 0.17, 0.19, 0.22 }, { 0.24, 0.27, 0.30 }, { 0.32, 0.35, 0.39 }, { 0.41, 0.44, 0.48 },
        { 0.50, 0.53, 0.57 } } },
    -- Gneiss: dark bands of hornblende and biotite (glossy, a trace of metal), light bands of
    -- feldspar and quartz.
    { name = "boulder.gneissdark", glossiness = 0.4, metallic = 0.15, colors = {
        { 0.16, 0.15, 0.15 }, { 0.24, 0.22, 0.21 }, { 0.33, 0.30, 0.28 } } },
    { name = "boulder.gneisslight", glossiness = 0.2, colors = {
        { 0.62, 0.55, 0.50 }, { 0.73, 0.66, 0.60 }, { 0.83, 0.77, 0.71 } } },
    -- Ironstone: rust red to purple-brown, with a dull metallic lustre.
    { name = "boulder.ironstone", glossiness = 0.22, metallic = 0.25, colors = {
        { 0.22, 0.10, 0.07 }, { 0.33, 0.15, 0.09 }, { 0.45, 0.22, 0.12 }, { 0.56, 0.30, 0.16 },
        { 0.65, 0.40, 0.23 } } },
    -- Quartzite: white to pink, sugary and glossy.
    { name = "boulder.quartzite", glossiness = 0.5, colors = {
        { 0.62, 0.58, 0.56 }, { 0.73, 0.69, 0.66 }, { 0.83, 0.79, 0.76 }, { 0.91, 0.88, 0.85 } } },
    -- Quartz veins, in any rock: milky white, glassy.
    { name = "boulder.quartz", glossiness = 0.75, colors = { { 0.94, 0.94, 0.91 }, { 0.84, 0.85, 0.83 } } },
    -- Metal accents. Pyrite cubes (slate, limestone, basalt): brass gold. Specular haematite
    -- (ironstone): steel grey.
    { name = "boulder.pyrite",   color = { 0.78, 0.66, 0.32 }, glossiness = 0.85, metallic = 0.95 },
    { name = "boulder.haematite", color = { 0.30, 0.30, 0.32 }, glossiness = 0.75, metallic = 0.9 },
    -- Water streaks: dark, and still a little wet.
    { name = "boulder.stain", glossiness = 0.3, colors = {
        { 0.12, 0.12, 0.11 }, { 0.19, 0.18, 0.16 }, { 0.26, 0.25, 0.22 } } },
    -- Moss: deep shade green to sunlit yellow-green, and a dry golden-brown. Named moss.* so the tree
    -- brush (and anything else listing undergrowth) grows through it.
    { name = "moss.lush", colors = {
        { 0.05, 0.10, 0.04 }, { 0.09, 0.17, 0.05 }, { 0.15, 0.26, 0.06 }, { 0.23, 0.36, 0.08 },
        { 0.33, 0.46, 0.11 }, { 0.45, 0.55, 0.16 } } },
    { name = "moss.dry", colors = { { 0.30, 0.28, 0.10 }, { 0.44, 0.40, 0.15 }, { 0.58, 0.52, 0.22 } } },
    -- Crustose lichen: pale grey-green, and gold-orange.
    { name = "lichen.pale", colors = { { 0.71, 0.74, 0.63 }, { 0.58, 0.64, 0.52 } } },
    { name = "lichen.gold", colors = { { 0.81, 0.63, 0.18 }, { 0.70, 0.42, 0.12 } } },
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
local GOLDEN = 2.39996323
-- Under the host's 120000 ceiling, so a miscount is a refusal with a reason rather than the host's.
local BUDGET = 110000
-- What may be grown through: ground cover, as the tree brush lists it (`grass.lush`/`grass.dry` are
-- the painted ground, and stay off the list).
local UNDERGROWTH = {
    "grass.blade", "grass.stem", "grass.tuft",
    "clover.", "daisy.", "poppy.", "dandelion.",
    "buttercup", "bluebell", "fern.", "moss.", "flower.",
}

local function clamp01(v) return v < 0 and 0 or (v > 1 and 1 or v) end

local function isUndergrowth(name)
    if not name then return false end
    for i = 1, #UNDERGROWTH do
        local prefix = UNDERGROWTH[i]
        if name:sub(1, #prefix) == prefix then return true end
    end
    return false
end

-- A ramp step for `v` (0..1), dithered so neighbouring steps interleave in patches rather than meet
-- at a line.
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

-- A random unit vector from a hash, for planes, veins and bedding.
local function randomDir(seed, a, b)
    local u = pv.hash(a, b, 1, seed) * 2 - 1
    local phi = pv.hash(a, b, 2, seed) * TAU
    local r = sqrt(max(0, 1 - u * u))
    return r * cos(phi), u, r * sin(phi)
end

-- ---------------------------------------------------------------------------------------------
-- Shapes
-- ---------------------------------------------------------------------------------------------
--
-- Each takes a boulder (b.H its height, b.w its width factor, b.seed, b.rough) and returns a signed
-- distance over coordinates relative to the boulder's centre, unrotated, and its half extents.
-- The distances only need to be *bounds* that do not overestimate by much: the march skips by them.

-- A rounded box: half extents (w, h, d), corner radius rc.
local function roundBox(x, y, z, w, h, d, rc)
    local qx, qy, qz = abs(x) - w + rc, abs(y) - h + rc, abs(z) - d + rc
    local ox, oy, oz = max(qx, 0), max(qy, 0), max(qz, 0)
    return sqrt(ox * ox + oy * oy + oz * oz) + min(max(qx, qy, qz), 0) - rc
end

-- Rounded: a superellipsoid between an egg and a pillow, lumpy at its own scale, worn flat underneath.
-- A river stone, a glacial erratic, a weathered corestone.
local function roundedShape(b)
    local s = b.seed
    local a = b.H * (0.55 + 0.35 * pv.hash(1, 2, 3, s)) * b.w
    local c = b.H * (0.45 + 0.35 * pv.hash(4, 5, 6, s)) * b.w
    local h = b.H * 0.5
    local e = 2.0 + 1.3 * pv.hash(7, 8, 9, s)
    local r = min(a, h, c)
    local lump = r * 0.7 * b.rough
    local band = lump * 0.5 + 0.5 * b.rough + 1
    local f = 1 / (r * 1.5)
    -- Skewed: the top sits off the base's centre, so it is not a dome.
    local kx, kz = (pv.hash(1, 9, 3, s) - 0.5) * 0.6, (pv.hash(2, 9, 3, s) - 0.5) * 0.6
    return function(x, y, z)
        local t = (y + h) / (2 * h)
        x, z = x - kx * t * a, z - kz * t * c
        local u = (abs(x) / a) ^ e + (abs(y) / h) ^ e + (abs(z) / c) ^ e
        local d = (u ^ (1 / e) - 1.0) * r
        d = max(d, -(y + h * 0.78))
        if abs(d) > band then return d end
        return d + (pv.fbm(x * f + 7, y * f, z * f, 3, s + 31) - 0.5) * lump
                 + (pv.noise(x * 0.4, y * 0.4, z * 0.4, s + 33) - 0.5) * 0.5 * b.rough
    end, a, h, c, band
end

-- Angular: a convex block cut by fresh fracture planes spread over the sphere -- talus, quarry spoil,
-- a frost-shattered block. Planes, so the edges stay sharp; a little noise so the faces are not glass.
local function angularShape(b)
    local s = b.seed
    local a = b.H * (0.55 + 0.4 * pv.hash(1, 2, 3, s)) * b.w
    local c = b.H * (0.45 + 0.4 * pv.hash(4, 5, 6, s)) * b.w
    local h = b.H * 0.5
    local planes = {}
    local n = 8 + floor(pv.hash(7, 7, 7, s) * 7)
    for i = 0, n - 1 do
        local yy = 1 - 2 * (i + 0.5) / n
        local rr = sqrt(max(0, 1 - yy * yy))
        local phi = i * GOLDEN + pv.hash(i, 9, 0, s) * 1.2
        local nx, ny, nz = normalize(rr * cos(phi) + (pv.hash(i, 10, 0, s) - 0.5) * 0.5, yy,
                                     rr * sin(phi) + (pv.hash(i, 11, 0, s) - 0.5) * 0.5)
        local support = sqrt((a * nx) ^ 2 + (h * ny) ^ 2 + (c * nz) ^ 2)
        planes[#planes + 1] = { nx, ny, nz, support * (0.78 + 0.2 * pv.hash(i, 12, 0, s)) }
    end
    planes[#planes + 1] = { 0, -1, 0, h * 0.85 }
    local band = 1.2 * b.rough + 1
    return function(x, y, z)
        local d = -1e9
        for i = 1, #planes do
            local q = planes[i]
            local v = q[1] * x + q[2] * y + q[3] * z - q[4]
            if v > d then d = v end
        end
        if abs(d) > band then return d end
        return d + (pv.fbm(x * 0.12, y * 0.12, z * 0.12, 3, s + 21) - 0.5) * 1.2 * b.rough
                 + (pv.noise(x * 0.6, y * 0.6, z * 0.6, s + 23) - 0.5) * 0.35 * b.rough
    end, a * 1.05, h * 1.05, c * 1.05, band
end

-- Slab: a plate, its thickness the height, two to four beds with their rims set in or out, often
-- tilted -- a slab that fell and came to rest leaning.
local function slabShape(b)
    local s = b.seed
    local h = b.H * (0.18 + 0.1 * pv.hash(1, 1, 3, s))
    local W = b.H * (0.8 + 0.5 * pv.hash(1, 2, 3, s)) * b.w
    local D = b.H * (0.6 + 0.4 * pv.hash(4, 5, 6, s)) * b.w
    local beds = 2 + floor(pv.hash(7, 8, 9, s) * 3)
    local insets = {}
    for k = 1, beds do insets[k] = pv.hash(k, 3, 3, s) * max(0.8, h * 0.5) end
    local band = 1.8 * b.rough + 1
    local rc = min(W, D) * 0.3
    return function(x, y, z)
        local k = min(beds, max(1, floor((y + h) / (2 * h) * beds) + 1))
        local inset = insets[k]
        local d = roundBox(x, y, z, W - inset, h, D - inset, min(rc, h * 0.9))
        if abs(d) > band then return d end
        return d + (pv.fbm(x * 0.1, y * 0.3, z * 0.1, 3, s + 41) - 0.5) * 2.2 * b.rough
                 + (pv.noise(x * 0.6, y * 0.6, z * 0.6, s + 43) - 0.5) * 0.5 * b.rough
    end, W, h, D, band
end

-- Split: a rounded or angular boulder cracked through by frost, the halves eased apart. The crack
-- faces are fresh rock (b.crack tells the colouring where they are).
local function splitShape(b)
    local s = b.seed
    local base, a, h, c, band
    if pv.hash(3, 1, 4, s) < 0.6 then base, a, h, c, band = roundedShape(b) else base, a, h, c, band = angularShape(b) end
    local phi = pv.hash(5, 9, 2, s) * TAU
    local nx, ny, nz = normalize(cos(phi), (pv.hash(6, 5, 3, s) - 0.5) * 0.5, sin(phi))
    local g0 = max(0.8, b.H * (0.025 + 0.03 * pv.hash(5, 8, 9, s)))
    local open = 1.0 + 2.0 * pv.hash(5, 7, 9, s)       -- how far wider it is at the top than the foot
    b.crack = { nx, ny, nz, g0 * (1 + open) }
    return function(x, y, z)
        local sd = nx * x + ny * y + nz * z
        local sg = sd >= 0 and 1 or -1
        -- The halves lean apart: the gap widens toward the top.
        local g = g0 * (1 + open * clamp01((y + h) / (2 * h)))
        local d = base(x - sg * nx * g, y - sg * ny * g, z - sg * nz * g)
        local rough = (pv.noise(x * 0.3, y * 0.3, z * 0.3, s + 61) - 0.5) * 0.8 * b.rough
        return max(d, g - abs(sd) + rough)
    end, a + g0 * (1 + open), h, c + g0 * (1 + open), band + 0.5
end

-- Tor: two or three pillowed blocks stacked, each smaller and set off the one under it -- granite
-- woolsacks, weathered along their horizontal joints.
local function torShape(b)
    local s = b.seed
    local n = 2 + floor(pv.hash(1, 1, 9, s) * 2)
    local fr, total = {}, 0
    for i = 1, n do fr[i] = 1.0 - 0.25 * (i - 1) + 0.2 * pv.hash(i, 2, 9, s); total = total + fr[i] end
    local blocks, y0, ext = {}, -b.H * 0.5, 0
    for i = 1, n do
        local bh = b.H * fr[i] / total * 0.5
        local shrink = 1.0 - 0.18 * (i - 1)
        local w = b.H * (0.45 + 0.25 * pv.hash(i, 3, 9, s)) * b.w * shrink
        local d = b.H * (0.35 + 0.25 * pv.hash(i, 4, 9, s)) * b.w * shrink
        local ox = (pv.hash(i, 5, 9, s) - 0.5) * w * 0.5 * (i > 1 and 1 or 0)
        local oz = (pv.hash(i, 6, 9, s) - 0.5) * d * 0.5 * (i > 1 and 1 or 0)
        local yaw = (pv.hash(i, 7, 9, s) - 0.5) * 0.8
        blocks[i] = { ox, y0 + bh, oz, w, bh, d, cos(yaw), sin(yaw) }
        ext = max(ext, sqrt(w * w + d * d) + abs(ox) + abs(oz))
        y0 = y0 + bh * 2
    end
    local band = 1.4 * b.rough + 1
    return function(x, y, z)
        local d = 1e9
        for i = 1, #blocks do
            local q = blocks[i]
            local lx, lz = x - q[1], z - q[3]
            local rx, rz = lx * q[7] - lz * q[8], lx * q[8] + lz * q[7]
            local v = roundBox(rx, y - q[2], rz, q[4], q[5], q[6], min(q[5], q[4], q[6]) * 0.75)
            if v < d then d = v end
        end
        if abs(d) > band then return d end
        return d + (pv.fbm(x * 0.1, y * 0.1, z * 0.1, 3, s + 71) - 0.5) * 1.6 * b.rough
                 + (pv.noise(x * 0.5, y * 0.5, z * 0.5, s + 73) - 0.5) * 0.5 * b.rough
    end, ext, b.H * 0.5, ext, band
end

-- A flat-topped hexagon's 2D distance, radius r (centre to flat).
local function hexagon(px, pz, r)
    local kx, kz, kw = -0.8660254, 0.5, 0.57735027
    px, pz = abs(px), abs(pz)
    local dd = 2 * min(kx * px + kz * pz, 0)
    px, pz = px - dd * kx, pz - dd * kz
    local cx = max(-kw * r, min(kw * r, px))
    px, pz = px - cx, pz - r
    local l = sqrt(px * px + pz * pz)
    return pz < 0 and -l or l
end

-- Columns: a clump of hexagonal prisms of different heights, their tops broken off at an angle --
-- columnar jointing, as in a basalt flow (and in any rock, if you tick it for one).
local function columnsShape(b)
    local s = b.seed
    local r = max(1.5, b.H * (0.16 + 0.08 * pv.hash(1, 3, 5, s)) * b.w)
    local pitch = r * 2 * 0.97
    local cols, ext = {}, 0
    -- Hex grid rings, kept while inside a ragged radius.
    local reach = r * (2.2 + 1.6 * pv.hash(2, 3, 5, s))
    for q = -3, 3 do
        for rr = -3, 3 do
            local cx = pitch * (q + rr * 0.5)
            local cz = pitch * rr * 0.8660254
            local dist = sqrt(cx * cx + cz * cz)
            if dist <= reach * (0.75 + 0.5 * pv.hash(q, rr, 7, s)) or (q == 0 and rr == 0) then
                local top = b.H * (0.45 + 0.55 * pv.hash(q, rr, 8, s) * (1 - 0.35 * dist / reach)) - b.H * 0.5
                if q == 0 and rr == 0 then top = b.H * 0.5 end
                local tx, tz = (pv.hash(q, rr, 9, s) - 0.5) * 0.7, (pv.hash(q, rr, 10, s) - 0.5) * 0.7
                cols[#cols + 1] = { cx, cz, top, tx, tz }
                ext = max(ext, dist + r)
            end
        end
    end
    local h = b.H * 0.5
    local band = 0.8 * b.rough + 1
    local circum = r * 0.93 * 1.1548
    return function(x, y, z)
        local d = 1e9
        for i = 1, #cols do
            local c = cols[i]
            local px, pz = x - c[1], z - c[2]
            -- A column further than its circumradius plus the best distance so far cannot be nearer.
            local reachSq = (d > 0) and (d + circum) * (d + circum) or circum * circum
            if px * px + pz * pz <= reachSq then
            local side = hexagon(px, pz, r * 0.93)
            local cap = y - (c[3] + c[4] * px + c[5] * pz)
            local v = max(side, cap, -(y + h))
            if v < d then d = v end
            end
        end
        if abs(d) > band then return d end
        return d + (pv.noise(x * 0.5, y * 0.5, z * 0.5, s + 83) - 0.5) * 0.6 * b.rough
    end, ext, h + r * 0.7, ext, band
end

local SHAPES = {
    rounded = roundedShape, angular = angularShape, slab = slabShape,
    split = splitShape, tor = torShape, columns = columnsShape,
}

-- ---------------------------------------------------------------------------------------------
-- Rocks: relief (geometry near the surface) and colour
-- ---------------------------------------------------------------------------------------------
--
-- Relief takes coordinates relative to the boulder centre in world orientation (so "up" is up for
-- runnels and pits), adds to the distance, and declares its amplitude so the march knows its band.
-- Colour takes a context `c` -- local x,y,z, normal, absolute ax,ay,az for dithering, the boulder --
-- and returns a material index.

local ROCKS = {}

ROCKS.Granite = {
    ramp = "boulder.granite", amp = 0.5,
    relief = function(b, x, y, z)
        return (pv.noise(x * 0.7, y * 0.7, z * 0.7, b.seed + 101) - 0.5) * 0.45
    end,
    colour = function(c, b, p)
        local h = pv.hash(c.ax, c.ay, c.az, b.seed + 103)
        local fleck = 0.015 + 0.04 * p.minerals
        if h < fleck then return RAMP_START["boulder.mica"] end
        if h < fleck + 0.05 then
            return RAMP_START["boulder.feldspar"] + (b.pink and 0 or 1)
        end
        local v = 0.55 + (pv.fbm(c.x * 0.1, c.y * 0.1, c.z * 0.1, 3, b.seed + 105) - 0.5) * 0.35 + c.ny * 0.08
        -- Where a weathered sheet has flaked away, the rock beneath is fresher and paler.
        if pv.noise(c.x * 0.09, c.y * 0.09, c.z * 0.09, b.seed + 107) > 0.72 then v = v + 0.22 end
        return step("boulder.granite", clamp01(v + b.tone), c.ax, c.ay, c.az, b.seed + 109)
    end,
}

ROCKS.Basalt = {
    ramp = "boulder.basalt", amp = 1.6,
    relief = function(b, x, y, z)
        -- Vesicles: gas bubbles frozen in the lava, opened as pits.
        local f1 = pv.worley(x * 0.3, y * 0.3, z * 0.3, b.seed + 111)
        if f1 < 0.24 then return (0.24 - f1) * 6.5 end
        return 0
    end,
    colour = function(c, b, p)
        local f1 = pv.worley(c.x * 0.3, c.y * 0.3, c.z * 0.3, b.seed + 111)
        if f1 < 0.2 then return RAMP_START["boulder.basalt"] end           -- in a vesicle
        if p.minerals > 0 and pv.hash(c.ax, c.ay, c.az, b.seed + 113) < 0.012 * p.minerals then
            return RAMP_START["boulder.pyrite"]
        end
        -- The rind: brown on top, where the weather reaches.
        if c.ny > 0.6 and pv.fbm(c.x * 0.08, c.y * 0.08, c.z * 0.08, 3, b.seed + 115) > 0.75 - 0.2 * c.ny then
            return step("boulder.rind", 0.5 + (pv.noise(c.x * 0.3, c.y * 0.3, c.z * 0.3, b.seed + 117) - 0.5), c.ax, c.ay, c.az, b.seed + 119)
        end
        local v = 0.5 + (pv.fbm(c.x * 0.12, c.y * 0.12, c.z * 0.12, 3, b.seed + 121) - 0.5) * 0.5 + c.ny * 0.12
        return step("boulder.basalt", clamp01(v + b.tone), c.ax, c.ay, c.az, b.seed + 123)
    end,
}

ROCKS.Sandstone = {
    ramp = "boulder.sandstone", amp = 2.2,
    relief = function(b, x, y, z)
        local bx, by, bz = b.bed[1], b.bed[2], b.bed[3]
        local bed = bx * x + by * y + bz * z
        -- Differential erosion: hard beds stand out as ridges.
        local d = sin(bed * 1.1 + pv.noise(x * 0.1, y * 0.1, z * 0.1, b.seed + 131) * 3) * 0.35
        -- Tafoni: honeycomb hollows on the sides, not the top.
        if y < b.H * 0.3 then
            local f1 = pv.worley(x * 0.16, y * 0.22, z * 0.16, b.seed + 133)
            if f1 < 0.3 then d = d + (0.3 - f1) * 6.0 end
        end
        return d
    end,
    colour = function(c, b, p)
        local bed = b.bed[1] * c.x + b.bed[2] * c.y + b.bed[3] * c.z
        bed = bed + (pv.noise(c.x * 0.06, c.y * 0.06, c.z * 0.06, b.seed + 135) - 0.5) * 4.0
        local period = max(2.5, b.H * 0.14)
        local f = bed / period - floor(bed / period)
        if f < 0.22 and pv.noise(c.x * 0.1, c.y * 0.1, c.z * 0.1, b.seed + 137) > 0.35 then
            return step("boulder.sandband", clamp01(0.5 + (f - 0.11) * 3), c.ax, c.ay, c.az, b.seed + 139)
        end
        local v = 0.55 + (pv.fbm(c.x * 0.1, c.y * 0.1, c.z * 0.1, 3, b.seed + 141) - 0.5) * 0.4
                + sin(bed * 1.1) * 0.08 + c.ny * 0.1
        return step("boulder.sandstone", clamp01(v + b.tone), c.ax, c.ay, c.az, b.seed + 143)
    end,
}

ROCKS.Limestone = {
    ramp = "boulder.limestone", amp = 1.8,
    relief = function(b, x, y, z)
        local d = 0
        -- Karren: runnels down the upper sides, dissolved by rain.
        if y > -b.H * 0.2 then
            local r = pv.noise(x * 0.38, y * 0.035, z * 0.38, b.seed + 151)
            d = d + max(0, r - 0.45) * 2.2
        end
        -- Solution pits on top.
        if y > b.H * 0.25 then
            local f1 = pv.worley(x * 0.15, 0, z * 0.15, b.seed + 153)
            if f1 < 0.25 then d = d + (0.25 - f1) * 5.0 end
        end
        return d
    end,
    colour = function(c, b, p)
        if p.minerals > 0 and pv.hash(c.ax, c.ay, c.az, b.seed + 155) < 0.006 * p.minerals then
            return RAMP_START["boulder.pyrite"]
        end
        local v = 0.62 + (pv.fbm(c.x * 0.09, c.y * 0.09, c.z * 0.09, 3, b.seed + 157) - 0.5) * 0.45 + c.ny * 0.12
        -- Darker in the runnels, where water runs and lingers.
        if c.y > -b.H * 0.2 then
            v = v - max(0, pv.noise(c.x * 0.38, c.y * 0.035, c.z * 0.38, b.seed + 151) - 0.5) * 0.9
        end
        return step("boulder.limestone", clamp01(v + b.tone), c.ax, c.ay, c.az, b.seed + 159)
    end,
}

ROCKS.Slate = {
    ramp = "boulder.slate", amp = 0.6,
    relief = function(b, x, y, z)
        local f = b.bed[1] * x + b.bed[2] * y + b.bed[3] * z
        -- Cleavage: sharp parallel ridges.
        return (abs(sin(f * 0.8)) - 0.5) * 0.6
    end,
    colour = function(c, b, p)
        if p.minerals > 0 and pv.hash(c.ax, c.ay, c.az, b.seed + 161) < 0.015 * p.minerals then
            return RAMP_START["boulder.pyrite"]
        end
        local f = b.bed[1] * c.x + b.bed[2] * c.y + b.bed[3] * c.z
        local v = 0.5 + sin(f * 0.8) * 0.15 + (pv.fbm(c.x * 0.1, c.y * 0.1, c.z * 0.1, 3, b.seed + 163) - 0.5) * 0.35
        return step("boulder.slate", clamp01(v + b.tone), c.ax, c.ay, c.az, b.seed + 165)
    end,
}

ROCKS.Gneiss = {
    ramp = "boulder.gneisslight", amp = 0.5,
    relief = function(b, x, y, z)
        local f = b.bed[1] * x + b.bed[2] * y + b.bed[3] * z
        return sin(f * 0.45 + (pv.fbm(x * 0.06, y * 0.06, z * 0.06, 2, b.seed + 171) - 0.5) * 8) * 0.3
    end,
    colour = function(c, b, p)
        -- Banding, folded: the band coordinate is warped by low-frequency noise.
        local f = b.bed[1] * c.x + b.bed[2] * c.y + b.bed[3] * c.z
        f = f + (pv.fbm(c.x * 0.06, c.y * 0.06, c.z * 0.06, 2, b.seed + 171) - 0.5) * 18
        local band = sin(f * 0.45)
        local t = (pv.noise(c.x * 0.25, c.y * 0.25, c.z * 0.25, b.seed + 173) - 0.5) * 0.6
        if band + t < -0.1 then
            if pv.hash(c.ax, c.ay, c.az, b.seed + 175) < 0.04 * p.minerals then return RAMP_START["boulder.mica"] end
            return step("boulder.gneissdark", clamp01(0.5 + band * 0.3 + b.tone), c.ax, c.ay, c.az, b.seed + 177)
        end
        return step("boulder.gneisslight", clamp01(0.5 + band * 0.25 + c.ny * 0.1 + b.tone), c.ax, c.ay, c.az, b.seed + 179)
    end,
}

ROCKS.Ironstone = {
    ramp = "boulder.ironstone", amp = 1.3,
    relief = function(b, x, y, z)
        -- Knobbly: concretions standing out of a softer matrix.
        return (0.5 - abs(pv.noise(x * 0.25, y * 0.25, z * 0.25, b.seed + 181) - 0.5) * 2) * 1.2
    end,
    colour = function(c, b, p)
        if p.minerals > 0 and pv.noise(c.x * 0.5, c.y * 0.5, c.z * 0.5, b.seed + 183) > 0.86 - 0.08 * p.minerals then
            return RAMP_START["boulder.haematite"]
        end
        local knob = abs(pv.noise(c.x * 0.25, c.y * 0.25, c.z * 0.25, b.seed + 181) - 0.5) * 2
        local v = 0.55 - knob * 0.35 + (pv.fbm(c.x * 0.12, c.y * 0.12, c.z * 0.12, 3, b.seed + 185) - 0.5) * 0.35 + c.ny * 0.1
        return step("boulder.ironstone", clamp01(v + b.tone), c.ax, c.ay, c.az, b.seed + 187)
    end,
}

ROCKS.Quartzite = {
    ramp = "boulder.quartzite", amp = 0.3,
    relief = function(b, x, y, z)
        return (pv.noise(x * 0.5, y * 0.5, z * 0.5, b.seed + 191) - 0.5) * 0.3
    end,
    colour = function(c, b, p)
        local v = 0.55 + (pv.fbm(c.x * 0.1, c.y * 0.1, c.z * 0.1, 3, b.seed + 193) - 0.5) * 0.5 + c.ny * 0.1
        return step("boulder.quartzite", clamp01(v + b.tone), c.ax, c.ay, c.az, b.seed + 195)
    end,
}

local ROCK_NAMES = { "Granite", "Basalt", "Sandstone", "Limestone", "Slate", "Gneiss", "Ironstone", "Quartzite" }
local ROCK_OPTIONS = { "Mixed" }
for _, n in ipairs(ROCK_NAMES) do ROCK_OPTIONS[#ROCK_OPTIONS + 1] = n end

-- ---------------------------------------------------------------------------------------------
-- One boulder: build, march
-- ---------------------------------------------------------------------------------------------

local OFF, SPAN = 1024, 2048
local SPAN2 = SPAN * SPAN
-- Cell keys are linear in the coordinates, so a neighbour is key +-1, +-SPAN, +-SPAN2, and a boulder's
-- cells move into the group by adding one number.
local function K(x, y, z) return (x + OFF) + (y + OFF) * SPAN + (z + OFF) * SPAN2 end
local function GK(x, z) return (x + OFF) + (z + OFF) * SPAN end
-- Solid cells deeper than this are interior: kept as runs per column, not cell by cell, and written
-- only if the whole group fits the budget solid. Shallower ones are the shell, always written.
local SHELL = -2.5

-- The boulder's full distance: its shape, rotated (yaw, and a tilt for slabs), plus its rock's relief
-- near the surface.
local function buildBoulder(b, p)
    local shape, ex, ey, ez, band = SHAPES[b.shape](b)
    local rock = ROCKS[b.rock]
    local cy, sy = cos(b.yaw), sin(b.yaw)
    local ct, st = cos(b.tilt), sin(b.tilt)
    local W = p.weathering
    local ramp = rock.amp * W
    local relief = rock.relief
    band = band + ramp + 0.5
    b.dist = function(x, y, z)
        -- Into the shape's frame: yaw about y, then tilt about x.
        local rx, rz = x * cy - z * sy, x * sy + z * cy
        local ry = y * ct - rz * st
        rz = y * st + rz * ct
        local d = shape(rx, ry, rz)
        if W > 0 and abs(d) < band then d = d + relief(b, x, y, z) * W end
        return d
    end
    -- Bounds of the rotated box.
    local R = sqrt(ex * ex + ez * ez)
    b.X = floor(R + band) + 1
    b.Y = floor(abs(ey * ct) + abs(R * st) + band) + 1
    b.Rxz = R + band
    -- Its surface area, roughly (an ellipsoid's, Thomsen's formula): what its shell will cost, known
    -- before a cell is evaluated, so an oversized group is refused at once rather than timed out.
    local q = 1.6
    b.area = 4 * math.pi * (((ex * ey) ^ q + (ex * ez) ^ q + (ey * ez) ^ q) / 3) ^ (1 / q)
end

-- Marches the distance over the boulder's bounds, a column at a time, stepping by the distance where
-- it is far from the surface: most of the box is either well outside or deep inside, and evaluating
-- it cell by cell is most of what a large boulder would cost.
--
-- b.cells: local key -> distance, shell cells only. b.near: outside cells next to the surface, for
-- the gradient. b.cols: per column { i, k, lowest solid j, shell js, interior runs { j0, j1, ... } }.
local function march(b)
    local cells, near, cols = {}, {}, {}
    local dist, X, Y = b.dist, b.X, b.Y
    local R2 = (b.Rxz + 1) * (b.Rxz + 1)
    for k = -X, X do
        for i = -X, X do
            if i * i + k * k <= R2 then
                local j, lowest, shell, runs = -Y, nil, nil, nil
                local key0 = K(i, 0, k)
                while j <= Y do
                    local d = dist(i, j, k)
                    if d >= 0 then
                        if d < 3 then near[key0 + j * SPAN] = d end
                        j = j + ((d > 4) and floor((d - 2.5) * 0.5) + 1 or 1)
                    else
                        if not lowest then lowest = j end
                        if d >= SHELL then
                            cells[key0 + j * SPAN] = d
                            shell = shell or {}
                            shell[#shell + 1] = j
                            j = j + 1
                        else
                            -- Interior: this cell, and the cells the distance says can be skipped.
                            local s = (d < -4) and floor((-d - 2.5) * 0.5) + 1 or 1
                            local j1 = min(Y, j + s - 1)
                            runs = runs or {}
                            local n = #runs
                            if n > 0 and runs[n] == j - 1 then runs[n] = j1
                            else runs[n + 1] = j; runs[n + 2] = j1 end
                            j = j + s
                        end
                    end
                end
                if lowest then cols[#cols + 1] = { i, k, lowest, shell, runs } end
            end
        end
    end
    b.cells, b.near, b.cols = cells, near, cols
end

-- The boulder's own distance at a local key, from what the march stored: exact near the surface, and
-- missing only deep inside (every outside neighbour of a surface cell is in `near`). For gradients.
local function storedDistance(b, key)
    return b.cells[key] or b.near[key] or -3.5
end

-- ---------------------------------------------------------------------------------------------
-- The brush
-- ---------------------------------------------------------------------------------------------

local SHAPE_PARAMS = { "rounded", "angular", "slab", "split", "tor", "columns" }

return {
    name = "Boulders",
    kind = "scatter",
    description = "Boulders, alone or in groups: six shapes (rounded, angular, slab, split, tor, columns) "
               .. "in eight rocks, each with its own colours, sheen, metal flecks and weathering relief; "
               .. "rubble at the foot; moss and lichen. Each boulder rests on the ground under it. "
               .. "Force plants one group exactly where you click.",
    author = "ProjectV",

    needs = { "position", "normal", "material" },
    displaces = UNDERGROWTH,

    params = {
        { name = "force", label = "Force (one, exactly where I click)", type = "bool", default = false,
          tooltip = "Plant one boulder (or group) on the voxel under the cursor, skipping the spacing\n"
                 .. "grid, Coverage and the site tests. Cells already holding something are left as\n"
                 .. "they are rather than refusing the placement." },
        { name = "spacing", label = "Spacing", type = "int", default = 48, min = 2, max = 160,
          tooltip = "How far apart boulders (or groups) are, in voxels. A group needs about\n"
                 .. "Height x Group spread x 2 or neighbouring groups refuse each other." },
        { name = "density", label = "Coverage", type = "float", default = 0.4, min = 0.0, max = 1.0 },

        -- Shape: tick any; each boulder picks one.
        { name = "rounded", label = "Rounded", type = "bool", default = true,
          tooltip = "Egg to pillow, lumpy, worn flat underneath: river stones, glacial erratics." },
        { name = "angular", label = "Angular", type = "bool", default = true,
          tooltip = "A block cut by fresh fracture planes: talus, frost-shattered rock." },
        { name = "slab", label = "Slab", type = "bool", default = false,
          tooltip = "A plate of beds with stepped rims, often fallen and leaning. Height is its\n"
                 .. "thickness." },
        { name = "split", label = "Split", type = "bool", default = false,
          tooltip = "A boulder cracked through by frost, the halves eased apart." },
        { name = "tor", label = "Tor", type = "bool", default = false,
          tooltip = "Two or three pillowed blocks stacked: granite woolsacks." },
        { name = "columns", label = "Columns", type = "bool", default = false,
          tooltip = "A clump of hexagonal prisms of different heights: columnar basalt." },

        { name = "rock", label = "Rock", type = "enum", default = "Mixed", options = ROCK_OPTIONS,
          tooltip = "What the boulders are made of: colours, sheen, metal and weathering relief.\n\n"
                 .. "  Granite    speckled grey, black mica that glints, pink or white feldspar\n"
                 .. "  Basalt     near black, glassy, pitted with vesicles, a rusty rind on top\n"
                 .. "  Sandstone  tan with iron bands, ridged beds, honeycomb hollows\n"
                 .. "  Limestone  pale cream-grey, rain runnels and solution pits\n"
                 .. "  Slate      blue-grey, satin sheen, cleavage ridges, pyrite cubes\n"
                 .. "  Gneiss     folded dark and pale bands, the dark ones glossy\n"
                 .. "  Ironstone  rust red, knobbly, a metal lustre and steel haematite\n"
                 .. "  Quartzite  white to pink, smooth and glossy\n"
                 .. "  Mixed      each group (or boulder, with Mix rocks in a group) picks one" },
        { name = "mixRocks", label = "Mix rocks in a group", type = "bool", default = false,
          tooltip = "Rock = Mixed: each boulder of a group picks its own rock, as in a glacial till.\n"
                 .. "Off, a group shares one, as stones broken from one outcrop do." },
        { name = "tone", label = "Tone variety", type = "float", default = 0.5, min = 0.0, max = 1.0,
          tooltip = "How far each boulder's shade strays from its rock's middle: lighter and darker\n"
                 .. "stones side by side." },

        { name = "height", label = "Height", type = "int", default = 14, min = 2, max = 64,
          tooltip = "How tall the largest boulder is, in voxels. Everything else follows from it." },
        { name = "sizeVariety", label = "Size variety", type = "float", default = 0.5, min = 0.0, max = 1.0,
          tooltip = "How much the boulders' sizes vary: within a group, and from group to group.\n"
                 .. "0 makes every boulder the same height." },
        { name = "width", label = "Width", type = "float", default = 1.0, min = 0.4, max = 2.5,
          tooltip = "Width and depth as a multiple of what the shape gives for its height." },
        { name = "roughness", label = "Roughness", type = "float", default = 1.0, min = 0.0, max = 2.0,
          tooltip = "How lumpy the shapes are. 0 is a clean solid." },
        { name = "weathering", label = "Weathering", type = "float", default = 1.0, min = 0.0, max = 2.0,
          tooltip = "How deep each rock's own relief is cut: vesicles, honeycomb, runnels, ridges,\n"
                 .. "knobs. 0 leaves the shape bare." },
        { name = "minerals", label = "Minerals", type = "float", default = 0.5, min = 0.0, max = 1.0,
          tooltip = "Metallic and glinting flecks: mica in granite and gneiss, pyrite in slate,\n"
                 .. "limestone and basalt, haematite in ironstone." },
        { name = "veins", label = "Quartz veins", type = "float", default = 0.3, min = 0.0, max = 1.0,
          tooltip = "The chance a boulder carries glassy white quartz veins." },
        { name = "stains", label = "Water stains", type = "float", default = 0.4, min = 0.0, max = 1.0,
          tooltip = "Dark, damp streaks running down the sides from the top." },

        -- Groups.
        { name = "group", label = "Group size", type = "int", default = 1, min = 1, max = 12,
          tooltip = "How many boulders each placement is: one large one and the rest smaller,\n"
                 .. "by Size variety." },
        { name = "layout", label = "Group layout", type = "enum", default = "Cluster",
          options = { "Cluster", "Pile", "Line" },
          tooltip = "  Cluster  around the largest, nestled against it\n"
                 .. "  Pile     heaped on and against each other\n"
                 .. "  Line     a train along one direction, as a glacier or a slide leaves them" },
        { name = "spread", label = "Group spread", type = "float", default = 1.0, min = 0.3, max = 4.0,
          tooltip = "How far apart the boulders of a group sit, relative to their size." },
        { name = "rubble", label = "Rubble", type = "float", default = 0.3, min = 0.0, max = 1.0,
          tooltip = "Small stones scattered round the foot." },
        { name = "embed", label = "Sunk", type = "float", default = 0.15, min = 0.0, max = 0.6,
          tooltip = "How deep each boulder sits in the ground, as a fraction of its height." },

        -- Growth.
        { name = "moss", label = "Moss", type = "float", default = 0.3, min = 0.0, max = 1.0,
          tooltip = "How much of the stone is mossed: tops first, then the shaded side and low down." },
        { name = "mossDry", label = "Moss dryness", type = "float", default = 0.2, min = 0.0, max = 1.0,
          tooltip = "0 lush green, 1 dry golden-brown; between, patches of each." },
        { name = "mossDepth", label = "Moss depth", type = "int", default = 1, min = 0, max = 2,
          tooltip = "How many voxels thick dense moss stands off the stone. 0 paints it only." },
        { name = "mossNorth", label = "Moss on the shaded side", type = "float", default = 0.5, min = 0.0, max = 1.0,
          tooltip = "How strongly moss favours the -z side, the one out of the sun." },
        { name = "lichen", label = "Lichen", type = "float", default = 0.3, min = 0.0, max = 1.0,
          tooltip = "Crusts of pale grey-green and gold-orange on the open, sunny faces." },

        -- Site.
        { name = "onlyOn", label = "Only on material", type = "text", default = "",
          tooltip = "Empty: any solid ground. Otherwise only where the material under the site\n"
                 .. "contains this text." },
        { name = "maxSlope", label = "Steepest ground", type = "float", default = 0.6, min = 0.0, max = 1.0 },
        { name = "seed", type = "seed", default = 5 },
    },

    materials = MATERIALS,

    apply = function(ctx, p)
        local force = ctx.force
        if not force then
            if ctx.ny < 1.0 - p.maxSlope - 0.03 then return nil, "too steep" end
            if isUndergrowth(ctx.material) then return nil, "on undergrowth" end
            if p.onlyOn ~= "" and (not ctx.material or not string.find(ctx.material, p.onlyOn, 1, true)) then
                return nil, "not on " .. p.onlyOn
            end
            if not pv.fits(ctx.x, ctx.y + 1, ctx.z, ctx.x, ctx.y + max(2, floor(p.height * 0.6)), ctx.z) then
                return nil, "no headroom"
            end
        end

        local shapes = {}
        for _, s in ipairs(SHAPE_PARAMS) do if p[s] then shapes[#shapes + 1] = s end end
        if #shapes == 0 then return nil, "no shape ticked" end

        local seed = p.seed + floor(pv.hash(ctx.x, ctx.y, ctx.z, p.seed) * 100000)
        local function rnd(a, b) return pv.hash(a, b, 77, seed) end
        local function pickRock(i)
            if p.rock ~= "Mixed" then return p.rock end
            return ROCK_NAMES[1 + floor(rnd(i, 1) * #ROCK_NAMES) % #ROCK_NAMES]
        end
        local groupRock = pickRock(0)

        -- ---- 1. The group ----
        local V = p.sizeVariety
        local mainH = max(2, p.height * (1 - 0.3 * V * rnd(0, 2)))
        local count = p.group
        local boulders = {}
        for i = 1, count do
            local H = mainH
            if i > 1 then
                -- Satellites: smaller, more so the later they come.
                local u = 0.3 + 0.55 * rnd(i, 3) - 0.15 * (i - 1) / count
                H = max(2, mainH * (1 - V + V * u))
            end
            local b = {
                index = i, H = H, w = p.width * (0.85 + 0.3 * rnd(i, 4)), seed = seed + i * 7919,
                rough = p.roughness, shape = shapes[1 + floor(rnd(i, 5) * #shapes) % #shapes],
                rock = (p.mixRocks and i > 1) and pickRock(i) or groupRock,
                yaw = rnd(i, 6) * TAU, tilt = 0,
                tone = (rnd(i, 7) - 0.5) * 0.5 * p.tone,
                pink = rnd(i, 8) < 0.5,
            }
            if b.shape == "slab" then b.tilt = (rnd(i, 9) - 0.5) * 1.3 end
            b.bed = { randomDir(b.seed, 3, 3) }
            if b.rock == "Sandstone" then
                -- Bedding lies near level in a sandstone; anywhere in slate and gneiss.
                b.bed = { normalize(b.bed[1] * 0.3, 1, b.bed[3] * 0.3) }
            end
            if rnd(i, 10) < p.veins then
                local vx, vy, vz = randomDir(b.seed, 5, 5)
                b.vein = { vx, vy, vz, (rnd(i, 11) - 0.5) * H * 0.6, 0.5 + rnd(i, 12) * 0.9 }
            end
            boulders[#boulders + 1] = b
        end
        -- Rubble: small stones of the group's rock, round the feet of the boulders.
        local stones = count
        local pebbles = floor(p.rubble * (4 + 3 * count) + 0.5)
        for i = 1, pebbles do
            local j = 100 + i
            boulders[#boulders + 1] = {
                index = #boulders + 1, pebble = true, H = 1.2 + max(1.5, mainH * 0.14) * rnd(j, 1),
                w = 1.0, seed = seed + j * 7919, rough = 0.6,
                shape = rnd(j, 2) < 0.5 and "rounded" or "angular",
                rock = p.mixRocks and pickRock(j) or groupRock,
                yaw = rnd(j, 3) * TAU, tilt = 0, tone = (rnd(j, 4) - 0.5) * 0.5 * p.tone,
                pink = rnd(j, 5) < 0.5, bed = { 0, 1, 0 },
                beside = 1 + floor(rnd(j, 6) * stones) % stones,
            }
        end

        -- Refuse what cannot fit before building any of it: a shell about two and a half voxels deep
        -- over every boulder's surface is the least the group can cost.
        local estimate = 0
        for _, b in ipairs(boulders) do
            buildBoulder(b, p)
            -- Measured shell per unit of estimated area: lumpy shapes carry more surface than their box.
            local shellDepth = (b.shape == "rounded" or b.shape == "split") and 3.4 or 2.6
            estimate = estimate + b.area * shellDepth
        end
        if estimate > BUDGET then
            return nil, "too big for one placement (about " .. floor(estimate) .. " voxels): lower Height, Width or Group size"
        end

        -- ---- The ground, lazily, per column ----
        local groundOf = {}
        local reach = floor(mainH * 2) + 12
        local lastGround = 0
        local function ground(x, z)
            local key = GK(x, z)
            local g = groundOf[key]
            if g then return g end
            local wx, wz, y = ctx.x + x, ctx.z + z, lastGround
            if pv.solid(wx, ctx.y + y, wz) then
                while y < reach and pv.solid(wx, ctx.y + y + 1, wz) do y = y + 1 end
            else
                while y > -reach and not pv.solid(wx, ctx.y + y, wz) do y = y - 1 end
            end
            groundOf[key], lastGround = y, y
            return y
        end

        -- ---- 2, 3. March, drop, add ----
        -- owner: world key -> boulder, for shell cells. The shell cells are also listed, in the order
        -- they were added (sk/sx/sy/sz), which is the order they are written: fixed, so the same site
        -- always gives the same placement, and no sort is needed.
        local owner, top = {}, {}
        local sk, sx, sy, sz, sd = {}, {}, {}, {}, {}
        local fillRuns, fillCount = {}, 0
        local groupR = 0
        local lineDir = rnd(0, 20) * TAU
        local function place(b, cx, cz)
            local cols = b.cols
            local sum, gmax, bmax, n = 0, -1e9, -1e9, #cols
            for c = 1, n do
                local col = cols[c]
                local wx, wz, bj = cx + col[1], cz + col[2], col[3]
                local t = ground(wx, wz) + 1 - bj
                sum = sum + t
                if t > gmax then gmax = t end
                local u = top[GK(wx, wz)]
                if u and u - bj > bmax then bmax = u - bj end
            end
            local oyG = 0.5 * (sum / max(1, n) + gmax) - (b.pebble and 0.25 or p.embed) * b.H
            return floor(max(oyG, bmax) + 0.5), oyG, bmax
        end
        for bi = 1, #boulders do
            local b = boulders[bi]
            march(b)
            local cx, cz, oy = 0, 0, 0
            if bi == 1 then
                oy = place(b, 0, 0)
                groupR = b.Rxz
            else
                -- Where it goes: by layout, then pushed outward until it rests acceptably.
                local angle, dist0, ox, oz = nil, nil, 0, 0
                if b.pebble then
                    local host = boulders[b.beside]
                    ox, oz = host.cx or 0, host.cz or 0
                    angle = rnd(bi, 30) * TAU
                    dist0 = host.Rxz * (0.55 + 0.5 * rnd(bi, 31)) + b.Rxz
                elseif p.layout == "Line" then
                    angle = lineDir + (bi % 2 == 0 and 0 or math.pi) + (rnd(bi, 32) - 0.5) * 0.5
                    dist0 = (boulders[1].Rxz * 0.6 + b.Rxz) * (0.6 + 0.35 * floor(bi / 2)) * p.spread * 1.2
                else
                    angle = bi * GOLDEN + rnd(bi, 33) * 0.8
                    local near = p.layout == "Pile" and 0.45 or 0.85
                    dist0 = (boulders[1].Rxz * 0.6 + b.Rxz * 0.7) * near * (0.8 + 0.4 * rnd(bi, 34)) * p.spread
                end
                local ca, sa = cos(angle), sin(angle)
                for attempt = 0, 6 do
                    local d = dist0 + attempt * max(1.5, b.Rxz * 0.45)
                    cx, cz = floor(ox + ca * d + 0.5), floor(oz + sa * d + 0.5)
                    local oyG, bmax
                    oy, oyG, bmax = place(b, cx, cz)
                    -- Cluster and Line rest on the ground, only nestling against the others; a Pile
                    -- may sit on them, but not perch on a sliver.
                    local lift = bmax - oyG
                    local allowed = (p.layout == "Pile" and not b.pebble) and b.H * 0.9 or 1.0
                    if lift <= allowed then break end
                    if attempt == 6 then oy = nil end
                end
                if not b.pebble then groupR = max(groupR, sqrt(cx * cx + cz * cz) + b.Rxz) end
            end
            if oy then
                b.cx, b.oy, b.cz = cx, oy, cz
                b.delta = cx + oy * SPAN + cz * SPAN2    -- local key + delta = world key
                local cols = b.cols
                for c = 1, #cols do
                    local col = cols[c]
                    local wx, wz = col[1] + cx, col[2] + cz
                    local g = ground(wx, wz)
                    local base = K(wx, 0, wz)
                    local shell, runs = col[4], col[5]
                    local gk, t = GK(wx, wz), nil
                    if shell then
                        for n = 1, #shell do
                            local j = shell[n]
                            local wy = j + oy
                            if wy > g then
                                local wk = base + wy * SPAN
                                if not owner[wk] then
                                    owner[wk] = bi
                                    local m = #sk + 1
                                    sk[m], sx[m], sy[m], sz[m] = wk, wx, wy, wz
                                    if not t or wy > t then t = wy end
                                end
                            end
                        end
                    end
                    if runs then
                        for n = 1, #runs, 2 do
                            local y0, y1 = max(runs[n] + oy, g + 1), runs[n + 1] + oy
                            if y1 >= y0 then
                                fillRuns[#fillRuns + 1] = { base, y0, y1, bi }
                                fillCount = fillCount + (y1 - y0 + 1)
                            end
                        end
                    end
                    if t and (not top[gk] or t > top[gk]) then top[gk] = t end
                end
            end
        end
        if #sk == 0 then return nil, "nothing above ground" end

        -- ---- 4. Colour ----
        local groupTop = 1
        for _, t in pairs(top) do if t > groupTop then groupTop = t end end
        local voxels = {}
        local extra = {}
        local m, mossSeed = p.moss, seed + 501
        local function air(wk, x, y, z)
            return not owner[wk] and y > ground(x, z)
        end
        local function mossScore(x, y, z, nx, ny, nz)
            if m <= 0 then return -1 end
            local hA = y - ground(x, z)
            local low = 1 - clamp01(hA / max(3, groupTop))
            local score = 0.95 * max(0, ny) + p.mossNorth * 0.85 * max(0, -nz) - 0.7 * max(0, -ny)
                        + 0.35 * low
                        + (pv.fbm(x * 0.07, y * 0.07, z * 0.07, 3, mossSeed) - 0.5) * 1.5
                        + (pv.noise(x * 0.35, y * 0.35, z * 0.35, mossSeed + 1) - 0.5) * 0.35
            return score - (1.6 - 1.6 * m)
        end
        local function mossColour(x, y, z, ny, excess)
            local v = 0.3 + 0.35 * max(0, ny) + clamp01(excess) * 0.3
                    + (pv.noise(x * 0.2, y * 0.2, z * 0.2, mossSeed + 3) - 0.5) * 0.4
            if pv.noise(x * 0.05, y * 0.05, z * 0.05, mossSeed + 5) < p.mossDry * 1.1 - 0.05 then
                return step("moss.dry", clamp01(v), x, y, z, mossSeed + 7)
            end
            return step("moss.lush", clamp01(v), x, y, z, mossSeed + 9)
        end

        for n = 1, #sk do
            local wk, x, y, z = sk[n], sx[n], sy[n], sz[n]
            local bi = owner[wk]
            local b = boulders[bi]
            local rock = ROCKS[b.rock]
            local material
            local open = air(wk + 1, x + 1, y, z) or air(wk - 1, x - 1, y, z) or
                         air(wk + SPAN, x, y + 1, z) or air(wk - SPAN, x, y - 1, z) or
                         air(wk + SPAN2, x, y, z + 1) or air(wk - SPAN2, x, y, z - 1)
            if not open then
                material = RAMP_START[rock.ramp] + floor(RAMP_STEPS[rock.ramp] / 2)
            else
                local i, j, k = x - b.cx, y - b.oy, z - b.cz
                local lk = wk - b.delta
                local nx, ny, nz = normalize(storedDistance(b, lk + 1) - storedDistance(b, lk - 1),
                                             storedDistance(b, lk + SPAN) - storedDistance(b, lk - SPAN),
                                             storedDistance(b, lk + SPAN2) - storedDistance(b, lk - SPAN2))
                local c = { x = i, y = j, z = k, nx = nx, ny = ny, nz = nz, ax = ctx.x + x, ay = ctx.y + y, az = ctx.z + z }
                -- Moss first: it covers whatever is under it.
                local excess = mossScore(x, y, z, nx, ny, nz)
                if excess > 0 or (excess > -0.08 and pv.hash(x, y, z, mossSeed + 11) < (excess + 0.08) / 0.08) then
                    material = mossColour(x, y, z, ny, excess)
                    -- A cushion: the moss stands off the stone, along the face's outward axis.
                    if p.mossDepth > 0 and excess > 0.22 and not b.pebble then
                        local ux, uy, uz = 0, 0, 0
                        if ny > 0.55 then uy = 1
                        elseif abs(nx) > abs(nz) then ux = nx > 0 and 1 or -1
                        else uz = nz > 0 and 1 or -1 end
                        for t = 1, (excess > 0.5 and p.mossDepth or 1) do
                            local ex, ey, ez = x + ux * t, y + uy * t, z + uz * t
                            local ek = K(ex, ey, ez)
                            if extra[ek] or not air(ek, ex, ey, ez) then break end
                            -- Its colour from its own cell, so whichever stone cell grows it first,
                            -- the answer is the same.
                            local em = mossColour(ex, ey, ez, 1, 0.4)
                            extra[ek] = em
                            voxels[#voxels + 1] = { ex, ey, ez, em }
                        end
                    end
                end
                -- Lichen: crusts on the open faces that see the sky or the sun (+z).
                if not material and p.lichen > 0 and (ny > 0.1 or nz > 0.4) then
                    local f1 = pv.worley(c.ax * 0.22, c.ay * 0.22, c.az * 0.22, seed + 601)
                    if f1 < 0.08 + 0.3 * p.lichen * max(ny, nz * 0.7) then
                        local gold = pv.noise(c.ax * 0.04, c.ay * 0.04, c.az * 0.04, seed + 603) > 0.55
                        material = step(gold and "lichen.gold" or "lichen.pale", clamp01(f1 * 4), c.ax, c.ay, c.az, seed + 605)
                    end
                end
                -- Water stains: streaks down the sides, strongest from the top edge.
                if not material and p.stains > 0 and abs(ny) < 0.65 then
                    local u = (abs(nx) > abs(nz)) and c.z or c.x
                    local streak = pv.noise(u * 0.6 + b.index * 13.0, c.y * 0.03, 0.5, b.seed + 611)
                    local fromTop = clamp01(1.0 - (b.H * 0.5 - j) / b.H)
                    local s = (streak - 0.72 + 0.25 * p.stains) / 0.22 * (0.3 + 0.9 * fromTop)
                    if s > 0 and pv.hash(c.ax, c.ay, c.az, b.seed + 613) < s then
                        material = step("boulder.stain", clamp01(1.0 - s), c.ax, c.ay, c.az, b.seed + 615)
                    end
                end
                -- Quartz veins.
                if not material and b.vein then
                    local v = b.vein
                    local w = abs(v[1] * i + v[2] * j + v[3] * k - v[4])
                            + (pv.noise(i * 0.15, j * 0.15, k * 0.15, b.seed + 621) - 0.5) * 1.5
                    if w < v[5] then
                        material = step("boulder.quartz", clamp01(w / v[5]), c.ax, c.ay, c.az, b.seed + 623)
                    end
                end
                -- The rock, paler on the fresh faces of a split.
                if not material then
                    local fresh = 0
                    if b.crack then
                        local cr = b.crack
                        if abs(cr[1] * i + cr[2] * j + cr[3] * k) < cr[4] + 1.3 then fresh = 0.18 end
                    end
                    b.tone = b.tone + fresh
                    material = rock.colour(c, b, p)
                    b.tone = b.tone - fresh
                end
            end
            voxels[#voxels + 1] = { x, y, z, material }
        end

        -- The interior, solid, if the whole group fits; otherwise the shell is what is written -- a
        -- boulder broken open is hollow past its outer few voxels.
        if #voxels + fillCount <= BUDGET then
            local filled = {}
            for n = 1, #fillRuns do
                local r = fillRuns[n]
                local base, b = r[1], boulders[r[4]]
                local material = RAMP_START[ROCKS[b.rock].ramp] + floor(RAMP_STEPS[ROCKS[b.rock].ramp] / 2)
                local x = base % SPAN - OFF
                local z = floor(base / SPAN2) - OFF
                for y = r[2], r[3] do
                    local wk = base + y * SPAN
                    if not owner[wk] and not extra[wk] and not filled[wk] then
                        filled[wk] = true
                        voxels[#voxels + 1] = { x, y, z, material }
                    end
                end
            end
        end

        if #voxels > BUDGET then
            return nil, "too big for one placement (" .. #voxels .. " voxels): lower Height, Width or Group size"
        end
        return voxels
    end,
}
