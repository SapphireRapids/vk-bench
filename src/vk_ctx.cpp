// vk_ctx.cpp -- Vulkan bring-up, capability probing and the timing harness.
#include "vk_ctx.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace {

std::string vkVersionStr(uint32_t v) {
    char b[64];
    std::snprintf(b, sizeof b, "%u.%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
    return std::string(b);
}

const char* deviceTypeName(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "Discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "Virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "Other";
    }
}

bool hasExt(const std::vector<VkExtensionProperties>& v, const char* name) {
    for (const auto& e : v) if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

double nowSecondsQpc() {
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

// Links a pNext chain out of a list of Vulkan structs (all begin with sType/pNext).
void linkChain(std::vector<VkBaseOutStructure*>& chain) {
    for (size_t i = 0; i + 1 < chain.size(); ++i) chain[i]->pNext = chain[i + 1];
    if (!chain.empty()) chain.back()->pNext = nullptr;
}

} // namespace

// ------------------------------------------------------------------ instance

bool Gpu::createInstance(std::string* err) {
    uint32_t loaderVersion = 0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion == 0) loaderVersion = VK_API_VERSION_1_0;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vkbench";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "vkbench";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    // The program targets Vulkan 1.1 ... 1.4.  We create the instance at the 1.1 baseline:
    // every 1.1+ loader/driver accepts that, and per-device capabilities are probed and
    // enabled individually afterwards.
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;

    VkResult r = vkCreateInstance(&ci, nullptr, &m_instance);
    if (r != VK_SUCCESS) {
        char b[256];
        std::snprintf(b, sizeof b,
                      "vkCreateInstance(Vulkan 1.1) failed with VkResult %d; loader reports %u.%u.%u",
                      (int)r, VK_VERSION_MAJOR(loaderVersion), VK_VERSION_MINOR(loaderVersion),
                      VK_VERSION_PATCH(loaderVersion));
        if (err) *err = b;
        return false;
    }
    vkMinLoadInstance(m_instance);
    return true;
}

bool Gpu::listAll(std::string* err) {
    Gpu g;
    if (!g.createInstance(err)) return false;

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.m_instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) vkEnumeratePhysicalDevices(g.m_instance, &n, devs.data());

    std::printf("Vulkan devices (loader %s):\n", vkVersionStr(VK_API_VERSION_1_1).c_str());
    uint32_t usable = 0;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devs[i], &p);
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qs.data());
        bool compute = false;
        for (auto& q : qs) if (q.queueFlags & VK_QUEUE_COMPUTE_BIT) compute = true;
        if (!compute) continue;   // devices without a compute queue are useless to us
        std::printf("  [%u] %-40s %-14s api %s  driver 0x%08x\n", usable, p.deviceName,
                    deviceTypeName(p.deviceType), vkVersionStr(p.apiVersion).c_str(), p.driverVersion);
        ++usable;
    }
    if (usable == 0) std::printf("  (no compute-capable Vulkan device found)\n");
    std::printf("\nUse: vkbench.exe --device <index>\n");
    g.shutdown();
    return true;
}

