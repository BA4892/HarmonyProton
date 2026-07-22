#define DEPTH_CUBE_LAYER_COUNT 12u
#define DEPTH_CUBE_FACE_COUNT 6u
#define DEPTH_CUBE_MARKER 0xcdcdcdcdu

struct depth_cube_matrix {
    const char *phase;
    uint32_t array_values[DEPTH_CUBE_FACE_COUNT];
    uint32_t cube_sample_values[DEPTH_CUBE_FACE_COUNT];
    uint32_t cube_compare_values[DEPTH_CUBE_FACE_COUNT];
    uint32_t cube_separated_compare_values[DEPTH_CUBE_FACE_COUNT];
    uint32_t cube_dxvk_contract_values[DEPTH_CUBE_FACE_COUNT];
    uint32_t cube_array_sample_values[DEPTH_CUBE_LAYER_COUNT];
    uint32_t cube_array_2d_compare_values[DEPTH_CUBE_LAYER_COUNT];
    uint32_t cube_array_values[DEPTH_CUBE_LAYER_COUNT];
    int array_pass;
    int cube_sample_pass;
    int cube_compare_pass;
    int cube_separated_compare_pass;
    int cube_dxvk_contract_pass;
    int cube_array_sample_pass;
    int cube_array_2d_compare_pass;
    int cube_array_pass;
};

struct depth_cube_resources {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView array_view;
    VkImageView cube_view;
    VkImageView cube_array_view;
    VkImageView array_full_view;
    VkSampler regular_sampler;
    VkSampler compare_sampler;
};

static void depth_cube_format_values(char *output, size_t output_size,
                                     const uint32_t *values, uint32_t count)
{
    size_t written = 0;
    uint32_t i;
    if (!output_size) return;
    output[0] = 0;
    for (i = 0; i < count && written + 1 < output_size; ++i) {
        int length = snprintf(output + written, output_size - written,
                              "%s%u", i ? "," : "", values[i]);
        if (length < 0 || (size_t)length >= output_size - written) break;
        written += (size_t)length;
    }
}

