/* Exact graphics replay for the captured DXVK Legacy D24 Cube comparison
 * fragment shader. This keeps the shader binary, separated sampler/image
 * bindings, comparison sampler, view type, aspect and resource state from
 * the failed DXVK contract, while using a deterministic offscreen target. */

#define GRAPHICS_REPLAY_WIDTH 64u
#define GRAPHICS_REPLAY_HEIGHT 4u
#define GRAPHICS_REPLAY_SAMPLE_COUNT 6u

enum graphics_replay_variant {
    GRAPHICS_REPLAY_EXACT = 0,
    GRAPHICS_REPLAY_GOLDEN,
    GRAPHICS_REPLAY_GOLDEN_DXVK_CONTRACT,
    GRAPHICS_REPLAY_INITIALIZED,
    GRAPHICS_REPLAY_OPTIMIZED,
    GRAPHICS_REPLAY_COORDINATE_TRACE,
    GRAPHICS_REPLAY_PADDED_DREF,
    GRAPHICS_REPLAY_NO_FLOAT_CONTROLS,
};

static const char *graphics_replay_shader(enum graphics_replay_variant variant)
{
    if (variant == GRAPHICS_REPLAY_GOLDEN)
        return "venus_depth_cube_golden.frag.spv";
    if (variant == GRAPHICS_REPLAY_GOLDEN_DXVK_CONTRACT)
        return "venus_depth_cube_golden_dxvk_contract.spv";
    if (variant == GRAPHICS_REPLAY_INITIALIZED)
        return "venus_depth_cube_fail_initialized.spv";
    if (variant == GRAPHICS_REPLAY_OPTIMIZED)
        return "venus_depth_cube_fail_optimized.spv";
    if (variant == GRAPHICS_REPLAY_COORDINATE_TRACE)
        return "venus_depth_cube_fail_coordinate_trace.spv";
    if (variant == GRAPHICS_REPLAY_PADDED_DREF)
        return "venus_depth_cube_fail_padded_dref.spv";
    if (variant == GRAPHICS_REPLAY_NO_FLOAT_CONTROLS)
        return "venus_depth_cube_fail_no_float_controls.spv";
    return "venus_depth_cube_fail.spv";
}

static const char *graphics_replay_form(enum graphics_replay_variant variant)
{
    if (variant == GRAPHICS_REPLAY_GOLDEN)
        return "hand-written-fragment-golden";
    if (variant == GRAPHICS_REPLAY_GOLDEN_DXVK_CONTRACT)
        return "hand-written-fragment-golden-dxvk-image-type-split";
    if (variant == GRAPHICS_REPLAY_INITIALIZED)
        return "dxvk-final-remapped-bool-frozen-private-zero-initialized";
    if (variant == GRAPHICS_REPLAY_OPTIMIZED)
        return "dxvk-final-remapped-bool-frozen-spirv-opt-O";
    if (variant == GRAPHICS_REPLAY_COORDINATE_TRACE)
        return "dxvk-final-remapped-coordinate-trace";
    if (variant == GRAPHICS_REPLAY_PADDED_DREF)
        return "dxvk-final-remapped-cube-dref-coordinate-vec4";
    if (variant == GRAPHICS_REPLAY_NO_FLOAT_CONTROLS)
        return "dxvk-final-remapped-no-float-controls";
    return "dxvk-final-remapped-bool-frozen";
}

static int graphics_replay_has_specialization(enum graphics_replay_variant variant)
{
    return variant == GRAPHICS_REPLAY_EXACT ||
           variant == GRAPHICS_REPLAY_INITIALIZED ||
           variant == GRAPHICS_REPLAY_OPTIMIZED ||
           variant == GRAPHICS_REPLAY_COORDINATE_TRACE ||
           variant == GRAPHICS_REPLAY_PADDED_DREF ||
           variant == GRAPHICS_REPLAY_NO_FLOAT_CONTROLS;
}