void Gpu::shutdown() {
    if (m_dev != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_dev);
        destroyBuffer(m_dummy);
        destroyBuffer(m_staging);
        if (m_fence) vkDestroyFence(m_dev, m_fence, nullptr);
        if (m_descPool) vkDestroyDescriptorPool(m_dev, m_descPool, nullptr);
        if (m_queryPool) vkDestroyQueryPool(m_dev, m_queryPool, nullptr);
        if (m_pipeLayout) vkDestroyPipelineLayout(m_dev, m_pipeLayout, nullptr);
        if (m_setLayout) vkDestroyDescriptorSetLayout(m_dev, m_setLayout, nullptr);
        if (m_cmdPool) vkDestroyCommandPool(m_dev, m_cmdPool, nullptr);
        vkDestroyDevice(m_dev, nullptr);
        m_dev = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

// ------------------------------------------------------------------ capabilities

void Gpu::queryCapabilities(uint32_t deviceIndex, bool verboseList) {
    (void)verboseList;

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(m_instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    if (n) vkEnumeratePhysicalDevices(m_instance, &n, all.data());

    // Filter to compute-capable devices so --device N indexes what --list shows.
    std::vector<VkPhysicalDevice> devs;
    std::vector<uint32_t> physIndex;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(all[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(all[i], &qn, qs.data());
        bool compute = false;
        for (auto& q : qs) if (q.queueFlags & VK_QUEUE_COMPUTE_BIT) compute = true;
        if (compute) { devs.push_back(all[i]); physIndex.push_back(i); }
    }
    if (devs.empty()) {
        m_capsOnly = true;
        m_phys = VK_NULL_HANDLE;
        return;
    }
    if (deviceIndex >= devs.size()) deviceIndex = 0;
    m_phys = devs[deviceIndex];

    // device extensions
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(m_phys, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts(en);
    if (en) vkEnumerateDeviceExtensionProperties(m_phys, nullptr, &en, exts.data());

    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    vkGetPhysicalDeviceProperties(m_phys, &m_cap.props);

    const bool has12 = m_cap.props.apiVersion >= VK_API_VERSION_1_2;
    const bool has13 = m_cap.props.apiVersion >= VK_API_VERSION_1_3;
    const bool extDriverProps = hasExt(exts, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
    const bool extCore2 = hasExt(exts, VK_AMD_SHADER_CORE_PROPERTIES_2_EXTENSION_NAME);
    const bool extBudget = hasExt(exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    const bool extCoop = hasExt(exts, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    const bool extCoopNV = hasExt(exts, VK_NV_COOPERATIVE_MATRIX_EXTENSION_NAME);
    const bool extF16I8 = hasExt(exts, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    const bool ext16 = hasExt(exts, VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
    const bool ext8 = hasExt(exts, VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
    const bool extDot = hasExt(exts, VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME);
    const bool extSgExt = hasExt(exts, VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME);
    const bool extBf16 = hasExt(exts, VK_KHR_SHADER_BFLOAT16_EXTENSION_NAME);
    const bool extF8 = hasExt(exts, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);

    // ---- properties2 chain (only structs backed by a supported extension/core version)
    m_cap.subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    m_cap.driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    m_cap.core2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_2_AMD;
    std::vector<VkBaseOutStructure*> pchain;
    pchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&m_cap.subgroup));
    if (has12 || extDriverProps) pchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&m_cap.driver));
    if (extCore2) pchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&m_cap.core2));
    VkPhysicalDeviceShaderIntegerDotProductProperties qDotProps{};
    qDotProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES;
    if (has13 || extDot) pchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&qDotProps));
    linkChain(pchain);
    p2.pNext = pchain.empty() ? nullptr : pchain[0];
    vkGetPhysicalDeviceProperties2(m_phys, &p2);

    if (extCore2) m_cap.amdCuCount = m_cap.core2.activeComputeUnitCount;
    m_cap.maxBufferBytes = m_cap.props.limits.maxStorageBufferRange;

    // ---- memory properties (heaps + optional budget)
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 mp2{};
    mp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    if (extBudget) mp2.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(m_phys, &mp2);
    m_cap.mem = mp2.memoryProperties;
    m_cap.memoryBudget = extBudget;
    for (uint32_t i = 0; i < m_cap.mem.memoryHeapCount; ++i) {
        const auto& h = m_cap.mem.memoryHeaps[i];
        if (h.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) m_cap.heapDeviceLocalBytes += h.size;
        else m_cap.heapHostVisibleBytes += h.size;
    }

    // ---- queue families
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_phys, &qn, nullptr);
    m_cap.queues.resize(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(m_phys, &qn, m_cap.queues.data());
    int bestComp = -1;
    for (uint32_t i = 0; i < qn; ++i) {
        if (!(m_cap.queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
        const bool dedicated = !(m_cap.queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT);
        if (bestComp < 0 || dedicated) bestComp = (int)i;
        if (dedicated) break;
    }
    m_cap.computeQueueFamily = bestComp >= 0 ? (uint32_t)bestComp : 0;

    // ---- features2 query (extension structs; valid on any 1.1+ device)
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkPhysicalDeviceShaderFloat16Int8Features qF16{};
    qF16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
    VkPhysicalDevice16BitStorageFeatures q16{};
    q16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    VkPhysicalDevice8BitStorageFeatures q8{};
    q8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
    VkPhysicalDeviceShaderIntegerDotProductFeatures qDot{};
    qDot.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES;
    VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures qSgExt{};
    qSgExt.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR qCoop{};
    qCoop.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
    VkPhysicalDeviceShaderBfloat16FeaturesKHR qBf16{};
    qBf16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_BFLOAT16_FEATURES_KHR;
    VkPhysicalDeviceShaderFloat8FeaturesEXT qF8{};
    qF8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT;

    std::vector<VkBaseOutStructure*> qchain;
    qchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&qF16));
    qchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&q16));
    qchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&q8));
    if (has13 || extDot) qchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&qDot));
    if (has12 || extSgExt) qchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&qSgExt));
    if (extCoop) qchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&qCoop));
    if (extBf16) qchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&qBf16));
    if (extF8) qchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&qF8));
    linkChain(qchain);
    f2.pNext = qchain[0];
    vkGetPhysicalDeviceFeatures2(m_phys, &f2);

    m_cap.f64 = f2.features.shaderFloat64 == VK_TRUE;
    m_cap.i64 = f2.features.shaderInt64 == VK_TRUE;
    m_cap.i16 = f2.features.shaderInt16 == VK_TRUE;

    // ---- store what we intend to enable (createDevice consumes it)
    struct EnablePlan {
        bool f16 = false, i8 = false, s16 = false, s16u = false, s8 = false, s8u = false;
        bool dot = false, dotPacked = false, sgExt = false, coop = false, bf16 = false, f8 = false;
    };
    EnablePlan plan;
    plan.f16 = qF16.shaderFloat16 == VK_TRUE;
    plan.i8 = qF16.shaderInt8 == VK_TRUE;
    plan.s16 = q16.storageBuffer16BitAccess == VK_TRUE;
    plan.s16u = q16.uniformAndStorageBuffer16BitAccess == VK_TRUE;
    plan.s8 = q8.storageBuffer8BitAccess == VK_TRUE;
    plan.s8u = q8.uniformAndStorageBuffer8BitAccess == VK_TRUE;
    plan.dot = (has13 || extDot) && qDot.shaderIntegerDotProduct == VK_TRUE;
    plan.sgExt = (has12 || extSgExt) && qSgExt.shaderSubgroupExtendedTypes == VK_TRUE;
    plan.coop = extCoop && qCoop.cooperativeMatrix == VK_TRUE;
    plan.bf16 = extBf16 && qBf16.shaderBFloat16Type == VK_TRUE;
    plan.f8 = extF8 && qF8.shaderFloat8 == VK_TRUE;
    // the 4x8 packed acceleration flags live in the *properties* struct
    plan.dotPacked = qDotProps.integerDotProduct4x8BitPackedSignedAccelerated == VK_TRUE ||
                     qDotProps.integerDotProduct4x8BitPackedMixedSignednessAccelerated == VK_TRUE ||
                     qDotProps.integerDotProduct4x8BitPackedUnsignedAccelerated == VK_TRUE;

    // Availability gating: a feature can only be enabled if a matching extension/core version exists.
    if (!has12 && !extF16I8) plan.f16 = plan.i8 = false;
    if (!has12 && !ext16) plan.s16 = plan.s16u = false;
    if (!has12 && !ext8) plan.s8 = plan.s8u = false;

    // ---- include-list for the device
    std::vector<const char*> devExts;
    auto addExt = [&](const char* name) { devExts.push_back(name); m_cap.enabledExtensions.push_back(name); };
    if (!has12 && extF16I8 && (plan.f16 || plan.i8)) addExt(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    if (!has12 && ext16 && (plan.s16 || plan.s16u)) addExt(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
    if (!has12 && ext8 && (plan.s8 || plan.s8u)) addExt(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
    if (!has13 && extDot && plan.dot) addExt(VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME);
    if (!has12 && extSgExt && plan.sgExt) addExt(VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME);
    if (plan.coop) addExt(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    if (plan.bf16 && (plan.coop)) addExt(VK_KHR_SHADER_BFLOAT16_EXTENSION_NAME);
    if (plan.f8 && plan.coop) addExt(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
    if (extBudget) addExt(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (extDriverProps) addExt(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
    if (extCore2) addExt(VK_AMD_SHADER_CORE_PROPERTIES_2_EXTENSION_NAME);

    m_cap.f16 = plan.f16; m_cap.i8 = plan.i8;
    m_cap.storage16 = plan.s16; m_cap.storage16u = plan.s16u;
    m_cap.storage8 = plan.s8 || plan.s8u;
    m_cap.dotProduct = plan.dot; m_cap.dotProduct4x8Packed = plan.dotPacked;
    m_cap.sgExtendedTypes = plan.sgExt;
    m_cap.coopKHR = plan.coop; m_cap.coopNV = false;

    // Core features we may enable (shaderFloat64 etc.)
    VkPhysicalDeviceFeatures coreFeatures{};
    coreFeatures.shaderFloat64 = m_cap.f64 ? VK_TRUE : VK_FALSE;
    coreFeatures.shaderInt64 = m_cap.i64 ? VK_TRUE : VK_FALSE;
    coreFeatures.shaderInt16 = m_cap.i16 ? VK_TRUE : VK_FALSE;

    // ---- build the enable chain, version-appropriate
    VkPhysicalDeviceVulkan11Features v11{};
    v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features v12{};
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features v13{};
    v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceShaderFloat16Int8Features eF16{};
    eF16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
    VkPhysicalDevice16BitStorageFeatures e16{};
    e16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    VkPhysicalDevice8BitStorageFeatures e8{};
    e8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
    VkPhysicalDeviceShaderIntegerDotProductFeatures eDot{};
    eDot.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES;
    VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures eSgExt{};
    eSgExt.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR eCoop{};
    eCoop.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
    VkPhysicalDeviceShaderBfloat16FeaturesKHR eBf16{};
    eBf16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_BFLOAT16_FEATURES_KHR;
    VkPhysicalDeviceShaderFloat8FeaturesEXT eF8{};
    eF8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT;

    std::vector<VkBaseOutStructure*> enchain;
    if (has12) {
        v11.storageBuffer16BitAccess = plan.s16 ? VK_TRUE : VK_FALSE;
        v11.uniformAndStorageBuffer16BitAccess = plan.s16u ? VK_TRUE : VK_FALSE;
        v12.shaderFloat16 = plan.f16 ? VK_TRUE : VK_FALSE;
        v12.shaderInt8 = plan.i8 ? VK_TRUE : VK_FALSE;
        v12.storageBuffer8BitAccess = plan.s8 ? VK_TRUE : VK_FALSE;
        v12.uniformAndStorageBuffer8BitAccess = plan.s8u ? VK_TRUE : VK_FALSE;
        v12.shaderSubgroupExtendedTypes = plan.sgExt ? VK_TRUE : VK_FALSE;
        enchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&v11));
        enchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&v12));
    } else {
        if (extF16I8 && (plan.f16 || plan.i8)) {
            eF16.shaderFloat16 = plan.f16 ? VK_TRUE : VK_FALSE;
            eF16.shaderInt8 = plan.i8 ? VK_TRUE : VK_FALSE;
            enchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&eF16));
        }
        if (ext16 && (plan.s16 || plan.s16u)) {
            e16.storageBuffer16BitAccess = plan.s16 ? VK_TRUE : VK_FALSE;
            e16.uniformAndStorageBuffer16BitAccess = plan.s16u ? VK_TRUE : VK_FALSE;
            enchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&e16));
        }
        if (ext8 && (plan.s8 || plan.s8u)) {
            e8.storageBuffer8BitAccess = plan.s8 ? VK_TRUE : VK_FALSE;
            e8.uniformAndStorageBuffer8BitAccess = plan.s8u ? VK_TRUE : VK_FALSE;
            enchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&e8));
        }
        if (extSgExt && plan.sgExt) {
            eSgExt.shaderSubgroupExtendedTypes = VK_TRUE;
            enchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&eSgExt));
        }
    }
    if (has13) {
        v13.shaderIntegerDotProduct = plan.dot ? VK_TRUE : VK_FALSE;
        enchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&v13));
    } else if (extDot && plan.dot) {
        eDot.shaderIntegerDotProduct = VK_TRUE;
        enchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&eDot));
    }
    if (plan.coop) {
        eCoop.cooperativeMatrix = VK_TRUE;
        enchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&eCoop));
    }
    if (plan.bf16 && plan.coop) {
        eBf16.shaderBFloat16Type = VK_TRUE;
        enchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&eBf16));
    }
    if (plan.f8 && plan.coop) {
        eF8.shaderFloat8 = VK_TRUE;
        enchain.push_back(reinterpret_cast<VkBaseOutStructure*>(&eF8));
    }

    // ---- create the device
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = m_cap.computeQueueFamily;
    qci.queueCount = 1;
    float prio = 1.0f;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.empty() ? nullptr : devExts.data();

    VkPhysicalDeviceFeatures2 enFeatures2{};
    enFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enFeatures2.features = coreFeatures;
    linkChain(enchain);
    enFeatures2.pNext = enchain.empty() ? nullptr : enchain[0];
    dci.pNext = &enFeatures2;

    VkResult r = vkCreateDevice(m_phys, &dci, nullptr, &m_dev);
    if (r != VK_SUCCESS) {
        // Retry with no optional features at all: still a valid (if reduced) benchmark run.
        VkDeviceCreateInfo basic = dci;
        basic.pNext = nullptr;
        basic.enabledExtensionCount = 0;
        basic.ppEnabledExtensionNames = nullptr;
        r = vkCreateDevice(m_phys, &basic, nullptr, &m_dev);
        if (r != VK_SUCCESS) return;  // m_dev stays null; init() reports it
        m_cap.enabledExtensions.clear();
        m_cap.f16 = m_cap.i8 = m_cap.storage16 = m_cap.storage16u = m_cap.storage8 = false;
        m_cap.dotProduct = m_cap.dotProduct4x8Packed = m_cap.sgExtendedTypes = false;
        m_cap.coopKHR = m_cap.coopNV = false;
        m_cap.f64 = false;
    }

    vkGetDeviceQueue(m_dev, m_cap.computeQueueFamily, 0, &m_queue);

    // ---- cooperative matrix configurations (only useful when the extension is on)
    if (m_cap.coopKHR && vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR) {
        uint32_t cn = 0;
        VkResult cr = vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(m_phys, &cn, nullptr);
        if (cr == VK_SUCCESS && cn > 0) {
            m_cap.coopKHRProps.resize(cn);
            for (auto& c : m_cap.coopKHRProps) {
                c = VkCooperativeMatrixPropertiesKHR{};
                c.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
            }
            cr = vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(m_phys, &cn, m_cap.coopKHRProps.data());
            if (cr != VK_SUCCESS) m_cap.coopKHRProps.clear();
            else if (cn < m_cap.coopKHRProps.size()) m_cap.coopKHRProps.resize(cn);
        }
    }
    if (!m_cap.coopKHR && extCoopNV && vkGetPhysicalDeviceCooperativeMatrixPropertiesNV) {
        uint32_t cn = 0;
        VkResult cr = vkGetPhysicalDeviceCooperativeMatrixPropertiesNV(m_phys, &cn, nullptr);
        if (cr == VK_SUCCESS && cn > 0) {
            m_cap.coopNVProps.resize(cn);
            for (auto& c : m_cap.coopNVProps) {
                c = VkCooperativeMatrixPropertiesNV{};
                c.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_NV;
            }
            cr = vkGetPhysicalDeviceCooperativeMatrixPropertiesNV(m_phys, &cn, m_cap.coopNVProps.data());
            if (cr != VK_SUCCESS) m_cap.coopNVProps.clear();
            else if (cn < m_cap.coopNVProps.size()) m_cap.coopNVProps.resize(cn);
            m_cap.coopNV = !m_cap.coopNVProps.empty();
        }
    }

    // ---- timestamps
    const auto& qf = m_cap.queues[m_cap.computeQueueFamily];
    m_hasTimestamps = qf.timestampValidBits > 0 && m_cap.props.limits.timestampPeriod > 0.0f;

    // ---- note what a user might expect but this device lacks
    if (!m_cap.f16) m_cap.notableUnsupported.push_back("shaderFloat16 (fp16 向量/矩阵测试将跳过)");
    if (!m_cap.f64) m_cap.notableUnsupported.push_back("shaderFloat64 (fp64 测试将跳过)");
    if (!m_cap.dotProduct) m_cap.notableUnsupported.push_back("shaderIntegerDotProduct (DP4A 测试将跳过)");
    if (!m_cap.coopKHR && !m_cap.coopNV) m_cap.notableUnsupported.push_back("cooperative matrix 扩展缺失 (矩阵单元测试将跳过)");
    if (!m_hasTimestamps) m_cap.notableUnsupported.push_back("无 GPU 时间戳，计时退化为主机侧 (精度下降)");

    // ---- SPIR-V version ceiling implied by apiVersion
    m_cap.spvMax = m_cap.props.apiVersion >= VK_API_VERSION_1_3 ? "1.6" :
                   m_cap.props.apiVersion >= VK_API_VERSION_1_2 ? "1.5" : "1.3";
}

