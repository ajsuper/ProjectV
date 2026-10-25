-- Trees -- a scatter brush, and the one that leans hardest on the fit test.
--
-- A blade of grass is three voxels and fits almost anywhere. A tree is a trunk and a crown that may
-- be a thousand voxels across, and the interesting question stops being "what does it look like" and
-- becomes **"is there room for it here"**. Three different answers to that are used below, cheapest
-- first, because most candidate sites are rejected and the cheap tests are what keep a dab affordable:
--
--   1. the ground itself -- slope, and whether the base is standing on anything (`footing`);
--   2. a thin column straight up, which catches the common case of a low ceiling for the cost of a
--      dozen cells;
--   3. the whole crown box plus `clearance`, via pv.fits, and only when clearance is asked for.
--
-- On top of all three, the editor refuses any placement whose voxels would land in occupied cells --
-- all or nothing, so a tree never appears with half its crown inside a wall. That check is exact and
-- automatic; the ones here exist to reject a site *before* a few thousand voxels are generated for it,
-- and to demand room the placement itself does not occupy.
--
--
-- ## How a tree is built, and why it is not a stick with a ball on it
--
-- The first version of this brush drew a one-voxel trunk and one hash-eroded sphere. Rendered, that
-- is a lollipop: a wire holding a primitive, and at a large trunk height it is a *long* wire holding a
-- primitive, because nothing about it was proportional to anything else. The conifer was the same
-- mistake with a cone. Three things fix it, and all three matter:
--
--   * **A trunk has a radius**, and that radius comes from the height. This is the whole of the
--     "doesn't scale up" complaint: a 35-voxel tree with a 1-voxel trunk is a wire whatever else is
--     done to it, and no amount of canopy detail hides it.
--   * **The crown is many lobes, not one ball.** Foliage is hung at the tips of a branching skeleton,
--     so the silhouette is the union of eight-odd overlapping blobs and reads as irregular. One
--     sphere, however eroded at the rim, reads as a sphere -- erosion changes the texture of an
--     outline, not its shape.
--   * **Lobes are shells, and their radius is warped by fbm rather than constant.** The shell is what
--     makes a large tree affordable at all (see the budget note below); the warp is what stops each
--     individual lobe from being the same primitive at a smaller size.
--
-- ## The budget, which is why this brush can be told to make a 200-voxel tree
--
-- The host caps one placement at 20000 voxels and refuses the whole thing past it, so a brush that
-- generates first and hopes is a brush that fails outright at large settings. Instead the skeleton is
-- built first (it is cheap and it is the part that must not be thinned), what is left of the budget is
-- measured, and the foliage is *fitted to it*: thinner leaves first, and a smaller crown once thinner
-- leaves would stop reading as foliage at all. So the size controls go up to something useful for a
-- high-resolution component instead of topping out at a tree that suited one voxel scale.

-- **Limbs are a different material from the trunk, and that is a placement decision rather than a
-- shading one.** They shade the same and could have shared one ramp, which is what they used to do --
-- but `displaces` is matched by material name, so sharing a name means sharing an answer to "may a
-- neighbouring tree grow through this", and trunks and branches need opposite answers. Two trunks in
-- one cell is a mistake; two crowns whose branches cross is what a wood looks like.
--
-- ## The palette
--
-- **Hue-shifted ramps, not straight lines.** A ramp interpolated from one colour to another only
-- changes value, and foliage shaded that way looks like one green with the lights turned down. Painted
-- foliage -- and real foliage under a blue sky -- shifts hue across its ramp: the shadows go cooler and
-- bluer (lit by sky, not sun), the highlights warmer and yellower (lit by sun). So the ramps that carry
-- most of a tree are given stop by stop (`colors`), each step turned a little round the wheel. Bark gets
-- the same treatment in miniature: a cool, nearly purple-grey underside and a warm grey-brown top.
--
-- Every colour must also differ from every other material's, step by step -- see the note on the
-- branch ramps below.
local MATERIALS = {
    -- The trunk. Darkest at the root, where it is shaded and wet.
    { name = "tree.bark", colors = {
        { 0.16, 0.12, 0.12 }, { 0.25, 0.18, 0.15 }, { 0.36, 0.27, 0.20 }, { 0.48, 0.38, 0.28 } } },
    -- Broadleaf foliage, the same hue shift as the needles but brighter and yellower: deep blue-green
    -- shade inside the crown to a warm yellow-green where the sun hits the top of a clump.
    { name = "tree.leaf", colors = {
        { 0.06, 0.14, 0.10 }, { 0.11, 0.24, 0.11 }, { 0.20, 0.36, 0.13 }, { 0.34, 0.50, 0.15 },
        { 0.55, 0.64, 0.24 } } },
    -- Needles: teal-black in the shade under a tier, through blue-green, to a warm yellow-green where
    -- the sun catches the top of a spray. Darker overall than broadleaf foliage, as conifers are.
    { name = "tree.needle", colors = {
        { 0.035, 0.10, 0.10 }, { 0.06, 0.17, 0.14 }, { 0.11, 0.26, 0.17 }, { 0.20, 0.36, 0.19 },
        { 0.34, 0.47, 0.22 } } },
    { name = "tree.blossom",            color = { 0.94, 0.76, 0.82 } },
    { name = "tree.dead",    steps = 2, color = { 0.30, 0.26, 0.22 }, colorTo = { 0.46, 0.42, 0.37 } },
    -- These exist to be *named* differently -- see the note at the top -- but they must also be
    -- *coloured* differently, and that is a hard requirement rather than a preference: interning a
    -- role falls back to matching an existing entry by colour when the name is new, so a ramp that
    -- duplicated tree.bark's colours exactly would bind to tree.bark and never become an entry of its
    -- own. Younger wood is lighter and greyer than trunk bark, so the shift these need in order to
    -- exist is one they wanted anyway.
    { name = "tree.branch", colors = {
        { 0.20, 0.15, 0.14 }, { 0.31, 0.23, 0.17 }, { 0.44, 0.34, 0.24 } } },
    { name = "tree.deadlimb", steps = 2, color = { 0.35, 0.31, 0.27 }, colorTo = { 0.53, 0.49, 0.44 } },
    -- Season = Autumn: the leaf ramp's replacement. Shadow is a deep oxblood, light a warm gold, and
    -- each clump picks its own stretch of it so a crown comes out mixed reds and yellows.
    { name = "tree.autumn", colors = {
        { 0.22, 0.06, 0.05 }, { 0.42, 0.11, 0.05 }, { 0.66, 0.27, 0.07 }, { 0.83, 0.47, 0.12 },
        { 0.93, 0.70, 0.26 } } },

    -- ---- Pinyon-juniper woodland (the Juniper, Pinyon, Snag and Sagebrush kinds) ----
    -- Appended, so every ramp above keeps its index and the other kinds come out as before.
    --
    -- Utah juniper's scale foliage is an olive, slightly grey green -- yellower and duller than a
    -- spruce's needles -- going blue-grey in the shade.
    { name = "tree.juniper", colors = {
        { 0.14, 0.17, 0.12 }, { 0.23, 0.28, 0.17 }, { 0.34, 0.39, 0.23 }, { 0.47, 0.51, 0.31 },
        { 0.61, 0.63, 0.42 } } },
    -- Pinyon needles: a deeper, truer green than the juniper beside it.
    { name = "tree.pinyon", colors = {
        { 0.04, 0.09, 0.08 }, { 0.07, 0.15, 0.11 }, { 0.12, 0.23, 0.14 }, { 0.20, 0.32, 0.17 },
        { 0.31, 0.42, 0.21 } } },
    -- Juniper strip bark: long fibrous strips, red-brown where freshly exposed and weathered grey on
    -- the outside. The ramp runs dark, red-brown, grey; the trunk picks along it by stripe.
    { name = "tree.stripbark", colors = {
        { 0.17, 0.13, 0.12 }, { 0.30, 0.21, 0.17 }, { 0.44, 0.31, 0.24 }, { 0.52, 0.45, 0.39 },
        { 0.63, 0.58, 0.52 } } },
    { name = "tree.juniperlimb", colors = {
        { 0.24, 0.18, 0.15 }, { 0.38, 0.29, 0.23 }, { 0.52, 0.46, 0.40 } } },
    -- Pinyon bark: scaly, grey-brown with a little red.
    { name = "tree.pinyonbark", colors = {
        { 0.19, 0.15, 0.14 }, { 0.29, 0.23, 0.20 }, { 0.40, 0.32, 0.26 }, { 0.50, 0.42, 0.34 } } },
    { name = "tree.pinyonlimb", colors = {
        { 0.25, 0.20, 0.17 }, { 0.36, 0.29, 0.24 }, { 0.46, 0.39, 0.32 } } },
    -- Dead juniper wood: weathered silver, warm in the light. Stands for centuries.
    { name = "tree.silver", colors = {
        { 0.33, 0.31, 0.29 }, { 0.47, 0.45, 0.42 }, { 0.62, 0.60, 0.56 }, { 0.76, 0.74, 0.69 } } },
    -- Juniper berries: dusty blue, a few on the outside of the foliage.
    { name = "tree.berry", color = { 0.42, 0.50, 0.63 } },
    -- Sagebrush: silver grey-green leaves on twisted grey-brown stems.
    { name = "tree.sage", colors = {
        { 0.27, 0.31, 0.27 }, { 0.40, 0.45, 0.38 }, { 0.53, 0.58, 0.49 }, { 0.66, 0.70, 0.60 } } },
    { name = "tree.sagestem", colors = { { 0.30, 0.25, 0.21 }, { 0.42, 0.37, 0.31 } } },
}

-- Where each material's ramp starts in the expanded list `apply` returns indices into, worked out
-- from the table rather than written by hand -- a ramp gaining a step used to mean renumbering every
-- constant after it, and getting one wrong shades a tree in the next material's colours.
local RAMP_START, RAMP_STEPS = {}, {}
do
    local index = 1
    for _, material in ipairs(MATERIALS) do
        local steps = material.colors and #material.colors or (material.steps or 1)
        RAMP_START[material.name], RAMP_STEPS[material.name] = index, steps
        index = index + steps
    end
end
local BARK     = RAMP_START["tree.bark"]       -- Root to crown. The trunk, and nothing else.
local LEAF     = RAMP_START["tree.leaf"]       -- Inside to outside.
local NEEDLE   = RAMP_START["tree.needle"]     -- Shade to sun.
local BLOSSOM  = RAMP_START["tree.blossom"]
local DEAD     = RAMP_START["tree.dead"]       -- A bare tree's trunk.
local BRANCH   = RAMP_START["tree.branch"]     -- Limbs and surface roots.
local DEADLIMB = RAMP_START["tree.deadlimb"]   -- The same, on a bare tree.
local AUTUMN   = RAMP_START["tree.autumn"]     -- Season = Autumn's leaves.
local BARK_STEPS, NEEDLE_STEPS, BRANCH_STEPS =
    RAMP_STEPS["tree.bark"], RAMP_STEPS["tree.needle"], RAMP_STEPS["tree.branch"]
local LEAF_STEPS, AUTUMN_STEPS = RAMP_STEPS["tree.leaf"], RAMP_STEPS["tree.autumn"]
local DEAD_STEPS, DEADLIMB_STEPS = RAMP_STEPS["tree.dead"], RAMP_STEPS["tree.deadlimb"]
local JUNIPER, PINYON = RAMP_START["tree.juniper"], RAMP_START["tree.pinyon"]
local STRIPBARK, JUNIPERLIMB = RAMP_START["tree.stripbark"], RAMP_START["tree.juniperlimb"]
local PINYONBARK, PINYONLIMB = RAMP_START["tree.pinyonbark"], RAMP_START["tree.pinyonlimb"]
local SILVER, BERRY = RAMP_START["tree.silver"], RAMP_START["tree.berry"]
local SAGE, SAGESTEM = RAMP_START["tree.sage"], RAMP_START["tree.sagestem"]

local TAU = 6.28318530718

-- What one placement may cost, kept under the host's own ceiling so a miscount is a slightly smaller
-- tree rather than a refused one.
local BUDGET = 110000

local floor, sqrt, cos, sin, max, min, abs = math.floor, math.sqrt, math.cos, math.sin,
                                             math.max, math.min, math.abs

-- ---------------------------------------------------------------------------------------------
-- Undergrowth
-- ---------------------------------------------------------------------------------------------
--
-- What a tree is allowed to grow through, and what it refuses to stand on top of. The same list does
-- both jobs, because they are the same fact about a material: a blade of grass is not somewhere a
-- trunk can be rooted, and it is not something that should stop one.
--
-- **Without this a field of grass refuses every tree.** The host's fit test is all or nothing over
-- the placement's cells, so one blade anywhere in a trunk's column is a refusal, and the brush's own
-- headroom check -- a column straight up from the site -- starts inside whatever is growing on that
-- site. Both come back "no room" on ground that is visibly open.
--
-- **`grass.lush` and `grass.dry` are deliberately absent.** They are what the Grass Tint brush paints
-- the *ground* with -- terrain, not undergrowth -- and a tree that displaced them would carve a hole
-- in the field it is standing in. It is the reason this is a list of specific names rather than the
-- bare `grass.` prefix, which is the obvious thing to write and is wrong.
--
-- Extend it for your own ground cover: a prefix matches a whole ramp, so one entry covers every step
-- of one plant.
local UNDERGROWTH = {
    "grass.blade", "grass.stem", "grass.tuft",
    "clover.", "daisy.", "poppy.", "dandelion.",
    "buttercup", "bluebell", "fern.", "moss.", "flower.",
}

