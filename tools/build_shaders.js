// build_shaders.js -- compiles every GLSL variant with glslang into build/spv/<name>.spv
// usage: node tools/build_shaders.js
'use strict';
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const root = path.resolve(__dirname, '..');
const glslang = path.join(root, 'tools', 'glslang', 'bin', 'glslang.exe');
const shaderDir = path.join(root, 'shaders');
const outDir = path.join(root, 'build', 'spv');

if (!fs.existsSync(glslang)) {
    console.error('[ERROR] glslang not found at ' + glslang);
    console.error('        run tools/fetch_toolchain.ps1 first');
    process.exit(1);
}
fs.mkdirSync(outDir, { recursive: true });

// Cooperative matrix configurations we precompile. Anything the device reports that is not in
// this table is listed in the report as "supported by the device, not benchmarked by this tool".
const F16 = 'float16_t';
const coopConfigs = [
    { name: 'cm_khr_16x16x16_f16f16f32', defs: { CM_M: 16, CM_N: 16, CM_K: 16, CM_TA: F16, CM_TB: F16, CM_TC: 'float', CM_NEED_F16: 1 } },
    { name: 'cm_khr_16x16x16_f16f16f16', defs: { CM_M: 16, CM_N: 16, CM_K: 16, CM_TA: F16, CM_TB: F16, CM_TC: F16, CM_NEED_F16: 1 } },
    { name: 'cm_khr_16x16x16_bf16bf16f32', defs: { CM_M: 16, CM_N: 16, CM_K: 16, CM_TA: 'bfloat16_t', CM_TB: 'bfloat16_t', CM_TC: 'float', CM_NEED_BF16: 1 } },
    { name: 'cm_khr_16x16x32_i8i8i32', defs: { CM_M: 16, CM_N: 16, CM_K: 32, CM_TA: 'int8_t', CM_TB: 'int8_t', CM_TC: 'int', CM_NEED_INT8: 1 } },
    { name: 'cm_khr_16x16x32_u8u8u32', defs: { CM_M: 16, CM_N: 16, CM_K: 32, CM_TA: 'uint8_t', CM_TB: 'uint8_t', CM_TC: 'uint', CM_NEED_INT8: 1 } },
    { name: 'cm_khr_16x16x8_f32f32f32', defs: { CM_M: 16, CM_N: 16, CM_K: 8, CM_TA: 'float', CM_TB: 'float', CM_TC: 'float' } },
    { name: 'cm_khr_16x16x32_f8e4m3f32', optional: true, defs: { CM_M: 16, CM_N: 16, CM_K: 32, CM_TA: 'floate4m3_t', CM_TB: 'floate4m3_t', CM_TC: 'float', CM_NEED_FP8: 1 } },
    { name: 'cm_khr_8x8x16_f16f16f32', defs: { CM_M: 8, CM_N: 8, CM_K: 16, CM_TA: F16, CM_TB: F16, CM_TC: 'float', CM_NEED_F16: 1 } },
    { name: 'cm_khr_8x8x32_i8i8i32', defs: { CM_M: 8, CM_N: 8, CM_K: 32, CM_TA: 'int8_t', CM_TB: 'int8_t', CM_TC: 'int', CM_NEED_INT8: 1 } },
    { name: 'cm_khr_16x8x16_f16f16f32', defs: { CM_M: 16, CM_N: 8, CM_K: 16, CM_TA: F16, CM_TB: F16, CM_TC: 'float', CM_NEED_F16: 1 } },
    { name: 'cm_khr_8x16x16_f16f16f32', defs: { CM_M: 8, CM_N: 16, CM_K: 16, CM_TA: F16, CM_TB: F16, CM_TC: 'float', CM_NEED_F16: 1 } },
    { name: 'cm_khr_16x16x16_f16f16f32_acc4', defs: { CM_M: 16, CM_N: 16, CM_K: 16, CM_TA: F16, CM_TB: F16, CM_TC: 'float', CM_NEED_F16: 1 } },
];