bool Gpu::createPools(std::string* err) {
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = m_cap.computeQueueFamily;
    VkResult r = vkCreateCommandPool(m_dev, &cpi, nullptr, &m_cmdPool);
    if (r != VK_SUCCESS) { if (err) *err = "vkCreateCommandPool failed"; return false; }

    VkQueryPoolCreateInfo qpi{};
    qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = m_querySlots;
    r = vkCreateQueryPool(m_dev, &qpi, nullptr, &m_queryPool);
    if (r != VK_SUCCESS) { if (err) *err = "vkCreateQueryPool(timestamp) failed"; return false; }

    VkDescriptorSetLayoutBinding bindings[kMaxBindings]{};
    for (int i = 0; i < kMaxBindings; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = kMaxBindings;
    sli.pBindings = bindings;
    r = vkCreateDescriptorSetLayout(m_dev, &sli, nullptr, &m_setLayout);
    if (r != VK_SUCCESS) { if (err) *err = "vkCreateDescriptorSetLayout failed"; return false; }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = 128;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    r = vkCreatePipelineLayout(m_dev, &pli, nullptr, &m_pipeLayout);
    if (r != VK_SUCCESS) { if (err) *err = "vkCreatePipelineLayout failed"; return false; }

    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 4096 * kMaxBindings;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 4096;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    r = vkCreateDescriptorPool(m_dev, &dpi, nullptr, &m_descPool);
    if (r != VK_SUCCESS) { if (err) *err = "vkCreateDescriptorPool failed"; return false; }

    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    r = vkCreateFence(m_dev, &fi, nullptr, &m_fence);
    if (r != VK_SUCCESS) { if (err) *err = "vkCreateFence failed"; return false; }

    std::string e;
    m_dummy = createBuffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, false, &e);
    if (m_dummy.buf == VK_NULL_HANDLE) { if (err) *err = "dummy buffer: " + e; return false; }

    // Staging: quarter of the host-visible heap, 4 MiB .. 64 MiB.
    uint64_t stageSize = m_cap.heapHostVisibleBytes / 4;
    if (stageSize < (4ull << 20)) stageSize = 4ull << 20;
    if (stageSize > (64ull << 20)) stageSize = 64ull << 20;
    m_staging = createBuffer(stageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             true, true, &e);
    if (m_staging.buf == VK_NULL_HANDLE) { if (err) *err = "staging buffer: " + e; return false; }
    return true;
}

