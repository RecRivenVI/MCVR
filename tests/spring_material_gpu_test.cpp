#include <volk.h>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void require(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan result " + std::to_string(result));
}
struct Vec4 { float x, y, z, w; };
static bool near(float a, float b, float epsilon = 2e-5f) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= epsilon;
}
static bool near3(Vec4 a, Vec4 b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

int main(int argc, char **argv) {
    try {
        if (argc != 2) return 1;
        require(volkInitialize());
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        VkInstance instance{};
        require(vkCreateInstance(&instanceInfo, nullptr, &instance));
        volkLoadInstance(instance);
        uint32_t deviceCount = 0;
        require(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr));
        std::vector<VkPhysicalDevice> physicals(deviceCount);
        require(vkEnumeratePhysicalDevices(instance, &deviceCount, physicals.data()));
        VkPhysicalDevice physical{};
        uint32_t family = 0;
        for (auto candidate : physicals) {
            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());
            for (uint32_t index = 0; index < families; ++index) {
                if ((queues[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
                    physical = candidate; family = index; break;
                }
            }
            if (physical) break;
        }
        if (!physical) { vkDestroyInstance(instance, nullptr); return 77; }
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        VkDevice device{};
        require(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
        volkLoadDevice(device);
        VkQueue queue{};
        vkGetDeviceQueue(device, family, 0, &queue);

        constexpr uint32_t cases = 10, rowsPerCase = 8;
        constexpr VkDeviceSize bytes = cases * rowsPerCase * sizeof(Vec4);
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = bytes; bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VkBuffer buffer{};
        require(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        uint32_t memoryType = memoryProperties.memoryTypeCount;
        constexpr auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags & host) == host) {
                memoryType = i; break;
            }
        if (memoryType == memoryProperties.memoryTypeCount)
            throw std::runtime_error("No host-coherent memory for bounded spring fixture");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = memoryType;
        VkDeviceMemory memory{};
        require(vkAllocateMemory(device, &allocation, nullptr, &memory));
        require(vkBindBufferMemory(device, buffer, memory, 0));
        void *mapped{};
        require(vkMapMemory(device, memory, 0, bytes, 0, &mapped));
        std::memset(mapped, 0, bytes);

        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 1; layoutInfo.pBindings = &binding;
        VkDescriptorSetLayout layout{};
        require(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout));
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1; poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize;
        VkDescriptorPool pool{};
        require(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool));
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool = pool; setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &layout;
        VkDescriptorSet set{};
        require(vkAllocateDescriptorSets(device, &setInfo, &set));
        VkDescriptorBufferInfo descriptor{buffer, 0, bytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set; write.dstBinding = 0; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &descriptor;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Missing spring material SPIR-V");
        size_t shaderBytes = static_cast<size_t>(file.tellg());
        if (shaderBytes == 0 || shaderBytes % 4 != 0) throw std::runtime_error("Invalid SPIR-V byte count");
        file.seekg(0);
        std::vector<uint32_t> words(shaderBytes / 4);
        file.read(reinterpret_cast<char *>(words.data()), shaderBytes);
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = shaderBytes; shaderInfo.pCode = words.data();
        VkShaderModule shader{};
        require(vkCreateShaderModule(device, &shaderInfo, nullptr, &shader));
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1; pipelineLayoutInfo.pSetLayouts = &layout;
        VkPipelineLayout pipelineLayout{};
        require(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout));
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
        VkPipeline pipeline{};
        require(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
            nullptr, &pipeline));

        VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commandPoolInfo.queueFamilyIndex = family;
        VkCommandPool commandPool{};
        require(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool));
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = commandPool; commandInfo.commandBufferCount = 1;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        VkCommandBuffer command{};
        require(vkAllocateCommandBuffers(device, &commandInfo, &command));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        require(vkBeginCommandBuffer(command, &begin));
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
            0, 1, &set, 0, nullptr);
        vkCmdDispatch(command, cases, 1, 1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        require(vkEndCommandBuffer(command));
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence{};
        require(vkCreateFence(device, &fenceInfo, nullptr, &fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        require(vkQueueSubmit(queue, 1, &submit, fence));
        require(vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000ull));

        const auto *rows = static_cast<const Vec4 *>(mapped);
        for (uint32_t i = 0; i < cases; ++i) {
            const Vec4 *r = rows + i * rowsPerCase;
            const float expectedZ = i == 2 ? -1.0f : 1.0f;
            if (!near(r[0].x, 0) || !near(r[0].y, 0) || !near(r[0].z, expectedZ) ||
                !near3(r[1], r[2]) || !near3(r[3], r[4]))
                throw std::runtime_error("Spring base/cache/normal-map mismatch case=" + std::to_string(i));
            if (i < 4) {
                const float originalLength = std::sqrt(0.3f * 0.3f + 0.4f * 0.4f
                    + 0.8660254f * 0.8660254f);
                float x = 0.3f / originalLength, y = 0.4f / originalLength;
                float z = 0.8660254f / originalLength;
                if (i == 2) { y = -y; z = -z; }
                if (i == 3) {
                    x *= 0.5f; y *= 2.0f; z *= 2.0f / 3.0f;
                    const float transformedLength = std::sqrt(x * x + y * y + z * z);
                    x /= transformedLength; y /= transformedLength; z /= transformedLength;
                }
                if (!near(r[1].x, x) || !near(r[1].y, y) || !near(r[1].z, z))
                    throw std::runtime_error("Triangle/source inverse-transpose oracle failed case="
                        + std::to_string(i));
            }
            if (i < 4 && near3(r[1], r[3]))
                throw std::runtime_error("Nonflat normal map had no effect case=" + std::to_string(i));
            if (i == 4 || i == 5 || i == 6 || i == 7 || i == 9)
                if (!near3(r[0], r[1]))
                    throw std::runtime_error("Invalid or untagged normal did not fall back to geometry");
            if (i == 8 && !(r[1].x > 0.99999f && r[1].z > 1e-5f && near3(r[1], r[2])))
                throw std::runtime_error("Near-cutoff source normal changed during cache decode");
            if (!near(r[5].y, 123) || !near(r[5].z, 456))
                throw std::runtime_error("Emissive-overlay ID channels changed");
            // Fixed source-byte cases: spring #EB3230 at 0/38/76/255 coverage,
            // entity hurt red with inverse alpha 178/255, and white-flash rows.
            constexpr std::array<std::array<float, 3>, cases> expectedTint{{
                {0.800000000f, 0.600000000f, 0.400000000f},
                {0.818116109f, 0.539807766f, 0.368442907f},
                {0.836232218f, 0.479615532f, 0.336885813f},
                {0.860392157f, 0.418823529f, 0.279215686f},
                {0.581176471f, 0.335058824f, 0.279215686f},
                {0.400000000f, 0.480000000f, 0.400000000f},
                {0.950588235f, 0.901176471f, 0.851764706f},
                {0.800000000f, 0.600000000f, 0.400000000f},
                {0.921568627f, 0.196078431f, 0.188235294f},
                {0.880000000f, 0.760000000f, 0.640000000f}
            }};
            if (!near(r[7].x, expectedTint[i][0]) ||
                !near(r[7].y, expectedTint[i][1]) ||
                !near(r[7].z, expectedTint[i][2]) ||
                !near(r[7].w, i == 0 ? 0.0f : 1.0f))
                throw std::runtime_error("Source stress/hurt/white tint or cutout alpha changed case="
                    + std::to_string(i));
            if (i == 6) {
                if (!near(r[5].x, 167) || !near(r[5].w, -17) || !near(r[6].x, -19))
                    throw std::runtime_error("Ordinary cache auxiliary bytes changed");
            } else if (!near(r[5].x, r[1].x) || !near(r[5].w, r[1].y) ||
                       !near(r[6].x, r[1].z)) {
                throw std::runtime_error("Tagged normal was not stored in three auxiliary channels");
            }
        }

        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyShaderModule(device, shader, nullptr);
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
        vkUnmapMemory(device, memory);
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        std::cout << "10 GPU spring helper/cache cases: triangle-cross geom oracle, mirrored/nonuniform source normal, near-cutoff cache roundtrip, fixture nonflat map, cutout 0.099/0.100/0.101, source stress/hurt/white tint, ordinary cache bytes; submit/fence/readback PASS\n";
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
