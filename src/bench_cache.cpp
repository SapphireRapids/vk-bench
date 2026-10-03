// bench_cache.cpp -- group C: cache latency (pointer chase) and access-stride behavior.
#include "bench.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct PcChase { uint32_t count; uint32_t seed; };
struct PcStride { uint32_t n; uint32_t strideBytes; };

int cacheReps(const BenchCtx& c) {
    if (c.opt.cacheReps > 0) return c.opt.cacheReps;
    if (c.opt.quick) return 1;
    if (c.opt.full) return 5;
    return 3;
}

// 1 KiB .. 1 GiB as exact powers of two (2^10 .. 2^30): the latency staircase is easiest to read
// when the points land on cache-size-looking numbers.  quick keeps every second exponent.
std::vector<double> footprintList(double maxBytes) {
    std::vector<double> v;
    for (int e = 10; e <= 30; ++e) {
        const double bytes = std::ldexp(1.0, e);
        if (bytes > maxBytes) break;
        v.push_back(bytes);
    }
    return v;
}

// Replicates chase.comp's start positions and chain walk for verification.
void cpuChaseChains(uint32_t count, uint32_t seed, uint32_t gid, int chains, uint32_t iters,
                    const std::vector<uint32_t>& perm, uint32_t* out) {
    const uint32_t mult[8] = {2654435761u, 40503u,        2246822519u, 3266489917u,
                              668265263u,  374761393u,    1103515245u, 214013u};
    const uint32_t off[8] = {0u, 7u, 13u, 29u, 37u, 41u, 43u, 47u};
    uint32_t p[8];
    const int nchains = chains == 1 ? 1 : (chains == 2 ? 2 : (chains == 4 ? 4 : 8));
    for (int i = 0; i < nchains; ++i) {
        p[i] = (uint32_t)(((uint64_t)gid * mult[i] + (uint64_t)seed * off[i] + (i ? (uint32_t)(i * 2 + 1) : 0u)) %
                          count);
    }
    for (uint32_t it = 0; it < iters; ++it) {
        for (int i = 0; i < nchains; ++i) p[i] = perm[p[i]];
    }
    uint32_t r = p[0];
    for (int i = 1; i < nchains; ++i) r ^= p[i];
    *out = r;
}

// One chase sweep across footprints. chains: 1 or 8. groups: workgroups (=64 threads each).
void chaseSweep(BenchCtx& c, uint32_t chains, uint32_t groups, const char* test, const char* label) {
    const char* spv = chains == 1 ? "chase_c1" : "chase_c8";
    // The chase needs only one buffer (the permutation is re-uploaded per footprint), so the
    // sweep may use the whole working buffer; 1 GiB is the requested ceiling.
    double maxFoot = std::min<double>(1024.0 * 1024 * 1024, (double)c.work.size);
    auto list = footprintList(maxFoot);
    if (c.opt.quick) {
        std::vector<double> f;
        for (size_t i = 0; i < list.size(); ++i)
            if (i % 2 == 0 || i + 1 == list.size()) f.push_back(list[i]);
        list.swap(f);
    }
    if (!list.empty() && list.back() > c.chaseMaxFoot) c.chaseMaxFoot = list.back();
    const int reps = cacheReps(c);
    std::string err;
    uint32_t firstSpec = 0;
    std::vector<uint32_t> firstPerm;
    uint32_t firstCount = 0;

    size_t k = 0;
    for (double foot : list) {
        ++k;
        const uint32_t count = (uint32_t)(foot / 4.0);
        if (count < 64) continue;
        if (!c.quiet && foot >= (256.0 * 1024 * 1024))
            std::printf("    [%zu/%zu] %s: building + uploading permutation ...\n", k, list.size(),
                        sizeStr(foot).c_str());
        std::vector<uint32_t> perm(count);
        sattoloPerm(perm, 0x9E3779B97F4A7C15ull + count);
        // chase.comp uses next = perm: idx[i] = perm[i]
        if (!c.gpu->upload(c.work, perm.data(), (VkDeviceSize)count * 4, &err)) {
            if (!c.quiet) std::printf("  chase upload failed at %s: %s\n", sizeStr(foot).c_str(), err.c_str());
            break;
        }
        PcChase pc{count, 0u};
        Timing t;
        uint32_t spec = 0;
        if (!measureSpec(c, spv, &pc, sizeof pc, groups, 1, 1, reps, &spec, &t, &err)) {
            Measure m;
            m.group = "cache";
            m.test = test;
            m.label = label;
            m.metric = "ns";
            m.verify = "FAIL: " + err;
            c.add(m);
            break;
        }
        Measure m;
        m.group = "cache";
        m.test = test;
        m.label = label;
        m.metric = "ns";
        m.value = t.nsMedian / double(spec);   // ns per dependent load
        m.ns = t.nsMedian;
        m.hostTimed = t.hostFallback;
        m.nsMin = t.nsMin;
        m.nsMax = t.nsMax;
        m.x = foot;
        m.xLabel = "footprint";
        const double loads = double(spec) * double(chains) * double(groups) * 64.0;
        m.extra = loads / (t.nsMedian / 1e9);
        c.add(m);
        if (firstCount == 0) {
            firstCount = count;
            firstSpec = spec;
            firstPerm = perm;
        }
        if (!c.quiet)
            std::printf("  %-16s %-9s %7.1f ns/load  (%6.1f Mloads/s, %.1f ms)\n", test, sizeStr(foot).c_str(),
                        m.value, m.extra / 1e6, t.nsMedian / 1e6);
    }

    // ---- verification: replicate the chain walk on the CPU for the smallest footprint
    if (firstCount > 0 && groups == 1) {
        PcChase pc{firstCount, 0u};
        VkPipeline pipe = makePipe(c, spv, firstSpec, &err);
        if (pipe != VK_NULL_HANDLE) {
            VkDescriptorSet set = c.gpu->allocSet();
            Buffer bufs[4] = {c.work, c.work2, c.res, Buffer{}};
            c.gpu->bindBuffers(set, bufs, 3);
            bool ran = c.gpu->runOnce(pipe, set, &pc, sizeof pc, 1, 1, 1, &err);
            std::vector<float> got(64, 0.0f);
            if (ran) c.gpu->download(c.work, got.data(), 64 * sizeof(float), &err);
            vkDestroyPipeline(c.gpu->dev(), pipe, nullptr);
            int bad = 0;
            if (ran) {
                for (uint32_t i = 0; i < 64; ++i) {
                    uint32_t want = 0;
                    cpuChaseChains(firstCount, 0u, i, (int)chains, firstSpec, firstPerm, &want);
                    if ((uint32_t)got[i] != want) ++bad;
                }
            }
            for (auto& m : c.items) {
                if (m.test == test) {
                    m.verify = !ran ? "FAIL: 校验运行失败" : (bad == 0 ? "ok" : ("FAIL: " + std::to_string(bad) + "/64"));
                    break;
                }
            }
        }
    }
}

} // namespace

