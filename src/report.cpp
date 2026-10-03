// report.cpp -- console report, JSON/CSV export, self-contained zh-CN HTML report
// and the derived architecture profile.
#include "report.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

namespace {

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", ch);
                    o += b;
                } else {
                    o += (char)ch;
                }
        }
    }
    return o;
}

std::string fmt(const char* f, double v) {
    char b[128];
    std::snprintf(b, sizeof b, f, v);
    return std::string(b);
}

const Measure* peak(const BenchCtx& c, const char* test) {
    const Measure* best = nullptr;
    for (const auto& m : c.items) {
        if (m.test == test && m.value > 0) {
            if (!best || m.value > best->value) best = &m;
        }
    }
    return best;
}

std::vector<const Measure*> series(const BenchCtx& c, const char* test) {
    std::vector<const Measure*> v;
    for (const auto& m : c.items)
        if (m.test == test) v.push_back(&m);
    std::sort(v.begin(), v.end(), [](const Measure* a, const Measure* b) { return a->x < b->x; });
    return v;
}

// "largest footprint that still stays within 1/threshFactor of the plateau best"
double plateauEnd(const std::vector<const Measure*>& pts, size_t from, double thresh, double* plateauBw) {
    double best = 0;
    for (size_t i = from; i < pts.size(); ++i) best = std::max(best, pts[i]->value);
    if (plateauBw) *plateauBw = best;
    double end = 0;
    for (size_t i = from; i < pts.size(); ++i) {
        if (pts[i]->value >= best / thresh) end = pts[i]->x;
        else break;
    }
    return end;
}

} // namespace

