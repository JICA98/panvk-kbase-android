#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kElements = 1024;
constexpr VkDeviceSize kBytes = VkDeviceSize(kElements) * sizeof(uint32_t);
constexpr uint64_t kFenceTimeoutNs = 30'000'000'000ull;
constexpr unsigned kDefaultRounds = 2;
constexpr unsigned kMaxRounds = 3;
constexpr uint32_t kGuardCycles = 8;

enum class ReadMode { Generic, Storage };

static const char* mode_name(ReadMode mode)
{
    return mode == ReadMode::Generic ? "generic-shader-read" : "shader-storage-read";
}

static VkAccessFlags2 read_access(ReadMode mode)
{
    return mode == ReadMode::Generic ? VK_ACCESS_2_SHADER_READ_BIT
                                     : VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
}

static VkAccessFlags2 shader_rw(ReadMode mode)
{
    return read_access(mode) | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
}

static void check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + ": VkResult=" +
                                 std::to_string(static_cast<int>(result)));
}

[[noreturn]] static void abort_pending_submission(const char* what, VkResult result)
{
    std::fprintf(stderr,
                 "FAIL %s: VkResult=%d; submission state may be pending, "
                 "skipping Vulkan cleanup\n",
                 what, static_cast<int>(result));
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(1);
}

template <typename T>
static T required(PFN_vkVoidFunction raw, const char* name)
{
    if (!raw)
        throw std::runtime_error(std::string("Missing Vulkan entry point: ") + name);
    return reinterpret_cast<T>(raw);
}

template <typename T>
static T instance_fn(PFN_vkGetInstanceProcAddr get, VkInstance instance,
                     const char* name)
{
    return required<T>(get(instance, name), name);
}

static uint64_t monotonic_ns()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

static unsigned parse_rounds(const char* text)
{
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (!end || *end != '\0' || value < 1 || value > kMaxRounds)
        throw std::runtime_error("rounds must be in 1..3");
    return static_cast<unsigned>(value);
}

struct Fns {
    PFN_vkDestroyInstance DestroyInstance{};
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices{};
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties{};
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties{};
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties{};
    PFN_vkCreateDevice CreateDevice{};
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr{};

    PFN_vkDestroyDevice DestroyDevice{};
    PFN_vkGetDeviceQueue GetDeviceQueue{};
    PFN_vkCreateBuffer CreateBuffer{};
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements{};
    PFN_vkAllocateMemory AllocateMemory{};
    PFN_vkBindBufferMemory BindBufferMemory{};
    PFN_vkMapMemory MapMemory{};
    PFN_vkUnmapMemory UnmapMemory{};
    PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges{};
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges{};
    PFN_vkCreateBufferView CreateBufferView{};
    PFN_vkDestroyBufferView DestroyBufferView{};
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout{};
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout{};
    PFN_vkCreateDescriptorPool CreateDescriptorPool{};
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool{};
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets{};
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets{};
    PFN_vkCreatePipelineLayout CreatePipelineLayout{};
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout{};
    PFN_vkCreateShaderModule CreateShaderModule{};
    PFN_vkDestroyShaderModule DestroyShaderModule{};
    PFN_vkCreateComputePipelines CreateComputePipelines{};
    PFN_vkDestroyPipeline DestroyPipeline{};
    PFN_vkCreateCommandPool CreateCommandPool{};
    PFN_vkDestroyCommandPool DestroyCommandPool{};
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers{};
    PFN_vkBeginCommandBuffer BeginCommandBuffer{};
    PFN_vkEndCommandBuffer EndCommandBuffer{};
    PFN_vkCmdBindPipeline CmdBindPipeline{};
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets{};
    PFN_vkCmdPushConstants CmdPushConstants{};
    PFN_vkCmdDispatch CmdDispatch{};
    PFN_vkCmdCopyBuffer CmdCopyBuffer{};
    PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2{};
    PFN_vkCreateFence CreateFence{};
    PFN_vkDestroyFence DestroyFence{};
    PFN_vkResetFences ResetFences{};
    PFN_vkQueueSubmit QueueSubmit{};
    PFN_vkWaitForFences WaitForFences{};
    PFN_vkFreeMemory FreeMemory{};
    PFN_vkDestroyBuffer DestroyBuffer{};
};

struct BufferMem {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
    VkMemoryPropertyFlags properties{};
};

