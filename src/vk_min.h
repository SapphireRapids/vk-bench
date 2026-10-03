// vk_min.h -- minimal dynamic Vulkan loader (no import library, no SDK needed).
// Loads vulkan-1.dll at runtime and resolves the ~50 entry points we use through
// vkGetInstanceProcAddr. Header-only use is not intended; see vk_min.cpp.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan_core.h>
#include <string>

// Every Vulkan entry point this program uses. X(name) declares a PFN_##name global.
#define VK_FUNCTION_LIST(X)                    \
    X(vkCreateInstance)                        \
    X(vkEnumerateInstanceVersion)              \
    X(vkEnumerateInstanceExtensionProperties)  \
    X(vkEnumeratePhysicalDevices)              \
    X(vkGetPhysicalDeviceProperties)           \
    X(vkGetPhysicalDeviceFeatures2)            \
    X(vkGetPhysicalDeviceProperties2)          \
    X(vkGetPhysicalDeviceQueueFamilyProperties)\
    X(vkGetPhysicalDeviceMemoryProperties2)    \
    X(vkEnumerateDeviceExtensionProperties)    \
    X(vkCreateDevice)                          \
    X(vkGetDeviceProcAddr)                     \
    X(vkDestroyInstance)                       \
    X(vkDestroyDevice)                         \
    X(vkGetDeviceQueue)                        \
    X(vkDeviceWaitIdle)                        \
    X(vkQueueSubmit)                           \
    X(vkQueueWaitIdle)                         \
    X(vkAllocateMemory)                        \
    X(vkFreeMemory)                            \
    X(vkMapMemory)                             \
    X(vkUnmapMemory)                           \
    X(vkFlushMappedMemoryRanges)               \
    X(vkInvalidateMappedMemoryRanges)          \
    X(vkCreateBuffer)                          \
    X(vkDestroyBuffer)                         \
    X(vkGetBufferMemoryRequirements)           \
    X(vkBindBufferMemory)                      \
    X(vkCreateShaderModule)                    \
    X(vkDestroyShaderModule)                   \
    X(vkCreateDescriptorSetLayout)             \
    X(vkDestroyDescriptorSetLayout)            \
    X(vkCreatePipelineLayout)                  \
    X(vkDestroyPipelineLayout)                 \
    X(vkCreateComputePipelines)                \
    X(vkDestroyPipeline)                       \
    X(vkCreateDescriptorPool)                  \
    X(vkDestroyDescriptorPool)                 \
    X(vkAllocateDescriptorSets)                \
    X(vkResetDescriptorPool)                   \
    X(vkUpdateDescriptorSets)                  \
    X(vkCreateCommandPool)                     \
    X(vkDestroyCommandPool)                    \
    X(vkAllocateCommandBuffers)                \
    X(vkFreeCommandBuffers)                    \
    X(vkBeginCommandBuffer)                    \
    X(vkEndCommandBuffer)                      \
    X(vkResetCommandBuffer)                    \
    X(vkCmdBindPipeline)                       \
    X(vkCmdBindDescriptorSets)                 \
    X(vkCmdPushConstants)                      \
    X(vkCmdDispatch)                           \
    X(vkCmdFillBuffer)                         \
    X(vkCmdCopyBuffer)                         \
    X(vkCmdPipelineBarrier)                    \
    X(vkCmdWriteTimestamp)                     \
    X(vkCmdResetQueryPool)                     \
    X(vkCreateQueryPool)                       \
    X(vkDestroyQueryPool)                      \
    X(vkGetQueryPoolResults)                   \
    X(vkCreateFence)                           \
    X(vkDestroyFence)                          \
    X(vkWaitForFences)                         \
    X(vkResetFences)                           \
    X(vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR) \
    X(vkGetPhysicalDeviceCooperativeMatrixPropertiesNV)

#define VK_DECLARE_FN(name) extern PFN_##name name;
VK_FUNCTION_LIST(VK_DECLARE_FN)
#undef VK_DECLARE_FN

// Loads vulkan-1.dll and resolves instance-independent + device-level entry points.
// Returns false with a message in err if the loader or vkGetInstanceProcAddr is missing.
bool vkMinLoad(std::string* err);

// Resolves instance-level entry points once the instance exists.
// (Any function not known to a 1.0/1.1 loader may stay null; callers must check.)
void vkMinLoadInstance(VkInstance instance);

// True if every pointer in a comma-free list of names is non-null; helper for capability gates.
bool vkMinHave(const char* name);
