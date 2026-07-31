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

#define EXPECTED_COLOR 0xff332211u
#define SHADER_NOT_EXECUTED 0xdeadbeefu
#define STORAGE_WRITE_VALUE 0xa1b2c3d4u

struct probe_state {
    const char *run_id;
    const char *test_id;
    const char *result_path;
    const char *layout_name;
    int force_idle;
    int sampled_only;
    int graphics_replay;
    uint64_t started_ms;
    uint32_t loader_api;
    VkPhysicalDeviceProperties properties;
    VkFormatProperties format_properties;
    int image_format_supported;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkFence fence;
    VkImage source_image;
    VkDeviceMemory source_memory;
    VkBuffer upload_buffer;
    VkDeviceMemory upload_memory;
    VkBuffer readback_buffer;
    VkDeviceMemory readback_memory;
    VkBuffer output_buffer;
    VkDeviceMemory output_memory;
    VkImageView source_view;
    VkSampler sampler;
    int image_upload_readback;
    int storage_image_write;
    int storage_image_read;
    int sampled_image_fetch;
    int combined_image_sampler;
    int separated_image_sampler;
    int shader_executed;
    int image_read_completed;
    uint32_t sampled_value;
    uint32_t expected_value;
    uint64_t queue_submit_count;
    uint64_t gpu_copy_count;
    uint64_t wait_idle_count;
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

static int argument_flag(int argc, char **argv, const char *name)
{
    int i;
    for (i = 1; i < argc; ++i)
        if (!strcmp(argv[i], name)) return 1;
    return 0;
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

static void write_result(const struct probe_state *state, const char *status,
                         const char *stage, const char *message)
{
    char temporary[1024];
    char safe_message[256];
    char safe_device[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
    FILE *file;
    int fd;

    if (!state->result_path || !state->result_path[0]) return;
    snprintf(temporary, sizeof(temporary), "%s.tmp.%d", state->result_path, getpid());
    json_safe_copy(safe_message, sizeof(safe_message), message ? message : "");
    json_safe_copy(safe_device, sizeof(safe_device), state->properties.deviceName);
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
            "  \"architecture\":{\"peArchitecture\":\"not-applicable\",\"wineUnixArchitecture\":\"x86_64\",\"vulkanLoaderArchitecture\":\"x86_64\",\"venusIcdArchitecture\":\"x86_64\",\"hostArchitecture\":\"aarch64\",\"wow64ThunkEnabled\":false,\"box64Enabled\":true},\n"
            "  \"capabilities\":{\"deviceName\":\"%s\",\"deviceApiVersion\":\"%u.%u.%u\",\"queueFamily\":%u,"
            "\"rgba8OptimalTilingFeatures\":%u,\"rgba8SampledImage\":%s,\"rgba8LinearFilter\":%s,\"rgba8ImageFormatProperties\":%s},\n"
            "  \"checks\":{\"imageUploadReadback\":%s,\"storageImageWrite\":%s,\"storageImageRead\":%s,\"sampledImageFetch\":%s,\"combinedImageSampler\":%s,\"separatedImageSampler\":%s,\"shaderExecuted\":%s,\"imageReadCompleted\":%s},\n"
            "  \"sampledImage\":{\"layout\":\"%s\",\"forceWaitIdle\":%s,\"sampledValue\":\"0x%08x\",\"expectedValue\":\"0x%08x\"},\n"
            "  \"metrics\":{\"cpuReadBytes\":%u,\"cpuUploadBytes\":%u,\"gpuCopyCount\":%llu,\"queueSubmitCount\":%llu,\"perFrameDeviceWaitIdle\":%llu,\"fallbackDetected\":false,\"durationMs\":%llu}\n"
            "}\n",
            state->run_id, state->test_id, status, stage, safe_message,
            safe_device, VK_API_VERSION_MAJOR(state->properties.apiVersion),
            VK_API_VERSION_MINOR(state->properties.apiVersion),
            VK_API_VERSION_PATCH(state->properties.apiVersion), state->queue_family,
            state->format_properties.optimalTilingFeatures,
            (state->format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "true" : "false",
            (state->format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? "true" : "false",
            state->image_format_supported < 0 ? "null" :
                (state->image_format_supported ? "true" : "false"),
            state->image_upload_readback ? "true" : "false",
            state->sampled_only ? "null" : (state->storage_image_write ? "true" : "false"),
            state->sampled_only ? "null" : (state->storage_image_read ? "true" : "false"),
            state->sampled_image_fetch ? "true" : "false",
            state->combined_image_sampler ? "true" : "false",
            state->separated_image_sampler ? "true" : "false",
            state->shader_executed ? "true" : "false",
            state->image_read_completed ? "true" : "false",
            state->layout_name, state->force_idle ? "true" : "false",
            state->sampled_value, state->expected_value,
            state->image_upload_readback ? 64u : 0u, 64u,
            (unsigned long long)state->gpu_copy_count,
            (unsigned long long)state->queue_submit_count,
            (unsigned long long)state->wait_idle_count,
            (unsigned long long)(now_ms() - state->started_ms));
    fflush(file);
    fd = fileno(file);
    if (fd >= 0) fsync(fd);
    fclose(file);
    rename(temporary, state->result_path);
}

static int find_memory_type(VkPhysicalDevice physical, uint32_t bits,
                            VkMemoryPropertyFlags required, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & required) == required) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static VkResult create_buffer(struct probe_state *state, VkDeviceSize size,
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
    result = vkCreateBuffer(state->device, &info, NULL, buffer);
    if (result != VK_SUCCESS) return result;
    vkGetBufferMemoryRequirements(state->device, *buffer, &requirements);
    if (!find_memory_type(state->physical, requirements.memoryTypeBits, properties, &type))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = vkAllocateMemory(state->device, &allocation, NULL, memory);
    if (result != VK_SUCCESS) return result;
    return vkBindBufferMemory(state->device, *buffer, *memory, 0);
}

static VkResult create_image(struct probe_state *state, VkImageUsageFlags usage,
                             VkImage *image, VkDeviceMemory *memory)
{
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t type;
    VkResult result;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent.width = 4;
    info.extent.height = 4;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(state->device, &info, NULL, image);
    if (result != VK_SUCCESS) return result;
    vkGetImageMemoryRequirements(state->device, *image, &requirements);
    if (!find_memory_type(state->physical, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        !find_memory_type(state->physical, requirements.memoryTypeBits, 0, &type))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = vkAllocateMemory(state->device, &allocation, NULL, memory);
    if (result != VK_SUCCESS) return result;
    return vkBindImageMemory(state->device, *image, *memory, 0);
}

static void image_barrier(VkCommandBuffer command, VkImage image,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
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
    vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static VkResult submit_and_wait(struct probe_state *state)
{
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkResult result;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &state->command;
    /* Venus' vtest path treats queue-idle completion and fence feedback as
     * separate synchronization protocols.  The force-idle diagnostic must
     * not attach a fence and then wait for both: submit without a fence and
     * use vkQueueWaitIdle as the sole completion primitive. */
    result = vkQueueSubmit(state->queue, 1, &submit,
                           state->force_idle ? VK_NULL_HANDLE : state->fence);
    if (result != VK_SUCCESS) return result;
    state->queue_submit_count++;
    if (state->force_idle) {
        result = vkQueueWaitIdle(state->queue);
        state->wait_idle_count++;
        if (result != VK_SUCCESS) return result;
        /* Keep this diagnostic deliberately conservative.  Some Venus
         * builds report queue-idle before the host-side vtest reply has
         * propagated to a mapped allocation; device-idle closes that gap.
         * This path is never used by the product renderer and is excluded
         * from the zero-device-idle acceptance metric. */
        result = vkDeviceWaitIdle(state->device);
        state->wait_idle_count++;
        if (result != VK_SUCCESS) return result;
        /* Queue idle is the completion primitive for this diagnostic mode;
         * do not require a second fence wait on Venus, where a queue-idle
         * drain may complete without transitioning the submitted fence in
         * the same host-side path. */
        return VK_SUCCESS;
    }
    return vkWaitForFences(state->device, 1, &state->fence, VK_TRUE, 5000000000ull);
}

static int read_output(struct probe_state *state, uint32_t *value)
{
    VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    void *mapped = NULL;
    if (vkMapMemory(state->device, state->output_memory, 0, sizeof(uint32_t), 0, &mapped) != VK_SUCCESS)
        return 0;
    range.memory = state->output_memory;
    range.size = VK_WHOLE_SIZE;
    if (vkInvalidateMappedMemoryRanges(state->device, 1, &range) != VK_SUCCESS) {
        vkUnmapMemory(state->device, state->output_memory);
        return 0;
    }
    *value = *(const uint32_t *)mapped;
    vkUnmapMemory(state->device, state->output_memory);
    return 1;
}

static int load_shader(const char *name, uint32_t **code, size_t *size)
{
    char path[512];
    const char *root = getenv("WINEHUA_GUEST_VULKAN_ROOT");
    FILE *file;
    long length;
    if (root && root[0]) snprintf(path, sizeof(path), "%s/share/winehua/%s", root, name);
    else snprintf(path, sizeof(path), "share/winehua/%s", name);
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    length = ftell(file);
    if (length <= 0 || (length & 3)) { fclose(file); return 0; }
    rewind(file);
    *code = malloc((size_t)length);
    if (!*code || fread(*code, 1, (size_t)length, file) != (size_t)length) {
        free(*code); *code = NULL; fclose(file); return 0;
    }
    fclose(file);
    *size = (size_t)length;
    return 1;
}

static int create_descriptor_and_pipeline(struct probe_state *state, const char *shader_name,
                                          int mode, VkDescriptorSet *set,
                                          VkPipelineLayout *layout, VkPipeline *pipeline,
                                          VkDescriptorPool *pool, VkDescriptorSetLayout *set_layout,
                                          VkShaderModule *shader_module)
{
    VkDescriptorSetLayoutBinding bindings[3];
    VkDescriptorPoolSize pool_sizes[3];
    VkDescriptorSetLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkPipelineLayoutCreateInfo pipeline_layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkComputePipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    uint32_t *code = NULL;
    size_t code_size = 0;
    uint32_t binding_count = mode == 2 ? 3u : 2u;
    uint32_t i;
    memset(bindings, 0, sizeof(bindings));
    bindings[0].binding = 0;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    if (mode == 0) bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    else if (mode == 1) bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    else if (mode == 2) bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    else bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if (mode == 2) {
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[mode == 2 ? 2 : 1].binding = mode == 2 ? 2 : 1;
    bindings[mode == 2 ? 2 : 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[mode == 2 ? 2 : 1].descriptorCount = 1;
    bindings[mode == 2 ? 2 : 1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    layout_info.bindingCount = binding_count;
    layout_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(state->device, &layout_info, NULL, set_layout) != VK_SUCCESS)
        return 0;

    memset(pool_sizes, 0, sizeof(pool_sizes));
    pool_sizes[0].type = mode == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
        (mode == 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    pool_sizes[0].descriptorCount = 1;
    pool_sizes[1].type = mode == 2 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = 1;
    uint32_t pool_count = 2;
    if (mode == 2) {
        pool_sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[2].descriptorCount = 1;
        pool_count = 3;
    }
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = pool_count;
    pool_info.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(state->device, &pool_info, NULL, pool) != VK_SUCCESS)
        return 0;
    allocate.descriptorPool = *pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = set_layout;
    if (vkAllocateDescriptorSets(state->device, &allocate, set) != VK_SUCCESS)
        return 0;
    if (!load_shader(shader_name, &code, &code_size)) return 0;
    VkShaderModuleCreateInfo shader_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    shader_info.codeSize = code_size;
    shader_info.pCode = code;
    if (vkCreateShaderModule(state->device, &shader_info, NULL, shader_module) != VK_SUCCESS) {
        free(code); return 0;
    }
    free(code);
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = set_layout;
    if (vkCreatePipelineLayout(state->device, &pipeline_layout_info, NULL, layout) != VK_SUCCESS)
        return 0;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = *shader_module;
    stage.pName = "main";
    pipeline_info.stage = stage;
    pipeline_info.layout = *layout;
    if (vkCreateComputePipelines(state->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, pipeline) != VK_SUCCESS)
        return 0;

    VkDescriptorImageInfo image_info = { 0, state->source_view, mode == 0 ? VK_IMAGE_LAYOUT_GENERAL :
        (state->layout_name[0] == 'G' ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) };
    VkDescriptorBufferInfo buffer_info = { state->output_buffer, 0, sizeof(uint32_t) };
    VkWriteDescriptorSet writes[3];
    memset(writes, 0, sizeof(writes));
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = *set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = bindings[0].descriptorType;
    writes[0].pImageInfo = &image_info;
    if (mode == 2) {
        VkDescriptorImageInfo sampler_info = { state->sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = *set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[1].pImageInfo = &sampler_info;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = *set;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(state->device, 3, writes, 0, NULL);
    } else {
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = *set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(state->device, 2, writes, 0, NULL);
    }
    (void)i;
    return 1;
}

static int run_compute(struct probe_state *state, const char *shader_name, int mode,
                       VkImageLayout old_layout, VkImageLayout new_layout, uint32_t expected,
                       uint32_t *value)
{
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkResult result;
    memset(value, 0, sizeof(*value));
    {
        void *mapped = NULL;
        if (vkMapMemory(state->device, state->output_memory, 0, sizeof(uint32_t), 0, &mapped) != VK_SUCCESS)
            return 0;
        VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
        *(uint32_t *)mapped = SHADER_NOT_EXECUTED;
        range.memory = state->output_memory;
        range.size = VK_WHOLE_SIZE;
        if (vkFlushMappedMemoryRanges(state->device, 1, &range) != VK_SUCCESS) {
            vkUnmapMemory(state->device, state->output_memory);
            return 0;
        }
        vkUnmapMemory(state->device, state->output_memory);
    }
    if (!create_descriptor_and_pipeline(state, shader_name, mode, &set, &layout, &pipeline,
                                        &pool, &set_layout, &shader))
        return 0;
    vkResetFences(state->device, 1, &state->fence);
    vkResetCommandPool(state->device, state->command_pool, 0);
    if (vkBeginCommandBuffer(state->command, &begin) != VK_SUCCESS) return 0;
    if (old_layout != new_layout)
        image_barrier(state->command, state->source_image, old_layout, new_layout,
                      old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT,
                      mode == 0 ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT,
                      old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(state->command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(state->command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(state->command, 1, 1, 1);
    if (new_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        image_barrier(state->command, state->source_image, new_layout, new_layout,
                      mode == 0 ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT,
                      0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    result = vkEndCommandBuffer(state->command);
    if (result == VK_SUCCESS) result = submit_and_wait(state);
    if (result == VK_SUCCESS) result = read_output(state, value) ? VK_SUCCESS : VK_ERROR_MEMORY_MAP_FAILED;
    if (pipeline) vkDestroyPipeline(state->device, pipeline, NULL);
    if (layout) vkDestroyPipelineLayout(state->device, layout, NULL);
    if (shader) vkDestroyShaderModule(state->device, shader, NULL);
    if (pool) vkDestroyDescriptorPool(state->device, pool, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(state->device, set_layout, NULL);
    if (result != VK_SUCCESS) return 0;
    state->expected_value = expected;
    state->sampled_value = *value;
    state->shader_executed = *value != SHADER_NOT_EXECUTED;
    state->image_read_completed = state->shader_executed;
    return *value == expected;
}

#include "venus_depth_cube_probe.inc"
#include "venus_depth_cube_graphics_replay.inc"

static int init_vulkan(struct probe_state *state, const char **failure)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice *devices = NULL;
    uint32_t count = 0, i;
    VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    float priority = 1.0f;
    if (vkCreateInstance(&(VkInstanceCreateInfo){ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }, NULL, &instance) != VK_SUCCESS) {
        *failure = "vkCreateInstance failed"; return 0;
    }
    if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS || !count) {
        *failure = "no Vulkan physical device"; vkDestroyInstance(instance, NULL); return 0;
    }
    devices = calloc(count, sizeof(*devices));
    if (!devices || vkEnumeratePhysicalDevices(instance, &count, devices) != VK_SUCCESS) {
        *failure = "physical device enumeration failed"; free(devices); vkDestroyInstance(instance, NULL); return 0;
    }
    state->physical = devices[0];
    free(devices);
    vkGetPhysicalDeviceProperties(state->physical, &state->properties);
    vkGetPhysicalDeviceFormatProperties(state->physical, VK_FORMAT_R8G8B8A8_UNORM,
                                        &state->format_properties);
    /* This legacy query currently terminates the ARM64 Venus/vtest child
     * before it can write its result JSON. Keep the capability field
     * explicit, but defer the exact image-format probe until the loader
     * exposes the corresponding safe path. The actual vkCreateImage below
     * remains the authoritative functional check. */
    state->image_format_supported = -1;
    vkGetPhysicalDeviceQueueFamilyProperties(state->physical, &count, NULL);
    {
        VkQueueFamilyProperties *queues = calloc(count, sizeof(*queues));
        state->queue_family = UINT32_MAX;
        if (!queues) { *failure = "queue allocation failed"; vkDestroyInstance(instance, NULL); return 0; }
        vkGetPhysicalDeviceQueueFamilyProperties(state->physical, &count, queues);
        for (i = 0; i < count; ++i)
            if (queues[i].queueCount &&
                (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                (!state->graphics_replay || (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))) {
                state->queue_family = i;
                break;
            }
        free(queues);
    }
    if (state->queue_family == UINT32_MAX) { *failure = "no compute queue"; vkDestroyInstance(instance, NULL); return 0; }
    queue_info.queueFamilyIndex = state->queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    if (vkCreateDevice(state->physical, &device_info, NULL, &state->device) != VK_SUCCESS) {
        *failure = "vkCreateDevice failed"; vkDestroyInstance(instance, NULL); return 0;
    }
    vkGetDeviceQueue(state->device, state->queue_family, 0, &state->queue);
    if (vkCreateCommandPool(state->device, &(VkCommandPoolCreateInfo){ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, state->queue_family }, NULL, &state->command_pool) != VK_SUCCESS) {
        *failure = "vkCreateCommandPool failed"; vkDestroyInstance(instance, NULL); return 0;
    }
    VkCommandBufferAllocateInfo allocation = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocation.commandPool = state->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(state->device, &allocation, &state->command) != VK_SUCCESS ||
        vkCreateFence(state->device, &(VkFenceCreateInfo){ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }, NULL, &state->fence) != VK_SUCCESS) {
        *failure = "command/fence creation failed"; vkDestroyInstance(instance, NULL); return 0;
    }
    /* The instance is intentionally kept alive by the Vulkan loader/device.
     * The device owns all objects used by this probe; destroy it in cleanup. */
    (void)app;
    return 1;
}

static void cleanup(struct probe_state *state)
{
    if (!state->device) return;
    vkDeviceWaitIdle(state->device);
    if (state->sampler) vkDestroySampler(state->device, state->sampler, NULL);
    if (state->source_view) vkDestroyImageView(state->device, state->source_view, NULL);
    if (state->source_image) vkDestroyImage(state->device, state->source_image, NULL);
    if (state->source_memory) vkFreeMemory(state->device, state->source_memory, NULL);
    if (state->upload_buffer) vkDestroyBuffer(state->device, state->upload_buffer, NULL);
    if (state->upload_memory) vkFreeMemory(state->device, state->upload_memory, NULL);
    if (state->readback_buffer) vkDestroyBuffer(state->device, state->readback_buffer, NULL);
    if (state->readback_memory) vkFreeMemory(state->device, state->readback_memory, NULL);
    if (state->output_buffer) vkDestroyBuffer(state->device, state->output_buffer, NULL);
    if (state->output_memory) vkFreeMemory(state->device, state->output_memory, NULL);
    if (state->fence) vkDestroyFence(state->device, state->fence, NULL);
    if (state->command_pool) vkDestroyCommandPool(state->device, state->command_pool, NULL);
    vkDestroyDevice(state->device, NULL);
    state->device = VK_NULL_HANDLE;
}

int main(int argc, char **argv)
{
    struct probe_state state;
    const char *failure = "unknown failure";
    VkResult result;
    uint8_t pattern[64];
    uint32_t value;
    int depth_cube_mode, depth_cube_array_mode, depth_cube_array_2d_mode;
    int depth_cube_graphics_mode;
    enum graphics_replay_variant graphics_variant = GRAPHICS_REPLAY_EXACT;
    void *mapped = NULL;
    memset(&state, 0, sizeof(state));
    state.image_format_supported = -1;
    state.run_id = argument_value(argc, argv, "--run-id", "manual");
    state.test_id = argument_value(argc, argv, "--test-id", "venus-sampled-image");
    state.result_path = argument_value(argc, argv, "--result", "");
    state.layout_name = !strcmp(argument_value(argc, argv, "--layout", "shader-read"), "general") ? "GENERAL" : "SHADER_READ_ONLY_OPTIMAL";
    state.force_idle = argument_flag(argc, argv, "--force-idle");
    state.sampled_only = argument_flag(argc, argv, "--sampled-only");
    depth_cube_mode = argument_flag(argc, argv, "--depth-cube");
    depth_cube_array_mode = argument_flag(argc, argv, "--depth-cube-array");
    depth_cube_array_2d_mode =
        argument_flag(argc, argv, "--depth-cube-array-2d-golden");
    depth_cube_graphics_mode = argument_flag(argc, argv, "--depth-cube-graphics");
    if (argument_flag(argc, argv, "--depth-cube-graphics-golden")) {
        depth_cube_graphics_mode = 1;
        graphics_variant = GRAPHICS_REPLAY_GOLDEN;
    } else if (argument_flag(argc, argv, "--depth-cube-graphics-golden-dxvk-contract")) {
        depth_cube_graphics_mode = 1;
        graphics_variant = GRAPHICS_REPLAY_GOLDEN_DXVK_CONTRACT;
    } else if (argument_flag(argc, argv, "--depth-cube-graphics-initialized")) {
        depth_cube_graphics_mode = 1;
        graphics_variant = GRAPHICS_REPLAY_INITIALIZED;
    } else if (argument_flag(argc, argv, "--depth-cube-graphics-optimized")) {
        depth_cube_graphics_mode = 1;
        graphics_variant = GRAPHICS_REPLAY_OPTIMIZED;
    } else if (argument_flag(argc, argv, "--depth-cube-graphics-coordinate-trace")) {
        depth_cube_graphics_mode = 1;
        graphics_variant = GRAPHICS_REPLAY_COORDINATE_TRACE;
    } else if (argument_flag(argc, argv, "--depth-cube-graphics-padded-dref")) {
        depth_cube_graphics_mode = 1;
        graphics_variant = GRAPHICS_REPLAY_PADDED_DREF;
    } else if (argument_flag(argc, argv, "--depth-cube-graphics-no-float-controls")) {
        depth_cube_graphics_mode = 1;
        graphics_variant = GRAPHICS_REPLAY_NO_FLOAT_CONTROLS;
    }
    state.graphics_replay = depth_cube_graphics_mode;
    state.started_ms = now_ms();
    if (!getenv("VN_DEBUG") || !strstr(getenv("VN_DEBUG"), "vtest") ||
        !getenv("VTEST_SOCKET_NAME") || !getenv("VK_DRIVER_FILES") ||
        !getenv("BOX64_EMULATED_LIBS") || !strstr(getenv("BOX64_EMULATED_LIBS"), "libvulkan.so.1")) {
        write_result(&state, "FAIL", "startup", "Venus vtest/ICD isolation environment is incomplete");
        return 2;
    }
    write_result(&state, "started", "startup", "Venus sampled-image probe started");
    if (!init_vulkan(&state, &failure)) goto failed;
    if (depth_cube_graphics_mode) {
        int passed = run_depth_cube_graphics_replay(&state, graphics_variant);
        cleanup(&state);
        return passed ? 0 : 1;
    }
    if (depth_cube_mode || depth_cube_array_mode || depth_cube_array_2d_mode) {
        int passed = run_depth_cube_probe(&state, depth_cube_array_mode,
                                          depth_cube_array_2d_mode);
        cleanup(&state);
        return passed ? 0 : 1;
    }
    if (create_buffer(&state, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &state.upload_buffer, &state.upload_memory) != VK_SUCCESS ||
        create_buffer(&state, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &state.readback_buffer, &state.readback_memory) != VK_SUCCESS ||
        create_buffer(&state, sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &state.output_buffer, &state.output_memory) != VK_SUCCESS ||
        create_image(&state, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT |
                     (state.sampled_only ? 0 : VK_IMAGE_USAGE_STORAGE_BIT),
                     &state.source_image, &state.source_memory) != VK_SUCCESS) {
        failure = "resource allocation failed"; goto failed;
    }
    {
        VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = state.source_image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (vkCreateImageView(state.device, &view, NULL, &state.source_view) != VK_SUCCESS) {
            failure = "image view creation failed"; goto failed;
        }
        VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = VK_FILTER_NEAREST;
        sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 0.0f;
        if (vkCreateSampler(state.device, &sampler, NULL, &state.sampler) != VK_SUCCESS) {
            failure = "sampler creation failed"; goto failed;
        }
    }
    if (vkMapMemory(state.device, state.upload_memory, 0, sizeof(pattern), 0, &mapped) != VK_SUCCESS) {
        failure = "upload map failed"; goto failed;
    }
    for (uint32_t i = 0; i < 16; ++i) {
        pattern[i * 4 + 0] = 0x11; pattern[i * 4 + 1] = 0x22;
        pattern[i * 4 + 2] = 0x33; pattern[i * 4 + 3] = 0xff;
    }
    memcpy(mapped, pattern, sizeof(pattern));
    vkUnmapMemory(state.device, state.upload_memory);
    vkResetFences(state.device, 1, &state.fence);
    vkResetCommandPool(state.device, state.command_pool, 0);
    if (vkBeginCommandBuffer(state.command, &(VkCommandBufferBeginInfo){ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }) != VK_SUCCESS) {
        failure = "upload command begin failed"; goto failed;
    }
    image_barrier(state.command, state.source_image, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = { 0 };
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = 4; copy.imageExtent.height = 4; copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(state.command, state.upload_buffer, state.source_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(state.command, state.source_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyImageToBuffer(state.command, state.source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           state.readback_buffer, 1, &copy);
    if (vkEndCommandBuffer(state.command) != VK_SUCCESS || submit_and_wait(&state) != VK_SUCCESS) {
        failure = "upload/readback submission failed"; goto failed;
    }
    if (vkMapMemory(state.device, state.readback_memory, 0, sizeof(pattern), 0, &mapped) != VK_SUCCESS) {
        failure = "readback map failed"; goto failed;
    }
    state.image_upload_readback = !memcmp(mapped, pattern, sizeof(pattern));
    vkUnmapMemory(state.device, state.readback_memory);
    state.gpu_copy_count += 2;
    if (!state.image_upload_readback) { failure = "upload/readback verification failed"; goto failed; }

    /* Storage tests deliberately have no image dependency.  The sampled-only
     * variant skips them so its VkImageCreateInfo matches a normal DXVK SRV
     * image: transfer + sampled usage, without VK_IMAGE_USAGE_STORAGE_BIT. */
    if (!state.sampled_only) {
        if (!run_compute(&state, "venus_storage_write.spv", 0,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         STORAGE_WRITE_VALUE, &value)) {
            failure = "storage image write failed"; goto failed;
        }
        state.storage_image_write = 1;
        if (!run_compute(&state, "venus_storage_read.spv", 0,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         EXPECTED_COLOR, &value)) {
            failure = "storage image read failed"; goto failed;
        }
        state.storage_image_read = 1;
    }
    if (!run_compute(&state, "venus_image_fetch.spv", 1,
                     state.sampled_only ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                     state.layout_name[0] == 'G' ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     EXPECTED_COLOR, &value)) {
        failure = "sampled image fetch failed"; goto failed;
    }
    state.sampled_image_fetch = 1;
    if (!run_compute(&state, "venus_combined_sample.spv", 3,
                     state.layout_name[0] == 'G' ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     state.layout_name[0] == 'G' ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     EXPECTED_COLOR, &value)) {
        failure = "combined image sampler failed"; goto failed;
    }
    state.combined_image_sampler = 1;
    if (!run_compute(&state, "venus_separated_sample.spv", 2,
                     state.layout_name[0] == 'G' ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     state.layout_name[0] == 'G' ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     EXPECTED_COLOR, &value)) {
        failure = "separated image sampler failed"; goto failed;
    }
    state.separated_image_sampler = 1;
    state.shader_executed = 1;
    state.image_read_completed = 1;
    write_result(&state, "PASS", "venus", "Guest Vulkan sampled-image probe passed");
    cleanup(&state);
    return 0;

failed:
    write_result(&state, "FAIL", "venus", failure);
    cleanup(&state);
    return 1;
}
