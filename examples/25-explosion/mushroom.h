// The mushroom cloud's quasi-simulation: not a fluid solve but a velocity field shaped like one, which
// the cloud's particles are dragged through.
//
//   the cap    a rising toroidal vortex ring: up through its middle, out over the top, down its outside
//              and back in underneath -- the roll that makes a mushroom's cap. It rises fast and slows,
//              and widens as it goes.
//   the stem   an updraft under the cap, narrow, that feeds it from the ground
//   the base   air (and dust) drawn in along the ground toward the stem's foot, then up it
//   turbulence two octaves of curl noise (divergence-free, so it stirs without bunching or emptying)
//   wind       a breeze that grows with height, so the cloud leans and the smoke drifts off late
//
// Each particle also rises by its own heat (buoyancy) and cools as it goes. The particles carry the
// cloud's density too: Grid splats them into the cells the renderer marches (smoke.frag).
#pragma once

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

#include "core/math.h"

namespace mushroom {
    using projv::core::vec3;

    inline float smoothstep(float a, float b, float x) {
        float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // One octave of curl noise: the curl of a vector potential made of sines, `amplitude` m/s.
    inline vec3 curlNoise(vec3 p, float k, float amplitude, float t, float seed) {
        const float a1 = 0.7f * t + seed, a2 = 0.53f * t + 2.1f * seed, a3 = 0.61f * t + 3.3f * seed;
        const float b1 = 1.3f + seed, b2 = 4.1f - seed, b3 = 2.7f + 0.5f * seed;
        // A = (sin(k y + a1) cos(k z + b1), sin(k z + a2) cos(k x + b2), sin(k x + a3) cos(k y + b3))
        const float sy1 = std::sin(k * p.y + a1), cy1 = std::cos(k * p.y + a1);
        const float sz1 = std::sin(k * p.z + b1), cz1 = std::cos(k * p.z + b1);
        const float sz2 = std::sin(k * p.z + a2), cz2 = std::cos(k * p.z + a2);
        const float sx2 = std::sin(k * p.x + b2), cx2 = std::cos(k * p.x + b2);
        const float sx3 = std::sin(k * p.x + a3), cx3 = std::cos(k * p.x + a3);
        const float sy3 = std::sin(k * p.y + b3), cy3 = std::cos(k * p.y + b3);
        vec3 curl((-sx3 * sy3) - (cz2 * cx2),     // dAz/dy - dAy/dz
                  (-sy1 * sz1) - (cx3 * cy3),     // dAx/dz - dAz/dx
                  (-sz2 * sx2) - (cy1 * cz1));    // dAy/dx - dAx/dy
        return curl * amplitude;
    }

    struct Field {
        vec3  origin{0.0f};      // ground zero
        float seconds = 0.0f;    // since the detonation
        // Derived by at().
        float capHeight = 0.0f, capRise = 0.0f, ringRadius = 1.0f, swirl = 0.0f, core = 1.0f;
        float stemRadius = 1.0f, stemSpeed = 0.0f, inflow = 0.0f, turbulence = 0.0f;
        vec3  centre{0.0f};      // the column's axis, where it is at this height (it leans with the wind)
        vec3  wind{0.5f, 0.0f, 0.2f};
        float scale = 1.0f;      // its size: lengths by it, speeds by its square root, the rise by it

        // The field's shape `t` seconds after the detonation.
        void at(float t) {
            seconds = t;
            const float rise = std::exp(-t / 4.5f), speed = std::sqrt(scale);
            capHeight = scale * (6.0f + 50.0f * (1.0f - rise) + 0.45f * t);
            capRise = scale * (50.0f / 4.5f * rise + 0.45f);
            ringRadius = scale * (4.5f + 13.0f * (1.0f - std::exp(-t / 3.5f)));
            swirl = speed * (3.5f + 11.0f * std::exp(-t / 5.0f));
            core = 0.5f * ringRadius;
            stemRadius = scale * 1.8f + 0.2f * ringRadius;
            stemSpeed = speed * (5.0f + 13.0f * std::exp(-t / 4.0f)) * (1.0f - smoothstep(22.0f, 45.0f, t));
            inflow = speed * (1.0f + 5.0f * std::exp(-t / 6.0f));
            turbulence = speed * (0.5f + 1.0f * smoothstep(4.0f, 30.0f, t));
        }

        // How far the column has leaned at height y: the wind takes the cap with it, the stem's foot stays.
        vec3 lean(float y) const {
            return wind * (0.35f * scale * std::max(seconds - 3.0f, 0.0f) * smoothstep(0.0f, capHeight + 1.0f, y));
        }

        vec3 velocity(vec3 p) const {
            vec3 q = p - origin - lean(p.y - origin.y);
            const float r = std::sqrt(q.x * q.x + q.z * q.z);
            const vec3 out = r > 1e-3f ? vec3(q.x / r, 0.0f, q.z / r) : vec3(0.0f);
            const float y = q.y;
            float ur = 0.0f, uy = 0.0f;

            // The cap's ring: poloidal swirl about its core circle, peak `swirl` at `core` from it.
            const float dr = r - ringRadius, dy = y - capHeight;
            const float d2 = dr * dr + dy * dy;
            const float k = 2.0f * swirl * core / (d2 + core * core);
            ur += k * dy;
            uy -= k * dr;
            // The roll holds together: what strays from the ring's core is drawn back toward a tube round
            // it, so the cap stays a torus rather than flying apart into a ball.
            const float dist = std::sqrt(d2);
            if (dist > 1e-3f) {
                const float hold = 0.9f * std::exp(-d2 / (10.0f * core * core)) * (dist - 0.75f * core) / dist;
                ur -= hold * dr;
                uy -= hold * dy;
            }
            // The ring carries what is in the cap up with it.
            const float inCap = std::exp(-(r * r) / (3.2f * ringRadius * ringRadius) - (dy * dy) / (1.5f * ringRadius * ringRadius));
            uy += capRise * inCap;
            // The stem: an updraft under the cap, dying out at the ground (the inflow takes over there).
            // ...and the stem keeps narrow: drawn in toward the axis under the cap.
            if (y < capHeight - core) ur -= 0.35f * r * smoothstep(scale, 4.0f * scale, y) * std::exp(-r / (3.0f * stemRadius));
            if (y < capHeight)
                uy += stemSpeed * std::exp(-(r * r) / (stemRadius * stemRadius)) * smoothstep(0.0f, 3.0f * scale, y) *
                      (1.0f - smoothstep(capHeight - ringRadius * 0.3f, capHeight + 2.0f, y) * 0.5f);
            // The base: drawn in along the ground toward the stem's foot, and up into it.
            const float nearGround = std::exp(-std::max(y, 0.0f) / (3.0f * scale));
            ur -= inflow * nearGround * (r / (r + 3.0f * scale)) * std::exp(-r / (40.0f * scale));
            uy += inflow * 1.2f * nearGround * std::exp(-(r * r) / (4.0f * stemRadius * stemRadius));

            vec3 v = out * ur + vec3(0.0f, uy, 0.0f);
            // Turbulence, stronger in the cap where the roll tears itself up.
            const float stir = turbulence * (0.6f + 0.8f * inCap);
            v += curlNoise(p, 0.11f / scale, 1.8f * stir, seconds, 0.0f) + curlNoise(p, 0.33f / scale, 0.9f * stir, seconds * 1.7f, 5.0f);
            // The breeze, from the ground up.
            v += wind * (0.4f + smoothstep(5.0f * scale, 60.0f * scale, y));
            return v;
        }
    };

    // The cloud's density grid: GRID^3 cells of `cell` metres from `min`, four channels -- smoke
    // density, density x heat, density x dust, density x vapour -- filled by splatting particles as
    // small Gaussians.
    template <int GRID>
    struct Grid {
        vec3  min{0.0f};
        float cell = 1.0f;
        std::vector<float> cells = std::vector<float>(size_t(GRID) * GRID * GRID * 4, 0.0f);

        struct Splat { vec3 at; float sigma, mass, heat, dust, vapour; };

        // Fills the grid from `splats`, on `threads` threads, each owning a range of z slices.
        void build(const std::vector<Splat>& splats, int threads = 4) {
            std::fill(cells.begin(), cells.end(), 0.0f);
            auto work = [&](int zBegin, int zEnd) {
                float wx[32], wy[32], wz[32];
                const float cellVolume = cell * cell * cell;
                for (const Splat& s : splats) {
                    if (s.mass <= 0.0f) continue;
                    const float sigma = std::clamp(s.sigma / cell, 0.55f, 2.2f);
                    const int reach = std::min(int(std::ceil(2.0f * sigma)), 15);
                    const vec3 g = (s.at - min) / cell - 0.5f;
                    const int cx = int(std::floor(g.x)), cy = int(std::floor(g.y)), cz = int(std::floor(g.z));
                    if (cz + reach < zBegin || cz - reach >= zEnd) continue;
                    if (cx + reach < 0 || cx - reach >= GRID || cy + reach < 0 || cy - reach >= GRID) continue;
                    // Separable weights, each axis normalised over the whole footprint (in or out of the
                    // grid) so a particle adds its mass, no more, wherever it is.
                    auto weights = [&](float centre, int c, float* w) {
                        float sum = 0.0f;
                        for (int i = -reach; i <= reach; i++) {
                            const float d = float(c + i) - centre;
                            w[i + reach] = std::exp(-0.5f * d * d / (sigma * sigma));
                            sum += w[i + reach];
                        }
                        for (int i = 0; i <= 2 * reach; i++) w[i] /= sum;
                    };
                    weights(g.x, cx, wx);
                    weights(g.y, cy, wy);
                    weights(g.z, cz, wz);
                    const float density = s.mass / cellVolume;
                    for (int k = -reach; k <= reach; k++) {
                        const int z = cz + k;
                        if (z < zBegin || z >= zEnd || z < 0 || z >= GRID) continue;
                        for (int j = -reach; j <= reach; j++) {
                            const int y = cy + j;
                            if (y < 0 || y >= GRID) continue;
                            const float wyz = density * wz[k + reach] * wy[j + reach];
                            float* row = &cells[(size_t(z) * GRID + y) * GRID * 4];
                            for (int i = -reach; i <= reach; i++) {
                                const int x = cx + i;
                                if (x < 0 || x >= GRID) continue;
                                const float d = wyz * wx[i + reach];
                                row[x * 4 + 0] += d;
                                row[x * 4 + 1] += d * s.heat;
                                row[x * 4 + 2] += d * s.dust;
                                row[x * 4 + 3] += d * s.vapour;
                            }
                        }
                    }
                }
            };
            std::vector<std::thread> pool;
            const int slab = (GRID + threads - 1) / threads;
            for (int t = 1; t < threads; t++) pool.emplace_back(work, t * slab, std::min(GRID, (t + 1) * slab));
            work(0, std::min(GRID, slab));
            for (std::thread& t : pool) t.join();
        }

        // The atlas the renderer reads: z slices side by side, `tiles` to a row, four floats a texel.
        void pack(std::vector<float>& atlas, int tiles) const {
            const int width = GRID * tiles;
            atlas.assign(size_t(width) * width * 4, 0.0f);
            for (int z = 0; z < GRID; z++) {
                const int tx = (z % tiles) * GRID, ty = (z / tiles) * GRID;
                for (int y = 0; y < GRID; y++) {
                    std::copy_n(&cells[(size_t(z) * GRID + y) * GRID * 4], GRID * 4, &atlas[(size_t(ty + y) * width + tx) * 4]);
                }
            }
        }
    };
}
