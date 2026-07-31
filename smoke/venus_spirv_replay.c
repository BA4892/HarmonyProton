#define _POSIX_C_SOURCE 200809L

/* Exact DXVK SPIR-V replay diagnostic.
 *
 * This is deliberately a small compute-only harness.  It does not translate
 * the shader, replace its descriptors, or use a hand-written shader.  The
 * replay loads the remapped binary emitted immediately before DXVK calls
 * vkCreateShaderModule(), reflects its DescriptorSet/Binding/type declarations,
 * and supplies that exact contract. DXVK shaders legitimately use both
 * sampler-first (s0/b0,t0/b1,u0/b2) and samplerless (t0/b0,u0/b1) layouts.
 *
 * The input is the same 4x4 RGBA8 pattern as the Wine/D3D11 smoke.  The
 * output is a 16x16 RGBA8 storage image and only a tiny readback is performed
 * for diagnosis.  It is intentionally not a display path.
 */

#include <vulkan/vulkan.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EXPECTED_COLOR 0xff332211u
#define SHADER_NOT_EXECUTED 0xdeadbeefu
#define INPUT_W 4u
#define INPUT_H 4u
#define OUTPUT_W 16u
#define OUTPUT_H 16u
#define MAX_SHADER_BINDINGS 16u

/* Minimal SPIR-V reflection used by the exact replay.  The replay must use
 * the descriptor types and binding numbers emitted by DXVK; a fixed
 * sampler/image/output layout would silently test a different shader
 * contract. */
enum spv_opcode {
   SPV_OP_TYPE_IMAGE = 25,
   SPV_OP_TYPE_SAMPLER = 26,
   SPV_OP_TYPE_SAMPLED_IMAGE = 27,
   SPV_OP_TYPE_POINTER = 32,
   SPV_OP_VARIABLE = 59,
   SPV_OP_DECORATE = 71
};

enum spv_decoration {
   SPV_DECORATION_SPEC_ID = 1,
   SPV_DECORATION_NON_READABLE = 25,
   SPV_DECORATION_BINDING = 33,
   SPV_DECORATION_DESCRIPTOR_SET = 34
};

enum spv_storage_class {
   SPV_STORAGE_UNIFORM_CONSTANT = 0,
   SPV_STORAGE_STORAGE_BUFFER = 12
};

enum spv_type_kind {
   SPV_TYPE_NONE,
   SPV_TYPE_IMAGE,
   SPV_TYPE_SAMPLER,
   SPV_TYPE_SAMPLED_IMAGE,
   SPV_TYPE_POINTER
};

struct spv_type_info {
   uint8_t kind;
   uint32_t pointee;
   uint32_t sampled;
};

struct spv_binding {
   uint32_t binding;
   VkDescriptorType descriptor_type;
   int non_readable;
};

struct spv_contract {
   struct spv_binding bindings[MAX_SHADER_BINDINGS];
   uint32_t count;
   int output_binding;
   uint32_t spec_ids[MAX_SHADER_BINDINGS];
   uint32_t spec_count;
   char text[512];
};

