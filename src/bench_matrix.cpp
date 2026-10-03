// bench_matrix.cpp -- group B: cooperative matrix (tensor-class) throughput.
#include "bench.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct CoopCfg {
    const char* spv;
    uint32_t m, n, k;
    VkComponentTypeKHR ta, tb, tc;
    const char* label;
};

const VkComponentTypeKHR F16 = VK_COMPONENT_TYPE_FLOAT16_KHR;
const VkComponentTypeKHR BF16 = VK_COMPONENT_TYPE_BFLOAT16_KHR;
const VkComponentTypeKHR F32 = VK_COMPONENT_TYPE_FLOAT32_KHR;
const VkComponentTypeKHR I8 = VK_COMPONENT_TYPE_SINT8_KHR;
const VkComponentTypeKHR U8 = VK_COMPONENT_TYPE_UINT8_KHR;
const VkComponentTypeKHR I32 = VK_COMPONENT_TYPE_SINT32_KHR;
const VkComponentTypeKHR U32 = VK_COMPONENT_TYPE_UINT32_KHR;
const VkComponentTypeKHR F8E4M3 = VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT;

const CoopCfg kCfgs[] = {
    {"cm_khr_16x16x16_f16f16f32", 16, 16, 16, F16, F16, F32, "fp16 x fp16 -> fp32"},
    {"cm_khr_16x16x16_f16f16f16", 16, 16, 16, F16, F16, F16, "fp16 x fp16 -> fp16"},
    {"cm_khr_16x16x16_bf16bf16f32", 16, 16, 16, BF16, BF16, F32, "bf16 x bf16 -> fp32"},
    {"cm_khr_16x16x32_i8i8i32", 16, 16, 32, I8, I8, I32, "int8 x int8 -> int32"},
    {"cm_khr_16x16x32_u8u8u32", 16, 16, 32, U8, U8, U32, "uint8 x uint8 -> uint32"},
    {"cm_khr_16x16x8_f32f32f32", 16, 16, 8, F32, F32, F32, "fp32(tf32) x fp32 -> fp32"},
    {"cm_khr_16x16x32_f8e4m3f32", 16, 16, 32, F8E4M3, F8E4M3, F32, "fp8 e4m3 x fp8 -> fp32"},
    {"cm_khr_8x8x16_f16f16f32", 8, 8, 16, F16, F16, F32, "fp16 8x8x16 -> fp32"},
    {"cm_khr_8x8x32_i8i8i32", 8, 8, 32, I8, I8, I32, "int8 8x8x32 -> int32"},
    {"cm_khr_16x8x16_f16f16f32", 16, 8, 16, F16, F16, F32, "fp16 16x8x16 -> fp32"},
    {"cm_khr_8x16x16_f16f16f32", 8, 16, 16, F16, F16, F32, "fp16 8x16x16 -> fp32"},
};

uint16_t f2h(float f) {
    uint32_t x = 0;
    std::memcpy(&x, &f, 4);
    uint16_t sign = (uint16_t)((x >> 16) & 0x8000u);
    int32_t exp = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t man = x & 0x7FFFFFu;
    if (exp <= 0) return sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (man >> 13));
}

bool coopSupported(const Cap& cap, const CoopCfg& cfg) {
    for (const auto& p : cap.coopKHRProps) {
        if (p.MSize != cfg.m || p.NSize != cfg.n || p.KSize != cfg.k) continue;
        if (p.AType != cfg.ta || p.BType != cfg.tb || p.CType != cfg.tc) continue;
        if (p.ResultType != cfg.tc) continue;
        return true;
    }
    return false;
}

std::string coopTypeName(VkComponentTypeKHR t) {
    switch (t) {
        case VK_COMPONENT_TYPE_FLOAT16_KHR: return "f16";
        case VK_COMPONENT_TYPE_FLOAT32_KHR: return "f32";
        case VK_COMPONENT_TYPE_FLOAT64_KHR: return "f64";
        case VK_COMPONENT_TYPE_BFLOAT16_KHR: return "bf16";
        case VK_COMPONENT_TYPE_SINT8_KHR: return "i8";
        case VK_COMPONENT_TYPE_SINT16_KHR: return "i16";
        case VK_COMPONENT_TYPE_SINT32_KHR: return "i32";
        case VK_COMPONENT_TYPE_SINT64_KHR: return "i64";
        case VK_COMPONENT_TYPE_UINT8_KHR: return "u8";
        case VK_COMPONENT_TYPE_UINT16_KHR: return "u16";
        case VK_COMPONENT_TYPE_UINT32_KHR: return "u32";
        case VK_COMPONENT_TYPE_UINT64_KHR: return "u64";
        case VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT: return "f8e4m3";
        case VK_COMPONENT_TYPE_FLOAT8_E5M2_EXT: return "f8e5m2";
        default: return "type" + std::to_string((int)t);
    }
}

