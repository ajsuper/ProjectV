// mountains_lab: the landform in 2D, in seconds (plan: docs/plans/mountains-santa-fe.md, D13).
//
// Writes, into --out (default ./lab):
//   region.png   160 x 160 km around the Plaza, 100 m a pixel: hillshade over surface colour
//   local.png    the Plaza to the crest, 24 x 24 km, 12 m a pixel
//   profile.png  elevation along two west-east lines: through the Plaza, and through Baldy
//   pano_east.png, pano_west.png   the view from the Plaza (a heightfield raycast, no shadows)
// and prints elevations at reference points against their targets.
//
//   ./mountains_lab [--seed N] [--out DIR] [--only region|local|profile|pano] [--at X,Z (km)]
//                   [--curvature]
#include "mountains/landform.hpp"
#include "mountains/surface.hpp"

#include <bimg/bimg.h>
#include <bx/file.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace mountains;

namespace {

struct Image {
    int w, h;
    std::vector<uint8_t> px;
    Image(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * size_t(h_) * 4, 255) {}
    void set(int x, int y, Rgb c) {
        auto enc = [](double v) {
            v = std::clamp(v, 0.0, 1.0);
            return uint8_t(std::lround(v * 255.0));
        };
        uint8_t* p = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
        p[0] = enc(c.r); p[1] = enc(c.g); p[2] = enc(c.b); p[3] = 255;
    }
    bool save(const std::string& path) const {
        bx::FileWriter writer;
        bx::Error err;
        if (!bx::open(&writer, path.c_str(), false, &err)) return false;
        bimg::imageWritePng(&writer, uint32_t(w), uint32_t(h), uint32_t(w) * 4, px.data(),
                            bimg::TextureFormat::RGBA8, false, &err);
        bx::close(&writer);
        return err.isOk();
    }
};

void parallelFor(int n, const std::function<void(int)>& fn) {
    std::atomic<int> next{0};
    const unsigned threads = std::max(1U, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (int i = next++; i < n; i = next++) fn(i);
        });
    for (auto& th : pool) th.join();
}

Rgb surfaceColour(const Sample& s, double x, double z, double footprint) {
    return mixColour(surfaceMix(s, x, z, footprint));
}

double shade(const Sample& s, double lx, double ly, double lz) {
    const double nx = -s.gx, ny = 1.0, nz = -s.gz;
    const double inv = 1.0 / std::sqrt(nx * nx + ny * ny + nz * nz);
    return std::max(0.0, (nx * lx + ny * ly + nz * lz) * inv);
}

// ---- Maps ----

void map(const Landform& land, const std::string& path, double cx, double cz, double size, int pixels,
         bool markPlaza) {
    Image img(pixels, pixels);
    const double step = size / pixels;
    const double l = 1.0 / std::sqrt(0.36 + 0.81 + 0.36);
    const int tile = 64, tiles = (pixels + tile - 1) / tile;
    parallelFor(tiles * tiles, [&](int t) {
        const int ti = t % tiles, tj = t / tiles;
        const int nx = std::min(tile, pixels - ti * tile), nz = std::min(tile, pixels - tj * tile);
        // Rows run north to south: sample from the tile's south edge up.
        const double x0 = cx - size * 0.5 + (ti * tile + 0.5) * step;
        const double zTop = cz + size * 0.5 - (tj * tile + 0.5) * step;
        const double z0 = zTop - (nz - 1) * step;
        std::vector<Sample> g;
        land.sampleGrid(x0, z0, step, nx, nz, step, g);
        for (int j = 0; j < nz; ++j)
            for (int i = 0; i < nx; ++i) {
                const Sample& s = g[size_t(j * nx + i)];
                const double x = x0 + i * step, z = z0 + j * step;
                const double lit = 0.3 + 0.85 * shade(s, -0.6 * l, 0.9 * l, 0.6 * l);
                img.set(ti * tile + i, tj * tile + (nz - 1 - j), surfaceColour(s, x, z, step) * lit);
            }
    });
    if (markPlaza) {
        const int pcx = int((0.0 - (cx - size * 0.5)) / step), pcz = int(((cz + size * 0.5) - 0.0) / step);
        for (int d = -4; d <= 4; ++d)
            for (int t = -1; t <= 1; ++t) {
                auto put = [&](int x, int y) {
                    if (x >= 0 && y >= 0 && x < pixels && y < pixels) img.set(x, y, {1.0, 0.1, 0.1});
                };
                put(pcx + d, pcz + t);
                put(pcx + t, pcz + d);
            }
    }
    img.save(path);
}