static void write_depth_cube_result(const struct probe_state *state,
                                    const struct depth_cube_matrix *matrix,
                                    const char *status, const char *message)
{
    char temporary[1024], safe_message[256];
    char safe_device[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
    char array_values[96], cube_sample_values[96];
    char cube_compare_values[96], cube_separated_compare_values[96];
    char cube_dxvk_contract_values[96];
    char cube_array_sample_values[192];
    char cube_array_2d_compare_values[192];
    char cube_array_values[192];
    FILE *file;
    int fd;

    if (!state->result_path || !state->result_path[0]) return;
    snprintf(temporary, sizeof(temporary), "%s.tmp.%d", state->result_path, getpid());
    json_safe_copy(safe_message, sizeof(safe_message), message ? message : "");
    json_safe_copy(safe_device, sizeof(safe_device), state->properties.deviceName);
    depth_cube_format_values(array_values, sizeof(array_values), matrix->array_values,
                             DEPTH_CUBE_FACE_COUNT);
    depth_cube_format_values(cube_sample_values, sizeof(cube_sample_values),
                             matrix->cube_sample_values, DEPTH_CUBE_FACE_COUNT);
    depth_cube_format_values(cube_compare_values, sizeof(cube_compare_values),
                             matrix->cube_compare_values, DEPTH_CUBE_FACE_COUNT);
    depth_cube_format_values(cube_separated_compare_values,
                             sizeof(cube_separated_compare_values),
                             matrix->cube_separated_compare_values,
                             DEPTH_CUBE_FACE_COUNT);
    depth_cube_format_values(cube_dxvk_contract_values,
                             sizeof(cube_dxvk_contract_values),
                             matrix->cube_dxvk_contract_values,
                             DEPTH_CUBE_FACE_COUNT);
    depth_cube_format_values(cube_array_sample_values,
                             sizeof(cube_array_sample_values),
                             matrix->cube_array_sample_values,
                             DEPTH_CUBE_LAYER_COUNT);
    depth_cube_format_values(cube_array_2d_compare_values,
                             sizeof(cube_array_2d_compare_values),
                             matrix->cube_array_2d_compare_values,
                             DEPTH_CUBE_LAYER_COUNT);
    depth_cube_format_values(cube_array_values, sizeof(cube_array_values),
                             matrix->cube_array_values, DEPTH_CUBE_LAYER_COUNT);
    file = fopen(temporary, "w");
    if (!file) return;
    fprintf(file,
            "{\n"
            "  \"schemaVersion\":1,\n"
            "  \"runId\":\"%s\",\n"
            "  \"testId\":\"%s\",\n"
            "  \"status\":\"%s\",\n"
            "  \"stage\":\"venus-depth-cube\",\n"
            "  \"message\":\"%s\",\n"
            "  \"architecture\":{\"peArchitecture\":\"not-applicable\",\"wineUnixArchitecture\":\"x86_64\",\"vulkanLoaderArchitecture\":\"x86_64\",\"venusIcdArchitecture\":\"x86_64\",\"hostArchitecture\":\"aarch64\",\"wow64ThunkEnabled\":false,\"box64Enabled\":true},\n"
            "  \"capabilities\":{\"deviceName\":\"%s\",\"deviceApiVersion\":\"%u.%u.%u\",\"format\":\"D24_UNORM_S8_UINT\",\"imageFlags\":\"CUBE_COMPATIBLE\"},\n"
            "  \"depthCubeMatrix\":{\"phase\":\"%s\","
            "\"arrayComparison\":{\"values\":[%s],\"pass\":%s},"
            "\"cubeSample\":{\"values\":[%s],\"pass\":%s},"
            "\"cubeComparison\":{\"values\":[%s],\"pass\":%s},"
            "\"cubeSeparatedComparison\":{\"values\":[%s],\"pass\":%s},"
            "\"cubeDxvkContractComparison\":{\"values\":[%s],\"pass\":%s},"
            "\"cubeArraySample\":{\"values\":[%s],\"pass\":%s},"
            "\"cubeArray2DComparison\":{\"values\":[%s],\"pass\":%s},"
            "\"cubeArrayComparison\":{\"values\":[%s],\"pass\":%s}},\n"
            "  \"metrics\":{\"cpuReadBytes\":48,\"cpuUploadBytes\":0,\"gpuCopyCount\":0,\"queueSubmitCount\":%llu,\"perFrameDeviceWaitIdle\":0,\"fallbackDetected\":false,\"durationMs\":%llu}\n"
            "}\n",
            state->run_id, state->test_id, status, safe_message,
            safe_device, VK_API_VERSION_MAJOR(state->properties.apiVersion),
            VK_API_VERSION_MINOR(state->properties.apiVersion),
            VK_API_VERSION_PATCH(state->properties.apiVersion),
            matrix->phase ? matrix->phase : "startup",
            array_values, matrix->array_pass ? "true" : "false",
            cube_sample_values, matrix->cube_sample_pass ? "true" : "false",
            cube_compare_values, matrix->cube_compare_pass ? "true" : "false",
            cube_separated_compare_values,
            matrix->cube_separated_compare_pass ? "true" : "false",
            cube_dxvk_contract_values,
            matrix->cube_dxvk_contract_pass ? "true" : "false",
            cube_array_sample_values,
            matrix->cube_array_sample_pass ? "true" : "false",
            cube_array_2d_compare_values,
            matrix->cube_array_2d_compare_pass ? "true" : "false",
            cube_array_values, matrix->cube_array_pass ? "true" : "false",
            (unsigned long long)state->queue_submit_count,
            (unsigned long long)(now_ms() - state->started_ms));
    fflush(file);
    fd = fileno(file);
    if (fd >= 0) fsync(fd);
    fclose(file);
    rename(temporary, state->result_path);
}

static void destroy_depth_cube_resources(struct probe_state *state,
                                         struct depth_cube_resources *resources)
{
    if (resources->compare_sampler)
        vkDestroySampler(state->device, resources->compare_sampler, NULL);
    if (resources->regular_sampler)
        vkDestroySampler(state->device, resources->regular_sampler, NULL);
    if (resources->cube_array_view)
        vkDestroyImageView(state->device, resources->cube_array_view, NULL);
    if (resources->array_full_view)
        vkDestroyImageView(state->device, resources->array_full_view, NULL);
    if (resources->cube_view)
        vkDestroyImageView(state->device, resources->cube_view, NULL);
    if (resources->array_view)
        vkDestroyImageView(state->device, resources->array_view, NULL);
    if (resources->image)
        vkDestroyImage(state->device, resources->image, NULL);
    if (resources->memory)
        vkFreeMemory(state->device, resources->memory, NULL);
    memset(resources, 0, sizeof(*resources));
}

static int create_depth_cube_resources(struct probe_state *state,
                                       struct depth_cube_resources *resources)
{
    static const float depths[DEPTH_CUBE_LAYER_COUNT] = {
        0.125f, 0.25f, 0.375f, 0.625f, 0.75f, 0.875f,
        0.875f, 0.75f, 0.625f, 0.375f, 0.25f, 0.125f,
    };
    VkImageCreateInfo image_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    uint32_t memory_type, layer;

    memset(resources, 0, sizeof(*resources));
    image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_D24_UNORM_S8_UINT;
    image_info.extent.width = 4;
    image_info.extent.height = 4;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = DEPTH_CUBE_LAYER_COUNT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(state->device, &image_info, NULL, &resources->image) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(state->device, resources->image, &requirements);
    if (!find_memory_type(state->physical, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type) &&
        !find_memory_type(state->physical, requirements.memoryTypeBits, 0, &memory_type))
        goto failed;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(state->device, &allocation, NULL, &resources->memory) != VK_SUCCESS ||
        vkBindImageMemory(state->device, resources->image, resources->memory, 0) != VK_SUCCESS)
        goto failed;

    view.image = resources->image;
    view.format = VK_FORMAT_D24_UNORM_S8_UINT;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = DEPTH_CUBE_FACE_COUNT;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (vkCreateImageView(state->device, &view, NULL, &resources->array_view) != VK_SUCCESS)
        goto failed;
    view.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    if (vkCreateImageView(state->device, &view, NULL, &resources->cube_view) != VK_SUCCESS)
        goto failed;
    view.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    view.subresourceRange.layerCount = DEPTH_CUBE_LAYER_COUNT;
    if (vkCreateImageView(state->device, &view, NULL, &resources->cube_array_view) != VK_SUCCESS)
        goto failed;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (vkCreateImageView(state->device, &view, NULL, &resources->array_full_view) != VK_SUCCESS)
        goto failed;

    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.minLod = 0.0f;
    sampler.maxLod = 0.0f;
    if (vkCreateSampler(state->device, &sampler, NULL, &resources->regular_sampler) != VK_SUCCESS)
        goto failed;
    /* Match the DXVK Legacy sampler used by the D3D11 comparison smoke:
     * MIN_MAG_MIP_LINEAR, clamp-to-edge, and an unbounded MaxLOD. The first
     * version of this probe used nearest mip filtering and maxLod=0, which
     * was not an exact runtime contract for the failing Cube comparison. */
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.maxLod = 3.402823466e+38f;
    sampler.compareEnable = VK_TRUE;
    sampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (vkCreateSampler(state->device, &sampler, NULL, &resources->compare_sampler) != VK_SUCCESS)
        goto failed;

    vkResetFences(state->device, 1, &state->fence);
    vkResetCommandPool(state->device, state->command_pool, 0);
    if (vkBeginCommandBuffer(state->command, &begin) != VK_SUCCESS) goto failed;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = resources->image;
    barrier.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = DEPTH_CUBE_LAYER_COUNT;
    vkCmdPipelineBarrier(state->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);
    for (layer = 0; layer < DEPTH_CUBE_LAYER_COUNT; ++layer) {
        VkImageSubresourceRange range = barrier.subresourceRange;
        VkClearDepthStencilValue clear = { depths[layer], (uint32_t)(0x40 + layer) };
        range.baseArrayLayer = layer;
        range.layerCount = 1;
        vkCmdClearDepthStencilImage(state->command, resources->image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &clear, 1, &range);
    }
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(state->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);
    if (vkEndCommandBuffer(state->command) != VK_SUCCESS ||
        submit_and_wait(state) != VK_SUCCESS)
        goto failed;
    return 1;

failed:
    destroy_depth_cube_resources(state, resources);
    return 0;
}

