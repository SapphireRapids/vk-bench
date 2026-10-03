// vk_min.cpp -- runtime loader for vulkan-1.dll (see vk_min.h).
#include "vk_min.h"

#include <windows.h>
#include <cstring>

#define VK_DEFINE_FN(name) PFN_##name name = nullptr;
VK_FUNCTION_LIST(VK_DEFINE_FN)
#undef VK_DEFINE_FN

namespace {

struct FnEntry {
    const char* name;
    void** slot;
};

FnEntry g_fns[] = {
#define X(name) {#name, reinterpret_cast<void**>(&name)},
    VK_FUNCTION_LIST(X)
#undef X
};

PFN_vkGetInstanceProcAddr g_gipa = nullptr;
HMODULE g_dll = nullptr;

// The three commands that are resolvable without an instance handle.
bool isGlobalCommand(const char* name) {
    return std::strcmp(name, "vkCreateInstance") == 0 ||
           std::strcmp(name, "vkEnumerateInstanceVersion") == 0 ||
           std::strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0;
}

} // namespace

bool vkMinLoad(std::string* err) {
    g_dll = LoadLibraryA("vulkan-1.dll");
    if (!g_dll) {
        if (err) *err = "LoadLibraryA(\"vulkan-1.dll\") failed: no Vulkan runtime installed";
        return false;
    }
    g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(g_dll, "vkGetInstanceProcAddr")));
    if (!g_gipa) {
        if (err) *err = "vulkan-1.dll has no vkGetInstanceProcAddr export";
        return false;
    }
    for (const FnEntry& e : g_fns) {
        if (isGlobalCommand(e.name)) {
            *e.slot = reinterpret_cast<void*>(g_gipa(nullptr, e.name));
        }
    }
    if (!vkCreateInstance) {
        if (err) *err = "vkCreateInstance could not be resolved";
        return false;
    }
    return true;
}

void vkMinLoadInstance(VkInstance instance) {
    if (!g_gipa || !instance) return;
    for (const FnEntry& e : g_fns) {
        if (*e.slot == nullptr) {
            *e.slot = reinterpret_cast<void*>(g_gipa(instance, e.name));
        }
    }
}

bool vkMinHave(const char* name) {
    for (const FnEntry& e : g_fns) {
        if (std::strcmp(e.name, name) == 0) return *e.slot != nullptr;
    }
    return false;
}
