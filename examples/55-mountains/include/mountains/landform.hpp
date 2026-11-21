// The landform: ground height, in metres above sea level, as a pure function of position.
//
// x is east and z is north, in metres; (0, 0) is "the Plaza". Three layers (plan:
// docs/plans/mountains-santa-fe.md, D7):
//
//   macro    a base level plus features: ranges, volcanic domes, hill masses, basalt mesa fields and
//            a river. Around the origin they are laid out like Santa Fe's surroundings; elsewhere one
//            random feature per 70 km province cell.
//   erosion  a gully filter, octave by octave: stripes that run down the slope found so far, so each
//            octave branches off the last (phasor noise, after runevision's erosion filter).
//   detail   fBm damped by its own slope (Quilez), down to the footprint asked for.
//
// Then glacial cirques carved under the high crests, and the river's gorge cut last.
//
// `footprint` is the size of what the caller samples for (a voxel edge, a map pixel). Octaves with a
// wavelength below about two footprints are left out, so a coarse sample is the average of fine ones
// rather than an alias of them (D5). No state: any thread, any order.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mountains {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
inline double smoothstep(double a, double b, double x) {
    const double t = clamp01((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}
inline double mix(double a, double b, double t) { return a + (b - a) * t; }
// Polynomial smooth minimum, `k` in the units of a and b.
inline double smin(double a, double b, double k) {
    const double h = std::max(k - std::abs(a - b), 0.0) / k;
    return std::min(a, b) - h * h * k * 0.25;
}
inline double smax(double a, double b, double k) { return -smin(-a, -b, k); }

// ---- Hashing and noise ---------------------------------------------------------------------------

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline uint32_t hash2(int64_t i, int64_t j, uint32_t seed) {
    return hash32(uint32_t(i) * 0x8da6b343U ^ hash32(uint32_t(j) * 0xd8163841U ^ seed));
}
inline double unit(uint32_t h) { return double(h >> 8) * (1.0 / 16777216.0); }   // [0, 1)
inline double signedUnit(uint32_t h) { return unit(h) * 2.0 - 1.0; }               // [-1, 1)

struct Noise { double v = 0, dx = 0, dz = 0; };

namespace detail {
// 32 unit gradients, evenly around the circle.
inline const std::array<double, 64>& gradients() {
    static const std::array<double, 64> table = [] {
        std::array<double, 64> t{};
        for (int i = 0; i < 32; ++i) {
            t[size_t(i) * 2] = std::cos(kTau * (i + 0.5) / 32.0);
            t[size_t(i) * 2 + 1] = std::sin(kTau * (i + 0.5) / 32.0);
        }
        return t;
    }();
    return table;
}
}  // namespace detail

// Gradient noise with its analytic derivative (Quilez). Value about [-0.7, 0.7].
inline Noise gradientNoise(double x, double z, uint32_t seed) {
    const auto& g = detail::gradients();
    const double fx0 = std::floor(x), fz0 = std::floor(z);
    const int64_t ix = int64_t(fx0), iz = int64_t(fz0);
    const double fx = x - fx0, fz = z - fz0;
    const double ux = fx * fx * fx * (fx * (fx * 6.0 - 15.0) + 10.0);
    const double uz = fz * fz * fz * (fz * (fz * 6.0 - 15.0) + 10.0);
    const double dux = 30.0 * fx * fx * (fx * (fx - 2.0) + 1.0);
    const double duz = 30.0 * fz * fz * (fz * (fz - 2.0) + 1.0);
    auto grad = [&](int64_t i, int64_t j, double& gx, double& gz) {
        const size_t k = (hash2(i, j, seed) & 31U) * 2;
        gx = g[k]; gz = g[k + 1];
    };
    double gax, gaz, gbx, gbz, gcx, gcz, gdx, gdz;
    grad(ix, iz, gax, gaz);
    grad(ix + 1, iz, gbx, gbz);
    grad(ix, iz + 1, gcx, gcz);
    grad(ix + 1, iz + 1, gdx, gdz);
    const double va = gax * fx + gaz * fz;
    const double vb = gbx * (fx - 1.0) + gbz * fz;
    const double vc = gcx * fx + gcz * (fz - 1.0);
    const double vd = gdx * (fx - 1.0) + gdz * (fz - 1.0);
    const double k4 = va - vb - vc + vd;
    Noise n;
    n.v = va + ux * (vb - va) + uz * (vc - va) + ux * uz * k4;
    n.dx = gax + ux * (gbx - gax) + uz * (gcx - gax) + ux * uz * (gax - gbx - gcx + gdx) + dux * (uz * k4 + vb - va);
    n.dz = gaz + ux * (gbz - gaz) + uz * (gcz - gaz) + ux * uz * (gaz - gbz - gcz + gdz) + duz * (ux * k4 + vc - va);
    return n;
}

inline double noise1(double t, uint32_t seed) { return gradientNoise(t, 0.37, seed).v; }

inline double fbm(double x, double z, int octaves, uint32_t seed) {
    double sum = 0.0, amp = 1.0, norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * gradientNoise(x, z, seed + uint32_t(i) * 7919U).v;
        norm += amp;
        amp *= 0.5;
        x = x * 2.03 + 17.1;
        z = z * 2.03 - 9.7;
    }
    return sum / norm;   // about [-0.7, 0.7]
}

inline double fbm1(double t, int octaves, uint32_t seed) { return fbm(t, 0.37, octaves, seed); }

// One octave of gullies: stripes that run along `dir` (unit), varying across it, one cell to a
// wavelength. Each cell's jittered point carries a phasor; their weighted sum is normalised, so the
// stripes keep full contrast where neighbours agree and fade where they disagree (phasor noise,
// "Phacelle"). The fade is where one gully ends and the next begins: what makes the network branch.
inline Noise gullies(double x, double z, double dirX, double dirZ, uint32_t seed) {
    const double px = -dirZ, pz = dirX;   // across the slope
    const double fx0 = std::floor(x), fz0 = std::floor(z);
    const int64_t ix = int64_t(fx0), iz = int64_t(fz0);
    const double fx = x - fx0, fz = z - fz0;
    double re = 0.0, im = 0.0, wsum = 0.0;
    // Points within 1.5 cells count; cell i's point lies in [i, i+1), so cells -2..2 hold them all.
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            const uint32_t h = hash2(ix + i, iz + j, seed);
            const double dx = fx - (i + unit(h));
            const double dz = fz - (j + unit(hash32(h)));
            const double d2 = dx * dx + dz * dz;
            if (d2 >= 2.25) continue;
            const double q = 1.0 - d2 * (1.0 / 2.25);
            const double w = q * q * q;
            const double ph = kTau * (dx * px + dz * pz);
            re += w * std::cos(ph);
            im += w * std::sin(ph);
            wsum += w;
        }
    }
    const double mag = std::sqrt(re * re + im * im);
    if (mag < 1e-12 || wsum < 1e-12) return {};
    const double c = re / mag, s = im / mag;
    const double coherence = smoothstep(0.1, 0.5, mag / wsum);
    Noise n;
    n.v = c * coherence;
    const double k = -s * kTau * coherence;
    n.dx = k * px;
    n.dz = k * pz;
    return n;
}

