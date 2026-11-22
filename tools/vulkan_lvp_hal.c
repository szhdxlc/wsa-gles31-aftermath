/*
 * vulkan_lvp_hal.c
 *
 * Android 的 Vulkan 加载器 (/system/lib64/libvulkan.so) 用的是 libhardware
 * 的 HAL 模型：它 dlopen("/vendor/lib64/hw/vulkan.<ro.hardware.vulkan>.so")
 * 然后 dlsym("HMI")，拿到 hw_module_t，再通过 methods->open 取 hwvulkan_device_t。
 *
 * 而 Mesa 的 lavapipe (vulkan.lvp.so) 是标准 ICD，只导出 vk_icdGetInstanceProcAddr，
 * 没有 HMI。这个文件就是那层薄壳：导出 HMI，把三个 HAL 入口转发给真正的 ICD。
 *
 * 构建:
 *   clang --target=x86_64-linux-android33 -shared -fPIC -O2 \
 *         -o vulkan.lvp.so vulkan_lvp_hal.c -ldl
 * 部署:
 *   /vendor/lib64/hw/vulkan.lvp.so        <- 本文件编译产物
 *   /vendor/lib64/hw/vulkan.lvp.real.so   <- 原始的 Mesa lavapipe
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

/* ---- libhardware 的最小 ABI 定义（NDK 不带这些头，按 AOSP 布局手写） ---- */
#define HARDWARE_MODULE_TAG 0x48574D54u /* 'HWMT' */
#define HARDWARE_DEVICE_TAG 0x48574454u /* 'HWDT' */

typedef struct hw_module_t hw_module_t;
typedef struct hw_device_t hw_device_t;

typedef struct hw_module_methods_t {
    int (*open)(const hw_module_t* module, const char* id, hw_device_t** device);
} hw_module_methods_t;

struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char* id;
    const char* name;
    const char* author;
    hw_module_methods_t* methods;
    void* dso;
#ifdef __LP64__
    uint64_t reserved[32 - 7];
#else
    uint32_t reserved[32 - 7];
#endif
};

/* 注意：AOSP 里 reserved 在 LP64 下是 uint64_t[12]（96 字节），close 在最后 */
struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    hw_module_t* module;
#ifdef __LP64__
    uint64_t reserved[12];
#else
    uint32_t reserved[12];
#endif
    int (*close)(hw_device_t* device);
};

typedef struct hwvulkan_device_t {
    hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;

#define HWVULKAN_DEVICE_0 0

/* ---- 转发到真正的 ICD ---- */
static void* g_icd = NULL;
static PFN_vkEnumerateInstanceExtensionProperties g_eiep = NULL;
static PFN_vkCreateInstance g_ci = NULL;
static PFN_vkGetInstanceProcAddr g_gipa = NULL;

static const char* kCandidates[] = {
    "/vendor/lib64/hw/vulkan.lvp.real.so",
    "/vendor/lib64/hw/vulkan_lvp_icd.so",
    "/system/vendor/lib64/hw/vulkan.lvp.real.so",
    NULL,
};

static int ensure_icd(void) {
    if (g_gipa) return 0;
    for (int i = 0; kCandidates[i]; i++) {
        g_icd = dlopen(kCandidates[i], RTLD_NOW | RTLD_LOCAL);
        if (g_icd) break;
    }
    if (!g_icd) return -1;

    /* 告诉 ICD 我们按 loader 协议第 5 版对接（可选，失败不算错） */
    PFN_vkVoidFunction neg =
        dlsym(g_icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (neg) {
        uint32_t ver = 5;
        ((VkResult (*)(uint32_t*))neg)(&ver);
    }

    g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_icd, "vk_icdGetInstanceProcAddr");
    if (!g_gipa) g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_icd, "vkGetInstanceProcAddr");
    if (!g_gipa) return -1;

    g_eiep = (PFN_vkEnumerateInstanceExtensionProperties)
        g_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    g_ci = (PFN_vkCreateInstance)g_gipa(NULL, "vkCreateInstance");
    if (!g_eiep || !g_ci) return -1;
    return 0;
}

static VkResult fwd_EnumerateInstanceExtensionProperties(
        const char* layerName, uint32_t* count, VkExtensionProperties* props) {
    if (ensure_icd() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    return g_eiep(layerName, count, props);
}

static VkResult fwd_CreateInstance(const VkInstanceCreateInfo* ci,
                                   const VkAllocationCallbacks* alloc,
                                   VkInstance* inst) {
    if (ensure_icd() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    return g_ci(ci, alloc, inst);
}

static PFN_vkVoidFunction fwd_GetInstanceProcAddr(VkInstance inst, const char* name) {
    if (ensure_icd() != 0) return NULL;
    return g_gipa(inst, name);
}

static hwvulkan_device_t g_device = {
    .common = {
        .tag = HARDWARE_DEVICE_TAG,
        .version = HWVULKAN_DEVICE_0,
        .module = NULL,
        .close = NULL,
        .reserved = {0},
    },
    .EnumerateInstanceExtensionProperties = fwd_EnumerateInstanceExtensionProperties,
    .CreateInstance = fwd_CreateInstance,
    .GetInstanceProcAddr = fwd_GetInstanceProcAddr,
};

static int hal_open(const hw_module_t* module, const char* id, hw_device_t** device) {
    (void)id;
    g_device.common.module = (hw_module_t*)module;
    (void)ensure_icd();
    *device = &g_device.common;
    return 0;
}

static hw_module_methods_t g_methods = { .open = hal_open };

/* 加载器 dlsym 找的就是这个符号 */
__attribute__((visibility("default")))
hw_module_t HMI = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = 1,
    .hal_api_version = 0,
    .id = "vulkan",
    .name = "Mesa lavapipe (HAL wrapper)",
    .author = "port",
    .methods = &g_methods,
    .dso = NULL,
    .reserved = {0},
};