-- On top of the undergrowth, a tree may grow through *other trees' foliage* -- and this is what makes
-- a wood possible at all.
--
-- The fit test is all or nothing, so before this one leaf of a neighbour anywhere inside a crown
-- refused the entire tree. Two trees could therefore never stand closer than the sum of their crown
-- radii, at any spacing, at any coverage: the lattice offered the sites and every one of them came
-- back "no room". A dense planting was not a matter of turning the settings up, it was unreachable.
--
-- Foliage and limbs. `tree.bark` and `tree.dead` -- the trunks, and only the trunks -- are kept off
-- the list on purpose: crowns and branches that interleave are a wood, trunks that interpenetrate are
-- a mistake, and the distinction is the whole reason this is a list of names rather than a switch.
--
-- Limbs earn their place here on measurement rather than taste. With foliage displaceable and limbs
-- not, every remaining refusal in a dense planting was a limb against a limb -- branches reach most of
-- the crown's radius, so any spacing under about twice that refused, which is the same wall one step
-- further out.
local CANOPY = { "tree.leaf", "tree.autumn", "tree.needle", "tree.blossom", "tree.branch", "tree.deadlimb",
                 "tree.juniper", "tree.pinyon", "tree.berry", "tree.juniperlimb", "tree.pinyonlimb",
                 "tree.sage", "tree.sagestem" }