struct Context {
    Fns f;
    VkDevice device{};
    VkQueue queue{};
    uint32_t queue_family{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
};

static uint32_t choose_memory_type(const VkPhysicalDeviceMemoryProperties& properties,
                                   uint32_t bits, VkMemoryPropertyFlags required_flags,
                                   VkMemoryPropertyFlags preferred_flags)
{
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) != 0 && (flags & required_flags) == required_flags &&
            (flags & preferred_flags) == preferred_flags)
            return i;
    }
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) != 0 && (flags & required_flags) == required_flags)
            return i;
    }
    return properties.memoryTypeCount;
}

static void create_buffer(Context& c, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags required_flags,
                          VkMemoryPropertyFlags preferred_flags, bool map,
                          BufferMem& result)
{
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(c.f.CreateBuffer(c.device, &info, nullptr, &result.buffer), "CreateBuffer");

    VkMemoryRequirements requirements{};
    c.f.GetBufferMemoryRequirements(c.device, result.buffer, &requirements);
    const uint32_t type = choose_memory_type(c.memory_properties,
                                              requirements.memoryTypeBits,
                                              required_flags, preferred_flags);
    if (type == c.memory_properties.memoryTypeCount)
        throw std::runtime_error("No compatible Vulkan memory type");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    check(c.f.AllocateMemory(c.device, &allocation, nullptr, &result.memory),
          "AllocateMemory");
    result.properties = c.memory_properties.memoryTypes[type].propertyFlags;
    check(c.f.BindBufferMemory(c.device, result.buffer, result.memory, 0),
          "BindBufferMemory");
    if (map)
        check(c.f.MapMemory(c.device, result.memory, 0, VK_WHOLE_SIZE, 0,
                            &result.mapped), "MapMemory");
}

