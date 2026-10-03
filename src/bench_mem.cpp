// bench_mem.cpp -- group D: DRAM/cache bandwidth vs footprint, dispatch overhead.
#include "bench.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct PcBw { uint32_t nVec; uint32_t pad; };

int memReps(const BenchCtx& c) {
    if (c.opt.reps > 0) return c.opt.reps;
    if (c.opt.quick) return 2;
    if (c.opt.full) return 5;
    return 3;
}

std::vector<double> footprintList(double maxBytes, bool quick) {
    // 64 KiB upward: with a smaller footprint the streaming loop is too short to separate memory
    // throughput from loop control (the smallest sizes are covered by the chase latency curve).
    static const double kAll[] = {64e3,  128e3, 256e3, 512e3, 1e6,  2e6,   4e6,   8e6,
                                  16e6,  32e6,  48e6,  64e6,  96e6, 128e6, 192e6, 256e6,
                                  384e6, 512e6, 768e6, 1e9,   1.5e9, 2e9};
    std::vector<double> v;
    for (size_t i = 0; i < sizeof(kAll) / sizeof(kAll[0]); ++i) {
        if (kAll[i] > maxBytes) break;
        if (quick && i % 2 == 1) continue;
        v.push_back(kAll[i]);
    }
    return v;
}

void bwSweep(BenchCtx& c, const char* mode, const char* test, const char* label, double limitBytes) {
    auto list = footprintList(limitBytes, c.opt.quick);
    const int reps = memReps(c);
    std::string err;
    for (double foot : list) {
        uint64_t nVec = (uint64_t)(foot / 16.0);
        if (nVec < 64) continue;
        // Keep ~4 vec4 per thread per round so even small footprints use the whole machine.
        // Grid sizing matters more than it looks: with a grid-stride loop each workgroup keeps
        // re-reading its own slice, and if the slices of the resident workgroups fit in the last
        // level cache the repeat sweeps never reach DRAM (measured 3x above the memory spec on an
        // RX 6700 XT until this changed).  Above 16 MiB give every workgroup a >= 8 MiB slice so
        // the footprint cannot stay cached; below that the point is cache-resident by design.
        uint64_t groups;
        if (foot < (16.0 * 1024 * 1024)) {
            groups = (nVec + 256 * 4 - 1) / (256 * 4);
        } else {
            groups = (uint64_t)(foot / (8.0 * 1024 * 1024));
        }
        // at least 32 workgroups so the mid-size points are not parallelism-starved
        groups = std::max<uint64_t>(groups, 32);
        groups = std::max<uint64_t>(1, std::min<uint64_t>(groups, c.fullGrid));
        PcBw pc{(uint32_t)std::min<uint64_t>(nVec, 0xFFFFFFFFull), 0u};
        Timing t;
        uint32_t spec = 0;
        if (!measureSpec(c, mode, &pc, sizeof pc, (uint32_t)groups, 1, 1, reps, &spec, &t, &err)) {
            Measure m;
            m.group = "memory";
            m.test = test;
            m.label = label;
            m.metric = "GB/s";
            m.verify = "FAIL: " + err;
            c.add(m);
            break;
        }
        // bytes moved: read touches nVec, copy reads nVec/2 and writes nVec/2
        const uint64_t bytes = (uint64_t)pc.nVec * 16ull * spec;
        Measure m;
        m.group = "memory";
        m.test = test;
        m.label = label;
        m.metric = "GB/s";
        m.value = gbytesFrom(bytes, t.nsMedian);
        m.ns = t.nsMedian;
        m.hostTimed = t.hostFallback;
        m.nsMin = t.nsMin;
        m.nsMax = t.nsMax;
        m.x = foot;
        m.xLabel = "footprint";
        c.add(m);
        if (!c.quiet)
            std::printf("  %-14s %-9s %8.1f GB/s  (%u groups, iters %u, %.1f ms)\n", test, sizeStr(foot).c_str(),
                        m.value, (unsigned)groups, spec, t.nsMedian / 1e6);
    }
}