static const char *graphics_replay_start_message(enum graphics_replay_variant variant)
{
    if (variant == GRAPHICS_REPLAY_GOLDEN)
        return "hand-written Golden fragment graphics replay started";
    if (variant == GRAPHICS_REPLAY_GOLDEN_DXVK_CONTRACT)
        return "Golden fragment with DXVK split image type started";
    if (variant == GRAPHICS_REPLAY_INITIALIZED)
        return "zero-initialized DXVK fragment graphics replay started";
    if (variant == GRAPHICS_REPLAY_OPTIMIZED)
        return "spirv-opt normalized DXVK fragment graphics replay started";
    if (variant == GRAPHICS_REPLAY_COORDINATE_TRACE)
        return "DXVK Cube coordinate trace started";
    if (variant == GRAPHICS_REPLAY_PADDED_DREF)
        return "DXVK Cube Dref vec4-coordinate replay started";
    if (variant == GRAPHICS_REPLAY_NO_FLOAT_CONTROLS)
        return "DXVK replay without float-control modes started";
    return "exact DXVK fragment graphics replay started";
}

static const uint32_t graphics_replay_sample_x[GRAPHICS_REPLAY_SAMPLE_COUNT] = {
    5u, 16u, 27u, 37u, 48u, 58u,
};

static const uint32_t graphics_replay_expected[GRAPHICS_REPLAY_SAMPLE_COUNT] = {
    0xff000000u, 0xff000000u, 0xff000000u,
    0xffffffffu, 0xffffffffu, 0xffffffffu,
};

static const uint32_t graphics_replay_coordinate_expected[GRAPHICS_REPLAY_SAMPLE_COUNT] = {
    0xff8080ffu, 0xff808000u, 0xff80ff80u,
    0xff800080u, 0xffff8080u, 0xff008080u,
};

static const uint32_t *graphics_replay_expected_for_variant(
        enum graphics_replay_variant variant)
{
    return variant == GRAPHICS_REPLAY_COORDINATE_TRACE ?
        graphics_replay_coordinate_expected : graphics_replay_expected;
}

struct graphics_replay_target {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    VkBuffer sampler_info;
    VkDeviceMemory sampler_info_memory;
};

static VkResult create_graphics_replay_target(struct probe_state *state,
                                              struct graphics_replay_target *target)
{
    VkImageCreateInfo image_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    uint32_t memory_type;
    VkResult result;

    memset(target, 0, sizeof(*target));
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent.width = GRAPHICS_REPLAY_WIDTH;
    image_info.extent.height = GRAPHICS_REPLAY_HEIGHT;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(state->device, &image_info, NULL, &target->image);
    if (result != VK_SUCCESS) return result;
    vkGetImageMemoryRequirements(state->device, target->image, &requirements);
    if (!find_memory_type(state->physical, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type) &&
        !find_memory_type(state->physical, requirements.memoryTypeBits, 0, &memory_type))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(state->device, &allocation, NULL, &target->memory);
    if (result != VK_SUCCESS) return result;
    result = vkBindImageMemory(state->device, target->image, target->memory, 0);
    if (result != VK_SUCCESS) return result;

    view_info.image = target->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    result = vkCreateImageView(state->device, &view_info, NULL, &target->view);
    if (result != VK_SUCCESS) return result;

    result = create_buffer(state, GRAPHICS_REPLAY_WIDTH * GRAPHICS_REPLAY_HEIGHT * 4,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &target->readback, &target->readback_memory);
    if (result != VK_SUCCESS) return result;
    result = create_buffer(state, 32u * 4u * 4u,
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &target->sampler_info, &target->sampler_info_memory);
    if (result != VK_SUCCESS) return result;
    {
        void *mapped = NULL;
        result = vkMapMemory(state->device, target->sampler_info_memory, 0,
                             32u * 4u * 4u, 0, &mapped);
        if (result == VK_SUCCESS) {
            memset(mapped, 0, 32u * 4u * 4u);
            vkUnmapMemory(state->device, target->sampler_info_memory);
        }
    }
    return result;
}