double aval(uint32_t row, uint32_t col, const CoopCfg& cfg) {
    bool isInt = (cfg.ta == I8 || cfg.ta == U8);
    return isInt ? (double)(1 + ((row + col) % 4)) : (1.0 + ((row + col) % 4) * 0.25);
}
double bval(uint32_t row, uint32_t col, const CoopCfg& cfg) {
    bool isInt = (cfg.tb == I8 || cfg.tb == U8);
    return isInt ? (double)(1 + ((row + col) % 3)) : (1.0 + ((row + col) % 3) * 0.5);
}

size_t elemSize(VkComponentTypeKHR t) {
    switch (t) {
        case VK_COMPONENT_TYPE_FLOAT16_KHR:
        case VK_COMPONENT_TYPE_BFLOAT16_KHR:
        case VK_COMPONENT_TYPE_SINT16_KHR:
        case VK_COMPONENT_TYPE_UINT16_KHR: return 2;
        case VK_COMPONENT_TYPE_SINT8_KHR:
        case VK_COMPONENT_TYPE_UINT8_KHR:
        case VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT:
        case VK_COMPONENT_TYPE_FLOAT8_E5M2_EXT: return 1;
        default: return 4;
    }
}

void storeElem(uint8_t* p, VkComponentTypeKHR t, double v) {
    switch (t) {
        case VK_COMPONENT_TYPE_FLOAT16_KHR: {
            uint16_t h = f2h((float)v);
            std::memcpy(p, &h, 2);
            break;
        }
        case VK_COMPONENT_TYPE_BFLOAT16_KHR: {
            float f = (float)v;
            uint32_t x = 0;
            std::memcpy(&x, &f, 4);
            uint16_t b = (uint16_t)(x >> 16);
            std::memcpy(p, &b, 2);
            break;
        }
        case VK_COMPONENT_TYPE_SINT8_KHR: {
            int8_t b = (int8_t)v;
            std::memcpy(p, &b, 1);
            break;
        }
        case VK_COMPONENT_TYPE_UINT8_KHR: {
            uint8_t b = (uint8_t)v;
            std::memcpy(p, &b, 1);
            break;
        }
        default: {
            float f = (float)v;
            std::memcpy(p, &f, 4);
            break;
        }
    }
}

double loadElem(const uint8_t* p, VkComponentTypeKHR t) {
    switch (t) {
        case VK_COMPONENT_TYPE_FLOAT16_KHR: {
            uint16_t h = 0;
            std::memcpy(&h, p, 2);
            uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
            uint32_t exp = (h >> 10) & 0x1Fu;
            uint32_t man = h & 0x3FFu;
            if (exp == 0) return (h & 0x8000u) ? -0.0 : 0.0;   // (we only produce normal values)
            uint32_t x = sign | ((exp - 15 + 127) << 23) | (man << 13);
            float f = 0;
            std::memcpy(&f, &x, 4);
            return (double)f;
        }
        case VK_COMPONENT_TYPE_BFLOAT16_KHR: {
            uint16_t b = 0;
            std::memcpy(&b, p, 2);
            uint32_t x = (uint32_t)b << 16;
            float f = 0;
            std::memcpy(&f, &x, 4);
            return f;
        }
        case VK_COMPONENT_TYPE_SINT8_KHR: return (double)*(const int8_t*)p;
        case VK_COMPONENT_TYPE_UINT8_KHR: return (double)*(const uint8_t*)p;
        default: {
            float f = 0;
            std::memcpy(&f, p, 4);
            return f;
        }
    }
}

} // namespace