// ---- Features ------------------------------------------------------------------------------------

// A mountain range: a crest along a line, flanks falling to the base on each side.
struct Range {
    double cx = 0, cz = 0;           // centre (m)
    double ax = 0, az = 1;           // unit direction along the crest
    double halfLength = 30000;
    double relief = 1500;            // crest height above the base (m)
    double peaks = 0.08;             // crest height varies by this fraction
    double widthLeft = 15000;        // flank width, left of the crest (west, for a crest running north)
    double widthRight = 15000;
    double wander = 2000;            // how far the crest wanders across its line (m)
    uint32_t seed = 1;

    double reach() const { return halfLength + std::max(widthLeft, widthRight); }

    // Where the crest is, across the line, at `t` along it.
    double crestOffset(double t) const {
        return wander * (fbm1(t / 22000.0, 2, seed) + 0.3 * noise1(t / 6000.0, seed + 11));
    }
    double taper(double t) const { return smoothstep(halfLength, halfLength * 0.55, std::abs(t)); }
    // Crest height above the base at `t`.
    double crestRelief(double t) const {
        return relief * taper(t) * (1.0 + peaks * 2.0 * fbm1(t / 6000.0, 3, seed + 23));
    }
    void local(double x, double z, double& t, double& s) const {
        const double dx = x - cx, dz = z - cz;
        t = dx * ax + dz * az;
        s = dx * az - dz * ax;   // + to the right of the direction (east, for a crest running north)
    }
    double uplift(double x, double z) const {
        double t, s;
        local(x, z, t, s);
        if (std::abs(t) > halfLength) return 0.0;
        s -= crestOffset(t);
        const double tp = taper(t);
        // Flanks bulge and narrow along the range: spurs and embayments at the range scale.
        const double w = (s < 0.0 ? widthLeft : widthRight) * (0.55 + 0.45 * tp) *
                         (1.0 + 0.3 * fbm1(t / 14000.0 + (s < 0.0 ? 0.0 : 50.0), 2, seed + 41));
        // Rounded over the crest rather than a knife edge: summits are domes (D8).
        const double round = 1800.0;
        const double q = (std::sqrt(s * s + round * round) - round) / w;
        if (q >= 1.0) return 0.0;
        const double a = 1.0 - q;
        // A concave flank with a long apron: the piedmont ramp is the tail of the 0.12 term.
        const double profile = 0.88 * a * a + 0.12 * std::sqrt(a * std::sqrt(a));
        return crestRelief(t) * profile;
    }
};