// ---- Profile ----

void profile(const Landform& land, const std::string& path) {
    const int w = 1600, h = 520;
    const double x0 = -60000.0, x1 = 40000.0;
    const double e0 = 1400.0, e1 = 4100.0;
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img.set(x, y, {0.97, 0.97, 0.96});
    auto rowOf = [&](double e) { return int((e1 - e) / (e1 - e0) * h); };
    for (double e = 1500.0; e < e1; e += 500.0) {
        const int r = rowOf(e);
        for (int x = 0; x < w; ++x) img.set(x, r, {0.8, 0.8, 0.8});
    }
    for (double xk = x0; xk <= x1; xk += 10000.0) {
        const int c = int((xk - x0) / (x1 - x0) * w);
        for (int y = 0; y < h; ++y) img.set(std::min(c, w - 1), y, {0.85, 0.85, 0.85});
    }
    struct Line { double z; Rgb colour; };
    const Line lines[] = {{0.0, {0.55, 0.3, 0.2}}, {17000.0, {0.2, 0.3, 0.55}}};
    for (const Line& line : lines) {
        std::vector<double> e(static_cast<size_t>(w));
        const double step = (x1 - x0) / w;
        const int chunk = 64;
        parallelFor((w + chunk - 1) / chunk, [&](int t) {
            const int n = std::min(chunk, w - t * chunk);
            std::vector<Sample> g;
            land.sampleGrid(x0 + (t * chunk + 0.5) * step, line.z, step, n, 1, step, g);
            for (int i = 0; i < n; ++i) e[size_t(t * chunk + i)] = g[size_t(i)].height;
        });
        for (int c = 0; c < w; ++c) {
            const int r = std::clamp(rowOf(e[size_t(c)]), 0, h - 1);
            const int prev = c > 0 ? std::clamp(rowOf(e[size_t(c - 1)]), 0, h - 1) : r;
            for (int y = std::min(r, prev); y <= std::max(r, prev); ++y) img.set(c, y, line.colour);
        }
    }
    // Targets: Plaza 2133 at x=0; Atalaya 2781 at x=6 km (on the Plaza line).
    auto target = [&](double x, double e, Rgb c) {
        const int col = int((x - x0) / (x1 - x0) * w), r = rowOf(e);
        for (int d = -5; d <= 5; ++d) {
            img.set(std::clamp(col + d, 0, w - 1), r, c);
            img.set(col, std::clamp(r + d, 0, h - 1), c);
        }
    };
    target(0.0, 2133.0, {0.9, 0.1, 0.1});
    target(6000.0, 2781.0, {0.9, 0.1, 0.1});
    target(14500.0, 3850.0, {0.1, 0.1, 0.9});   // Baldy, on the second line
    img.save(path);
}

// ---- Panorama from the Plaza ----

// Height rings around a point, the lab's own small clipmap: ring k is 512^2 samples 0.5 * 2^k m apart.
struct HeightRings {
    struct Ring { double x0, z0, step; int n; std::vector<Sample> s; };
    std::vector<Ring> rings;
    HeightRings(const Landform& land, double cx, double cz, int count) {
        const int n = 512, tile = 64;
        for (int k = 0; k < count; ++k) {
            Ring r;
            r.step = 0.5 * double(1 << k);
            r.n = n;
            r.x0 = cx - r.step * n * 0.5;
            r.z0 = cz - r.step * n * 0.5;
            r.s.resize(size_t(n) * size_t(n));
            const int tiles = n / tile;
            parallelFor(tiles * tiles, [&](int t) {
                const int ti = t % tiles, tj = t / tiles;
                std::vector<Sample> g;
                land.sampleGrid(r.x0 + ti * tile * r.step, r.z0 + tj * tile * r.step, r.step, tile, tile, r.step, g);
                for (int j = 0; j < tile; ++j)
                    for (int i = 0; i < tile; ++i)
                        r.s[size_t(tj * tile + j) * size_t(n) + size_t(ti * tile + i)] = g[size_t(j * tile + i)];
            });
            rings.push_back(std::move(r));
        }
    }
    // The finest ring holding (x, z) with a sample of border: the nearest sample, its height bilinear.
    const Sample* at(double x, double z, double& step, double* height = nullptr) const {
        for (const Ring& r : rings) {
            const double fi = (x - r.x0) / r.step, fj = (z - r.z0) / r.step;
            if (fi < 1 || fj < 1 || fi >= r.n - 2 || fj >= r.n - 2) continue;
            step = r.step;
            if (height) {
                const int i = int(fi), j = int(fj);
                const double u = fi - i, v = fj - j;
                auto hh = [&](int a, int b) { return r.s[size_t(b) * size_t(r.n) + size_t(a)].height; };
                *height = mix(mix(hh(i, j), hh(i + 1, j), u), mix(hh(i, j + 1), hh(i + 1, j + 1), u), v);
            }
            return &r.s[size_t(std::lround(fj)) * size_t(r.n) + size_t(std::lround(fi))];
        }
        return nullptr;
    }
};

