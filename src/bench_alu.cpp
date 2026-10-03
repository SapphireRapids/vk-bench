// bench_alu.cpp -- group A: vector / ALU throughput, SFU, bit ops, subgroup ops,
// shared-memory bandwidth, plus ILP and occupancy scans.
//
// Note on the push constants: every arithmetic kernel receives *four distinct runtime scalars*
// per coefficient, so no lane-uniform (splat) vector exists that a compiler could collapse
// into one scalar recurrence.  See shaders/alu_f32.comp.
#include "bench.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kThreads128 = 128;
constexpr uint32_t kThreads64 = 64;

struct PcF32 { float laneB[4]; float laneC[4]; float laneM[4]; float laneD[4]; };
struct PcF16 { float laneB[4]; float laneC[4]; };
struct PcInt { uint32_t laneB[4]; uint32_t laneC[4]; };
struct PcF64 { double seed; };
struct PcU32 { uint32_t seed; };
struct PcU32x2 { uint32_t a; uint32_t b; };

PcF32 pcF32() {
    PcF32 p{};
    const float b[4] = {1.0000001f, 1.0000002f, 1.0000003f, 1.0000004f};
    const float c[4] = {0.0009765625f, 0.0009765626f, 0.0009765627f, 0.0009765628f};
    const float m[4] = {0.9999999f, 0.9999998f, 0.9999997f, 0.9999996f};
    for (int i = 0; i < 4; ++i) {
        p.laneB[i] = b[i];
        p.laneC[i] = c[i];
        p.laneM[i] = m[i];
        p.laneD[i] = c[i];
    }
    return p;
}

PcF16 pcF16() {
    PcF16 p{};
    const float b[4] = {1.0f, 1.0009765625f, 1.001953125f, 1.0029296875f};
    const float c[4] = {0.0009765625f, 0.001953125f, 0.0029296875f, 0.00390625f};
    for (int i = 0; i < 4; ++i) {
        p.laneB[i] = b[i];
        p.laneC[i] = c[i];
    }
    return p;
}

PcInt pcInt() {
    PcInt p{};
    // > 2^24 on purpose: forces the full 32-bit multiply path (V_MAD_U32_U24 would truncate).
    const uint32_t b[4] = {0x01000001u, 0x01000003u, 0x01000005u, 0x01000007u};
    const uint32_t c[4] = {1u, 3u, 5u, 7u};
    for (int i = 0; i < 4; ++i) {
        p.laneB[i] = b[i];
        p.laneC[i] = c[i];
    }
    return p;
}

int autoReps(const BenchCtx& c) {
    if (c.opt.reps > 0) return c.opt.reps;
    if (c.opt.quick) return 3;
    if (c.opt.full) return 7;
    return 5;
}

// Runs one throughput kernel and records a measurement point.
// unitsPerIterThread = useful units (flops / ops / macs / bytes) per iteration per thread.
void runCase(BenchCtx& c, const char* spv, const char* test, const char* label, const char* metric,
             double unitsPerIterThread, uint32_t grid, uint32_t threadsPerGroup, const void* pc,
             uint32_t pcSize, int reps) {
    Timing t;
    uint32_t spec = 0;
    std::string err;
    if (!measureSpec(c, spv, pc, pcSize, grid, 1, 1, reps, &spec, &t, &err)) {
        Measure m;
        m.group = "alu";
        m.test = test;
        m.label = label;
        m.metric = metric;
        m.verify = "FAIL: " + err;
        c.add(m);
        if (!c.quiet) std::printf("  [FAIL] %-22s %s\n", test, err.c_str());
        return;
    }
    const uint64_t threads = (uint64_t)grid * threadsPerGroup;
    const double units = unitsPerIterThread * double(spec) * double(threads);
    Measure m;
    m.group = "alu";
    m.test = test;
    m.label = label;
    m.metric = metric;
    m.value = units / t.nsMedian;
    m.ns = t.nsMedian;
    m.hostTimed = t.hostFallback;
    m.hostTimed = t.hostFallback;
    m.nsMin = t.nsMin;
    m.nsMax = t.nsMax;
    m.x = (double)spec;
    m.xLabel = "iters";
    c.add(m);
    if (!c.quiet)
        std::printf("  %-22s %10.2f %-7s  (%.2f ms, spread %.1f%%)\n", test, m.value, metric, t.nsMedian / 1e6,
                    t.spreadPct());
}

// ---------------------------------------------------------------- CPU models