// A volcanic shield with a caldera and a resurgent dome inside it (the Jemez).
struct Dome {
    double cx = 0, cz = 0;
    double radius = 30000;           // the shield
    double height = 2000;            // the shield's height at its centre, before the caldera
    double calderaRadius = 0;        // 0: no caldera
    double calderaDepth = 0;         // below the rim
    double resurgentRadius = 0, resurgentHeight = 0;   // above the caldera floor
    double resurgentX = 0, resurgentZ = 0;             // offset from the centre
    uint32_t seed = 2;

    double reach() const { return radius * 1.1; }
    double shield(double r) const {
        if (r >= radius) return 0.0;
        const double a = 1.0 - r / radius;
        return height * a * std::sqrt(a);
    }
    double uplift(double x, double z) const {
        const double dx = x - cx, dz = z - cz;
        double r = std::sqrt(dx * dx + dz * dz);
        if (r > reach()) return 0.0;
        r *= 1.0 + 0.09 * fbm(dx / 9000.0, dz / 9000.0, 2, seed);   // a lumpy outline
        double u = shield(r) * (1.0 + 0.10 * fbm(dx / 4000.0, dz / 4000.0, 2, seed + 5));
        if (calderaRadius > 0.0) {
            const double floor = shield(calderaRadius) - calderaDepth;
            const double rc = calderaRadius * (1.0 + 0.12 * fbm(dx / 6000.0 + 7.0, dz / 6000.0, 2, seed + 9));
            const double wall = smoothstep(rc * 0.9, rc * 1.03, r);
            u = mix(floor, u, wall);
            if (resurgentRadius > 0.0) {
                const double rx = dx - resurgentX, rz = dz - resurgentZ;
                const double rr = std::sqrt(rx * rx + rz * rz) / resurgentRadius;
                if (rr < 1.0) {
                    const double a = 1.0 - rr * rr;
                    u = smax(u, floor + resurgentHeight * a * std::sqrt(a), 60.0);
                }
            }
        }
        return u;
    }
};

// A cluster of rounded hills (laccoliths: the Ortiz, the Cerrillos).
struct HillMass {
    struct Bump { double x, z, height, radius; };
    std::vector<Bump> bumps;
    double cx = 0, cz = 0, extent = 0;

    void finish() {
        double sx = 0, sz = 0;
        for (const Bump& b : bumps) { sx += b.x; sz += b.z; }
        cx = sx / double(bumps.size());
        cz = sz / double(bumps.size());
        extent = 0;
        for (const Bump& b : bumps)
            extent = std::max(extent, std::hypot(b.x - cx, b.z - cz) + b.radius);
    }
    double reach() const { return extent; }
    double uplift(double x, double z) const {
        double u = 0.0;
        for (const Bump& b : bumps) {
            const double d2 = ((x - b.x) * (x - b.x) + (z - b.z) * (z - b.z)) / (b.radius * b.radius);
            if (d2 < 1.0) {
                const double a = 1.0 - d2;
                u += b.height * a * a;
            }
        }
        return u;
    }
};

// A basalt mesa field: a flat cap at one elevation ending in a scarp over a talus apron, with cinder
// cones on it (the Caja del Rio, La Bajada). The cap is level, so its scarp is tallest where the base
// below it is lowest.
struct MesaField {
    struct Cone { double x, z, height, radius; };
    double cx = 0, cz = 0, radius = 12000;
    double capElevation = 2000;      // absolute
    std::vector<Cone> cones;
    uint32_t seed = 3;

    double reach() const { return radius * 1.6; }
    // How far inside the cap's edge, about -1..1; > 0 on the cap.
    double inside(double x, double z) const {
        const double dx = x - cx, dz = z - cz;
        const double d = std::sqrt(dx * dx + dz * dz) / radius;
        return (1.0 - d) + 0.5 * fbm(dx / 6500.0, dz / 6500.0, 3, seed) - 0.05;
    }
    // Uplift over `base`, and how much of the surface is basalt (0..1).
    double uplift(double x, double z, double base, double& basalt) const {
        const double dx = x - cx, dz = z - cz;
        if (dx * dx + dz * dz > reach() * reach()) return 0.0;
        const double m = inside(x, z);
        const double above = std::max(capElevation - base, 0.0);
        const double cap = smoothstep(0.0, 0.006, m);
        double u = above * (0.25 * smoothstep(-0.05, 0.0, m) + 0.75 * cap);
        basalt = std::max(basalt, cap);
        for (const Cone& c : cones) {
            const double d = std::hypot(x - c.x, z - c.z) / c.radius;
            if (d >= 1.0) continue;
            const double a = 1.0 - d;
            double cone = c.height * a * std::sqrt(a);
            if (d < 0.16) cone -= c.height * 0.22 * (1.0 - d / 0.16) * (1.0 - d / 0.16);   // the crater
            u = smax(u, above * cap + cone, 20.0);
            basalt = std::max(basalt, smoothstep(0.0, 0.2, a));
        }
        return u;
    }
};

