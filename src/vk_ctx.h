// vk_ctx.h -- Vulkan context: instance/device bring-up, capability probing,
// buffers, pipelines, descriptor sets and the timestamp-based timing harness.
#pragma once

#include "vk_min.h"

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------- small types

struct Timing {
    double nsMedian = 0;   // median GPU time of one dispatch, nanoseconds
    double nsMin = 0;
    double nsMax = 0;
    double hostMs = 0;     // host wall clock per dispatch (submit + fence included)
    bool hostFallback = false;   // true when the GPU timestamp was unusable and hostMs was used
    double spreadPct() const { return nsMedian > 0 ? 100.0 * (nsMax - nsMin) / nsMedian : 0; }
};

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

// Everything we learned about the device that the report shows or the tests gate on.
struct Cap {
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceDriverProperties driver{};
    VkPhysicalDeviceSubgroupProperties subgroup{};
    VkPhysicalDeviceShaderCoreProperties2AMD core2{};
    std::vector<VkQueueFamilyProperties> queues;
    VkPhysicalDeviceMemoryProperties mem{};
    uint32_t computeQueueFamily = 0;

    // features actually enabled on the device
    bool f16 = false;            // shaderFloat16
    bool i8 = false;             // shaderInt8
    bool f64 = false;            // shaderFloat64
    bool i16 = false;            // shaderInt16
    bool i64 = false;            // shaderInt64
    bool storage16 = false;      // storageBuffer16BitAccess
    bool storage16u = false;     // uniformAndStorageBuffer16BitAccess
    bool storage8 = false;       // storageBuffer8BitAccess
    bool dotProduct = false;     // shaderIntegerDotProduct
    bool dotProduct4x8Packed = false;
    bool sgExtendedTypes = false;
    bool coopKHR = false;
    bool coopNV = false;
    bool memoryBudget = false;
    uint32_t amdCuCount = 0;     // 0 when unknown
    std::string spvMax;          // max SPIR-V version the apiVersion allows (text)

    std::vector<std::string> enabledExtensions;
    std::vector<std::string> notableUnsupported;

    std::vector<VkCooperativeMatrixPropertiesKHR> coopKHRProps;
    std::vector<VkCooperativeMatrixPropertiesNV> coopNVProps;
    std::vector<std::string> coopExtraUnsupported;  // supported by device but not precompiled

    uint64_t heapDeviceLocalBytes = 0;
    uint64_t heapHostVisibleBytes = 0;
    uint64_t maxBufferBytes = 0;    // how big a storage buffer we may allocate
};

// ---------------------------------------------------------------- Gpu context

class Gpu {
public:
    ~Gpu() { shutdown(); }

    bool init(std::string* err, uint32_t deviceIndex, bool verboseList);
    // Enumerates compute-capable devices and prints a short summary; no device is created.
    static bool listAll(std::string* err);
    bool capsOnly() const { return m_capsOnly; }
    void shutdown();

    const Cap& cap() const { return m_cap; }
    bool hasTimestamps() const { return m_hasTimestamps; }

    // --- resources
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                        bool map, std::string* err);
    void destroyBuffer(Buffer& b);
    bool upload(const Buffer& dst, const void* data, VkDeviceSize size, std::string* err);
    bool download(const Buffer& src, void* out, VkDeviceSize size, std::string* err);
    bool fill(const Buffer& dst, uint32_t value, std::string* err);

    // --- pipeline plumbing. Shaders may use up to 4 storage buffers in set 0.
    static const int kMaxBindings = 4;
    VkShaderModule makeModule(const uint32_t* spv, size_t bytes, std::string* err);
    // spec constants pointer/length are passed straight to VkSpecializationInfo
    VkPipeline makePipeline(VkShaderModule mod, const VkSpecializationInfo* spec, std::string* err);
    void bindBuffers(VkDescriptorSet set, const Buffer* bufs, int count);
    VkDescriptorSet allocSet();

    // --- execution / timing
    // Repeated dispatch of one pipeline; warmup runs untimed, then `reps` timed runs.
    // Median of the timed dispatches is returned. Each dispatch is bracketed by GPU timestamps.
    Timing runTimed(VkPipeline pipe, VkDescriptorSet set, const void* pc, uint32_t pcSize,
                    uint32_t gx, uint32_t gy, uint32_t gz, int reps, int warmup, std::string* err);
    bool runOnce(VkPipeline pipe, VkDescriptorSet set, const void* pc, uint32_t pcSize,
                 uint32_t gx, uint32_t gy, uint32_t gz, std::string* err);
    // Timed vkCmdCopyBuffer (transfer engine), same timestamp bracketing as runTimed.
    Timing runCopyTimed(const Buffer& src, const Buffer& dst, VkDeviceSize bytes, int reps, int warmup,
                        std::string* err);

    VkDevice dev() const { return m_dev; }
    VkQueue queue() const { return m_queue; }
    VkPhysicalDevice phys() const { return m_phys; }

private:
    bool createInstance(std::string* err);
    void queryCapabilities(uint32_t deviceIndex, bool verboseList);
    bool createPools(std::string* err);

    VkCommandBuffer beginSingle();
    bool submitSingle(VkCommandBuffer cb, std::string* err);

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys = VK_NULL_HANDLE;
    VkDevice m_dev = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkQueryPool m_queryPool = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    Buffer m_dummy;      // bound to unused descriptor slots
    Buffer m_staging;    // reused host-visible staging
    Cap m_cap;
    bool m_hasTimestamps = false;
    bool m_capsOnly = false;
    uint32_t m_querySlots = 0;
};