Profile buildProfile(const BenchCtx& c) {
    Profile p;
    auto val = [&](const char* t) -> double {
        const Measure* m = peak(c, t);
        return m ? m->value : 0;
    };
    p.fp32 = val("fma_f32_vec4");
    p.vec4 = val("fma_f32_vec4");
    p.vec1 = val("fma_f32_vec1");
    p.mul = val("mul_f32_vec4");
    p.add = val("add_f32_vec4");
    p.fmaDep = val("fma_f32_dep");
    p.fp16 = std::max(val("fma_f16_vec2"), val("fma_f16_vec4"));
    p.fp64 = val("fma_f64");
    p.int32 = val("imad_i32_vec4");
    p.dp4a = val("dp4a_i8");
    p.sfu = std::max({val("sfu_sin"), val("sfu_exp2"), val("sfu_log2"), val("sfu_rsqrt"), val("sfu_sqrt")});
    p.lds = val("lds_read");
    p.atomicScatter = val("atomic_scatter");
    p.dispatchOverheadNs = val("dispatch_overhead");

    // Matrix peaks per data type.
    // A dedicated matrix unit can plausibly sit a small multiple above the packed-fp16 ALU rate;
    // anything far above that is a timing artifact of the driver shim, not a measurement.
    const double aluCeiling = 2.0 * std::max(p.fp16, p.fp32);
    int matrixExcluded = 0, matrixNoisy = 0;
    // matrix measures are recorded in TFLOPS/TOPS; the profile keeps GFLOPS/GOPS everywhere
    for (const auto& m : c.items) {
        if (m.group != "matrix" || m.value <= 0) continue;
        const double g = m.value * 1000.0;
        if (aluCeiling > 0 && g > aluCeiling) { ++matrixExcluded; continue; }
        if (m.ns > 0 && 100.0 * (m.nsMax - m.nsMin) / m.ns > 30.0) ++matrixNoisy;
        if (m.test == "cm_khr_16x16x16_f16f16f32") p.matrixF16 = std::max(p.matrixF16, g);
        else if (m.test == "cm_khr_16x16x16_f16f16f16") p.matrixF16acc16 = std::max(p.matrixF16acc16, g);
        else if (m.test == "cm_khr_16x16x16_bf16bf16f32") p.matrixBf16 = std::max(p.matrixBf16, g);
        else if (m.test == "cm_khr_16x16x32_i8i8i32") p.matrixI8 = std::max(p.matrixI8, g);
        else if (m.test == "cm_khr_16x16x32_u8u8u32") p.matrixU8 = std::max(p.matrixU8, g);
        else if (m.test == "cm_khr_16x16x8_f32f32f32") p.matrixTf32 = std::max(p.matrixTf32, g);
        if (g > p.matrixBest) {
            p.matrixBest = g;
            p.matrixBestName = m.test;
        }
    }

    // Bandwidth peaks by footprint sweep.
    {
        auto rd = series(c, "bw_read_footprint");
        auto wr = series(c, "bw_write_footprint");
        auto cp = series(c, "bw_copy_footprint");
        for (const auto* m : rd) p.bwReadPeak = std::max(p.bwReadPeak, m->value);
        for (const auto* m : wr) p.bwWritePeak = std::max(p.bwWritePeak, m->value);
        for (const auto* m : cp) p.bwCopyPeak = std::max(p.bwCopyPeak, m->value);
        if (!rd.empty()) p.bwReadDram = rd.back()->value;
        if (!wr.empty()) p.bwWriteDram = wr.back()->value;
        if (!cp.empty()) p.bwCopyDram = cp.back()->value;

        // Fallback only: if the latency staircase below cannot be built (e.g. --no-cache or a
        // --no-cache-style run), derive cache levels from the bandwidth plateaus (successive
        // plateaus within 1/1.3 of the local best).  The latency curve is the sharper signal.
        if (rd.size() >= 6 && c.chaseMaxFoot <= 0) {
            size_t from = 0;
            const char* names[3] = {"L1", "L2", "L3 / Infinity Cache"};
            for (int lvl = 0; lvl < 3; ++lvl) {
                double bw = 0;
                double end = plateauEnd(rd, from, 1.30, &bw);
                if (end <= 0 || end >= rd.back()->x) break;
                CacheLevel cl;
                cl.name = names[lvl];
                cl.capacityBytes = end;
                cl.bwRead = bw;
                p.levels.push_back(cl);
                size_t i = from;
                while (i < rd.size() && rd[i]->x <= end) ++i;
                from = i;
                if (from >= rd.size()) break;
            }
        }
        if (!rd.empty()) p.dramBw = rd.back()->value;
    }

    // Cache hierarchy from the latency staircase.  The low-queue series (single workgroup, single
    // chain = 64 outstanding loads) is the closest to a true latency measurement; the 8-chain
    // curves are limited by outstanding-request capacity and level off too early.
    {
        auto lat = series(c, "chase_latency_low");
        std::string latTest = "chase_latency_low";
        if (lat.empty()) { lat = series(c, "chase_latency_1cu"); latTest = "chase_latency_1cu"; }
        if (lat.empty()) { lat = series(c, "chase_latency"); latTest = "chase_latency"; }
        if (lat.size() >= 4) {
            p.latencySeries = latTest;
            p.l1Latency = lat.front()->value;
            p.dramLatency = lat.back()->value;
            // A level boundary is a latency step of >=1.30x that never falls back to the previous
            // plateau (a lone spike from desktop interference must not create a level).
            struct Knee { double capBytes, latBelow, latAbove; };
            std::vector<Knee> knees;
            for (size_t i = 1; i < lat.size(); ++i) {
                const double before = lat[i - 1]->value, after = lat[i]->value;
                if (after < before * 1.30) continue;
                bool persists = true;
                for (size_t j = i + 1; j < lat.size(); ++j) {
                    if (lat[j]->value < before * 1.15) { persists = false; break; }
                }
                if (persists) knees.push_back({lat[i - 1]->x, before, after});
            }
            // A real hierarchy often shows more steps than there are named levels (this GPU has
            // four); keep the three *largest* steps -- the physically meaningful ones -- and let the
            // rest fold into the neighbouring plateau.  L1/L2/LLC sit near 1.5-1.8x, partition and
            // associativity artifacts sit near 1.3x.
            if (knees.size() > 3) {
                std::sort(knees.begin(), knees.end(), [](const Knee& a, const Knee& b) {
                    return (a.latAbove / a.latBelow) > (b.latAbove / b.latBelow);
                });
                knees.resize(3);
            }
            std::sort(knees.begin(), knees.end(),
                      [](const Knee& a, const Knee& b) { return a.capBytes < b.capBytes; });
            const char* names[3] = {"L1", "L2", "L3 / Infinity Cache"};
            if (c.chaseMaxFoot > 0) p.levels.clear();   // the latency curve is the authority
            for (size_t i = 0; i < knees.size(); ++i) {
                CacheLevel cl;
                cl.name = names[i];
                cl.capacityBytes = knees[i].capBytes;
                cl.latencyNs = knees[i].latBelow;
                p.levels.push_back(cl);
            }
            if (knees.size() >= 1) { p.l1Latency = knees[0].latBelow; p.l2Latency = knees[0].latAbove; }
            if (knees.size() >= 2) p.l2Latency = knees[1].latBelow;
            CacheLevel dr;
            dr.name = "\u663e\u5b58";
            dr.capacityBytes = lat.back()->x;
            dr.latencyNs = lat.back()->value;
            p.levels.push_back(dr);
            // read bandwidth measured at each level's capacity (nearest sweep point)
            auto rdPts = series(c, "bw_read_footprint");
            for (auto& l : p.levels) {
                double bd = 1e30;
                for (const auto* m : rdPts) {
                    const double d = std::fabs(std::log2(m->x) - std::log2(std::max(1.0, l.capacityBytes)));
                    if (d < bd) { bd = d; l.bwRead = m->value; }
                }
            }
        }
    }

    // Stride scan: the two drops that reveal sector / cacheline fill sizes.
    {
        auto st = series(c, "bw_stride");
        if (st.size() >= 4) {
            double peakBw = 0;
            for (const auto* m : st) peakBw = std::max(peakBw, m->value);
            for (const auto* m : st) {
                if (m->x == 32 && m->value < peakBw * 0.6 && p.strideKnee32 == 0) p.strideKnee32 = 32;
                if (m->x == 128 && m->value < peakBw * 0.35 && p.strideKnee128 == 0) p.strideKnee128 = 128;
            }
        }
    }

    // Occupancy: smallest grid that reaches 95% of the best.
    {
        auto occ = series(c, "fma_f32_occupancy");
        if (!occ.empty()) {
            double best = 0;
            for (const auto* m : occ) best = std::max(best, m->value);
            for (const auto* m : occ) {
                if (m->value >= best * 0.95) { p.occSaturation = (int)m->x; break; }
            }
        }
    }

    p.l1Bw = p.levels.size() > 0 ? p.levels[0].bwRead : 0;
    p.l2Bw = p.levels.size() > 1 ? p.levels[1].bwRead : 0;

    // ---- auto-generated conclusions
    if (p.fp32 > 0 && p.fp16 > 0) {
        double r = p.fp16 / p.fp32;
        if (r > 1.7) p.notes.push_back("fp16 打包运算约为 fp32 的 " + fmt("%.2f", r) + " 倍 → 硬件对半精度有成倍吞吐能力");
        else if (r > 1.2) p.notes.push_back("fp16 约为 fp32 的 " + fmt("%.2f", r) + " 倍");
        else p.notes.push_back("fp16 与 fp32 同速率（" + fmt("%.2f", r) + " 倍）→ 未见打包加速");
    }
    if (p.fp32 > 0 && p.fp64 > 0) {
        double r = p.fp32 / p.fp64;
        p.notes.push_back("fp64 为 fp32 的 1/" + fmt("%.1f", r) + " → " +
                          (r >= 4 ? std::string("双精度不是消费级速率") : std::string("双精度加速明显")));
    }
    if (matrixNoisy > 0)
        p.notes.push_back(std::to_string(matrixNoisy) +
                          " 个矩阵测点离散度 >30%：本机驱动的 cooperative matrix 实现耗时抖动很大，比值仅供参考");
    if (matrixExcluded > 0)
        p.notes.push_back(std::to_string(matrixExcluded) + " 个矩阵测点超过 fp16 ALU 上界（" +
                          fmt("%.1f", aluCeiling / 1000.0) +
                          " TFLOPS）已剔除：本机驱动的 cooperative matrix 实现计时不稳定");
    if (p.fp32 > 0 && p.matrixBest > 0) {
        double r = p.matrixBest / p.fp32;
        std::string s = "矩阵单元 : 向量单元 = " + fmt("%.2f", r) + " : 1（" + p.matrixBestName + "）";
        if (r >= 3.0) s += " → 存在独立矩阵硬件";
        else if (r >= 1.2) s += " → 矩阵吞吐约等于打包 FP16 向量 ALU，未见独立矩阵硬件";
        else s += " → 矩阵吞吐与向量 ALU 相当（cooperative matrix 由 ALU 路径实现）";
        p.notes.push_back(s);
    } else if (p.matrixBest == 0) {
        p.notes.push_back("未测到矩阵单元数据（扩展缺失或全部配置未通过校验）");
    }
    if (p.fp32 > 0 && p.vec1 > 0) {
        double r = p.vec4 / p.vec1;
        if (std::fabs(r - 1.0) < 0.15)
            p.notes.push_back("vec4 与标量 fp32 速率相同（" + fmt("%.3f", r) +
                              "）→ 指令是逐线程标量执行，向量宽度不改吞吐");
        else if (r > 1.5)
            p.notes.push_back("vec4 比标量快 " + fmt("%.2f", r) + " 倍 → 存在打包/双发射路径");
        else
            p.notes.push_back("vec4 : 标量 = " + fmt("%.3f", r));
    }
    if (p.fp32 > 0 && p.add > 0 && p.add / p.fp32 > 1.35)
        p.notes.push_back("ADD 速率是 FMA 的 " + fmt("%.2f", p.add / p.fp32) +
                          " 倍 → 加法走第二条发射路径（FMA 是单发射上界）");
    if (p.fp32 > 0 && p.int32 > 0)
        p.notes.push_back("INT32 IMAD : FP32 FMA = 1 : " + fmt("%.2f", p.fp32 / p.int32));
    if (p.fp32 > 0 && p.sfu > 0)
        p.notes.push_back("SFU : FP32 ALU = 1 : " + fmt("%.2f", p.fp32 / p.sfu));
    if (p.bwReadPeak > 0) {
        p.notes.push_back("峰值读带宽 " + fmt("%.0f", p.bwReadPeak) + " GB/s（含缓存内工作集）；"
                          "最大工作集（显存）读 " + fmt("%.0f", p.bwReadDram) + " / 写 " +
                          fmt("%.0f", p.bwWriteDram) + " / 拷贝 " + fmt("%.0f", p.bwCopyDram) + " GB/s");
        if (!p.levels.empty() && p.dramBw > 0) {
            std::string s = "缓存层级（延迟台阶推断）: ";
            for (size_t i = 0; i + 1 < p.levels.size(); ++i)
                s += p.levels[i].name + " \u2264" + sizeStr(p.levels[i].capacityBytes) + "\uff08" +
                     fmt("%.1f", p.levels[i].latencyNs) + " ns\uff09\u2192 ";
            s += p.levels.back().name + "\uff08" + fmt("%.0f", p.levels.back().latencyNs) + " ns\uff09";
            p.notes.push_back(s);
            for (const auto& l : p.levels) {
                if (l.name.find("Infinity") != std::string::npos && l.bwRead > 0)
                    p.notes.push_back("\u672b\u7ea7\u7f13\u5b58\u8bfb\u5e26\u5bbd\u662f\u663e\u5b58\u7684 " +
                                      fmt("%.1f", l.bwRead / p.dramBw) +
                                      " \u500d\u4e14\u5bb9\u91cf\u8fdc\u5927\u4e8e L2 \u2192 \u5927\u5bb9\u91cf\u672b\u7ea7\u7f13\u5b58\uff08\u5982 Infinity Cache/LLC\uff09\u5728\u8d77\u4f5c\u7528");
            }
        }
    }
    if (p.l1Latency > 0)
        p.notes.push_back("延迟: 最小 " + fmt("%.1f", p.l1Latency) + " ns，最大容量处 " +
                          fmt("%.0f", p.dramLatency) + " ns");
    if (p.occSaturation > 0)
        p.notes.push_back("约 " + std::to_string(p.occSaturation) + " 个 workgroup 即可达到 95% 峰值");
    if (p.fp32 > 0 && c.gpu->cap().amdCuCount > 0 && c.gpu->cap().props.vendorID == 0x1002) {
        // Derived, not measured: assumes the classic AMD layout of 128 flops/cycle per CU.
        double clock = p.fp32 / (double)c.gpu->cap().amdCuCount / 128.0;   // GFLOPS/(CU*flops/cycle) = GHz
        p.notes.push_back("每 CU FP32 约 " + fmt("%.1f", p.fp32 / c.gpu->cap().amdCuCount) +
                          " GFLOPS；按每 CU 128 flops/cycle 推算运行时钟 ≈ " + fmt("%.2f", clock) + " GHz（推算值）");
    }
    if (c.chaseMaxFoot > 0 && c.chaseMaxFoot < (1024.0 * 1024 * 1024))
        p.notes.push_back("\u8ffd\u8d76\u626b\u9891\u6700\u5927\u5230 " + sizeStr(c.chaseMaxFoot) +
                          "\uff08\u53d7\u5de5\u4f5c\u7f13\u51b2\u4e0a\u9650\u9650\u5236\uff0c\u5b8c\u6574\u8303\u56f4\u662f 1 KiB \u2192 1 GiB\uff09");
    if (p.dispatchOverheadNs > 0)
        p.notes.push_back("空内核调度开销 " + fmt("%.2f", p.dispatchOverheadNs / 1000.0) + " us/次");
    return p;
}