// A river: a polyline with an elevation at each point. The base falls toward it, and its gorge is cut
// through whatever stands in its way (White Rock Canyon, through the Jemez flank).
struct River {
    struct Point { double x, z, elevation; };
    std::vector<Point> points;
    uint32_t seed = 4;

    // Distance to the river (meandering) and its elevation at the nearest point.
    void nearest(double x, double z, double& distance, double& elevation) const {
        // Meanders: move the query rather than the line.
        const double mx = x + 1200.0 * fbm(x / 7000.0, z / 7000.0, 2, seed);
        const double mz = z + 1200.0 * fbm(x / 7000.0 + 31.0, z / 7000.0, 2, seed);
        double best = 1e300;
        elevation = points.empty() ? 0.0 : points[0].elevation;
        // The elevation is blended over every segment by distance, not taken from the nearest one:
        // where the nearest segment changes, a jump in elevation would leave a crease in the base.
        double eSum = 0.0, wSum = 0.0;
        std::array<double, 32> dist{}, elev{};
        const size_t n = std::min<size_t>(points.size() - 1, dist.size());
        for (size_t i = 0; i < n; ++i) {
            const Point& a = points[i];
            const Point& b = points[i + 1];
            const double ex = b.x - a.x, ez = b.z - a.z;
            const double len2 = ex * ex + ez * ez;
            const double t = clamp01(((mx - a.x) * ex + (mz - a.z) * ez) / len2);
            const double px = a.x + ex * t - mx, pz = a.z + ez * t - mz;
            const double d2 = px * px + pz * pz;
            dist[i] = std::sqrt(d2);
            elev[i] = mix(a.elevation, b.elevation, t);
            best = std::min(best, d2);
        }
        distance = std::sqrt(best);
        for (size_t i = 0; i < n; ++i) {
            const double w = std::exp(-(dist[i] - distance) / 3000.0);
            eSum += w * elev[i];
            wSum += w;
        }
        if (wSum > 0.0) elevation = eSum / wSum;
    }
};

// ---- The landform --------------------------------------------------------------------------------

struct Sample {
    double height = 0;      // metres above sea level
    double gx = 0, gz = 0;  // d height / dx, dz
    double core = 0;        // 0: basin sediment .. 1: crystalline core of a range
    double basalt = 0;      // 0..1: basalt cap or cone
    double cirque = 0;      // 1 on a cirque floor (a tarn where the floor is lowest)
    double river = 0;       // 1 in the river
    double badland = 0;     // 0..1: soft piedmont sediment, dissected
};

struct Settings {
    uint32_t seed = 1;
    // Erosion: octaves from `erosionWavelength` down, each `kErosion * wavelength * slope` tall.
    double erosionWavelength = 4500.0;
    double erosionFinest = 6.0;
    double kErosion = 0.42;
    double erosionGain = 0.62;       // per octave, on top of the wavelength halving
    double erosionSlopeCap = 0.7;
    double erosionBias = 0.25;       // valleys cut deeper than ridges stand up
    // Detail: damped fBm from `detailWavelength` down to the footprint.
    double detailWavelength = 160.0;
    double detailAmplitude = 0.03;   // fraction of the wavelength
    // Above this the summits round off: erosion fades (D8).
    double summitStart = 3400.0, summitFull = 3750.0;
    // Cirques are carved under crests above this.
    double cirqueCrest = 3450.0;
    double cirqueSpacing = 3200.0, cirqueRadius = 950.0, cirqueDepth = 330.0;
    // The layout around the origin (Santa Fe) stands in for random provinces within this radius.
    double layoutRadius = 95000.0;
    double provinceCell = 50000.0;
};

struct Macro {
    double height = 0;
    double base = 0;
    double core = 0;
    double basalt = 0;
};

class Landform {
public:
    explicit Landform(Settings settings = {}) : s_(settings) { layOutSantaFe(); }

    const Settings& settings() const { return s_; }

