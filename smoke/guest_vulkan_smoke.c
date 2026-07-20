#define _POSIX_C_SOURCE 200809L
#include <vulkan/vulkan.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct probe_state {
    const char *run_id;
    const char *test_id;
    const char *result_path;
    uint32_t loader_api;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures features;
    uint32_t queue_family;
    int buffer_copy_ok;
    int image_clear_ok;
    int bc1;
    int bc2;
    int bc3;
    int bc4;
    int bc5;
    int bc6;
    int bc7;
    int descriptor_indexing;
    int scalar_block_layout;
    int robustness2;
    int transform_feedback;
    int shader_int8;
    int timeline_semaphore;
    int synchronization2;
    int dynamic_rendering;
    int maintenance4;
    int maintenance5;
    int maintenance6;
    int present_wait;
    int swapchain_maintenance;
    int fallback_detected;
    uint64_t started_ms;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static const char *argument_value(int argc, char **argv, const char *name, const char *fallback)
{
    int i;
    for (i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return fallback;
}

static void json_safe_copy(char *output, size_t output_size, const char *input)
{
    size_t written = 0;
    if (!output_size) return;
    while (input && *input && written + 1 < output_size) {
        unsigned char ch = (unsigned char)*input++;
        if (ch == '"' || ch == '\\' || ch < 0x20 || ch > 0x7e) ch = '_';
        output[written++] = (char)ch;
    }
    output[written] = 0;
}

static void version_text(uint32_t version, char *buffer, size_t size)
{
    snprintf(buffer, size, "%u.%u.%u", VK_API_VERSION_MAJOR(version),
             VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
}

static void write_result(const struct probe_state *state, const char *status,
                         const char *stage, const char *message)
{
    char temporary[1200];
    char safe_message[512];
    char safe_device[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
    char loader_version[32];
    char device_version[32];
    const char *host_arch = getenv("WINEHUA_HOST_ARCH");
    FILE *file;
    int fd;

    if (!state->result_path || !state->result_path[0]) return;
    snprintf(temporary, sizeof(temporary), "%s.tmp.%d", state->result_path, getpid());
    json_safe_copy(safe_message, sizeof(safe_message), message ? message : "");
    json_safe_copy(safe_device, sizeof(safe_device), state->properties.deviceName);
    version_text(state->loader_api, loader_version, sizeof(loader_version));
    version_text(state->properties.apiVersion, device_version, sizeof(device_version));
    file = fopen(temporary, "w");
    if (!file) return;

    fprintf(file,
            "{\n"
            "  \"schemaVersion\": 1,\n"
            "  \"runId\": \"%s\",\n"
            "  \"testId\": \"%s\",\n"
            "  \"status\": \"%s\",\n"
            "  \"stage\": \"%s\",\n"
            "  \"message\": \"%s\",\n"
            "  \"pid\": %d,\n"
            "  \"heartbeatTimestampMs\": %llu,\n"
            "  \"architecture\": {\"peArchitecture\":\"not-applicable\","
            "\"wineUnixArchitecture\":\"not-applicable\","
            "\"vulkanLoaderArchitecture\":\"x86_64\","
            "\"venusIcdArchitecture\":\"x86_64\","
            "\"hostArchitecture\":\"%s\","
            "\"wow64ThunkEnabled\":false,\"box64Enabled\":true},\n"
            "  \"transport\": {\"renderer\":\"venus-vtest\","
            "\"vtestSocket\":\"%s\",\"box64HostVulkanWrapperDisabled\":true},\n"
            "  \"capabilities\": {\"loaderApiVersion\":\"%s\","
            "\"deviceApiVersion\":\"%s\",\"deviceName\":\"%s\","
            "\"vendorId\":%u,\"deviceId\":%u,\"driverVersion\":%u,"
            "\"graphicsQueueFamily\":%u,\"pushConstantBytes\":%u,"
            "\"geometryShader\":%s,\"tessellationShader\":%s,"
            "\"multiDrawIndirect\":%s,\"descriptorIndexing\":%s,"
            "\"scalarBlockLayout\":%s,\"robustness2\":%s,"
            "\"transformFeedback\":%s,\"shaderInt8\":%s,"
            "\"shaderInt16\":%s,\"shaderInt64\":%s,"
            "\"timelineSemaphore\":%s,\"synchronization2\":%s,"
            "\"dynamicRendering\":%s,\"maintenance4\":%s,"
            "\"maintenance5\":%s,\"maintenance6\":%s,"
            "\"presentWait\":%s,\"swapchainMaintenance\":%s,"
            "\"bc1\":%s,\"bc2\":%s,\"bc3\":%s,\"bc4\":%s,"
            "\"bc5\":%s,\"bc6\":%s,\"bc7\":%s},\n"
            "  \"checks\": {\"bufferCopy\":%s,\"imageClear\":%s},\n"
            "  \"metrics\": {\"cpuReadBytes\":4096,\"cpuUploadBytes\":4096,"
            "\"gpuCopyCount\":2,\"queueSubmitCount\":2,"
            "\"perFrameDeviceWaitIdle\":0,\"fallbackDetected\":%s,"
            "\"durationMs\":%llu}\n"
            "}\n",
            state->run_id, state->test_id, status, stage, safe_message, getpid(),
            (unsigned long long)now_ms(), host_arch && host_arch[0] ? host_arch : "aarch64",
            getenv("VTEST_SOCKET_NAME") ? getenv("VTEST_SOCKET_NAME") : "",
            loader_version, device_version, safe_device, state->properties.vendorID,
            state->properties.deviceID, state->properties.driverVersion, state->queue_family,
            state->properties.limits.maxPushConstantsSize,
            state->features.geometryShader ? "true" : "false",
            state->features.tessellationShader ? "true" : "false",
            state->features.multiDrawIndirect ? "true" : "false",
            state->descriptor_indexing ? "true" : "false",
            state->scalar_block_layout ? "true" : "false",
            state->robustness2 ? "true" : "false",
            state->transform_feedback ? "true" : "false",
            state->shader_int8 ? "true" : "false",
            state->features.shaderInt16 ? "true" : "false",
            state->features.shaderInt64 ? "true" : "false",
            state->timeline_semaphore ? "true" : "false",
            state->synchronization2 ? "true" : "false",
            state->dynamic_rendering ? "true" : "false",
            state->maintenance4 ? "true" : "false",
            state->maintenance5 ? "true" : "false",
            state->maintenance6 ? "true" : "false",
            state->present_wait ? "true" : "false",
            state->swapchain_maintenance ? "true" : "false",
            state->bc1 ? "true" : "false", state->bc2 ? "true" : "false",
            state->bc3 ? "true" : "false", state->bc4 ? "true" : "false",
            state->bc5 ? "true" : "false", state->bc6 ? "true" : "false",
            state->bc7 ? "true" : "false",
            state->buffer_copy_ok ? "true" : "false",
            state->image_clear_ok ? "true" : "false",
            state->fallback_detected ? "true" : "false",
            (unsigned long long)(now_ms() - state->started_ms));
    fflush(file);
    fd = fileno(file);
    if (fd >= 0) fsync(fd);
    fclose(file);
    rename(temporary, state->result_path);
}

static void checkpoint(const struct probe_state *state, const char *message)
{
    write_result(state, "started", "venus", message);
}

static int find_memory_type(VkPhysicalDevice physical, uint32_t bits,
                            VkMemoryPropertyFlags required, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & required) == required) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static VkResult create_buffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                              VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                              VkBuffer *buffer, VkDeviceMemory *memory)
{
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t type;
    VkResult result;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateBuffer(device, &info, NULL, buffer);
    if (result != VK_SUCCESS) return result;
    vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    if (!find_memory_type(physical, requirements.memoryTypeBits, properties, &type))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = vkAllocateMemory(device, &allocation, NULL, memory);
    if (result != VK_SUCCESS) return result;
    return vkBindBufferMemory(device, *buffer, *memory, 0);
}

static int format_supports(VkPhysicalDevice physical, VkFormat format,
                           VkFormatFeatureFlags flags)
{
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    return (properties.optimalTilingFeatures & flags) == flags;
}

static int has_extension(const VkExtensionProperties *extensions, uint32_t count,
                         const char *name)
{
    uint32_t i;
    for (i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, name)) return 1;
    return 0;
}

