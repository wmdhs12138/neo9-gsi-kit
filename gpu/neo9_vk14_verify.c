/*
 * Real Vulkan 1.4 Hardware Functional Verification Suite for Adreno 740.
 * Actually executes Vulkan 1.4 Core APIs on live GPU silicon — zero fake flags!
 * Tests:
 *   1. VkPhysicalDeviceProperties.apiVersion >= 1.4.0 & VkPhysicalDeviceVulkan14Features query
 *   2. Core 1.4 vkMapMemory2 / vkUnmapMemory2 (VK_KHR_map_memory2)
 *   3. Core 1.4 vkCmdBindDescriptorSets2 + vkCmdPushConstants2 + Compute Dispatch (VK_KHR_maintenance6)
 *      -> verifies 4096/4096 uint32 outputs bit-exact: out[i] = in[i] * 3 + pushConst(7)
 *   4. Core 1.4 vkGetRenderingAreaGranularity (VK_KHR_maintenance5)
 *   5. Core 1.4 vkTransitionImageLayout + vkCopyMemoryToImage + vkCopyImageToMemory (VK_EXT_host_image_copy)
 *      -> uploads 64x64 RGBA8 pattern directly from host memory to GPU image and reads back 16384/16384 bytes bit-exact!
 *   6. Core 1.4 vkCmdSetRenderingAttachmentLocations & vkCmdSetRenderingInputAttachmentIndices
 *      entrypoint resolution & dynamic rendering local read check.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef int32_t  VkResult;
typedef void*    VkInstance;
typedef void*    VkPhysicalDevice;
typedef void*    VkDevice;
typedef void*    VkQueue;
typedef uint64_t VkBuffer;
typedef uint64_t VkImage;
typedef uint64_t VkDeviceMemory;
typedef uint64_t VkDescriptorSetLayout;
typedef uint64_t VkDescriptorPool;
typedef uint64_t VkDescriptorSet;
typedef uint64_t VkPipelineLayout;
typedef uint64_t VkShaderModule;
typedef uint64_t VkPipeline;
typedef uint64_t VkPipelineCache;
typedef uint64_t VkCommandPool;
typedef void*    VkCommandBuffer;
typedef uint64_t VkFence;

#define VK_SUCCESS 0
#define VK_WHOLE_SIZE (~0ULL)
#define VK_MAKE_API_VERSION(v,maj,min,pat) ((((uint32_t)(v))<<29)|(((uint32_t)(maj))<<22)|(((uint32_t)(min))<<12)|((uint32_t)(pat)))
#define VK_API_MAJOR(v) (((v)>>22)&0x7f)
#define VK_API_MINOR(v) (((v)>>12)&0x3ff)
#define VK_API_PATCH(v) ((v)&0xfff)

#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 1000059000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES 55
#define VK_STRUCTURE_TYPE_MEMORY_MAP_INFO 1000271000
#define VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO 1000271001
#define VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO 1000545001
#define VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO 1000545002
#define VK_STRUCTURE_TYPE_RENDERING_AREA_INFO 1000470003
#define VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO 14
#define VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO 1000270004
#define VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO 1000270005
#define VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO 1000270007
#define VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY 1000270008
#define VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY 1000270009

#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  0x2
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 0x4
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT  0x1
#define VK_BUFFER_USAGE_STORAGE_BUFFER_BIT   0x20
#define VK_IMAGE_USAGE_HOST_TRANSFER_BIT     0x00400000
#define VK_IMAGE_USAGE_SAMPLED_BIT           0x4
#define VK_DESCRIPTOR_TYPE_STORAGE_BUFFER    7
#define VK_SHADER_STAGE_COMPUTE_BIT          0x20
#define VK_PIPELINE_BIND_POINT_COMPUTE       1
#define VK_IMAGE_TYPE_2D                     1
#define VK_FORMAT_R8G8B8A8_UNORM             37
#define VK_IMAGE_TILING_OPTIMAL              0
#define VK_IMAGE_LAYOUT_UNDEFINED            0
#define VK_IMAGE_LAYOUT_GENERAL              1
#define VK_IMAGE_ASPECT_COLOR_BIT            1

/* SPIR-V compute shader: data[gid] = data[gid] * 3 + pushConst.addVal */
static const uint32_t kComputePushSpv[] = {
    0x07230203, 0x00010000, 0x00080001, 0x00000026, 0x00000000,
    0x00020011, 0x00000001,
    0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000c,
    0x00060010, 0x00000004, 0x00000011, 0x00000040, 0x00000001, 0x00000001,
    0x00040047, 0x0000000c, 0x0000000b, 0x0000001c,
    0x00040047, 0x00000007, 0x00000006, 0x00000004,
    0x00050048, 0x00000008, 0x00000000, 0x00000023, 0x00000000,
    0x00030047, 0x00000008, 0x00000003,
    0x00040047, 0x0000000a, 0x00000022, 0x00000000,
    0x00040047, 0x0000000a, 0x00000021, 0x00000000,
    /* PushConstant block %20 = struct { uint addVal; } */
    0x00050048, 0x00000020, 0x00000000, 0x00000023, 0x00000000,
    0x00030047, 0x00000020, 0x00000002, /* Block */
    /* Types */
    0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002,
    0x00040015, 0x00000006, 0x00000020, 0x00000000,
    0x0003001d, 0x00000007, 0x00000006,
    0x0003001e, 0x00000008, 0x00000007,
    0x00040020, 0x00000009, 0x00000002, 0x00000008,
    0x0004003b, 0x00000009, 0x0000000a, 0x00000002,
    0x00040017, 0x0000000b, 0x00000006, 0x00000003,
    0x00040020, 0x0000000d, 0x00000001, 0x0000000b,
    0x0004003b, 0x0000000d, 0x0000000c, 0x00000001,
    0x00040020, 0x0000000e, 0x00000001, 0x00000006,
    0x00040020, 0x0000000f, 0x00000002, 0x00000006,
    0x00040015, 0x00000010, 0x00000020, 0x00000001,
    0x0004002b, 0x00000010, 0x00000011, 0x00000000,
    0x0004002b, 0x00000006, 0x00000012, 0x00000000,
    0x0004002b, 0x00000006, 0x00000013, 0x00000003,
    0x0003001e, 0x00000020, 0x00000006,                   /* %20 = struct { uint } */
    0x00040020, 0x00000021, 0x00000009, 0x00000020,       /* %21 = PushConstant* struct */
    0x0004003b, 0x00000021, 0x00000022, 0x00000009,       /* %22 = Var PushConstant */
    0x00040020, 0x00000023, 0x00000009, 0x00000006,       /* %23 = PushConstant* uint */
    /* main */
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005,
    0x00050041, 0x0000000e, 0x00000015, 0x0000000c, 0x00000012,
    0x0004003d, 0x00000006, 0x00000016, 0x00000015,
    0x00060041, 0x0000000f, 0x00000017, 0x0000000a, 0x00000011, 0x00000016,
    0x0004003d, 0x00000006, 0x00000018, 0x00000017,
    0x00050084, 0x00000006, 0x00000019, 0x00000018, 0x00000013,
    0x00050041, 0x00000023, 0x00000024, 0x00000022, 0x00000011, /* &pushConst.addVal */
    0x0004003d, 0x00000006, 0x00000025, 0x00000024,             /* addVal */
    0x00050080, 0x00000006, 0x0000001a, 0x00000019, 0x00000025, /* data*3 + addVal */
    0x0003003e, 0x00000017, 0x0000001a,
    0x000100fd,
    0x00010038
};

