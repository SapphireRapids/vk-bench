// bench_common.cpp -- pipeline helpers, calibration and small utilities.
#include "bench.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

Measure* BenchCtx::find(const std::string& test, size_t series) {
    size_t seen = 0;
    for (auto& m : items) {
        if (m.test == test) {
            if (seen == series) return &m;
            ++seen;
        }
    }
    return nullptr;
}

VkPipeline makePipe(BenchCtx& c, const std::string& spvName, uint32_t spec0, std::string* err) {
    const SpvEntry* e = spvFind(spvName.c_str());
    if (!e) {
        if (err) *err = "shader not embedded: " + spvName;
        return VK_NULL_HANDLE;
    }
    VkShaderModule mod = c.gpu->makeModule(e->data, e->sizeBytes, err);
    if (mod == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    VkSpecializationMapEntry me{};
    me.constantID = 0;
    me.offset = 0;
    me.size = sizeof(uint32_t);
    VkSpecializationInfo si{};
    si.mapEntryCount = 1;
    si.pMapEntries = &me;
    si.dataSize = sizeof(uint32_t);
    si.pData = &spec0;

    VkPipeline p = c.gpu->makePipeline(mod, &si, err);
    vkDestroyShaderModule(c.gpu->dev(), mod, nullptr);
    return p;
}

VkPipeline makePipePlain(BenchCtx& c, const std::string& spvName, std::string* err) {
    const SpvEntry* e = spvFind(spvName.c_str());
    if (!e) {
        if (err) *err = "shader not embedded: " + spvName;
        return VK_NULL_HANDLE;
    }
    VkShaderModule mod = c.gpu->makeModule(e->data, e->sizeBytes, err);
    if (mod == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkPipeline p = c.gpu->makePipeline(mod, nullptr, err);
    vkDestroyShaderModule(c.gpu->dev(), mod, nullptr);
    return p;
}

double medianOf(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// wall clock helper (host side)
static double qpcSec() {
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

bool measureSpec(BenchCtx& c, const std::string& spvName, const void* pc, uint32_t pcSize, uint32_t gx,
                 uint32_t gy, uint32_t gz, int reps, uint32_t* outSpec, Timing* outTime, std::string* err) {
    uint32_t spec = 64;
    VkPipeline pipe = makePipe(c, spvName, spec, err);
    if (pipe == VK_NULL_HANDLE) return false;

    VkDescriptorSet set = c.gpu->allocSet();
    Buffer bufsImg[4] = {c.work, c.work2, c.res, Buffer{}};
    c.gpu->bindBuffers(set, bufsImg, 3);

    // Calibration: scale the spec constant so one dispatch lands near targetMs.
    //  * the GPU timestamp is the primary signal;
    //  * the host wall clock per dispatch (~0.5 ms of submit/fence overhead) is only used when the
    //    GPU reports nothing at all, or when it is so large that the dispatch clearly ran away;
    //  * the step per pass is bounded to 16x and the iteration count to 1e6, so a mis-measurement
    //    can never grow a single dispatch into minutes.
    for (int pass = 0; pass < 3; ++pass) {
        std::string e;
        c.gpu->runOnce(pipe, set, pc, pcSize, gx, gy, gz, &e);   // force lazy driver compilation
        const int total = 2;   // warmup + rep
        double t0 = qpcSec();
        Timing t = c.gpu->runTimed(pipe, set, pc, pcSize, gx, gy, gz, 1, 1, &e);
        double hostMs = (qpcSec() - t0) * 1000.0 / total;
        double gpuMs = t.nsMedian / 1e6;
        double effMs;
        // runTimed already cross-checks the timestamp against the host clock; stay conservative
        // and never calibrate to less than 80% of the host wall clock per dispatch.
        if (gpuMs > 0) effMs = std::max(gpuMs, hostMs * 0.8);
        else effMs = std::max(hostMs, 0.05);
        if (effMs < 0.001) effMs = 0.001;
        double scaled = double(spec) * c.opt.targetMs / effMs;
        double lo = double(spec) / 16.0, hi = double(spec) * 16.0;
        scaled = std::min(std::max(scaled, lo), hi);
        uint32_t next = (uint32_t)std::min(1.0e6, std::max(1.0, std::floor(scaled)));
        if (next == spec) break;
        spec = next;
        vkDestroyPipeline(c.gpu->dev(), pipe, nullptr);
        pipe = makePipe(c, spvName, spec, err);
        if (pipe == VK_NULL_HANDLE) return false;
    }

    c.gpu->runOnce(pipe, set, pc, pcSize, gx, gy, gz, err);      // ditto before the final timing
    Timing t = c.gpu->runTimed(pipe, set, pc, pcSize, gx, gy, gz, reps, 1, err);
    vkDestroyPipeline(c.gpu->dev(), pipe, nullptr);
    if (outSpec) *outSpec = spec;
    if (outTime) *outTime = t;
    return true;
}

void sattoloPerm(std::vector<uint32_t>& p, uint64_t seed) {
    const uint32_t n = (uint32_t)p.size();
    for (uint32_t i = 0; i < n; ++i) p[i] = i;
    uint64_t s = seed * 6364136223846793005ull + 1442695040888963407ull;
    for (uint32_t i = n - 1; i > 0; --i) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t j = (uint32_t)((s >> 33) % i);   // 0 .. i-1  => single cycle
        std::swap(p[i], p[j]);
    }
}

uint32_t cpuChase(const std::vector<uint32_t>& next, uint32_t start, uint32_t iters) {
    uint32_t p = start;
    for (uint32_t i = 0; i < iters; ++i) p = next[p];
    return p;
}

std::string sizeStr(double bytes) {
    static const char* u[] = {"B", "KiB", "MiB", "GiB"};
    int i = 0;
    double v = bytes;
    while (v >= 1024.0 && i < 3) { v /= 1024.0; ++i; }
    char b[64];
    if (i == 0) std::snprintf(b, sizeof b, "%.0f %s", v, u[i]);
    else std::snprintf(b, sizeof b, "%.4g %s", v, u[i]);
    return std::string(b);
}