void panorama(const Landform& land, const HeightRings& rings, const std::string& path, double heading,
              bool curvature, double eyeHeight) {
    const int w = 2400, h = 640;
    const double hfov = 90.0 * kPi / 180.0;
    const double focal = w * 0.5 / std::tan(hfov * 0.5);
    const double horizon = h * 0.70;
    const double ex = 0.0, ez = 0.0;
    double unused;
    const double eye = rings.at(ex, ez, unused)->height + eyeHeight;
    // Late afternoon: the sun low in the west-southwest, lighting the range's west face.
    const double sunAz = 250.0 * kPi / 180.0, sunEl = 22.0 * kPi / 180.0;   // azimuth from north, clockwise
    const double lx = std::sin(sunAz) * std::cos(sunEl), lz = std::cos(sunAz) * std::cos(sunEl),
                 ly = std::sin(sunEl);
    const Rgb haze{0.66, 0.75, 0.90};
    const double extinction = 1.0 / 70000.0;   // ~270 km visual range
    (void)land;
    Image img(w, h);
    parallelFor(w, [&](int col) {
        const double a = heading + std::atan((col + 0.5 - w * 0.5) / focal);
        const double dx = std::sin(a), dz = std::cos(a);
        // Rays per column are perspective-correct only at the centre row; fine for judging a skyline.
        const double colScale = std::cos(a - heading);
        int yMax = h;
        double d = 3.0;
        while (d < 150000.0 && yMax > 0) {
            const double x = ex + dx * d, z = ez + dz * d;
            double step = 1.0;
            double height = 0.0;
            const Sample* s = rings.at(x, z, step, &height);
            if (!s) break;
            double rel = height - eye;
            if (curvature) rel -= d * d / (2.0 * 6371000.0) * 0.87;
            const double y = horizon - rel / (d * colScale) * focal;
            const int yi = int(std::ceil(y));
            if (yi < yMax) {
                const double lit = 0.32 + 0.8 * shade(*s, lx, ly, lz);
                Rgb c = surfaceColour(*s, x, z, step) * lit;
                const double t = std::exp(-d * extinction);
                c = c * t + haze * (1.0 - t);
                for (int r = std::max(yi, 0); r < yMax; ++r) img.set(col, r, c);
                yMax = std::max(yi, 0);
            }
            d += std::max(0.5, step * 0.7);
        }
        for (int r = 0; r < yMax; ++r) {
            const double up = clamp01((horizon - r) / horizon);
            img.set(col, r, lerp(Rgb{0.78, 0.84, 0.92}, Rgb{0.24, 0.43, 0.78}, std::pow(up, 0.6)));
        }
    });
    img.save(path);
}

// ---- The grid sampler against the point sampler ----

void check(const Landform& land) {
    // A ring and the next coarser one over the same ground: where their samples coincide, they may
    // differ only by what the coarser leaves out (octaves and detail finer than its voxels).
    struct Case { const char* name; double x, z, step; };
    const Case cases[] = {{"0.25 m vs 0.5 m, foothills", 9000, 2000, 0.25},
                          {"0.25 m vs 0.5 m, crest", 15000, 15000, 0.25},
                          {"2 m vs 4 m, Plaza", -60, -40, 2.0},
                          {"16 m vs 32 m, cirques", 14000, 9000, 16.0},
                          {"16 m vs 32 m, gorge", -29000, 6000, 16.0},
                          {"128 m vs 256 m", -40000, 0, 128.0}};
    std::printf("%-30s %9s %9s %12s %12s\n", "64^2 vs 32^2", "max |dh|", "rms dh", "fine us", "coarse us");
    for (const Case& c : cases) {
        std::vector<Sample> fine, coarse;
        const auto t0 = std::chrono::steady_clock::now();
        land.sampleGrid(c.x, c.z, c.step, 64, 64, c.step, fine);
        const auto t1 = std::chrono::steady_clock::now();
        land.sampleGrid(c.x, c.z, c.step * 2.0, 32, 32, c.step * 2.0, coarse);
        const auto t2 = std::chrono::steady_clock::now();
        double maxd = 0, sum2 = 0;
        for (int j = 0; j < 32; ++j)
            for (int i = 0; i < 32; ++i) {
                const double d = coarse[size_t(j * 32 + i)].height - fine[size_t(j * 2 * 64 + i * 2)].height;
                maxd = std::max(maxd, std::abs(d));
                sum2 += d * d;
            }
        std::printf("%-30s %9.3f %9.3f %12.2f %12.2f\n", c.name, maxd, std::sqrt(sum2 / 1024.0),
                    std::chrono::duration<double, std::micro>(t1 - t0).count() / 4096.0,
                    std::chrono::duration<double, std::micro>(t2 - t1).count() / 1024.0);
    }
}

