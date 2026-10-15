#include "runtime/fracture.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "core/log.h"

namespace projv::runtime {
    namespace {
        struct FracturePatterns {
            std::map<std::string, FracturePattern> byName;
        };

        core::vec3 centreOf(const FractureVoxel& v) { return core::vec3(v.position) + core::vec3(0.5f); }

        // The index of the voxel at `p` in voxels ordered z, y, x; -1 when there is none.
        bool zyxLess(core::ivec3 a, core::ivec3 b) {
            if (a.z != b.z) return a.z < b.z;
            if (a.y != b.y) return a.y < b.y;
            return a.x < b.x;
        }
        int64_t indexOf(const std::vector<FractureVoxel>& voxels, core::ivec3 p) {
            auto it = std::lower_bound(voxels.begin(), voxels.end(), p,
                                       [](const FractureVoxel& v, core::ivec3 q) { return zyxLess(v.position, q); });
            return it != voxels.end() && it->position == p ? int64_t(it - voxels.begin()) : -1;
        }

        const core::ivec3 FACES[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

        struct Seed {
            core::vec3 at;
            int32_t    shard;   // -1: what is nearest it stays
        };

        // Seeds scattered among the breakable voxels whose centres lie between `from` and `to` of
        // the strike (and that `keep` accepts): each voxel becomes one with the chance that gives a
        // cell of about `size(d)` voxels across -- 1 / size^3, over `elongation` for cells that long
        // -- scaled down so there are at most `cap`. A seed sits in its voxel at a random spot.
        template <typename Size, typename Keep>
        void scatter(const FractureInput& in, FractureRandom& random, float from, float to, float elongation, size_t cap,
                     bool breaking, Size size, Keep keep, std::vector<Seed>& seeds, int32_t& nextShard) {
            auto chance = [&](float d) {
                float s = std::max(size(d), 1.0f);
                return 1.0f / (s * s * s * elongation);
            };
            double expected = 0.0;
            for (const FractureVoxel& v : in.voxels) {
                float d = glm::distance(centreOf(v), in.at);
                if (v.breakable && d >= from && d <= to && keep(v)) expected += chance(d);
            }
            if (expected <= 0.0) return;
            const float scale = float(std::min(1.0, double(cap) / expected));
            for (const FractureVoxel& v : in.voxels) {
                float d = glm::distance(centreOf(v), in.at);
                if (!v.breakable || d < from || d > to || !keep(v)) continue;
                if (random.uniform() >= chance(d) * scale) continue;
                core::vec3 jitter(random.uniform(-0.5f, 0.5f), random.uniform(-0.5f, 0.5f), random.uniform(-0.5f, 0.5f));
                seeds.push_back({centreOf(v) + jitter, breaking ? nextShard++ : -1});
            }
        }

        // Each breakable voxel within `outer` takes its nearest seed's shard; distance along the
        // grain counts 1/`elongation` as much as across it.
        std::vector<int32_t> nearest(const FractureInput& in, const std::vector<Seed>& seeds, float elongation) {
            std::vector<int32_t> out(in.voxels.size(), -1);
            if (seeds.empty()) return out;
            const float squash = 1.0f / (elongation * elongation) - 1.0f;
            for (size_t i = 0; i < in.voxels.size(); i++) {
                const FractureVoxel& v = in.voxels[i];
                core::vec3 c = centreOf(v);
                if (!v.breakable || glm::distance(c, in.at) > in.outer) continue;
                float best = 1e30f;
                int32_t shard = -1;
                for (const Seed& s : seeds) {
                    core::vec3 d = c - s.at;
                    float along = glm::dot(d, in.grain);
                    float metric = glm::dot(d, d) + squash * along * along;
                    if (metric < best) { best = metric; shard = s.shard; }
                }
                out[i] = shard;
            }
            return out;
        }

        // The breakable voxel nearest the strike, within the reach: the seed a strike too small for
        // any other still breaks.
        void atLeastOne(const FractureInput& in, std::vector<Seed>& seeds, int32_t& nextShard) {
            for (const Seed& s : seeds)
                if (s.shard >= 0) return;
            int64_t best = -1;
            float bestDistance = in.reach;
            for (size_t i = 0; i < in.voxels.size(); i++) {
                float d = glm::distance(centreOf(in.voxels[i]), in.at);
                if (in.voxels[i].breakable && d <= bestDistance) { bestDistance = d; best = int64_t(i); }
            }
            if (best >= 0) seeds.push_back({centreOf(in.voxels[size_t(best)]), nextShard++});
        }

        constexpr size_t MAX_SEEDS = 256;

        std::vector<int32_t> voronoi(const FractureInput& in, FractureRandom& random, float elongation) {
            std::vector<Seed> seeds;
            int32_t next = 0;
            const float reach = std::max(in.reach, 1e-3f);
            auto any = [](const FractureVoxel&) { return true; };
            // Small at the strike, shardSize at the reach.
            scatter(in, random, 0.0f, in.reach, elongation, MAX_SEEDS, true,
                    [&](float d) { return in.shardSize * (0.35f + 0.65f * std::min(d / reach, 1.0f)); }, any, seeds, next);
            atLeastOne(in, seeds, next);
            // What stays: seeds of shardSize beyond the reach.
            scatter(in, random, in.reach, in.outer, elongation, MAX_SEEDS, false, [&](float) { return in.shardSize; }, any,
                    seeds, next);
            return nearest(in, seeds, elongation);
        }
    }

