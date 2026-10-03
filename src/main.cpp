// main.cpp -- CLI, interactive menu, orchestration and report output.
#include "bench.h"
#include "report.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char* kUsage =
    "vkbench -- Vulkan 1.1-1.4 GPU \xe6\x9e\xb6\xe6\x9e\x84\xe8\x83\xbd\xe5\x8a\x9b\xe5\x9f\xba\xe5\x87\x86\n"
    "\n"
    "\xe7\x94\xa8\xe6\xb3\x95: vkbench [\xe9\x80\x89\xe9\xa1\xb9]\n"
    "  --list            \xe5\x88\x97\xe5\x87\xba\xe6\x89\x80\xe6\x9c\x89\xe6\x94\xaf\xe6\x8c\x81 Vulkan \xe7\x9a\x84\xe8\xae\xa1\xe7\xae\x97\xe8\xae\xbe\xe5\xa4\x87\xe5\x90\x8e\xe9\x80\x80\xe5\x87\xba\n"
    "  --caps            \xe5\x8f\xaa\xe6\x8e\xa2\xe6\xb5\x8b\xe5\xb9\xb6\xe6\x89\x93\xe5\x8d\xb0\xe8\xae\xbe\xe5\xa4\x87\xe8\x83\xbd\xe5\x8a\x9b\xef\xbc\x8c\xe4\xb8\x8d\xe6\xb5\x8b\xe8\xaf\x95\n"
    "  --device N        \xe9\x80\x89\xe6\x8b\xa9\xe7\xac\xac N \xe4\xb8\xaa\xe8\xae\xa1\xe7\xae\x97\xe8\xae\xbe\xe5\xa4\x87\xef\xbc\x88\xe9\xbb\x98\xe8\xae\xa4 0\xef\xbc\x8c\xe8\xa7\x81 --list\xef\xbc\x89\n"
    "  --quick           \xe5\xbf\xab\xe9\x80\x9f\xe6\xa8\xa1\xe5\xbc\x8f\xef\xbc\x88\xe9\x87\x87\xe6\xa0\xb7\xe5\x87\x8f\xe5\x8d\x8a\xef\xbc\x8c\xe7\xba\xa6 40 \xe7\xa7\x92\xef\xbc\x89\n"
    "  --full            \xe5\xae\x8c\xe6\x95\xb4\xe6\xa8\xa1\xe5\xbc\x8f\xef\xbc\x88\xe9\xbb\x98\xe8\xae\xa4\xef\xbc\x8c\xe7\xba\xa6 1.5-3 \xe5\x88\x86\xe9\x92\x9f\xef\xbc\x89\n"
    "  --no-alu --no-matrix --no-cache --no-mem   \xe8\xb7\xb3\xe8\xbf\x87\xe5\xaf\xb9\xe5\xba\x94\xe5\x88\x86\xe7\xbb\x84\n"
    "  --reps N          \xe6\xaf\x8f\xe4\xb8\xaa\xe6\xb5\x8b\xe7\x82\xb9\xe7\x9a\x84\xe9\x87\x8d\xe5\xa4\x8d\xe6\xac\xa1\xe6\x95\xb0\n"
    "  --target-ms X     \xe6\xaf\x8f\xe6\xac\xa1\xe8\xb0\x83\xe5\xba\xa6\xe7\x9b\xae\xe6\xa0\x87 GPU \xe6\x97\xb6\xe9\x95\xbf\xef\xbc\x88\xe9\xbb\x98\xe8\xae\xa4 15 ms\xef\xbc\x89\n"
    "  --max-buffer MB   \xe5\xb7\xa5\xe4\xbd\x9c\xe7\xbc\x93\xe5\x86\xb2\xe4\xb8\x8a\xe9\x99\x90\xef\xbc\x88\xe9\xbb\x98\xe8\xae\xa4 1.5 GiB\xef\xbc\x8c\xe5\xb0\x8f\xe6\x98\xbe\xe5\xad\x98\xe5\x8d\xa1\xe5\x8f\xaf\xe8\xb0\x83\xe5\xb0\x8f\xef\xbc\x89\n"
    "  --out DIR         \xe6\x8a\xa5\xe5\x91\x8a\xe8\xbe\x93\xe5\x87\xba\xe7\x9b\xae\xe5\xbd\x95\n"
    "  --no-html         \xe4\xb8\x8d\xe7\x94\x9f\xe6\x88\x90 HTML\xef\xbc\x88\xe4\xbb\x85 JSON+CSV\xef\xbc\x89\n"
    "  --open            \xe8\xb7\x91\xe5\xae\x8c\xe6\x89\x93\xe5\xbc\x80 HTML \xe6\x8a\xa5\xe5\x91\x8a\xef\xbc\x88\xe9\xbb\x98\xe8\xae\xa4\xe5\xb7\xb2\xe5\xbc\x80\xef\xbc\x89\n"
    "  --no-open         \xe8\xb7\x91\xe5\xae\x8c\xe4\xb8\x8d\xe6\x89\x93\xe5\xbc\x80\xe6\xb5\x8f\xe8\xa7\x88\xe5\x99\xa8\xef\xbc\x88\xe8\x84\x9a\xe6\x9c\xac\xe5\x9c\xba\xe6\x99\xaf\xe7\x94\xa8\xef\xbc\x89\n"
    "  -h --help         \xe6\x98\xbe\xe7\xa4\xba\xe6\xad\xa4\xe5\xb8\xae\xe5\x8a\xa9\n"
    "\n"
    "\xe6\x97\xa0\xe5\x8f\x82\xe6\x95\xb0\xe5\x90\xaf\xe5\x8a\xa8\xe4\xb8\x94\xe6\xa0\x87\xe5\x87\x86\xe8\xbe\x93\xe5\x85\xa5\xe6\x98\xaf\xe6\x8e\xa7\xe5\x88\xb6\xe5\x8f\xb0\xe7\x9a\x84\xe8\xaf\x9d\xef\xbc\x8c\xe4\xbc\x9a\xe5\xbc\xb9\xe4\xba\xa4\xe4\xba\x92\xe8\x8f\x9c\xe5\x8d\x95\xe3\x80\x82\n";