    // The base level: a low regional surface, falling toward the river where there is one.
    double base(double x, double z, double* riverDistance = nullptr, double* riverElevation = nullptr) const {
        double b = 1800.0 + 320.0 * fbm(x / 140000.0, z / 140000.0, 2, s_.seed + 101);
        // Low rolling ground in the basins, so their arroyos have slopes to follow.
        const double roll = 35.0 * fbm(x / 7000.0, z / 7000.0, 3, s_.seed + 103);
        if (!river_.points.empty()) {
            double d, e;
            river_.nearest(x, z, d, e);
            if (riverDistance) *riverDistance = d;
            if (riverElevation) *riverElevation = e;
            const double rise = 600.0 * (1.0 - std::exp(-0.012 * std::max(d - 1500.0, 0.0) / 600.0));
            b = smin(b + 400.0, e + rise, 250.0);
        }
        return b + roll;
    }

    // The large-scale surface: base plus every feature.
    Macro macro(double x, double z) const {
        Macro m;
        m.base = base(x, z);
        double uplift = 0.0, coreUplift = 0.0;
        auto addRange = [&](const Range& r) {
            if (std::abs(x - r.cx) > r.reach() || std::abs(z - r.cz) > r.reach()) return;
            const double u = r.uplift(x, z);
            uplift += u;
            coreUplift += u;
        };
        auto addDome = [&](const Dome& d) {
            if (std::abs(x - d.cx) > d.reach() || std::abs(z - d.cz) > d.reach()) return;
            const double u = d.uplift(x, z);
            uplift += u;
            coreUplift += u;
        };
        auto addHills = [&](const HillMass& h) {
            if (std::abs(x - h.cx) > h.reach() || std::abs(z - h.cz) > h.reach()) return;
            const double u = h.uplift(x, z);
            uplift += u;
            coreUplift += u;
        };
        auto addMesa = [&](const MesaField& f) {
            if (std::abs(x - f.cx) > f.reach() || std::abs(z - f.cz) > f.reach()) return;
            uplift += f.uplift(x, z, m.base, m.basalt);
        };
        for (const Range& r : ranges_) addRange(r);
        for (const Dome& d : domes_) addDome(d);
        for (const HillMass& h : hills_) addHills(h);
        for (const MesaField& f : mesas_) addMesa(f);

        // Random provinces, outside the laid-out region.
        const double cell = s_.provinceCell;
        const int64_t ci = int64_t(std::floor(x / cell)), cj = int64_t(std::floor(z / cell));
        for (int64_t j = cj - 1; j <= cj + 1; ++j) {
            for (int64_t i = ci - 1; i <= ci + 1; ++i) {
                Province p;
                if (!province(i, j, p)) continue;
                switch (p.kind) {
                    case Province::RangeKind: addRange(p.range); break;
                    case Province::DomeKind: addDome(p.dome); break;
                    case Province::HillKind: addHills(p.hills); break;
                    case Province::MesaKind: addMesa(p.mesa); break;
                    default: break;
                }
            }
        }
        // Soft ceiling: overlapping uplifts don't stack into impossible peaks.
        if (uplift > 2200.0) uplift = 2200.0 + (uplift - 2200.0) * 0.3;
        m.height = m.base + uplift;
        m.core = smoothstep(250.0, 650.0, coreUplift);
        return m;
    }