    bool registerFracturePattern(World& world, const std::string& name, FracturePattern pattern) {
        if (name.empty() || name.size() > 64 || !pattern) {
            core::warn("physics: refused fracture pattern '{}': a name of 1 to 64 characters and a function", name);
            return false;
        }
        FracturePatterns* patterns = world.ctx().find<FracturePatterns>();
        if (!patterns) patterns = &world.ctx().emplace<FracturePatterns>();
        patterns->byName[name] = std::move(pattern);
        return true;
    }

    const FracturePattern* fracturePattern(const World& world, const std::string& name) {
        const FracturePatterns* patterns = world.ctx().find<FracturePatterns>();
        if (!patterns) return nullptr;
        auto found = patterns->byName.find(name);
        return found != patterns->byName.end() ? &found->second : nullptr;
    }

    namespace fracture_patterns {
        std::vector<int32_t> shatter(const FractureInput& in, FractureRandom& random) { return voronoi(in, random, 1.0f); }

        std::vector<int32_t> splinter(const FractureInput& in, FractureRandom& random) { return voronoi(in, random, 3.0f); }

        std::vector<int32_t> crumble(const FractureInput& in, FractureRandom& random) {
            std::vector<Seed> seeds;
            int32_t next = 0;
            auto any = [](const FractureVoxel&) { return true; };
            scatter(in, random, 0.0f, in.reach + in.shardSize * 0.5f, 1.0f, MAX_SEEDS, true,
                    [&](float) { return in.shardSize; }, any, seeds, next);
            atLeastOne(in, seeds, next);
            std::vector<int32_t> out = nearest(in, seeds, 1.0f);
            for (size_t i = 0; i < out.size(); i++)
                if (glm::distance(centreOf(in.voxels[i]), in.at) > in.reach) out[i] = -1;
            return out;
        }

        std::vector<int32_t> seams(const FractureInput& in, FractureRandom& random) {
            const std::vector<FractureVoxel>& voxels = in.voxels;
            std::vector<int32_t> out(voxels.size(), -1);
            // Runs: face-connected voxels of one palette entry.
            std::vector<int32_t> run(voxels.size(), -1);
            std::vector<std::vector<size_t>> runs;
            for (size_t i = 0; i < voxels.size(); i++) {
                if (!voxels[i].breakable || run[i] >= 0) continue;
                int32_t id = int32_t(runs.size());
                runs.emplace_back();
                std::vector<size_t> stack{i};
                run[i] = id;
                while (!stack.empty()) {
                    size_t at = stack.back();
                    stack.pop_back();
                    runs.back().push_back(at);
                    for (core::ivec3 f : FACES) {
                        int64_t n = indexOf(voxels, voxels[at].position + f);
                        if (n < 0 || run[size_t(n)] >= 0 || !voxels[size_t(n)].breakable ||
                            voxels[size_t(n)].slot != voxels[at].slot)
                            continue;
                        run[size_t(n)] = id;
                        stack.push_back(size_t(n));
                    }
                }
            }
            // A run the strike reaches comes away; one much bigger than a shard is crumbled.
            int32_t next = 0;
            const float big = 2.0f * in.shardSize * in.shardSize * in.shardSize;
            for (std::vector<size_t>& members : runs) {
                std::sort(members.begin(), members.end());
                float closest = 1e30f;
                for (size_t i : members) closest = std::min(closest, glm::distance(centreOf(voxels[i]), in.at));
                if (closest > in.reach) continue;
                if (float(members.size()) <= big) {
                    for (size_t i : members) out[i] = next;
                    next++;
                    continue;
                }
                std::vector<Seed> seeds;
                for (size_t i : members) {
                    float s = std::max(in.shardSize, 1.0f);
                    if (random.uniform() >= 1.0f / (s * s * s)) continue;
                    seeds.push_back({centreOf(voxels[i]), next++});
                    if (seeds.size() >= MAX_SEEDS) break;
                }
                if (seeds.empty()) seeds.push_back({centreOf(voxels[members.front()]), next++});
                for (size_t i : members) {
                    float best = 1e30f;
                    for (const Seed& s : seeds) {
                        float d = glm::distance(centreOf(voxels[i]), s.at);
                        if (d < best) { best = d; out[i] = s.shard; }
                    }
                }
            }
            return out;
        }
    }