static void destroy_graphics_replay_target(struct probe_state *state,
                                           struct graphics_replay_target *target)
{
    if (target->sampler_info) vkDestroyBuffer(state->device, target->sampler_info, NULL);
    if (target->sampler_info_memory) vkFreeMemory(state->device, target->sampler_info_memory, NULL);
    if (target->readback) vkDestroyBuffer(state->device, target->readback, NULL);
    if (target->readback_memory) vkFreeMemory(state->device, target->readback_memory, NULL);
    if (target->view) vkDestroyImageView(state->device, target->view, NULL);
    if (target->image) vkDestroyImage(state->device, target->image, NULL);
    if (target->memory) vkFreeMemory(state->device, target->memory, NULL);
    memset(target, 0, sizeof(*target));
}

static void write_graphics_replay_result(const struct probe_state *state,
                                         const char *status, const char *stage,
                                         const char *message, uint32_t alpha_pixels,
                                         uint32_t color_pixels, uint32_t min_color,
                                         uint32_t max_color, uint32_t checksum,
                                         const uint32_t *sample_values,
                                         uint32_t mismatch_count,
                                         enum graphics_replay_variant variant)
{
    char temporary[1024], safe_message[256], safe_device[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
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
            "  \"architecture\":{\"wineUnixArchitecture\":\"x86_64\",\"vulkanLoaderArchitecture\":\"x86_64\",\"venusIcdArchitecture\":\"x86_64\",\"hostArchitecture\":\"aarch64\",\"box64Enabled\":true},\n"
            "  \"capabilities\":{\"deviceName\":\"%s\"},\n"
            "  \"shader\":{\"stage\":\"fragment\",\"file\":\"%s\",\"entryPoint\":\"main\",\"moduleForm\":\"%s\",\"specialization\":{\"bindingPresence\":\"%s\",\"outputMap\":%u}},\n"
            "  \"descriptorContract\":{\"set\":0,\"uniformBufferBinding\":0,\"samplerBinding\":1,\"sampledImageBinding\":2,\"samplerDescriptorType\":0,\"sampledImageDescriptorType\":2,\"imageViewType\":\"CUBE\",\"imageFormat\":\"D24_UNORM_S8_UINT\",\"aspect\":\"DEPTH\",\"baseMip\":0,\"mipCount\":1,\"baseLayer\":0,\"layerCount\":6,\"compareEnable\":true,\"compareOp\":\"LESS_OR_EQUAL\",\"layout\":\"SHADER_READ_ONLY_OPTIMAL\"},\n"
            "  \"graphics\":{\"renderPass\":true,\"fullscreenTriangle\":true,\"outputFormat\":\"R8G8B8A8_UNORM\",\"outputExtent\":\"64x4\",\"alphaPixels\":%u,\"colorPixels\":%u,\"minColor\":%u,\"maxColor\":%u,\"checksum\":\"0x%08x\",\"samplePoints\":{\"x\":[5,16,27,37,48,58],\"y\":2,\"expected\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],\"actual\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],\"mismatches\":%u}},\n"
            "  \"metrics\":{\"cpuReadBytes\":1024,\"cpuUploadBytes\":0,\"gpuCopyCount\":1,\"queueSubmitCount\":%llu,\"perFrameDeviceWaitIdle\":0,\"fallbackDetected\":false,\"durationMs\":%llu}\n"
            "}\n",
            state->run_id, state->test_id, status, stage, safe_message,
            safe_device, graphics_replay_shader(variant), graphics_replay_form(variant),
            graphics_replay_has_specialization(variant) ? "frozen-true" : "not-applicable",
            graphics_replay_has_specialization(variant) ? 12816u : 0u,
            alpha_pixels, color_pixels, min_color, max_color, checksum,
            graphics_replay_expected_for_variant(variant)[0],
            graphics_replay_expected_for_variant(variant)[1],
            graphics_replay_expected_for_variant(variant)[2],
            graphics_replay_expected_for_variant(variant)[3],
            graphics_replay_expected_for_variant(variant)[4],
            graphics_replay_expected_for_variant(variant)[5],
            sample_values ? sample_values[0] : 0,
            sample_values ? sample_values[1] : 0,
            sample_values ? sample_values[2] : 0,
            sample_values ? sample_values[3] : 0,
            sample_values ? sample_values[4] : 0,
            sample_values ? sample_values[5] : 0,
            mismatch_count,
            (unsigned long long)state->queue_submit_count,
            (unsigned long long)(now_ms() - state->started_ms));
    fflush(file);
    fd = fileno(file);
    if (fd >= 0) fsync(fd);
    fclose(file);
    rename(temporary, state->result_path);
}

