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
-- Appended rather than inserted, so every index below keeps the value it had.
local BARK     = 1     -- ...through 3: root to crown. The trunk, and nothing else.
local LEAF     = 4     -- ...through 7: inside to outside.
local NEEDLE   = 8     -- ...through 10.
local BLOSSOM  = 11
local DEAD     = 12    -- ...through 13. A bare tree's trunk.
local BRANCH   = 14    -- ...through 16. Limbs and surface roots.
local DEADLIMB = 17    -- ...through 18. The same, on a bare tree.

local TAU = 6.28318530718

-- What one placement may cost, kept under the host's own ceiling so a miscount is a slightly smaller
-- tree rather than a refused one.
local BUDGET = 110000
-- Foliage thinner than this stops reading as leaves and starts reading as a rendering fault, so when
-- the budget cannot buy a crown at this density the crown is made smaller instead of sparser.
local MIN_LEAF_DENSITY = 0.30
-- How coarsely the foliage's shape noise is sampled, in voxels, and the frequency that goes with it.
-- See the note at the sample site: this is the brush's main cost control and 2 is close to free
-- visually, because the warp's own features are several voxels across.
local WARP_STRIDE = 2
local WARP_FREQ = 0.11 * WARP_STRIDE

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
local CANOPY = { "tree.leaf", "tree.needle", "tree.blossom", "tree.branch", "tree.deadlimb" }

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

local function growTrunk(tree, path, height, baseRadius, tipRadius, material, steps, phase)
    for level = 1, height do
        local t = (height > 1) and ((level - 1) / (height - 1)) or 0
        -- Most of the taper happens low down, which is where a real trunk loses it.
        local radius = baseRadius + (tipRadius - baseRadius) * (t ^ 0.7)
        -- Root flare: a swelling in the bottom eighth, squared so it is a curve into the ground
        -- rather than a step. Only worth drawing on a trunk thick enough to show it.
        local flare = 0
        if baseRadius >= 2.0 then
            local low = clamp01(1.0 - t / 0.12)
            flare = low * low * 0.75
        end
        local shade = material + min(steps - 1, floor(t * steps))
        tree:disc(floor(path[level][1] + 0.5), level, floor(path[level][2] + 0.5),
                  radius, shade, flare, phase, min(radius, max(2.0, radius * 0.4)))
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
-- The branching skeleton
-- ---------------------------------------------------------------------------------------------