bool Gpu::init(std::string* err, uint32_t deviceIndex, bool verboseList) {
    (void)verboseList;
    m_querySlots = 1024;
    if (!createInstance(err)) return false;
    if (!vkCreateDevice || !vkGetPhysicalDeviceFeatures2) {
        if (err) *err = "Vulkan 1.1 entry points missing (loader too old?)";
        return false;
    }
    queryCapabilities(deviceIndex, verboseList);
    if (m_phys == VK_NULL_HANDLE) {
        m_capsOnly = true;
        return true;
    }
    if (m_dev == VK_NULL_HANDLE) {
        if (err) *err = "vkCreateDevice failed (see reported features)";
        return false;
    }
    if (!createPools(err)) return false;

    // Working buffer ceiling: what we may bind as one storage buffer.
    uint64_t cap = m_cap.heapDeviceLocalBytes ? m_cap.heapDeviceLocalBytes * 3 / 4 : (uint64_t)(1u << 30);
    cap = std::min<uint64_t>(cap, m_cap.props.limits.maxStorageBufferRange);
    if (cap > (2ull << 30)) cap = 2ull << 30;
    m_cap.maxBufferBytes = cap;
    return true;
}

// ------------------------------------------------------------------ buffers

int findMemoryType(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags want,
                   VkMemoryPropertyFlags avoid = 0) {
    int fallback = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & want) == want) {
            if (avoid && (f & avoid)) { if (fallback < 0) fallback = (int)i; continue; }
            return (int)i;
        }
    }
    return fallback;
}

