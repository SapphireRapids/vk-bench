// bench.h -- shared types for the benchmark groups.
#pragma once

#include "vk_ctx.h"
#include "spv_embedded.h"

#include <string>
#include <vector>

struct Options {
    bool quick = false;
    bool full = false;
    bool noAlu = false, noMatrix = false, noCache = false, noMem = false;
    int device = 0;
    int reps = 0;              // 0 = profile default (quick 3 / normal 5 / full 7)
    int cacheReps = 0;         // reps for the long cache curves
    double targetMs = 15.0;    // target GPU time per timed dispatch
    uint64_t maxBufferMB = 0;  // 0 = auto (3/4 of device-local heap, capped)
    std::string outDir;
    bool open = true;          // open the HTML report in the default browser when done
    bool noHtml = false;
    bool listOnly = false, capsOnly = false;
};

// One measurement point (a whole series is a list of these, sharing `test`).
struct Measure {
    std::string group;      // "alu" | "matrix" | "cache" | "memory"
    std::string test;       // test key, e.g. "fma_f32_vec4"
    std::string label;      // human-readable note (Chinese ok, UTF-8)
    std::string metric;     // "GFLOPS" | "GOPS" | "GB/s" | "ns" | "TFLOPS" ...
    double value = 0;       // metric value
    double ns = 0, nsMin = 0, nsMax = 0;
    double x = 0;           // sweep coordinate
    std::string xLabel;     // "footprint" | "stride" | "workgroups" | "acc" | ""
    std::string verify;     // "" | "ok" | "FAIL: reason"
    bool hostTimed = false; // true when the driver timestamp was unusable and the host clock was used
    double extra = 0;       // secondary value (e.g. accesses/s)
};

// Everything the report needs about the run.
struct BenchCtx {
    Gpu* gpu = nullptr;
    Options opt;
    std::vector<Measure> items;
    Buffer work;     // large device-local scratch
    Buffer work2;    // second scratch (copy destination)
    Buffer res;      // result / reduction output
    uint32_t fullGrid = 4096;   // workgroups that comfortably fill the device
    double chaseMaxFoot = 0;    // largest chase footprint actually measured (bytes, for the report)
    uint32_t maxGroups = 1;
    bool quiet = false;

    void add(Measure m) { items.push_back(std::move(m)); }
    Measure* find(const std::string& test, size_t series = 0);
};

// ---------------------------------------------------------------- helpers

// Compiles one embedded shader into a pipeline with spec constant 0 = spec0.
VkPipeline makePipe(BenchCtx& c, const std::string& spvName, uint32_t spec0, std::string* err);

// Compiles one embedded shader with no specialization constants at all.
VkPipeline makePipePlain(BenchCtx& c, const std::string& spvName, std::string* err);

// Runs a shader, calibrating spec constant 0 so the dispatch takes ~targetMs, then times it.
// Returns false if the pipeline could not even be created.
bool measureSpec(BenchCtx& c, const std::string& spvName, const void* pc, uint32_t pcSize, uint32_t gx,
                 uint32_t gy, uint32_t gz, int reps, uint32_t* outSpec, Timing* outTime,
                 std::string* err);

double medianOf(std::vector<double>& v);

// GFLOPS from flop count and nanoseconds.
inline double gflopsFrom(uint64_t flops, double ns) { return ns > 0 ? double(flops) / ns : 0.0; }
inline double gbytesFrom(uint64_t bytes, double ns) { return ns > 0 ? double(bytes) / ns : 0.0; }

// Sattolo single-cycle permutation (for the pointer chase). p must be pre-sized to N.
void sattoloPerm(std::vector<uint32_t>& p, uint64_t seed);

// CPU simulation of the pointer chase for verification.
uint32_t cpuChase(const std::vector<uint32_t>& next, uint32_t start, uint32_t iters);

// Human-readable byte size (Chinese units ok).
std::string sizeStr(double bytes);

// ---------------------------------------------------------------- groups

void benchAlu(BenchCtx& c);
void benchMatrix(BenchCtx& c);
void benchCache(BenchCtx& c);
void benchMem(BenchCtx& c);
void benchOverhead(BenchCtx& c);