static int run_depth_cube_graphics_replay(struct probe_state *state,
                                          enum graphics_replay_variant variant)
{
    struct depth_cube_resources depth = { 0 };
    struct graphics_replay_target target = { 0 };
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkShaderModule fragment_shader = VK_NULL_HANDLE;
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkResult result = VK_SUCCESS;
    uint32_t alpha_pixels = 0, color_pixels = 0, min_color = 255, max_color = 0;
    uint32_t checksum = 2166136261u;
    uint32_t sample_values[GRAPHICS_REPLAY_SAMPLE_COUNT] = { 0 };
    uint32_t mismatch_count = GRAPHICS_REPLAY_SAMPLE_COUNT;
    int passed = 0;

    write_graphics_replay_result(state, "started", "startup",
                                 graphics_replay_start_message(variant),
                                 0, 0, 0, 0, 0, sample_values, mismatch_count, variant);
    if (!create_depth_cube_resources(state, &depth)) {
        write_graphics_replay_result(state, "FAIL", "host-vulkan",
                                     "D24 Cube resource creation failed", 0, 0, 0, 0, 0,
                                     sample_values, mismatch_count, variant);
        return 0;
    }
    result = create_graphics_replay_target(state, &target);
    if (result != VK_SUCCESS) goto failed;

    {
        uint32_t *vertex_code = NULL, *fragment_code = NULL;
        size_t vertex_size = 0, fragment_size = 0;
        VkShaderModuleCreateInfo shader_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        if (!load_shader("venus_fullscreen_triangle.vert.spv", &vertex_code, &vertex_size) ||
            !load_shader(graphics_replay_shader(variant), &fragment_code, &fragment_size)) {
            free(vertex_code); free(fragment_code); goto failed;
        }
        shader_info.codeSize = vertex_size;
        shader_info.pCode = vertex_code;
        result = vkCreateShaderModule(state->device, &shader_info, NULL, &vertex_shader);
        if (result == VK_SUCCESS) {
            shader_info.codeSize = fragment_size;
            shader_info.pCode = fragment_code;
            result = vkCreateShaderModule(state->device, &shader_info, NULL, &fragment_shader);
        }
        free(vertex_code); free(fragment_code);
        if (result != VK_SUCCESS) goto failed;
    }