static void flush_if_needed(Context& c, const BufferMem& memory)
{
    if ((memory.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
        return;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory.memory;
    range.size = VK_WHOLE_SIZE;
    check(c.f.FlushMappedMemoryRanges(c.device, 1, &range), "FlushMappedMemoryRanges");
}

static void invalidate_if_needed(Context& c, const BufferMem& memory)
{
    if ((memory.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
        return;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory.memory;
    range.size = VK_WHOLE_SIZE;
    check(c.f.InvalidateMappedMemoryRanges(c.device, 1, &range),
          "InvalidateMappedMemoryRanges");
}

static std::vector<uint32_t> read_spirv(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw std::runtime_error("Cannot open SPIR-V: " + path);
    const std::streamsize size = stream.tellg();
    if (size <= 0 || (size % 4) != 0)
        throw std::runtime_error("Invalid SPIR-V size: " + path);
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(words.data()), size);
    if (!stream)
        throw std::runtime_error("Cannot read SPIR-V: " + path);
    return words;
}

static VkShaderModule create_shader(Context& c, const std::string& path)
{
    const auto words = read_spirv(path);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.size() * sizeof(uint32_t);
    info.pCode = words.data();
    VkShaderModule shader{};
    check(c.f.CreateShaderModule(c.device, &info, nullptr, &shader), "CreateShaderModule");
    return shader;
}

static VkPipeline create_compute_pipeline(Context& c, const std::string& path,
                                           VkPipelineLayout layout)
{
    VkShaderModule shader = create_shader(c, path);
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = shader;
    info.stage.pName = "main";
    info.layout = layout;
    VkPipeline pipeline{};
    const VkResult result = c.f.CreateComputePipelines(c.device, VK_NULL_HANDLE, 1,
                                                        &info, nullptr, &pipeline);
    c.f.DestroyShaderModule(c.device, shader, nullptr);
    check(result, "CreateComputePipelines");
    return pipeline;
}

static VkDescriptorSetLayout create_layout(Context& c,
                                            const std::vector<VkDescriptorSetLayoutBinding>& bindings)
{
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout{};
    check(c.f.CreateDescriptorSetLayout(c.device, &info, nullptr, &layout),
          "CreateDescriptorSetLayout");
    return layout;
}

static VkPipelineLayout create_pipeline_layout(Context& c, VkDescriptorSetLayout layout,
                                                uint32_t push_size)
{
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 1;
    info.pSetLayouts = &layout;
    if (push_size != 0) {
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &push;
    }
    VkPipelineLayout result{};
    check(c.f.CreatePipelineLayout(c.device, &info, nullptr, &result),
          "CreatePipelineLayout");
    return result;
}

static void barrier_stages(Context& c, VkCommandBuffer command, VkBuffer buffer,
                           VkPipelineStageFlags2 source_stage, VkAccessFlags2 source,
                           VkPipelineStageFlags2 destination_stage,
                           VkAccessFlags2 destination)
{
    VkBufferMemoryBarrier2 info{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    info.srcStageMask = source_stage;
    info.srcAccessMask = source;
    info.dstStageMask = destination_stage;
    info.dstAccessMask = destination;
    info.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    info.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    info.buffer = buffer;
    info.offset = 0;
    info.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &info;
    c.f.CmdPipelineBarrier2(command, &dependency);
}

static void barrier(Context& c, VkCommandBuffer command, VkBuffer buffer,
                    VkAccessFlags2 source, VkAccessFlags2 destination)
{
    barrier_stages(c, command, buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, source,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, destination);
}

static void host_read_barrier(Context& c, VkCommandBuffer command, VkBuffer buffer)
{
    barrier_stages(c, command, buffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                   VK_ACCESS_2_HOST_READ_BIT);
}

static VkCommandBuffer allocate_command(Context& c, VkCommandPool pool)
{
    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer command{};
    check(c.f.AllocateCommandBuffers(c.device, &info, &command), "AllocateCommandBuffers");
    return command;
}

static std::vector<uint32_t> initial_values()
{
    std::vector<uint32_t> values(kElements);
    for (uint32_t i = 0; i < kElements; ++i)
        values[i] = 0x12345678u ^ (i * 0x9e3779b9u);
    return values;
}

static uint32_t lcg(uint32_t value, uint32_t iteration, uint32_t index)
{
    value ^= iteration * 0x9e3779b9u + index * 0x85ebca6bu;
    value = value * 1664525u + 1013904223u;
    return value ^ (value >> 16u);
}

static void destroy_buffer(Context& c, BufferMem& buffer)
{
    if (buffer.mapped)
        c.f.UnmapMemory(c.device, buffer.memory);
    if (buffer.buffer)
        c.f.DestroyBuffer(c.device, buffer.buffer, nullptr);
    if (buffer.memory)
        c.f.FreeMemory(c.device, buffer.memory, nullptr);
    buffer = {};
}

static void wait_submit(Context& c, VkFence fence, VkCommandBuffer command,
                        unsigned round, const char* label, uint64_t& elapsed_ns)
{
    if (round != 0)
        check(c.f.ResetFences(c.device, 1, &fence), "ResetFences");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    const uint64_t start = monotonic_ns();
    const VkResult submit_result = c.f.QueueSubmit(c.queue, 1, &submit, fence);
    if (submit_result != VK_SUCCESS)
        abort_pending_submission("QueueSubmit", submit_result);
    const VkResult wait = c.f.WaitForFences(c.device, 1, &fence, VK_TRUE,
                                            kFenceTimeoutNs);
    const uint64_t end = monotonic_ns();
    if (wait != VK_SUCCESS)
        abort_pending_submission(label, wait);
    elapsed_ns = end - start;
    std::printf("sample=%s round=%u submit_fence_ms=%.3f\n", label, round,
                static_cast<double>(elapsed_ns) / 1.0e6);
}

static void run_ssbo_case(Context& c, const std::string& shader_dir, ReadMode mode,
                          unsigned rounds, uint32_t iterations)
{
    BufferMem upload{}, first{}, second{}, readback{};
    VkDescriptorSetLayout layout{};
    VkDescriptorPool pool{};
    VkPipelineLayout pipeline_layout{};
    VkPipeline pipeline{};
    VkCommandPool command_pool{};
    VkFence fence{};
    try {
        const auto initial = initial_values();
        create_buffer(c, kBytes * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      true, upload);
        auto* upload_values = static_cast<uint32_t*>(upload.mapped);
        std::copy(initial.begin(), initial.end(), upload_values);
        std::fill(upload_values + kElements, upload_values + 2 * kElements, 0u);
        flush_if_needed(c, upload);
        create_buffer(c, kBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, first);
        create_buffer(c, kBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, second);
        create_buffer(c, kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      true, readback);

        layout = create_layout(c, {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        });
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = 2;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        check(c.f.CreateDescriptorPool(c.device, &pool_info, nullptr, &pool),
              "CreateDescriptorPool");
        std::array<VkDescriptorSetLayout, 2> layouts{layout, layout};
        std::array<VkDescriptorSet, 2> sets{};
        VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_info.descriptorPool = pool;
        set_info.descriptorSetCount = 2;
        set_info.pSetLayouts = layouts.data();
        check(c.f.AllocateDescriptorSets(c.device, &set_info, sets.data()),
              "AllocateDescriptorSets");
        VkDescriptorBufferInfo first_info{first.buffer, 0, kBytes};
        VkDescriptorBufferInfo second_info{second.buffer, 0, kBytes};
        std::array<VkWriteDescriptorSet, 4> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[0], 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &first_info, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[0], 1, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &second_info, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[1], 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &second_info, nullptr};
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[1], 1, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &first_info, nullptr};
        c.f.UpdateDescriptorSets(c.device, static_cast<uint32_t>(writes.size()), writes.data(),
                                 0, nullptr);
        pipeline_layout = create_pipeline_layout(c, layout, sizeof(uint32_t));
        pipeline = create_compute_pipeline(c, shader_dir + "/ssbo_pingpong.comp.spv",
                                           pipeline_layout);
        VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        command_pool_info.queueFamilyIndex = c.queue_family;
        check(c.f.CreateCommandPool(c.device, &command_pool_info, nullptr, &command_pool),
              "CreateCommandPool");
        const VkCommandBuffer command = allocate_command(c, command_pool);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(c.f.BeginCommandBuffer(command, &begin), "BeginCommandBuffer");
        VkBufferCopy copy_a{0, 0, kBytes};
        VkBufferCopy copy_b{kBytes, 0, kBytes};
        c.f.CmdCopyBuffer(command, upload.buffer, first.buffer, 1, &copy_a);
        c.f.CmdCopyBuffer(command, upload.buffer, second.buffer, 1, &copy_b);
        const VkAccessFlags2 rw = shader_rw(mode);
        barrier(c, command, first.buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, rw);
        barrier(c, command, second.buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, rw);
        c.f.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
            const VkDescriptorSet set = (iteration & 1u) == 0 ? sets[0] : sets[1];
            c.f.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                      pipeline_layout, 0, 1, &set, 0, nullptr);
            c.f.CmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                 0, sizeof(iteration), &iteration);
            c.f.CmdDispatch(command, kElements / 64, 1, 1);
            barrier(c, command, first.buffer, rw, rw);
            barrier(c, command, second.buffer, rw, rw);
        }
        VkBuffer final_buffer = (iterations & 1u) == 0 ? first.buffer : second.buffer;
        barrier(c, command, final_buffer, rw, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy final_copy{0, 0, kBytes};
        c.f.CmdCopyBuffer(command, final_buffer, readback.buffer, 1, &final_copy);
        host_read_barrier(c, command, readback.buffer);
        check(c.f.EndCommandBuffer(command), "EndCommandBuffer");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(c.f.CreateFence(c.device, &fence_info, nullptr, &fence), "CreateFence");

        std::vector<uint32_t> expected = initial;
        for (uint32_t iteration = 0; iteration < iterations; ++iteration)
            for (uint32_t i = 0; i < kElements; ++i)
                expected[i] = lcg(expected[i], iteration, i);
        const std::string sample_label = std::string(mode_name(mode)) +
                                         "-iterations-" + std::to_string(iterations);
        for (unsigned round = 0; round < rounds; ++round) {
            uint64_t elapsed = 0;
            wait_submit(c, fence, command, round, sample_label.c_str(), elapsed);
            invalidate_if_needed(c, readback);
            const auto* values = static_cast<const uint32_t*>(readback.mapped);
            for (uint32_t i = 0; i < kElements; ++i) {
                if (values[i] != expected[i])
                    throw std::runtime_error(std::string("SSBO mismatch mode=") +
                                             mode_name(mode) + " index=" +
                                             std::to_string(i));
            }
        }
        std::printf("PASS workload=ssbo-pingpong mode=%s iterations=%u elements=%u\n",
                    mode_name(mode), iterations, kElements);
    } catch (...) {
        if (fence) c.f.DestroyFence(c.device, fence, nullptr);
        if (command_pool) c.f.DestroyCommandPool(c.device, command_pool, nullptr);
        if (pipeline) c.f.DestroyPipeline(c.device, pipeline, nullptr);
        if (pipeline_layout) c.f.DestroyPipelineLayout(c.device, pipeline_layout, nullptr);
        if (pool) c.f.DestroyDescriptorPool(c.device, pool, nullptr);
        if (layout) c.f.DestroyDescriptorSetLayout(c.device, layout, nullptr);
        destroy_buffer(c, readback);
        destroy_buffer(c, second);
        destroy_buffer(c, first);
        destroy_buffer(c, upload);
        throw;
    }
    c.f.DestroyFence(c.device, fence, nullptr);
    c.f.DestroyCommandPool(c.device, command_pool, nullptr);
    c.f.DestroyPipeline(c.device, pipeline, nullptr);
    c.f.DestroyPipelineLayout(c.device, pipeline_layout, nullptr);
    c.f.DestroyDescriptorPool(c.device, pool, nullptr);
    c.f.DestroyDescriptorSetLayout(c.device, layout, nullptr);
    destroy_buffer(c, readback);
    destroy_buffer(c, second);
    destroy_buffer(c, first);
    destroy_buffer(c, upload);
}