Buffer Gpu::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, bool map,
                         std::string* err) {
    Buffer b;
    if (size == 0) size = 4;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(m_dev, &bi, nullptr, &b.buf);
    if (r != VK_SUCCESS) { if (err) *err = "vkCreateBuffer failed"; return Buffer{}; }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_dev, b.buf, &req);

    VkMemoryPropertyFlags want = hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int mt = findMemoryType(m_cap.mem, req.memoryTypeBits, want,
                            hostVisible ? 0 : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mt < 0) mt = findMemoryType(m_cap.mem, req.memoryTypeBits, want);
    if (mt < 0) {
        vkDestroyBuffer(m_dev, b.buf, nullptr);
        if (err) *err = "no suitable memory type";
        return Buffer{};
    }

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    r = vkAllocateMemory(m_dev, &ai, nullptr, &b.mem);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(m_dev, b.buf, nullptr);
        if (err) *err = "vkAllocateMemory failed (" + std::to_string((unsigned long long)req.size) + " bytes)";
        return Buffer{};
    }
    r = vkBindBufferMemory(m_dev, b.buf, b.mem, 0);
    if (r != VK_SUCCESS) {
        vkFreeMemory(m_dev, b.mem, nullptr);
        vkDestroyBuffer(m_dev, b.buf, nullptr);
        if (err) *err = "vkBindBufferMemory failed";
        return Buffer{};
    }
    b.size = size;
    if (map) {
        r = vkMapMemory(m_dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped);
        if (r != VK_SUCCESS) b.mapped = nullptr;
    }
    return b;
}