-- Grows one limb and then, unless it is a terminal one, two or three children aimed in a cone around
-- it. Terminal limbs record their tip in `tips`; that list is what the foliage is hung on.
--
-- Recursion depth is the shape control that the old brush was missing entirely. Depth 0 is a stick,
-- which is what the old `growBranches` amounted to; depth 2 is a tree.
local function growBranch(tree, ctx, p, x, y, z, dx, dy, dz, length, radius, depth, tips,
                          material, steps, seed)
    if tree.full or length < 1.5 then return end

    -- Branches reach up; the higher they start the less they have to. `curl` is in the same units as
    -- the direction, so 0.6 is a firm bend over the limb's length and 0 is a straight ray.
    local tipX, tipY, tipZ, ndx, ndy, ndz =
        tree:limb(x, y, z, dx, dy, dz, length, radius, max(0.4, radius * 0.45),
                  0, 1, 0, p.reach, material, steps)

    if depth <= 0 then
        tips[#tips + 1] = { tipX, tipY, tipZ, length }
        return
    end

    local children = 2
    if pv.hash(floor(x), floor(y) + depth, floor(z), seed + 31) > 0.55 then children = 3 end
    local ax, ay, az, bx, by, bz = basisOf(ndx, ndy, ndz)
    -- The golden angle between siblings, jittered. Evenly spaced children are as readable as an even
    -- anything -- and unjittered, two trees whose branches start at the same angle are visibly the
    -- same tree.
    local spin = pv.hash(floor(x), floor(y), floor(z), seed + depth * 7) * TAU
    for child = 1, children do
        local jitter = pv.hash(floor(x) + child, floor(y), floor(z), seed + 53)
        local azimuth = spin + child * 2.39996 + (jitter - 0.5) * 1.1
        local spread = (0.45 + jitter * 0.40)          -- radians off the parent
        local sx, sy, sz = normalize(
            ndx * cos(spread) + (ax * cos(azimuth) + bx * sin(azimuth)) * sin(spread),
            ndy * cos(spread) + (ay * cos(azimuth) + by * sin(azimuth)) * sin(spread),
            ndz * cos(spread) + (az * cos(azimuth) + bz * sin(azimuth)) * sin(spread))
        local shrink = 0.62 + jitter * 0.16
        growBranch(tree, ctx, p, tipX, tipY, tipZ, sx, sy, sz,
                   length * shrink, max(0.4, radius * 0.62), depth - 1, tips, material, steps,
                   seed + child * 101)
    end
end

-- ---------------------------------------------------------------------------------------------
-- Foliage
-- ---------------------------------------------------------------------------------------------

-- One lobe of leaves: a shell, warped by fbm, eroded at the rim by hash.
--
-- The shell is the affordability of the whole brush. Foliage is opaque, so the inside of a lobe can
-- never be seen -- filling it spends the budget on voxels that exist only to be hidden, and it is a
-- cubic cost against the shell's square one. A solid crown of radius 20 is 33000 voxels and cannot be
-- placed at all; the same crown as shells fits several times over.
--
-- The fbm warp is what stops each lobe being a small sphere. Raggedness erodes the rim, which changes
-- how an outline is textured; warping the radius changes what the outline *is*, coherently, so the
-- lobe has lumps rather than fuzz.
-- `base` is the first index of the four-step ramp the lobe shades through, and `flatten` how much
-- wider than tall it is. Both are arguments rather than constants because the conifer hangs its
-- needles through this same function: the two kinds differ in the skeleton they hang foliage on and in
-- what that foliage is called, not in how a clump of foliage is built, and a second copy of this loop
-- was a second place for the budget and the shell arithmetic to drift.
local function growLobe(tree, ctx, p, cx, cy, cz, radius, density, seed, base, flatten, steps)
    base = base or LEAF
    -- The ramp's own length, because not every ramp is four steps. Hardcoding four shaded the
    -- conifer's three-step needle ramp into the entry *after* it, which is tree.blossom -- so a fir
    -- came out flecked with pink. A ramp is only as long as it is declared.
    steps = steps or 4
    local warpSpan = p.lumpiness * 0.85
    local extent = floor(radius * (1.0 + warpSpan) + 0.5) + 1
    -- Wider than tall: a crown spreads, and a conifer's spray is flatter still.
    local squash = 1.0 / (flatten or 0.82)
    local thickness = min(radius, max(2.0, radius * 0.34))
    local innerFraction = 1.0 - thickness / radius
    local ox, oy, oz = ctx.x + cx, ctx.y + cy, ctx.z + cz

    -- **What a cell must be within to have any chance, before the noise is paid for.** fbm is the
    -- expensive call in this brush by an order of magnitude -- one per candidate cell, against a
    -- candidate box that is cubic in the radius -- so the loop asks it only about cells some warp
    -- could actually bring into the shell. The bounds are the extremes the warp can reach, which
    -- makes the cull exact: nothing that would have been drawn is skipped.
    --
    -- How much it saves depends entirely on `lumpiness`, and it is worth knowing which way round. The
    -- outer bound always earns its keep -- it is what stops the corners of the box being sampled. The
    -- inner one pays only when the warp is narrow: at lumpiness 0 it removes the whole hollow middle,
    -- and at 1 it collapses to nearly nothing, because a warp that wide genuinely can pull a cell
    -- near the centre out into the shell. Lumpy crowns cost more to generate than smooth ones, and
    -- that is a real property of them rather than a missed optimisation.
    local outerBound = radius * (1.0 + warpSpan)
    local innerBound = innerFraction * radius * (1.0 - warpSpan)

    -- Per lobe rather than per tree: the keys are absolute coordinates, so a shared table would keep
    -- growing across a whole placement for no reuse -- lobes do not overlap enough to pay for it.
    local warpCache = {}

    for dy = -extent, extent do
        for dz = -extent, extent do
            for dx = -extent, extent do
                local ey = dy * squash
                local d = sqrt(dx * dx + ey * ey + dz * dz)
                if d <= outerBound and d >= innerBound then
                    -- The warp, sampled coarsely and remembered. It is a smooth field -- the input is
                    -- scaled to about a nine-voxel wavelength -- so asking for it once per cell was
                    -- sampling it several times inside every feature it has, and fbm is the single
                    -- most-called thing in this brush by an order of magnitude (a large tree asked for
                    -- it half a million times). One sample per WARP_STRIDE cells on each axis is the
                    -- same field to look at and a fraction of the calls; what quantising costs is a
                    -- surface that steps in twos, which is invisible under a rim the raggedness test
                    -- is already eroding a cell at a time.
                    -- Keyed on the quantised *offset* from the lobe's centre, not on the absolute
                    -- coordinate. The offset is bounded by the lobe's own extent, so the key cannot
                    -- run out of the range packed into it -- an absolute coordinate can, on a
                    -- component large enough to need a grid, and the failure would be two cells
                    -- silently sharing a warp value somewhere out in the scene.
                    local ux, uy, uz = dx // WARP_STRIDE, dy // WARP_STRIDE, dz // WARP_STRIDE
                    local wkey = (ux + 512) + (uy + 512) * 1024 + (uz + 512) * 1048576
                    local warp = warpCache[wkey]
                    if not warp then
                        warp = pv.fbm((ox + ux * WARP_STRIDE) * 0.11, (oy + uy * WARP_STRIDE) * 0.11,
                                      (oz + uz * WARP_STRIDE) * 0.11, 2, seed)
                        warpCache[wkey] = warp
                    end
                    local effective = radius * (1.0 + (warp - 0.5) * p.lumpiness * 1.7)
                    if effective > 0.5 then
                        local edge = d / effective
                        if edge <= 1.0 and edge >= innerFraction then
                            -- Thinner towards the outside, as before, and then thinned again by
                            -- whatever the budget could afford.
                            local n = pv.hash(ox + dx, oy + dy, oz + dz, seed)
                            if n > edge * edge * p.ragged * 0.85 then
                                if density >= 1.0 or
                                   pv.hash(ox + dx, oy + dy, oz + dz, seed + 991) < density then
                                    -- Shaded across the *shell's* span, not the lobe's radius. The
                                    -- ramp runs inside-to-outside and the shell only occupies its
                                    -- outer third, so measuring from the centre spent a four-step
                                    -- ramp on two shades and threw away the darks that give the
                                    -- foliage its depth.
                                    local through = (edge - innerFraction) /
                                                    max(0.001, 1.0 - innerFraction)
                                    local shade = base + min(steps - 1, floor(through * steps))
                                    tree:plot(cx + dx, cy + dy, cz + dz, shade)
                                end
                            end
                        end
                    end
                end
            end
        end
    end
end

-- What a lobe will cost, before one is generated. A shell's volume times the two thinning factors,
-- which is rough -- the fbm warp moves cells across the boundary in both directions and roughly
-- cancels -- but it only has to be good enough to choose a density, and being wrong by a fifth costs
-- a fifth of the crown's density rather than a failed placement.
local function lobeCost(p, radius)
    local thickness = min(radius, max(2.0, radius * 0.34))
    local inner = radius - thickness
    local shell = 4.18879 * (radius * radius * radius - inner * inner * inner) / 0.82
    return shell * (1.0 - p.ragged * 0.28)
end

-- Fits the crown to what is left of the budget: thin the leaves first, and once thinning would take
-- them below the point of reading as foliage, shrink the lobes instead. Returns the density and the
-- radius scale to build them at.
local function fitCrown(p, tips, radius, spent)
    local budget = BUDGET - spent
    if budget <= 0 or #tips == 0 then return 0, 1.0 end
    local estimate = lobeCost(p, radius) * #tips
    if estimate <= budget then return 1.0, 1.0 end

    local density = budget / estimate
    if density >= MIN_LEAF_DENSITY then return density, 1.0 end
    -- Shrink until the crown costs what MIN_LEAF_DENSITY can pay for. Cost goes as roughly the square
    -- of the radius once the shell thickness has stopped growing with it, which is the regime this
    -- branch is only ever reached in.
    local scale = (budget / (estimate * MIN_LEAF_DENSITY)) ^ 0.5
    return MIN_LEAF_DENSITY, max(0.25, min(1.0, scale))
end

return {
    name = "Trees",
    kind = "scatter",
    description = "Branching broadleaf, whorled conifer and bare trees, each checked for room before it lands.",
    author = "ProjectV",

    needs = { "position", "normal", "crevice", "material" },
    creviceRadius = 2,

    -- Grow through the undergrowth, and through other trees' foliage. See UNDERGROWTH and CANOPY above
    -- for what is on the list and, more importantly, what is kept off it.
    displaces = DISPLACES,

    params = {
        -- The editor's two, read by name.
        { name = "spacing", label = "Spacing", type = "int", default = 14, min = 1, max = 128,
          tooltip = "How far apart trees are, in voxels. Far larger than a grass brush wants: this is\n"
                 .. "roughly the width of a crown, and below it every tree is refused for want of\n"
                 .. "room by the one beside it.\n\n"
                 .. "Scale this with Trunk height. A tall tree at a small spacing plants one tree and\n"
                 .. "refuses its neighbours." },
        { name = "density", label = "Coverage", type = "float", default = 0.6, min = 0.0, max = 1.0,
          tooltip = "What fraction of the eligible cells get a tree. Refusals come out of this: a\n"
                 .. "wooded slope plants far fewer than a flat clearing at the same setting." },

        -- What may grow, in any combination.
        { name = "broadleaf", label = "Broadleaf", type = "bool", default = true,
          tooltip = "A tapering trunk, a branching skeleton, and foliage hung in lobes at the tips." },
        { name = "conifer", label = "Conifer", type = "bool", default = true,
          tooltip = "A straight trunk under whorls of drooping branches. The narrowest, so the one\n"
                 .. "that still fits where a broadleaf will not." },
        { name = "bare", label = "Bare", type = "bool", default = false,
          tooltip = "The same skeleton with no foliage, and branched one level further -- a bare tree\n"
                 .. "is its branching, so there is more of it to see. Fits almost anywhere, which\n"
                 .. "makes it the useful one for a crowded or a dead landscape." },
        { name = "blossom", label = "Blossom", type = "bool", default = false,
          tooltip = "Scatters petals through a broadleaf crown. Costs one more palette entry and\n"
                 .. "nothing else." },

        { name = "height", label = "Trunk height", type = "int", default = 8, min = 2, max = 256,
          tooltip = "The tallest a trunk may be. Each tree takes a share of it, so this is the top of\n"
                 .. "a range rather than a fixed height.\n\n"
                 .. "This is the master control: trunk thickness, branch count and how deeply the\n"
                 .. "skeleton divides are all derived from it unless they are set by hand. Raising it\n"
                 .. "alone gives a bigger tree rather than a longer wire." },
        { name = "canopy", label = "Crown radius", type = "int", default = 4, min = 1, max = 96,
          tooltip = "How wide the foliage is, measured to the outside of the whole crown -- not the\n"
                 .. "size of one lobe. Cost is paid out of the placement budget, so a crown too large\n"
                 .. "for it comes back thinner and then smaller rather than failing to place." },
        { name = "trunk", label = "Trunk radius", type = "int", default = 0, min = 0, max = 32,
          tooltip = "Radius of the trunk at the ground, in voxels. **0 derives it from the height**,\n"
                 .. "which is what you want almost always -- it is the setting that keeps a tall tree\n"
                 .. "from being a wire, and deriving it means one control scales the whole tree.\n\n"
                 .. "Set it only to make something deliberately squat or spindly." },
        { name = "branching", label = "Branch levels", type = "int", default = 0, min = 0, max = 4,
          tooltip = "How many times the skeleton divides. 0 derives it from the height: a small tree\n"
                 .. "has no room for branches worth drawing, a large one needs three levels before\n"
                 .. "the crown stops looking like one lump.\n\n"
                 .. "Each level roughly doubles the limb count, so this is the expensive control." },
        { name = "roots", label = "Roots", type = "float", default = 1.0, min = 0.0, max = 3.0,
          tooltip = "How far surface roots spread from the base, as a multiple of the trunk's radius.\n"
                 .. "0 turns them off.\n\n"
                 .. "They are what makes a tree look planted rather than set down on the ground: a\n"
                 .. "trunk is a cylinder meeting a flat surface at a right angle, and roots are what\n"
                 .. "break that seam. They lie on the surface rather than digging into it, so they\n"
                 .. "never carve the terrain -- which does mean they read best on ground that is\n"
                 .. "roughly level under the trunk." },
        { name = "reach", label = "Branch reach", type = "float", default = 0.6, min = 0.0, max = 1.5,
          tooltip = "How hard branches curl toward the light as they grow. 0 gives straight rays out\n"
                 .. "of the trunk; high values give the upswept look of an old broadleaf." },
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

    materials = {
        { name = "tree.bark",    steps = 3, color = { 0.26, 0.18, 0.12 }, colorTo = { 0.44, 0.33, 0.22 } },
        { name = "tree.leaf",    steps = 4, color = { 0.10, 0.26, 0.09 }, colorTo = { 0.38, 0.62, 0.22 } },
        { name = "tree.needle",  steps = 3, color = { 0.07, 0.20, 0.13 }, colorTo = { 0.20, 0.40, 0.22 } },
        { name = "tree.blossom",            color = { 0.94, 0.76, 0.82 } },
        { name = "tree.dead",    steps = 2, color = { 0.30, 0.26, 0.22 }, colorTo = { 0.46, 0.42, 0.37 } },
        -- These exist to be *named* differently -- see the note by the constants -- but they must
        -- also be *coloured* differently, and that is a hard requirement rather than a preference:
        -- interning a role falls back to matching an existing entry by colour when the name is new,
        -- so a ramp that duplicated tree.bark's colours exactly would bind to tree.bark and never
        -- become an entry of its own. Younger wood is lighter and greyer than trunk bark, so the
        -- shift these need in order to exist is one they wanted anyway.
        { name = "tree.branch",  steps = 3, color = { 0.31, 0.23, 0.15 }, colorTo = { 0.52, 0.41, 0.28 } },
        { name = "tree.deadlimb", steps = 2, color = { 0.35, 0.31, 0.27 }, colorTo = { 0.53, 0.49, 0.44 } },
    },

    apply = function(ctx, p)
        -- ---- 1. The ground ----
        --
        -- The cheapest tests, and the ones that reject the most sites. Everything below them costs
        -- either a scene query or a few thousand voxels of generated tree.
        -- The slack is not a fudge. A discrete normal off real terrain is never exactly (0,1,0), so
        -- the bare comparison makes `maxSlope = 0` mean "refuse everywhere" rather than "flat ground
        -- only" -- a setting that reads as the strictest useful value and is actually the one value
        -- that can never plant a tree. The whole control was unusable at its own minimum.
        if ctx.ny < 1.0 - p.maxSlope - 0.03 then return nil end
        if ctx.crevice > p.enclosure then return nil end

        -- Not on top of the undergrowth. A blade of grass is a surface voxel like any other, so the
        -- scatter lattice offers its tip as a site perfectly happily, and a tree planted there stands
        -- on a stalk with its roots in the air. The ground voxels around it are offered too, and those
        -- are the ones to grow from -- so this is a refusal that costs nothing: it rejects a site the
        -- lattice will make a better offer for.
        if isUndergrowth(ctx.materialName) then return nil end

        local kinds = {}
        if p.broadleaf then kinds[#kinds + 1] = "broadleaf" end
        if p.conifer   then kinds[#kinds + 1] = "conifer" end
        if p.bare      then kinds[#kinds + 1] = "bare" end
        if #kinds == 0 then return nil end

        local r1 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed)
        local r2 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed + 101)
        local r3 = pv.hash(ctx.x, ctx.y, ctx.z, p.seed + 202)
        local kind = kinds[min(#kinds, 1 + floor(r1 * #kinds))]

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

        -- ---- 2. Proportions ----
        --
        -- Everything the tree is, derived from its height before a voxel is generated -- so that the
        -- headroom test below can be told the truth about how tall this tree will be, and so that one
        -- control scales the whole thing. The derivations are the point of the rewrite; the old brush
        -- had a 1-voxel trunk at every height and a crown that did not know how tall it was.
        local trunkHeight = max(2, floor(p.height * (0.55 + r2 * 0.45)))
        local crownRadius = p.canopy
        if kind == "conifer" then crownRadius = max(1, p.canopy * 0.8) end

        -- A real trunk is about a fortieth of its height. A voxel one has to be chunkier than that or
        -- it is a wire: below a radius of about 1.2 a limb is a single line of cells whatever the
        -- arithmetic says, because a ball of radius 0.9 contains exactly one voxel. A ninth is the
        -- ratio that keeps every part of the skeleton above that floor at the sizes this brush is
        -- used at, and it is deliberately stouter than nature.
        local trunkRadius = p.trunk
        if trunkRadius <= 0 then trunkRadius = max(1.2, trunkHeight / 9.0) end
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
        -- Only the skeleton is solved for here. The crown is fitted separately and later (fitCrown),
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
        local topY = ctx.y + trunkHeight + floor(crownRadius * 1.6) + 2
        if not pv.fits(ctx.x, ctx.y + 1, ctx.z, ctx.x, topY, ctx.z) then return nil end

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
        if not pv.fits(ctx.x - footprint, ctx.y + 1, ctx.z - footprint,
                       ctx.x + footprint, ctx.y + 2, ctx.z + footprint) then
            return nil
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
        if p.clearance > 0 then
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
                    if not pv.fits(px, ctx.y + 1, pz, px, top, pz) then return nil end
                end
            end
        end

        -- ---- The tree itself ----

        local tree = newTree()
        local phase = r1 * TAU

        -- ---- Conifer ----
        --
        -- Whorls, not a cone. A conifer's silhouette is layered and you can see the trunk between the
        -- layers; a solid taper is the one shape that cannot read as one, however finely its rim is
        -- eroded. Each whorl is a ring of short drooping branches with needle clusters hung along
        -- them, and the vertical gaps between whorls are as much of the look as the branches.
        if kind == "conifer" then
            local path = trunkPath(ctx, trunkHeight, p.curve * 0.35, dirX, dirZ, arc * 0.35, p.seed)
            growTrunk(tree, path, trunkHeight, trunkRadius, max(0.6, trunkRadius * 0.22), BARK, 3, phase)
            if p.roots > 0 then
                growRoots(tree, ctx, rootCount, trunkRadius * 2.4 * p.roots,
                          max(0.9, trunkRadius * 0.55), BRANCH, 3, phase, p.seed + 811)
            end

            local crownBase = max(2, floor(trunkHeight * 0.28))
            local crownTop = trunkHeight
            local span = max(1, crownTop - crownBase)
            -- **The gap between whorls is the whole effect.** Two voxels apart they merge and the
            -- result is the solid cone this was written to replace -- the layers have to be further
            -- apart than the needle clusters hanging off them are deep, or there is nothing to see
            -- between them. Three voxels is the floor at which the eye separates them at all.
            local step = max(3.0, trunkHeight * 0.085)

            -- Collected first and needled second, for the same reason the broadleaf does it: the
            -- branches are the part that must not be thinned, so they are paid for before the budget
            -- is divided among the foliage.
            local hangs = {}
            local level = crownBase
            local whorl = 0
            while level <= crownTop do
                whorl = whorl + 1
                local t = (level - crownBase) / span
                local jitter = pv.hash(ctx.x, ctx.y + level, ctx.z, p.seed + 71)
                -- The profile: mostly a taper, perturbed per whorl so no two rings agree. Kept under
                -- the crown radius that was asked for -- a jitter that can multiply above 1 makes the
                -- widest whorl wider than the control says, and the control is what the clearance
                -- test was given.
                local profile = crownRadius * ((1.0 - t) ^ 0.75) * (0.72 + jitter * 0.38)
                if profile >= 1.0 then
                    local count = floor(min(11, max(4, 4 + profile * 0.9)))
                    local spin = jitter * TAU
                    local cx = path[min(level, trunkHeight)][1]
                    local cz = path[min(level, trunkHeight)][2]
                    for branch = 1, count do
                        local azimuth = spin + branch * TAU / count
                        local droop = -0.18 - jitter * 0.18
                        -- A gentle sag over the branch's length, not a fall. At a firmer curl the
                        -- lowest whorls -- the longest ones -- reach the ground and pool there, and
                        -- the bottom of the tree becomes a solid slab with the tiers only visible
                        -- above it.
                        -- **A conifer's branch is a twig, whatever the trunk is doing.** Taken from
                        -- the trunk's radius it grew with the tree -- at a large crown that is a
                        -- four-voxel log eighty voxels long, ninety times over, and the wood alone
                        -- came to 66000 of the placement's budget. The needles then had nothing left
                        -- to be built from, so the tree arrived as bare branches with a few beads on
                        -- them. Sized from the branch's own length instead, and capped.
                        local ex, ey, ez, _, _, _ = tree:limb(
                            cx, level, cz, cos(azimuth), droop, sin(azimuth),
                            profile, max(0.8, min(2.6, profile * 0.07)), 0.5,
                            0, -1, 0, 0.18, BRANCH, 3)
                        hangs[#hangs + 1] = { cx, level, cz, ex, ey, ez, profile }
                        if tree.full then break end
                    end
                end
                if tree.full then break end
                level = level + max(1, floor(step * (0.75 + jitter * 0.5) + 0.5))
            end

            -- The leader: a bare spike of trunk above the last whorl, which is what a fir has and
            -- what the old brush's single top voxel was reaching for.
            local leader = max(1, floor(trunkHeight * 0.04))
            for i = 1, leader do
                tree:disc(floor(path[trunkHeight][1] + 0.5), trunkHeight + i,
                          floor(path[trunkHeight][2] + 0.5), 0.6 - i * 0.1, NEEDLE + 2)
            end

            -- ---- Needles ----
            --
            -- Sprays hung along each whorl branch, built by the same shelled, budget-fitted lobe the
            -- broadleaf's crown is made of -- flattened harder, because a conifer's spray is a frond
            -- rather than a ball.
            --
            -- **Their size comes from the branch they hang on.** Capped at a constant radius they did
            -- not grow when the tree did: a branch eighty voxels long carried the same two-voxel beads
            -- as one eight voxels long, which is what made a large conifer read as bare sticks with
            -- decorations. Everything about a tree has to be proportional to it or it only looks right
            -- at the one size it was tuned at -- the same lesson as the trunk, in the other direction.
            local sprays = {}
            for i = 1, #hangs do
                local h = hangs[i]
                -- **The floor is not cosmetic.** Below about two voxels the shell arithmetic in
                -- growLobe degenerates: the warp band collapses onto a handful of candidate cells, the
                -- raggedness test then removes most of those, and consecutive sprays -- spaced by a
                -- fraction of their own radius -- deduplicate into each other. A small conifer came out
                -- with two hundred needles on it for that reason, which is the barren look at the other
                -- end of the size range from the one the proportional radius fixed.
                local sprayRadius = max(2.4, h[7] * 0.20)
                -- Spaced at just under a radius so consecutive sprays merge into a continuous frond
                -- along the branch instead of a string of beads.
                local along = max(2, floor(h[7] / (sprayRadius * 0.9)))
                for c = 1, along do
                    -- Starts a little out from the trunk: the inner part of a whorl branch is bare on
                    -- a real conifer, and it is also where every branch of the whorl converges, so
                    -- foliage there is hidden inside the tree.
                    local t = 0.25 + 0.75 * (c / along)
                    sprays[#sprays + 1] = {
                        h[1] + (h[4] - h[1]) * t,
                        h[2] + (h[5] - h[2]) * t,
                        h[3] + (h[6] - h[3]) * t,
                        -- Tapering toward the branch's tip.
                        sprayRadius * (1.15 - 0.45 * t),
                    }
                end
            end

            -- Fitted to what the skeleton left, exactly as the broadleaf's crown is. fitCrown wants
            -- one representative radius, so it is given the mean.
            local meanRadius = 0
            for i = 1, #sprays do meanRadius = meanRadius + sprays[i][4] end
            meanRadius = #sprays > 0 and (meanRadius / #sprays) or 1
            local density, scale = fitCrown(p, sprays, meanRadius, tree.spent)
            for i = 1, #sprays do
                if tree.full then break end
                local spray = sprays[i]
                growLobe(tree, ctx, p, floor(spray[1] + 0.5), floor(spray[2] + 0.5),
                         floor(spray[3] + 0.5), max(1.0, spray[4] * scale), density,
                         p.seed + 401 + i * 7, NEEDLE, 0.62, 3)
            end

            return tree.voxels
        end

        -- ---- Broadleaf and bare ----
        --
        -- The same skeleton. A bare tree is one with the foliage step skipped and a level more
        -- branching, which is the honest relationship between them: what you see of a bare tree *is*
        -- the structure a leafy one hides.
        local barkMaterial = (kind == "bare") and DEAD or BARK
        local limbMaterial = (kind == "bare") and DEADLIMB or BRANCH
        local barkSteps = (kind == "bare") and 2 or 3

        local path = trunkPath(ctx, trunkHeight, p.curve, dirX, dirZ, arc, p.seed)
        growTrunk(tree, path, trunkHeight, trunkRadius, max(0.5, trunkRadius * 0.3),
                  barkMaterial, barkSteps, phase)
        if p.roots > 0 then
            growRoots(tree, ctx, rootCount, trunkRadius * 2.4 * p.roots,
                      max(0.9, trunkRadius * 0.55), limbMaterial, barkSteps, phase, p.seed + 811)
        end

        local tips = {}
        -- Primary limbs off the upper trunk. More of them on a bigger tree, because the count that
        -- makes a small crown irregular makes a large one bald.
        local primary = floor(min(7, max(3, 3 + crownRadius * 0.22)))
        local first = max(2, floor(trunkHeight * 0.45))

        -- **The crown has to end up the size that was asked for.** A limb is not the whole reach: each
        -- level of branching adds its children's length on top of its own, so a primary limb of the
        -- crown's radius makes a crown two and a half times too wide -- which is what "Crown radius"
        -- did before it meant anything. Divide the reach by the length of the chain that will grow out
        -- of it, and the control means what its label says at every branch depth.
        local spanFactor, term = 1.0, 1.0
        for _ = 1, levels do
            term = term * 0.70
            spanFactor = spanFactor + term
        end
        local limbLength = crownRadius * 0.62 / spanFactor
        for limb = 1, primary do
            local jitter = pv.hash(ctx.x + limb, ctx.y, ctx.z, p.seed + 401)
            local level = min(trunkHeight, first + floor((trunkHeight - first) * (limb - 1) / primary
                                                         + jitter * 2.0))
            local azimuth = phase + limb * 2.39996 + (jitter - 0.5) * 0.9
            -- Higher limbs leave the trunk closer to vertical, which is what stacks a crown rather
            -- than spoking it.
            local rise = 0.35 + 0.75 * (level / trunkHeight)
            local length = limbLength * (0.78 + jitter * 0.44)
            growBranch(tree, ctx, p,
                       path[level][1], level, path[level][2],
                       cos(azimuth), rise, sin(azimuth),
                       length, max(0.9, trunkRadius * 0.62), levels, tips,
                       limbMaterial, barkSteps, p.seed + limb * 37)
            if tree.full then break end
        end

        -- A crown with no branches to hang on still needs somewhere to put its leaves: a small tree
        -- is a trunk with a lobe on top, which is correct for a sapling and is the one case the old
        -- brush's shape was actually right for.
        if #tips == 0 then
            tips[1] = { path[trunkHeight][1], trunkHeight + crownRadius * 0.5,
                        path[trunkHeight][2], crownRadius }
        end

        if kind == "bare" then return tree.voxels end

        -- ---- Foliage ----
        --
        -- **A lobe's size comes from how many there are.** Sized as a fixed fraction of the crown it
        -- works at one branch depth and fails at the next: three lobes at 0.38 of the crown is a
        -- pleasantly lumpy tree, and forty of them at 0.38 is a sphere again -- a bigger primitive
        -- than the one this brush was rewritten to stop making, and the shape a large tree came out as
        -- for exactly that reason. Solving for a share of the crown's volume instead keeps the gaps
        -- between clumps open at every size, which is what lets the branches be seen through the
        -- foliage and is most of what makes a big tree read as a tree.
        --
        -- Under 1: the lobes are meant to overlap, and a coverage of 1 would mean a crown filled
        -- exactly once over -- which, with them clustered around branch tips rather than spread
        -- evenly, is a solid ball with bald patches at the rim.
        local COVERAGE = 0.45
        local lobeRadius = crownRadius * ((COVERAGE / max(1, #tips)) ^ (1.0 / 3.0))
        lobeRadius = max(1.5, min(lobeRadius, crownRadius * (levels > 0 and 0.55 or 1.0)))
        local density, scale = fitCrown(p, tips, lobeRadius, tree.spent)
        lobeRadius = lobeRadius * scale
        for i = 1, #tips do
            if tree.full then break end
            local tip = tips[i]
            local jitter = pv.hash(floor(tip[1]) + i, floor(tip[2]), floor(tip[3]), p.seed + 601)
            growLobe(tree, ctx, p, floor(tip[1] + 0.5), floor(tip[2] + 0.5), floor(tip[3] + 0.5),
                     lobeRadius * (0.75 + jitter * 0.5), density, p.seed + 3 + i * 13)
        end

        if p.blossom then
            -- Sprinkled through the crown afterwards rather than chosen per leaf inside it: the
            -- crown is already written, and a second pass over its voxels is cheaper than a branch
            -- inside the triple loop that generated them.
            for i = 1, #tree.voxels do
                local voxel = tree.voxels[i]
                if voxel[4] >= LEAF and voxel[4] < LEAF + 4 then
                    local n = pv.hash(ctx.x + voxel[1], ctx.y + voxel[2], ctx.z + voxel[3], p.seed + 5)
                    if n > 0.82 then voxel[4] = BLOSSOM end
                end
            end
        end

        return tree.voxels
    end,
}