static uint32_t guard_value(uint32_t seed, uint32_t index)
{
    uint32_t value = seed ^ (index * 0x27d4eb2du);
    value = value * 747796405u + 2891336453u;
    return value ^ (value >> 13u);
}

static uint32_t guard_seed(uint32_t cycle)
{
    return 0x51ed270bu + cycle * 0x9e3779b9u;
}

static void run_texel_guard(Context& c, const std::string& shader_dir,
                            ReadMode mode, unsigned rounds)
{
    BufferMem source{}, output{}, readback{};
    VkBufferView view{};
    VkDescriptorSetLayout write_layout{}, sample_layout{};
    VkDescriptorPool pool{};
    VkPipelineLayout write_pipeline_layout{}, sample_pipeline_layout{};
    VkPipeline write_pipeline{}, sample_pipeline{};
    VkCommandPool command_pool{};
    VkFence fence{};
    try {
        create_buffer(c, kBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
                      0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, source);
        create_buffer(c, kBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, output);
        create_buffer(c, kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      true, readback);
        VkBufferViewCreateInfo view_info{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
        view_info.buffer = source.buffer;
        view_info.format = VK_FORMAT_R32_UINT;
        view_info.range = kBytes;
        check(c.f.CreateBufferView(c.device, &view_info, nullptr, &view),
              "CreateBufferView uniform texel");

        write_layout = create_layout(c, {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        });
        sample_layout = create_layout(c, {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        });
        VkDescriptorPoolSize pool_sizes[3]{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1},
        };
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = 2;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes = pool_sizes;
        check(c.f.CreateDescriptorPool(c.device, &pool_info, nullptr, &pool),
              "CreateDescriptorPool texel guard");
        std::array<VkDescriptorSetLayout, 2> layouts{write_layout, sample_layout};
        std::array<VkDescriptorSet, 2> sets{};
        VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_info.descriptorPool = pool;
        set_info.descriptorSetCount = 2;
        set_info.pSetLayouts = layouts.data();
        check(c.f.AllocateDescriptorSets(c.device, &set_info, sets.data()),
              "AllocateDescriptorSets texel guard");
        VkDescriptorBufferInfo source_info{source.buffer, 0, kBytes};
        VkDescriptorBufferInfo output_info{output.buffer, 0, kBytes};
        VkWriteDescriptorSet writes[3]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[0], 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &source_info, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[1], 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, nullptr, nullptr, &view};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[1], 1, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &output_info, nullptr};
        c.f.UpdateDescriptorSets(c.device, 3, writes, 0, nullptr);
        write_pipeline_layout = create_pipeline_layout(c, write_layout, sizeof(uint32_t));
        sample_pipeline_layout = create_pipeline_layout(c, sample_layout, 0);
        write_pipeline = create_compute_pipeline(c, shader_dir + "/guard_write.comp.spv",
                                                 write_pipeline_layout);
        sample_pipeline = create_compute_pipeline(c, shader_dir + "/guard_sample.comp.spv",
                                                  sample_pipeline_layout);
        VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        command_pool_info.queueFamilyIndex = c.queue_family;
        check(c.f.CreateCommandPool(c.device, &command_pool_info, nullptr, &command_pool),
              "CreateCommandPool texel guard");
        const VkCommandBuffer command = allocate_command(c, command_pool);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(c.f.BeginCommandBuffer(command, &begin), "BeginCommandBuffer texel guard");
        for (uint32_t cycle = 0; cycle < kGuardCycles; ++cycle) {
            if (cycle != 0) {
                barrier(c, command, source.buffer, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                barrier(c, command, output.buffer, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            }
            const uint32_t seed = guard_seed(cycle);
            c.f.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, write_pipeline);
            c.f.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                      write_pipeline_layout, 0, 1, &sets[0], 0, nullptr);
            c.f.CmdPushConstants(command, write_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                 0, sizeof(seed), &seed);
            c.f.CmdDispatch(command, kElements / 64, 1, 1);
            barrier(c, command, source.buffer, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            c.f.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, sample_pipeline);
            c.f.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                      sample_pipeline_layout, 0, 1, &sets[1], 0, nullptr);
            c.f.CmdDispatch(command, kElements / 64, 1, 1);
        }
        barrier(c, command, output.buffer, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy copy{0, 0, kBytes};
        c.f.CmdCopyBuffer(command, output.buffer, readback.buffer, 1, &copy);
        host_read_barrier(c, command, readback.buffer);
        check(c.f.EndCommandBuffer(command), "EndCommandBuffer texel guard");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(c.f.CreateFence(c.device, &fence_info, nullptr, &fence),
              "CreateFence texel guard");
        for (unsigned round = 0; round < rounds; ++round) {
            uint64_t elapsed = 0;
            wait_submit(c, fence, command, round, "uniform-texel-guard", elapsed);
            invalidate_if_needed(c, readback);
            const auto* values = static_cast<const uint32_t*>(readback.mapped);
            const uint32_t seed = guard_seed(kGuardCycles - 1);
            for (uint32_t i = 0; i < kElements; ++i) {
                if (values[i] != (guard_value(seed, i) ^ 0xa5a5a5a5u))
                    throw std::runtime_error("uniform texel guard mismatch index=" +
                                             std::to_string(i));
            }
        }
        std::printf("PASS workload=uniform-texel-buffer-gpu-write mode=%s cycles=%u elements=%u\n",
                    mode_name(mode), kGuardCycles, kElements);
    } catch (...) {
        if (fence) c.f.DestroyFence(c.device, fence, nullptr);
        if (command_pool) c.f.DestroyCommandPool(c.device, command_pool, nullptr);
        if (sample_pipeline) c.f.DestroyPipeline(c.device, sample_pipeline, nullptr);
        if (write_pipeline) c.f.DestroyPipeline(c.device, write_pipeline, nullptr);
        if (sample_pipeline_layout) c.f.DestroyPipelineLayout(c.device, sample_pipeline_layout, nullptr);
        if (write_pipeline_layout) c.f.DestroyPipelineLayout(c.device, write_pipeline_layout, nullptr);
        if (pool) c.f.DestroyDescriptorPool(c.device, pool, nullptr);
        if (sample_layout) c.f.DestroyDescriptorSetLayout(c.device, sample_layout, nullptr);
        if (write_layout) c.f.DestroyDescriptorSetLayout(c.device, write_layout, nullptr);
        if (view) c.f.DestroyBufferView(c.device, view, nullptr);
        destroy_buffer(c, readback);
        destroy_buffer(c, output);
        destroy_buffer(c, source);
        throw;
    }
    c.f.DestroyFence(c.device, fence, nullptr);
    c.f.DestroyCommandPool(c.device, command_pool, nullptr);
    c.f.DestroyPipeline(c.device, sample_pipeline, nullptr);
    c.f.DestroyPipeline(c.device, write_pipeline, nullptr);
    c.f.DestroyPipelineLayout(c.device, sample_pipeline_layout, nullptr);
    c.f.DestroyPipelineLayout(c.device, write_pipeline_layout, nullptr);
    c.f.DestroyDescriptorPool(c.device, pool, nullptr);
    c.f.DestroyDescriptorSetLayout(c.device, sample_layout, nullptr);
    c.f.DestroyDescriptorSetLayout(c.device, write_layout, nullptr);
    c.f.DestroyBufferView(c.device, view, nullptr);
    destroy_buffer(c, readback);
    destroy_buffer(c, output);
    destroy_buffer(c, source);
}