struct replay_state {
   const char *run_id;
   const char *test_id;
   const char *result_path;
   const char *spv_dir;
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
   VkImage input_image;
   VkDeviceMemory input_memory;
   VkImageView input_view;
   VkImage output_image;
   VkDeviceMemory output_memory;
   VkImageView output_view;
   VkSampler sampler;
   VkBuffer upload_buffer;
   VkDeviceMemory upload_memory;
   VkBuffer readback_buffer;
   VkDeviceMemory readback_memory;
   uint64_t queue_submits;
   uint64_t gpu_copies;
   uint64_t cpu_read_bytes;
   uint64_t cpu_upload_bytes;
   uint32_t sampled_value;
   uint32_t expected_value;
   int storage_image_read_without_format;
   int storage_image_write_without_format;
   int output_in_transfer_src;
   uint32_t candidate_count;
   uint32_t pipeline_create_count;
   uint32_t shader_execute_count;
   uint32_t golden_value;
   int golden_pass;
   uint32_t golden_storage_value;
   int golden_storage_pass;
   uint32_t golden_spec_value;
   int golden_spec_pass;
   uint32_t golden_vector_spec_value;
   int golden_vector_spec_pass;
   int frozen_pass;
   char frozen_report[1024];
   char descriptor_contract[512];
   char specialization_constants[256];
   char selected_spv[256];
   char candidate_report[1024];
   char failure[256];
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

static void write_result(const struct replay_state *s, const char *status,
                         const char *stage, const char *message)
{
   char tmp[1024], safe_message[512], safe_spv[512], safe_device[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
   FILE *file;
   int fd;
   if (!s->result_path || !s->result_path[0]) return;
   snprintf(tmp, sizeof(tmp), "%s.tmp.%d", s->result_path, getpid());
   json_safe(safe_message, sizeof(safe_message), message ? message : "");
   json_safe(safe_spv, sizeof(safe_spv), s->selected_spv);
   json_safe(safe_device, sizeof(safe_device), s->properties.deviceName);
   file = fopen(tmp, "w");
   if (!file) return;
   fprintf(file,
      "{\n"
      "  \"schemaVersion\":1,\n"
      "  \"runId\":\"%s\",\n"
      "  \"testId\":\"%s\",\n"
      "  \"status\":\"%s\",\n"
      "  \"stage\":\"%s\",\n"
      "  \"message\":\"%s\",\n"
      "  \"pid\":%d,\n"
      "  \"heartbeatTimestampMs\":%" PRIu64 ",\n"
      "  \"architecture\":{\"peArchitecture\":\"not-applicable\",\"wineUnixArchitecture\":\"x86_64\",\"vulkanLoaderArchitecture\":\"x86_64\",\"venusIcdArchitecture\":\"x86_64\",\"hostArchitecture\":\"aarch64\",\"wow64ThunkEnabled\":false,\"box64Enabled\":true},\n"
      "  \"replay\":{\"spvDirectory\":\"%s\",\"selectedSpv\":\"%s\",\"shaderStage\":\"compute\",\"descriptorContract\":\"%s\",\"inputFormat\":\"R8G8B8A8_UNORM\",\"inputExtent\":\"4x4\",\"outputFormat\":\"R8G8B8A8_UNORM\",\"outputExtent\":\"16x16\",\"candidateCount\":%u,\"pipelineCreateCount\":%u,\"shaderExecuteCount\":%u,\"candidates\":\"%s\",\"pushConstants\":false,\"specializationConstants\":\"%s\",\"shaderStorageImageReadWithoutFormat\":%s,\"shaderStorageImageWriteWithoutFormat\":%s},\n"
      "  \"sampledImage\":{\"value\":\"0x%08x\",\"expected\":\"0x%08x\",\"pass\":%s},\n"
      "  \"goldenControl\":{\"spv\":\"venus_dxvk_contract_sample.spv\",\"value\":\"0x%08x\",\"expected\":\"0x%08x\",\"pass\":%s},\n"
      "  \"goldenStorageImage\":{\"spv\":\"venus_dxvk_contract_unknown_sample.spv\",\"value\":\"0x%08x\",\"expected\":\"nonzero\",\"pass\":%s},\n"
      "  \"goldenSpecStorageImage\":{\"spv\":\"venus_dxvk_contract_spec_sample.spv\",\"value\":\"0x%08x\",\"expected\":\"nonzero\",\"pass\":%s},\n"
      "  \"goldenVectorSpecStorageImage\":{\"spv\":\"venus_dxvk_contract_vector_spec_sample.spv\",\"value\":\"0x%08x\",\"expected\":\"nonzero\",\"pass\":%s},\n"
      "  \"frozenSpecialization\":{\"directory\":\"share/winehua/replay_frozen\",\"candidates\":\"%s\",\"pass\":%s},\n"
      "  \"metrics\":{\"cpuReadBytes\":%" PRIu64 ",\"cpuUploadBytes\":%" PRIu64 ",\"gpuCopyCount\":%" PRIu64 ",\"queueSubmitCount\":%" PRIu64 ",\"perFrameDeviceWaitIdle\":0,\"fallbackDetected\":false,\"durationMs\":%" PRIu64 "}\n"
      "}\n",
      s->run_id ? s->run_id : "", s->test_id ? s->test_id : "", status,
      stage, safe_message, getpid(), now_ms(), s->spv_dir ? s->spv_dir : "",
      safe_spv, s->descriptor_contract, s->candidate_count, s->pipeline_create_count,
      s->shader_execute_count, s->candidate_report, s->specialization_constants,
      s->storage_image_read_without_format ? "true" : "false",
      s->storage_image_write_without_format ? "true" : "false",
      s->sampled_value, s->expected_value,
      s->sampled_value == s->expected_value ? "true" : "false",
      s->golden_value, s->expected_value, s->golden_pass ? "true" : "false",
      s->golden_storage_value, s->golden_storage_pass ? "true" : "false",
      s->golden_spec_value, s->golden_spec_pass ? "true" : "false",
      s->golden_vector_spec_value, s->golden_vector_spec_pass ? "true" : "false",
      s->frozen_report, s->frozen_pass ? "true" : "false",
      s->cpu_read_bytes, s->cpu_upload_bytes, s->gpu_copies, s->queue_submits,
      now_ms() - s->started_ms);
   fflush(file);
   fd = fileno(file);
   if (fd >= 0) fsync(fd);
   fclose(file);
   rename(tmp, s->result_path);
}

static int find_memory_type(struct replay_state *s, uint32_t bits,
                            VkMemoryPropertyFlags required, uint32_t *index)
{
   VkPhysicalDeviceMemoryProperties props;
   uint32_t i;
   vkGetPhysicalDeviceMemoryProperties(s->physical, &props);
   for (i = 0; i < props.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) &&
          (props.memoryTypes[i].propertyFlags & required) == required) {
         *index = i;
         return 1;
      }
   }
   return 0;
}

static VkResult create_buffer(struct replay_state *s, VkDeviceSize size,
                              VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                              VkBuffer *buffer, VkDeviceMemory *memory)
{
   VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
   VkMemoryRequirements req;
   VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   uint32_t type;
   VkResult result;
   info.size = size;
   info.usage = usage;
   info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   result = vkCreateBuffer(s->device, &info, NULL, buffer);
   if (result != VK_SUCCESS) return result;
   vkGetBufferMemoryRequirements(s->device, *buffer, &req);
   if (!find_memory_type(s, req.memoryTypeBits, props, &type)) return VK_ERROR_FEATURE_NOT_PRESENT;
   alloc.allocationSize = req.size;
   alloc.memoryTypeIndex = type;
   result = vkAllocateMemory(s->device, &alloc, NULL, memory);
   if (result != VK_SUCCESS) return result;
   return vkBindBufferMemory(s->device, *buffer, *memory, 0);
}