void benchMatrix(BenchCtx& c) {
    if (!c.quiet) std::printf("\n[B] \xe7\x9f\xa9\xe9\x98\xb5\xe5\x8d\x95\xe5\x85\x83 (cooperative matrix)\n");
    const Cap& cap = c.gpu->cap();
    const uint32_t subgroupSize = cap.subgroup.subgroupSize ? cap.subgroup.subgroupSize : 32;
    const uint32_t subgroupsPerGroup = 64 / subgroupSize;

    if (!cap.coopKHR && !cap.coopNV) {
        Measure m;
        m.group = "matrix";
        m.test = "coop_unsupported";
        m.label = "cooperative matrix extension not exposed by this device/driver";
        m.metric = "TFLOPS";
        m.verify = "unsupported";
        c.add(m);
        if (!c.quiet) std::printf("  VK_KHR_cooperative_matrix / VK_NV_cooperative_matrix \xe5\x9d\x87\xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8\n");
        return;
    }
    if (subgroupsPerGroup == 0) {
        if (!c.quiet) std::printf("  subgroup size %u does not divide the 64-thread workgroup\n", subgroupSize);
        return;
    }

    // Report what the device advertises.
    if (!c.quiet) {
        std::printf("  \xe8\xae\xbe\xe5\xa4\x87\xe5\xa3\xb0\xe6\x98\x8e %u \xe4\xb8\xaa config:\n", (unsigned)cap.coopKHRProps.size());
        for (const auto& p : cap.coopKHRProps) {
            std::printf("    %ux%ux%u  A=%s B=%s C=%s result=%s scope=%u sat=%u\n", p.MSize, p.NSize, p.KSize,
                        coopTypeName(p.AType).c_str(), coopTypeName(p.BType).c_str(), coopTypeName(p.CType).c_str(),
                        coopTypeName(p.ResultType).c_str(), (unsigned)p.scope, (unsigned)p.saturatingAccumulation);
        }
    }

    std::string err;
    // Small dedicated buffers: A, B, C.
    Buffer bufA = c.gpu->createBuffer(64 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      false, false, &err);
    Buffer bufB = c.gpu->createBuffer(64 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      false, false, &err);
    Buffer bufC = c.gpu->createBuffer(64 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      false, false, &err);
    if (!bufA.buf || !bufB.buf || !bufC.buf) {
        if (!c.quiet) std::printf("  \xe7\xbc\x93\xe5\x86\xb2\xe5\x88\x86\xe9\x85\x8d\xe5\xa4\xb1\xe8\xb4\xa5: %s\n", err.c_str());
        return;
    }

    int ran = 0;
    for (const CoopCfg& cfg : kCfgs) {
        if (!coopSupported(cap, cfg)) continue;
        const size_t ea = elemSize(cfg.ta), eb = elemSize(cfg.tb), ec = elemSize(cfg.tc);
        std::vector<uint8_t> ha(cfg.m * cfg.k * ea), hb(cfg.k * cfg.n * eb), hc(cfg.m * cfg.n * ec, 0);
        for (uint32_t r = 0; r < cfg.m; ++r)
            for (uint32_t k = 0; k < cfg.k; ++k) storeElem(&ha[(r * cfg.k + k) * ea], cfg.ta, aval(r, k, cfg));
        for (uint32_t k = 0; k < cfg.k; ++k)
            for (uint32_t n = 0; n < cfg.n; ++n) storeElem(&hb[(k * cfg.n + n) * eb], cfg.tb, bval(k, n, cfg));
        c.gpu->upload(bufA, ha.data(), ha.size(), &err);
        c.gpu->upload(bufB, hb.data(), hb.size(), &err);
        c.gpu->fill(bufC, 0, &err);

        // ---- calibration + measurement
        uint32_t spec = 64;
        Timing t;
        const uint32_t groups = c.fullGrid;
        bool ok = true;
        for (int pass = 0; pass < 2; ++pass) {
            VkPipeline pipe = makePipe(c, cfg.spv, spec, &err);
            if (!pipe) {
                ok = false;
                break;
            }
            VkDescriptorSet set = c.gpu->allocSet();
            Buffer bufs[4] = {bufA, bufB, bufC, Buffer{}};
            c.gpu->bindBuffers(set, bufs, 3);
            Timing s = c.gpu->runTimed(pipe, set, nullptr, 0, groups, 1, 1, 1, 1, &err);
            vkDestroyPipeline(c.gpu->dev(), pipe, nullptr);
            if (s.nsMedian <= 0) {
                ok = false;
                break;
            }
            double scaled = double(spec) * (c.opt.targetMs * 1e6) / s.nsMedian;
            uint32_t next = (uint32_t)std::min(4.0e9, std::max(1.0, std::floor(scaled)));
            if (next == spec) {
                t = s;
                break;
            }
            spec = next;
            t = s;
        }
        if (!ok) {
            Measure m;
            m.group = "matrix";
            m.test = cfg.spv;
            m.label = cfg.label;
            m.metric = "TFLOPS";
            m.verify = "FAIL: " + err;
            c.add(m);
            continue;
        }
        // final timed run
        {
            VkPipeline pipe = makePipe(c, cfg.spv, spec, &err);
            if (pipe) {
                VkDescriptorSet set = c.gpu->allocSet();
                Buffer bufs[4] = {bufA, bufB, bufC, Buffer{}};
                c.gpu->bindBuffers(set, bufs, 3);
                t = c.gpu->runTimed(pipe, set, nullptr, 0, groups, 1, 1, c.opt.quick ? 2 : 4, 1, &err);
                vkDestroyPipeline(c.gpu->dev(), pipe, nullptr);
            }
        }
        const bool isInt = (cfg.tc == I32 || cfg.tc == U32);
        const double ops = 2.0 * cfg.m * cfg.n * cfg.k * 2.0 * double(spec) * double(subgroupsPerGroup) * double(groups);
        Measure m;
        m.group = "matrix";
        m.test = cfg.spv;
        m.label = cfg.label;
        m.metric = isInt ? "TOPS" : "TFLOPS";
        // Median of the timed dispatches: on this class of GPU the shim behind cooperative matrix
        // has very irregular cost, and the minimum lands on the dispatches whose timestamps
        // under-report, which inflates the number.  The spread is reported alongside.
        m.value = ops / t.nsMedian / 1000.0;   // ops/ns = GOPS -> TFLOPS/TOPS
        m.ns = t.nsMedian;
        m.hostTimed = t.hostFallback;
        m.nsMin = t.nsMin;
        m.nsMax = t.nsMax;
        m.x = (double)cfg.m * cfg.n * cfg.k;
        m.xLabel = "m*n*k";
        c.add(m);
        ++ran;
        if (!c.quiet)
            std::printf("  %-28s %8.2f %-7s (%.2f ms, iters %u)\n", cfg.spv, m.value, m.metric.c_str(), t.nsMedian / 1e6,
                        spec);

        // ---- verification with a tiny iteration count
        {
            const uint32_t vspec = 3;
            VkPipeline pipe = makePipe(c, cfg.spv, vspec, &err);
            if (pipe) {
                VkDescriptorSet set = c.gpu->allocSet();
                Buffer bufs[4] = {bufA, bufB, bufC, Buffer{}};
                c.gpu->bindBuffers(set, bufs, 3);
                c.gpu->fill(bufC, 0, &err);
                bool ranOk = c.gpu->runOnce(pipe, set, nullptr, 0, 1, 1, 1, &err);
                std::vector<uint8_t> got(cfg.m * cfg.n * ec);
                if (ranOk) c.gpu->download(bufC, got.data(), got.size(), &err);
                vkDestroyPipeline(c.gpu->dev(), pipe, nullptr);
                int bad = 0;
                if (ranOk) {
                    for (uint32_t r = 0; r < cfg.m && bad == 0; ++r) {
                        for (uint32_t n = 0; n < cfg.n; ++n) {
                            double want = 0;
                            for (uint32_t k = 0; k < cfg.k; ++k) want += aval(r, k, cfg) * bval(k, n, cfg);
                            want *= (double)vspec;
                            double have = loadElem(&got[(r * cfg.n + n) * ec], cfg.tc);
                            double diff = std::fabs(have - want);
                            double tol = isInt ? 1e-9 : std::max(1e-4, std::fabs(want) * 1e-2);
                            if (diff > tol) {
                                if (bad == 0)
                                    std::printf("      [%s] C[%u][%u]: got %.6g want %.6g\n", cfg.spv, r, n, have,
                                                want);
                                ++bad;
                                break;
                            }
                        }
                    }
                }
                for (auto& mm : c.items) {
                    if (mm.test == cfg.spv) {
                        mm.verify = !ranOk ? "FAIL: 校验运行失败" : (bad == 0 ? "ok" : "FAIL: 与 CPU 参考不符");
                        break;
                    }
                }
            }
        }
    }

    // List configs the device supports but we did not precompile.
    for (const auto& p : cap.coopKHRProps) {
        bool known = false;
        for (const CoopCfg& cfg : kCfgs) {
            if (p.MSize == cfg.m && p.NSize == cfg.n && p.KSize == cfg.k && p.AType == cfg.ta && p.BType == cfg.tb &&
                p.CType == cfg.tc)
                known = true;
        }
        if (!known) {
            char b[160];
            std::snprintf(b, sizeof b, "%ux%ux%u A=%s B=%s C=%s", p.MSize, p.NSize, p.KSize,
                          coopTypeName(p.AType).c_str(), coopTypeName(p.BType).c_str(), coopTypeName(p.CType).c_str());
            Measure m;
            m.group = "matrix";
            m.test = "coop_not_precompiled";
            m.label = b;
            m.metric = "TFLOPS";
            m.verify = "not benchmarked";
            c.add(m);
        }
    }
    if (ran == 0 && !c.quiet) std::printf("  \xe8\xae\xbe\xe5\xa4\x87\xe5\xa3\xb0\xe6\x98\x8e\xe7\x9a\x84 config \xe9\x83\xbd\xe4\xb8\x8d\xe5\x9c\xa8\xe9\xa2\x84\xe7\xbc\x96\xe8\xaf\x91\xe8\xa1\xa8\xe4\xb8\xad\n");

    c.gpu->destroyBuffer(bufA);
    c.gpu->destroyBuffer(bufB);
    c.gpu->destroyBuffer(bufC);
}