void Gpu::destroyBuffer(Buffer& b) {
    if (b.mapped && m_dev) { vkUnmapMemory(m_dev, b.mem); b.mapped = nullptr; }
    if (b.buf && m_dev) vkDestroyBuffer(m_dev, b.buf, nullptr);
    if (b.mem && m_dev) vkFreeMemory(m_dev, b.mem, nullptr);
    b = Buffer{};
}

VkCommandBuffer Gpu::beginSingle() {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_dev, &ai, &cb);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

bool Gpu::submitSingle(VkCommandBuffer cb, std::string* err) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkResetFences(m_dev, 1, &m_fence);
    VkResult r = vkQueueSubmit(m_queue, 1, &si, m_fence);
    if (r != VK_SUCCESS) { if (err) *err = "vkQueueSubmit failed"; return false; }
    r = vkWaitForFences(m_dev, 1, &m_fence, VK_TRUE, UINT64_MAX);
    vkFreeCommandBuffers(m_dev, m_cmdPool, 1, &cb);
    if (r != VK_SUCCESS) { if (err) *err = "vkWaitForFences failed"; return false; }
    return true;
}

bool Gpu::upload(const Buffer& dst, const void* data, VkDeviceSize size, std::string* err) {
    if (size > dst.size) { if (err) *err = "upload larger than buffer"; return false; }
    const char* p = (const char*)data;
    VkDeviceSize off = 0;
    while (off < size) {
        VkDeviceSize chunk = std::min<VkDeviceSize>(size - off, m_staging.size);
        std::memcpy(m_staging.mapped, p + off, (size_t)chunk);
        VkCommandBuffer cb = beginSingle();
        VkBufferCopy cp{};
        cp.srcOffset = 0;
        cp.dstOffset = off;
        cp.size = chunk;
        vkCmdCopyBuffer(cb, m_staging.buf, dst.buf, 1, &cp);
        // Make the transfer writes visible to later compute/transfer reads.
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
        if (!submitSingle(cb, err)) return false;
        off += chunk;
    }
    return true;
}

bool Gpu::download(const Buffer& src, void* out, VkDeviceSize size, std::string* err) {
    if (size > src.size) { if (err) *err = "download larger than buffer"; return false; }
    char* p = (char*)out;
    VkDeviceSize off = 0;
    while (off < size) {
        VkDeviceSize chunk = std::min<VkDeviceSize>(size - off, m_staging.size);
        VkCommandBuffer cb = beginSingle();
        // Make earlier shader writes visible to the transfer read.
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        VkBufferCopy cp{};
        cp.srcOffset = off;
        cp.dstOffset = 0;
        cp.size = chunk;
        vkCmdCopyBuffer(cb, src.buf, m_staging.buf, 1, &cp);
        if (!submitSingle(cb, err)) return false;
        std::memcpy(p + off, m_staging.mapped, (size_t)chunk);
        off += chunk;
    }
    return true;
}