static VkResult create_image(struct replay_state *s, uint32_t width, uint32_t height,
                             VkImageUsageFlags usage, VkImage *image,
                             VkDeviceMemory *memory)
{
   VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
   VkMemoryRequirements req;
   VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   uint32_t type;
   VkResult result;
   info.imageType = VK_IMAGE_TYPE_2D;
   info.format = VK_FORMAT_R8G8B8A8_UNORM;
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
   vkGetImageMemoryRequirements(s->device, *image, &req);
   if (!find_memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   alloc.allocationSize = req.size;
   alloc.memoryTypeIndex = type;
   result = vkAllocateMemory(s->device, &alloc, NULL, memory);
   if (result != VK_SUCCESS) return result;
   return vkBindImageMemory(s->device, *image, *memory, 0);
}

static VkResult create_view(struct replay_state *s, VkImage image, VkImageView *view)
{
   VkImageViewCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
   info.image = image;
   info.viewType = VK_IMAGE_VIEW_TYPE_2D;
   info.format = VK_FORMAT_R8G8B8A8_UNORM;
   info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   info.subresourceRange.levelCount = 1;
   info.subresourceRange.layerCount = 1;
   return vkCreateImageView(s->device, &info, NULL, view);
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
   barrier.image = image;
   barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   barrier.subresourceRange.levelCount = 1;
   barrier.subresourceRange.layerCount = 1;
   vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, NULL, 0, NULL,
                        1, &barrier);
}

static VkResult submit_and_wait(struct replay_state *s)
{
   VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
   VkResult result;
   submit.commandBufferCount = 1;
   submit.pCommandBuffers = &s->command;
   result = vkResetFences(s->device, 1, &s->fence);
   if (result == VK_SUCCESS) result = vkQueueSubmit(s->queue, 1, &submit, s->fence);
   if (result == VK_SUCCESS) result = vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, UINT64_MAX);
   if (result == VK_SUCCESS) s->queue_submits++;
   return result;
}