std::string exeDir() {
    char b[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, b, MAX_PATH);
    std::string s = b;
    size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? std::string(".") : s.substr(0, p);
}

std::string sanitize(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
            ch == '_')
            o += ch;
        else if (ch == ' ')
            o += '_';
    }
    while (!o.empty() && o.back() == '_') o.pop_back();
    return o.empty() ? "gpu" : o;
}

bool fileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);   // progress must be visible when redirected to a log
    Options o;
    bool wantHelp = false;
    bool interactive = (argc == 1);

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nextArg = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::printf("[\xe9\x94\x99\xe8\xaf\xaf] %s \xe7\xbc\xba\xe5\xb0\x91\xe5\x8f\x82\xe6\x95\xb0\n", what);
                wantHelp = true;
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--list") o.listOnly = true;
        else if (a == "--caps") o.capsOnly = true;
        else if (a == "--quick") { o.quick = true; o.full = false; }
        else if (a == "--full") { o.full = true; o.quick = false; }
        else if (a == "--no-alu") o.noAlu = true;
        else if (a == "--no-matrix") o.noMatrix = true;
        else if (a == "--no-cache") o.noCache = true;
        else if (a == "--no-mem") o.noMem = true;
        else if (a == "--open") o.open = true;
        else if (a == "--no-open") o.open = false;
        else if (a == "--no-html") o.noHtml = true;
        else if (a == "--device") { const char* v = nextArg("--device"); if (v) o.device = std::atoi(v); }
        else if (a == "--reps") { const char* v = nextArg("--reps"); if (v) o.reps = std::atoi(v); }
        else if (a == "--cache-reps") { const char* v = nextArg("--cache-reps"); if (v) o.cacheReps = std::atoi(v); }
        else if (a == "--target-ms") { const char* v = nextArg("--target-ms"); if (v) o.targetMs = std::atof(v); }
        else if (a == "--max-buffer") { const char* v = nextArg("--max-buffer"); if (v) o.maxBufferMB = (uint64_t)std::atoll(v); }
        else if (a == "--out") { const char* v = nextArg("--out"); if (v) o.outDir = v; }
        else if (a == "-h" || a == "--help") wantHelp = true;
        else {
            std::printf("[\xe9\x94\x99\xe8\xaf\xaf] \xe6\x9c\xaa\xe7\x9f\xa5\xe5\x8f\x82\xe6\x95\xb0: %s\n\n", a.c_str());
            wantHelp = true;
        }
    }
    if (wantHelp) {
        std::printf("%s", kUsage);
        return 0;
    }
    if (!o.quick && !o.full) o.full = true;   // the complete profile is the default

    std::string err;
    if (!vkMinLoad(&err)) {
        std::printf("[\xe9\x94\x99\xe8\xaf\xaf] %s\n", err.c_str());
        return 2;
    }
    if (o.listOnly) {
        Gpu::listAll(&err);
        return err.empty() ? 0 : 2;
    }

    if (interactive) {
        std::printf("\xe6\x99\xba\xe8\x83\xbd GPU \xe6\x9e\xb6\xe6\x9e\x84\xe5\x9f\xba\xe5\x87\x86 vkbench\n\n");
        Gpu::listAll(&err);
        std::printf("\n\xe8\xaf\xb7\xe9\x80\x89\xe6\x8b\xa9:\n");
        std::printf("  1) \xe5\xae\x8c\xe6\x95\xb4\xe6\xb5\x8b\xe8\xaf\x95\xef\xbc\x88\xe7\xba\xa6 2-4 \xe5\x88\x86\xe9\x92\x9f\xef\xbc\x8c\xe6\x9c\x9f\xe9\x97\xb4\xe8\xaf\xb7\xe5\x8b\xbf\xe5\xb9\xb2\xe6\x89\xb0 GPU \xe8\xb4\x9f\xe8\xbd\xbd\xef\xbc\x89\n");
        std::printf("  2) \xe5\xbf\xab\xe9\x80\x9f\xe6\xb5\x8b\xe8\xaf\x95\xef\xbc\x88\xe9\x87\x87\xe6\xa0\xb7\xe5\x87\x8f\xe5\x8d\x8a\xef\xbc\x89\n");
        std::printf("  3) \xe5\x8f\xaa\xe5\x88\x97\xe8\xae\xbe\xe5\xa4\x87\xef\xbc\x8c\xe4\xb8\x8d\xe6\xb5\x8b\xe8\xaf\x95\n");
        std::printf("  4) \xe5\x8f\xaa\xe6\x8e\xa2\xe6\xb5\x8b\xe8\x83\xbd\xe5\x8a\x9b\xef\xbc\x8c\xe4\xb8\x8d\xe6\xb5\x8b\xe8\xaf\x95\n");
        std::printf("  q) \xe9\x80\x80\xe5\x87\xba\n");
        std::printf("\xe9\x80\x89\xe6\x8b\xa9 [1]: ");
        char line[64] = {0};
        if (!std::fgets(line, sizeof line, stdin)) return 0;
        char c = line[0];
        if (c == 'q' || c == 'Q') return 0;
        if (c == '3') { Gpu::listAll(&err); return 0; }
        if (c == '4') o.capsOnly = true;
        if (c == '2') o.quick = true;
        if (c == '\n' || c == '1') { o.reps = 0; }
        o.open = true;
    }

    Gpu gpu;
    if (!gpu.init(&err, (uint32_t)o.device, false)) {
        std::printf("[\xe9\x94\x99\xe8\xaf\xaf] \xe8\xae\xbe\xe5\xa4\x87\xe5\x88\x9d\xe5\xa7\x8b\xe5\x8c\x96\xe5\xa4\xb1\xe8\xb4\xa5: %s\n", err.c_str());
        return 2;
    }
    if (gpu.capsOnly()) {
        std::printf("[\xe9\x94\x99\xe8\xaf\xaf] \xe6\xb2\xa1\xe6\x9c\x89\xe6\x89\xbe\xe5\x88\xb0\xe6\x94\xaf\xe6\x8c\x81\xe8\xae\xa1\xe7\xae\x97\xe7\x9a\x84 Vulkan \xe8\xae\xbe\xe5\xa4\x87\n");
        return 2;
    }

    BenchCtx ctx;
    ctx.gpu = &gpu;
    ctx.opt = o;
    ctx.quiet = false;
    printDeviceReport(ctx);
    if (o.capsOnly) return 0;

    // Working buffers: large enough for the cache/DRAM sweeps, but never greedy -- the tool is
    // usually run on the display adapter, where hogging VRAM hurts the desktop.
    uint64_t workBytes = std::min<uint64_t>(gpu.cap().maxBufferBytes, 1536ull << 20);
    if (o.maxBufferMB) workBytes = std::min<uint64_t>(workBytes, o.maxBufferMB << 20);
    workBytes = std::max<uint64_t>(workBytes, 64ull << 20);
    VkBufferUsageFlags u = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    uint64_t work2Bytes = std::max<uint64_t>(16ull << 20, workBytes / 4);
    ctx.work = gpu.createBuffer(workBytes, u, false, false, &err);
    ctx.work2 = gpu.createBuffer(work2Bytes, u, false, false, &err);
    ctx.res = gpu.createBuffer(1ull << 20, u, false, false, &err);
    if (!ctx.work.buf || !ctx.work2.buf || !ctx.res.buf) {
        std::printf("[\xe9\x94\x99\xe8\xaf\xaf] \xe7\xbc\x93\xe5\x86\xb2\xe5\x88\x86\xe9\x85\x8d\xe5\xa4\xb1\xe8\xb4\xa5: %s\n", err.c_str());
        return 2;
    }
    ctx.maxGroups = std::min<uint32_t>(gpu.cap().props.limits.maxComputeWorkGroupCount[0], 65535u);
    ctx.fullGrid = std::min<uint32_t>(4096u, ctx.maxGroups);
    if (!ctx.quiet)
        std::printf("  working buffers: %.2f GiB + %.2f GiB  (use --max-buffer to shrink)\n",
                    workBytes / 1073741824.0, work2Bytes / 1073741824.0);

    DWORD t0 = GetTickCount();
    if (!o.noAlu) benchAlu(ctx);
    if (!o.noMatrix) benchMatrix(ctx);
    if (!o.noCache) benchCache(ctx);
    if (!o.noMem) benchMem(ctx);
    benchOverhead(ctx);
    DWORD elapsed = GetTickCount() - t0;

    printSummary(ctx);

    // ---- write reports next to the exe (or --out)
    std::string dir = o.outDir.empty() ? exeDir() + "\\vkbench-report" : o.outDir;
    CreateDirectoryA(dir.c_str(), nullptr);
    const std::string base = sanitize(gpu.cap().props.deviceName);
    const std::string jsonPath = dir + "\\vkbench-" + base + ".json";
    const std::string csvPath = dir + "\\vkbench-" + base + ".csv";
    const std::string htmlPath = dir + "\\vkbench-" + base + ".html";
    writeJsonFile(ctx, jsonPath);
    writeCsvFile(ctx, csvPath);
    if (!o.noHtml) writeHtmlFile(ctx, htmlPath);

    std::printf("\n=== \xe8\xbe\x93\xe5\x87\xba (%.1f \xe7\xa7\x92\xe5\xae\x8c\xe6\x88\x90) ===\n", elapsed / 1000.0);
    std::printf("  JSON : %s\n", jsonPath.c_str());
    std::printf("  CSV  : %s\n", csvPath.c_str());
    if (!o.noHtml) {
        std::printf("  HTML : %s\n", htmlPath.c_str());
        if (o.open && fileExists(htmlPath)) {
            ShellExecuteA(nullptr, "open", htmlPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    return 0;
}