    Sample sample(double x, double z, double footprint) const {
        Sample out;
        const Macro m = macro(x, z);
        double h = m.height;
        out.core = m.core;
        out.basalt = m.basalt;

        // The macro surface's gradient, by central differences (it is smooth at this scale).
        const double e = 60.0;
        double gx = (macro(x + e, z).height - macro(x - e, z).height) / (2.0 * e);
        double gz = (macro(x, z + e).height - macro(x, z - e).height) / (2.0 * e);

        // Soft sediment: the piedmont, low and gently sloping, away from the cores and caps.
        out.badland = (1.0 - m.core) * (1.0 - m.basalt) * smoothstep(1700.0, 1900.0, h) *
                      (1.0 - smoothstep(2350.0, 2500.0, h));

        // Erosion.
        const double relief = std::max(m.height - m.base, 0.0);
        // The piedmont (a few hundred metres over the base) keeps shallow arroyos; the mountains get
        // their canyons.
        double reliefCap = 0.3 * relief * smoothstep(150.0, 700.0, relief) + 7.0;
        double wavelength = s_.erosionWavelength;
        double gain = 1.0;
        const double cutoff = std::max(s_.erosionFinest, 2.0 * footprint);
        for (int octave = 0; wavelength >= cutoff * 0.5; ++octave) {
            const double fade = smoothstep(cutoff * 0.5, cutoff, wavelength);
            const double slope = std::sqrt(gx * gx + gz * gz);
            if (slope > 1e-5) {
                const double summit = 1.0 - 0.5 * smoothstep(s_.summitStart, s_.summitFull, h);
                const double capFlat = 1.0 - 0.7 * m.basalt;
                // All octaves together cut no deeper than about 0.6x what stands above the base: a
                // 100 m scarp gets gullies, not 4 km-wavelength spikes, and the piedmont keeps its
                // shallow arroyos.
                const double amp = std::min(s_.kErosion * gain * wavelength * std::min(slope, s_.erosionSlopeCap),
                                            reliefCap) *
                                   summit * capFlat * fade;
                const Noise n = gullies(x / wavelength, z / wavelength, gx / slope, gz / slope,
                                        s_.seed * 977U + uint32_t(octave) * 131U);
                // Biased: valleys (v < 0) cut deeper than ridges (v > 0) stand up.
                const double v = n.v - s_.erosionBias * (1.0 - n.v * n.v);
                const double dv = 1.0 + 2.0 * s_.erosionBias * n.v;
                h += amp * v;
                gx += amp * dv * n.dx / wavelength;
                gz += amp * dv * n.dz / wavelength;
            }
            wavelength *= 0.5;
            gain *= s_.erosionGain;
            reliefCap *= 0.6;
        }

        // Detail.
        {
            double lambda = s_.detailWavelength;
            double ddx = 0.0, ddz = 0.0;
            const double slope = std::sqrt(gx * gx + gz * gz);
            const double rough = 0.4 + 1.2 * smoothstep(0.2, 0.8, slope);
            for (int octave = 0; lambda >= footprint; ++octave) {
                const double fade = smoothstep(footprint, 2.0 * footprint, lambda);
                const Noise n = gradientNoise(x / lambda, z / lambda, s_.seed * 31U + 5000U + uint32_t(octave));
                ddx += n.dx;
                ddz += n.dz;
                const double amp = s_.detailAmplitude * lambda * rough * fade / (1.0 + ddx * ddx + ddz * ddz);
                h += amp * n.v;
                gx += amp * n.dx / lambda;
                gz += amp * n.dz / lambda;
                lambda *= 0.5;
            }
        }

        // Cirques under the high crests.
        out.cirque = 0.0;
        for (const Range& r : ranges_) carveCirques(r, x, z, h, out.cirque);

        // The river: its gorge cut through anything above it.
        if (!river_.points.empty()) {
            double d, re;
            river_.nearest(x, z, d, re);
            const double gorge = re + std::max(d - 45.0, 0.0) * 1.15;
            if (gorge < h + 30.0) {
                const double before = h;
                h = smin(h, gorge, 30.0);
                if (h < before - 0.5) {
                    // The gorge's walls replace the slope there; good enough for shading.
                    const double w = clamp01((before - h) / 20.0);
                    double rx = 0, rz = 0;
                    const double eps = 25.0;
                    double d1, e1, d2, e2;
                    river_.nearest(x + eps, z, d1, e1);
                    river_.nearest(x - eps, z, d2, e2);
                    rx = (d1 - d2) / (2.0 * eps) * 1.15;
                    river_.nearest(x, z + eps, d1, e1);
                    river_.nearest(x, z - eps, d2, e2);
                    rz = (d1 - d2) / (2.0 * eps) * 1.15;
                    gx = mix(gx, rx, w);
                    gz = mix(gz, rz, w);
                }
            }
            out.river = 1.0 - smoothstep(30.0, 60.0, d);
        }

        out.height = h;
        out.gx = gx;
        out.gz = gz;
        return out;
    }

    double height(double x, double z, double footprint) const { return sample(x, z, footprint).height; }

    const std::vector<Range>& ranges() const { return ranges_; }

private:
    struct Province {
        enum Kind { None, RangeKind, DomeKind, HillKind, MesaKind } kind = None;
        Range range;
        Dome dome;
        HillMass hills;
        MesaField mesa;
    };