static int read_file(const char *path, uint32_t **code, size_t *size)
{
   FILE *file;
   long length;
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

static int compare_binding(const void *left, const void *right)
{
   const struct spv_binding *a = (const struct spv_binding *)left;
   const struct spv_binding *b = (const struct spv_binding *)right;
   if (a->binding < b->binding) return -1;
   if (a->binding > b->binding) return 1;
   return 0;
}

static VkDescriptorType descriptor_type_for(const struct spv_type_info *types,
                                            uint32_t type_id,
                                            uint32_t storage)
{
   const struct spv_type_info *type;
   if (storage == SPV_STORAGE_STORAGE_BUFFER) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   if (storage != SPV_STORAGE_UNIFORM_CONSTANT) return VK_DESCRIPTOR_TYPE_MAX_ENUM;
   type = &types[type_id];
   if (type->kind == SPV_TYPE_SAMPLER) return VK_DESCRIPTOR_TYPE_SAMPLER;
   if (type->kind == SPV_TYPE_SAMPLED_IMAGE) return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   if (type->kind == SPV_TYPE_IMAGE) {
      if (type->sampled == 1) return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      if (type->sampled == 2) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
   }
   return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

static int reflect_contract(const uint32_t *code, size_t code_size,
                            struct spv_contract *contract)
{
   uint32_t bound, word_count, offset;
   struct spv_type_info *types = NULL;
   uint32_t *variable_type = NULL, *variable_storage = NULL;
   uint32_t *binding = NULL, *descriptor_set = NULL;
   uint8_t *variable_valid = NULL, *non_readable = NULL;
   int ok = 0;
   uint32_t i;

   memset(contract, 0, sizeof(*contract));
   contract->output_binding = -1;
   if (!code || code_size < 20 || (code_size & 3) || code[0] != 0x07230203u)
      return 0;
   bound = code[3];
   if (!bound || bound > 65536u) return 0;
   types = calloc(bound, sizeof(*types));
   variable_type = calloc(bound, sizeof(*variable_type));
   variable_storage = calloc(bound, sizeof(*variable_storage));
   variable_valid = calloc(bound, sizeof(*variable_valid));
   non_readable = calloc(bound, sizeof(*non_readable));
   binding = malloc(bound * sizeof(*binding));
   descriptor_set = malloc(bound * sizeof(*descriptor_set));
   if (!types || !variable_type || !variable_storage || !variable_valid ||
       !non_readable || !binding || !descriptor_set) goto done;
   for (i = 0; i < bound; ++i) {
      binding[i] = UINT32_MAX;
      descriptor_set[i] = UINT32_MAX;
   }
   word_count = (uint32_t)(code_size / sizeof(uint32_t));
   offset = 5;
   while (offset < word_count) {
      uint32_t instruction = code[offset];
      uint16_t words = (uint16_t)(instruction >> 16);
      uint16_t opcode = (uint16_t)(instruction & 0xffffu);
      uint32_t result_id;
      if (!words || offset + words > word_count) goto done;
      switch (opcode) {
      case SPV_OP_TYPE_IMAGE:
         if (words >= 9 && code[offset + 1] < bound) {
            result_id = code[offset + 1];
            types[result_id].kind = SPV_TYPE_IMAGE;
            types[result_id].sampled = code[offset + 7];
         }
         break;
      case SPV_OP_TYPE_SAMPLER:
         if (words >= 2 && code[offset + 1] < bound)
            types[code[offset + 1]].kind = SPV_TYPE_SAMPLER;
         break;
      case SPV_OP_TYPE_SAMPLED_IMAGE:
         if (words >= 3 && code[offset + 1] < bound) {
            types[code[offset + 1]].kind = SPV_TYPE_SAMPLED_IMAGE;
            types[code[offset + 1]].pointee = code[offset + 2];
         }
         break;
      case SPV_OP_TYPE_POINTER:
         if (words >= 4 && code[offset + 1] < bound) {
            types[code[offset + 1]].kind = SPV_TYPE_POINTER;
            types[code[offset + 1]].pointee = code[offset + 3];
         }
         break;
      case SPV_OP_VARIABLE:
         if (words >= 4 && code[offset + 2] < bound) {
            result_id = code[offset + 2];
            variable_valid[result_id] = 1;
            variable_type[result_id] = code[offset + 1];
            variable_storage[result_id] = code[offset + 3];
         }
         break;
      case SPV_OP_DECORATE:
         if (words >= 3 && code[offset + 1] < bound) {
            result_id = code[offset + 1];
            if (code[offset + 2] == SPV_DECORATION_SPEC_ID && words >= 4) {
               if (contract->spec_count < MAX_SHADER_BINDINGS)
                  contract->spec_ids[contract->spec_count++] = code[offset + 3];
            } else if (code[offset + 2] == SPV_DECORATION_BINDING && words >= 4)
               binding[result_id] = code[offset + 3];
            else if (code[offset + 2] == SPV_DECORATION_DESCRIPTOR_SET && words >= 4)
               descriptor_set[result_id] = code[offset + 3];
            else if (code[offset + 2] == SPV_DECORATION_NON_READABLE)
               non_readable[result_id] = 1;
         }
         break;
      default:
         break;
      }
      offset += words;
   }
   for (i = 0; i < bound; ++i) {
      uint32_t pointer_type, pointee;
      VkDescriptorType descriptor_type;
      if (!variable_valid[i] || binding[i] == UINT32_MAX ||
          descriptor_set[i] == UINT32_MAX || descriptor_set[i] != 0)
         continue;
      pointer_type = variable_type[i];
      if (pointer_type >= bound || types[pointer_type].kind != SPV_TYPE_POINTER)
         continue;
      pointee = types[pointer_type].pointee;
      if (pointee >= bound) continue;
      descriptor_type = descriptor_type_for(types, pointee, variable_storage[i]);
      if (descriptor_type == VK_DESCRIPTOR_TYPE_MAX_ENUM ||
          contract->count >= MAX_SHADER_BINDINGS)
         continue;
      contract->bindings[contract->count].binding = binding[i];
      contract->bindings[contract->count].descriptor_type = descriptor_type;
      contract->bindings[contract->count].non_readable = non_readable[i] ? 1 : 0;
      if (descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
          contract->output_binding < 0 && non_readable[i])
         contract->output_binding = (int)binding[i];
      contract->count++;
   }
   if (contract->output_binding < 0) {
      for (i = 0; i < contract->count; ++i)
         if (contract->bindings[i].descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
             contract->bindings[i].descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            contract->output_binding = (int)contract->bindings[i].binding;
            break;
         }
   }
   if (!contract->count || contract->output_binding < 0) goto done;
   qsort(contract->bindings, contract->count, sizeof(contract->bindings[0]), compare_binding);
   contract->text[0] = 0;
   for (i = 0; i < contract->count; ++i) {
      const char *name = "unknown";
      char item[64];
      switch (contract->bindings[i].descriptor_type) {
      case VK_DESCRIPTOR_TYPE_SAMPLER: name = "sampler"; break;
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: name = "sampled-image"; break;
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: name = "combined-image-sampler"; break;
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: name = "storage-image"; break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: name = "storage-buffer"; break;
      default: break;
      }
      snprintf(item, sizeof(item), "%sset0:b%u=%s%s", i ? "," : "",
               contract->bindings[i].binding, name,
               (int)contract->bindings[i].binding == contract->output_binding ? "(output)" : "");
      if (strlen(contract->text) + strlen(item) + 1 < sizeof(contract->text))
         strcat(contract->text, item);
   }
   ok = 1;
done:
   free(types);
   free(variable_type);
   free(variable_storage);
   free(variable_valid);
   free(non_readable);
   free(binding);
   free(descriptor_set);
   return ok;
}

static int create_resources(struct replay_state *s)
{
   VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
   VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   VkBufferImageCopy copy = { 0 };
   uint8_t pattern[INPUT_W * INPUT_H * 4];
   void *mapped;
   uint32_t i;
   VkResult result;
   for (i = 0; i < INPUT_W * INPUT_H; ++i) {
      pattern[i * 4 + 0] = 0x11;
      pattern[i * 4 + 1] = 0x22;
      pattern[i * 4 + 2] = 0x33;
      pattern[i * 4 + 3] = 0xff;
   }
   result = create_buffer(s, sizeof(pattern), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &s->upload_buffer, &s->upload_memory);
   if (result != VK_SUCCESS) return 0;
   result = create_buffer(s, OUTPUT_W * OUTPUT_H * 4,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &s->readback_buffer, &s->readback_memory);
   if (result != VK_SUCCESS) return 0;
   result = create_image(s, INPUT_W, INPUT_H,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT, &s->input_image, &s->input_memory);
   if (result != VK_SUCCESS || create_view(s, s->input_image, &s->input_view) != VK_SUCCESS) return 0;
   result = create_image(s, OUTPUT_W, OUTPUT_H,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         &s->output_image, &s->output_memory);
   if (result != VK_SUCCESS || create_view(s, s->output_image, &s->output_view) != VK_SUCCESS) return 0;
   sampler.magFilter = VK_FILTER_NEAREST;
   sampler.minFilter = VK_FILTER_NEAREST;
   sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
   sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   sampler.maxLod = 0.0f;
   if (vkCreateSampler(s->device, &sampler, NULL, &s->sampler) != VK_SUCCESS) return 0;
   if (vkMapMemory(s->device, s->upload_memory, 0, sizeof(pattern), 0, &mapped) != VK_SUCCESS) return 0;
   memcpy(mapped, pattern, sizeof(pattern));
   vkUnmapMemory(s->device, s->upload_memory);
   s->cpu_upload_bytes += sizeof(pattern);
   if (vkBeginCommandBuffer(s->command, &begin) != VK_SUCCESS) return 0;
   image_barrier(s->command, s->input_image, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
   copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   copy.imageSubresource.layerCount = 1;
   copy.imageExtent.width = INPUT_W;
   copy.imageExtent.height = INPUT_H;
   copy.imageExtent.depth = 1;
   vkCmdCopyBufferToImage(s->command, s->upload_buffer, s->input_image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
   image_barrier(s->command, s->input_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
   image_barrier(s->command, s->output_image, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
   if (vkEndCommandBuffer(s->command) != VK_SUCCESS || submit_and_wait(s) != VK_SUCCESS) return 0;
   s->gpu_copies++;
   return 1;
}

static int replay_one(struct replay_state *s, const char *path)
{
   struct spv_contract contract;
   VkDescriptorSetLayoutBinding bindings[MAX_SHADER_BINDINGS] = { 0 };
   VkDescriptorSetLayoutCreateInfo set_layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
   VkDescriptorPoolSize pool_sizes[MAX_SHADER_BINDINGS] = { 0 };
   VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
   VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
   VkPipelineLayoutCreateInfo pipeline_layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkComputePipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
   VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
   VkSpecializationMapEntry spec_entries[MAX_SHADER_BINDINGS] = { 0 };
   uint32_t spec_values[MAX_SHADER_BINDINGS] = { 0 };
   VkSpecializationInfo spec_info = { 0 };
   VkShaderModuleCreateInfo shader_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
   VkDescriptorImageInfo image_infos[MAX_SHADER_BINDINGS] = { 0 };
   VkDescriptorBufferInfo buffer_infos[MAX_SHADER_BINDINGS] = { 0 };
   VkWriteDescriptorSet writes[MAX_SHADER_BINDINGS] = { 0 };
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkDescriptorPool pool = VK_NULL_HANDLE;
   VkDescriptorSet set = VK_NULL_HANDLE;
   VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   VkShaderModule shader = VK_NULL_HANDLE;
   uint32_t *code = NULL;
   size_t code_size = 0;
   VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   VkBufferImageCopy copy = { 0 };
   void *mapped = NULL;
   VkResult result;
   uint32_t value = SHADER_NOT_EXECUTED;
   uint32_t pool_count = 0;
   uint32_t i;
   int has_output_image = 0;
   int has_output_buffer = 0;
   int ok = 0;

   if (!read_file(path, &code, &code_size) || !reflect_contract(code, code_size, &contract)) {
      free(code);
      return 0;
   }
   snprintf(s->descriptor_contract, sizeof(s->descriptor_contract), "%s", contract.text);
   shader_info.codeSize = code_size;
   shader_info.pCode = code;
   result = vkCreateShaderModule(s->device, &shader_info, NULL, &shader);
   free(code);
   if (result != VK_SUCCESS) return 0;
   s->candidate_count++;
   for (i = 0; i < contract.count; ++i) {
      VkDescriptorType type = contract.bindings[i].descriptor_type;
      uint32_t j;
      bindings[i].binding = contract.bindings[i].binding;
      bindings[i].descriptorType = type;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      for (j = 0; j < pool_count; ++j)
         if (pool_sizes[j].type == type) break;
      if (j == pool_count) {
         pool_sizes[pool_count].type = type;
         pool_sizes[pool_count].descriptorCount = 1;
         pool_count++;
      } else {
         pool_sizes[j].descriptorCount++;
      }
   }
   set_layout_info.bindingCount = contract.count;
   set_layout_info.pBindings = bindings;
   result = vkCreateDescriptorSetLayout(s->device, &set_layout_info, NULL, &set_layout);
   if (result != VK_SUCCESS) goto done;
   pool_info.maxSets = 1; pool_info.poolSizeCount = pool_count; pool_info.pPoolSizes = pool_sizes;
   if (vkCreateDescriptorPool(s->device, &pool_info, NULL, &pool) != VK_SUCCESS) goto done;
   alloc.descriptorPool = pool; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &set_layout;
   if (vkAllocateDescriptorSets(s->device, &alloc, &set) != VK_SUCCESS) goto done;
   pipeline_layout_info.setLayoutCount = 1; pipeline_layout_info.pSetLayouts = &set_layout;
   if (vkCreatePipelineLayout(s->device, &pipeline_layout_info, NULL, &pipeline_layout) != VK_SUCCESS) goto done;
   stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; stage.module = shader; stage.pName = "main";
   for (i = 0; i < contract.spec_count; ++i) {
      spec_entries[i].constantID = contract.spec_ids[i];
      spec_entries[i].offset = i * sizeof(uint32_t);
      spec_entries[i].size = sizeof(uint32_t);
      spec_values[i] = 1u;
   }
   if (contract.spec_count) {
      spec_info.mapEntryCount = contract.spec_count;
      spec_info.pMapEntries = spec_entries;
      spec_info.dataSize = contract.spec_count * sizeof(uint32_t);
      spec_info.pData = spec_values;
      stage.pSpecializationInfo = &spec_info;
      snprintf(s->specialization_constants, sizeof(s->specialization_constants),
               "explicit-true SpecId[0..%u]", contract.spec_count - 1);
   } else {
      snprintf(s->specialization_constants, sizeof(s->specialization_constants), "none");
   }
   pipeline_info.stage = stage; pipeline_info.layout = pipeline_layout;
   s->pipeline_create_count++;
   if (vkCreateComputePipelines(s->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline) != VK_SUCCESS)
      goto done;

   for (i = 0; i < contract.count; ++i) {
      VkDescriptorType type = contract.bindings[i].descriptor_type;
      uint32_t binding = contract.bindings[i].binding;
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = binding;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = type;
      switch (type) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
         image_infos[i].sampler = s->sampler;
         writes[i].pImageInfo = &image_infos[i];
         break;
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
         image_infos[i].imageView = s->input_view;
         image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         writes[i].pImageInfo = &image_infos[i];
         break;
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         image_infos[i].sampler = s->sampler;
         image_infos[i].imageView = s->input_view;
         image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         writes[i].pImageInfo = &image_infos[i];
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         image_infos[i].imageView = s->output_view;
         image_infos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
         writes[i].pImageInfo = &image_infos[i];
         if ((int)binding == contract.output_binding) has_output_image = 1;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         buffer_infos[i].buffer = s->readback_buffer;
         buffer_infos[i].offset = 0;
         buffer_infos[i].range = sizeof(uint32_t);
         writes[i].pBufferInfo = &buffer_infos[i];
         if ((int)binding == contract.output_binding) has_output_buffer = 1;
         break;
      default:
         goto done;
      }
   }
   vkUpdateDescriptorSets(s->device, contract.count, writes, 0, NULL);
   if (vkResetCommandPool(s->device, s->command_pool, 0) != VK_SUCCESS ||
       vkBeginCommandBuffer(s->command, &begin) != VK_SUCCESS) goto done;
   if (has_output_image && s->output_in_transfer_src) {
      image_barrier(s->command, s->output_image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      s->output_in_transfer_src = 0;
   }
   vkCmdBindPipeline(s->command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(s->command, VK_PIPELINE_BIND_POINT_COMPUTE,
                           pipeline_layout, 0, 1, &set, 0, NULL);
   vkCmdDispatch(s->command, 2, 2, 1);
   if (has_output_image) {
      image_barrier(s->command, s->output_image, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
      copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.imageSubresource.layerCount = 1;
      copy.imageExtent.width = OUTPUT_W; copy.imageExtent.height = OUTPUT_H; copy.imageExtent.depth = 1;
      vkCmdCopyImageToBuffer(s->command, s->output_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             s->readback_buffer, 1, &copy);
   }
   if (vkEndCommandBuffer(s->command) != VK_SUCCESS || submit_and_wait(s) != VK_SUCCESS) goto done;
   if (has_output_image) s->output_in_transfer_src = 1;
   if (has_output_image) s->gpu_copies++;
   if (vkMapMemory(s->device, s->readback_memory, 0,
                   has_output_buffer ? sizeof(uint32_t) : OUTPUT_W * OUTPUT_H * 4,
                   0, &mapped) != VK_SUCCESS)
      goto done;
   value = *(uint32_t *)mapped;
   vkUnmapMemory(s->device, s->readback_memory);
   s->cpu_read_bytes += has_output_buffer ? sizeof(uint32_t) : OUTPUT_W * OUTPUT_H * 4;
   s->shader_execute_count++;
   s->sampled_value = value;
   {
      const char *slash = strrchr(path, '/');
      char item[640];
      int used = snprintf(item, sizeof(item), "%s[%s]=0x%08x;",
                          slash ? slash + 1 : path, contract.text, value);
      if (used > 0 && (size_t)used < sizeof(item)) {
         size_t old = strlen(s->candidate_report);
         if (old + (size_t)used + 1 < sizeof(s->candidate_report))
            memcpy(s->candidate_report + old, item, (size_t)used + 1);
      }
   }
   ok = value == s->expected_value;
   if (ok) {
      const char *slash = strrchr(path, '/');
      snprintf(s->selected_spv, sizeof(s->selected_spv), "%s", slash ? slash + 1 : path);
   }

done:
   if (pipeline) vkDestroyPipeline(s->device, pipeline, NULL);
   if (pipeline_layout) vkDestroyPipelineLayout(s->device, pipeline_layout, NULL);
   if (pool) vkDestroyDescriptorPool(s->device, pool, NULL);
   if (set_layout) vkDestroyDescriptorSetLayout(s->device, set_layout, NULL);
   if (shader) vkDestroyShaderModule(s->device, shader, NULL);
   return ok;
}

static int init_vulkan(struct replay_state *s)
{
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
   VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
   VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
   VkPhysicalDeviceFeatures supported_features = { 0 };
   VkPhysicalDeviceFeatures enabled_features = { 0 };
   VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
   VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkPhysicalDevice *devices;
   VkQueueFamilyProperties *queues;
   uint32_t count = 0, i;
   float priority = 1.0f;
   VkResult result;
   /* Match the already passing Guest Vulkan smoke: the Venus/vtest loader
    * accepts a minimal VkInstanceCreateInfo most reliably on this device. */
   app.apiVersion = VK_API_VERSION_1_1;
   instance_info.pApplicationInfo = NULL;
   result = vkCreateInstance(&instance_info, NULL, &s->instance);
   if (result != VK_SUCCESS) { fprintf(stderr, "venus_spirv_replay: vkCreateInstance=%d\\n", result); return 0; }
   result = vkEnumeratePhysicalDevices(s->instance, &count, NULL);
   if (result != VK_SUCCESS || !count) { fprintf(stderr, "venus_spirv_replay: enumerate count result=%d count=%u\\n", result, count); return 0; }
   devices = calloc(count, sizeof(*devices));
   if (!devices) { fprintf(stderr, "venus_spirv_replay: device allocation failed\\n"); return 0; }
   result = vkEnumeratePhysicalDevices(s->instance, &count, devices);
   if (result != VK_SUCCESS) { fprintf(stderr, "venus_spirv_replay: enumerate devices result=%d\\n", result); free(devices); return 0; }
   s->physical = devices[0];
   free(devices);
   vkGetPhysicalDeviceProperties(s->physical, &s->properties);
   vkGetPhysicalDeviceFeatures(s->physical, &supported_features);
   enabled_features.shaderStorageImageReadWithoutFormat =
      supported_features.shaderStorageImageReadWithoutFormat;
   enabled_features.shaderStorageImageWriteWithoutFormat =
      supported_features.shaderStorageImageWriteWithoutFormat;
   s->storage_image_read_without_format =
      enabled_features.shaderStorageImageReadWithoutFormat ? 1 : 0;
   s->storage_image_write_without_format =
      enabled_features.shaderStorageImageWriteWithoutFormat ? 1 : 0;
   vkGetPhysicalDeviceQueueFamilyProperties(s->physical, &count, NULL);
   queues = calloc(count, sizeof(*queues));
   if (!queues) { fprintf(stderr, "venus_spirv_replay: queue allocation failed\\n"); return 0; }
   vkGetPhysicalDeviceQueueFamilyProperties(s->physical, &count, queues);
   s->queue_family = UINT32_MAX;
   for (i = 0; i < count; ++i)
      if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { s->queue_family = i; break; }
   free(queues);
   if (s->queue_family == UINT32_MAX) { fprintf(stderr, "venus_spirv_replay: no compute queue\\n"); return 0; }
   queue_info.queueFamilyIndex = s->queue_family;
   queue_info.queueCount = 1;
   queue_info.pQueuePriorities = &priority;
   device_info.queueCreateInfoCount = 1;
   device_info.pQueueCreateInfos = &queue_info;
   device_info.pEnabledFeatures = &enabled_features;
   result = vkCreateDevice(s->physical, &device_info, NULL, &s->device);
   if (result != VK_SUCCESS) { fprintf(stderr, "venus_spirv_replay: vkCreateDevice=%d\\n", result); return 0; }
   vkGetDeviceQueue(s->device, s->queue_family, 0, &s->queue);
   pool_info.queueFamilyIndex = s->queue_family;
   result = vkCreateCommandPool(s->device, &pool_info, NULL, &s->command_pool);
   if (result != VK_SUCCESS) { fprintf(stderr, "venus_spirv_replay: vkCreateCommandPool=%d\\n", result); return 0; }
   command_info.commandPool = s->command_pool;
   command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   command_info.commandBufferCount = 1;
   result = vkAllocateCommandBuffers(s->device, &command_info, &s->command);
   if (result != VK_SUCCESS) { fprintf(stderr, "venus_spirv_replay: vkAllocateCommandBuffers=%d\\n", result); return 0; }
   result = vkCreateFence(s->device, &fence_info, NULL, &s->fence);
   if (result != VK_SUCCESS) { fprintf(stderr, "venus_spirv_replay: vkCreateFence=%d\\n", result); return 0; }
   return 1;
}

static void cleanup(struct replay_state *s)
{
   if (!s->device) return;
   vkDeviceWaitIdle(s->device);
   if (s->sampler) vkDestroySampler(s->device, s->sampler, NULL);
   if (s->input_view) vkDestroyImageView(s->device, s->input_view, NULL);
   if (s->output_view) vkDestroyImageView(s->device, s->output_view, NULL);
   if (s->input_image) vkDestroyImage(s->device, s->input_image, NULL);
   if (s->output_image) vkDestroyImage(s->device, s->output_image, NULL);
   if (s->input_memory) vkFreeMemory(s->device, s->input_memory, NULL);
   if (s->output_memory) vkFreeMemory(s->device, s->output_memory, NULL);
   if (s->upload_buffer) vkDestroyBuffer(s->device, s->upload_buffer, NULL);
   if (s->readback_buffer) vkDestroyBuffer(s->device, s->readback_buffer, NULL);
   if (s->upload_memory) vkFreeMemory(s->device, s->upload_memory, NULL);
   if (s->readback_memory) vkFreeMemory(s->device, s->readback_memory, NULL);
   if (s->fence) vkDestroyFence(s->device, s->fence, NULL);
   if (s->command_pool) vkDestroyCommandPool(s->device, s->command_pool, NULL);
   vkDestroyDevice(s->device, NULL);
   s->device = VK_NULL_HANDLE;
   if (s->instance) vkDestroyInstance(s->instance, NULL);
   s->instance = VK_NULL_HANDLE;
}

int main(int argc, char **argv)
{
   struct replay_state state;
   DIR *dir;
   DIR *frozen_dir;
   struct dirent *entry;
   struct dirent *frozen_entry;
   char path[1024];
   char frozen_path[1024];
   char saved_selected[sizeof(state.selected_spv)];
   char saved_report[sizeof(state.candidate_report)];
   char golden_path[1024];
   char golden_storage_path[1024];
   char golden_spec_path[1024];
   char golden_vector_spec_path[1024];
   char saved_contract[sizeof(state.descriptor_contract)];
   char saved_specialization[sizeof(state.specialization_constants)];
   uint32_t saved_candidate_count, saved_pipeline_count, saved_execute_count;
   uint32_t exact_value;
   int pass = 0;
   memset(&state, 0, sizeof(state));
   state.run_id = argument_value(argc, argv, "--run-id", "manual");
   state.test_id = argument_value(argc, argv, "--test-id", "dxvk-exact-replay-cs");
   state.result_path = argument_value(argc, argv, "--result", "");
   state.spv_dir = argument_value(argc, argv, "--spv-dir", "");
   state.expected_value = EXPECTED_COLOR;
   state.started_ms = now_ms();
   write_result(&state, "started", "startup", "Exact DXVK remapped SPIR-V replay started");
   if (!state.spv_dir[0] || !init_vulkan(&state)) {
      snprintf(state.failure, sizeof(state.failure), "Vulkan initialization failed");
      write_result(&state, "FAIL", "host-vulkan", state.failure);
      cleanup(&state);
      return 1;
   }
   if (!create_resources(&state)) {
      snprintf(state.failure, sizeof(state.failure), "replay resources or input upload failed");
      write_result(&state, "FAIL", "venus", state.failure);
      cleanup(&state);
      return 1;
   }
   dir = opendir(state.spv_dir);
   if (!dir) {
      snprintf(state.failure, sizeof(state.failure), "SPIR-V directory unavailable: %s", strerror(errno));
      write_result(&state, "FAIL", "dxvk", state.failure);
      cleanup(&state);
      return 1;
   }
   while ((entry = readdir(dir)) != NULL) {
      if (strncmp(entry->d_name, "CS_", 3) || !strstr(entry->d_name, ".remapped.spv")) continue;
      snprintf(path, sizeof(path), "%s/%s", state.spv_dir, entry->d_name);
      if (replay_one(&state, path)) { pass = 1; break; }
   }
   closedir(dir);
   exact_value = state.sampled_value;
   saved_candidate_count = state.candidate_count;
   saved_pipeline_count = state.pipeline_create_count;
   saved_execute_count = state.shader_execute_count;
   memcpy(saved_selected, state.selected_spv, sizeof(saved_selected));
   memcpy(saved_report, state.candidate_report, sizeof(saved_report));
   memcpy(saved_contract, state.descriptor_contract, sizeof(saved_contract));
   memcpy(saved_specialization, state.specialization_constants, sizeof(saved_specialization));
   state.candidate_report[0] = 0;
   frozen_dir = opendir("share/winehua/replay_frozen");
   if (frozen_dir) {
      while ((frozen_entry = readdir(frozen_dir)) != NULL) {
         if (strncmp(frozen_entry->d_name, "CS_", 3) ||
             !strstr(frozen_entry->d_name, ".remapped.spv")) continue;
         snprintf(frozen_path, sizeof(frozen_path), "share/winehua/replay_frozen/%s",
                  frozen_entry->d_name);
         if (replay_one(&state, frozen_path)) state.frozen_pass = 1;
      }
      closedir(frozen_dir);
   }
   snprintf(state.frozen_report, sizeof(state.frozen_report), "%s", state.candidate_report);
   snprintf(golden_path, sizeof(golden_path),
            "share/winehua/venus_dxvk_contract_sample.spv");
   state.golden_pass = replay_one(&state, golden_path);
   state.golden_value = state.sampled_value;
   snprintf(golden_storage_path, sizeof(golden_storage_path),
            "share/winehua/venus_dxvk_contract_unknown_sample.spv");
   state.golden_storage_pass = replay_one(&state, golden_storage_path);
   state.golden_storage_value = state.sampled_value;
   state.golden_storage_pass = state.golden_storage_value != 0 &&
      state.golden_storage_value != SHADER_NOT_EXECUTED;
   snprintf(golden_spec_path, sizeof(golden_spec_path),
            "share/winehua/venus_dxvk_contract_spec_sample.spv");
   (void)replay_one(&state, golden_spec_path);
   state.golden_spec_value = state.sampled_value;
   state.golden_spec_pass = state.golden_spec_value != 0 &&
      state.golden_spec_value != SHADER_NOT_EXECUTED;
   snprintf(golden_vector_spec_path, sizeof(golden_vector_spec_path),
            "share/winehua/venus_dxvk_contract_vector_spec_sample.spv");
   (void)replay_one(&state, golden_vector_spec_path);
   state.golden_vector_spec_value = state.sampled_value;
   state.golden_vector_spec_pass = state.golden_vector_spec_value != 0 &&
      state.golden_vector_spec_value != SHADER_NOT_EXECUTED;
   state.sampled_value = exact_value;
   state.candidate_count = saved_candidate_count;
   state.pipeline_create_count = saved_pipeline_count;
   state.shader_execute_count = saved_execute_count;
   memcpy(state.candidate_report, saved_report, sizeof(state.candidate_report));
   memcpy(state.selected_spv, saved_selected, sizeof(state.selected_spv));
   memcpy(state.descriptor_contract, saved_contract, sizeof(state.descriptor_contract));
   memcpy(state.specialization_constants, saved_specialization, sizeof(state.specialization_constants));
   if (!pass) {
      snprintf(state.failure, sizeof(state.failure),
               "Exact CS replay failed: no sampled-image candidate produced 0x%08x (last=0x%08x)",
               state.expected_value, state.sampled_value);
      write_result(&state, "FAIL", "dxvk", state.failure);
      cleanup(&state);
      return 1;
   }
   write_result(&state, "PASS", "dxvk", "Exact DXVK remapped SPIR-V replay passed");
   cleanup(&state);
   return 0;
}