// ---- Reference points ----

void report(const Landform& land) {
    struct Ref { const char* name; double x, z, target; bool peak; };
    const Ref refs[] = {
        {"Plaza", 0, 0, 2133, false},           {"Atalaya", 6, 0.5, 2781, true},
        {"Tesuque Peak", 15.5, 6, 3671, true},  {"Lake Peak", 13.5, 13.5, 3782, true},
        {"Santa Fe Baldy", 14.5, 17, 3850, true}, {"Redondo Peak", -56.5, 21, 3431, true},
        {"Sandia Crest", -30, -62, 3255, true}, {"Ortiz", -3, -38, 2712, true},
        {"Caja del Rio", -17, -7, 2277, true},  {"Rio Grande, Cochiti", -38, -18, 1620, false},
    };
    std::printf("%-22s %9s %9s %9s\n", "reference", "target", "got", "at (km)");
    for (const Ref& r : refs) {
        double best = -1e9, bx = r.x, bz = r.z;
        if (r.peak) {
            // The highest point within 2.5 km (the generator places its own peaks).
            std::vector<Sample> g;
            land.sampleGrid(r.x * 1000.0 - 2500.0, r.z * 1000.0 - 2500.0, 50.0, 101, 101, 50.0, g);
            for (int j = 0; j < 101; ++j)
                for (int i = 0; i < 101; ++i)
                    if (g[size_t(j * 101 + i)].height > best) {
                        best = g[size_t(j * 101 + i)].height;
                        bx = r.x - 2.5 + i * 0.05;
                        bz = r.z - 2.5 + j * 0.05;
                    }
        } else {
            best = land.height(r.x * 1000.0, r.z * 1000.0, 5.0);
        }
        std::printf("%-22s %9.0f %9.0f   %5.1f,%5.1f\n", r.name, r.target, best, bx, bz);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Settings settings;
    std::string out = "lab", only;
    double atX = 0.0, atZ = 0.0;
    bool curvature = false;
    double eyeHeight = 30.0;   // a rooftop: over the nearest rise
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc) settings.seed = uint32_t(std::stoul(argv[++i]));
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--at" && i + 1 < argc) std::sscanf(argv[++i], "%lf,%lf", &atX, &atZ);
        else if (a == "--curvature") curvature = true;
        else if (a == "--check") { check(Landform(settings)); return 0; }
        else if (a == "--eye" && i + 1 < argc) eyeHeight = std::stod(argv[++i]);
        else {
            std::fprintf(stderr, "usage: mountains_lab [--seed N] [--out DIR] [--only region|local|profile|pano] "
                                 "[--at X,Z (km)] [--curvature]\n");
            return 1;
        }
    }
    std::filesystem::create_directories(out);
    const Landform land(settings);
    auto timed = [](const char* what, const std::function<void()>& fn) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%-12s %6.2f s\n", what, s);
    };
    if (only.empty() || only == "region")
        timed("region", [&] { map(land, out + "/region.png", atX * 1000.0, atZ * 1000.0, 160000.0, 1600, true); });
    if (only.empty() || only == "local")
        timed("local", [&] { map(land, out + "/local.png", 7000.0 + atX * 1000.0, 8000.0 + atZ * 1000.0, 24000.0, 2000, true); });
    if (only.empty() || only == "profile") timed("profile", [&] { profile(land, out + "/profile.png"); });
    if (only.empty() || only == "pano") {
        std::unique_ptr<HeightRings> rings;
        timed("pano rings", [&] { rings = std::make_unique<HeightRings>(land, 0.0, 0.0, 10); });
        timed("pano east", [&] { panorama(land, *rings, out + "/pano_east.png", 75.0 * kPi / 180.0, curvature, eyeHeight); });
        timed("pano west", [&] { panorama(land, *rings, out + "/pano_west.png", 255.0 * kPi / 180.0, curvature, eyeHeight); });
    }
    report(land);
    return 0;
}