    std::vector<std::vector<size_t>> fractureShards(const FractureInput& in, const std::vector<int32_t>& shardOf,
                                                    int maxExtent, size_t maxShards) {
        std::vector<std::vector<size_t>> shards;
        const std::vector<FractureVoxel>& voxels = in.voxels;
        if (shardOf.size() != voxels.size()) return shards;
        std::map<int32_t, std::vector<size_t>> groups;
        for (size_t i = 0; i < voxels.size(); i++)
            if (shardOf[i] >= 0 && voxels[i].breakable) groups[shardOf[i]].push_back(i);

        // A group is final once it fits and is one face-connected piece.
        std::vector<int32_t> mark(voxels.size(), -1);
        int32_t stamp = 0;
        std::vector<std::vector<size_t>> pending;
        for (auto& [id, members] : groups) pending.push_back(std::move(members));
        std::reverse(pending.begin(), pending.end());   // popped in shard-number order
        while (!pending.empty()) {
            std::vector<size_t> members = std::move(pending.back());
            pending.pop_back();
            if (members.empty()) continue;
            core::ivec3 lo(std::numeric_limits<int>::max()), hi(std::numeric_limits<int>::min());
            for (size_t i : members) { lo = glm::min(lo, voxels[i].position); hi = glm::max(hi, voxels[i].position); }
            core::ivec3 extent = hi - lo + core::ivec3(1);
            if (extent.x > maxExtent || extent.y > maxExtent || extent.z > maxExtent) {
                int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
                int middle = lo[axis] + extent[axis] / 2;
                std::vector<size_t> a, b;
                for (size_t i : members) (voxels[i].position[axis] < middle ? a : b).push_back(i);
                pending.push_back(std::move(b));
                pending.push_back(std::move(a));
                continue;
            }
            // Its face-connected parts.
            const int32_t inGroup = stamp++;
            for (size_t i : members) mark[i] = inGroup;
            std::vector<std::vector<size_t>> parts;
            for (size_t start : members) {
                if (mark[start] != inGroup) continue;
                const int32_t taken = stamp++;
                parts.emplace_back();
                std::vector<size_t> stack{start};
                mark[start] = taken;
                while (!stack.empty()) {
                    size_t at = stack.back();
                    stack.pop_back();
                    parts.back().push_back(at);
                    for (core::ivec3 f : FACES) {
                        int64_t n = indexOf(voxels, voxels[at].position + f);
                        if (n < 0 || mark[size_t(n)] != inGroup) continue;
                        mark[size_t(n)] = taken;
                        stack.push_back(size_t(n));
                    }
                }
            }
            for (std::vector<size_t>& part : parts) {
                std::sort(part.begin(), part.end());
                shards.push_back(std::move(part));
            }
        }
        std::sort(shards.begin(), shards.end(), [](const auto& a, const auto& b) { return a.front() < b.front(); });
        if (shards.size() > maxShards) {
            core::warn("physics: a fracture made {} shards; the {} beyond {} stay where they are", shards.size(),
                       shards.size() - maxShards, maxShards);
            shards.resize(maxShards);
        }
        return shards;
    }
}