    // One random feature per province cell, from its hash alone.
    bool province(int64_t i, int64_t j, Province& p) const {
        const double cell = s_.provinceCell;
        const uint32_t h = hash2(i, j, s_.seed * 7477U + 3U);
        auto rnd = [&](uint32_t k) { return unit(hash32(h + k * 0x9e3779b9U)); };
        const double cx = (double(i) + 0.25 + 0.5 * rnd(1)) * cell;
        const double cz = (double(j) + 0.25 + 0.5 * rnd(2)) * cell;
        if (cx * cx + cz * cz < s_.layoutRadius * s_.layoutRadius) return false;
        const double kind = rnd(3);
        if (kind < 0.55) {
            p.kind = Province::RangeKind;
            Range& r = p.range;
            r.cx = cx; r.cz = cz;
            const double angle = (rnd(4) - 0.5) * 1.2;   // mostly north-south, like the Rockies
            r.ax = std::sin(angle); r.az = std::cos(angle);
            r.halfLength = 18000.0 + 30000.0 * rnd(5);
            r.relief = 900.0 + 1000.0 * rnd(6);
            r.peaks = 0.05 + 0.08 * rnd(7);
            r.widthLeft = 6000.0 + 14000.0 * rnd(8);
            r.widthRight = 6000.0 + 14000.0 * rnd(9);
            r.wander = 800.0 + 2500.0 * rnd(10);
            r.seed = h;
        } else if (kind < 0.67) {
            p.kind = Province::DomeKind;
            Dome& d = p.dome;
            d.cx = cx; d.cz = cz;
            d.radius = 15000.0 + 15000.0 * rnd(4);
            d.height = 900.0 + 1300.0 * rnd(5);
            if (rnd(6) < 0.5) {
                d.calderaRadius = d.radius * (0.25 + 0.15 * rnd(7));
                d.calderaDepth = 250.0 + 300.0 * rnd(8);
                d.resurgentRadius = d.calderaRadius * 0.45;
                d.resurgentHeight = d.calderaDepth * (0.8 + 1.0 * rnd(9));
                d.resurgentX = (rnd(10) - 0.5) * d.calderaRadius * 0.4;
                d.resurgentZ = (rnd(11) - 0.5) * d.calderaRadius * 0.4;
            }
            d.seed = h;
        } else if (kind < 0.75) {
            p.kind = Province::MesaKind;
            MesaField& f = p.mesa;
            f.cx = cx; f.cz = cz;
            f.radius = 8000.0 + 10000.0 * rnd(4);
            f.capElevation = base(cx, cz) + 90.0 + 160.0 * rnd(5);
            f.seed = h;
            const int cones = int(rnd(6) * 5.0);
            for (int k = 0; k < cones; ++k) {
                const double a = kTau * rnd(20 + uint32_t(k)), rr = f.radius * 0.7 * std::sqrt(rnd(40 + uint32_t(k)));
                const double ht = 80.0 + 160.0 * rnd(60 + uint32_t(k));
                f.cones.push_back({cx + std::cos(a) * rr, cz + std::sin(a) * rr, ht, ht * 2.4});
            }
        } else if (kind < 0.92) {
            p.kind = Province::HillKind;
            HillMass& hm = p.hills;
            const int n = 2 + int(rnd(4) * 5.0);
            for (int k = 0; k < n; ++k) {
                const double a = kTau * rnd(20 + uint32_t(k)), rr = 6000.0 * std::sqrt(rnd(40 + uint32_t(k)));
                hm.bumps.push_back({cx + std::cos(a) * rr, cz + std::sin(a) * rr,
                                    200.0 + 550.0 * rnd(60 + uint32_t(k)), 2500.0 + 3500.0 * rnd(80 + uint32_t(k))});
            }
            hm.finish();
        } else {
            return false;   // 8%: open basin
        }
        return true;
    }

    // Bowls bitten out of a crest's north- and east-facing flanks where it is high enough.
    void carveCirques(const Range& r, double x, double z, double& h, double& cirque) const {
        double t, s;
        r.local(x, z, t, s);
        if (std::abs(t) > r.halfLength || std::abs(s) > 8000.0) return;
        const double spacing = s_.cirqueSpacing;
        const int64_t k0 = int64_t(std::floor(t / spacing));
        for (int64_t k = k0 - 1; k <= k0 + 1; ++k) {
            for (int side = -1; side <= 1; side += 2) {
                const uint32_t hh = hash2(k, side, r.seed * 31U + 77U);
                if (unit(hh) < 0.45) continue;
                // Outward from the crest, on this side.
                const double ox = side * r.az, oz = -side * r.ax;
                if (ox * 0.62 + oz * 0.78 < -0.2) continue;   // only north- and east-facing
                const double tk = (double(k) + 0.25 + 0.5 * unit(hash32(hh))) * spacing;
                const double crestAbs = base(r.cx + r.ax * tk, r.cz + r.az * tk) + r.crestRelief(tk);
                if (crestAbs < s_.cirqueCrest) continue;
                const double radius = s_.cirqueRadius * (0.75 + 0.5 * unit(hash32(hh + 1)));
                const double sk = r.crestOffset(tk) + side * radius * 1.05;
                // Local frame: u outward, v along the crest.
                const double u = (s - sk) * side;
                const double v = t - tk;
                const double uu = u > 0.0 ? u * 0.4 : u;   // open downhill
                const double d = std::sqrt(v * v + uu * uu) / radius;
                if (d > 1.6) continue;
                const double floor = crestAbs - s_.cirqueDepth * (0.8 + 0.4 * unit(hash32(hh + 2)));
                const double bowl = floor + 1100.0 * std::pow(d, 2.6);
                if (bowl < h) {
                    h = smin(h, bowl, 35.0);
                    cirque = std::max(cirque, (1.0 - smoothstep(0.1, 0.35, d)) * (unit(hash32(hh + 3)) < 0.5 ? 1.0 : 0.85));
                }
            }
        }
    }

