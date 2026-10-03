// report.h -- console/JSON/CSV/HTML reporting and the architecture profile.
#pragma once

#include "bench.h"

#include <string>
#include <vector>

// Derived architecture picture: normalized ratios, cache levels, conclusions.
struct CacheLevel {
    std::string name;
    double capacityBytes = 0;
    double latencyNs = 0;    // from the chase curve, if available
    double bwRead = 0;       // GB/s at the plateau
};

struct Profile {
    double fp32 = 0, fp16 = 0, fp64 = 0, int32 = 0, dp4a = 0, sfu = 0, lds = 0;
    double atomicScatter = 0;
    double vec1 = 0, vec4 = 0, fmaDep = 0, mul = 0, add = 0;
    double matrixF16 = 0, matrixF16acc16 = 0, matrixBf16 = 0, matrixI8 = 0, matrixU8 = 0, matrixTf32 = 0;
    double matrixBest = 0;
    std::string matrixBestName;
    double bwReadPeak = 0, bwWritePeak = 0, bwCopyPeak = 0;      // best over all footprints
    double bwReadDram = 0, bwWriteDram = 0, bwCopyDram = 0;      // largest footprint = DRAM
    double l1Bw = 0, l2Bw = 0, dramBw = 0;
    double l1Latency = 0, l2Latency = 0, dramLatency = 0;
    std::vector<CacheLevel> levels;
    std::string latencySeries;   // which chase series the levels were derived from
    double strideKnee32 = 0, strideKnee128 = 0;   // inferred sector/line thresholds (bytes)
    int occSaturation = 0;                        // workgroups needed to reach 95% of peak
    double dispatchOverheadNs = 0;
    std::vector<std::string> notes;               // auto-generated conclusions
};

Profile buildProfile(const BenchCtx& c);

void printDeviceReport(const BenchCtx& c);
void printSummary(const BenchCtx& c);
std::string buildJson(const BenchCtx& c);
void writeJsonFile(const BenchCtx& c, const std::string& path);
void writeCsvFile(const BenchCtx& c, const std::string& path);
void writeHtmlFile(const BenchCtx& c, const std::string& path);