// sizeStr() lives in bench_common.cpp (declared in bench.h).

void printDeviceReport(const BenchCtx& c) {
    const Cap& cap = c.gpu->cap();
    auto vstr = [](uint32_t v) {
        char b[32];
        std::snprintf(b, sizeof b, "%u.%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
        return std::string(b);
    };
    std::printf("\n=== \xe8\xae\xbe\xe5\xa4\x87 ===\n");
    std::printf("  %s  [%s]\n", cap.props.deviceName,
                cap.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? "Discrete GPU"
                : cap.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "Integrated GPU"
                : cap.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? "CPU"
                                                                                 : "Other");
    std::printf("  apiVersion %s   driver 0x%08x (%s)   SPIR-V <= %s\n", vstr(cap.props.apiVersion).c_str(),
                cap.props.driverVersion, cap.driver.driverName[0] ? cap.driver.driverName : "?", cap.spvMax.c_str());
    std::printf("  compute queue family %u,  timestamp %s\n", cap.computeQueueFamily,
                c.gpu->hasTimestamps() ? "yes" : "NO (host timing fallback)");
    std::printf("  subgroup size %u,  supported ops 0x%x\n", cap.subgroup.subgroupSize,
                cap.subgroup.supportedOperations);
    std::printf("  memory: device-local %.2f GiB, host-visible %.2f GiB, working buffer cap %.2f GiB\n",
                cap.heapDeviceLocalBytes / 1073741824.0, cap.heapHostVisibleBytes / 1073741824.0,
                cap.maxBufferBytes / 1073741824.0);
    if (cap.amdCuCount) std::printf("  compute units (VK_AMD_shader_core_properties2): %u\n", cap.amdCuCount);
    std::printf("  limits: maxWG %u x %u x %u, maxWGInvocations %u, sharedMem %u B, timestampPeriod %.1f ns\n",
                cap.props.limits.maxComputeWorkGroupCount[0], cap.props.limits.maxComputeWorkGroupCount[1],
                cap.props.limits.maxComputeWorkGroupCount[2], cap.props.limits.maxComputeWorkGroupSize[0],
                cap.props.limits.maxComputeSharedMemorySize, cap.props.limits.timestampPeriod);
    std::string f;
    if (cap.f16) f += "fp16 ";
    if (cap.f64) f += "fp64 ";
    if (cap.storage16) f += "storage16 ";
    if (cap.storage8) f += "storage8 ";
    if (cap.dotProduct) f += "intDotProduct ";
    if (cap.sgExtendedTypes) f += "subgroupExtTypes ";
    if (cap.coopKHR) f += "coopMatrixKHR ";
    if (cap.coopNV) f += "coopMatrixNV ";
    std::printf("  enabled features: %s\n", f.empty() ? "(core only)" : f.c_str());
    if (!cap.coopKHRProps.empty()) {
        std::printf("  cooperative matrix configs advertised: %u\n", (unsigned)cap.coopKHRProps.size());
    }
    for (const auto& u : cap.notableUnsupported) std::printf("  ! %s\n", u.c_str());
}

void printSummary(const BenchCtx& c) {
    Profile p = buildProfile(c);
    std::printf("\n=== \xe7\xbb\x93\xe6\x9e\x9c\xe6\x91\x98\xe8\xa6\x81 ===\n");
    auto row = [](const char* name, double v, const char* unit) {
        if (v > 0) std::printf("  %-28s %12.2f %s\n", name, v, unit);
    };
    row("FP32 FMA (vec4)", p.fp32, "GFLOPS");
    row("FP32 FMA (scalar)", p.vec1, "GFLOPS");
    row("FP32 FMA (dep chain)", p.fmaDep, "GFLOPS");
    row("FP16 FMA (best)", p.fp16, "GFLOPS");
    row("FP64 FMA", p.fp64, "GFLOPS");
    row("INT32 IMAD", p.int32, "GOPS");
    row("INT8 DP4A (MAC)", p.dp4a, "GOPS");
    row("SFU (best)", p.sfu, "GOPS");
    row("shared memory (LDS)", p.lds, "GB/s");
    row("matrix fp16->fp32", p.matrixF16 / 1000.0, "TFLOPS");
    row("matrix fp16->fp16", p.matrixF16acc16 / 1000.0, "TFLOPS");
    row("matrix bf16->fp32", p.matrixBf16 / 1000.0, "TFLOPS");
    row("matrix int8->int32", p.matrixI8 / 1000.0, "TOPS");
    row("read bandwidth (peak)", p.bwReadPeak, "GB/s");
    row("write bandwidth (peak)", p.bwWritePeak, "GB/s");
    row("copy bandwidth (peak)", p.bwCopyPeak, "GB/s");
    std::printf("\n=== \xe6\x9e\xb6\xe6\x9e\x84\xe7\x94\xbb\xe5\x83\x8f ===\n");
    for (const auto& n : p.notes) std::printf("  - %s\n", n.c_str());
}

// ---------------------------------------------------------------- JSON

namespace {
struct StrBuf {
    std::string s;
    void addf(const char* f, ...) {
        char b[1024];
        va_list ap;
        va_start(ap, f);
        int n = std::vsnprintf(b, sizeof b, f, ap);
        va_end(ap);
        if (n > 0) s.append(b, (size_t)std::min<int>(n, (int)sizeof b - 1));
    }
};
} // namespace

std::string buildJson(const BenchCtx& c) {
    const Cap& cap = c.gpu->cap();
    StrBuf b;
    b.addf("{\n  \"tool\": \"vkbench\",\n");
    b.addf("  \"device\": {\n");
    b.addf("    \"name\": \"%s\",\n", jsonEscape(cap.props.deviceName).c_str());
    b.addf("    \"type\": %d,\n", (int)cap.props.deviceType);
    b.addf("    \"apiVersion\": \"%u.%u.%u\",\n", VK_VERSION_MAJOR(cap.props.apiVersion),
           VK_VERSION_MINOR(cap.props.apiVersion), VK_VERSION_PATCH(cap.props.apiVersion));
    b.addf("    \"driverVersion\": %u,\n", cap.props.driverVersion);
    b.addf("    \"driverName\": \"%s\",\n", jsonEscape(cap.driver.driverName).c_str());
    b.addf("    \"driverInfo\": \"%s\",\n", jsonEscape(cap.driver.driverInfo).c_str());
    b.addf("    \"vendorID\": %u, \"deviceID\": %u,\n", cap.props.vendorID, cap.props.deviceID);
    b.addf("    \"subgroupSize\": %u,\n", cap.subgroup.subgroupSize);
    b.addf("    \"subgroupOps\": %u,\n", cap.subgroup.supportedOperations);
    b.addf("    \"subgroupStages\": %u,\n", cap.subgroup.supportedStages);
    b.addf("    \"computeUnits\": %u,\n", cap.amdCuCount);
    b.addf("    \"heapDeviceLocal\": %llu,\n", (unsigned long long)cap.heapDeviceLocalBytes);
    b.addf("    \"heapHostVisible\": %llu,\n", (unsigned long long)cap.heapHostVisibleBytes);
    b.addf("    \"maxWorkingBuffer\": %llu,\n", (unsigned long long)c.work.size);
    b.addf("    \"maxComputeWorkGroupInvocations\": %u,\n", cap.props.limits.maxComputeWorkGroupInvocations);
    b.addf("    \"maxComputeSharedMemorySize\": %u,\n", cap.props.limits.maxComputeSharedMemorySize);
    b.addf("    \"timestampPeriod\": %.4f,\n", cap.props.limits.timestampPeriod);
    b.addf("    \"hasTimestamps\": %s,\n", c.gpu->hasTimestamps() ? "true" : "false");
    b.addf("    \"spvMax\": \"%s\",\n", jsonEscape(cap.spvMax).c_str());
    b.addf("    \"features\": { \"fp16\": %s, \"fp64\": %s, \"storage16\": %s, \"storage8\": %s, "
           "\"dotProduct\": %s, \"coopKHR\": %s, \"coopNV\": %s },\n",
           cap.f16 ? "true" : "false", cap.f64 ? "true" : "false", cap.storage16 ? "true" : "false",
           cap.storage8 ? "true" : "false", cap.dotProduct ? "true" : "false", cap.coopKHR ? "true" : "false",
           cap.coopNV ? "true" : "false");
    b.addf("    \"enabledExtensions\": [");
    for (size_t i = 0; i < cap.enabledExtensions.size(); ++i)
        b.addf("%s\"%s\"", i ? ", " : "", jsonEscape(cap.enabledExtensions[i]).c_str());
    b.addf("],\n    \"unsupported\": [");
    for (size_t i = 0; i < cap.notableUnsupported.size(); ++i)
        b.addf("%s\"%s\"", i ? ", " : "", jsonEscape(cap.notableUnsupported[i]).c_str());
    b.addf("]\n  },\n");

    Profile p = buildProfile(c);
    b.addf("  \"profile\": {\n");
    b.addf("    \"fp32\": %.3f, \"fp16\": %.3f, \"fp64\": %.3f, \"int32\": %.3f, \"dp4a\": %.3f,\n", p.fp32,
           p.fp16, p.fp64, p.int32, p.dp4a);
    b.addf("    \"vec1\": %.3f, \"vec4\": %.3f, \"fmaDep\": %.3f, \"mul\": %.3f, \"add\": %.3f,\n", p.vec1,
           p.vec4, p.fmaDep, p.mul, p.add);
    b.addf("    \"sfu\": %.3f, \"lds\": %.3f, \"atomicScatter\": %.3f,\n", p.sfu, p.lds, p.atomicScatter);
    b.addf("    \"matrixBest\": %.3f, \"matrixBestName\": \"%s\", \"matrixF16\": %.3f, \"matrixBf16\": %.3f, "
           "\"matrixI8\": %.3f, \"matrixTf32\": %.3f,\n",
           p.matrixBest, jsonEscape(p.matrixBestName).c_str(), p.matrixF16, p.matrixBf16, p.matrixI8, p.matrixTf32);
    b.addf("    \"bwRead\": %.3f, \"bwWrite\": %.3f, \"bwCopy\": %.3f,\n", p.bwReadPeak, p.bwWritePeak,
           p.bwCopyPeak);
    b.addf("    \"l1Bw\": %.3f, \"l2Bw\": %.3f, \"dramBw\": %.3f,\n", p.l1Bw, p.l2Bw, p.dramBw);
    b.addf("    \"bwReadDram\": %.3f, \"bwWriteDram\": %.3f, \"bwCopyDram\": %.3f,\n", p.bwReadDram,
           p.bwWriteDram, p.bwCopyDram);
    b.addf("    \"l1Latency\": %.3f, \"l2Latency\": %.3f, \"dramLatency\": %.3f,\n", p.l1Latency, p.l2Latency,
           p.dramLatency);
    b.addf("    \"occSaturation\": %d, \"dispatchOverheadNs\": %.2f,\n", p.occSaturation, p.dispatchOverheadNs);
    b.addf("    \"chaseMaxFoot\": %.0f, \"latencySeries\": \"%s\",\n", c.chaseMaxFoot,
           jsonEscape(p.latencySeries).c_str());
    b.addf("    \"levels\": [");
    for (size_t i = 0; i < p.levels.size(); ++i)
        b.addf("%s{\"name\":\"%s\",\"bytes\":%.0f,\"bw\":%.2f,\"latency\":%.2f}", i ? ", " : "",
               jsonEscape(p.levels[i].name).c_str(), p.levels[i].capacityBytes, p.levels[i].bwRead,
               p.levels[i].latencyNs);
    b.addf("],\n    \"notes\": [");
    for (size_t i = 0; i < p.notes.size(); ++i)
        b.addf("%s\"%s\"", i ? ", " : "", jsonEscape(p.notes[i]).c_str());
    b.addf("]\n  },\n");

    b.addf("  \"measures\": [\n");
    for (size_t i = 0; i < c.items.size(); ++i) {
        const Measure& m = c.items[i];
        b.addf("    {\"group\":\"%s\",\"test\":\"%s\",\"label\":\"%s\",\"metric\":\"%s\",\"value\":%.6g,"
               "\"ns\":%.4f,\"nsMin\":%.4f,\"nsMax\":%.4f,\"x\":%.6g,\"xLabel\":\"%s\",\"verify\":\"%s\","
               "\"extra\":%.6g}%s\n",
               jsonEscape(m.group).c_str(), jsonEscape(m.test).c_str(), jsonEscape(m.label).c_str(),
               jsonEscape(m.metric).c_str(), m.value, m.ns, m.nsMin, m.nsMax, m.x, jsonEscape(m.xLabel).c_str(),
               jsonEscape(m.verify).c_str(), m.extra, i + 1 < c.items.size() ? "," : "");
    }
    b.addf("  ]\n}\n");
    return b.s;
}

void writeJsonFile(const BenchCtx& c, const std::string& path) {
    std::string s = buildJson(c);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
}

void writeCsvFile(const BenchCtx& c, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "group,test,label,metric,value,ns,ns_min,ns_max,x,x_label,verify\n");
    for (const auto& m : c.items) {
        std::fprintf(f, "%s,%s,%s,%s,%.6g,%.2f,%.2f,%.2f,%.6g,%s,%s\n", m.group.c_str(), m.test.c_str(),
                     m.label.c_str(), m.metric.c_str(), m.value, m.ns, m.nsMin, m.nsMax, m.x, m.xLabel.c_str(),
                     m.verify.c_str());
    }
    std::fclose(f);
}