bool Gpu::fill(const Buffer& dst, uint32_t value, std::string* err) {
    VkCommandBuffer cb = beginSingle();
    vkCmdFillBuffer(cb, dst.buf, 0, VK_WHOLE_SIZE, value);
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    return submitSingle(cb, err);
}

// ------------------------------------------------------------------ pipelines

VkShaderModule Gpu::makeModule(const uint32_t* spv, size_t bytes, std::string* err) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = spv;
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(m_dev, &ci, nullptr, &m);
    if (r != VK_SUCCESS) {
        if (err) *err = "vkCreateShaderModule failed (VkResult " + std::to_string((int)r) + ")";
        return VK_NULL_HANDLE;
    }
    return m;
}

VkPipeline Gpu::makePipeline(VkShaderModule mod, const VkSpecializationInfo* spec, std::string* err) {
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = spec;
    ci.layout = m_pipeLayout;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateComputePipelines(m_dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    if (r != VK_SUCCESS) {
        if (err) *err = "vkCreateComputePipelines failed (VkResult " + std::to_string((int)r) + ")";
        return VK_NULL_HANDLE;
    }
    return p;
}

VkDescriptorSet Gpu::allocSet() {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    VkDescriptorSet s = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_dev, &ai, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    return s;
}

void Gpu::bindBuffers(VkDescriptorSet set, const Buffer* bufs, int count) {
    VkDescriptorBufferInfo info[kMaxBindings]{};
    VkWriteDescriptorSet w[kMaxBindings]{};
    for (int i = 0; i < kMaxBindings; ++i) {
        const Buffer& b = (i < count && bufs[i].buf != VK_NULL_HANDLE) ? bufs[i] : m_dummy;
        info[i].buffer = b.buf;
        info[i].offset = 0;
        info[i].range = VK_WHOLE_SIZE;
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &info[i];
    }
    vkUpdateDescriptorSets(m_dev, kMaxBindings, w, 0, nullptr);
}


Timing Gpu::runCopyTimed(const Buffer& src, const Buffer& dst, VkDeviceSize bytes, int reps, int warmup,
                         std::string* err) {
    Timing t;
    if (bytes > src.size || bytes > dst.size) { if (err) *err = "copy larger than buffer"; return t; }
    if (reps < 1) reps = 1;
    if (warmup < 0) warmup = 0;
    const int total = reps + warmup;
    if (2 * total > (int)m_querySlots) reps = std::max(1, (int)m_querySlots / 2 - warmup);
    const int totalClamped = reps + warmup;

    std::vector<uint64_t> ts(2 * totalClamped, 0);
    VkCommandBuffer cb = beginSingle();
    vkCmdResetQueryPool(cb, m_queryPool, 0, 2 * totalClamped);
    const VkPipelineStageFlagBits tsStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const double hostT0 = nowSecondsQpc();
    for (int i = 0; i < totalClamped; ++i) {
        vkCmdWriteTimestamp(cb, tsStage, m_queryPool, 2 * i);
        VkBufferCopy cp{};
        cp.srcOffset = 0;
        cp.dstOffset = 0;
        cp.size = bytes;
        vkCmdCopyBuffer(cb, src.buf, dst.buf, 1, &cp);
        vkCmdWriteTimestamp(cb, tsStage, m_queryPool, 2 * i + 1);
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
    }
    if (!submitSingle(cb, err)) return t;
    const double hostMsPerDispatch = (nowSecondsQpc() - hostT0) * 1000.0 / totalClamped;
    t.hostMs = hostMsPerDispatch;
    VkResult r = vkGetQueryPoolResults(m_dev, m_queryPool, 0, 2 * totalClamped, ts.size() * sizeof(uint64_t),
                                       ts.data(), sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (r != VK_SUCCESS) { if (err) *err = "vkGetQueryPoolResults failed"; return t; }
    const uint32_t validBits = m_cap.queues[m_cap.computeQueueFamily].timestampValidBits;
    const uint64_t wrapMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1);
    const double period = m_cap.props.limits.timestampPeriod;
    std::vector<double> deltas;
    for (int i = 0; i < totalClamped; ++i) {
        uint64_t a = ts[2 * i] & wrapMask;
        uint64_t b = ts[2 * i + 1] & wrapMask;
        if (b < a) b += (wrapMask + 1);
        if (i >= warmup && b >= a) deltas.push_back(double(b - a) * period);
    }
    if (deltas.empty()) { if (err) *err = "no valid timestamp deltas"; return t; }
    std::sort(deltas.begin(), deltas.end());
    t.nsMin = deltas.front();
    t.nsMax = deltas.back();
    t.nsMedian = deltas[deltas.size() / 2];
    if (t.nsMedian < 0.5 * hostMsPerDispatch * 1e6) {
        double scale = (hostMsPerDispatch * 1e6) / std::max(1.0, t.nsMedian);
        t.nsMedian *= scale;
        t.nsMin *= scale;
        t.nsMax *= scale;
        t.hostFallback = true;
    }
    return t;
}

// ------------------------------------------------------------------ timing

bool Gpu::runOnce(VkPipeline pipe, VkDescriptorSet set, const void* pc, uint32_t pcSize, uint32_t gx,
                  uint32_t gy, uint32_t gz, std::string* err) {
    VkCommandBuffer cb = beginSingle();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeLayout, 0, 1, &set, 0, nullptr);
    if (pcSize) vkCmdPushConstants(cb, m_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize, pc);
    vkCmdDispatch(cb, gx, gy, gz);
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    return submitSingle(cb, err);
}

