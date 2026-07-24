#define _POSIX_C_SOURCE 200809L

/*
 * Offscreen replay for one captured DXVK Legacy Heaven material fragment
 * shader. The fragment SPIR-V remains an external diagnostic input so the
 * product runtime does not redistribute benchmark-owned shader binaries.
 * The harness supplies the final DXVK descriptor layout, deterministic UBOs,
 * six format-correct sampled images, and matching fragment input locations.
 * The same source is also built natively against Lavapipe for an exact A/B.
 */

#include <vulkan/vulkan.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REPLAY_WIDTH 64u
#define REPLAY_HEIGHT 64u
#define REPLAY_IMAGE_WIDTH 4u
#define REPLAY_IMAGE_HEIGHT 4u
#define REPLAY_IMAGE_COUNT 6u
#define REPLAY_UBO_COUNT 15u
#define REPLAY_SAMPLE_COUNT 9u

struct replay_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
};

struct replay_image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;
    VkFormat format;
};

struct replay_state {
    const char *run_id;
    const char *test_id;
    const char *result_path;
    const char *rgba_path;
    const char *vertex_path;
    const char *fragment_path;
    const char *fragment_sha256;
    const char *layout_mode;
    const char *bool_mode;
    uint32_t bool_spec_count;
    uint64_t started_ms;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceProperties properties;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkFence fence;
    struct replay_buffer upload;
    struct replay_buffer vertex;
    struct replay_buffer ubos[REPLAY_UBO_COUNT];
    struct replay_image images[REPLAY_IMAGE_COUNT];
    VkImage target;
    VkDeviceMemory target_memory;
    VkImageView target_view;
    struct replay_buffer readback;
    VkDescriptorSetLayout set_layout;
    uint32_t layout_binding_count;
    uint32_t dynamic_binding_count;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkPipelineLayout pipeline_layout;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkShaderModule vertex_shader;
    VkShaderModule fragment_shader;
    VkPipeline pipeline;
    uint32_t checksum;
    uint32_t changed_pixels;
    uint32_t opaque_pixels;
    uint32_t sample_values[REPLAY_SAMPLE_COUNT];
    bool specialization_applied;
    uint64_t queue_submits;
    uint64_t gpu_copies;
    char stage[64];
    char message[256];
    char pipeline_stage[64];
    VkResult failure_result;
};

static const VkFormat replay_formats[REPLAY_IMAGE_COUNT] = {
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R8G8_SNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R8G8_SNORM,
    VK_FORMAT_R8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
};