local DISPLACES = {}
for _, name in ipairs(UNDERGROWTH) do DISPLACES[#DISPLACES + 1] = name end
for _, name in ipairs(CANOPY) do DISPLACES[#DISPLACES + 1] = name end

local function isUndergrowth(name)
    if not name then return false end
    for i = 1, #UNDERGROWTH do
        local prefix = UNDERGROWTH[i]
        if name:sub(1, #prefix) == prefix then return true end
    end
    return false
end

local function clamp01(v) return v < 0 and 0 or (v > 1 and 1 or v) end

-- ---------------------------------------------------------------------------------------------
-- The voxel set
-- ---------------------------------------------------------------------------------------------
--
-- Deduplicated, and that is not tidiness. Limbs overlap their parent at every joint and lobes overlap
-- each other by design, so the same cell is offered many times over -- and every repeat is a slot of
-- the placement budget spent on a voxel that was already there. The old brush emitted its duplicates
-- and paid for them.
--
-- The budget is enforced here rather than checked by each generator, so there is one place that can
-- say no and no way for a caller to forget to ask.

local Tree = {}
Tree.__index = Tree

local function newTree()
    return setmetatable({ voxels = {}, seen = {}, spent = 0, full = false }, Tree)
end

function Tree:plot(x, y, z, material)
    -- Never below the surface the tree stands on. The site itself is solid ground, and a placement
    -- that reaches into it is one the host's all-or-nothing fit test throws away whole.
    if y < 1 then return end
    local key = (x + 1024) + (y + 1024) * 2048 + (z + 1024) * 4194304
    if self.seen[key] then return end
    if self.spent >= BUDGET then self.full = true return end
    self.seen[key] = true
    self.spent = self.spent + 1
    self.voxels[self.spent] = { x, y, z, material }
end

-- A filled disc in the XZ plane: the trunk's cross-section. Discs rather than spheres because the
-- trunk is near vertical, and one disc per level is a quarter of the iterations a swept ball costs
-- for the same result.
--
-- `flare` spreads the disc unevenly by azimuth, which is what makes the base of a big tree read as
-- buttress roots rather than as a cone. Cheap: one atan and one cos per cell of a disc that is only
-- large near the ground.
-- `wall`, when the trunk is thick enough for it to mean anything, makes the disc a ring: the inside of
-- a trunk is sealed by the trunk, so filling it buys nothing anyone can see and costs the square of
-- the radius rather than the first power of it. This is what decides whether a large tree can exist
-- inside the placement budget at all -- a solid trunk of radius 20 is 1250 voxels a level, a walled one
-- is 350 -- and it is safe for exactly the reason it is invisible: nothing renders, picks or fits
-- against a cavity with no opening. The ring closes itself back into a solid disc as the taper brings
-- the radius down to the wall thickness, so the top of the trunk is capped without a special case.
function Tree:disc(cx, y, cz, radius, material, flare, phase, wall)
    if radius < 0.75 then self:plot(cx, y, cz, material) return end
    local spread = flare and flare > 0 and flare or 0
    local extent = floor(radius * (1.0 + spread) + 0.5)
    local plain = radius * radius
    local hollow = 0
    if wall and wall > 0 and radius - wall > 1.2 then
        hollow = (radius - wall) * (radius - wall)
    end
    for dz = -extent, extent do
        for dx = -extent, extent do
            local d2 = dx * dx + dz * dz
            if d2 >= hollow then
                local limit = plain
                if spread > 0 and d2 > 0 then
                    local lobe = 0.5 + 0.5 * cos(math.atan(dz, dx) * 3.0 + phase)
                    local r = radius * (1.0 + spread * lobe)
                    limit = r * r
                end
                if d2 <= limit then self:plot(cx + dx, y, cz + dz, material) end
            end
        end
    end
end

function Tree:ball(cx, cy, cz, radius, material)
    if radius < 0.75 then self:plot(cx, cy, cz, material) return end
    local extent = floor(radius + 0.5)
    local limit = radius * radius
    for dy = -extent, extent do
        for dz = -extent, extent do
            for dx = -extent, extent do
                if dx * dx + dy * dy + dz * dz <= limit then
                    self:plot(cx + dx, cy + dy, cz + dz, material)
                end
            end
        end
    end
end

-- ---------------------------------------------------------------------------------------------
-- Vectors
-- ---------------------------------------------------------------------------------------------

local function normalize(x, y, z)
    local length = sqrt(x * x + y * y + z * z)
    if length < 1e-6 then return 0, 1, 0 end
    return x / length, y / length, z / length
end

-- Two unit vectors perpendicular to `d` and to each other. Used to aim a child branch: a cone around
-- the parent's direction needs a frame to measure the cone's azimuth in, and any frame will do as
-- long as it is perpendicular -- the azimuth itself is chosen by hash below.
local function basisOf(dx, dy, dz)
    local ux, uy, uz = 0, 1, 0
    if abs(dy) > 0.9 then ux, uy, uz = 1, 0, 0 end
    local ax, ay, az = normalize(dy * uz - dz * uy, dz * ux - dx * uz, dx * uy - dy * ux)
    local bx, by, bz = dy * az - dz * ay, dz * ax - dx * az, dx * ay - dy * ax
    return ax, ay, az, bx, by, bz
end

-- ---------------------------------------------------------------------------------------------
-- Limbs
-- ---------------------------------------------------------------------------------------------

-- One tapering tube along a curving path, stamped as overlapping balls. Steps of half a voxel, which
-- is what guarantees no gaps at the thin end where the "ball" is a single cell.
--
-- `curl` bends the limb toward (cx, cy, cz) as it goes -- upward for a broadleaf's branches, which is
-- what gives them their reach-for-the-light shape, and downward for a conifer's, which is what makes
-- its whorls droop. Applied per step so the bend is a curve rather than a kink.
--
-- `floorY`, when given, stops the path descending below that height without stopping it travelling.
-- A root is the case: it leaves the trunk heading down and out, and what it must do on reaching the
-- surface is *run along* it. Left to keep descending it goes under the ground, where every voxel of
-- it is discarded by plot's y >= 1 rule -- so the whole feature came out as nothing but a slightly
-- wider trunk base, the part near the trunk having been deduplicated into the trunk itself. Clamping
-- the position rather than the direction is what turns the dive into an arc that flattens out.
--
-- Returns the tip and the direction it arrived travelling in, so a child can carry on from there.
function Tree:limb(x, y, z, dx, dy, dz, length, radius0, radius1, cx, cy, cz, curl,
                   material, steps, floorY)
    dx, dy, dz = normalize(dx, dy, dz)
    local walk = max(2, floor(length / 0.5))
    local bend = curl / walk
    local px, py, pz = x, y, z
    for i = 1, walk do
        local t = i / walk
        if curl > 0 then
            dx, dy, dz = normalize(dx + cx * bend, dy + cy * bend, dz + cz * bend)
        end
        px, py, pz = px + dx * 0.5, py + dy * 0.5, pz + dz * 0.5
        if floorY and py < floorY then py = floorY end
        local radius = radius0 + (radius1 - radius0) * t
        local shade = material + min(steps - 1, floor(t * steps))
        self:ball(floor(px + 0.5), floor(py + 0.5), floor(pz + 0.5), radius, shade)
        if self.full then break end
    end
    return px, py, pz, dx, dy, dz
end

-- The trunk's centre line, one entry per level, as floats.
--
-- Two sources of lean, and both are needed. The quadratic is the old brush's and it is the one that
-- keeps the tree standing up out of the ground -- lean has to be zero at the base or the trunk leaves
-- the site it was planted on. The noise term is what stops the result being a parabola: a real trunk
-- wanders, and a curve that only ever bends one way at an increasing rate is instantly readable as an
-- equation.
local function trunkPath(ctx, height, lean, dirX, dirZ, arc, seed)
    local path = {}
    for level = 1, height do
        local t = (height > 1) and ((level - 1) / (height - 1)) or 0
        local reach = t * t * arc
        -- Centred on zero, so this is a wander rather than a second lean.
        local wanderX = (pv.noise(level * 0.09, 0.0, seed * 0.37, seed) - 0.5) * 2.0
        local wanderZ = (pv.noise(0.0, level * 0.09, seed * 0.61, seed + 17) - 0.5) * 2.0
        local wander = lean * height * 0.10 * t
        path[level] = {
            dirX * reach + wanderX * wander,
            dirZ * reach + wanderZ * wander,
        }
    end
    return path
end

-- What a trunk of this shape will cost, before it is drawn. The integral of the ring's area over the
-- taper, near enough: it only has to be good enough to decide whether the tree needs to be smaller.
local function trunkCost(height, baseRadius, tipRadius)
    local total = 0
    local samples = 8
    for i = 0, samples - 1 do
        local t = i / samples
        local radius = baseRadius + (tipRadius - baseRadius) * (t ^ 0.7)
        local wall = min(radius, max(2.0, radius * 0.4))
        local inner = max(0, radius - wall)
        total = total + 3.14159 * (radius * radius - inner * inner)
    end
    return total / samples * height
end

-- A ramp step for a continuous position `v` (0..1) along it, dithered so neighbouring steps interleave
-- across a band instead of meeting at a line. **Choosing the step by `floor(v * steps)` alone draws
-- the ramp as stripes**: on a trunk, where `v` is the height, every step change is a perfect
-- horizontal ring, and a four-step bark ramp reads as four painted bands. Blurring the boundary by
-- most of a step with a hash -- and a smooth noise underneath it, so the mixing comes in patches rather
-- than as salt -- is the voxel equivalent of a painter's dithering.
local function ditheredStep(v, steps, x, y, z, seed, spread)
    spread = spread or 0.85
    local jitter = (pv.hash(x, y, z, seed) - 0.5) * spread
                 + (pv.noise(x * 0.31, y * 0.19, z * 0.31, seed + 3) - 0.5) * spread
    return max(0, min(steps - 1, floor(v * steps + jitter)))
end

-- The trunk, a level at a time.
--
-- **Not a cylinder.** Drawn as perfect discs a big trunk is exactly what it is: a tapered tube, and
-- the eye finds the primitive at once. Three things break it, all cheap:
--
--   * the cross-section is warped by noise around the circumference and up the height, so the trunk
--     bulges and flattens a little as it rises (a real one is never round);
--   * **furrows**: narrow vertical grooves, wandering as they go up, cut a voxel into the surface and
--     shaded darker -- the plates and fissures of bark, which are what say "bark" at any size;
--   * **colour by bark plate, not by height band**: each surface voxel's step comes from its height,
--     its furrow, a vertically stretched noise (plates are tall and thin) and a dither.
--
-- The inside stays hollow when the trunk is thick enough (`wall`), as it always was: nothing can see it.
local function growTrunk(tree, path, height, baseRadius, tipRadius, material, steps, phase, ctx, seed)
    ctx = ctx or { x = 0, y = 0, z = 0 }
    seed = seed or 0
    -- More furrows round a wider trunk, so their spacing stays a few voxels whatever the size.
    local furrows = max(5, floor(baseRadius * 2.2))
    for level = 1, height do
        local t = (height > 1) and ((level - 1) / (height - 1)) or 0
        -- Most of the taper happens low down, which is where a real trunk loses it.
        local radius = baseRadius + (tipRadius - baseRadius) * (t ^ 0.7)
        local flare = 0
        if baseRadius >= 2.0 then
            local low = clamp01(1.0 - t / 0.12)
            flare = low * low * 0.75
        end
        local cx, cz = floor(path[level][1] + 0.5), floor(path[level][2] + 0.5)
        if radius < 1.6 then
            -- Too thin for any of it to show: a plain disc, dithered.
            local extent = floor(radius + 0.5)
            for dz = -extent, extent do
                for dx = -extent, extent do
                    if dx * dx + dz * dz <= radius * radius + 0.25 then
                        local step = ditheredStep(t, steps, ctx.x + cx + dx, ctx.y + level, ctx.z + cz + dz, seed + 61)
                        tree:plot(cx + dx, level, cz + dz, material + step)
                    end
                end
            end
        else
            local wall = min(radius, max(2.0, radius * 0.4))
            local reach = radius * (1.15 + flare * 2.0) + 0.5
            local extent = floor(reach + 1.0)
            -- Inside this nothing is drawn whatever the noise says; outside `reach` likewise. Tested
            -- before the noise is paid for, which is most of the cells of a thick trunk.
            local inner = max(0, radius * 0.85 - 0.9 - wall)
            -- Everything noisy here depends on the angle round the trunk and the level, never on the
            -- distance from the centre -- so it is worked out once per slice of angle per level, not
            -- once per voxel. At the largest trunks that is the difference between the call fitting in
            -- the editor's time limit and being killed as a runaway.
            local slices = max(24, floor(radius * 7))
            local warpAt, grooveAt, plateAt = {}, {}, {}
            for i = 0, slices - 1 do
                local angle = (i + 0.5) / slices * TAU - math.pi
                local ca, sa = cos(angle), sin(angle)
                warpAt[i] = (pv.noise(ca * 1.4 + 11.0, level * 0.07, sa * 1.4, seed + 5) - 0.5) * 0.30
                local wander = (pv.noise(level * 0.11, ca + 3.0, sa, seed + 17) - 0.5) * 3.0
                grooveAt[i] = cos(angle * furrows + wander + phase) > 0.72
                plateAt[i] = (pv.noise(ca * 2.2, level * 0.05, sa * 2.2, seed + 29) - 0.5) * 0.35
            end
            for dz = -extent, extent do
                for dx = -extent, extent do
                    local d = sqrt(dx * dx + dz * dz)
                    if d <= reach and d >= inner then
                    local angle = math.atan(dz, dx)
                    local slice = min(slices - 1, floor((angle + math.pi) / TAU * slices))
                    -- The warp: smooth round the trunk and up it, so it is a lumpiness of the whole
                    -- stem, not of single voxels.
                    local r = radius * (1.0 + warpAt[slice])
                    if flare > 0 then
                        local lobe = 0.5 + 0.5 * cos(angle * 3.0 + phase)
                        r = r * (1.0 + flare * lobe)
                    end
                    -- A furrow: where a wandering line round the circumference comes close. The wander
                    -- is what keeps them from reading as flutes on a column.
                    local inFurrow = grooveAt[slice]
                    if inFurrow then r = r - 0.9 end
                    if d <= r and d >= r - wall then
                        local ax, ay, az = ctx.x + cx + dx, ctx.y + level, ctx.z + cz + dz
                        -- Plates: a noise stretched up the trunk, so colour runs in tall patches.
                        local v = t * 0.65 + 0.30 + plateAt[slice] + (inFurrow and -0.35 or 0.0)
                        tree:plot(cx + dx, level, cz + dz, material + ditheredStep(v, steps, ax, ay, az, seed + 61))
                    end
                    end
                end
            end
        end
        if tree.full then return end
    end
end

-- ---------------------------------------------------------------------------------------------
-- Roots
-- ---------------------------------------------------------------------------------------------

-- Surface roots: a few limbs leaving the base, arcing down and out, and running along the ground.
--
-- **What makes a tree look planted rather than placed is the join, and a cylinder meeting a plane at
-- a right angle has no join.** The root flare in the trunk widens the bottom of that cylinder, which
-- helps and is not enough -- it is still a shape ending abruptly at a flat surface. Roots break the
-- silhouette of the seam itself, so the eye reads the ground as continuing into the tree instead of
-- stopping at it.
--
-- Everything stays at y >= 1, above the ground rather than dug into it. That is not a compromise: the
-- brush is not entitled to carve the terrain it is standing on -- the ground below the site may be
-- painted, may be someone's sculpted hillside, and the placement's cells are the only ones the host's
-- fit test has agreed are free. Roots that lie *on* the surface and swell where they meet the trunk
-- read as roots; roots that displace the ground would read as damage.
local function growRoots(tree, ctx, count, spread, radius, material, steps, phase, seed)
    for root = 1, count do
        local jitter = pv.hash(ctx.x + root, ctx.y, ctx.z, seed)
        -- Spaced around the trunk by the golden angle, so no two trees show the same star of roots
        -- and no root sits exactly over the one opposite.
        local azimuth = phase + root * 2.39996 + (jitter - 0.5) * 0.8
        local length = spread * (0.65 + jitter * 0.7)
        -- Leaves the trunk a little above the ground and heads outward and down, so it arrives at the
        -- surface a couple of voxels out and then runs along it. The downward curl is what turns two
        -- straight segments into an arc.
        local start = 1.0 + radius * 0.6
        tree:limb(0, start, 0,
                  cos(azimuth), -0.55, sin(azimuth),
                  length, radius, max(0.9, radius * 0.35),
                  0, -1, 0, 0.7, material, steps, 1)
        if tree.full then return end
    end
end

-- ---------------------------------------------------------------------------------------------
-- Leaf clumps
-- ---------------------------------------------------------------------------------------------

-- One clump of leaves, the broadleaf's unit of foliage, built like the conifer's needle sprays:
--
--   * a big clump is three to five small ones at jittered offsets, so a large crown is lumpy with
--     consistent small shapes instead of being made of visible ovals;
--   * the rim is warped by a smooth noise;
--   * shaded top-lit (the top of each clump is lighter, the underside darker -- the per-clump half of
--     the two-level shading painted foliage uses), shifted by `light` (the per-crown half), and
--     dithered so the ramp's steps interleave instead of meeting at lines.
--
-- A little flatter underneath than on top, as a clump of leaves hangs.
local function leafClump(tree, ctx, p, x, y, z, radius, light, seed, ramp, steps, nested)
    if radius >= 2.6 and not nested then
        local count = 3 + floor(pv.hash(floor(x), floor(y), floor(z), seed + 201) * 3)
        for i = 1, count do
            local a = pv.hash(i, floor(x), floor(z), seed + 203) * TAU
            local out = radius * (0.25 + 0.4 * pv.hash(i, floor(y), floor(x), seed + 207))
            local sub = radius * (0.5 + 0.22 * pv.hash(floor(z), i, floor(y), seed + 209))
            leafClump(tree, ctx, p, x + cos(a) * out, y + (pv.hash(i, i, floor(x), seed + 211) - 0.5) * radius * 0.5,
                      z + sin(a) * out, sub, light, seed + i * 37, ramp, steps, true)
        end
        return
    end
    local up, down = radius * 0.85, radius * 0.6
    local e0 = floor(radius * 1.2 + 0.5)
    local ix, iy, iz = floor(x + 0.5), floor(y + 0.5), floor(z + 0.5)
    for dy = -floor(down * 1.2 + 0.5), floor(up * 1.2 + 0.5) do
        local rv = dy >= 0 and up or down
        for dz = -e0, e0 do
            for dx = -e0, e0 do
                local e = (dx * dx + dz * dz) / (radius * radius) + (dy * dy) / (rv * rv)
                local ax, ay, az = ctx.x + ix + dx, ctx.y + iy + dy, ctx.z + iz + dz
                if e > 0.45 and e < 1.6 then
                    e = e * (1.0 + (pv.noise(ax * 0.4, ay * 0.4, az * 0.4, seed + 11) - 0.5) * 0.8)
                end
                if e <= 1.0 and not (e > 0.55 and pv.hash(ax, ay, az, seed) < (e - 0.55) / 0.45 * p.ragged * 0.55) then
                    local v = 0.46 + 0.34 * (dy / rv) + light
                    tree:plot(ix + dx, iy + dy, iz + dz, ramp + ditheredStep(v, steps, ax, ay, az, seed + 7, 0.7))
                end
            end
        end
    end
end

-- ---------------------------------------------------------------------------------------------
-- Broadleaf and bare: space colonization
-- ---------------------------------------------------------------------------------------------
--
-- **The branches are grown by competing for space** (Runions, Lane & Prusinkiewicz, "Modeling Trees
-- with a Space Colonization Algorithm", 2007). The crown's volume is seeded with attraction points --
-- markers of free space a branch could grow into. Each step every point pulls on the nearest branch
-- node within the radius of influence `di`; each node pulled grows one new node a distance `D` along
-- the average of the unit vectors to its points (plus a tropism: up toward the light, or down for a
-- weeping crown); and a point is consumed once a node comes within the kill distance `dk`. Repeat
-- until the points are gone.
--
-- What this buys over the recursive skeleton it replaced is the thing that recursion cannot do:
-- **branches avoid each other.** A recursive tree is the same branching rule applied everywhere, so it
-- reads as a pattern; here two limbs never grow into the same space, gaps open between them, and the
-- crown fills its envelope the way a real one fills the light. The shape of the envelope is the
-- crown-shape control (Weber & Penn's 1995 ShapeRatio idea: the crown's radius as a function of
-- height). Points are seeded denser toward the envelope -- the paper's Figure 7 -- because that is
-- where the light is and where a real crown's twigs are.
--
-- Branch thickness is the **pipe model** (Shinozaki et al.): a branch carries the cross-sections of
-- everything above it, r^n = r1^n + r2^n with n = 2.5, computed from the tips down -- then remapped so
-- the trunk is the width asked for and the twigs are a single voxel. That is what makes the wood taper
-- convincingly: a limb is exactly as thick as what it holds up.
--
-- And the paper's smoothing pass, moving each one-child node toward its neighbours, takes the kinks out
-- of the D-length steps so limbs curve rather than zig-zag.

local CROWN_SHAPES = { "Round", "Spreading", "Columnar", "Weeping" }
-- How tall each crown is, as a multiple of its radius.
local CROWN_HEIGHT = { Round = 1.8, Spreading = 1.15, Columnar = 3.2, Weeping = 1.9 }

-- The crown's radius at height `h` up it (0..1), as a fraction of the crown radius.
local function crownProfile(shape, h)
    if h <= 0 or h >= 1 then return 0 end
    if shape == "Spreading" then
        -- Widest a third of the way up, a low dome above: an oak in open ground.
        local d = (h < 0.35) and ((0.35 - h) / 0.35) or ((h - 0.35) / 0.65)
        return sqrt(max(0, 1 - d * d))
    elseif shape == "Juniper" then
        -- Bushy to the ground, fullest a little under the middle: a juniper's crown starts where its
        -- trunk forks, a few voxels up.
        local d = (h < 0.42) and ((0.42 - h) / 0.50) or ((h - 0.42) / 0.58)
        return sqrt(max(0, 1 - d * d))
    elseif shape == "Pinyon" then
        -- A low, broad dome, widest a third of the way up.
        local d = (h < 0.34) and ((0.34 - h) / 0.42) or ((h - 0.34) / 0.66)
        return sqrt(max(0, 1 - d * d))
    elseif shape == "Columnar" then
        -- A poplar: a long tapered cylinder with rounded ends.
        return min(1, sqrt(h / 0.12)) * min(1, sqrt((1 - h) / 0.25)) * (1 - 0.3 * h)
    end
    -- Round and Weeping: an egg, fullest a little above the middle.
    local d = (h < 0.55) and ((0.55 - h) / 0.55) or ((h - 0.55) / 0.45)
    return sqrt(max(0, 1 - d * d))
end

-- `look` (optional) is what turns this into a juniper, a pinyon or a snag rather than a broadleaf; see
-- LOOKS in apply. Every field is optional and an absent one keeps the broadleaf's behaviour.
local function growBroadleaf(tree, ctx, p, kind, path, trunkHeight, bole, crownH, R, trunkRadius,
                             rootCount, phase, look)
    look = look or {}
    local bare = kind == "bare" or look.bare
    local shape = look.shape or p.crownShape
    local barkMaterial, barkSteps = bare and DEAD or BARK, bare and DEAD_STEPS or BARK_STEPS
    local limbMaterial, limbSteps = bare and DEADLIMB or BRANCH, bare and DEADLIMB_STEPS or BRANCH_STEPS
    if look.bark then
        barkMaterial, barkSteps = look.bark, RAMP_STEPS[look.barkName]
        limbMaterial, limbSteps = look.limb, RAMP_STEPS[look.limbName]
    end
    local cx, cz = path[trunkHeight][1], path[trunkHeight][2]

    -- ---- The parameters of the colonization, in voxels ----
    -- D is the step: a tenth of the crown radius keeps a ~100-voxel tree's limbs smooth without a
    -- node per voxel. di = 8D-ish is the paper's "tree" end of the range (17D is a shrub); dk is the
    -- density control -- small and the crown fills with fine twigs, large and it is a few bold limbs.
    local D = max(1.0, R / 10)
    local di = max(6 * D, R * 0.8)
    local density = ({ [0] = 2.5, 6.0, 4.0, 2.5, 1.7 })[floor(p.branching)] or 2.5
    if look.density then density = look.density end
    if bare then density = density * 0.75 end   -- A bare tree *is* its twigs.
    local dk = D * density

    -- ---- Attraction points ----
    local volume = 3.14159 * R * R * crownH * 0.5
    local target = floor(min(1300, max(120, volume / (D * D * D) * 0.35)))
    local px, py, pz, alive, best, owner = {}, {}, {}, {}, {}, {}
    local tries = 0
    while #px < target and tries < target * 12 do
        tries = tries + 1
        local h = pv.hash(tries, 1, 7, p.seed + 1301)
        local x = (pv.hash(tries, 2, 7, p.seed + 1303) * 2 - 1) * R
        local z = (pv.hash(tries, 3, 7, p.seed + 1307) * 2 - 1) * R
        local rr = crownProfile(shape, h) * R
        -- An irregular envelope: the radius wanders round the crown and up it, so the outline is a few
        -- big lobes -- a juniper's or a pinyon's crown is never a clean dome.
        if look.lobes then
            local az = math.atan(z, x)
            rr = rr * (1.0 + look.lobes * (pv.noise(cos(az) * 1.3 + 3.0, h * 2.2, sin(az) * 1.3, p.seed + 1319) - 0.5) * 2.0)
        end
        local d = sqrt(x * x + z * z)
        if d <= rr and pv.hash(tries, 4, 7, p.seed + 1311) < 0.3 + 0.7 * (d / max(0.001, rr)) ^ 2 then
            local i = #px + 1
            px[i], py[i], pz[i] = cx + x, bole + h * crownH, cz + z
            alive[i], best[i] = true, math.huge
        end
    end
    local living = #px

    -- ---- Nodes: the trunk up to the crown, then whatever grows ----
    local nx, ny, nz, parent = {}, {}, {}, {}
    local function addNode(x, y, z, from)
        local i = #nx + 1
        nx[i], ny[i], nz[i], parent[i] = x, y, z, from
        return i
    end
    local previous = nil
    local y = 1
    while y <= bole + 0.01 do
        local level = max(1, min(trunkHeight, floor(y + 0.5)))
        previous = addNode(path[level][1], y, path[level][2], previous)
        y = y + D
    end
    local trunkTop = previous

    -- Points bucketed by `di`, so a node only looks at the points that could be its.
    local cell = di
    local grid = {}
    local function key(gx, gy, gz) return (gx + 512) + (gy + 512) * 1024 + (gz + 512) * 1048576 end
    for i = 1, #px do
        local k = key(floor(px[i] / cell), floor(py[i] / cell), floor(pz[i] / cell))
        local list = grid[k]
        if not list then list = {}; grid[k] = list end
        list[#list + 1] = i
    end
    -- A point's nearest node can only change when a node is added, so each new node is offered to
    -- the points around it once, instead of every point searching every node every step.
    local function offer(n)
        local gx, gy, gz = floor(nx[n] / cell), floor(ny[n] / cell), floor(nz[n] / cell)
        for kz = gz - 1, gz + 1 do
            for ky = gy - 1, gy + 1 do
                for kx = gx - 1, gx + 1 do
                    local list = grid[key(kx, ky, kz)]
                    if list then
                        for _, i in ipairs(list) do
                            if alive[i] then
                                local dx, dy, dz = px[i] - nx[n], py[i] - ny[n], pz[i] - nz[n]
                                local d = sqrt(dx * dx + dy * dy + dz * dz)
                                if d < dk then
                                    alive[i] = false
                                    living = living - 1
                                elseif d < di and d < best[i] then
                                    best[i], owner[i] = d, n
                                end
                            end
                        end
                    end
                end
            end
        end
    end

    -- Tropism: up toward the light, harder with Branch reach; down for a weeping crown.
    local lift = look.lift or ((shape == "Weeping") and -0.30 or (0.06 + 0.22 * p.reach))
    -- Twist: every step turned a little round the vertical, so limbs spiral as they grow -- the
    -- twisted, wind-wrung wood of an old juniper.
    local twistC, twistS = cos(look.twist or 0), sin(look.twist or 0)
    local taken = {}
    local fresh = {}
    for n = 1, #nx do
        if ny[n] >= bole - D * 1.5 then fresh[#fresh + 1] = n end
    end
    for _ = 1, 240 do
        for _, n in ipairs(fresh) do offer(n) end
        if living <= 0 then break end
        local sx, sy, sz = {}, {}, {}
        local pulled = {}
        for i = 1, #px do
            local n = owner[i]
            if alive[i] and n then
                local d = max(1e-6, best[i])
                if not sx[n] then
                    sx[n], sy[n], sz[n] = 0, 0, 0
                    pulled[#pulled + 1] = n
                end
                sx[n] = sx[n] + (px[i] - nx[n]) / d
                sy[n] = sy[n] + (py[i] - ny[n]) / d
                sz[n] = sz[n] + (pz[i] - nz[n]) / d
            end
        end
        -- In node order, never table order, so the same site always grows the same tree.
        table.sort(pulled)
        fresh = {}
        for _, n in ipairs(pulled) do
            local length = sqrt(sx[n] * sx[n] + sy[n] * sy[n] + sz[n] * sz[n])
            if length > 1e-3 then
                local dx, dy, dz = normalize(sx[n] / length, sy[n] / length + lift, sz[n] / length)
                if look.twist then dx, dz = dx * twistC - dz * twistS, dx * twistS + dz * twistC end
                local x, y, z = nx[n] + dx * D, ny[n] + dy * D, nz[n] + dz * D
                -- Two pulls that cancel, or a node re-growing the step it already took, would loop
                -- forever on the same spot; a position is grown into once.
                local q = floor(x * 2 + 0.5) + floor(y * 2 + 0.5) * 4096 + floor(z * 2 + 0.5) * 16777216
                if not taken[q] and y > 1 then
                    taken[q] = true
                    fresh[#fresh + 1] = addNode(x, y, z, n)
                end
            end
        end
        if #fresh == 0 or #nx > 6000 then break end
    end

    local count = #nx
    local children, onlyChild = {}, {}
    for i = 2, count do
        local from = parent[i]
        children[from] = (children[from] or 0) + 1
        onlyChild[from] = i
    end

    -- ---- Dieback ----
    -- An old juniper sheds whole limbs to drought and keeps living on the rest: one or two of the main
    -- limbs are bare silver wood, the others in leaf. A limb is dead if the main limb it grows from is;
    -- nodes are numbered after their parents, so one pass in order settles it.
    local dead = {}
    if look.dieback and look.dieback > 0 then
        for i = trunkTop + 1, count do
            local from = parent[i]
            if from <= trunkTop then dead[i] = pv.hash(i, 5, 3, p.seed + 1409) < look.dieback
            else dead[i] = dead[from] end
        end
    end

    -- ---- Smoothing: the paper's pass, twice ----
    for _ = 1, 2 do
        local mx, my, mz = {}, {}, {}
        for i = trunkTop + 1, count do
            local from = parent[i]
            if from and children[i] == 1 then
                local c = onlyChild[i]
                mx[i] = nx[i] * 0.5 + (nx[from] + nx[c]) * 0.25
                my[i] = ny[i] * 0.5 + (ny[from] + ny[c]) * 0.25
                mz[i] = nz[i] * 0.5 + (nz[from] + nz[c]) * 0.25
            end
        end
        for i, v in pairs(mx) do nx[i], ny[i], nz[i] = v, my[i], mz[i] end
    end

    -- ---- Pipe model ----
    local PIPE, TIP = 2.5, 0.45
    local flow, hops = {}, {}
    for i = count, 1, -1 do
        local f = flow[i] or TIP ^ PIPE
        flow[i] = f
        hops[i] = hops[i] or 0
        local from = parent[i]
        if from then
            flow[from] = (flow[from] or 0) + f
            hops[from] = min(hops[from] or math.huge, hops[i] + 1)
        end
    end
    local rootPipe = flow[1] ^ (1 / PIPE)
    local radius = {}
    for i = 1, count do
        local r = flow[i] ^ (1 / PIPE)
        radius[i] = 0.5 + (r - TIP) / max(1e-6, rootPipe - TIP) * (trunkRadius - 0.5)
    end

    -- ---- Draw ----
    -- The bole with the trunk drawer (bark plates, furrows, flare); everything above as tapered sweeps.
    growTrunk(tree, path, max(2, floor(bole)), trunkRadius, max(0.6, radius[trunkTop]), barkMaterial,
              barkSteps, phase, ctx, p.seed + 401)
    if p.roots > 0 then
        growRoots(tree, ctx, rootCount, trunkRadius * 2.2 * p.roots, max(0.9, trunkRadius * 0.5),
                  limbMaterial, limbSteps, phase, p.seed + 811)
    end

    local function sweep(i)
        local from = parent[i]
        local x0, y0, z0, x1, y1, z1 = nx[from], ny[from], nz[from], nx[i], ny[i], nz[i]
        local r0, r1 = radius[from], radius[i]
        local length = sqrt((x1 - x0) ^ 2 + (y1 - y0) ^ 2 + (z1 - z0) ^ 2)
        local steps = max(1, floor(length / 0.5 + 0.5))
        for k = 0, steps do
            local t = k / steps
            local r = r0 + (r1 - r0) * t
            local bx = floor(x0 + (x1 - x0) * t + 0.5)
            local by = floor(y0 + (y1 - y0) * t + 0.5)
            local bz = floor(z0 + (z1 - z0) * t + 0.5)
            local material = (r > 1.6 and not bare and r > trunkRadius * 0.55) and barkMaterial or limbMaterial
            -- The trunk's continuation into the crown is shaded by height exactly as the bole below
            -- it is (see growTrunk), or the seam between the two shows as a dark collar. Limbs go by
            -- thickness: thick wood is older and darker. Dithered, as everything here is.
            local v
            if material == barkMaterial then
                v = (by / max(1, trunkHeight)) * 0.65 + 0.30
            else
                v = 0.25 + 0.6 * (1.0 - min(1.0, r / max(1.0, trunkRadius)))
            end
            -- Strip bark: long streaks running up the wood, alternating red-brown and grey.
            if look.stripes and r > 0.9 then
                v = 0.45 + 0.45 * (pv.noise((ctx.x + bx) * 0.55, (ctx.y + by) * 0.08, (ctx.z + bz) * 0.55, p.seed + 1423) - 0.5) * 2.0
                    + 0.15 * (by / max(1, trunkHeight))
            end
            local rampSteps = (material == barkMaterial) and barkSteps or limbSteps
            if dead[i] then material, rampSteps = SILVER, RAMP_STEPS["tree.silver"] end
            tree:ball(bx, by, bz, r, material + ditheredStep(v, rampSteps, ctx.x + bx, ctx.y + by, ctx.z + bz, p.seed + 71))
        end
    end
    -- Thick wood before the leaves (it should show through gaps in the crown), twigs after (they
    -- should only show where the leaves left a gap).
    local TWIG = 0.9
    -- A snag's twigs have broken off: only wood thicker than `breakHops` steps from a tip is left.
    local function kept(i) return not look.breakHops or hops[i] >= look.breakHops end
    for i = trunkTop + 1, count do
        if radius[i] >= TWIG and kept(i) then sweep(i) end
        if tree.full then break end
    end

    if not bare then
        local autumn = p.season == "Autumn"
        local ramp, steps = autumn and AUTUMN or LEAF, autumn and AUTUMN_STEPS or LEAF_STEPS
        if look.foliage then ramp, steps = look.foliage, RAMP_STEPS[look.foliageName] end
        local clump = max(1.5, min(5.0, D * 1.25 * p.leafSize))
        if look.clump then clump = max(1.2, look.clump * R * p.leafSize) end
        local holders = {}
        for i = trunkTop + 1, count do
            if hops[i] <= (look.holderHops or 2) and not dead[i] then
                local h = clamp01((ny[i] - bole) / crownH)
                local out = sqrt((nx[i] - cx) ^ 2 + (nz[i] - cz) ^ 2) / max(1, crownProfile(shape, max(0.02, min(0.98, h))) * R)
                -- The inside of a crown is in its own shade and invisible: thin out leaves there.
                if out > 0.45 or h > 0.72 or pv.hash(i, 3, 5, p.seed + 17) < (look.innerLeaves or 0.25) then
                    holders[#holders + 1] = { i, h, min(1, out) }
                end
            end
        end
        local estimate = #holders * 4.19 * clump ^ 3 * 0.42
        local available = (BUDGET - tree.spent) * 0.9
        if estimate > available and available > 0 then
            clump = max(1.2, clump * (available / estimate) ^ (1 / 3))
        end
        for _, holder in ipairs(holders) do
            if tree.full then break end
            local i, h, out = holder[1], holder[2], holder[3]
            local jitter = pv.hash(i, 9, 1, p.seed + 601)
            -- The per-crown half of the shading: lighter toward the top and the outside. In autumn
            -- each clump also takes its own stretch of the ramp, so the crown is mixed reds and golds.
            local light = 0.22 * (h - 0.5) + 0.12 * (out - 0.6) + (autumn and (jitter - 0.5) * 0.5 or 0)
            leafClump(tree, ctx, p, nx[i], ny[i], nz[i], clump * (0.8 + 0.4 * jitter), light,
                      p.seed + i * 13, ramp, steps)
            -- A weeping crown hangs curtains from its twigs.
            -- A weeping crown hangs curtains from its twigs: strands two voxels across, swaying a
            -- little, lit like the outside of the crown and only darkening toward their ends.
            if shape == "Weeping" and jitter < 0.6 then
                local hang = floor(crownH * (0.15 + 0.35 * pv.hash(i, 2, 9, p.seed + 3)))
                local ix, iy, iz = floor(nx[i] + 0.5), floor(ny[i] + 0.5), floor(nz[i] + 0.5)
                local side = (pv.hash(i, 4, 4, p.seed + 7) < 0.5) and 1 or -1
                for k = 1, hang do
                    local sx = floor((pv.noise(i * 0.7, k * 0.12, 0, p.seed + 5) - 0.5) * 4 + 0.5)
                    local sz = floor((pv.noise(0, k * 0.12, i * 0.7, p.seed + 6) - 0.5) * 4 + 0.5)
                    local v = 0.62 - 0.25 * (k / hang) + light
                    local wx, wy, wz = ix + sx, iy - k, iz + sz
                    tree:plot(wx, wy, wz, ramp + ditheredStep(v, steps, ctx.x + wx, ctx.y + wy, ctx.z + wz, p.seed + 9, 0.7))
                    if k < hang * 0.8 then
                        tree:plot(wx + side, wy, wz, ramp + ditheredStep(v - 0.1, steps, ctx.x + wx + side, ctx.y + wy, ctx.z + wz, p.seed + 9, 0.7))
                    end
                end
            end
        end
    end

    for i = trunkTop + 1, count do
        if radius[i] < TWIG and kept(i) then sweep(i) end
        if tree.full then break end
    end

    -- Berries: a scatter of dusty blue on the outside of a juniper's foliage.
    if look.berries and not bare then
        local last = look.foliage + RAMP_STEPS[look.foliageName] - 1
        for i = 1, #tree.voxels do
            local voxel = tree.voxels[i]
            -- The two lightest steps are the outside of the clumps.
            if voxel[4] >= last - 1 and voxel[4] <= last then
                if pv.hash(ctx.x + voxel[1], ctx.y + voxel[2], ctx.z + voxel[3], p.seed + 1433) < look.berries then
                    voxel[4] = BERRY
                end
            end
        end
    end

    if p.blossom and not bare then
        for i = 1, #tree.voxels do
            local voxel = tree.voxels[i]
            if voxel[4] >= LEAF and voxel[4] < LEAF + LEAF_STEPS then
                local n = pv.hash(ctx.x + voxel[1], ctx.y + voxel[2], ctx.z + voxel[3], p.seed + 5)
                if n > 0.82 then voxel[4] = BLOSSOM end
            end
        end
    end
    return tree.voxels
end

-- ---------------------------------------------------------------------------------------------
-- Sagebrush
-- ---------------------------------------------------------------------------------------------
--
-- A shrub, not a small tree: no trunk, but five to eight thin, twisted stems leaning out of one root
-- crown, carrying a mound of small silver-green tufts. What says "sagebrush" at a few voxels across is
-- the colour and the mound -- wider than tall, rough-edged, the stems showing underneath.
local function growShrub(tree, ctx, p, height, radius, phase)
    local stems = 5 + floor(pv.hash(ctx.x, ctx.y, ctx.z, p.seed + 31) * 4)
    local sageSteps, stemSteps = RAMP_STEPS["tree.sage"], RAMP_STEPS["tree.sagestem"]
    local tips = {}
    for s = 1, stems do
        local h = pv.hash(s, 3, 1, p.seed + 33)
        local a = phase + s * 2.39996 + (h - 0.5) * 0.6
        local out = (0.45 + 0.6 * h) * radius / max(1, height)
        local length = height * (0.7 + 0.3 * pv.hash(s, 5, 1, p.seed + 35))
        local x, y, z = tree:limb(0, 1, 0, cos(a) * out, 1.0, sin(a) * out, length, 0.6, 0.5,
                                  cos(a), 0.2, sin(a), 0.6, SAGESTEM, stemSteps)
        tips[#tips + 1] = { x, y, z }
    end
    -- Tufts at the stem tips and part way up them, then a few more round the mound's outline so it
    -- closes into one plant instead of a bunch of lollipops.
    local tuft = max(1.2, radius * 0.32)
    local function tuftAt(x, y, z, k)
        local light = 0.25 * (y / max(1, height) - 0.5)
        leafClump(tree, ctx, p, x, y, z, tuft * (0.8 + 0.4 * pv.hash(k, 7, 3, p.seed + 37)), light,
                  p.seed + k * 11, SAGE, sageSteps, true)
    end
    for k, tip in ipairs(tips) do
        tuftAt(tip[1], tip[2], tip[3], k)
        tuftAt(tip[1] * 0.6, 1 + (tip[2] - 1) * 0.65, tip[3] * 0.6, k + 50)
    end
    local ring = 4 + floor(radius)
    for k = 1, ring do
        local a = phase + k * TAU / ring + pv.hash(k, 1, 9, p.seed + 39)
        local up = 0.35 + 0.4 * pv.hash(k, 2, 9, p.seed + 41)
        tuftAt(cos(a) * radius * 0.75, 1 + height * up, sin(a) * radius * 0.75, k + 100)
    end
    return tree.voxels
end

-- ---------------------------------------------------------------------------------------------
-- Conifer
-- ---------------------------------------------------------------------------------------------
--
-- **Built the way a spruce or fir is built: Massart's architectural model.** One straight trunk
-- (orthotropic, monopodial) that grows in spurts, and at the top of each spurt a *tier* of 4-8
-- near-horizontal (plagiotropic) branches. Each branch carries side branches that alternate left and
-- right **in the branch's own horizontal plane**, so a branch with its side shoots is a flat frond --
-- and the needles sit on those fronds. That flat, layered habit is the whole look: shelves of foliage
-- with dark gaps between them, through which you see the limbs and the trunk.
--
-- Two earlier versions missed it from opposite sides. Needle *lobes* hung on drooping sticks came out
-- as a shaggy blob, because lobes are round and a stack of round things is round. Solid needle *tiers*
-- got the silhouette but had no branches at all -- a stack of green lampshades. This one draws the
-- branches and lets the shelves come out of them.
--
-- Three details carry most of the realism:
--
--   * **The sweep.** A spruce limb leaves the trunk level or slightly down, sags along its length and
--     turns up again at the tip (`rise`, `sag`, `turn` below). Low limbs sag more -- they are longer
--     and older -- and the top ones angle upward. Branch reach scales the sag.
--   * **Bare inner wood.** Needles live a few years, so the inner third of every limb, in the shade of
--     the tiers above, has lost them. Drawing needles only on the outer part is what lets the limbs be
--     seen at all.
--   * **Shading per spray, then per crown.** The top of each spray catches the sun and the underside is
--     in shade, which is what separates the shelves; on top of that the crown lightens toward its top
--     and its outside. The needle ramp is hue-shifted (see MATERIALS), so the shade is cool and the
--     light is warm.
--
-- Plus the small things a real one has: tiers rotated by the golden angle so no two line up, smaller
-- branches between the tiers on a big tree, dead stubs on the bare lower trunk, and a leader at the top.

-- Where a primary branch is at `s` (0 at the trunk, 1 at the tip).
local function limbPoint(limb, s)
    local reach = limb.length * s
    local lift = limb.length * (limb.rise * s - limb.sag * s * s + limb.turn * s * s * s)
    local bearing = limb.azimuth + limb.curl * s * s
    return limb.x + cos(bearing) * reach, limb.y + lift, limb.z + sin(bearing) * reach, bearing
end

-- One flattened clump of needles: a spray. Wider than tall, because the shoots it stands for lie in
-- the plane of their branch -- and **lopsided, deeper below than above**: a thin layer over the top
-- that hides the twig it grows on, and a deeper hang underneath, which is how a spruce's shoots droop
-- off their branch. Hanging the whole clump below the twig instead left the wood showing along the top
-- of every frond. `light` shifts its whole ramp (crown position); the top of the clump is lit and the
-- underside shaded on top of that.
local function needleSpray(tree, ctx, p, x, y, z, radius, light, seed, nested)
    -- **A big spray is several small ones.** One ellipsoid at a radius of four or more is exactly what
    -- it is -- an oval, and a crown of them is a crown of ovals. Built from three to five clumps at
    -- jittered offsets, it has a lumpy outline made of the same small shapes as a little tree's, which is
    -- how a painter builds foliage too: big masses out of a consistent small leaf-clump.
    if radius >= 2.2 and not nested then
        local count = 3 + floor(pv.hash(floor(x), floor(y), floor(z), seed + 101) * 3)
        for i = 1, count do
            local a = pv.hash(i, floor(x), floor(z), seed + 103) * TAU
            local reachOut = radius * (0.25 + 0.4 * pv.hash(i, floor(y), floor(x), seed + 107))
            local sub = radius * (0.5 + 0.22 * pv.hash(floor(z), i, floor(y), seed + 109))
            needleSpray(tree, ctx, p, x + cos(a) * reachOut,
                        y + (pv.hash(i, i, floor(x), seed + 113) - 0.5) * radius * 0.35,
                        z + sin(a) * reachOut, sub, light, seed + i * 31, true)
        end
        return
    end
    local rh = radius
    -- Branch angle reshapes the clump with the limb: a pine's upswept tufts are rounder and sit on
    -- top of the shoot; a juniper's or weeping spruce's hang well below it.
    local lift, hang = max(0, p.limbAngle), max(0, -p.limbAngle)
    local up = max(0.8, radius * (0.35 + 0.30 * lift))
    local down = max(1.0, radius * (0.75 - 0.20 * lift + 0.45 * hang))
    local eh = floor(rh * 1.2 + 0.5)
    local ix, iy, iz = floor(x + 0.5), floor(y + 0.5), floor(z + 0.5)
    local sparse = (1.0 - p.needles * 0.6) * p.ragged
    for dy = -floor(down + 0.5), floor(up + 0.5) do
        local rv = dy >= 0 and up or down
        for dz = -eh, eh do
            for dx = -eh, eh do
                local e = (dx * dx + dz * dz) / (rh * rh) + (dy * dy) / (rv * rv)
                local ax, ay, az = ctx.x + ix + dx, ctx.y + iy + dy, ctx.z + iz + dz
                -- The rim warped by a smooth noise, so even a small clump is not a clean ellipse. Only
                -- near the rim, where it can change the answer.
                if e > 0.45 and e < 1.6 then
                    e = e * (1.0 + (pv.noise(ax * 0.45, ay * 0.45, az * 0.45, seed + 11) - 0.5) * 0.8)
                end
                if e <= 1.0 then
                    local keep = true
                    if e > 0.5 and pv.hash(ax, ay, az, seed) < (e - 0.5) / 0.5 * sparse then
                        keep = false
                    end
                    if keep then
                        local layer = dy / rv   -- -1 underside .. +1 top
                        local v = 0.48 + 0.36 * layer + light
                        local shade = ditheredStep(v, NEEDLE_STEPS, ax, ay, az, seed + 7, 0.7)
                        tree:plot(ix + dx, iy + dy, iz + dz, NEEDLE + shade)
                    end
                end
            end
        end
    end
end

local function growConifer(tree, ctx, p, trunkHeight, crownRadius, trunkRadius, rootCount, phase,
                           dirX, dirZ, arc)
    -- Straighter than a broadleaf: a conifer's leader grows to the light and the trunk follows it.
    local path = trunkPath(ctx, trunkHeight, p.curve * 0.25, dirX, dirZ, arc * 0.25, p.seed)
    local function trunkAt(y)
        local level = max(1, min(trunkHeight, floor(y + 0.5)))
        return path[level][1], path[level][2]
    end

    local base = max(1, floor(trunkHeight * 0.16))
    local top = trunkHeight + max(1, floor(crownRadius * 0.2))
    local span = max(3, top - base)
    -- Tier spacing. A year's growth on a big tree; never so close that the shelves merge.
    local step = max(2.5, min(span / 4, 1.8 + crownRadius * 0.36))
    local sprayRadius = max(1.0, min(4.0, crownRadius * 0.10 + 0.6))

    -- ---- The leader, and the dense little spire around it ----
    -- Drawn before the trunk, so the top of the trunk inside it is needles rather than a stub of bark
    -- standing out of the crown -- plot keeps the first thing written to a cell.
    do
        local tx, tz = trunkAt(trunkHeight)
        local itx, itz = floor(tx + 0.5), floor(tz + 0.5)
        local from = floor(top - step * 1.5)
        if crownRadius >= 8 then
            -- **Not a cone.** Stacked sprays of steadily shrinking radius made a perfect triangle at
            -- the top of every big tree. Instead a few clumps scattered round the leader, of uneven
            -- size and height -- and on an upswept pine, fewer and wider, which is how a pine's top
            -- goes from a point to a rounded crown with age.
            local clumps = 4 + floor(pv.hash(itx, top, itz, p.seed + 5) * 3)
            for i = 1, clumps do
                local h = pv.hash(i, top, p.seed, p.seed + 7)
                local a = i * 2.39996 + h
                local out = sprayRadius * (0.3 + 0.9 * h) * (1.0 + 0.6 * max(0, p.limbAngle))
                local ly = from + (top + 1 - from) * pv.hash(i, p.seed, 3, p.seed + 9)
                needleSpray(tree, ctx, p, itx + cos(a) * out, ly, itz + sin(a) * out,
                            sprayRadius * (0.6 + 0.6 * h), 0.14, p.seed + 3 + i)
            end
        else
            for ly = top + 1, from, -1 do
                local r = (top + 1 - ly) / (step * 1.5 + 1) * sprayRadius * 1.1
                r = r * (0.7 + 0.6 * pv.hash(itx, ly, itz, p.seed + 15))
                if r < 0.75 then
                    tree:plot(itx, ly, itz, NEEDLE + NEEDLE_STEPS - 2)
                else
                    needleSpray(tree, ctx, p, itx, ly, itz, r, 0.12, p.seed + 3)
                end
            end
        end
        for ly = min(from, trunkHeight), top + 1 do tree:plot(itx, ly, itz, NEEDLE + NEEDLE_STEPS - 2) end
    end

    growTrunk(tree, path, trunkHeight, trunkRadius, max(0.6, trunkRadius * 0.2), BARK, BARK_STEPS, phase,
              ctx, p.seed + 401)
    if p.roots > 0 then
        -- Short: they only have to break the seam under the lowest limbs.
        growRoots(tree, ctx, rootCount, trunkRadius * 1.3 * p.roots,
                  max(0.9, trunkRadius * 0.5), BRANCH, 2, phase, p.seed + 811)
    end

    -- ---- The limbs: tiers, plus the smaller ones between them ----
    local limbs = {}
    -- -1 hanging (juniper, weeping spruce), 0 the spruce/fir sweep, +1 upswept (a big pine).
    local angle = max(-1.0, min(1.0, p.limbAngle))
    local function addTier(y, k, lengthScale, countBias)
        local j = pv.hash(k, p.seed, 3, p.seed + 71)
        local t = (y - base) / span
        -- The cone. The power just under 1 keeps the lower tiers broad.
        -- An upswept pine's crown rounds and flattens with age: the profile's power drops toward a
        -- dome, so the upper tiers stay long.
        local profile = 0.95 - 0.55 * max(0, angle)
        local length = (crownRadius * ((1.0 - t) ^ profile) * (0.8 + 0.4 * j) + 0.6) * lengthScale
        if length < 1.2 then return end
        local count = floor(min(8, max(3, 3.5 + length * 0.22 + j + countBias)))
        local cx, cz = trunkAt(y)
        -- Golden angle between tiers, so no limb sits straight over the one below.
        local spin = k * 2.39996 + j * TAU
        for i = 1, count do
            local h = pv.hash(k, i, p.seed, p.seed + 13)
            local slot = TAU / count
            limbs[#limbs + 1] = {
                x = cx, y = y, z = cz,
                azimuth = spin + i * slot + (h - 0.5) * slot * p.lumpiness,
                -- Uneven, and now and then broken short: one smooth cone of limb lengths is a clean
                -- triangle of a silhouette, which a real crown never is.
                length = length * (0.68 + 0.55 * h) * ((pv.hash(k, i, 5, p.seed + 19) < 0.12) and 0.5 or 1.0),
                -- Level or a little down low, angled up near the top -- then tilted as a whole by
                -- Branch angle: steeply down for a juniper, up for a big pine.
                rise = -0.10 + 0.50 * t + (h - 0.5) * 0.12 + angle * 0.85,
                -- An upswept limb barely sags; a hanging one sags further still.
                sag = (0.35 + 0.45 * (1.0 - t)) * (0.35 + p.reach) *
                      (angle >= 0 and (1.0 - 0.75 * angle) or (1.0 - 0.6 * angle)),
                turn = 0,
                curl = (h - 0.5) * 0.5,
                t = t, seed = p.seed + k * 131 + i * 17,
            }
            local limb = limbs[#limbs]
            -- The upswept tip. Fades out as the limbs turn down: a juniper's branch ends hang, they
            -- do not curl back up.
            limb.turn = limb.sag * 0.6 * min(1.0, 1.0 + angle)
        end
    end

    local y, k = base, 0
    while y < top - 1.5 do
        addTier(y, k, 1.0, 0)
        -- Interwhorl branches on a big tree: short and few, halfway up the spurt, in the lower crown
        -- only. Enough to stop a large conifer reading as a stack of plates; any longer and they fill
        -- the gaps under the tiers, which are what show the layering at all.
        if crownRadius >= 12 and (y - base) / span < 0.6 then addTier(y + step * 0.5, k + 1000, 0.38, -2) end
        local j = pv.hash(k, p.seed, 9, p.seed + 5)
        y = y + step * (0.8 + 0.4 * j)
        k = k + 1
    end

    -- ---- The fronds: side branches alternating in the limb's own plane ----
    local twigs = {}
    for _, limb in ipairs(limbs) do
        local spacing = max(1.2, sprayRadius * 1.05) / limb.length
        local side = (pv.hash(limb.seed, 1, 2, 3) > 0.5) and 1 or -1
        -- Bare for the inner part: the needles there were shed in the shade of the tier above.
        local s = 0.30 + 0.22 * max(0, angle) + spacing * 0.5
        while s <= 0.97 do
            local px, py, pz, bearing = limbPoint(limb, s)
            local h = pv.hash(limb.seed, floor(s * 100), 7, p.seed + 29)
            local angle = bearing + side * (0.85 + h * 0.3)
            local length = ((1.0 - s) * limb.length * 0.5 + 1.0) * (0.8 + 0.4 * h)
            twigs[#twigs + 1] = { px, py, pz, cos(angle), sin(angle), length, limb.t, s }
            side = -side
            s = s + spacing
        end
        -- The leading shoot of the limb carries needles too.
        local tx, ty, tz, bearing = limbPoint(limb, 1.0)
        twigs[#twigs + 1] = { tx, ty, tz, cos(bearing), sin(bearing), max(1.0, sprayRadius), limb.t, 1.0 }
    end

    -- ---- Fit the needles to the budget ----
    -- Sprays every `sprayRadius` along every twig: estimate their cost, and shrink the sprays (not
    -- drop them -- a gap in a frond reads as damage) until they fit what the wood left.
    local samples = 0
    for _, twig in ipairs(twigs) do samples = samples + max(1, floor(twig[6] / (sprayRadius * 0.9)) + 1) end
    local estimate = samples * 4.19 * sprayRadius * sprayRadius * max(0.6, sprayRadius * 0.42) * 0.45
    local woodGuess = #limbs * crownRadius * 2.0
    local available = (BUDGET - tree.spent - woodGuess) * 0.9
    if estimate > available and available > 0 then
        sprayRadius = max(1.0, sprayRadius * (available / estimate) ^ (1.0 / 3.0))
    end

    -- ---- Draw: inner limbs, needles, then the outer limbs and twigs inside them ----
    -- Order is visibility: plot keeps the first material written to a cell. The bare inner wood goes
    -- first so it stays wood; the needles next; and the outer limbs and twigs last, so they only show
    -- where the needles left a gap -- underneath a frond, which is exactly where a real one's are
    -- seen. Drawn first, the outer limbs ran along the middle of every frond and showed through its top
    -- as a brown stripe.
    local BARE = 0.34 + 0.22 * max(0, angle)
    -- How far the side shoots fall away from their limb: hardly at all on a pine, like a curtain on a
    -- juniper.
    local twigDroop = 0.12 + 0.55 * max(0, -angle)
    local function drawLimbs(from, to)
        for _, limb in ipairs(limbs) do
            -- A limb is a fraction of the trunk, and thin: at the ~100-voxel size this is tuned for a
            -- spruce limb is one or two voxels thick where it leaves the trunk and a single voxel
            -- for most of its length. Thicker read as logs bolted to a post.
            local radius0 = max(0.5, min(trunkRadius * 0.38, limb.length * 0.035))
            local walk = max(2, floor(limb.length / 0.5))
            for i = 0, walk do
                local s = i / walk
                if s >= from and s <= to then
                    local lx, ly, lz = limbPoint(limb, s)
                    local radius = radius0 + (0.5 - radius0) * s
                    local bx, by, bz = floor(lx + 0.5), floor(ly + 0.5), floor(lz + 0.5)
                    local shade = BRANCH + ditheredStep(s, BRANCH_STEPS, ctx.x + bx, ctx.y + by, ctx.z + bz, limb.seed)
                    tree:ball(bx, by, bz, radius, shade)
                end
            end
            if tree.full then break end
        end
    end
    drawLimbs(0.0, BARE)

    for i, twig in ipairs(twigs) do
        if tree.full then break end
        local x0, y0, z0, ux, uz, length, t, s = twig[1], twig[2], twig[3], twig[4], twig[5], twig[6], twig[7], twig[8]
        -- Lighter toward the top of the crown and toward the outside of each tier.
        local light = 0.20 * (t - 0.4) + 0.14 * (s - 0.6)
        local count = max(1, floor(length / (sprayRadius * 0.9)) + 1)
        for c = 0, count - 1 do
            local u = (count > 1) and (c / (count - 1)) or 0.5
            local d = length * u
            needleSpray(tree, ctx, p, x0 + ux * d, y0 - twigDroop * d * u, z0 + uz * d,
                        sprayRadius * (1.0 - 0.3 * u) * (0.75 + 0.35 * p.needles), light, p.seed + i * 7)
        end
    end

    drawLimbs(BARE, 1.0)
    for _, twig in ipairs(twigs) do
        if tree.full then break end
        local x0, y0, z0, ux, uz, length = twig[1], twig[2], twig[3], twig[4], twig[5], twig[6]
        local walk = max(1, floor(length / 0.5))
        for i = 0, walk do
            local d = length * i / walk
            tree:plot(floor(x0 + ux * d + 0.5), floor(y0 - twigDroop * d * (i / walk) + 0.5),
                      floor(z0 + uz * d + 0.5), BRANCH + BRANCH_STEPS - 1)
        end
    end

    -- ---- Dead stubs on the bare lower trunk ----
    -- A conifer self-prunes: the lowest limbs die in the shade and break off, leaving short stubs.
    if trunkHeight >= 16 then
        local sy = 2 + step * 0.5
        local n = 0
        while sy < base - 1 do
            n = n + 1
            local h = pv.hash(n, p.seed, 41, p.seed)
            local cx, cz = trunkAt(sy)
            local a = n * 2.39996 + h * 2.0
            tree:limb(cx, sy, cz, cos(a), -0.25, sin(a), trunkRadius + 1.0 + h * 2.0,
                      0.7, 0.5, 0, -1, 0, 0, BRANCH, 1)
            sy = sy + step * (0.6 + h * 0.6)
        end
    end

    return tree.voxels
end

return {
    name = "Trees",
    kind = "scatter",
    -- **Where it grows, said where the user reads it.** Any solid surface: there is no material filter,
    -- so stone, dirt, sand and painted grass ground all take a tree. What it will not stand *on* is
    -- undergrowth (blades, flowers, moss -- UNDERGROWTH), which it grows through instead; what refuses a
    -- site is shape, not material: slope, enclosure, footing and room.
    description = "Broadleaf, conifer and bare trees. Grows on any solid surface -- stone, dirt, grass "
               .. "ground -- and through grass blades and flowers. Sites are refused for slope, "
               .. "footing or room; the status line says which. Force plants one exactly where you click.",
    author = "ProjectV",

    needs = { "position", "normal", "crevice", "material" },
    creviceRadius = 2,

    -- Grow through the undergrowth, and through other trees' foliage. See UNDERGROWTH and CANOPY above
    -- for what is on the list and, more importantly, what is kept off it.
    displaces = DISPLACES,

    params = {
        -- Read by name by the editor, like spacing and density below: see BRUSH_FORCE_PARAM.
        { name = "force", label = "Force (one, exactly where I click)", type = "bool", default = false,
          tooltip = "Plant one tree on the voxel under the cursor, every time.\n\n"
                 .. "Skips the spacing grid and Coverage, and every site test below -- slope,\n"
                 .. "enclosure, footing, headroom, clearance. Cells already holding something are\n"
                 .. "left as they are rather than refusing the tree, so a tree forced against a wall\n"
                 .. "keeps the wall. Dragging plants a row, one per Spacing." },

        -- The editor's two, read by name.
        { name = "spacing", label = "Spacing", type = "int", default = 36, min = 1, max = 128,
          tooltip = "How far apart trees are, in voxels. Far larger than a grass brush wants: this is\n"
                 .. "roughly the width of a crown, and below it every tree is refused for want of\n"
                 .. "room by the one beside it.\n\n"
                 .. "Trees grow only at one site per Spacing cell, so a brush narrower than this\n"
                 .. "often covers no site at all. Widen the brush, or use Force.\n\n"
                 .. "Scale this with Tree height: about a third of it suits a mixed wood. A tall tree\n"
                 .. "at a small spacing plants one tree and refuses its neighbours." },
        { name = "density", label = "Coverage", type = "float", default = 0.6, min = 0.0, max = 1.0,
          tooltip = "What fraction of the eligible cells get a tree. Refusals come out of this: a\n"
                 .. "wooded slope plants far fewer than a flat clearing at the same setting." },

        -- What may grow, in any combination.
        { name = "broadleaf", label = "Broadleaf", type = "bool", default = true,
          tooltip = "Branches grown by space colonization -- limbs competing for room inside the crown\n"
                 .. "shape -- with leaf clumps on the twigs. Crown shape, Season and Leaf clump size\n"
                 .. "shape it." },
        { name = "conifer", label = "Conifer", type = "bool", default = true,
          tooltip = "A straight trunk under tiers of branches carrying flat needle fronds. Branch\n"
                 .. "angle turns it from a juniper into a spruce into a pine." },
        { name = "bare", label = "Bare", type = "bool", default = false,
          tooltip = "The broadleaf's skeleton with no leaves and finer twigs -- a bare tree is its\n"
                 .. "branching, so there is more of it to see. Winter, or a dead landscape." },
        { name = "juniper", label = "Juniper", type = "bool", default = false,
          tooltip = "Utah juniper: a short trunk forked low into twisted stems, an irregular bushy crown\n"
                 .. "of olive scale foliage with blue berries, strip bark -- and often a dead silver limb\n"
                 .. "(Dieback). Tuned for 35-55 voxels tall." },
        { name = "pinyon", label = "Pinyon", type = "bool", default = false,
          tooltip = "Two-needle pinyon: a short trunk under a low, broad, dense dome of dark needles.\n"
                 .. "Tuned for 50-75 voxels tall." },
        { name = "snag", label = "Dead juniper", type = "bool", default = false,
          tooltip = "A juniper long dead: twisted silver wood, the twigs broken off." },
        { name = "shrub", label = "Sagebrush", type = "bool", default = false,
          tooltip = "A mound of silver-green tufts on thin twisted stems. Tree height is the shrub's\n"
                 .. "height (4-12) and Crown radius its half-width." },
        { name = "dieback", label = "Dieback", type = "float", default = 0.3, min = 0.0, max = 1.0,
          tooltip = "Juniper: the chance each main limb is dead, bare silver wood among the living ones." },
        { name = "twist", label = "Twist", type = "float", default = 0.6, min = 0.0, max = 1.0,
          tooltip = "Juniper and dead juniper: how hard the limbs spiral as they grow." },
        { name = "blossom", label = "Blossom", type = "bool", default = false,
          tooltip = "Scatters petals through a broadleaf crown. Costs one more palette entry and\n"
                 .. "nothing else." },

        { name = "height", label = "Tree height", type = "int", default = 100, min = 4, max = 256,
          tooltip = "How tall the trees are, in voxels. Each tree takes 72-100% of it, so a stand\n"
                 .. "varies a little. Tuned for about 100; below 30 there is too little room for the\n"
                 .. "branch structure to show.\n\n"
                 .. "This is the master control: trunk thickness and how finely the branches divide\n"
                 .. "follow from it unless they are set by hand." },
        { name = "canopy", label = "Crown radius", type = "int", default = 28, min = 1, max = 96,
          tooltip = "How wide the foliage is, measured to the outside of the whole crown. A conifer\n"
                 .. "takes 70% of it and never more than 30% of its height, so one setting suits a mixed\n"
                 .. "wood. Cost is paid out of the placement budget, so a crown too large for it comes\n"
                 .. "back with smaller clumps rather than failing to place." },
        { name = "crownShape", label = "Crown shape", type = "enum", default = "Round",
          options = CROWN_SHAPES,
          tooltip = "Broadleaf and bare: the envelope the branches grow to fill.\n\n"
                 .. "  Round      an egg, fullest above the middle (lime, maple)\n"
                 .. "  Spreading  wide and low-domed (an oak in open ground)\n"
                 .. "  Columnar   a tall narrow column (poplar)\n"
                 .. "  Weeping    round, with branches pulled down and curtains of leaves (willow)" },
        { name = "season", label = "Season", type = "enum", default = "Summer", options = { "Summer", "Autumn" },
          tooltip = "Broadleaf leaves: summer greens, or autumn's mix of reds, oranges and golds." },
        { name = "leafSize", label = "Leaf clump size", type = "float", default = 1.0, min = 0.4, max = 2.0,
          tooltip = "Broadleaf: how big each clump of leaves on a twig is. Small shows the branches\n"
                 .. "through the crown; large closes it into masses." },
        { name = "trunk", label = "Trunk radius", type = "int", default = 0, min = 0, max = 32,
          tooltip = "Radius of the trunk at the ground, in voxels. **0 derives it from the height**,\n"
                 .. "which is what you want almost always -- it is the setting that keeps a tall tree\n"
                 .. "from being a wire, and deriving it means one control scales the whole tree.\n\n"
                 .. "Set it only to make something deliberately squat or spindly." },
        { name = "branching", label = "Branch density", type = "int", default = 0, min = 0, max = 4,
          tooltip = "Broadleaf and bare: how finely the crown divides. 1 is a few bold limbs, 4 a\n"
                 .. "dense mass of twigs; 0 is the default, between 2 and 3.\n\n"
                 .. "(It is the colonization's kill distance: how close a branch must come to a patch\n"
                 .. "of free space before that space counts as taken.)" },
        { name = "roots", label = "Roots", type = "float", default = 1.0, min = 0.0, max = 3.0,
          tooltip = "How far surface roots spread from the base, as a multiple of the trunk's radius.\n"
                 .. "0 turns them off.\n\n"
                 .. "They are what makes a tree look planted rather than set down on the ground: a\n"
                 .. "trunk is a cylinder meeting a flat surface at a right angle, and roots are what\n"
                 .. "break that seam. They lie on the surface rather than digging into it, so they\n"
                 .. "never carve the terrain -- which does mean they read best on ground that is\n"
                 .. "roughly level under the trunk." },
        { name = "limbAngle", label = "Branch angle", type = "float", default = 0.0, min = -1.0, max = 1.0,
          tooltip = "Conifers: which way the limbs go.\n\n"
                 .. "  -1  hanging -- steeply down, no upturn, side shoots like a curtain\n"
                 .. "      (a juniper, a weeping spruce)\n"
                 .. "   0  level, sagging, tip turned up (spruce, fir)\n"
                 .. "  +1  swept up, hardly sagging, needles in tufts toward the ends (a big pine)\n\n"
                 .. "Branch reach still sets how deep the sag is within that." },
        { name = "needles", label = "Needle fullness", type = "float", default = 0.75, min = 0.0, max = 1.0,
          tooltip = "Conifers: how full the needle sprays are. Low shows more of the branches and the\n"
                 .. "trunk through the tiers -- a pine on a windy ridge; high is a dense young fir." },
        { name = "reach", label = "Branch reach", type = "float", default = 0.6, min = 0.0, max = 1.5,
          tooltip = "How hard branches curl toward the light as they grow. 0 gives straight rays out\n"
                 .. "of the trunk; high values give the upswept look of an old broadleaf.\n\n"
                 .. "On a conifer it is the sweep: how far each limb sags before its tip turns up." },
        { name = "ragged", label = "Raggedness", type = "float", default = 0.8, min = 0.0, max = 1.0,
          tooltip = "How eroded the rim of the foliage is. This is the *texture* of the outline." },
        { name = "lumpiness", label = "Lumpiness", type = "float", default = 0.55, min = 0.0, max = 1.0,
          tooltip = "How far the foliage strays from round, coherently -- this is the *shape* of the\n"
                 .. "outline, where Raggedness is its texture. 0 gives smooth lobes; the two do\n"
                 .. "different jobs and both are usually wanted." },
        { name = "curve", label = "Lean", type = "float", default = 0.4, min = 0.0, max = 1.0,
          tooltip = "How far a trunk leans off vertical, and how much it wanders on the way. Zero at\n"
                 .. "the base whatever the setting, so the tree always stands up out of its site." },

        -- The fit test's own settings.
        { name = "clearance", label = "Clearance", type = "int", default = 0, min = 0, max = 16,
          tooltip = "Empty space demanded *around* the crown, in voxels, on top of the room the tree\n"
                 .. "itself takes. 0 lets crowns touch; 2 or more keeps them apart and refuses far\n"
                 .. "more sites. This is the one check the editor cannot do for the brush -- it knows\n"
                 .. "what the tree occupies, not what it wants to be left alone." },
        -- The material filter the paint brushes have, so "only on the grass" is a setting. Empty -- the
        -- default -- means any solid ground at all; see the description.
        { name = "onlyOn", label = "Only on material", type = "text", default = "",
          tooltip = "Leave empty to grow on any solid surface: stone, dirt, sand, painted grass\n"
                 .. "ground -- whatever the palette entry is called.\n\n"
                 .. "Otherwise a site is used only when the name of the material under it contains\n"
                 .. "this text -- 'grass' catches grass.lush and grass.dry alike. Grass blades and\n"
                 .. "flowers are never stood on either way: a tree grows through them instead." },
        { name = "footing", label = "Footing", type = "int", default = 3, min = 0, max = 9,
          tooltip = "How many of the nine cells under the base must be solid ground. 9 demands a flat\n"
                 .. "slab; 0 will plant a tree on a single voxel sticking out of a cliff." },
        { name = "maxSlope", label = "Steepest ground", type = "float", default = 0.75, min = 0.0, max = 1.0,
          tooltip = "How far off level the ground may be, 0 (flat only) to 1 (anything). Generous by\n"
                 .. "default: this refuses whole sites, and a slope that is too steep for a tree is\n"
                 .. "rarer than ground the normal reads as slightly tilted." },
        { name = "enclosure", label = "Skip ground more enclosed than", type = "float",
          default = 0.97, min = 0.5, max = 1.0,
          tooltip = "Skips ground that is boxed in -- the bottom of a narrow gully is where a crown\n"
                 .. "has nowhere to go.\n\n"
                 .. "Note where the useful range starts. `crevice` is the fraction of nearby cells\n"
                 .. "that are solid, and **flat open ground already reads about 0.6** -- half the ball\n"
                 .. "around a surface voxel is the ground it stands on. So a threshold below that\n"
                 .. "rejects everything, including the field you were aiming at." },
        { name = "seed", type = "seed", default = 7 },
    },

    materials = MATERIALS,

    apply = function(ctx, p)
        -- ---- 1. The ground ----
        --
        -- The cheapest tests, and the ones that reject the most sites. Everything below them costs
        -- either a scene query or a few thousand voxels of generated tree.
        -- The slack is not a fudge. A discrete normal off real terrain is never exactly (0,1,0), so
        -- the bare comparison makes `maxSlope = 0` mean "refuse everywhere" rather than "flat ground
        -- only" -- a setting that reads as the strictest useful value and is actually the one value
        -- that can never plant a tree. The whole control was unusable at its own minimum.
        --
        -- Every refusal says why (`return nil, "reason"`): the editor tallies them per dab and shows
        -- the count in the status line, which is the difference between "it planted nothing" and "it
        -- planted nothing because the ground is too steep". `ctx.force` -- the Force setting -- skips
        -- all of these site tests: the user has said where, and the host writes what fits.
        local force = ctx.force
        if not force then
            if ctx.ny < 1.0 - p.maxSlope - 0.03 then return nil, "too steep" end
            if ctx.crevice > p.enclosure then return nil, "too enclosed" end
        end

        -- Not on top of the undergrowth. A blade of grass is a surface voxel like any other, so the
        -- scatter lattice offers its tip as a site perfectly happily, and a tree planted there stands
        -- on a stalk with its roots in the air. The ground voxels around it are offered too, and those
        -- are the ones to grow from -- so this is a refusal that costs nothing: it rejects a site the
        -- lattice will make a better offer for.
        -- `ctx.material`, the name the host passes. This read `ctx.materialName` -- a field nothing sets --
        -- so the check never fired and trees were happily rooted on the tips of grass blades.
        if not force and isUndergrowth(ctx.material) then return nil, "on undergrowth" end
        if p.onlyOn ~= "" and not force then
            if not ctx.material or not string.find(ctx.material, p.onlyOn, 1, true) then
                return nil, "not on " .. p.onlyOn
            end
        end

        local kinds = {}
        if p.broadleaf then kinds[#kinds + 1] = "broadleaf" end
        if p.conifer   then kinds[#kinds + 1] = "conifer" end
        if p.bare      then kinds[#kinds + 1] = "bare" end
        if p.juniper   then kinds[#kinds + 1] = "juniper" end
        if p.pinyon    then kinds[#kinds + 1] = "pinyon" end
        if p.snag      then kinds[#kinds + 1] = "snag" end
        if p.shrub     then kinds[#kinds + 1] = "shrub" end
        if #kinds == 0 then return nil, "no tree kind ticked" end

        local r1 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed)
        local r2 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed + 101)
        local r3 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed + 202)
        local kind = kinds[min(#kinds, 1 + floor(r1 * #kinds))]

        -- Is the base standing on anything? A tree rooted on a one-voxel spur reads as a mistake, and
        -- nothing else here would reject it -- the site is a surface voxel, so it is solid by
        -- definition; what is in question is its neighbours.
        if p.footing > 0 and not force then
            local firm = 0
            for dz = -1, 1 do
                for dx = -1, 1 do
                    if pv.solid(ctx.x + dx, ctx.y, ctx.z + dz) then firm = firm + 1 end
                end
            end
            if firm < p.footing then return nil, "not enough footing" end
        end

        -- ---- 2. Proportions ----
        --
        -- Everything the tree is, derived from its height before a voxel is generated -- so that the
        -- headroom test below can be told the truth about how tall this tree will be, and so that one
        -- control scales the whole thing. The derivations are the point of the rewrite; the old brush
        -- had a 1-voxel trunk at every height and a crown that did not know how tall it was.
        -- `height` is the whole tree's, and each tree takes 72-100% of it: a stand varies, but a brush
        -- set to 100 should plant trees about 100 tall, which is the size all of this is tuned for.
        local trunkHeight = max(4, floor(p.height * (0.72 + r2 * 0.28)))
        local crownRadius = p.canopy * (0.85 + 0.3 * r3)
        -- A conifer is a spire: however wide the crown setting, it stays well under half its height,
        -- or the tiers flatten into a pancake that reads as a bush.
        if kind == "conifer" then crownRadius = max(1.5, min(p.canopy * 0.7, trunkHeight * 0.3)) end

        -- **Trunk thickness, from the height.** A real trunk is a fiftieth to a thirtieth of its
        -- height across. Voxels want it chunkier than nature -- below a radius of about 1.2 a stem is a
        -- single line of cells -- but this used to be a *ninth*, which at 100 tall is a log 22 voxels
        -- across: every tree read as a stump with a crown. Now a fortieth for a conifer (about 5
        -- across at 100 tall) and a thirtieth for a broadleaf (about 7), each well clear of the
        -- one-voxel floor at the sizes this is for.
        local trunkRadius = p.trunk
        if trunkRadius <= 0 then
            trunkRadius = (kind == "conifer") and max(1.0, trunkHeight / 40.0) or max(1.2, trunkHeight / 30.0)
            -- A juniper is short and stout, its trunk thick for its height; a pinyon a little less so.
            if kind == "juniper" or kind == "snag" then trunkRadius = max(1.5, trunkHeight / 12.0) end
            if kind == "pinyon" then trunkRadius = max(1.3, trunkHeight / 24.0) end
        end
        trunkRadius = min(trunkRadius, max(1.2, crownRadius * 0.8))

        -- How many times the skeleton divides. Derived from how much trunk there is to divide: below
        -- about a dozen voxels there is no room for a level that would be visible.
        local levels = p.branching
        if levels <= 0 then
            levels = 0
            if trunkHeight >= 10 then levels = 1 end
            if trunkHeight >= 22 then levels = 2 end
            if trunkHeight >= 60 then levels = 3 end
        end
        if kind == "bare" then levels = min(4, levels + 1) end

        -- Enough roots to break the seam all the way round, and no more: past about seven they merge
        -- into a collar, which is the flare again and not worth the voxels.
        local rootCount = floor(min(7, max(3, trunkRadius * 1.4 + 2)))

        -- ---- Fit the whole tree to the budget, before a voxel of it exists ----
        --
        -- The budget has to be spent from the outside in. Left to run into the ceiling mid-generation
        -- a tree simply stops -- and because it is built trunk first, then limb by limb, what stops is
        -- always the same side of it: the last branches and their foliage are missing and the result
        -- is a tree sheared off, which reads as a bug rather than as a limit. Shrinking first costs
        -- the user a smaller tree than they asked for, which is a thing they can see and answer.
        --
        -- Only the skeleton is solved for here. The crown is fitted separately and later,
        -- against what the skeleton actually spent rather than against an estimate of it, because by
        -- then the real number is known.
        for _ = 1, 12 do
            local cost = trunkCost(trunkHeight, trunkRadius, max(0.5, trunkRadius * 0.3))
            -- Branches: thinner than the trunk but there are many, and their total comes out near
            -- two-thirds of it across the range this brush is used at.
            cost = cost * (1.0 + 0.65 * min(1.0, levels * 0.5))
            -- Leave at least a third of the budget for foliage, or the tree is a bare skeleton at
            -- exactly the sizes where the crown is what you were looking at.
            if cost <= BUDGET * 0.66 then break end
            trunkHeight = max(2, floor(trunkHeight * 0.88))
            crownRadius = max(1, crownRadius * 0.88)
            trunkRadius = max(1.2, trunkRadius * 0.88)
        end

        -- The crown sits on the branch tips, which reach about `crownRadius` above the trunk. Worth
        -- deriving rather than guessing: a headroom check that stops short of the crown passes sites
        -- where the top of the tree is inside a ceiling, and the editor then refuses the placement
        -- anyway -- so the brush pays for generating a whole tree to have it thrown away.
        local topY = ctx.y + trunkHeight + floor(crownRadius * 0.3) + 3
        if not force and not pv.fits(ctx.x, ctx.y + 1, ctx.z, ctx.x, topY, ctx.z) then
            return nil, "no headroom"
        end

        -- The trunk's own footprint, two levels of it, before anything is generated.
        --
        -- This is the one refusal a dense planting actually runs into once foliage and limbs may
        -- interleave: trunks may not, so in a wood most refused sites are refused by a neighbour's
        -- base. Without this the brush finds that out the expensive way -- the host's fit test runs
        -- *after* the placement comes back, so a site that was never going to be planted still paid
        -- for a whole tree to be built and thrown away, and at a large trunk height that is tens of
        -- thousands of voxels of generation per refusal.
        --
        -- Deliberately only the trunk, and only the bottom of it. A test that tried to predict the
        -- host's whole answer would be the box test this brush just stopped doing; this one is a few
        -- thousand cells and catches the common case.
        local footprint = floor(trunkRadius * 1.8) + 1
        if not force and not pv.fits(ctx.x - footprint, ctx.y + 1, ctx.z - footprint,
                                     ctx.x + footprint, ctx.y + 2, ctx.z + footprint) then
            return nil, "a trunk already there"
        end

        -- ---- 3. Room to spread ----
        --
        -- **Sampled on a ring of columns, not swept through a solid box.** The box was the single most
        -- expensive thing in this brush by three orders of magnitude, and it was invisible because it
        -- costs nothing at the default: at clearance 0 the test is skipped and pv.fits examines about
        -- five hundred cells for the whole tree, and at clearance 1 it examines nearly nine million.
        -- Every one of those is a voxel query into the scene, per candidate site -- so turning
        -- clearance on at a large crown radius took a dab from milliseconds to tens of seconds, and
        -- the control that did it reads like a placement preference rather than a cost.
        --
        -- The cost is inherent to the shape of the question: a filled box is cubic in the crown radius
        -- and the tree is not. Two rings of vertical columns around the crown sample the same envelope
        -- for a few thousand cells, and sampling is the right standard here -- clearance is a
        -- preference for how far apart trees stand, not a correctness guarantee. What guarantees the
        -- tree does not intersect anything is the host's own test, which is exact, checks the cells the
        -- tree actually occupies rather than a box around them, and runs regardless of this.
        local arc = p.curve * (0.4 + r3) * trunkHeight * 0.35
        local angle = r2 * TAU
        local dirX, dirZ = cos(angle), sin(angle)
        if p.clearance > 0 and not force then
            local lean = floor(arc + 0.5)
            local reach = floor(crownRadius + p.clearance + abs(lean)) + 1
            local top = topY + p.clearance
            local PROBES = 12
            for ring = 1, 2 do
                -- The outer ring is the clearance envelope; the inner one catches something standing
                -- inside it that the outer ring would pass over.
                local at = floor(reach * (ring == 1 and 1.0 or 0.62) + 0.5)
                for probe = 0, PROBES - 1 do
                    -- Rotated per tree, so neighbours do not all probe the same twelve bearings.
                    local a = probe * TAU / PROBES + r1 * TAU
                    local px = ctx.x + floor(cos(a) * at + 0.5)
                    local pz = ctx.z + floor(sin(a) * at + 0.5)
                    if not pv.fits(px, ctx.y + 1, pz, px, top, pz) then return nil, "short of clearance" end
                end
            end
        end

        -- ---- The tree itself ----

        local tree = newTree()
        local phase = r1 * TAU

        if kind == "shrub" then
            return growShrub(tree, ctx, p, trunkHeight, crownRadius, phase)
        end

        -- ---- Pinyon-juniper: the broadleaf's colonization with a different look ----
        --
        -- A juniper is grown the same way as a broadleaf -- branches competing for room inside the
        -- crown's envelope -- because that is what produces its habit: a trunk that forks a few voxels
        -- up into several stems, each wandering off on its own. The envelope starts almost at the
        -- ground and is lumpy; the stems twist; strip bark; olive foliage in big tufts.
        if kind == "juniper" or kind == "pinyon" or kind == "snag" then
            local looks = {
                juniper = { shape = "Juniper", bark = STRIPBARK, barkName = "tree.stripbark",
                            limb = JUNIPERLIMB, limbName = "tree.juniperlimb",
                            foliage = JUNIPER, foliageName = "tree.juniper",
                            lobes = 0.35, density = 3.2, lift = 0.10, twist = 0.10 * p.twist,
                            stripes = true, dieback = p.dieback, berries = 0.035, clump = 0.16,
                            holderHops = 4, innerLeaves = 0.6,
                            boleFraction = 0.10, crownFraction = 0.90 },
                snag    = { shape = "Juniper", bare = true, bark = SILVER, barkName = "tree.silver",
                            limb = SILVER, limbName = "tree.silver",
                            lobes = 0.4, density = 3.6, lift = 0.10, twist = 0.12 * p.twist,
                            breakHops = 2, boleFraction = 0.12, crownFraction = 0.85 },
                pinyon  = { shape = "Pinyon", bark = PINYONBARK, barkName = "tree.pinyonbark",
                            limb = PINYONLIMB, limbName = "tree.pinyonlimb",
                            foliage = PINYON, foliageName = "tree.pinyon",
                            lobes = 0.22, density = 2.2, lift = 0.16, clump = 0.14,
                            holderHops = 5, innerLeaves = 0.85,
                            boleFraction = 0.08, crownFraction = 0.90 },
            }
            local look = looks[kind]
            local crownH = trunkHeight * look.crownFraction
            local bole = max(2, floor(trunkHeight * look.boleFraction))
            local path = trunkPath(ctx, trunkHeight, p.curve, dirX, dirZ, arc * 0.5, p.seed)
            return growBroadleaf(tree, ctx, p, kind, path, trunkHeight, bole, crownH, crownRadius,
                                 trunkRadius, rootCount, phase, look)
        end

        -- ---- Conifer ----
        --
        -- Its own generator (growConifer, above), because a conifer is not a broadleaf with different
        -- foliage: its silhouette is the shape, and the shape is tiers.
        if kind == "conifer" then
            return growConifer(tree, ctx, p, trunkHeight, crownRadius, trunkRadius, rootCount, phase,
                               dirX, dirZ, arc)
        end

        -- ---- Broadleaf and bare ----
        --
        -- One generator (growBroadleaf, space colonization), and a bare tree is the same tree with
        -- the leaves skipped and a finer twig density -- what you see of a bare tree *is* the
        -- structure a leafy one hides. The crown sits on a clear bole: as tall as the shape asks for,
        -- but never more than 80% of the tree.
        local crownH = min((CROWN_HEIGHT[p.crownShape] or 1.8) * crownRadius, trunkHeight * 0.8)
        local bole = max(2, floor(trunkHeight - crownH))
        local path = trunkPath(ctx, trunkHeight, p.curve, dirX, dirZ, arc, p.seed)
        return growBroadleaf(tree, ctx, p, kind, path, trunkHeight, bole, crownH, crownRadius,
                             trunkRadius, rootCount, phase)
    end,
}
