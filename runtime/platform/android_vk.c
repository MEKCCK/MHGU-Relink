// Vulkan 1.2 entry points on Android. The NDK's libvulkan.so for older API levels only exports Vulkan 1.1, but the
// phone's driver has 1.2 (the renderer needs timeline semaphores, nvn/vulkan/device.c): these two are found through
// vkGetDeviceProcAddr on the device they are called for.
#include <vulkan/vulkan.h>

typedef VkResult (*PfnCounter)(VkDevice, VkSemaphore, uint64_t *);
typedef VkResult (*PfnWait)(VkDevice, const VkSemaphoreWaitInfo *, uint64_t);

static PfnCounter counter_fn;
static PfnWait wait_fn;

VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice dev, VkSemaphore s, uint64_t *value) {
    PfnCounter f = __atomic_load_n(&counter_fn, __ATOMIC_ACQUIRE);
    if (!f) {
        f = (PfnCounter)vkGetDeviceProcAddr(dev, "vkGetSemaphoreCounterValue");
        if (!f) f = (PfnCounter)vkGetDeviceProcAddr(dev, "vkGetSemaphoreCounterValueKHR");
        if (!f) return VK_ERROR_FEATURE_NOT_PRESENT;
        __atomic_store_n(&counter_fn, f, __ATOMIC_RELEASE);
    }
    return f(dev, s, value);
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice dev, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    PfnWait f = __atomic_load_n(&wait_fn, __ATOMIC_ACQUIRE);
    if (!f) {
        f = (PfnWait)vkGetDeviceProcAddr(dev, "vkWaitSemaphores");
        if (!f) f = (PfnWait)vkGetDeviceProcAddr(dev, "vkWaitSemaphoresKHR");
        if (!f) return VK_ERROR_FEATURE_NOT_PRESENT;
        __atomic_store_n(&wait_fn, f, __ATOMIC_RELEASE);
    }
    return f(dev, info, timeout);
}
