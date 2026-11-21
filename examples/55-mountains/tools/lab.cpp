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

#include <bimg/bimg.h>
#include <bx/file.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace mountains;

namespace {

struct Rgb { double r = 0, g = 0, b = 0; };
Rgb operator*(Rgb a, double k) { return {a.r * k, a.g * k, a.b * k}; }
Rgb operator+(Rgb a, Rgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
Rgb lerp(Rgb a, Rgb b, double t) { return a * (1.0 - t) + b * t; }

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

// ---- Surface colour (a first pass at M4/M5's bands; sRGB-ish albedo) ----

const Rgb kSoil{0.72, 0.58, 0.45}, kGrama{0.72, 0.63, 0.46}, kPinyon{0.22, 0.28, 0.17},
    kPonderosa{0.19, 0.26, 0.15}, kFir{0.11, 0.18, 0.12}, kSpruce{0.09, 0.15, 0.12},
    kAspenBare{0.56, 0.53, 0.49}, kTundra{0.60, 0.55, 0.41}, kGranite{0.63, 0.50, 0.46},
    kGneiss{0.50, 0.48, 0.47}, kSediment{0.82, 0.67, 0.53}, kBasalt{0.26, 0.24, 0.23},
    kSnow{0.93, 0.95, 0.99}, kWater{0.22, 0.32, 0.40};

Rgb surfaceColour(const Sample& s, double x, double z, double footprint) {
    const double slope = std::sqrt(s.gx * s.gx + s.gz * s.gz);
    const double aspectW = smoothstep(0.03, 0.2, slope);
    const double northness = slope > 1e-6 ? -s.gz / slope * aspectW : 0.0;
    const double h = s.height;
    const double e = h + 170.0 * northness;   // north slopes live higher

    // Ground under the trees.
    Rgb ground = lerp(kSoil, kGrama, 0.5 + 0.5 * fbm(x / 900.0, z / 900.0, 2, 71));
    ground = lerp(ground, kSediment, s.badland * smoothstep(0.08, 0.3, slope));
    ground = lerp(ground, kTundra, smoothstep(3450.0, 3600.0, e));

    // Tree cover and which trees.
    double cover = 0.0;
    Rgb tree = kPinyon;
    if (e < 2300.0) {
        cover = 0.25 * smoothstep(1880.0, 2000.0, e);
        tree = kPinyon;
    } else if (e < 2600.0) {
        const double t = smoothstep(2300.0, 2600.0, e);
        cover = mix(0.25, 0.5, t);
        tree = lerp(kPinyon, kPonderosa, t);
    } else if (e < 3000.0) {
        const double t = smoothstep(2600.0, 3000.0, e);
        cover = mix(0.5, 0.75, t);
        tree = lerp(kPonderosa, kFir, smoothstep(2550.0, 2800.0, e));
    } else {
        cover = 0.78 * (1.0 - smoothstep(3450.0, 3600.0, e));
        tree = lerp(kFir, kSpruce, smoothstep(3000.0, 3200.0, e));
        const double aspen = smoothstep(0.25, 0.45, fbm(x / 1500.0, z / 1500.0, 3, 91)) *
                             (1.0 - smoothstep(3300.0, 3450.0, e));
        tree = lerp(tree, kAspenBare, aspen);
    }
    // South-facing low slopes are drier; patches and clearings everywhere.
    cover *= 1.0 - 0.35 * std::max(-northness, 0.0) * (1.0 - smoothstep(2500.0, 2900.0, e));
    cover *= clamp01(0.75 + 0.6 * fbm(x / 700.0, z / 700.0, 3, 93));
    cover *= 1.0 - smoothstep(0.7, 1.1, slope);
    cover *= 1.0 - s.basalt * 0.6;
    // At a fine footprint the open woodland is dots, not a tint.
    if (footprint < 6.0) {
        const double dots = fbm(x / 5.0, z / 5.0, 2, 95);
        cover = smoothstep(0.55, 0.6, cover + dots * 0.9 + 0.1);
    }
    Rgb c = lerp(ground, tree, cover);

    // Bare rock on steep ground and in cirques.
    Rgb rock = lerp(kGranite, kGneiss, 0.5 + 0.5 * fbm(x / 2500.0, z / 2500.0, 2, 97));
    rock = lerp(kSediment * 0.95, rock, s.core);
    rock = lerp(rock, kBasalt, s.basalt);
    const double rockiness = std::max(smoothstep(0.75, 1.1, slope), s.cirque * 0.6);
    c = lerp(c, rock, rockiness);
    c = lerp(c, kBasalt, s.basalt * smoothstep(0.35, 0.7, slope));   // dark scarp faces

    // Late November snow: low on north faces, only the top on south ones.
    const double snowline = 3450.0 - 250.0 * northness;
    double snow = smoothstep(snowline, snowline + 180.0, h) * (1.0 - smoothstep(1.0, 1.5, slope));
    snow *= 1.0 - 0.55 * cover;   // under trees, less of it shows from above
    c = lerp(c, kSnow, snow);

    if (s.river > 0.0) c = lerp(c, kWater, s.river);
    if (s.cirque > 0.9) c = lerp(c, kWater, 0.8);   // tarn
    return c;
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
    parallelFor(pixels, [&](int row) {
        for (int col = 0; col < pixels; ++col) {
            const double x = cx - size * 0.5 + (col + 0.5) * step;
            const double z = cz + size * 0.5 - (row + 0.5) * step;
            const Sample s = land.sample(x, z, step);
            const double lit = 0.3 + 0.85 * shade(s, -0.6 * l, 0.9 * l, 0.6 * l);
            img.set(col, row, surfaceColour(s, x, z, step) * lit);
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
        parallelFor(w, [&](int c) {
            e[size_t(c)] = land.height(x0 + (c + 0.5) * (x1 - x0) / w, line.z, (x1 - x0) / w);
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

void panorama(const Landform& land, const std::string& path, double heading, bool curvature, double eyeHeight) {
    const int w = 2400, h = 640;
    const double hfov = 90.0 * kPi / 180.0;
    const double focal = w * 0.5 / std::tan(hfov * 0.5);
    const double horizon = h * 0.70;
    const double ex = 0.0, ez = 0.0;
    const double eye = land.height(ex, ez, 1.0) + eyeHeight;
    // Late afternoon: the sun low in the west-southwest, lighting the range's west face.
    const double sunAz = 250.0 * kPi / 180.0, sunEl = 22.0 * kPi / 180.0;   // azimuth from north, clockwise
    const double lx = std::sin(sunAz) * std::cos(sunEl), lz = std::cos(sunAz) * std::cos(sunEl),
                 ly = std::sin(sunEl);
    const Rgb haze{0.66, 0.75, 0.90};
    const double extinction = 1.0 / 70000.0;   // ~270 km visual range
    Image img(w, h);
    parallelFor(w, [&](int col) {
        const double a = heading + std::atan((col + 0.5 - w * 0.5) / focal);
        const double dx = std::sin(a), dz = std::cos(a);
        // Rays per column are perspective-correct only at the centre row; fine for judging a skyline.
        const double colScale = std::cos(a - heading);
        int yMax = h;
        double d = 3.0;
        while (d < 150000.0 && yMax > 0) {
            const double step = std::max(1.5, d * 0.004);
            const double x = ex + dx * d, z = ez + dz * d;
            const Sample s = land.sample(x, z, std::max(0.5, d * 0.0012));
            double rel = s.height - eye;
            if (curvature) rel -= d * d / (2.0 * 6371000.0) * 0.87;
            const double y = horizon - rel / (d * colScale) * focal;
            const int yi = int(std::ceil(y));
            if (yi < yMax) {
                const double lit = 0.32 + 0.8 * shade(s, lx, ly, lz);
                Rgb c = surfaceColour(s, x, z, std::max(0.5, d * 0.0012)) * lit;
                const double t = std::exp(-d * extinction);
                c = c * t + haze * (1.0 - t);
                for (int r = std::max(yi, 0); r < yMax; ++r) img.set(col, r, c);
                yMax = std::max(yi, 0);
            }
            d += step;
        }
        for (int r = 0; r < yMax; ++r) {
            const double up = clamp01((horizon - r) / horizon);
            img.set(col, r, lerp(Rgb{0.78, 0.84, 0.92}, Rgb{0.24, 0.43, 0.78}, std::pow(up, 0.6)));
        }
    });
    img.save(path);
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
            for (int j = -25; j <= 25; ++j)
                for (int i = -25; i <= 25; ++i) {
                    const double x = r.x * 1000.0 + i * 100.0, z = r.z * 1000.0 + j * 100.0;
                    const double e = land.height(x, z, 50.0);
                    if (e > best) { best = e; bx = x / 1000.0; bz = z / 1000.0; }
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
        timed("pano east", [&] { panorama(land, out + "/pano_east.png", 75.0 * kPi / 180.0, curvature, eyeHeight); });
        timed("pano west", [&] { panorama(land, out + "/pano_west.png", 255.0 * kPi / 180.0, curvature, eyeHeight); });
    }
    report(land);
    return 0;
}