// Per-lane replica of alu_f32.comp (same coefficients, same order; std::fma is fused).
double cpuAluF32(int op, int vec, int dep, uint32_t iters, uint32_t gid) {
    const PcF32 pcv = pcF32();
    const float init[8] = {1.0f, 0.75f, 0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f, 0.015625f};
    const uint32_t masks[8] = {3u, 7u, 15u, 31u, 63u, 127u, 255u, 511u};
    float a[8][4];
    for (int j = 0; j < 8; ++j)
        for (int L = 0; L < vec; ++L)
            a[j][L] = init[j] + pcv.laneB[L] * (float(gid & masks[j]) + 1.0f);
    auto step = [&](float x, int L) -> float {
        if (op == 0) return std::fma(x, pcv.laneB[L], pcv.laneC[L]);
        if (op == 1) return x * pcv.laneM[L];
        return x + pcv.laneD[L];
    };
    if (dep) {
        for (uint32_t i = 0; i < iters; ++i)
            for (int k = 0; k < 32; ++k)
                for (int L = 0; L < vec; ++L) a[0][L] = step(a[0][L], L);
    } else {
        for (uint32_t i = 0; i < iters; ++i)
            for (int k = 0; k < 4; ++k)
                for (int j = 0; j < 8; ++j)
                    for (int L = 0; L < vec; ++L) a[j][L] = step(a[j][L], L);
    }
    double s = 0.0;
    for (int j = 0; j < 8; ++j)
        for (int L = 0; L < vec; ++L) s += (double)a[j][L];
    return s;
}

uint32_t cpuImad(uint32_t iters, uint32_t gid) {
    const PcInt pcv = pcInt();
    const uint32_t init[8] = {1, 2, 3, 5, 7, 11, 13, 17};
    uint32_t a[8][4];
    for (int j = 0; j < 8; ++j)
        for (int L = 0; L < 4; ++L) a[j][L] = gid + init[j];
    for (uint32_t i = 0; i < iters; ++i)
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 8; ++j)
                for (int L = 0; L < 4; ++L)
                    a[j][L] = a[j][L] * pcv.laneB[L] + pcv.laneC[L];
    return a[0][0] + a[0][1] + a[0][2] + a[0][3] + a[1][0] + a[2][1] + a[3][2] + a[4][3] + a[5][0] + a[6][1] +
           a[7][2] + a[7][3];
}

uint32_t cpuDp4a(uint32_t iters, uint32_t gid) {
    const uint32_t p = 0x01020304u ^ (gid * 2654435761u);
    const uint32_t q = 0x07060504u + gid * 40503u;
    uint32_t a[8] = {p, q, p ^ q, p + q, p * 3u + 1u, q * 5u + 3u, p ^ (q << 3u), q ^ (p >> 5u)};
    uint64_t dot4 = 0;
    for (int k = 0; k < 4; ++k)
        dot4 += (uint64_t)((p >> (8 * k)) & 0xFFu) * (uint64_t)((q >> (8 * k)) & 0xFFu);
    for (uint32_t i = 0; i < iters; ++i)
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 8; ++j) {
                uint64_t s = (uint64_t)a[j] + dot4;
                a[j] = s > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)s;
            }
    return a[0] ^ a[1] ^ a[2] ^ a[3] ^ a[4] ^ a[5] ^ a[6] ^ a[7];
}

uint32_t fnBit(uint32_t x, uint32_t op) {
    if (op == 0) {   // findMSB as the shader uses it
        if (x == 0) return 0xFFFFFFFFu;
        uint32_t lead = 0;
        for (int b = 31; b >= 0; --b) {
            if (x & (1u << b)) break;
            ++lead;
        }
        return 31u - lead;
    }
    if (op == 1) {
        uint32_t n = 0, y = x;
        while (y) {
            n += y & 1u;
            y >>= 1;
        }
        return n;
    }
    if (op == 2) {
        if (x == 0) return 0xFFFFFFFFu;
        uint32_t n = 0;
        while (!(x & 1u)) {
            x >>= 1;
            ++n;
        }
        return n;
    }
    uint32_t r = 0;
    for (int b = 0; b < 32; ++b)
        if (x & (1u << b)) r |= (1u << (31 - b));
    return r;
}

uint32_t cpuBit(uint32_t iters, uint32_t gid, uint32_t op) {
    const uint32_t s = gid * 2654435761u;
    uint32_t a[8] = {s | 1u, s * 3u + 1u, s * 5u + 3u, s * 7u + 5u, s ^ 0x55555555u, s ^ 0xAAAAAAAAu,
                     s * 11u + 7u, s * 13u + 11u};
    for (uint32_t i = 0; i < iters; ++i)
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 8; ++j) a[j] = fnBit(a[j], op);
    return a[0] ^ a[1] ^ a[2] ^ a[3] ^ a[4] ^ a[5] ^ a[6] ^ a[7];
}