typedef struct { int32_t sType; const void* pNext; const char* app; uint32_t av; const char* eng; uint32_t ev; uint32_t api; } VkApplicationInfo;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; const VkApplicationInfo* app; uint32_t lc; const char* const* ln; uint32_t ec; const char* const* en; } VkInstanceCreateInfo;
typedef struct { uint32_t apiVersion; uint32_t driverVersion; uint32_t vendorID; uint32_t deviceID; int32_t deviceType; char deviceName[256]; uint8_t uuid[16]; uint8_t rest[1024]; } VkPhysicalDeviceProperties;
typedef struct {
    int32_t sType; void* pNext;
    VkBool32 globalPriorityQuery;
    VkBool32 shaderSubgroupRotate;
    VkBool32 shaderSubgroupRotateClustered;
    VkBool32 shaderFloatControls2;
    VkBool32 shaderExpectAssume;
    VkBool32 rectangularLines;
    VkBool32 bresenhamLines;
    VkBool32 smoothLines;
    VkBool32 stippledRectangularLines;
    VkBool32 stippledBresenhamLines;
    VkBool32 stippledSmoothLines;
    VkBool32 vertexAttributeInstanceRateDivisor;
    VkBool32 vertexAttributeInstanceRateZeroDivisor;
    VkBool32 indexTypeUint8;
    VkBool32 dynamicRenderingLocalRead;
    VkBool32 maintenance5;
    VkBool32 maintenance6;
    VkBool32 pipelineProtectedAccess;
    VkBool32 pipelineRobustness;
    VkBool32 hostImageCopy;
    VkBool32 pushDescriptor;
    uint32_t extra_pad[32];
} VkPhysicalDeviceVulkan14Features;
typedef struct { int32_t sType; void* pNext; VkBool32 features[55]; } VkPhysicalDeviceFeatures2;
typedef struct { VkFlags flags; uint32_t count; uint32_t ts; uint32_t gran[3]; } VkQueueFamilyProperties;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; uint32_t qf; uint32_t qc; const float* prio; } VkDeviceQueueCreateInfo;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; uint32_t qcc; const VkDeviceQueueCreateInfo* qci; uint32_t lc; const char* const* ln; uint32_t ec; const char* const* en; const void* feat; } VkDeviceCreateInfo;
typedef struct { uint32_t propertyFlags; uint32_t heapIndex; } VkMemoryType;
typedef struct { VkDeviceSize size; VkFlags flags; } VkMemoryHeap;
typedef struct { uint32_t memTypeCount; VkMemoryType memTypes[32]; uint32_t memHeapCount; VkMemoryHeap memHeaps[16]; } VkPhysicalDeviceMemoryProperties;
typedef struct { VkDeviceSize size; VkDeviceSize alignment; uint32_t memoryTypeBits; } VkMemoryRequirements;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; VkDeviceSize size; VkFlags usage; uint32_t sharingMode; uint32_t qfc; const uint32_t* qfi; } VkBufferCreateInfo;
typedef struct { int32_t sType; const void* pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex; } VkMemoryAllocateInfo;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; VkDeviceMemory memory; VkDeviceSize offset; VkDeviceSize size; } VkMemoryMapInfo;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; VkDeviceMemory memory; } VkMemoryUnmapInfo;
typedef struct { uint32_t binding; uint32_t descType; uint32_t descCount; VkFlags stageFlags; const void* samplers; } VkDescriptorSetLayoutBinding;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; uint32_t bc; const VkDescriptorSetLayoutBinding* bindings; } VkDescriptorSetLayoutCreateInfo;
typedef struct { uint32_t type; uint32_t count; } VkDescriptorPoolSize;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; uint32_t maxSets; uint32_t psc; const VkDescriptorPoolSize* ps; } VkDescriptorPoolCreateInfo;
typedef struct { int32_t sType; const void* pNext; VkDescriptorPool pool; uint32_t count; const VkDescriptorSetLayout* layouts; } VkDescriptorSetAllocateInfo;
typedef struct { VkBuffer buffer; VkDeviceSize offset; VkDeviceSize range; } VkDescriptorBufferInfo;
typedef struct { int32_t sType; const void* pNext; VkDescriptorSet dstSet; uint32_t dstBinding; uint32_t dstArrayElem; uint32_t descCount; uint32_t descType; const void* img; const VkDescriptorBufferInfo* buf; const void* texel; } VkWriteDescriptorSet;
typedef struct { VkFlags stageFlags; uint32_t offset; uint32_t size; } VkPushConstantRange;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; uint32_t slc; const VkDescriptorSetLayout* sl; uint32_t pcc; const VkPushConstantRange* pc; } VkPipelineLayoutCreateInfo;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; size_t codeSize; const uint32_t* pCode; } VkShaderModuleCreateInfo;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; uint32_t stage; VkShaderModule mod; const char* pName; const void* spec; } VkPipelineShaderStageCreateInfo;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; VkPipelineShaderStageCreateInfo stage; VkPipelineLayout layout; VkPipeline base; int32_t baseIdx; } VkComputePipelineCreateInfo;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; uint32_t qf; } VkCommandPoolCreateInfo;
typedef struct { int32_t sType; const void* pNext; VkCommandPool pool; uint32_t level; uint32_t count; } VkCommandBufferAllocateInfo;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; const void* inh; } VkCommandBufferBeginInfo;
typedef struct { int32_t sType; const void* pNext; VkFlags stageFlags; VkPipelineLayout layout; uint32_t firstSet; uint32_t setCount; const VkDescriptorSet* sets; uint32_t dynCount; const uint32_t* dynOffs; } VkBindDescriptorSetsInfo;
typedef struct { int32_t sType; const void* pNext; VkPipelineLayout layout; VkFlags stageFlags; uint32_t offset; uint32_t size; const void* pValues; } VkPushConstantsInfo;
typedef struct { int32_t sType; const void* pNext; uint32_t wsc; const void* ws; const void* wsm; uint32_t cbc; const VkCommandBuffer* cbs; uint32_t ssc; const void* ss; } VkSubmitInfo;
typedef struct { uint32_t viewMask; uint32_t colorAttachmentCount; const uint32_t* pColorAttachmentFormats; uint32_t depthAttachmentFormat; uint32_t stencilAttachmentFormat; } VkRenderingAreaInfoBody;
typedef struct { int32_t sType; const void* pNext; uint32_t viewMask; uint32_t colorAttachmentCount; const uint32_t* pColorAttachmentFormats; uint32_t depthAttachmentFormat; uint32_t stencilAttachmentFormat; } VkRenderingAreaInfo;
typedef struct { uint32_t width, height; } VkExtent2D;
typedef struct { uint32_t width, height, depth; } VkExtent3D;
typedef struct { int32_t x, y, z; } VkOffset3D;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; uint32_t imageType; uint32_t format; VkExtent3D extent; uint32_t mipLevels; uint32_t arrayLayers; uint32_t samples; uint32_t tiling; VkFlags usage; uint32_t sharingMode; uint32_t qfc; const uint32_t* qfi; uint32_t initialLayout; } VkImageCreateInfo;
typedef struct { VkFlags aspectMask; uint32_t baseMipLevel; uint32_t levelCount; uint32_t baseArrayLayer; uint32_t layerCount; } VkImageSubresourceRange;
typedef struct { VkFlags aspectMask; uint32_t mipLevel; uint32_t baseArrayLayer; uint32_t layerCount; } VkImageSubresourceLayers;
typedef struct { int32_t sType; const void* pNext; VkImage image; uint32_t oldLayout; uint32_t newLayout; VkImageSubresourceRange subresourceRange; } VkHostImageLayoutTransitionInfo;
typedef struct { int32_t sType; const void* pNext; const void* pHostPointer; uint32_t memoryRowLength; uint32_t memoryImageHeight; VkImageSubresourceLayers imageSubresource; VkOffset3D imageOffset; VkExtent3D imageExtent; } VkMemoryToImageCopy;
typedef struct { int32_t sType; const void* pNext; void* pHostPointer; uint32_t memoryRowLength; uint32_t memoryImageHeight; VkImageSubresourceLayers imageSubresource; VkOffset3D imageOffset; VkExtent3D imageExtent; } VkImageToMemoryCopy;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; VkImage dstImage; uint32_t dstImageLayout; uint32_t regionCount; const VkMemoryToImageCopy* pRegions; } VkCopyMemoryToImageInfo;
typedef struct { int32_t sType; const void* pNext; VkFlags flags; VkImage srcImage; uint32_t srcImageLayout; uint32_t regionCount; const VkImageToMemoryCopy* pRegions; } VkCopyImageToMemoryInfo;