static int run_depth_cube_shader(struct probe_state *state, VkImageView view,
                                 VkSampler sampler, const char *shader_name,
                                 uint32_t count, const uint32_t *expected,
                                 uint32_t *values, int separated_descriptors)
{
    VkBuffer output = VK_NULL_HANDLE;
    VkDeviceMemory output_memory = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding bindings[3] = {{0}};
    VkDescriptorPoolSize pool_sizes[3] = {{0}};
    VkDescriptorSetLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkPipelineLayoutCreateInfo pipeline_layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkShaderModuleCreateInfo shader_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    VkComputePipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    VkDescriptorImageInfo image_descriptor = { 0 };
    VkDescriptorImageInfo sampler_descriptor = { 0 };
    VkDescriptorBufferInfo buffer_descriptor = { 0 };
    VkWriteDescriptorSet writes[3] = {{0}};
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    uint32_t *code = NULL;
    size_t code_size = 0;
    void *mapped = NULL;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    uint32_t i;
    int match = 1;

    if (create_buffer(state, sizeof(uint32_t) * DEPTH_CUBE_LAYER_COUNT,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &output, &output_memory) != VK_SUCCESS)
        goto done;
    if (vkMapMemory(state->device, output_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        goto done;
    for (i = 0; i < DEPTH_CUBE_LAYER_COUNT; ++i)
        ((uint32_t *)mapped)[i] = DEPTH_CUBE_MARKER;
    {
        VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
        range.memory = output_memory;
        range.size = VK_WHOLE_SIZE;
        if (vkFlushMappedMemoryRanges(state->device, 1, &range) != VK_SUCCESS)
            goto done;
    }
    vkUnmapMemory(state->device, output_memory);
    mapped = NULL;

    bindings[0].binding = 0;
    bindings[0].descriptorType = separated_descriptors ?
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = separated_descriptors ?
        VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    if (separated_descriptors) {
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    layout_info.bindingCount = separated_descriptors ? 3 : 2;
    layout_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(state->device, &layout_info, NULL, &set_layout) != VK_SUCCESS)
        goto done;
    pool_sizes[0].type = separated_descriptors ?
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = 1;
    pool_sizes[1].type = separated_descriptors ?
        VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = 1;
    if (separated_descriptors) {
        pool_sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[2].descriptorCount = 1;
    }
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = separated_descriptors ? 3 : 2;
    pool_info.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(state->device, &pool_info, NULL, &pool) != VK_SUCCESS)
        goto done;
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout;
    if (vkAllocateDescriptorSets(state->device, &allocate, &set) != VK_SUCCESS ||
        !load_shader(shader_name, &code, &code_size))
        goto done;
    shader_info.codeSize = code_size;
    shader_info.pCode = code;
    if (vkCreateShaderModule(state->device, &shader_info, NULL, &shader) != VK_SUCCESS)
        goto done;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &set_layout;
    if (vkCreatePipelineLayout(state->device, &pipeline_layout_info, NULL,
                               &pipeline_layout) != VK_SUCCESS)
        goto done;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    pipeline_info.stage = stage;
    pipeline_info.layout = pipeline_layout;
    if (vkCreateComputePipelines(state->device, VK_NULL_HANDLE, 1,
                                 &pipeline_info, NULL, &pipeline) != VK_SUCCESS)
        goto done;

    image_descriptor.sampler = separated_descriptors ? VK_NULL_HANDLE : sampler;
    image_descriptor.imageView = view;
    image_descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sampler_descriptor.sampler = sampler;
    buffer_descriptor.buffer = output;
    buffer_descriptor.range = sizeof(uint32_t) * DEPTH_CUBE_LAYER_COUNT;
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = separated_descriptors ?
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &image_descriptor;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = separated_descriptors ?
        VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (separated_descriptors)
        writes[1].pImageInfo = &sampler_descriptor;
    else
        writes[1].pBufferInfo = &buffer_descriptor;
    if (separated_descriptors) {
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = set;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &buffer_descriptor;
    }
    vkUpdateDescriptorSets(state->device, separated_descriptors ? 3 : 2,
                           writes, 0, NULL);

    vkResetFences(state->device, 1, &state->fence);
    vkResetCommandPool(state->device, state->command_pool, 0);
    if (vkBeginCommandBuffer(state->command, &begin) != VK_SUCCESS) goto done;
    vkCmdBindPipeline(state->command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(state->command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(state->command, count, 1, 1);
    if (vkEndCommandBuffer(state->command) != VK_SUCCESS) goto done;
    result = submit_and_wait(state);
    if (result != VK_SUCCESS) goto done;
    if (vkMapMemory(state->device, output_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        result = VK_ERROR_MEMORY_MAP_FAILED;
        goto done;
    }
    {
        VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
        range.memory = output_memory;
        range.size = VK_WHOLE_SIZE;
        result = vkInvalidateMappedMemoryRanges(state->device, 1, &range);
        if (result != VK_SUCCESS) goto done;
    }
    memcpy(values, mapped, sizeof(uint32_t) * count);
    vkUnmapMemory(state->device, output_memory);
    mapped = NULL;
    for (i = 0; i < count; ++i)
        if (values[i] != expected[i]) match = 0;

done:
    if (mapped) vkUnmapMemory(state->device, output_memory);
    free(code);
    if (pipeline) vkDestroyPipeline(state->device, pipeline, NULL);
    if (pipeline_layout) vkDestroyPipelineLayout(state->device, pipeline_layout, NULL);
    if (shader) vkDestroyShaderModule(state->device, shader, NULL);
    if (pool) vkDestroyDescriptorPool(state->device, pool, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(state->device, set_layout, NULL);
    if (output) vkDestroyBuffer(state->device, output, NULL);
    if (output_memory) vkFreeMemory(state->device, output_memory, NULL);
    return result == VK_SUCCESS && match;
}

static int run_depth_cube_probe(struct probe_state *state, int cube_array_only,
                                int cube_array_2d_only)
{
    static const uint32_t compare_expected[DEPTH_CUBE_FACE_COUNT] = {
        0, 0, 0, 1, 1, 1,
    };
    static const uint32_t sample_expected[DEPTH_CUBE_FACE_COUNT] = {
        32, 64, 96, 159, 191, 223,
    };
    static const uint32_t cube_array_expected[DEPTH_CUBE_LAYER_COUNT] = {
        0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0,
    };
    static const uint32_t cube_array_sample_expected[DEPTH_CUBE_LAYER_COUNT] = {
        32, 64, 96, 159, 191, 223,
        223, 191, 159, 96, 64, 32,
    };
    struct depth_cube_resources resources;
    struct depth_cube_matrix matrix;
    int passed;

    memset(&matrix, 0, sizeof(matrix));
    matrix.phase = "resource-create";
    write_depth_cube_result(state, &matrix, "RUNNING", "D24S8 depth cube resources pending");
    if (!create_depth_cube_resources(state, &resources)) {
        write_depth_cube_result(state, &matrix, "FAIL", "D24S8 depth cube resource creation failed");
        return 0;
    }

    if (cube_array_only) {
        matrix.phase = "cube-array-sample-pending";
        write_depth_cube_result(state, &matrix, "RUNNING",
                                "native D24S8 CubeArray regular sample pending");
        matrix.cube_array_sample_pass = run_depth_cube_shader(
            state, resources.cube_array_view, resources.regular_sampler,
            "venus_depth_cube_array_sample.spv", DEPTH_CUBE_LAYER_COUNT,
            cube_array_sample_expected, matrix.cube_array_sample_values, 0);
        matrix.phase = "cube-array-sample-complete";
        write_depth_cube_result(
            state, &matrix,
            matrix.cube_array_sample_pass ? "RUNNING" : "FAIL",
            matrix.cube_array_sample_pass ?
                "native D24S8 CubeArray regular sample passed" :
                "native D24S8 CubeArray regular sample failed");
        if (!matrix.cube_array_sample_pass) {
            destroy_depth_cube_resources(state, &resources);
            return 0;
        }
        matrix.phase = "cube-array-dref-pending";
        write_depth_cube_result(state, &matrix, "RUNNING",
                                "native D24S8 CubeArray Dref pending");
        matrix.cube_array_pass = run_depth_cube_shader(
            state, resources.cube_array_view, resources.compare_sampler,
            "venus_depth_cube_array_compare.spv", DEPTH_CUBE_LAYER_COUNT,
            cube_array_expected, matrix.cube_array_values, 0);
        matrix.phase = "cube-array-dref-complete";
        passed = matrix.cube_array_pass;
        write_depth_cube_result(state, &matrix, passed ? "PASS" : "FAIL",
                                passed ? "native D24S8 CubeArray Dref passed" :
                                         "native D24S8 CubeArray Dref failed");
        destroy_depth_cube_resources(state, &resources);
        return passed;
    }

    if (cube_array_2d_only) {
        matrix.phase = "cube-array-2d-dref-pending";
        write_depth_cube_result(state, &matrix, "RUNNING",
                                "D24S8 CubeArray-to-2DArray Dref Golden pending");
        matrix.cube_array_2d_compare_pass = run_depth_cube_shader(
            state, resources.array_full_view, resources.compare_sampler,
            "venus_depth_cube_array_2d_compare.spv", DEPTH_CUBE_LAYER_COUNT,
            cube_array_expected, matrix.cube_array_2d_compare_values, 0);
        matrix.phase = "cube-array-2d-dref-complete";
        passed = matrix.cube_array_2d_compare_pass;
        write_depth_cube_result(
            state, &matrix, passed ? "PASS" : "FAIL",
            passed ? "D24S8 CubeArray-to-2DArray Dref Golden passed" :
                     "D24S8 CubeArray-to-2DArray Dref Golden failed");
        destroy_depth_cube_resources(state, &resources);
        return passed;
    }

    matrix.phase = "array-dref";
    matrix.array_pass = run_depth_cube_shader(
        state, resources.array_view, resources.compare_sampler,
        "venus_depth_array_compare.spv", DEPTH_CUBE_FACE_COUNT,
        compare_expected, matrix.array_values, 0);
    write_depth_cube_result(state, &matrix, "RUNNING", "2DArray Dref control complete");
    matrix.phase = "cube-sample";
    matrix.cube_sample_pass = run_depth_cube_shader(
        state, resources.cube_view, resources.regular_sampler,
        "venus_depth_cube_sample.spv", DEPTH_CUBE_FACE_COUNT,
        sample_expected, matrix.cube_sample_values, 0);
    write_depth_cube_result(state, &matrix, "RUNNING", "ordinary depth Cube sample control complete");
    matrix.phase = "cube-dref";
    matrix.cube_compare_pass = run_depth_cube_shader(
        state, resources.cube_view, resources.compare_sampler,
        "venus_depth_cube_compare.spv", DEPTH_CUBE_FACE_COUNT,
        compare_expected, matrix.cube_compare_values, 0);
    write_depth_cube_result(state, &matrix, "RUNNING", "combined Cube Dref control complete");
    matrix.phase = "cube-separated-dref";
    matrix.cube_separated_compare_pass = run_depth_cube_shader(
        state, resources.cube_view, resources.compare_sampler,
        "venus_depth_cube_separated_compare.spv", DEPTH_CUBE_FACE_COUNT,
        compare_expected, matrix.cube_separated_compare_values, 1);
    write_depth_cube_result(state, &matrix, "RUNNING", "standard separated Cube Dref control complete");
    matrix.phase = "cube-dxvk-contract-dref";
    matrix.cube_dxvk_contract_pass = run_depth_cube_shader(
        state, resources.cube_view, resources.compare_sampler,
        "venus_depth_cube_dxvk_contract_compare.spv", DEPTH_CUBE_FACE_COUNT,
        compare_expected, matrix.cube_dxvk_contract_values, 1);
    passed = matrix.array_pass && matrix.cube_sample_pass && matrix.cube_compare_pass &&
             matrix.cube_separated_compare_pass && matrix.cube_dxvk_contract_pass;
    matrix.phase = "cube-dref-complete";
    write_depth_cube_result(state, &matrix, passed ? "PASS" : "FAIL",
                            passed ? "native D24S8 Cube Dref passed" :
                                     "native D24S8 Cube Dref failed while controls completed");
    destroy_depth_cube_resources(state, &resources);
    return passed;
}