// ---------------------------------------------------------------- verification

void verifyF32(BenchCtx& c, const char* spv, uint32_t spec, int op, int vec, int dep, uint32_t groups,
               const std::string& testName) {
    std::string err;
    VkPipeline pipe = makePipe(c, spv, spec, &err);
    if (pipe == VK_NULL_HANDLE) return;
    PcF32 pc = pcF32();
    VkDescriptorSet set = c.gpu->allocSet();
    Buffer bufs[4] = {c.work, c.work2, c.res, Buffer{}};
    c.gpu->bindBuffers(set, bufs, 3);
    bool ran = c.gpu->runOnce(pipe, set, &pc, sizeof pc, groups, 1, 1, &err);
    uint32_t threads = groups * kThreads128;
    std::vector<float> got(threads, 0.0f);
    if (ran) c.gpu->download(c.work, got.data(), threads * sizeof(float), &err);
    vkDestroyPipeline(c.gpu->dev(), pipe, nullptr);
    int bad = 0;
    if (ran) {
        for (uint32_t i = 0; i < threads; ++i) {
            double want = cpuAluF32(op, vec, dep, spec, i);
            double have = (double)got[i];
            double scale = std::max(1.0, std::fabs(want));
            if (std::fabs(have - want) / scale > 2e-3) {
                if (bad == 0) std::printf("      [%s] gid %u: got %.9g want %.9g\n", spv, i, have, want);
                ++bad;
            }
        }
    }
    for (auto& m : c.items) {
        if (m.test == testName) {
            if (!ran) m.verify = "FAIL: run failed: " + err;
            else m.verify = bad == 0 ? "ok" : ("FAIL: " + std::to_string(bad) + " threads differ");
            break;
        }
    }
}

void verifyU32(BenchCtx& c, const char* spv, uint32_t spec, const std::string& testName, uint32_t groups,
               uint32_t (*model)(uint32_t, uint32_t, uint32_t), uint32_t op, const void* pc, uint32_t pcSize) {
    std::string err;
    VkPipeline pipe = makePipe(c, spv, spec, &err);
    if (pipe == VK_NULL_HANDLE) return;
    VkDescriptorSet set = c.gpu->allocSet();
    Buffer bufs[4] = {c.work, c.work2, c.res, Buffer{}};
    c.gpu->bindBuffers(set, bufs, 3);
    bool ran = c.gpu->runOnce(pipe, set, pc, pcSize, groups, 1, 1, &err);
    uint32_t threads = groups * kThreads128;
    // Raw 32-bit readback: these kernels write uint32, reading it through a float would reinterpret
    // the bit pattern (0x84445C4F as a float is a tiny denormal -> casts back to 0).
    std::vector<uint32_t> got(threads, 0u);
    if (ran) c.gpu->download(c.work, got.data(), threads * sizeof(uint32_t), &err);
    vkDestroyPipeline(c.gpu->dev(), pipe, nullptr);
    int bad = 0;
    if (ran) {
        for (uint32_t i = 0; i < threads; ++i) {
            uint32_t want = model(spec, i, op);
            if (got[i] != want) {
                if (bad == 0) std::printf("      [%s] gid %u: got %u want %u\n", spv, i, got[i], want);
                ++bad;
            }
        }
    }
    for (auto& m : c.items) {
        if (m.test == testName) {
            if (!ran) m.verify = "FAIL: run failed";
            else m.verify = bad == 0 ? "ok" : ("FAIL: " + std::to_string(bad) + " threads differ");
            break;
        }
    }
}

uint32_t modelWrapImad(uint32_t spec, uint32_t gid, uint32_t) { return cpuImad(spec, gid); }
uint32_t modelWrapDp4a(uint32_t spec, uint32_t gid, uint32_t) { return cpuDp4a(spec, gid); }
uint32_t modelWrapBit(uint32_t spec, uint32_t gid, uint32_t op) { return cpuBit(spec, gid, op); }