// Verifies the read kernel's address mapping: fill a small region with a known pattern and
// compare the reduction with a CPU sum.
void verifyRead(BenchCtx& c, const char* spv) {
    const uint32_t nVec = 4096;    // 64 KiB
    const uint32_t groups = 8;
    std::vector<float> data((size_t)nVec * 4);
    for (uint32_t i = 0; i < nVec * 4; ++i) data[i] = float(i % 7) * 0.25f;
    std::string err;
    if (!c.gpu->upload(c.work, data.data(), (VkDeviceSize)data.size() * 4, &err)) return;
    VkPipeline pipe = makePipe(c, spv, 1, &err);   // spec 0 = ITERS/ROUNDS = 1
    if (pipe == VK_NULL_HANDLE) return;
    PcBw pc{nVec, 0u};
    VkDescriptorSet set = c.gpu->allocSet();
    Buffer bufs[4] = {c.work, c.work2, c.res, Buffer{}};
    c.gpu->bindBuffers(set, bufs, 3);
    bool ran = c.gpu->runOnce(pipe, set, &pc, sizeof pc, groups, 1, 1, &err);
    std::vector<float> got(groups * 256, 0.0f);
    if (ran) c.gpu->download(c.work, got.data(), got.size() * sizeof(float), &err);
    vkDestroyPipeline(c.gpu->dev(), pipe, nullptr);
    int bad = 0;
    if (ran) {
        double total = 0;
        for (float v : data) total += v;
        double gotSum = 0;
        for (float v : got) gotSum += v;   // one round: each element read exactly once
        if (std::fabs(gotSum - total) / std::max(1.0, std::fabs(total)) > 1e-3) ++bad;
    }
    for (auto& m : c.items) {
        if (m.test == "bw_read_footprint") {
            m.verify = !ran ? "FAIL: 校验运行失败" : (bad == 0 ? "ok" : "FAIL: 归约和与 CPU 不符");
            break;
        }
    }
}

} // namespace

void benchOverhead(BenchCtx& c) {
    if (!c.quiet) std::printf("\n[E] \xe8\xb0\x83\xe5\xba\xa6\xe5\xbc\x80\xe9\x94\x80\n");
    std::string err;
    const SpvEntry* e = spvFind("empty");
    if (!e) return;
    VkShaderModule mod = c.gpu->makeModule(e->data, e->sizeBytes, &err);
    if (!mod) return;
    VkPipeline pipe = c.gpu->makePipeline(mod, nullptr, &err);
    vkDestroyShaderModule(c.gpu->dev(), mod, nullptr);
    if (!pipe) return;
    VkDescriptorSet set = c.gpu->allocSet();
    Buffer bufs[4] = {c.work, c.work2, c.res, Buffer{}};
    c.gpu->bindBuffers(set, bufs, 3);
    Timing t = c.gpu->runTimed(pipe, set, nullptr, 0, c.fullGrid, 1, 1, 5, 1, &err);
    vkDestroyPipeline(c.gpu->dev(), pipe, nullptr);
    Measure m;
    m.group = "memory";
    m.test = "dispatch_overhead";
    m.label = "empty kernel dispatch time";
    m.metric = "ns";
    m.value = t.nsMedian;
    m.ns = t.nsMedian;
    m.hostTimed = t.hostFallback;
    m.nsMin = t.nsMin;
    m.nsMax = t.nsMax;
    c.add(m);
    if (!c.quiet) std::printf("  empty kernel: %.2f us per dispatch\n", t.nsMedian / 1000.0);
}

// Independent cross-check of the timing chain: a plain vkCmdCopyBuffer through the transfer path,
// no compute shader involved.  A device-local copy moves 2x the bytes through DRAM (read + write),
// so on a 384 GB/s card this must land near the DRAM figure, not several times above it.
void dmaCopyBench(BenchCtx& c) {
    const uint64_t bytes = std::min<uint64_t>(c.work.size, c.work2.size);
    if (bytes < (64ull << 20)) return;
    std::string err;
    std::vector<double> deltas;
    for (int rep = 0; rep < 3; ++rep) {
        Timing t = c.gpu->runCopyTimed(c.work, c.work2, bytes, 1, 1, &err);
        if (t.nsMedian > 0) deltas.push_back(t.nsMedian);
    }
    if (deltas.empty()) return;
    std::sort(deltas.begin(), deltas.end());
    const double ns = deltas[deltas.size() / 2];
    Measure m;
    m.group = "memory";
    m.test = "dma_copy";
    m.label = "vkCmdCopyBuffer through the transfer engine";
    m.metric = "GB/s";
    m.value = double(bytes * 2) / ns;      // read + write
    m.ns = ns;
    m.x = (double)bytes;
    m.xLabel = "footprint";
    m.extra = double(bytes) / ns;          // one direction
    c.add(m);
    if (!c.quiet)
        std::printf("  dma_copy       %-9s %8.1f GB/s (R+W, %.1f ms, %.0f MiB)\n", sizeStr((double)bytes).c_str(),
                    m.value, ns / 1e6, bytes / 1048576.0);
}

void benchMem(BenchCtx& c) {
    if (!c.quiet) std::printf("\n[D] \xe5\xb8\xa6\xe5\xae\xbd\xef\xbc\x88\xe5\xae\xb9\xe9\x87\x8f\xe6\x89\xab\xe6\x8f\x8f\xef\xbc\x89\n");
    const double workLimit = (double)c.work.size;
    const double copyLimit = (double)c.work2.size;
    bwSweep(c, "bw_read", "bw_read_footprint", "streaming read (vec4 reduce)", workLimit);
    bwSweep(c, "bw_write", "bw_write_footprint", "streaming write (vec4 store)", workLimit);
    bwSweep(c, "bw_copy", "bw_copy_footprint", "copy (read+write)", copyLimit);
    dmaCopyBench(c);
    verifyRead(c, "bw_read");
}