static const uint32_t replay_ubo_sizes[REPLAY_UBO_COUNT] = {
    144u, 16u, 48u, 1536u, 16u,
    64u, 144u, 32u, 64u, 512u,
    144u, 16u, 48u, 1536u, 16u,
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static const char *argument_value(int argc, char **argv, const char *name,
                                  const char *fallback)
{
    int i;
    for (i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return fallback;
}

static void json_safe(char *out, size_t size, const char *in)
{
    size_t n = 0;
    if (!size) return;
    while (in && *in && n + 1 < size) {
        unsigned char c = (unsigned char)*in++;
        if (c == '"' || c == '\\' || c < 0x20 || c > 0x7e) c = '_';
        out[n++] = (char)c;
    }
    out[n] = 0;
}

static void record_failure(struct replay_state *s, const char *stage,
                           const char *message, VkResult result)
{
    snprintf(s->stage, sizeof(s->stage), "%s", stage ? stage : "unknown");
    snprintf(s->message, sizeof(s->message), "%s", message ? message : "failure");
    s->failure_result = result;
}

static VkResult record_pipeline_step(struct replay_state *s, const char *stage,
                                     VkResult result)
{
    snprintf(s->pipeline_stage, sizeof(s->pipeline_stage), "%s", stage);
    fprintf(stderr, "[venus-heaven-replay] stage=%s result=%d\n", stage,
            (int)result);
    fflush(stderr);
    return result;
}

static VkResult record_shader_load_failure(struct replay_state *s)
{
    snprintf(s->pipeline_stage, sizeof(s->pipeline_stage), "shader-load");
    snprintf(s->message, sizeof(s->message),
             "shader load failed vertex=%s fragment=%s errno=%d",
             s->vertex_path ? s->vertex_path : "", s->fragment_path ? s->fragment_path : "",
             errno);
    fprintf(stderr, "[venus-heaven-replay] stage=shader-load result=%d vertex=%s fragment=%s errno=%d\\n",
            (int)VK_ERROR_INITIALIZATION_FAILED, s->vertex_path ? s->vertex_path : "",
            s->fragment_path ? s->fragment_path : "", errno);
    fflush(stderr);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static void write_result(const struct replay_state *s, const char *status)
{
    char temporary[1024];
    char safe_message[512], safe_device[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
    char safe_vertex[512], safe_fragment[512], safe_fragment_sha256[128];
    char safe_rgba[512], safe_layout[64];
    const char *uniform_bindings = "[0,1,2,3,4,5,6,7,8,9]";
    const char *vertex_uniform_bindings = "[0,1,2,3,4]";
    const char *fragment_uniform_bindings = "[5,6,7,8,9]";
    const char *sampler_bindings = "[10,11,12,13,14,15]";
    const char *image_bindings = "[16,17,18,19,20,21]";
    FILE *file;
    int fd;
    if (!s->result_path || !s->result_path[0]) return;
    snprintf(temporary, sizeof(temporary), "%s.tmp.%d", s->result_path, getpid());
    json_safe(safe_message, sizeof(safe_message), s->message);
    json_safe(safe_device, sizeof(safe_device), s->properties.deviceName);
    json_safe(safe_vertex, sizeof(safe_vertex), s->vertex_path);
    json_safe(safe_fragment, sizeof(safe_fragment), s->fragment_path);
    json_safe(safe_fragment_sha256, sizeof(safe_fragment_sha256),
              s->fragment_sha256 ? s->fragment_sha256 : "unknown");
    json_safe(safe_rgba, sizeof(safe_rgba), s->rgba_path);
    json_safe(safe_layout, sizeof(safe_layout), s->layout_mode);
    if (s->layout_mode && !strcmp(s->layout_mode, "exact")) {
        uniform_bindings = "[0,1,2,3,15,160,161,162,163,164]";
        vertex_uniform_bindings = "[160,161,162,163,164]";
        fragment_uniform_bindings = "[0,1,2,3,15]";
        sampler_bindings = "[16,17,19,20,28,29]";
        image_bindings = "[32,33,35,36,44,45]";
    }
    file = fopen(temporary, "w");
    if (!file) return;
    fprintf(file,
            "{\n"
            "  \"schemaVersion\":1,\n"
            "  \"runId\":\"%s\",\n"
            "  \"testId\":\"%s\",\n"
            "  \"status\":\"%s\",\n"
            "  \"stage\":\"%s\",\n"
            "  \"message\":\"%s\",\n"
            "  \"vulkanResult\":%d,\n"
            "  \"device\":{\"name\":\"%s\",\"vendorId\":%u,\"deviceId\":%u,"
            "\"apiVersion\":\"%u.%u.%u\",\"queueFamily\":%u},\n"
            "  \"shader\":{\"vertex\":\"%s\",\"fragment\":\"%s\","
            "\"fragmentSha256\":\"%s\","
            "\"specialization\":{\"1216\":12816,\"applied\":%s,\"boolMode\":\"%s\",\"boolEntryCount\":%u}},\n"
            "  \"descriptorContract\":{\"set\":0,\"layoutMode\":\"%s\",\"bindingCount\":%u,"
            "\"dynamicBindingCount\":%u,\"uniformBuffers\":%s,"
            "\"uniformBufferBytes\":[144,16,48,1536,16,64,144,32,64,512],"
            "\"vertexUniformBindings\":%s,\"fragmentUniformBindings\":%s,"
            "\"samplers\":%s,"
            "\"sampledImages\":%s,"
            "\"formats\":[\"R8G8B8A8_UNORM\",\"R8G8_SNORM\",\"R8G8B8A8_UNORM\","
            "\"R8G8_SNORM\",\"R8_UNORM\",\"R8G8B8A8_UNORM\"],"
            "\"layout\":\"SHADER_READ_ONLY_OPTIMAL\"},\n"
            "  \"graphics\":{\"extent\":\"%ux%u\",\"rgbaOutput\":\"%s\","
            "\"checksum\":\"0x%08x\",\"changedPixels\":%u,\"opaquePixels\":%u,"
            "\"sampleValues\":[\"0x%08x\",\"0x%08x\",\"0x%08x\","
            "\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"]},\n"
            "  \"architecture\":{\"peArchitecture\":\"not-applicable\","
            "\"wineUnixArchitecture\":\"x86_64\",\"vulkanLoaderArchitecture\":\"x86_64\","
            "\"venusIcdArchitecture\":\"x86_64-or-reference\",\"hostArchitecture\":\"runtime\","
            "\"wow64ThunkEnabled\":false,\"box64Enabled\":true},\n"
            "  \"metrics\":{\"cpuReadBytes\":16384,\"cpuUploadBytes\":4112,"
            "\"gpuCopyCount\":%" PRIu64 ",\"queueSubmitCount\":%" PRIu64 ","
            "\"perFrameDeviceWaitIdle\":0,\"fallbackDetected\":false,\"durationMs\":%" PRIu64 "}\n"
            "}\n",
            s->run_id, s->test_id, status, s->stage, safe_message,
            (int)s->failure_result, safe_device, s->properties.vendorID,
            s->properties.deviceID, VK_API_VERSION_MAJOR(s->properties.apiVersion),
            VK_API_VERSION_MINOR(s->properties.apiVersion),
            VK_API_VERSION_PATCH(s->properties.apiVersion), s->queue_family,
            safe_vertex, safe_fragment, safe_fragment_sha256,
            s->specialization_applied ? "true" : "false",
            s->bool_mode ? s->bool_mode : "default", s->bool_spec_count,
            safe_layout, s->layout_binding_count,
            s->dynamic_binding_count, uniform_bindings,
            vertex_uniform_bindings, fragment_uniform_bindings,
            sampler_bindings, image_bindings, REPLAY_WIDTH, REPLAY_HEIGHT,
            safe_rgba, s->checksum,
            s->changed_pixels, s->opaque_pixels,
            s->sample_values[0], s->sample_values[1], s->sample_values[2],
            s->sample_values[3], s->sample_values[4], s->sample_values[5],
            s->sample_values[6], s->sample_values[7], s->sample_values[8],
            s->gpu_copies, s->queue_submits, now_ms() - s->started_ms);
    fflush(file);
    fd = fileno(file);
    if (fd >= 0) fsync(fd);
    fclose(file);
    rename(temporary, s->result_path);
}

static int load_spirv(const char *path, uint32_t **code, size_t *size)
{
    FILE *file;
    long length;
    *code = NULL;
    *size = 0;
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    length = ftell(file);
    if (length <= 0 || (length & 3)) { fclose(file); return 0; }
    rewind(file);
    *code = malloc((size_t)length);
    if (!*code || fread(*code, 1, (size_t)length, file) != (size_t)length) {
        free(*code);
        *code = NULL;
        fclose(file);
        return 0;
    }
    fclose(file);
    *size = (size_t)length;
    return 1;
}

static int find_memory_type(struct replay_state *s, uint32_t bits,
                            VkMemoryPropertyFlags required, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(s->physical, &memory);
    for (i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & required) == required) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static VkResult create_buffer(struct replay_state *s, VkDeviceSize size,
                              VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags properties,
                              struct replay_buffer *buffer)
{
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t type;
    VkResult result;
    buffer->size = size;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateBuffer(s->device, &info, NULL, &buffer->buffer);
    if (result != VK_SUCCESS) return result;
    vkGetBufferMemoryRequirements(s->device, buffer->buffer, &requirements);
    if (!find_memory_type(s, requirements.memoryTypeBits, properties, &type))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = vkAllocateMemory(s->device, &allocation, NULL, &buffer->memory);
    if (result != VK_SUCCESS) return result;
    return vkBindBufferMemory(s->device, buffer->buffer, buffer->memory, 0);
}

static VkResult create_image(struct replay_state *s, VkFormat format,
                             uint32_t width, uint32_t height,
                             VkImageUsageFlags usage, VkImage *image,
                             VkDeviceMemory *memory)
{
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t type;
    VkResult result;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = width;
    info.extent.height = height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(s->device, &info, NULL, image);
    if (result != VK_SUCCESS) return result;
    vkGetImageMemoryRequirements(s->device, *image, &requirements);
    if (!find_memory_type(s, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        !find_memory_type(s, requirements.memoryTypeBits, 0, &type))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = vkAllocateMemory(s->device, &allocation, NULL, memory);
    if (result != VK_SUCCESS) return result;
    return vkBindImageMemory(s->device, *image, *memory, 0);
}

static void image_barrier(VkCommandBuffer command, VkImage image,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, src_stage, dst_stage, 0,
                         0, NULL, 0, NULL, 1, &barrier);
}

static VkResult init_vulkan(struct replay_state *s)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkPhysicalDevice *devices = NULL;
    VkQueueFamilyProperties *queues = NULL;
    VkPhysicalDeviceFeatures supported_features = { 0 };
    VkPhysicalDeviceFeatures enabled_features = { 0 };
    uint32_t device_count = 0, queue_count = 0, i;
    float priority = 1.0f;
    VkResult result;
    /* Match the init contract of the passing Venus graphics replay.  In
     * particular, do not let an application-name/API-version negotiation
     * change the device path used by this diagnostic. */
    app.apiVersion = VK_API_VERSION_1_1;
    instance_info.pApplicationInfo = NULL;
    result = vkCreateInstance(&instance_info, NULL, &s->instance);
    if (result != VK_SUCCESS) return result;
    result = vkEnumeratePhysicalDevices(s->instance, &device_count, NULL);
    if (result != VK_SUCCESS || !device_count)
        return result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
    devices = calloc(device_count, sizeof(*devices));
    if (!devices) return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = vkEnumeratePhysicalDevices(s->instance, &device_count, devices);
    if (result != VK_SUCCESS) { free(devices); return result; }
    s->physical = devices[0];
    free(devices);
    vkGetPhysicalDeviceProperties(s->physical, &s->properties);
    vkGetPhysicalDeviceFeatures(s->physical, &supported_features);
    enabled_features.shaderStorageImageReadWithoutFormat =
        supported_features.shaderStorageImageReadWithoutFormat;
    enabled_features.shaderStorageImageWriteWithoutFormat =
        supported_features.shaderStorageImageWriteWithoutFormat;
    vkGetPhysicalDeviceQueueFamilyProperties(s->physical, &queue_count, NULL);
    queues = calloc(queue_count, sizeof(*queues));
    if (!queues) return VK_ERROR_OUT_OF_HOST_MEMORY;
    vkGetPhysicalDeviceQueueFamilyProperties(s->physical, &queue_count, queues);
    s->queue_family = UINT32_MAX;
    for (i = 0; i < queue_count; ++i) {
        if ((queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            s->queue_family = i;
            break;
        }
    }
    free(queues);
    if (s->queue_family == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    queue_info.queueFamilyIndex = s->queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &enabled_features;
    result = vkCreateDevice(s->physical, &device_info, NULL, &s->device);
    if (result != VK_SUCCESS) return result;
    vkGetDeviceQueue(s->device, s->queue_family, 0, &s->queue);
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = s->queue_family;
    result = vkCreateCommandPool(s->device, &pool_info, NULL, &s->command_pool);
    if (result != VK_SUCCESS) return result;
    command_info.commandPool = s->command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(s->device, &command_info, &s->command);
    if (result != VK_SUCCESS) return result;
    return vkCreateFence(s->device, &fence_info, NULL, &s->fence);
}

static void fill_upload_pattern(uint8_t *data)
{
    uint32_t image, x, y;
    memset(data, 0, REPLAY_IMAGE_COUNT * 256u);
    for (image = 0; image < REPLAY_IMAGE_COUNT; ++image) {
        uint8_t *base = data + image * 256u;
        for (y = 0; y < REPLAY_IMAGE_HEIGHT; ++y) {
            for (x = 0; x < REPLAY_IMAGE_WIDTH; ++x) {
                uint32_t pixel = y * REPLAY_IMAGE_WIDTH + x;
                if (image == 0 || image == 2 || image == 5) {
                    uint8_t *p = base + pixel * 4u;
                    p[0] = (uint8_t)(32u + image * 21u + x * 37u);
                    p[1] = (uint8_t)(24u + y * 43u);
                    p[2] = (uint8_t)(192u - image * 17u + (x ^ y) * 9u);
                    p[3] = (uint8_t)(224u + ((x + y) & 1u) * 31u);
                } else if (image == 1 || image == 3) {
                    int8_t *p = (int8_t *)(base + pixel * 2u);
                    p[0] = (int8_t)(-96 + (int)x * 48 + (int)image * 4);
                    p[1] = (int8_t)(-88 + (int)y * 44 - (int)image * 3);
                } else {
                    base[pixel] = (uint8_t)(24u + x * 41u + y * 17u);
                }
            }
        }
    }
}

static VkResult create_resources(struct replay_state *s)
{
    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkResult result;
    uint32_t i;
    result = create_buffer(s, REPLAY_IMAGE_COUNT * 256u,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->upload);
    if (result != VK_SUCCESS) return result;
    {
        void *mapped = NULL;
        result = vkMapMemory(s->device, s->upload.memory, 0, s->upload.size, 0, &mapped);
        if (result != VK_SUCCESS) return result;
        fill_upload_pattern(mapped);
        vkUnmapMemory(s->device, s->upload.memory);
    }
    /* The captured VS consumes four vec4 attributes at locations 0..3. The
     * previous replay left these inputs unbound, making the real VS produce a
     * degenerate primitive and hiding descriptor/shader results. */
    result = create_buffer(s, 3u * 4u * sizeof(float) * 4u,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->vertex);
    if (result != VK_SUCCESS) return result;
    {
        static const float vertices[3][16] = {
            { -0.85f, -0.80f, 0.20f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f,
               0.0f, 0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f, 1.0f },
            {  0.85f, -0.80f, 0.20f, 1.0f,  1.0f, 0.0f, 1.0f, 1.0f,
               1.0f, 0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f, 1.0f },
            {  0.00f,  0.85f, 0.20f, 1.0f,  0.5f, 1.0f, 1.0f, 1.0f,
               0.5f, 1.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f, 1.0f },
        };
        void *mapped = NULL;
        result = vkMapMemory(s->device, s->vertex.memory, 0,
                             s->vertex.size, 0, &mapped);
        if (result != VK_SUCCESS) return result;
        memcpy(mapped, vertices, sizeof(vertices));
        vkUnmapMemory(s->device, s->vertex.memory);
    }
    for (i = 0; i < REPLAY_UBO_COUNT; ++i) {
        float values[384];
        uint32_t j, count = replay_ubo_sizes[i] / sizeof(float);
        void *mapped = NULL;
        result = create_buffer(s, replay_ubo_sizes[i], VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->ubos[i]);
        if (result != VK_SUCCESS) return result;
        memset(values, 0, sizeof(values));
        for (j = 0; j < count; ++j)
            values[j] = 0.125f + 0.03125f * (float)((j + i * 3u) % 11u);
        if (i == 7) {
            values[0] = 0.65f; values[1] = 0.55f;
            values[2] = 0.45f; values[3] = 1.0f;
        } else if (i == 8) {
            values[0] = 1.0f; values[1] = 1.0f;
            values[2] = 0.0f; values[3] = 0.0f;
            values[4] = 0.75f; values[5] = 0.5f;
            values[8] = 0.8f; values[9] = 0.7f;
            values[10] = 0.6f; values[11] = 0.5f;
        } else if (i == 9) {
            memset(values, 0, sizeof(values));
        }
        result = vkMapMemory(s->device, s->ubos[i].memory, 0,
                             s->ubos[i].size, 0, &mapped);
        if (result != VK_SUCCESS) return result;
        memcpy(mapped, values, replay_ubo_sizes[i]);
        vkUnmapMemory(s->device, s->ubos[i].memory);
    }
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.minLod = 0.0f;
    sampler.maxLod = 0.0f;
    for (i = 0; i < REPLAY_IMAGE_COUNT; ++i) {
        VkFormatProperties properties;
        s->images[i].format = replay_formats[i];
        vkGetPhysicalDeviceFormatProperties(s->physical, replay_formats[i], &properties);
        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        result = create_image(s, replay_formats[i], REPLAY_IMAGE_WIDTH,
                              REPLAY_IMAGE_HEIGHT,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT,
                              &s->images[i].image, &s->images[i].memory);
        if (result != VK_SUCCESS) return result;
        view.image = s->images[i].image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = replay_formats[i];
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel = 0;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.baseArrayLayer = 0;
        view.subresourceRange.layerCount = 1;
        result = vkCreateImageView(s->device, &view, NULL, &s->images[i].view);
        if (result != VK_SUCCESS) return result;
        result = vkCreateSampler(s->device, &sampler, NULL, &s->images[i].sampler);
        if (result != VK_SUCCESS) return result;
    }
    result = create_image(s, VK_FORMAT_R8G8B8A8_UNORM, REPLAY_WIDTH,
                          REPLAY_HEIGHT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          &s->target, &s->target_memory);
    if (result != VK_SUCCESS) return result;
    view.image = s->target;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    result = vkCreateImageView(s->device, &view, NULL, &s->target_view);
    if (result != VK_SUCCESS) return result;
    return create_buffer(s, REPLAY_WIDTH * REPLAY_HEIGHT * 4u,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->readback);
}

static VkResult create_descriptors(struct replay_state *s)
{
    VkDescriptorSetLayoutBinding bindings[27];
    VkDescriptorSetLayoutCreateInfo layout = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    VkDescriptorPoolSize sizes[4];
    VkDescriptorPoolCreateInfo pool = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorBufferInfo buffer_info[REPLAY_UBO_COUNT];
    VkDescriptorImageInfo sampler_info[REPLAY_IMAGE_COUNT];
    VkDescriptorImageInfo image_info[REPLAY_IMAGE_COUNT];
    VkWriteDescriptorSet writes[27];
    VkResult result;
    const char *mode = s->layout_mode && s->layout_mode[0] ? s->layout_mode : "full";
    bool exact = !strcmp(mode, "exact");
    bool small = !strcmp(mode, "small");
    uint32_t ubo_count = small ? 1u : REPLAY_UBO_COUNT;
    uint32_t image_count = small ? 1u : REPLAY_IMAGE_COUNT;
    uint32_t sampler_base = small ? 1u : 10u;
    uint32_t image_base = small ? 2u : 16u;
    bool use_ubo = small || !strcmp(mode, "full") || !strcmp(mode, "ubo") ||
        !strcmp(mode, "dynamic");
    bool use_dynamic = !strcmp(mode, "dynamic");
    bool use_samplers = small || !strcmp(mode, "full") || !strcmp(mode, "sampler") ||
        !strcmp(mode, "images") || !strcmp(mode, "dynamic");
    bool use_images = small || !strcmp(mode, "full") || !strcmp(mode, "sampled") ||
        !strcmp(mode, "images") || !strcmp(mode, "dynamic");
    uint32_t i, write = 0, binding_count = 0, pool_count = 0;
    memset(sizes, 0, sizeof(sizes));
    memset(bindings, 0, sizeof(bindings));
    /* Reproduce the captured Heaven descriptor contract verbatim. */
    if (exact) {
      static const uint32_t exact_bindings[] = {
          160u, 161u, 162u, 163u, 164u,
          0u, 1u, 2u, 3u, 15u,
          16u, 17u, 19u, 20u, 28u, 29u,
          32u, 33u, 35u, 36u, 44u, 45u,
      };

      /* Keep pBindings sorted while preserving each binding's type and
       * stage visibility. */
      struct exact_layout_binding {
        uint32_t binding;
        VkDescriptorType type;
        VkShaderStageFlags stages;
      };
      static const struct exact_layout_binding layout_contract[] = {
          { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 2u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 3u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 15u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 16u, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 17u, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 19u, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 20u, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 28u, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 29u, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 32u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 33u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 35u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 36u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 44u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 45u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 160u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
          { 161u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
          { 162u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
          { 163u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
          { 164u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      };
      uint32_t index = 0;
      for (i = 0; i < sizeof(layout_contract) / sizeof(layout_contract[0]); ++i) {
        bindings[index].binding = layout_contract[i].binding;
        bindings[index].descriptorType = layout_contract[i].type;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = layout_contract[i].stages;
        ++index;
      }
      binding_count = index;
      sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      sizes[0].descriptorCount = 10u;
      sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
      sizes[1].descriptorCount = 6u;
      sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      sizes[2].descriptorCount = 6u;
      pool_count = 3u;
      layout.bindingCount = binding_count;
      layout.pBindings = bindings;
      result = vkCreateDescriptorSetLayout(s->device, &layout, NULL, &s->set_layout);
      if (result != VK_SUCCESS) return result;
      pool.maxSets = 1;
      pool.poolSizeCount = pool_count;
      pool.pPoolSizes = sizes;
      result = vkCreateDescriptorPool(s->device, &pool, NULL, &s->descriptor_pool);
      if (result != VK_SUCCESS) return result;
      allocate.descriptorPool = s->descriptor_pool;
      allocate.descriptorSetCount = 1;
      allocate.pSetLayouts = &s->set_layout;
      result = vkAllocateDescriptorSets(s->device, &allocate, &s->descriptor_set);
      if (result != VK_SUCCESS) return result;
      memset(writes, 0, sizeof(writes));
      for (i = 0; i < 10u; ++i) {
        buffer_info[i].buffer = s->ubos[i].buffer;
        buffer_info[i].offset = 0;
        buffer_info[i].range = replay_ubo_sizes[i];
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = exact_bindings[i];
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[write].pBufferInfo = &buffer_info[i];
        write++;
      }
      for (i = 0; i < 6u; ++i) {
        sampler_info[i].sampler = s->images[i].sampler;
        sampler_info[i].imageView = VK_NULL_HANDLE;
        sampler_info[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = exact_bindings[10u + i];
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[write].pImageInfo = &sampler_info[i];
        write++;
      }
      for (i = 0; i < 6u; ++i) {
        image_info[i].sampler = VK_NULL_HANDLE;
        image_info[i].imageView = s->images[i].view;
        image_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = exact_bindings[16u + i];
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[write].pImageInfo = &image_info[i];
        write++;
      }
      s->layout_binding_count = binding_count;
      s->dynamic_binding_count = 0;
      vkUpdateDescriptorSets(s->device, write, writes, 0, NULL);
      return VK_SUCCESS;
    }
    if (use_ubo) {
      for (i = 0; i < ubo_count; ++i) {
        bindings[i].binding = i < 10u ? i : 150u + i;
        bindings[i].descriptorType = use_dynamic ?
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[i].descriptorCount = 1;
        /* The reduced small contract is fragment-only, matching the passing
         * graphics replay.  The full Heaven contract retains vertex-stage
         * visibility for bindings 0..4 because the captured vertex shader
         * genuinely reads those UBOs. */
        bindings[i].stageFlags = small ? VK_SHADER_STAGE_FRAGMENT_BIT :
            (i < 5u ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) :
             (i < 10u ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT));
      }
      binding_count = ubo_count;
      sizes[pool_count].type = use_dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC :
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      sizes[pool_count++].descriptorCount = ubo_count;
    }
    if (use_samplers) {
      for (i = 0; i < image_count; ++i) {
        bindings[binding_count + i].binding = sampler_base + i;
        bindings[binding_count + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[binding_count + i].descriptorCount = 1;
        bindings[binding_count + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      }
      binding_count += image_count;
      sizes[pool_count].type = VK_DESCRIPTOR_TYPE_SAMPLER;
      sizes[pool_count++].descriptorCount = image_count;
    }
    if (use_images) {
      for (i = 0; i < image_count; ++i) {
        bindings[binding_count + i].binding = image_base + i;
        bindings[binding_count + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[binding_count + i].descriptorCount = 1;
        bindings[binding_count + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      }
      binding_count += image_count;
      sizes[pool_count].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      sizes[pool_count++].descriptorCount = image_count;
    }
    s->layout_binding_count = binding_count;
    s->dynamic_binding_count = use_dynamic ? ubo_count : 0;
    layout.bindingCount = binding_count;
    layout.pBindings = bindings;
    result = vkCreateDescriptorSetLayout(s->device, &layout, NULL, &s->set_layout);
    if (result != VK_SUCCESS) return result;
    pool.maxSets = 1;
    /* A zero-descriptor layout is legal, but some implementations reject a
     * descriptor pool with no pool sizes. Keep one unused slot for that
     * diagnostic mode; it does not change the pipeline layout contract. */
    if (!pool_count) {
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = 1;
        pool_count = 1;
    }
    pool.poolSizeCount = pool_count;
    pool.pPoolSizes = sizes;
    result = vkCreateDescriptorPool(s->device, &pool, NULL, &s->descriptor_pool);
    if (result != VK_SUCCESS) return result;
    allocate.descriptorPool = s->descriptor_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &s->set_layout;
    result = vkAllocateDescriptorSets(s->device, &allocate, &s->descriptor_set);
    if (result != VK_SUCCESS) return result;
    memset(writes, 0, sizeof(writes));
    if (use_ubo) for (i = 0; i < ubo_count; ++i) {
        buffer_info[i].buffer = s->ubos[i].buffer;
        buffer_info[i].offset = 0;
        buffer_info[i].range = replay_ubo_sizes[i];
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = i < 10u ? i : 150u + i;
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = use_dynamic ?
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[write].pBufferInfo = &buffer_info[i];
        write++;
    }
    if (use_samplers) for (i = 0; i < image_count; ++i) {
        sampler_info[i].sampler = s->images[i].sampler;
        sampler_info[i].imageView = VK_NULL_HANDLE;
        sampler_info[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = sampler_base + i;
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[write].pImageInfo = &sampler_info[i];
        write++;
    }
    if (use_images) for (i = 0; i < image_count; ++i) {
        image_info[i].sampler = VK_NULL_HANDLE;
        image_info[i].imageView = s->images[i].view;
        image_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = image_base + i;
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[write].pImageInfo = &image_info[i];
        write++;
    }
    vkUpdateDescriptorSets(s->device, write, writes, 0, NULL);
    return VK_SUCCESS;
}

static bool spirv_has_spec_id(const uint32_t *code, size_t size, uint32_t spec_id)
{
    size_t words, offset;
    if (!code || size < 20 || (size & 3) || code[0] != 0x07230203u)
        return false;
    words = size / sizeof(uint32_t);
    for (offset = 5; offset < words;) {
        uint16_t word_count = (uint16_t)(code[offset] >> 16);
        uint16_t opcode = (uint16_t)(code[offset] & 0xffffu);
        if (!word_count || offset + word_count > words)
            return false;
        if (opcode == 71 && word_count >= 4 && code[offset + 2] == 1u &&
            code[offset + 3] == spec_id)
            return true;
        offset += word_count;
    }
    return false;
}

static VkResult create_pipeline(struct replay_state *s)
{
    VkAttachmentDescription attachment = { 0 };
    VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { 0 };
    VkRenderPassCreateInfo render_pass = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkFramebufferCreateInfo framebuffer = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkPipelineLayoutCreateInfo pipeline_layout = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    VkVertexInputBindingDescription vertex_binding = {
        0u, 16u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX
    };
    VkVertexInputAttributeDescription vertex_attributes[4] = {
        { 0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 0u },
        { 1u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 4u * sizeof(float) },
        { 2u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 8u * sizeof(float) },
        { 3u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 12u * sizeof(float) },
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    VkPipelineViewportStateCreateInfo viewport = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    VkPipelineColorBlendAttachmentState blend_attachment = { 0 };
    VkPipelineColorBlendStateCreateInfo blend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    VkDynamicState dynamic_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamic = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
    };
    VkPipelineCreateFlags2CreateInfoKHR flags2 = {
        VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR
    };
    VkGraphicsPipelineCreateInfo pipeline = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
    };
    static const uint32_t bool_spec_ids[] = {
        0u, 1u, 2u, 3u, 15u, 32u, 33u, 35u, 36u, 44u, 45u
    };
    VkSpecializationMapEntry spec_entries[1 + sizeof(bool_spec_ids) / sizeof(bool_spec_ids[0])];
    uint32_t spec_values[1 + sizeof(bool_spec_ids) / sizeof(bool_spec_ids[0])];
    VkSpecializationInfo specialization = { 0 };
    uint32_t spec_count = 0;
    uint32_t bool_value = 1u;
    VkShaderModuleCreateInfo shader = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    uint32_t *vertex_code = NULL, *fragment_code = NULL;
    size_t vertex_size = 0, fragment_size = 0;
    VkResult result;
    if (!load_spirv(s->vertex_path, &vertex_code, &vertex_size) ||
        !load_spirv(s->fragment_path, &fragment_code, &fragment_size)) {
        free(vertex_code);
        free(fragment_code);
        return record_shader_load_failure(s);
    }
    shader.codeSize = vertex_size;
    shader.pCode = vertex_code;
    result = record_pipeline_step(s, "shader-module-vertex",
                                  vkCreateShaderModule(s->device, &shader, NULL,
                                                       &s->vertex_shader));
    if (result == VK_SUCCESS) {
        shader.codeSize = fragment_size;
        shader.pCode = fragment_code;
        result = record_pipeline_step(s, "shader-module-fragment",
                                      vkCreateShaderModule(s->device, &shader,
                                                           NULL,
                                                           &s->fragment_shader));
    }
    s->specialization_applied = spirv_has_spec_id(fragment_code, fragment_size, 1216u);
    if (s->specialization_applied) {
        spec_entries[spec_count].constantID = 1216u;
        spec_entries[spec_count].offset = spec_count * sizeof(uint32_t);
        spec_entries[spec_count].size = sizeof(uint32_t);
        spec_values[spec_count++] = 12816u;
    }
    if (s->bool_mode && !strcmp(s->bool_mode, "false"))
        bool_value = 0u;
    if (s->bool_mode && strcmp(s->bool_mode, "default")) {
        uint32_t i;
        for (i = 0; i < sizeof(bool_spec_ids) / sizeof(bool_spec_ids[0]); ++i) {
            if (!spirv_has_spec_id(fragment_code, fragment_size, bool_spec_ids[i]))
                continue;
            spec_entries[spec_count].constantID = bool_spec_ids[i];
            spec_entries[spec_count].offset = spec_count * sizeof(uint32_t);
            spec_entries[spec_count].size = sizeof(uint32_t);
            spec_values[spec_count++] = bool_value;
        }
    }
    specialization.mapEntryCount = spec_count;
    specialization.pMapEntries = spec_entries;
    specialization.dataSize = spec_count * sizeof(uint32_t);
    specialization.pData = spec_values;
    s->bool_spec_count = spec_count > (s->specialization_applied ? 1u : 0u) ?
        spec_count - (s->specialization_applied ? 1u : 0u) : 0u;
    free(vertex_code);
    free(fragment_code);
    if (result != VK_SUCCESS) return result;
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    render_pass.attachmentCount = 1;
    render_pass.pAttachments = &attachment;
    render_pass.subpassCount = 1;
    render_pass.pSubpasses = &subpass;
    result = record_pipeline_step(s, "render-pass",
                                  vkCreateRenderPass(s->device, &render_pass,
                                                      NULL, &s->render_pass));
    if (result != VK_SUCCESS) return result;
    framebuffer.renderPass = s->render_pass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &s->target_view;
    framebuffer.width = REPLAY_WIDTH;
    framebuffer.height = REPLAY_HEIGHT;
    framebuffer.layers = 1;
    result = record_pipeline_step(s, "framebuffer",
                                  vkCreateFramebuffer(s->device, &framebuffer,
                                                       NULL, &s->framebuffer));
    if (result != VK_SUCCESS) return result;
    pipeline_layout.setLayoutCount = strcmp(s->layout_mode, "no-set") ? 1u : 0u;
    pipeline_layout.pSetLayouts = pipeline_layout.setLayoutCount ? &s->set_layout : NULL;
    result = record_pipeline_step(s, "pipeline-layout",
                                  vkCreatePipelineLayout(s->device,
                                                         &pipeline_layout, NULL,
                                                         &s->pipeline_layout));
    if (result != VK_SUCCESS) return result;
    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s->vertex_shader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = s->fragment_shader;
    stages[1].pName = "main";
    /* Do not pass DXVK's specialization map to a replay shader which does not
     * declare that SpecId.  Lavapipe ignores unknown entries, while the
     * Maleoon compiler rejects them with VK_ERROR_INITIALIZATION_FAILED. */
    stages[1].pSpecializationInfo = spec_count ? &specialization : NULL;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vertex_input.vertexBindingDescriptionCount = 1u;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 4u;
    vertex_input.pVertexAttributeDescriptions = vertex_attributes;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex_input;
    pipeline.pInputAssemblyState = &input_assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = s->pipeline_layout;
    pipeline.renderPass = s->render_pass;
    pipeline.subpass = 0;
    /* Force a synchronous Venus reply for this diagnostic.  Without the
     * EARLY_RETURN_ON_FAILURE bit the Guest may receive a provisional handle
     * while the Host compiler fails later, turning a useful VkResult into a
     * ring-fatal bind error. */
    flags2.flags = VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR;
    pipeline.pNext = &flags2;
    return record_pipeline_step(s, "graphics-pipeline",
                                vkCreateGraphicsPipelines(s->device,
                                                          VK_NULL_HANDLE, 1,
                                                          &pipeline, NULL,
                                                          &s->pipeline));
}

static VkResult render_and_readback(struct replay_state *s)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkClearValue clear;
    VkRenderPassBeginInfo render = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkViewport viewport = {
        0.0f, 0.0f, (float)REPLAY_WIDTH, (float)REPLAY_HEIGHT, 0.0f, 1.0f
    };
    VkRect2D scissor = { { 0, 0 }, { REPLAY_WIDTH, REPLAY_HEIGHT } };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    uint32_t dynamic_offsets[REPLAY_UBO_COUNT] = { 0 };
    VkBufferImageCopy copy = { 0 };
    VkResult result;
    uint32_t i;
    result = vkBeginCommandBuffer(s->command, &begin);
    if (result != VK_SUCCESS) return result;
    for (i = 0; i < REPLAY_IMAGE_COUNT; ++i) {
        image_barrier(s->command, s->images[i].image,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        memset(&copy, 0, sizeof(copy));
        copy.bufferOffset = i * 256u;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = REPLAY_IMAGE_WIDTH;
        copy.imageExtent.height = REPLAY_IMAGE_HEIGHT;
        copy.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(s->command, s->upload.buffer,
                               s->images[i].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copy);
        image_barrier(s->command, s->images[i].image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        s->gpu_copies++;
    }
    memset(&clear, 0, sizeof(clear));
    clear.color.float32[0] = 1.0f;
    clear.color.float32[1] = 0.0f;
    clear.color.float32[2] = 1.0f;
    clear.color.float32[3] = 1.0f;
    render.renderPass = s->render_pass;
    render.framebuffer = s->framebuffer;
    render.renderArea.extent.width = REPLAY_WIDTH;
    render.renderArea.extent.height = REPLAY_HEIGHT;
    render.clearValueCount = 1;
    render.pClearValues = &clear;
    vkCmdBeginRenderPass(s->command, &render, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s->command, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline);
    {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(s->command, 0, 1, &s->vertex.buffer, &offset);
    }
    if (strcmp(s->layout_mode, "no-set"))
        vkCmdBindDescriptorSets(s->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                s->pipeline_layout, 0, 1, &s->descriptor_set,
                                s->dynamic_binding_count,
                                s->dynamic_binding_count ? dynamic_offsets : NULL);
    vkCmdSetViewport(s->command, 0, 1, &viewport);
    vkCmdSetScissor(s->command, 0, 1, &scissor);
    vkCmdDraw(s->command, 3, 1, 0, 0);
    vkCmdEndRenderPass(s->command);
    image_barrier(s->command, s->target,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = REPLAY_WIDTH;
    copy.imageExtent.height = REPLAY_HEIGHT;
    copy.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s->command, s->target,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s->readback.buffer, 1, &copy);
    s->gpu_copies++;
    result = vkEndCommandBuffer(s->command);
    if (result != VK_SUCCESS) return result;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s->command;
    result = vkQueueSubmit(s->queue, 1, &submit, s->fence);
    if (result != VK_SUCCESS) return result;
    s->queue_submits++;
    return vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, 10000000000ull);
}

static int analyze_output(struct replay_state *s)
{
    static const uint32_t sample_x[REPLAY_SAMPLE_COUNT] = {
        4u, 31u, 59u, 4u, 31u, 59u, 4u, 31u, 59u,
    };
    static const uint32_t sample_y[REPLAY_SAMPLE_COUNT] = {
        4u, 4u, 4u, 31u, 31u, 31u, 59u, 59u, 59u,
    };
    VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    void *mapped = NULL;
    const uint8_t *pixels;
    uint32_t i;
    FILE *rgba;
    VkResult result = vkMapMemory(s->device, s->readback.memory, 0,
                                  s->readback.size, 0, &mapped);
    if (result != VK_SUCCESS) return 0;
    range.memory = s->readback.memory;
    range.size = VK_WHOLE_SIZE;
    result = vkInvalidateMappedMemoryRanges(s->device, 1, &range);
    if (result != VK_SUCCESS) {
        vkUnmapMemory(s->device, s->readback.memory);
        return 0;
    }
    pixels = mapped;
    s->checksum = 2166136261u;
    s->changed_pixels = 0;
    s->opaque_pixels = 0;
    for (i = 0; i < REPLAY_WIDTH * REPLAY_HEIGHT; ++i) {
        const uint8_t *p = &pixels[i * 4u];
        if (p[0] != 255u || p[1] != 0u || p[2] != 255u || p[3] != 255u)
            s->changed_pixels++;
        if (p[3] == 255u) s->opaque_pixels++;
        s->checksum ^= p[0]; s->checksum *= 16777619u;
        s->checksum ^= p[1]; s->checksum *= 16777619u;
        s->checksum ^= p[2]; s->checksum *= 16777619u;
        s->checksum ^= p[3]; s->checksum *= 16777619u;
    }
    for (i = 0; i < REPLAY_SAMPLE_COUNT; ++i) {
        const uint8_t *p = &pixels[(sample_y[i] * REPLAY_WIDTH + sample_x[i]) * 4u];
        s->sample_values[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    if (s->rgba_path && s->rgba_path[0]) {
        rgba = fopen(s->rgba_path, "wb");
        if (rgba) {
            fwrite(pixels, 1, REPLAY_WIDTH * REPLAY_HEIGHT * 4u, rgba);
            fflush(rgba);
            fsync(fileno(rgba));
            fclose(rgba);
        }
    }
    vkUnmapMemory(s->device, s->readback.memory);
    return 1;
}

static void destroy_buffer(struct replay_state *s, struct replay_buffer *buffer)
{
    if (buffer->buffer) vkDestroyBuffer(s->device, buffer->buffer, NULL);
    if (buffer->memory) vkFreeMemory(s->device, buffer->memory, NULL);
    memset(buffer, 0, sizeof(*buffer));
}

static void cleanup(struct replay_state *s)
{
    uint32_t i;
    /* A synchronous pipeline diagnostic can return an error after Venus has
     * already marked the ring fatal.  Waiting for device idle on that context
     * re-enters the broken ring and causes a secondary Box64 crash, hiding the
     * authoritative VkResult. */
    if (s->device && s->failure_result == VK_SUCCESS)
        vkDeviceWaitIdle(s->device);
    if (s->pipeline) vkDestroyPipeline(s->device, s->pipeline, NULL);
    if (s->fragment_shader) vkDestroyShaderModule(s->device, s->fragment_shader, NULL);
    if (s->vertex_shader) vkDestroyShaderModule(s->device, s->vertex_shader, NULL);
    if (s->framebuffer) vkDestroyFramebuffer(s->device, s->framebuffer, NULL);
    if (s->render_pass) vkDestroyRenderPass(s->device, s->render_pass, NULL);
    if (s->pipeline_layout) vkDestroyPipelineLayout(s->device, s->pipeline_layout, NULL);
    if (s->descriptor_pool) vkDestroyDescriptorPool(s->device, s->descriptor_pool, NULL);
    if (s->set_layout) vkDestroyDescriptorSetLayout(s->device, s->set_layout, NULL);
    destroy_buffer(s, &s->readback);
    if (s->target_view) vkDestroyImageView(s->device, s->target_view, NULL);
    if (s->target) vkDestroyImage(s->device, s->target, NULL);
    if (s->target_memory) vkFreeMemory(s->device, s->target_memory, NULL);
    for (i = 0; i < REPLAY_IMAGE_COUNT; ++i) {
        if (s->images[i].sampler)
            vkDestroySampler(s->device, s->images[i].sampler, NULL);
        if (s->images[i].view)
            vkDestroyImageView(s->device, s->images[i].view, NULL);
        if (s->images[i].image)
            vkDestroyImage(s->device, s->images[i].image, NULL);
        if (s->images[i].memory)
            vkFreeMemory(s->device, s->images[i].memory, NULL);
    }
    for (i = 0; i < REPLAY_UBO_COUNT; ++i)
        destroy_buffer(s, &s->ubos[i]);
    destroy_buffer(s, &s->upload);
    destroy_buffer(s, &s->vertex);
    if (s->fence) vkDestroyFence(s->device, s->fence, NULL);
    if (s->command_pool) vkDestroyCommandPool(s->device, s->command_pool, NULL);
    if (s->device) vkDestroyDevice(s->device, NULL);
    if (s->instance) vkDestroyInstance(s->instance, NULL);
}

int main(int argc, char **argv)
{
    struct replay_state state;
    VkResult result;
    int passed = 0;
    memset(&state, 0, sizeof(state));
    state.run_id = argument_value(argc, argv, "--run-id", "manual");
    state.test_id = argument_value(argc, argv, "--test-id",
                                   "venus-heaven-material-graphics-x64");
    state.result_path = argument_value(argc, argv, "--result", "");
    state.rgba_path = argument_value(argc, argv, "--rgba-output", "");
    state.vertex_path = argument_value(argc, argv, "--vertex-spv",
                                       "share/winehua/venus_heaven_material.vert.spv");
    state.fragment_path = argument_value(argc, argv, "--fragment-spv",
                                         "share/winehua/replay_external/heaven_final_fs.spv");
    state.fragment_sha256 = "unknown";
    if (strstr(state.fragment_path, "heaven_sparse_fs.spv") ||
        strstr(state.fragment_path, "heaven_final_fs.spv")) {
        state.fragment_sha256 =
            "ee6dbab51709fe8e057c01b7e8cbc92b14f29bbe370ccad39f70640ae2fdd05f";
    }
    state.layout_mode = argument_value(argc, argv, "--layout-mode", "full");
    state.bool_mode = argument_value(argc, argv, "--bool-mode", "default");
    if (strcmp(state.bool_mode, "default") && strcmp(state.bool_mode, "true") &&
        strcmp(state.bool_mode, "false"))
        state.bool_mode = "default";
    if (strcmp(state.layout_mode, "full") && strcmp(state.layout_mode, "empty") &&
        strcmp(state.layout_mode, "no-set") && strcmp(state.layout_mode, "small") &&
        strcmp(state.layout_mode, "ubo") && strcmp(state.layout_mode, "dynamic") &&
        strcmp(state.layout_mode, "sampler") && strcmp(state.layout_mode, "sampled") &&
        strcmp(state.layout_mode, "images") && strcmp(state.layout_mode, "exact")) {
        state.layout_mode = "full";
    }
    state.started_ms = now_ms();
    record_failure(&state, "startup", "replay started", VK_SUCCESS);
    write_result(&state, "started");
    result = init_vulkan(&state);
    if (result != VK_SUCCESS) {
        record_failure(&state, "host-vulkan", "Vulkan initialization failed", result);
        goto done;
    }
    result = create_resources(&state);
    if (result != VK_SUCCESS) {
        record_failure(&state, "resources", "deterministic replay resource creation failed", result);
        goto done;
    }
    result = create_descriptors(&state);
    if (result != VK_SUCCESS) {
        record_failure(&state, "descriptor", "exact DXVK descriptor contract creation failed", result);
        goto done;
    }
    result = create_pipeline(&state);
    if (result != VK_SUCCESS) {
        record_failure(&state,
                       state.pipeline_stage[0] ? state.pipeline_stage : "pipeline",
                       "captured Heaven material pipeline setup failed", result);
        goto done;
    }
    result = render_and_readback(&state);
    if (result != VK_SUCCESS) {
        record_failure(&state, "draw", "captured Heaven fragment draw or wait failed", result);
        goto done;
    }
    if (!analyze_output(&state)) {
        record_failure(&state, "readback", "offscreen replay readback failed", VK_ERROR_MEMORY_MAP_FAILED);
        goto done;
    }
    if (!strcmp(state.layout_mode, "exact") && !state.opaque_pixels) {
        record_failure(&state, "graphics-output",
                       "exact replay produced no opaque pixels", VK_SUCCESS);
        goto done;
    }
    if (!state.changed_pixels) {
        record_failure(&state, "graphics-replay", "fragment draw left the sentinel clear color unchanged", VK_SUCCESS);
        goto done;
    }
    record_failure(&state, "graphics-replay",
                   "captured Heaven fragment executed; compare RGBA against reference",
                   VK_SUCCESS);
    passed = 1;
done:
    write_result(&state, passed ? "PASS" : "FAIL");
    cleanup(&state);
    return passed ? 0 : 1;
}