void verifyAll(BenchCtx& c) {
    const uint32_t groups = 1;
    // Verification only needs a small iteration count: the CPU reference model replays every
    // step, so use the measured spec capped at 256 to keep the model cheap.
    const PcInt pci = pcInt();
    const PcU32 pcu{0u};
    if (!c.quiet) std::printf("  verify (GPU vs CPU reference): ");
    for (const auto& m : c.items) {
        if (m.group != "alu" || m.x <= 0) continue;
        const uint32_t spec = std::min<uint32_t>((uint32_t)m.x, 256u);
        const std::string t = m.test;
        if (t == "fma_f32_vec4") verifyF32(c, "alu_f32_fma_v4", spec, 0, 4, 0, groups, t);
        else if (t == "fma_f32_vec2") verifyF32(c, "alu_f32_fma_v2", spec, 0, 2, 0, groups, t);
        else if (t == "fma_f32_vec1") verifyF32(c, "alu_f32_fma_v1", spec, 0, 1, 0, groups, t);
        else if (t == "fma_f32_dep") verifyF32(c, "alu_f32_dep_v4", spec, 0, 4, 1, groups, t);
        else if (t == "mul_f32_vec4") verifyF32(c, "alu_f32_mul_v4", spec, 1, 4, 0, groups, t);
        else if (t == "add_f32_vec4") verifyF32(c, "alu_f32_add_v4", spec, 2, 4, 0, groups, t);
        else if (t == "imad_i32_vec4")
            verifyU32(c, "alu_i32_imad_v4", spec, t, groups, modelWrapImad, 0, &pci, sizeof pci);
        else if (t == "dp4a_i8") verifyU32(c, "dp4a", spec, t, groups, modelWrapDp4a, 0, &pcu, sizeof pcu);
        else if (t == "bit_clz") verifyU32(c, "bit_clz", spec, t, groups, modelWrapBit, 0, &pcu, sizeof pcu);
        else if (t == "bit_popcount") verifyU32(c, "bit_bcnt", spec, t, groups, modelWrapBit, 1, &pcu, sizeof pcu);
        else if (t == "bit_flsb") verifyU32(c, "bit_flsb", spec, t, groups, modelWrapBit, 2, &pcu, sizeof pcu);
        else if (t == "bit_reverse") verifyU32(c, "bit_rev", spec, t, groups, modelWrapBit, 3, &pcu, sizeof pcu);
    }
    if (!c.quiet) {
        int ok = 0, fail = 0;
        for (const auto& m : c.items)
            if (m.group == "alu" && !m.verify.empty()) (m.verify == "ok" ? ok : fail)++;
        std::printf("ok %d, fail %d\n", ok, fail);
    }
}

} // namespace

