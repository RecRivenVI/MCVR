#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int SKIP_RETURN_CODE = 77;
constexpr uint32_t WIDTH = 16;
constexpr uint32_t HEIGHT = 16;

struct SableVertex {
    int8_t position[3];
    int8_t pad0;
    int8_t normal[3];
    int8_t pad1;
};
static_assert(sizeof(SableVertex) == 8);

struct SableData {
    uint32_t x;
    uint32_t y;
};
static_assert(sizeof(SableData) == 8);

void checked(VkResult result, const char *operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

bool nearByte(uint8_t actual, uint8_t expected) {
    return actual + 1 >= expected && actual <= expected + 1;
}

std::vector<uint32_t> readSpirv(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot open SPIR-V: " + path.string());
    auto length = stream.tellg();
    if (length <= 0 || length % 4 != 0) throw std::runtime_error("Invalid SPIR-V size");
    std::vector<uint32_t> words(static_cast<size_t>(length) / 4);
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(words.data()), static_cast<std::streamsize>(length));
    return words;
}

uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("No compatible Vulkan memory type");
}

VkShaderModule shader(VkDevice device, const std::filesystem::path &path) {
    auto words = readSpirv(path);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.size() * sizeof(uint32_t);
    info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    checked(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "[FAIL] expected vert and frag SPIR-V paths\n";
        return 1;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkBuffer vertexBuffer = VK_NULL_HANDLE, instanceBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE, indirectBuffer = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE, instanceMemory = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE, indirectMemory = VK_NULL_HANDLE, readbackMemory = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::array<VkShaderModule, 2> modules{};
    void *readbackMapped = nullptr;
    try {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "mcvr-custom-vertex-array-gpu-test";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        checked(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");

        uint32_t physicalCount = 0;
        checked(vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> physicals(physicalCount);
        checked(vkEnumeratePhysicalDevices(instance, &physicalCount, physicals.data()), "enumerate devices");
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        uint32_t queueFamily = UINT32_MAX;
        for (VkPhysicalDevice candidate : physicals) {
            VkPhysicalDeviceFeatures supported{};
            vkGetPhysicalDeviceFeatures(candidate, &supported);
            if (!supported.drawIndirectFirstInstance || !supported.multiDrawIndirect) continue;
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, queues.data());
            for (uint32_t i = 0; i < count; ++i) {
                if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    physical = candidate;
                    queueFamily = i;
                    break;
                }
            }
            if (physical != VK_NULL_HANDLE) break;
        }
        if (physical == VK_NULL_HANDLE) {
            std::cout << "[SKIP] no headless graphics device\n";
            vkDestroyInstance(instance, nullptr);
            return SKIP_RETURN_CODE;
        }

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        VkPhysicalDeviceFeatures features{};
        features.drawIndirectFirstInstance = VK_TRUE;
        features.multiDrawIndirect = VK_TRUE;
        deviceInfo.pEnabledFeatures = &features;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        checked(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        modules = {shader(device, argv[1]), shader(device, argv[2])};

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {WIDTH, HEIGHT, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        checked(vkCreateImage(device, &imageInfo, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements imageRequirements{};
        vkGetImageMemoryRequirements(device, image, &imageRequirements);
        VkMemoryAllocateInfo imageAllocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        imageAllocation.allocationSize = imageRequirements.size;
        imageAllocation.memoryTypeIndex = memoryType(physical, imageRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(vkAllocateMemory(device, &imageAllocation, nullptr, &imageMemory), "allocate image");
        checked(vkBindImageMemory(device, image, imageMemory, 0), "bind image");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageInfo.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        checked(vkCreateImageView(device, &viewInfo, nullptr, &view), "create image view");

        auto makeBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer &buffer,
                              VkDeviceMemory &memory, void **mapped) {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = size;
            info.usage = usage;
            checked(vkCreateBuffer(device, &info, nullptr, &buffer), "create buffer");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, buffer, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memoryType(physical, requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            checked(vkAllocateMemory(device, &allocation, nullptr, &memory), "allocate buffer");
            checked(vkBindBufferMemory(device, buffer, memory, 0), "bind buffer");
            if (mapped) checked(vkMapMemory(device, memory, 0, size, 0, mapped), "map buffer");
        };
        const std::array<SableVertex, 5> vertices{{
            {{99, 99, 0}, 0, {0, 0, 1}, 0},
            {{-1, -1, 0}, 0, {0, 0, 1}, 0},
            {{1, -1, 0}, 0, {0, 0, 1}, 0},
            {{1, 1, 0}, 0, {0, 0, 1}, 0},
            {{-1, 1, 0}, 0, {0, 0, 1}, 0},
        }};
        const std::array<SableData, 2> instances{{
            {0u, 0u},
            {0x12345678u, 0x89ABCDEFu},
        }};
        const std::array<uint16_t, 7> indices{999, 0, 1, 2, 2, 3, 0};
        const std::array<VkDrawIndexedIndirectCommand, 2> indirect{{
            {3, 1, 1, 1, 1}, {3, 1, 4, 1, 1}
        }};
        void *mapped = nullptr;
        makeBuffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBuffer, vertexMemory, &mapped);
        std::memcpy(mapped, vertices.data(), sizeof(vertices));
        vkUnmapMemory(device, vertexMemory);
        makeBuffer(sizeof(instances), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, instanceBuffer, instanceMemory, &mapped);
        std::memcpy(mapped, instances.data(), sizeof(instances));
        vkUnmapMemory(device, instanceMemory);
        makeBuffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer, indexMemory, &mapped);
        std::memcpy(mapped, indices.data(), sizeof(indices));
        vkUnmapMemory(device, indexMemory);
        makeBuffer(sizeof(indirect), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, indirectBuffer, indirectMemory, &mapped);
        std::memcpy(mapped, &indirect, sizeof(indirect));
        vkUnmapMemory(device, indirectMemory);
        makeBuffer(WIDTH * HEIGHT * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback, readbackMemory, &readbackMapped);

        VkAttachmentDescription attachment{};
        attachment.format = imageInfo.format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &attachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        checked(vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass), "create render pass");
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &view;
        framebufferInfo.width = WIDTH;
        framebufferInfo.height = HEIGHT;
        framebufferInfo.layers = 1;
        checked(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer), "create framebuffer");
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        checked(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout), "create pipeline layout");

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        const std::array<VkShaderStageFlagBits, 2> stageBits{
            VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        for (size_t i = 0; i < stages.size(); ++i) {
            stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[i].stage = stageBits[i];
            stages[i].module = modules[i];
            stages[i].pName = "main";
        }
        const std::array<VkVertexInputBindingDescription, 2> bindings{{
            {0, sizeof(SableVertex), VK_VERTEX_INPUT_RATE_VERTEX},
            {1, sizeof(SableData), VK_VERTEX_INPUT_RATE_INSTANCE},
        }};
        const std::array<VkVertexInputAttributeDescription, 3> attributes{{
            {0, 0, VK_FORMAT_R8G8B8_SSCALED, 0},
            {1, 0, VK_FORMAT_R8G8B8_SSCALED, 4},
            {2, 1, VK_FORMAT_R32G32_UINT, 0},
        }};
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0, 0, float(WIDTH), float(HEIGHT), 0, 1};
        VkRect2D scissor{{0, 0}, {WIDTH, HEIGHT}};
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1; viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1; viewportState.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo blendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blendState.attachmentCount = 1; blendState.pAttachments = &blend;
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = static_cast<uint32_t>(stages.size()); pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput; pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster; pipelineInfo.pMultisampleState = &samples;
        pipelineInfo.pColorBlendState = &blendState; pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        checked(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
            "create Sable custom VertexArray pipeline");

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = queueFamily;
        checked(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool), "create command pool");
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = commandPool; commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        checked(vkAllocateCommandBuffers(device, &commandInfo, &command), "allocate command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        checked(vkBeginCommandBuffer(command, &begin), "begin command buffer");
        VkClearValue clear{};
        VkRenderPassBeginInfo renderBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        renderBegin.renderPass = renderPass; renderBegin.framebuffer = framebuffer;
        renderBegin.renderArea = {{0, 0}, {WIDTH, HEIGHT}}; renderBegin.clearValueCount = 1;
        renderBegin.pClearValues = &clear;
        vkCmdBeginRenderPass(command, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const std::array<VkBuffer, 2> vertexBuffers{vertexBuffer, instanceBuffer};
        const std::array<VkDeviceSize, 2> offsets{0, 0};
        vkCmdBindVertexBuffers(command, 0, static_cast<uint32_t>(vertexBuffers.size()),
            vertexBuffers.data(), offsets.data());
        vkCmdBindIndexBuffer(command, indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexedIndirect(command, indirectBuffer, 0, 2,
            sizeof(VkDrawIndexedIndirectCommand));
        vkCmdEndRenderPass(command);
        VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        imageBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.image = image;
        imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {WIDTH, HEIGHT, 1};
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = readback; host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, nullptr, 1, &host, 0, nullptr);
        checked(vkEndCommandBuffer(command), "end command buffer");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        checked(vkCreateFence(device, &fenceInfo, nullptr, &fence), "create fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        checked(vkQueueSubmit(queue, 1, &submit, fence), "submit custom VertexArray draw");
        checked(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX),
            "wait custom VertexArray draw");

        const auto *pixels = static_cast<const uint8_t *>(readbackMapped);
        const std::array<uint8_t, 4> inside{51, 204, 102, 255};
        const std::array<uint8_t, 4> outside{0, 0, 0, 0};
        size_t colored = 0;
        for (uint32_t y = 0; y < HEIGHT; ++y) {
            for (uint32_t x = 0; x < WIDTH; ++x) {
                const bool covered = x >= 2 && x <= 13 && y >= 2 && y <= 13;
                const auto &expected = covered ? inside : outside;
                const size_t base = (y * WIDTH + x) * 4;
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    if (!nearByte(pixels[base + channel], expected[channel])) {
                        throw std::runtime_error("Custom VertexArray coverage/color mismatch at "
                            + std::to_string(x) + "," + std::to_string(y));
                    }
                }
                if (covered) colored++;
            }
        }
        if (colored != 144) throw std::runtime_error("Expected exactly 144 covered custom-layout pixels");
        std::cout << "[PASS] Sable 2-binding layout and indexed-indirect fields drew exact 12x12 coverage\n";
    } catch (const std::exception &error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
    if (device) vkDeviceWaitIdle(device);
    if (readbackMapped) vkUnmapMemory(device, readbackMemory);
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
    if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
    for (VkShaderModule module : modules) if (module) vkDestroyShaderModule(device, module, nullptr);
    if (readback) vkDestroyBuffer(device, readback, nullptr);
    if (readbackMemory) vkFreeMemory(device, readbackMemory, nullptr);
    if (indexBuffer) vkDestroyBuffer(device, indexBuffer, nullptr);
    if (indexMemory) vkFreeMemory(device, indexMemory, nullptr);
    if (indirectBuffer) vkDestroyBuffer(device, indirectBuffer, nullptr);
    if (indirectMemory) vkFreeMemory(device, indirectMemory, nullptr);
    if (instanceBuffer) vkDestroyBuffer(device, instanceBuffer, nullptr);
    if (instanceMemory) vkFreeMemory(device, instanceMemory, nullptr);
    if (vertexBuffer) vkDestroyBuffer(device, vertexBuffer, nullptr);
    if (vertexMemory) vkFreeMemory(device, vertexMemory, nullptr);
    if (view) vkDestroyImageView(device, view, nullptr);
    if (image) vkDestroyImage(device, image, nullptr);
    if (imageMemory) vkFreeMemory(device, imageMemory, nullptr);
    if (device) vkDestroyDevice(device, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    return 0;
}