Timing Gpu::runTimed(VkPipeline pipe, VkDescriptorSet set, const void* pc, uint32_t pcSize, uint32_t gx,
                     uint32_t gy, uint32_t gz, int reps, int warmup, std::string* err) {
    Timing t;
    if (reps < 1) reps = 1;
    if (warmup < 0) warmup = 0;
    const int total = reps + warmup;
    if (2 * total > (int)m_querySlots) {
        reps = std::max(1, (int)m_querySlots / 2 - warmup);
    }
    const int totalClamped = reps + warmup;

    if (m_hasTimestamps) {
        std::vector<uint64_t> ts(2 * totalClamped, 0);
        VkCommandBuffer cb = beginSingle();
        vkCmdResetQueryPool(cb, m_queryPool, 0, 2 * totalClamped);
        // Timestamps must be taken at COMPUTE_SHADER: with ALL_COMMANDS some drivers record the
        // timestamp as soon as the command processor gets there, which badly under-measures short
        // dispatches (verified on this host's AMD driver).
        const VkPipelineStageFlagBits tsStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        const double hostT0 = nowSecondsQpc();
        for (int i = 0; i < totalClamped; ++i) {
            vkCmdWriteTimestamp(cb, tsStage, m_queryPool, 2 * i);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeLayout, 0, 1, &set, 0, nullptr);
            if (pcSize) vkCmdPushConstants(cb, m_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize, pc);
            vkCmdDispatch(cb, gx, gy, gz);
            vkCmdWriteTimestamp(cb, tsStage, m_queryPool, 2 * i + 1);
            VkMemoryBarrier mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb,
                                 0, nullptr, 0, nullptr);
        }
        if (!submitSingle(cb, err)) return t;
        VkResult r = vkGetQueryPoolResults(m_dev, m_queryPool, 0, 2 * totalClamped,
                                           ts.size() * sizeof(uint64_t), ts.data(), sizeof(uint64_t),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (r != VK_SUCCESS) { if (err) *err = "vkGetQueryPoolResults failed"; return t; }
        const double hostMsPerDispatch = (nowSecondsQpc() - hostT0) * 1000.0 / totalClamped;
        t.hostMs = hostMsPerDispatch;

        const uint32_t validBits = m_cap.queues[m_cap.computeQueueFamily].timestampValidBits;
        const uint64_t wrapMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1);
        const double period = m_cap.props.limits.timestampPeriod;
        std::vector<double> deltas;
        for (int i = 0; i < totalClamped; ++i) {
            uint64_t a = ts[2 * i] & wrapMask;
            uint64_t b = ts[2 * i + 1] & wrapMask;
            if (b < a) b += (wrapMask + 1);   // timestamp wrap on <64-bit counters
            if (i >= warmup && b >= a) deltas.push_back(double(b - a) * period);
        }
        if (deltas.empty()) { if (err) *err = "no valid timestamp deltas"; return t; }
        std::sort(deltas.begin(), deltas.end());
        t.nsMin = deltas.front();
        t.nsMax = deltas.back();
        t.nsMedian = deltas[deltas.size() / 2];
        // Some drivers (AMD on this host) write the COMPUTE_SHADER timestamp as soon as the front end
        // has *issued* the dispatch, which under-measures whenever the grid is small enough that all
        // waves go resident immediately.  When the timestamp disagrees badly with the host wall clock
        // we prefer the wall clock and flag the point.
        if (t.nsMedian < 0.5 * hostMsPerDispatch * 1e6) {
            double scale = (hostMsPerDispatch * 1e6) / std::max(1.0, t.nsMedian);
            t.nsMedian *= scale;
            t.nsMin *= scale;
            t.nsMax *= scale;
            t.hostFallback = true;
        }
        return t;
    }

    // Fallback: host-side wall clock around submit+fence. Includes submission overhead.
    std::vector<double> deltas;
    std::vector<double> hostAll;
    for (int i = 0; i < totalClamped; ++i) {
        double t0 = nowSecondsQpc();
        if (!runOnce(pipe, set, pc, pcSize, gx, gy, gz, err)) return t;
        double t1 = nowSecondsQpc();
        if (i >= warmup) deltas.push_back((t1 - t0) * 1e9);
        hostAll.push_back((t1 - t0) * 1e9);
    }
    if (deltas.empty()) return t;
    std::sort(deltas.begin(), deltas.end());
    t.nsMin = deltas.front();
    t.nsMax = deltas.back();
    t.nsMedian = deltas[deltas.size() / 2];
    t.hostMs = hostAll.empty() ? 0 : (hostAll.back() / 1e6);
    t.hostFallback = true;   // the whole measurement came from the host clock
    return t;
}