void benchAlu(BenchCtx& c) {
    const int reps = autoReps(c);
    const uint32_t grid = c.fullGrid;
    const PcF32 pcf = pcF32();
    const PcF16 pc16 = pcF16();
    const PcInt pci = pcInt();
    const PcU32 pcu{0u};
    const PcF64 pcd{0.0};
    if (!c.quiet) std::printf("\n[A] vector/ALU throughput\n");

    runCase(c, "alu_f32_fma_v4", "fma_f32_vec4", "fp32 FMA vec4 x8 acc", "GFLOPS", 32 * 4 * 2, grid, kThreads128,
            &pcf, sizeof pcf, reps);
    runCase(c, "alu_f32_fma_v2", "fma_f32_vec2", "fp32 FMA vec2 x8 acc", "GFLOPS", 32 * 2 * 2, grid, kThreads128,
            &pcf, sizeof pcf, reps);
    runCase(c, "alu_f32_fma_v1", "fma_f32_vec1", "fp32 FMA scalar x8 acc", "GFLOPS", 32 * 1 * 2, grid, kThreads128,
            &pcf, sizeof pcf, reps);
    runCase(c, "alu_f32_dep_v4", "fma_f32_dep", "fp32 FMA single dependency chain", "GFLOPS", 32 * 4 * 2, grid,
            kThreads128, &pcf, sizeof pcf, reps);
    runCase(c, "alu_f32_mul_v4", "mul_f32_vec4", "fp32 MUL vec4 (no add)", "GFLOPS", 32 * 4 * 1, grid, kThreads128,
            &pcf, sizeof pcf, reps);
    runCase(c, "alu_f32_add_v4", "add_f32_vec4", "fp32 ADD vec4", "GFLOPS", 32 * 4 * 1, grid, kThreads128, &pcf,
            sizeof pcf, reps);

    if (c.gpu->cap().f16) {
        runCase(c, "alu_f16_fma_v2", "fma_f16_vec2", "fp16 packed FMA x8 acc", "GFLOPS", 32 * 2 * 2, grid,
                kThreads128, &pc16, sizeof pc16, reps);
    }
    if (c.gpu->cap().f64) {
        runCase(c, "alu_f64_fma_v1", "fma_f64", "fp64 FMA x8 acc", "GFLOPS", 32 * 1 * 2, grid, kThreads128, &pcd,
                sizeof pcd, reps);
    }

    runCase(c, "alu_i32_imad_v4", "imad_i32_vec4", "int32 IMAD (a*b+c) vec4", "GOPS", 32 * 4, grid, kThreads128,
            &pci, sizeof pci, reps);

    if (c.gpu->cap().dotProduct) {
        runCase(c, "dp4a", "dp4a_i8", "int8 DP4A (4 MAC/instruction)", "GOPS", 32 * 4, grid, kThreads128, &pcu,
                sizeof pcu, reps);
    }

    {
        const char* names[5] = {"sfu_sin", "sfu_exp2", "sfu_log2", "sfu_rsqrt", "sfu_sqrt"};
        const char* tests[5] = {"sfu_sin", "sfu_exp2", "sfu_log2", "sfu_rsqrt", "sfu_sqrt"};
        const char* labels[5] = {"SFU sin", "SFU exp2", "SFU log2", "SFU inversesqrt", "SFU sqrt"};
        for (int i = 0; i < 5; ++i)
            runCase(c, names[i], tests[i], labels[i], "GOPS", 32, grid, kThreads128, &pcu, sizeof pcu, reps);
    }

    {
        const char* names[4] = {"bit_clz", "bit_bcnt", "bit_flsb", "bit_rev"};
        const char* tests[4] = {"bit_clz", "bit_popcount", "bit_flsb", "bit_reverse"};
        const char* labels[4] = {"bit findMSB", "bit popcount", "bit findLSB", "bit reverse"};
        for (int i = 0; i < 4; ++i)
            runCase(c, names[i], tests[i], labels[i], "GOPS", 32, grid, kThreads128, &pcu, sizeof pcu, reps);
    }

    {
        const Cap& cap = c.gpu->cap();
        const bool computeStage = (cap.subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
        const bool arith = (cap.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0;
        const bool shuffle = (cap.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0;
        const bool ballot = (cap.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0;
        if (computeStage && arith)
            runCase(c, "sg_add", "sg_add", "subgroupAdd reduction per iter", "GOPS", 1, grid, kThreads64, &pcu,
                    sizeof pcu, reps);
        if (computeStage && shuffle)
            runCase(c, "sg_shuffle", "sg_shuffle", "subgroupShuffle per iter", "GOPS", 1, grid, kThreads64, &pcu,
                    sizeof pcu, reps);
        if (computeStage && ballot)
            runCase(c, "sg_ballot", "sg_ballot", "subgroupBallot per iter", "GOPS", 1, grid, kThreads64, &pcu,
                    sizeof pcu, reps);
        if (computeStage)
            runCase(c, "sg_barrier", "sg_barrier", "shared round trip + 2 barriers per iter", "GOPS", 1, grid,
                    kThreads64, &pcu, sizeof pcu, reps);
    }

    runCase(c, "shared_lds", "lds_read", "shared memory (LDS) read bandwidth", "GB/s", 16, grid, 256, &pcf,
            sizeof pcf, reps);

    {
        PcU32x2 pcs{4096u, 0u};
        runCase(c, "atomic_same", "atomic_same_addr", "global atomicAdd (same address)", "GOPS", 1, grid,
                kThreads128, &pcs, sizeof pcs, reps);
        runCase(c, "atomic_scatter", "atomic_scatter", "global atomicAdd (scattered)", "GOPS", 1, grid, kThreads128,
                &pcs, sizeof pcs, reps);
    }

    verifyAll(c);

    if (!c.quiet) std::printf("  occupancy scan (workgroups -> GFLOPS): ");
    {
        // Every grid size gets its own calibrated iteration count so each dispatch is long enough
        // for the GPU timestamps to be meaningful (very short dispatches get under-measured).
        const uint32_t scans[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
        std::string err;
        for (uint32_t g : scans) {
            if (g > c.maxGroups) break;
            uint32_t spec = 0;
            Timing s;
            if (!measureSpec(c, "alu_f32_fma_v4", &pcf, sizeof pcf, g, 1, 1, 1, &spec, &s, &err)) break;
            double flops = 32.0 * 4 * 2 * double(spec) * double(g) * kThreads128;
            Measure m;
            m.group = "alu";
            m.test = "fma_f32_occupancy";
            m.label = "saturation scan: fixed kernel vs workgroup count";
            m.metric = "GFLOPS";
            m.value = flops / s.nsMedian;
            m.ns = s.nsMedian;
            m.hostTimed = s.hostFallback;
            m.nsMin = s.nsMin;
            m.nsMax = s.nsMax;
            m.x = g;
            m.xLabel = "workgroups";
            c.add(m);
            if (!c.quiet) std::printf("%u:%.0f ", g, m.value);
        }
        if (!c.quiet) std::printf("\n");
    }
}