static int run(const char* library_path, const std::string& shader_dir,
               unsigned rounds)
{
    void* library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
    if (!library)
        throw std::runtime_error(std::string("dlopen: ") + dlerror());
    try {
        auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(library, "vk_icdGetInstanceProcAddr"));
        auto negotiate = reinterpret_cast<VkResult (*)(uint32_t*)>(
            dlsym(library, "vk_icdNegotiateLoaderICDInterfaceVersion"));
        if (!get || !negotiate)
            throw std::runtime_error("ICD entry points missing");
        uint32_t loader_version = 7;
        check(negotiate(&loader_version), "vk_icdNegotiateLoaderICDInterfaceVersion");

        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Bachata buffer cache driver probe";
        application.applicationVersion = 1;
        application.pEngineName = "standalone";
        application.engineVersion = 1;
        application.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.pApplicationInfo = &application;
        VkInstance instance{};
        auto create_instance = instance_fn<PFN_vkCreateInstance>(get, VK_NULL_HANDLE,
                                                                  "vkCreateInstance");
        check(create_instance(&instance_info, nullptr, &instance), "CreateInstance");
        Fns f{};
        f.DestroyInstance = instance_fn<PFN_vkDestroyInstance>(get, instance, "vkDestroyInstance");
        f.EnumeratePhysicalDevices = instance_fn<PFN_vkEnumeratePhysicalDevices>(
            get, instance, "vkEnumeratePhysicalDevices");
        f.GetPhysicalDeviceProperties = instance_fn<PFN_vkGetPhysicalDeviceProperties>(
            get, instance, "vkGetPhysicalDeviceProperties");
        f.GetPhysicalDeviceFeatures2 = instance_fn<PFN_vkGetPhysicalDeviceFeatures2>(
            get, instance, "vkGetPhysicalDeviceFeatures2");
        f.GetPhysicalDeviceQueueFamilyProperties = instance_fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            get, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        f.GetPhysicalDeviceMemoryProperties = instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>(
            get, instance, "vkGetPhysicalDeviceMemoryProperties");
        f.CreateDevice = instance_fn<PFN_vkCreateDevice>(get, instance, "vkCreateDevice");
        f.GetDeviceProcAddr = instance_fn<PFN_vkGetDeviceProcAddr>(get, instance,
                                                                   "vkGetDeviceProcAddr");

        uint32_t count = 0;
        check(f.EnumeratePhysicalDevices(instance, &count, nullptr),
              "EnumeratePhysicalDevices count");
        if (count == 0)
            throw std::runtime_error("No Vulkan physical device");
        std::vector<VkPhysicalDevice> devices(count);
        check(f.EnumeratePhysicalDevices(instance, &count, devices.data()),
              "EnumeratePhysicalDevices");
        VkPhysicalDevice physical = devices[0];
        VkPhysicalDeviceProperties properties{};
        f.GetPhysicalDeviceProperties(physical, &properties);
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties candidate_properties{};
            f.GetPhysicalDeviceProperties(candidate, &candidate_properties);
            if (candidate_properties.vendorID == 0x13b5) {
                physical = candidate;
                properties = candidate_properties;
                break;
            }
        }
        std::printf("device name=\"%s\" vendor=0x%04x device=0x%04x driver=0x%08x api=%u.%u.%u\n",
                    properties.deviceName, properties.vendorID, properties.deviceID,
                    properties.driverVersion, VK_VERSION_MAJOR(properties.apiVersion),
                    VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion));
        std::printf("icd=%s rounds=%u workloads=512,2048 barriers=ALL_COMMANDS read_masks=generic|storage\n",
                    library_path, rounds);

        VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan13Features vulkan13_available{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        available.pNext = &vulkan13_available;
        f.GetPhysicalDeviceFeatures2(physical, &available);
        if (!vulkan13_available.synchronization2 || !vulkan13_available.maintenance4)
            throw std::runtime_error("Vulkan 1.3 synchronization2+maintenance4 unavailable");

        uint32_t family_count = 0;
        f.GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        f.GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
        uint32_t family = 0;
        while (family < family_count &&
               (families[family].queueCount == 0 ||
                (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0))
            ++family;
        if (family == family_count)
            throw std::runtime_error("No compute queue family");
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features vulkan13_enabled{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        vulkan13_enabled.synchronization2 = VK_TRUE;
        vulkan13_enabled.maintenance4 = VK_TRUE;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.pNext = &vulkan13_enabled;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        Context context{};
        context.f = f;
        context.queue_family = family;
        check(f.CreateDevice(physical, &device_info, nullptr, &context.device), "CreateDevice");
        context.f.DestroyDevice = required<PFN_vkDestroyDevice>(
            f.GetDeviceProcAddr(context.device, "vkDestroyDevice"), "vkDestroyDevice");
#define LOAD_DEVICE(name) context.f.name = required<PFN_vk##name>( \
            f.GetDeviceProcAddr(context.device, "vk" #name), "vk" #name)
        LOAD_DEVICE(GetDeviceQueue);
        LOAD_DEVICE(CreateBuffer);
        LOAD_DEVICE(GetBufferMemoryRequirements);
        LOAD_DEVICE(AllocateMemory);
        LOAD_DEVICE(BindBufferMemory);
        LOAD_DEVICE(MapMemory);
        LOAD_DEVICE(UnmapMemory);
        LOAD_DEVICE(FlushMappedMemoryRanges);
        LOAD_DEVICE(InvalidateMappedMemoryRanges);
        LOAD_DEVICE(CreateBufferView);
        LOAD_DEVICE(DestroyBufferView);
        LOAD_DEVICE(CreateDescriptorSetLayout);
        LOAD_DEVICE(DestroyDescriptorSetLayout);
        LOAD_DEVICE(CreateDescriptorPool);
        LOAD_DEVICE(DestroyDescriptorPool);
        LOAD_DEVICE(AllocateDescriptorSets);
        LOAD_DEVICE(UpdateDescriptorSets);
        LOAD_DEVICE(CreatePipelineLayout);
        LOAD_DEVICE(DestroyPipelineLayout);
        LOAD_DEVICE(CreateShaderModule);
        LOAD_DEVICE(DestroyShaderModule);
        LOAD_DEVICE(CreateComputePipelines);
        LOAD_DEVICE(DestroyPipeline);
        LOAD_DEVICE(CreateCommandPool);
        LOAD_DEVICE(DestroyCommandPool);
        LOAD_DEVICE(AllocateCommandBuffers);
        LOAD_DEVICE(BeginCommandBuffer);
        LOAD_DEVICE(EndCommandBuffer);
        LOAD_DEVICE(CmdBindPipeline);
        LOAD_DEVICE(CmdBindDescriptorSets);
        LOAD_DEVICE(CmdPushConstants);
        LOAD_DEVICE(CmdDispatch);
        LOAD_DEVICE(CmdCopyBuffer);
        LOAD_DEVICE(CmdPipelineBarrier2);
        LOAD_DEVICE(CreateFence);
        LOAD_DEVICE(DestroyFence);
        LOAD_DEVICE(ResetFences);
        LOAD_DEVICE(QueueSubmit);
        LOAD_DEVICE(WaitForFences);
        LOAD_DEVICE(FreeMemory);
        LOAD_DEVICE(DestroyBuffer);
#undef LOAD_DEVICE
        context.f.GetDeviceQueue(context.device, family, 0, &context.queue);
        context.f.GetPhysicalDeviceMemoryProperties(physical, &context.memory_properties);

        run_ssbo_case(context, shader_dir, ReadMode::Generic, rounds, 512);
        run_ssbo_case(context, shader_dir, ReadMode::Generic, rounds, 2048);
        run_ssbo_case(context, shader_dir, ReadMode::Storage, rounds, 512);
        run_ssbo_case(context, shader_dir, ReadMode::Storage, rounds, 2048);
        run_texel_guard(context, shader_dir, ReadMode::Generic, rounds);
        run_texel_guard(context, shader_dir, ReadMode::Storage, rounds);
        std::printf("PASS buffer-cache-driver-probe all_cases=6\n");
        context.f.DestroyDevice(context.device, nullptr);
        f.DestroyInstance(instance, nullptr);
        dlclose(library);
        return 0;
    } catch (...) {
        dlclose(library);
        throw;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 4 || argv[1][0] != '/') {
        std::fprintf(stderr, "Usage: %s /absolute/path/libvulkan_panfrost.so shader-dir [rounds 1..3]\n",
                     argv[0]);
        return 2;
    }
    try {
        const unsigned rounds = argc == 4 ? parse_rounds(argv[3]) : kDefaultRounds;
        return run(argv[1], argv[2], rounds);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL buffer-cache-driver-probe: %s\n", error.what());
        return 1;
    }
}