void benchCache(BenchCtx& c) {
    if (!c.quiet) std::printf("\n[C] \xe7\xbc\x93\xe5\xad\x98\xe5\xae\xb9\xe9\x87\x8f\xe4\xb8\x8e\xe5\xbb\xb6\xe8\xbf\x9f\n");

    // Latency curves: aggregate (many CUs) and single-CU (per-CU L1 view).
    // NOTE: the chase buffer shares c.work with the bandwidth sweep; contents are re-uploaded
    // for every footprint so both tests are independent.
    chaseSweep(c, 8, 64, "chase_latency", "pointer chase, aggregate, 8 chains/thread");
    chaseSweep(c, 8, 1, "chase_latency_1cu", "pointer chase, single workgroup (per-CU L1)");
    // 64 outstanding loads total: low enough that the memory system never queues, so this series
    // is the closest to a true latency measurement (the 8-chain curves are throughput-limited).
    chaseSweep(c, 1, 1, "chase_latency_low", "pointer chase, single workgroup, single chain");

    // ---- stride scan: useful bandwidth vs access stride
    if (!c.quiet) std::printf("  [D] stride \xe6\x89\xab\xe6\x8f\x8f\n");
    const uint32_t strides[] = {4, 8, 16, 32, 64, 128, 256, 512, 1024};
    const double maxFoot = (double)c.work.size * 0.5;
    const int reps = cacheReps(c);
    std::string err;
    PcStride pcLast{0, 0};
    uint32_t lastSpec = 0;
    for (uint32_t s : strides) {
        uint64_t n = std::min<uint64_t>(1u << 20, (uint64_t)(maxFoot / s));
        if (n < 1024) break;
        PcStride pc{(uint32_t)n, s};
        Timing t;
        uint32_t spec = 0;
        if (!measureSpec(c, "bw_stride", &pc, sizeof pc, c.fullGrid, 1, 1, reps, &spec, &t, &err)) break;
        const uint64_t bytes = (uint64_t)n * spec * 4;   // useful bytes
        Measure m;
        m.group = "cache";
        m.test = "bw_stride";
        m.label = "useful bandwidth vs stride";
        m.metric = "GB/s";
        m.value = gbytesFrom(bytes, t.nsMedian);
        m.ns = t.nsMedian;
        m.hostTimed = t.hostFallback;
        m.nsMin = t.nsMin;
        m.nsMax = t.nsMax;
        m.x = s;
        m.xLabel = "stride";
        m.extra = double(n) * spec / (t.nsMedian / 1e9);   // accesses/s
        c.add(m);
        pcLast = pc;
        lastSpec = spec;
        if (!c.quiet)
            std::printf("    stride %5u B: %8.1f GB/s 有效, %8.1f Maccess/s\n", s, m.value, m.extra / 1e6);
    }
    (void)pcLast;
    (void)lastSpec;
}