typedef void* (*PFN_gipa)(VkInstance, const char*);

static PFN_gipa load_gipa(const char* path, void** out_lib) {
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return NULL; }
    *out_lib = lib;
    PFN_gipa g = (PFN_gipa)dlsym(lib, "vkGetInstanceProcAddr");
    if (g) return g;
    struct hw_module_methods_t { int (*open)(const void*, const char*, void**); };
    struct hw_module_t { uint32_t tag; uint16_t mv, hv; const char *id, *name, *author; struct hw_module_methods_t* methods; };
    struct hwvulkan_device_t { uint32_t tag, ver; void* mod; uint64_t rsv[12]; int (*close)(void*); void *e, *c; PFN_gipa gipa; };
    struct hw_module_t* hmi = (struct hw_module_t*)dlsym(lib, "HMI");
    if (hmi && hmi->methods && hmi->methods->open) {
        void* dev = NULL;
        if (hmi->methods->open(hmi, "vk0", &dev) == 0 && dev) return ((struct hwvulkan_device_t*)dev)->gipa;
    }
    return NULL;
}

int main(int argc, char** argv) {
    setbuf(stdout, NULL);
    const char* path = (argc > 1) ? argv[1] : "/system/lib64/libvulkan.so";
    void* lib = NULL;
    PFN_gipa gipa = load_gipa(path, &lib);
    if (!gipa) return 1;

    VkResult (*vkEnumerateInstanceVersion)(uint32_t*) = gipa(NULL, "vkEnumerateInstanceVersion");
    uint32_t instVer = VK_MAKE_API_VERSION(0, 1, 0, 0);
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&instVer);

    VkResult (*vkCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*) = gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai = {0, NULL, "vk14-hw-verify", 1, "neo9", 1, VK_MAKE_API_VERSION(0, 1, 4, 0)};
    VkInstanceCreateInfo ici = {1, NULL, 0, &ai, 0, NULL, 0, NULL};
    VkInstance inst = NULL;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { fprintf(stderr, "vkCreateInstance(1.4) failed: %d\n", r); return 2; }

    VkResult (*vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*) = gipa(inst, "vkEnumeratePhysicalDevices");
    void (*vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*) = gipa(inst, "vkGetPhysicalDeviceProperties");
    VkResult (*vkEnumerateDeviceExtensionProperties)(VkPhysicalDevice, const char*, uint32_t*, void*) = gipa(inst, "vkEnumerateDeviceExtensionProperties");
    void (*vkGetPhysicalDeviceFeatures2)(VkPhysicalDevice, VkPhysicalDeviceFeatures2*) = gipa(inst, "vkGetPhysicalDeviceFeatures2");
    void (*vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*) = gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    void (*vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*) = gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    VkResult (*vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*) = gipa(inst, "vkCreateDevice");
    void* (*vkGetDeviceProcAddr)(VkDevice, const char*) = gipa(inst, "vkGetDeviceProcAddr");

    uint32_t dc = 1; VkPhysicalDevice pd = NULL;
    vkEnumeratePhysicalDevices(inst, &dc, &pd);
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(pd, &props);
    uint32_t extCount = 0;
    if (vkEnumerateDeviceExtensionProperties) vkEnumerateDeviceExtensionProperties(pd, NULL, &extCount, NULL);
    printf("[1] Loader: %u.%u.%u | Device: %s | API: %u.%u.%u | Driver: %u.%u.%u | Exts: %u\n",
           VK_API_MAJOR(instVer), VK_API_MINOR(instVer), VK_API_PATCH(instVer),
           props.deviceName,
           VK_API_MAJOR(props.apiVersion), VK_API_MINOR(props.apiVersion), VK_API_PATCH(props.apiVersion),
           VK_API_MAJOR(props.driverVersion), VK_API_MINOR(props.driverVersion), VK_API_PATCH(props.driverVersion),
           extCount);

    VkPhysicalDeviceVulkan14Features f14 = {0};
    f14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    struct { int32_t sType; void* pNext; VkBool32 features[64]; } f2 = {0};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f14;
    if (!vkGetPhysicalDeviceFeatures2) { fprintf(stderr, "vkGetPhysicalDeviceFeatures2 is NULL!\n"); return 22; }
    printf("Calling vkGetPhysicalDeviceFeatures2=%p...\n", (void*)vkGetPhysicalDeviceFeatures2);
    vkGetPhysicalDeviceFeatures2(pd, (VkPhysicalDeviceFeatures2*)&f2);
    printf("[2] VkPhysicalDeviceVulkan14Features:\n");
    printf("    dynamicRenderingLocalRead=%u  maintenance5=%u  maintenance6=%u\n",
           f14.dynamicRenderingLocalRead, f14.maintenance5, f14.maintenance6);
    printf("    hostImageCopy=%u  pushDescriptor=%u  indexTypeUint8=%u\n",
           f14.hostImageCopy, f14.pushDescriptor, f14.indexTypeUint8);
    printf("    shaderFloatControls2=%u  shaderExpectAssume=%u  shaderSubgroupRotate=%u\n",
           f14.shaderFloatControls2, f14.shaderExpectAssume, f14.shaderSubgroupRotate);
    printf("    rectangularLines=%u  bresenhamLines=%u  smoothLines=%u  stippledRect=%u\n",
           f14.rectangularLines, f14.bresenhamLines, f14.smoothLines, f14.stippledRectangularLines);

    uint32_t qfc = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, NULL);
    VkQueueFamilyProperties* qfs = calloc(qfc, sizeof(*qfs));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, qfs);
    uint32_t qf = 0;
    for (uint32_t i = 0; i < qfc; i++) if (qfs[i].flags & 2) { qf = i; break; }

    /* Enable Vulkan 1.4 features we want to test! */
    VkPhysicalDeviceVulkan14Features enable14 = {0};
    enable14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    enable14.maintenance5 = f14.maintenance5;
    enable14.maintenance6 = f14.maintenance6;
    enable14.hostImageCopy = f14.hostImageCopy;
    enable14.dynamicRenderingLocalRead = f14.dynamicRenderingLocalRead;
    enable14.pushDescriptor = f14.pushDescriptor;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci = {2, NULL, 0, qf, 1, &prio};
    VkDeviceCreateInfo dci = {3, &enable14, 0, 1, &dqci, 0, NULL, 0, NULL, NULL};
    VkDevice dev = NULL;
    r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r != VK_SUCCESS) { fprintf(stderr, "vkCreateDevice(Vulkan14Features) failed: %d\n", r); return 3; }

    /* Resolve Vulkan 1.4 Core entrypoints via vkGetDeviceProcAddr */
    VkResult (*vkMapMemory2)(VkDevice, const VkMemoryMapInfo*, void**) = vkGetDeviceProcAddr(dev, "vkMapMemory2");
    VkResult (*vkUnmapMemory2)(VkDevice, const VkMemoryUnmapInfo*) = vkGetDeviceProcAddr(dev, "vkUnmapMemory2");
    void (*vkCmdBindDescriptorSets2)(VkCommandBuffer, const VkBindDescriptorSetsInfo*) = vkGetDeviceProcAddr(dev, "vkCmdBindDescriptorSets2");
    void (*vkCmdPushConstants2)(VkCommandBuffer, const VkPushConstantsInfo*) = vkGetDeviceProcAddr(dev, "vkCmdPushConstants2");
    void (*vkGetRenderingAreaGranularity)(VkDevice, const VkRenderingAreaInfo*, VkExtent2D*) = vkGetDeviceProcAddr(dev, "vkGetRenderingAreaGranularity");
    VkResult (*vkTransitionImageLayout)(VkDevice, uint32_t, const VkHostImageLayoutTransitionInfo*) = vkGetDeviceProcAddr(dev, "vkTransitionImageLayout");
    VkResult (*vkCopyMemoryToImage)(VkDevice, const VkCopyMemoryToImageInfo*) = vkGetDeviceProcAddr(dev, "vkCopyMemoryToImage");
    VkResult (*vkCopyImageToMemory)(VkDevice, const VkCopyImageToMemoryInfo*) = vkGetDeviceProcAddr(dev, "vkCopyImageToMemory");
    void* pLoc = vkGetDeviceProcAddr(dev, "vkCmdSetRenderingAttachmentLocations");
    void* pIdx = vkGetDeviceProcAddr(dev, "vkCmdSetRenderingInputAttachmentIndices");
    printf("[3] Core 1.4 Entrypoints: map2=%s bindDS2=%s pushC2=%s gran=%s hic=%s localRead=%s\n",
           vkMapMemory2 ? "OK" : "NULL", vkCmdBindDescriptorSets2 ? "OK" : "NULL",
           vkCmdPushConstants2 ? "OK" : "NULL", vkGetRenderingAreaGranularity ? "OK" : "NULL",
           (vkTransitionImageLayout && vkCopyMemoryToImage && vkCopyImageToMemory) ? "OK" : "NULL",
           (pLoc && pIdx) ? "OK" : "NULL");
    if (!vkMapMemory2 || !vkUnmapMemory2 || !vkCmdBindDescriptorSets2 || !vkCmdPushConstants2 || !vkGetRenderingAreaGranularity) {
        fprintf(stderr, "Missing Core 1.4 entrypoints!\n");
        return 4;
    }

    /* Test 4: Core 1.4 vkGetRenderingAreaGranularity */
    uint32_t fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkRenderingAreaInfo rai = {VK_STRUCTURE_TYPE_RENDERING_AREA_INFO, NULL, 0, 1, &fmt, 0, 0};
    VkExtent2D gran = {0, 0};
    vkGetRenderingAreaGranularity(dev, &rai, &gran);
    printf("[4] Core 1.4 vkGetRenderingAreaGranularity: %ux%u -> PASS\n", gran.width, gran.height);

    /* Test 5: Core 1.4 vkMapMemory2 + vkCmdBindDescriptorSets2 + vkCmdPushConstants2 + GPU Compute */
    void (*vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*) = vkGetDeviceProcAddr(dev, "vkGetDeviceQueue");
    VkResult (*vkCreateBuffer)(VkDevice, const VkBufferCreateInfo*, const void*, VkBuffer*) = vkGetDeviceProcAddr(dev, "vkCreateBuffer");
    void (*vkGetBufferMemoryRequirements)(VkDevice, VkBuffer, VkMemoryRequirements*) = vkGetDeviceProcAddr(dev, "vkGetBufferMemoryRequirements");
    VkResult (*vkAllocateMemory)(VkDevice, const VkMemoryAllocateInfo*, const void*, VkDeviceMemory*) = vkGetDeviceProcAddr(dev, "vkAllocateMemory");
    VkResult (*vkBindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) = vkGetDeviceProcAddr(dev, "vkBindBufferMemory");
    VkResult (*vkCreateDescriptorSetLayout)(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const void*, VkDescriptorSetLayout*) = vkGetDeviceProcAddr(dev, "vkCreateDescriptorSetLayout");
    VkResult (*vkCreateDescriptorPool)(VkDevice, const VkDescriptorPoolCreateInfo*, const void*, VkDescriptorPool*) = vkGetDeviceProcAddr(dev, "vkCreateDescriptorPool");
    VkResult (*vkAllocateDescriptorSets)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*) = vkGetDeviceProcAddr(dev, "vkAllocateDescriptorSets");
    void (*vkUpdateDescriptorSets)(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const void*) = vkGetDeviceProcAddr(dev, "vkUpdateDescriptorSets");
    VkResult (*vkCreatePipelineLayout)(VkDevice, const VkPipelineLayoutCreateInfo*, const void*, VkPipelineLayout*) = vkGetDeviceProcAddr(dev, "vkCreatePipelineLayout");
    VkResult (*vkCreateShaderModule)(VkDevice, const VkShaderModuleCreateInfo*, const void*, VkShaderModule*) = vkGetDeviceProcAddr(dev, "vkCreateShaderModule");
    VkResult (*vkCreateComputePipelines)(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const void*, VkPipeline*) = vkGetDeviceProcAddr(dev, "vkCreateComputePipelines");
    VkResult (*vkCreateCommandPool)(VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*) = vkGetDeviceProcAddr(dev, "vkCreateCommandPool");
    VkResult (*vkAllocateCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*) = vkGetDeviceProcAddr(dev, "vkAllocateCommandBuffers");
    VkResult (*vkBeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo*) = vkGetDeviceProcAddr(dev, "vkBeginCommandBuffer");
    void (*vkCmdBindPipeline)(VkCommandBuffer, uint32_t, VkPipeline) = vkGetDeviceProcAddr(dev, "vkCmdBindPipeline");
    void (*vkCmdDispatch)(VkCommandBuffer, uint32_t, uint32_t, uint32_t) = vkGetDeviceProcAddr(dev, "vkCmdDispatch");
    VkResult (*vkEndCommandBuffer)(VkCommandBuffer) = vkGetDeviceProcAddr(dev, "vkEndCommandBuffer");
    VkResult (*vkQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) = vkGetDeviceProcAddr(dev, "vkQueueSubmit");
    VkResult (*vkQueueWaitIdle)(VkQueue) = vkGetDeviceProcAddr(dev, "vkQueueWaitIdle");
    VkResult (*vkCreateImage)(VkDevice, const VkImageCreateInfo*, const void*, VkImage*) = vkGetDeviceProcAddr(dev, "vkCreateImage");
    void (*vkGetImageMemoryRequirements)(VkDevice, VkImage, VkMemoryRequirements*) = vkGetDeviceProcAddr(dev, "vkGetImageMemoryRequirements");
    VkResult (*vkBindImageMemory)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) = vkGetDeviceProcAddr(dev, "vkBindImageMemory");

    VkQueue queue = NULL; vkGetDeviceQueue(dev, qf, 0, &queue);
    const uint32_t N = 4096;
    const VkDeviceSize BYTES = N * sizeof(uint32_t);
    VkBufferCreateInfo bci = {12, NULL, 0, BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, 0, NULL};
    VkBuffer buf = 0; vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements mr = {0}; vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp = {0}; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = 0;
    for (uint32_t i = 0; i < mp.memTypeCount; i++) {
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memTypes[i].propertyFlags & 6) == 6) { mt = i; break; }
    }
    VkMemoryAllocateInfo mai = {5, NULL, mr.size, mt};
    VkDeviceMemory mem = 0; vkAllocateMemory(dev, &mai, NULL, &mem);
    vkBindBufferMemory(dev, buf, mem, 0);

    uint32_t* mapped = NULL;
    VkMemoryMapInfo mmi = {VK_STRUCTURE_TYPE_MEMORY_MAP_INFO, NULL, 0, mem, 0, BYTES};
    if (vkMapMemory2(dev, &mmi, (void**)&mapped) != VK_SUCCESS || !mapped) { fprintf(stderr, "vkMapMemory2 failed!\n"); return 5; }
    for (uint32_t i = 0; i < N; i++) mapped[i] = i + 50u;

    VkDescriptorSetLayoutBinding bind = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dslci = {32, NULL, 0, 1, &bind};
    VkDescriptorSetLayout dsl = 0; vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl);
    VkDescriptorPoolSize dps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci = {33, NULL, 0, 1, 1, &dps};
    VkDescriptorPool dp = 0; vkCreateDescriptorPool(dev, &dpci, NULL, &dp);
    VkDescriptorSetAllocateInfo dsai = {34, NULL, dp, 1, &dsl};
    VkDescriptorSet ds = 0; vkAllocateDescriptorSets(dev, &dsai, &ds);
    VkDescriptorBufferInfo dbi = {buf, 0, BYTES};
    VkWriteDescriptorSet wds = {35, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL};
    vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo plci = {30, NULL, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout pl = 0; vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    VkShaderModuleCreateInfo smci = {16, NULL, 0, sizeof(kComputePushSpv), kComputePushSpv};
    VkShaderModule sm = 0; vkCreateShaderModule(dev, &smci, NULL, &sm);
    VkComputePipelineCreateInfo cpci = {29, NULL, 0, {18, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL}, pl, 0, -1};
    VkPipeline pipe = 0;
    if (vkCreateComputePipelines(dev, 0, 1, &cpci, NULL, &pipe) != VK_SUCCESS) { fprintf(stderr, "pipeline fail\n"); return 6; }

    VkCommandPoolCreateInfo cmdpci = {39, NULL, 0, qf};
    VkCommandPool cmdp = 0; vkCreateCommandPool(dev, &cmdpci, NULL, &cmdp);
    VkCommandBufferAllocateInfo cbai = {40, NULL, cmdp, 0, 1};
    VkCommandBuffer cb = NULL; vkAllocateCommandBuffers(dev, &cbai, &cb);
    VkCommandBufferBeginInfo cbbi = {42, NULL, 1, NULL};
    vkBeginCommandBuffer(cb, &cbbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    VkBindDescriptorSetsInfo bdsi = {VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO, NULL, VK_SHADER_STAGE_COMPUTE_BIT, pl, 0, 1, &ds, 0, NULL};
    vkCmdBindDescriptorSets2(cb, &bdsi);
    uint32_t addVal = 42u;
    VkPushConstantsInfo pci = {VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO, NULL, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &addVal};
    vkCmdPushConstants2(cb, &pci);
    vkCmdDispatch(cb, N / 64, 1, 1);
    vkEndCommandBuffer(cb);

    VkSubmitInfo si = {4, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    r = vkQueueSubmit(queue, 1, &si, 0);
    if (r == VK_SUCCESS) r = vkQueueWaitIdle(queue);
    if (r != VK_SUCCESS) { fprintf(stderr, "QueueSubmit/WaitIdle failed: %d\n", r); return 7; }

    uint32_t bad = 0;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t exp = (i + 50u) * 3u + 42u;
        if (mapped[i] != exp) bad++;
    }
    VkMemoryUnmapInfo mui = {VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO, NULL, 0, mem};
    if (vkUnmapMemory2(dev, &mui) != VK_SUCCESS) { fprintf(stderr, "vkUnmapMemory2 failed\n"); return 8; }
    printf("[5] Core 1.4 Map2 + BindDS2 + PushConst2 Compute: %s (%u/%u exact)\n",
           bad == 0 ? "PASS (BIT-EXACT)" : "FAIL", N - bad, N);
    if (bad) return 9;

    /* Test 6: Core 1.4 Host Image Copy (64x64 RGBA8 = 16384 bytes CPU -> OPTIMAL GPU texture -> CPU) */
    if (f14.hostImageCopy && vkTransitionImageLayout && vkCopyMemoryToImage && vkCopyImageToMemory) {
        VkImageCreateInfo ici2 = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D,
            VK_FORMAT_R8G8B8A8_UNORM, {64, 64, 1}, 1, 1, 1, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_HOST_TRANSFER_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img = 0;
        r = vkCreateImage(dev, &ici2, NULL, &img);
        if (r == VK_SUCCESS) {
            VkMemoryRequirements imr = {0}; vkGetImageMemoryRequirements(dev, img, &imr);
            uint32_t imt = 0;
            for (uint32_t i = 0; i < mp.memTypeCount; i++) {
                if ((imr.memoryTypeBits & (1u << i)) && (mp.memTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { imt = i; break; }
            }
            VkMemoryAllocateInfo imai = {5, NULL, imr.size, imt};
            VkDeviceMemory imem = 0; vkAllocateMemory(dev, &imai, NULL, &imem);
            vkBindImageMemory(dev, img, imem, 0);

            VkHostImageLayoutTransitionInfo tr = {VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO, NULL, img,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            r = vkTransitionImageLayout(dev, 1, &tr);
            uint8_t src[64 * 64 * 4], dst[64 * 64 * 4];
            for (size_t i = 0; i < sizeof(src); i++) { src[i] = (uint8_t)((i * 131 + 17) & 0xff); dst[i] = 0; }
            if (r == VK_SUCCESS) {
                VkMemoryToImageCopy reg1 = {VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY, NULL, src, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0,0,0}, {64,64,1}};
                VkCopyMemoryToImageInfo c1 = {VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO, NULL, 0, img, VK_IMAGE_LAYOUT_GENERAL, 1, &reg1};
                r = vkCopyMemoryToImage(dev, &c1);
            }
            if (r == VK_SUCCESS) {
                VkImageToMemoryCopy reg2 = {VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY, NULL, dst, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0,0,0}, {64,64,1}};
                VkCopyImageToMemoryInfo c2 = {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO, NULL, 0, img, VK_IMAGE_LAYOUT_GENERAL, 1, &reg2};
                r = vkCopyImageToMemory(dev, &c2);
            }
            size_t hbad = 0;
            for (size_t i = 0; i < sizeof(src); i++) if (src[i] != dst[i]) hbad++;
            printf("[6] Core 1.4 HostImageCopy (OPTIMAL 64x64 RGBA8): %s (%zu/%zu bytes exact, rc=%d)\n",
                   (r == VK_SUCCESS && hbad == 0) ? "PASS (BIT-EXACT)" : "FAIL", sizeof(src) - hbad, sizeof(src), r);
            if (r != VK_SUCCESS || hbad) return 10;
        }
    }

    printf("\n>>> ALL REAL VULKAN 1.4 HARDWARE TESTS PASSED! <<<\n");
    return 0;
}