    // The surroundings of Santa Fe, from the research table in the plan (positions in km from the
    // Plaza, x east, z north).
    void layOutSantaFe() {
        const double km = 1000.0;
        // The Sangre de Cristo: a long crest running north 15 km east of town, its south end
        // dropping toward Glorieta.
        {
            Range r;
            r.cx = 15.5 * km; r.cz = 40.0 * km;
            r.ax = 0.0; r.az = 1.0;
            r.halfLength = 66.0 * km;
            r.relief = 1720.0;
            r.peaks = 0.12;
            r.widthLeft = 21.0 * km;
            r.widthRight = 15.0 * km;
            r.wander = 2200.0;
            r.seed = s_.seed * 13U + 1U;
            ranges_.push_back(r);
        }
        // The Sandias: a tilted fault block, a steep west face and a long east dip slope.
        {
            Range r;
            r.cx = -30.0 * km; r.cz = -62.0 * km;
            r.ax = std::sin(0.17); r.az = std::cos(0.17);
            r.halfLength = 15.0 * km;
            r.relief = 1150.0;
            r.peaks = 0.05;
            r.widthLeft = 4.5 * km;
            r.widthRight = 15.0 * km;
            r.wander = 700.0;
            r.seed = s_.seed * 13U + 2U;
            ranges_.push_back(r);
        }
        // The Jemez: a broad shield with the Valles Caldera and Redondo Peak.
        {
            Dome d;
            d.cx = -58.0 * km; d.cz = 20.0 * km;
            d.radius = 32.0 * km;
            d.height = 2050.0;
            d.calderaRadius = 11.0 * km;
            d.calderaDepth = 480.0;
            d.resurgentRadius = 5.5 * km;
            d.resurgentHeight = 620.0;
            d.resurgentX = 1.5 * km; d.resurgentZ = 1.0 * km;
            d.seed = s_.seed * 13U + 3U;
            domes_.push_back(d);
        }
        // The Ortiz and the Cerrillos hills.
        {
            HillMass h;
            h.bumps = {{-3.0 * km, -38.0 * km, 330.0, 5.0 * km},
                       {-0.5 * km, -36.5 * km, 180.0, 3.5 * km},
                       {-5.0 * km, -40.0 * km, 200.0, 4.0 * km},
                       {-1.0 * km, -41.5 * km, 250.0, 3.0 * km}};
            h.finish();
            hills_.push_back(h);
            HillMass c;
            c.bumps = {{-10.0 * km, -27.0 * km, 170.0, 2.5 * km},
                       {-8.5 * km, -25.5 * km, 120.0, 2.0 * km},
                       {-11.5 * km, -28.5 * km, 110.0, 1.8 * km}};
            c.finish();
            hills_.push_back(c);
        }
        // The Caja del Rio: a basalt cap west of town, ending south in La Bajada.
        {
            MesaField f;
            f.cx = -18.0 * km; f.cz = -8.0 * km;
            f.radius = 13.0 * km;
            f.capElevation = 1990.0;
            f.seed = s_.seed * 13U + 4U;
            f.cones = {{-22.0 * km, -18.0 * km, 230.0, 560.0},
                       {-15.0 * km, -4.0 * km, 200.0, 500.0},
                       {-24.0 * km, -8.0 * km, 150.0, 380.0},
                       {-12.0 * km, -13.0 * km, 120.0, 320.0}};
            mesas_.push_back(f);
        }
        // The Rio Grande: from Española, through White Rock Canyon, past Cochiti, to Albuquerque.
        river_.seed = s_.seed * 13U + 5U;
        river_.points = {{-6.0 * km, 110.0 * km, 1830.0}, {-10.0 * km, 45.0 * km, 1715.0},
                         {-14.0 * km, 34.0 * km, 1695.0}, {-22.0 * km, 20.0 * km, 1675.0},
                         {-28.0 * km, 8.0 * km, 1655.0},  {-33.0 * km, -5.0 * km, 1640.0},
                         {-38.0 * km, -18.0 * km, 1620.0}, {-41.0 * km, -35.0 * km, 1590.0},
                         {-40.0 * km, -60.0 * km, 1540.0}, {-41.0 * km, -90.0 * km, 1500.0},
                         {-40.0 * km, -150.0 * km, 1440.0}};
    }

    Settings s_;
    std::vector<Range> ranges_;
    std::vector<Dome> domes_;
    std::vector<HillMass> hills_;
    std::vector<MesaField> mesas_;
    River river_;
};

}  // namespace mountains