const variants = [
    { name: 'alu_f32_fma_v4', src: 'alu_f32.comp', defs: { OP: 0, VEC: 4 } },
    { name: 'alu_f32_fma_v2', src: 'alu_f32.comp', defs: { OP: 0, VEC: 2 } },
    { name: 'alu_f32_fma_v1', src: 'alu_f32.comp', defs: { OP: 0, VEC: 1 } },
    { name: 'alu_f32_mul_v4', src: 'alu_f32.comp', defs: { OP: 1, VEC: 4 } },
    { name: 'alu_f32_add_v4', src: 'alu_f32.comp', defs: { OP: 2, VEC: 4 } },
    { name: 'alu_f32_dep_v4', src: 'alu_f32.comp', defs: { OP: 0, VEC: 4, DEP: 1 } },
    { name: 'alu_f16_fma_v2', src: 'alu_f16.comp', defs: { VEC: 2 } },
    { name: 'alu_f16_fma_v4', src: 'alu_f16.comp', defs: { VEC: 4 } },
    { name: 'alu_f64_fma_v1', src: 'alu_f64.comp', defs: {} },
    { name: 'alu_i32_imad_v4', src: 'alu_int.comp', defs: { IOP: 0 } },
    { name: 'alu_i32_iadd_v4', src: 'alu_int.comp', defs: { IOP: 1 } },
    { name: 'dp4a', src: 'dp4a.comp', defs: {} },
    { name: 'sfu_sin', src: 'sfu.comp', defs: { SFU_OP: 0 } },
    { name: 'sfu_exp2', src: 'sfu.comp', defs: { SFU_OP: 1 } },
    { name: 'sfu_log2', src: 'sfu.comp', defs: { SFU_OP: 2 } },
    { name: 'sfu_rsqrt', src: 'sfu.comp', defs: { SFU_OP: 3 } },
    { name: 'sfu_sqrt', src: 'sfu.comp', defs: { SFU_OP: 4 } },
    { name: 'bit_clz', src: 'bits.comp', defs: { BIT_OP: 0 } },
    { name: 'bit_bcnt', src: 'bits.comp', defs: { BIT_OP: 1 } },
    { name: 'bit_flsb', src: 'bits.comp', defs: { BIT_OP: 2 } },
    { name: 'bit_rev', src: 'bits.comp', defs: { BIT_OP: 3 } },
    { name: 'sg_add', src: 'subgroup.comp', defs: { SG_OP: 0 } },
    { name: 'sg_shuffle', src: 'subgroup.comp', defs: { SG_OP: 1 } },
    { name: 'sg_ballot', src: 'subgroup.comp', defs: { SG_OP: 2 } },
    { name: 'sg_barrier', src: 'subgroup.comp', defs: { SG_OP: 3 } },
    { name: 'shared_lds', src: 'shared.comp', defs: {} },
    { name: 'chase_c1', src: 'chase.comp', defs: { CHAINS: 1 } },
    { name: 'chase_c2', src: 'chase.comp', defs: { CHAINS: 2 } },
    { name: 'chase_c4', src: 'chase.comp', defs: { CHAINS: 4 } },
    { name: 'chase_c8', src: 'chase.comp', defs: { CHAINS: 8 } },
    { name: 'bw_read', src: 'bw.comp', defs: { BW_MODE: 0 } },
    { name: 'bw_write', src: 'bw.comp', defs: { BW_MODE: 1 } },
    { name: 'bw_copy', src: 'bw.comp', defs: { BW_MODE: 2 } },
    { name: 'bw_stride', src: 'bw_stride.comp', defs: {} },
    { name: 'atomic_same', src: 'atomic.comp', defs: { ATOM_MODE: 0 } },
    { name: 'atomic_scatter', src: 'atomic.comp', defs: { ATOM_MODE: 1 } },
    { name: 'empty', src: 'empty.comp', defs: {} },
];

for (const c of coopConfigs) {
    variants.push({ name: c.name, src: 'coopmat_khr.comp', defs: c.defs, optional: c.optional, coopmat: true });
}

function tryCompile(v, targetEnv) {
    const out = path.join(outDir, v.name + '.spv');
    const args = ['--target-env', targetEnv, '-V', '-o', out, path.join(shaderDir, v.src)];
    for (const [k, val] of Object.entries(v.defs || {})) args.push('-D' + k + '=' + val);
    const r = spawnSync(glslang, args, { encoding: 'utf8' });
    return { ok: r.status === 0, out, stderr: (r.stdout || '') + (r.stderr || '') };
}

let ok = 0, failed = [], skipped = [];
for (const v of variants) {
    // Prefer the lowest SPIR-V target that compiles, so 1.1-class devices can run as much as possible.
    const envs = v.coopmat ? ['vulkan1.1', 'vulkan1.2', 'vulkan1.3'] : ['vulkan1.1'];
    let done = false, lastErr = '';
    for (const env of envs) {
        const r = tryCompile(v, env);
        if (r.ok) { ok++; done = true; if (env !== 'vulkan1.1') console.log('  ' + v.name + ': compiled for ' + env); break; }
        lastErr = r.stderr;
    }
    if (!done) {
        if (v.optional) { skipped.push(v.name); console.log('  [skip] ' + v.name + ' (optional)'); }
        else { failed.push(v.name + ': ' + lastErr.trim().split('\n').slice(0, 3).join(' | ')); }
    }
}

console.log('build_shaders: ' + ok + ' compiled, ' + skipped.length + ' optional skipped, ' + failed.length + ' failed');
if (failed.length) {
    for (const f of failed) console.error('  [FAIL] ' + f);
    process.exit(1);
}