static int query_extended_capabilities(VkPhysicalDevice physical, struct probe_state *state)
{
    uint32_t extension_count = 0;
    VkExtensionProperties *extensions = NULL;
    VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan12Features vulkan12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
    VkPhysicalDeviceTransformFeedbackFeaturesEXT transform_feedback = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT };
    VkPhysicalDeviceSynchronization2Features synchronization2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
    VkPhysicalDeviceMaintenance4Features maintenance4 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES };
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR };
    VkPhysicalDeviceMaintenance6FeaturesKHR maintenance6 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR };
    void **tail = &features2.pNext;
    int api12 = state->properties.apiVersion >= VK_API_VERSION_1_2;
    int api13 = state->properties.apiVersion >= VK_API_VERSION_1_3;
    int has_robustness2, has_transform_feedback, has_synchronization2;
    int has_dynamic_rendering, has_maintenance4, has_maintenance5, has_maintenance6;

    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, NULL) != VK_SUCCESS)
        return 0;
    extensions = calloc(extension_count ? extension_count : 1, sizeof(*extensions));
    if (!extensions) return 0;
    if (extension_count &&
        vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, extensions) != VK_SUCCESS) {
        free(extensions);
        return 0;
    }

    has_robustness2 = has_extension(extensions, extension_count, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    has_transform_feedback = has_extension(extensions, extension_count, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    has_synchronization2 = api13 || has_extension(extensions, extension_count, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    has_dynamic_rendering = api13 || has_extension(extensions, extension_count, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    has_maintenance4 = api13 || has_extension(extensions, extension_count, VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    has_maintenance5 = has_extension(extensions, extension_count, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    has_maintenance6 = has_extension(extensions, extension_count, VK_KHR_MAINTENANCE_6_EXTENSION_NAME);

#define APPEND_FEATURE(feature, supported) do { \
    if (supported) { *tail = &(feature); tail = &(feature).pNext; } \
} while (0)
    APPEND_FEATURE(vulkan12, api12);
    APPEND_FEATURE(robustness2, has_robustness2);
    APPEND_FEATURE(transform_feedback, has_transform_feedback);
    APPEND_FEATURE(synchronization2, has_synchronization2);
    APPEND_FEATURE(dynamic_rendering, has_dynamic_rendering);
    APPEND_FEATURE(maintenance4, has_maintenance4);
    APPEND_FEATURE(maintenance5, has_maintenance5);
    APPEND_FEATURE(maintenance6, has_maintenance6);
#undef APPEND_FEATURE
    vkGetPhysicalDeviceFeatures2(physical, &features2);

    state->descriptor_indexing = api12 && vulkan12.descriptorIndexing;
    state->scalar_block_layout = api12 && vulkan12.scalarBlockLayout;
    state->shader_int8 = api12 && vulkan12.shaderInt8;
    state->timeline_semaphore = api12 && vulkan12.timelineSemaphore;
    state->robustness2 = has_robustness2 && robustness2.robustBufferAccess2;
    state->transform_feedback = has_transform_feedback && transform_feedback.transformFeedback;
    state->synchronization2 = has_synchronization2 && synchronization2.synchronization2;
    state->dynamic_rendering = has_dynamic_rendering && dynamic_rendering.dynamicRendering;
    state->maintenance4 = has_maintenance4 && maintenance4.maintenance4;
    state->maintenance5 = has_maintenance5 && maintenance5.maintenance5;
    state->maintenance6 = has_maintenance6 && maintenance6.maintenance6;
    state->present_wait = has_extension(extensions, extension_count, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    state->swapchain_maintenance = has_extension(extensions, extension_count,
                                                 VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    state->bc1 = format_supports(physical, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc2 = format_supports(physical, VK_FORMAT_BC2_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc3 = format_supports(physical, VK_FORMAT_BC3_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc4 = format_supports(physical, VK_FORMAT_BC4_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc5 = format_supports(physical, VK_FORMAT_BC5_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc6 = format_supports(physical, VK_FORMAT_BC6H_UFLOAT_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    state->bc7 = format_supports(physical, VK_FORMAT_BC7_UNORM_BLOCK,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    free(extensions);
    return 1;
}

int main(int argc, char **argv)
{
    struct probe_state state;
    VkApplicationInfo application = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer source = VK_NULL_HANDLE, destination = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory source_memory = VK_NULL_HANDLE, destination_memory = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE, image_memory = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkResult result;
    uint32_t count = 0, i;
    const char *failure = "unknown failure";
    void *mapped = NULL;
    VkDeviceMemory mapped_memory = VK_NULL_HANDLE;
    int exit_code = 1;

    memset(&state, 0, sizeof(state));
    state.run_id = argument_value(argc, argv, "--run-id", "manual");
    state.test_id = argument_value(argc, argv, "--test-id", "venus-offscreen-x64");
    state.result_path = argument_value(argc, argv, "--result", "");
    state.started_ms = now_ms();
    state.queue_family = UINT32_MAX;
    state.loader_api = VK_API_VERSION_1_0;
    if (!getenv("VN_DEBUG") || !strstr(getenv("VN_DEBUG"), "vtest") ||
        !getenv("VTEST_SOCKET_NAME") || !getenv("VK_DRIVER_FILES") ||
        !getenv("BOX64_EMULATED_LIBS") ||
        !strstr(getenv("BOX64_EMULATED_LIBS"), "libvulkan.so.1") ||
        !strcmp(getenv("BOX64_NOVULKAN") ? getenv("BOX64_NOVULKAN") : "", "1")) {
        write_result(&state, "FAIL", "startup", "Venus vtest/ICD isolation environment is incomplete");
        return 2;
    }
    write_result(&state, "started", "venus", "Guest Vulkan offscreen probe started");

    {
        PFN_vkEnumerateInstanceVersion enumerate_version =
            (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE,
                                                                  "vkEnumerateInstanceVersion");
        if (enumerate_version) enumerate_version(&state.loader_api);
    }
    application.pApplicationName = "winehua_guest_vulkan_smoke";
    application.applicationVersion = 1;
    application.pEngineName = "WineHua";
    application.engineVersion = 1;
    application.apiVersion = VK_API_VERSION_1_1;
    instance_info.pApplicationInfo = &application;
    result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS) { failure = "vkCreateInstance failed"; goto cleanup; }
    checkpoint(&state, "vkCreateInstance passed");

    result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (result != VK_SUCCESS || !count) { failure = "Venus exposed no physical device"; goto cleanup; }
    {
        VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
        if (!devices) { failure = "physical device allocation failed"; goto cleanup; }
        result = vkEnumeratePhysicalDevices(instance, &count, devices);
        if (result == VK_SUCCESS) physical = devices[0];
        free(devices);
        if (result != VK_SUCCESS || !physical) { failure = "physical device enumeration failed"; goto cleanup; }
    }
    vkGetPhysicalDeviceProperties(physical, &state.properties);
    vkGetPhysicalDeviceFeatures(physical, &state.features);
    checkpoint(&state, "physical device enumeration passed");
    state.fallback_detected = strstr(state.properties.deviceName, "llvmpipe") != NULL ||
                              strstr(state.properties.deviceName, "softpipe") != NULL;
    if (!query_extended_capabilities(physical, &state)) {
        failure = "extended capability query failed";
        goto cleanup;
    }
    if (state.fallback_detected) { failure = "software Vulkan fallback detected"; goto cleanup; }

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    {
        VkQueueFamilyProperties *queues = calloc(count, sizeof(*queues));
        if (!queues) { failure = "queue family allocation failed"; goto cleanup; }
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
        for (i = 0; i < count; ++i)
            if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                state.queue_family = i;
                break;
            }
        free(queues);
    }
    if (state.queue_family == UINT32_MAX) { failure = "no graphics queue family"; goto cleanup; }

    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        queue_info.queueFamilyIndex = state.queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        result = vkCreateDevice(physical, &device_info, NULL, &device);
        if (result != VK_SUCCESS) { failure = "vkCreateDevice failed"; goto cleanup; }
    }
    vkGetDeviceQueue(device, state.queue_family, 0, &queue);
    checkpoint(&state, "vkCreateDevice and queue acquisition passed");

    {
        VkCommandPoolCreateInfo info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo allocation = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = state.queue_family;
        result = vkCreateCommandPool(device, &info, NULL, &pool);
        if (result != VK_SUCCESS) { failure = "vkCreateCommandPool failed"; goto cleanup; }
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device, &allocation, &command);
        if (result != VK_SUCCESS) { failure = "vkAllocateCommandBuffers failed"; goto cleanup; }
        result = vkCreateFence(device, &fence_info, NULL, &fence);
        if (result != VK_SUCCESS) { failure = "vkCreateFence failed"; goto cleanup; }
    }

    result = create_buffer(physical, device, 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &source, &source_memory);
    if (result != VK_SUCCESS) { failure = "source buffer creation failed"; goto cleanup; }
    result = create_buffer(physical, device, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &destination, &destination_memory);
    if (result != VK_SUCCESS) { failure = "destination buffer creation failed"; goto cleanup; }
    checkpoint(&state, "buffer allocations passed");
    result = vkMapMemory(device, source_memory, 0, 4096, 0, &mapped);
    if (result != VK_SUCCESS) { failure = "source buffer map failed"; goto cleanup; }
    mapped_memory = source_memory;
    for (i = 0; i < 4096; ++i) ((uint8_t *)mapped)[i] = (uint8_t)(i * 37u + 11u);
    vkUnmapMemory(device, source_memory);
    mapped = NULL;
    mapped_memory = VK_NULL_HANDLE;
    {
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkBufferCopy copy = { 0, 0, 4096 };
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        vkBeginCommandBuffer(command, &begin);
        vkCmdCopyBuffer(command, source, destination, 1, &copy);
        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) { failure = "buffer copy command recording failed"; goto cleanup; }
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        result = vkQueueSubmit(queue, 1, &submit, fence);
        if (result != VK_SUCCESS) { failure = "buffer copy submit failed"; goto cleanup; }
        checkpoint(&state, "buffer copy submitted; waiting for fence");
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
        if (result != VK_SUCCESS) { failure = "buffer copy fence timeout"; goto cleanup; }
        checkpoint(&state, "buffer copy fence passed");
    }
    result = vkMapMemory(device, destination_memory, 0, 4096, 0, &mapped);
    if (result != VK_SUCCESS) { failure = "destination buffer map failed"; goto cleanup; }
    mapped_memory = destination_memory;
    for (i = 0; i < 4096; ++i)
        if (((uint8_t *)mapped)[i] != (uint8_t)(i * 37u + 11u)) break;
    vkUnmapMemory(device, destination_memory);
    mapped = NULL;
    mapped_memory = VK_NULL_HANDLE;
    if (i != 4096) { failure = "buffer copy verification failed"; goto cleanup; }
    state.buffer_copy_ok = 1;

    {
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        VkMemoryRequirements requirements;
        VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        uint32_t type;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent.width = 16;
        info.extent.height = 16;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        result = vkCreateImage(device, &info, NULL, &image);
        if (result != VK_SUCCESS) { failure = "test image creation failed"; goto cleanup; }
        vkGetImageMemoryRequirements(device, image, &requirements);
        if (!find_memory_type(physical, requirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
            !find_memory_type(physical, requirements.memoryTypeBits, 0, &type)) {
            failure = "test image memory type unavailable";
            goto cleanup;
        }
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        result = vkAllocateMemory(device, &allocation, NULL, &image_memory);
        if (result != VK_SUCCESS ||
            vkBindImageMemory(device, image, image_memory, 0) != VK_SUCCESS) {
            failure = "test image memory allocation failed";
            goto cleanup;
        }
    }
    result = create_buffer(physical, device, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &readback, &readback_memory);
    if (result != VK_SUCCESS) { failure = "image readback buffer creation failed"; goto cleanup; }
    checkpoint(&state, "image and readback allocations passed");

    vkResetFences(device, 1, &fence);
    vkResetCommandPool(device, pool, 0);
    {
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        VkClearColorValue color = {{ 0.25f, 0.5f, 0.75f, 1.0f }};
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkBufferImageCopy copy;
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        memset(&copy, 0, sizeof(copy));
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = 16;
        copy.imageExtent.height = 16;
        copy.imageExtent.depth = 1;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkBeginCommandBuffer(command, &begin);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &color, 1, &range);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback, 1, &copy);
        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) { failure = "image clear command recording failed"; goto cleanup; }
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        result = vkQueueSubmit(queue, 1, &submit, fence);
        if (result != VK_SUCCESS) { failure = "image clear submit failed"; goto cleanup; }
        checkpoint(&state, "image clear submitted; waiting for fence");
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
        if (result != VK_SUCCESS) { failure = "image clear fence timeout"; goto cleanup; }
        checkpoint(&state, "image clear fence passed");
    }
    result = vkMapMemory(device, readback_memory, 0, 1024, 0, &mapped);
    if (result != VK_SUCCESS) { failure = "image readback map failed"; goto cleanup; }
    mapped_memory = readback_memory;
    {
        const uint8_t *pixel = mapped;
        int ok = pixel[0] >= 62 && pixel[0] <= 66 && pixel[1] >= 126 && pixel[1] <= 130 &&
                 pixel[2] >= 189 && pixel[2] <= 193 && pixel[3] >= 253;
        vkUnmapMemory(device, readback_memory);
        mapped = NULL;
        mapped_memory = VK_NULL_HANDLE;
        if (!ok) { failure = "image clear verification failed"; goto cleanup; }
    }
    state.image_clear_ok = 1;
    write_result(&state, "PASS", "venus", "Guest Venus buffer/image/queue/fence checks passed");
    exit_code = 0;

cleanup:
    if (exit_code) write_result(&state, "FAIL", "venus", failure);
    if (mapped && device && mapped_memory) vkUnmapMemory(device, mapped_memory);
    if (device && fence) vkDestroyFence(device, fence, NULL);
    if (device && pool) vkDestroyCommandPool(device, pool, NULL);
    if (device && readback) vkDestroyBuffer(device, readback, NULL);
    if (device && readback_memory) vkFreeMemory(device, readback_memory, NULL);
    if (device && image) vkDestroyImage(device, image, NULL);
    if (device && image_memory) vkFreeMemory(device, image_memory, NULL);
    if (device && destination) vkDestroyBuffer(device, destination, NULL);
    if (device && destination_memory) vkFreeMemory(device, destination_memory, NULL);
    if (device && source) vkDestroyBuffer(device, source, NULL);
    if (device && source_memory) vkFreeMemory(device, source_memory, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    return exit_code;
}