    {
        VkDescriptorSetLayoutBinding bindings[3] = { 0 };
        VkDescriptorSetLayoutCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        info.bindingCount = 3;
        info.pBindings = bindings;
        result = vkCreateDescriptorSetLayout(state->device, &info, NULL, &set_layout);
        if (result != VK_SUCCESS) goto failed;
        {
            VkDescriptorPoolSize sizes[3] = {
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
                { VK_DESCRIPTOR_TYPE_SAMPLER, 1 },
                { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 }
            };
            VkDescriptorPoolCreateInfo pool = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            VkDescriptorSetAllocateInfo allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            pool.maxSets = 1;
            pool.poolSizeCount = 3;
            pool.pPoolSizes = sizes;
            result = vkCreateDescriptorPool(state->device, &pool, NULL, &descriptor_pool);
            if (result != VK_SUCCESS) goto failed;
            allocate.descriptorPool = descriptor_pool;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &set_layout;
            result = vkAllocateDescriptorSets(state->device, &allocate, &descriptor_set);
            if (result != VK_SUCCESS) goto failed;
        }
        {
            VkDescriptorBufferInfo buffer = { target.sampler_info, 0, 32u * 4u * 4u };
            VkDescriptorImageInfo sampler = { depth.compare_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            VkDescriptorImageInfo image = { VK_NULL_HANDLE, depth.cube_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet writes[3] = { 0 };
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = descriptor_set;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &buffer;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = descriptor_set;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[1].pImageInfo = &sampler;
            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = descriptor_set;
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[2].pImageInfo = &image;
            vkUpdateDescriptorSets(state->device, 3, writes, 0, NULL);
        }
    }

    {
        VkAttachmentDescription attachment = { 0 };
        VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = { 0 };
        VkRenderPassCreateInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
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
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        result = vkCreateRenderPass(state->device, &info, NULL, &render_pass);
        if (result != VK_SUCCESS) goto failed;
        {
            VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            framebuffer_info.renderPass = render_pass;
            framebuffer_info.attachmentCount = 1;
            framebuffer_info.pAttachments = &target.view;
            framebuffer_info.width = GRAPHICS_REPLAY_WIDTH;
            framebuffer_info.height = GRAPHICS_REPLAY_HEIGHT;
            framebuffer_info.layers = 1;
            result = vkCreateFramebuffer(state->device, &framebuffer_info, NULL, &framebuffer);
            if (result != VK_SUCCESS) goto failed;
        }
    }

    {
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkPipelineShaderStageCreateInfo stages[2] = { 0 };
        VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo input_assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        VkPipelineColorBlendAttachmentState blend_attachment = { 0 };
        VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        VkGraphicsPipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        VkSpecializationMapEntry entry = { 1216, 0, sizeof(uint32_t) };
        uint32_t specialization_data = 12816;
        VkSpecializationInfo specialization = {
            1, &entry, sizeof(specialization_data), &specialization_data
        };
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &set_layout;
        result = vkCreatePipelineLayout(state->device, &layout_info, NULL, &pipeline_layout);
        if (result != VK_SUCCESS) goto failed;
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_shader;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_shader;
        stages[1].pName = "main";
        stages[1].pSpecializationInfo =
            graphics_replay_has_specialization(variant) ? &specialization : NULL;
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = stages;
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = pipeline_layout;
        pipeline_info.renderPass = render_pass;
        result = vkCreateGraphicsPipelines(state->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline);
        if (result != VK_SUCCESS) goto failed;
    }

    vkResetFences(state->device, 1, &state->fence);
    vkResetCommandPool(state->device, state->command_pool, 0);
    result = vkBeginCommandBuffer(state->command, &begin);
    if (result != VK_SUCCESS) goto failed;
    {
        VkImageMemoryBarrier depth_barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        depth_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        depth_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        depth_barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depth_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depth_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depth_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depth_barrier.image = depth.image;
        depth_barrier.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        depth_barrier.subresourceRange.levelCount = 1;
        depth_barrier.subresourceRange.layerCount = DEPTH_CUBE_LAYER_COUNT;
        vkCmdPipelineBarrier(state->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, NULL, 0, NULL, 1, &depth_barrier);
    }
    {
        VkClearValue clear;
        VkRenderPassBeginInfo render = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        VkViewport viewport = { 0.0f, 0.0f, GRAPHICS_REPLAY_WIDTH, GRAPHICS_REPLAY_HEIGHT, 0.0f, 1.0f };
        VkRect2D scissor = { { 0, 0 }, { GRAPHICS_REPLAY_WIDTH, GRAPHICS_REPLAY_HEIGHT } };
        memset(&clear, 0, sizeof(clear));
        render.renderPass = render_pass;
        render.framebuffer = framebuffer;
        render.renderArea.extent.width = GRAPHICS_REPLAY_WIDTH;
        render.renderArea.extent.height = GRAPHICS_REPLAY_HEIGHT;
        render.clearValueCount = 1;
        render.pClearValues = &clear;
        vkCmdBeginRenderPass(state->command, &render, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(state->command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(state->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
        vkCmdSetViewport(state->command, 0, 1, &viewport);
        vkCmdSetScissor(state->command, 0, 1, &scissor);
        vkCmdDraw(state->command, 3, 1, 0, 0);
        vkCmdEndRenderPass(state->command);
    }
    {
        VkBufferImageCopy copy = { 0 };
        image_barrier(state->command, target.image,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = GRAPHICS_REPLAY_WIDTH;
        copy.imageExtent.height = GRAPHICS_REPLAY_HEIGHT;
        copy.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(state->command, target.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.readback, 1, &copy);
    }
    result = vkEndCommandBuffer(state->command);
    if (result == VK_SUCCESS) result = submit_and_wait(state);
    if (result != VK_SUCCESS) goto failed;
    {
        void *mapped = NULL;
        VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
        result = vkMapMemory(state->device, target.readback_memory, 0,
                             GRAPHICS_REPLAY_WIDTH * GRAPHICS_REPLAY_HEIGHT * 4, 0, &mapped);
        if (result != VK_SUCCESS) goto failed;
        range.memory = target.readback_memory;
        range.size = VK_WHOLE_SIZE;
        result = vkInvalidateMappedMemoryRanges(state->device, 1, &range);
        if (result == VK_SUCCESS) {
            const uint8_t *pixels = (const uint8_t *)mapped;
            uint32_t i;
            for (i = 0; i < GRAPHICS_REPLAY_WIDTH * GRAPHICS_REPLAY_HEIGHT; ++i) {
                uint8_t r = pixels[i * 4 + 0];
                uint8_t g = pixels[i * 4 + 1];
                uint8_t b = pixels[i * 4 + 2];
                uint8_t a = pixels[i * 4 + 3];
                if (a == 255) alpha_pixels++;
                if (r || g || b) color_pixels++;
                if (r < min_color) min_color = r;
                if (g < min_color) min_color = g;
                if (b < min_color) min_color = b;
                if (r > max_color) max_color = r;
                if (g > max_color) max_color = g;
                if (b > max_color) max_color = b;
                checksum ^= pixels[i * 4 + 0]; checksum *= 16777619u;
                checksum ^= pixels[i * 4 + 1]; checksum *= 16777619u;
                checksum ^= pixels[i * 4 + 2]; checksum *= 16777619u;
                checksum ^= pixels[i * 4 + 3]; checksum *= 16777619u;
            }
            const uint32_t *expected = graphics_replay_expected_for_variant(variant);
            mismatch_count = 0;
            for (i = 0; i < GRAPHICS_REPLAY_SAMPLE_COUNT; ++i) {
                const uint32_t pixel =
                    (GRAPHICS_REPLAY_HEIGHT / 2u) * GRAPHICS_REPLAY_WIDTH +
                    graphics_replay_sample_x[i];
                const uint8_t *rgba = &pixels[pixel * 4u];
                sample_values[i] = ((uint32_t)rgba[3] << 24) |
                                   ((uint32_t)rgba[2] << 16) |
                                   ((uint32_t)rgba[1] << 8) |
                                   (uint32_t)rgba[0];
                if (sample_values[i] != expected[i])
                    mismatch_count++;
            }
            passed = alpha_pixels == GRAPHICS_REPLAY_WIDTH * GRAPHICS_REPLAY_HEIGHT &&
                mismatch_count == 0;
        }
        vkUnmapMemory(state->device, target.readback_memory);
        if (result != VK_SUCCESS) goto failed;
    }
    write_graphics_replay_result(state, passed ? "PASS" : "FAIL", "graphics-replay",
                                 passed ? "fragment replay matched all D24 Cube comparison stripes" :
                                 "fragment replay mismatched D24 Cube comparison stripes",
                                 alpha_pixels, color_pixels, min_color == 255 ? 0 : min_color,
                                 max_color, checksum, sample_values, mismatch_count, variant);
    goto cleanup;

failed:
    write_graphics_replay_result(state, "FAIL", "graphics-replay",
                                 "exact fragment graphics replay failed", alpha_pixels,
                                 color_pixels, min_color == 255 ? 0 : min_color,
                                 max_color, checksum, sample_values, mismatch_count, variant);
cleanup:
    if (pipeline) vkDestroyPipeline(state->device, pipeline, NULL);
    if (pipeline_layout) vkDestroyPipelineLayout(state->device, pipeline_layout, NULL);
    if (fragment_shader) vkDestroyShaderModule(state->device, fragment_shader, NULL);
    if (vertex_shader) vkDestroyShaderModule(state->device, vertex_shader, NULL);
    if (framebuffer) vkDestroyFramebuffer(state->device, framebuffer, NULL);
    if (render_pass) vkDestroyRenderPass(state->device, render_pass, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(state->device, descriptor_pool, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(state->device, set_layout, NULL);
    destroy_graphics_replay_target(state, &target);
    destroy_depth_cube_resources(state, &depth);
    return passed;
}
