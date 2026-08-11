#include "core/vulkan/vertex.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(_WIN32) && defined(SPRING_RASTER_RENDERDOC_CAPTURE)
#include <windows.h>
#include <renderdoc_app.h>
#endif

// Explicit, ignored-run fixture: exact saved production spring bytes and generated
// GLSL, but a small independent Vulkan submission rather than UIModule/ShaderProxy.
namespace {
constexpr uint32_t W = 256, H = 192, TEXTURE_SIZE = 16, TEXTURE_SLOTS = 130;
void check(VkResult value, const char *operation) {
    if (value != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " VkResult=" + std::to_string(value));
}
void require(bool value, const char *reason) {
    if (!value) throw std::runtime_error(reason);
}
std::vector<uint8_t> read(const std::filesystem::path &path, size_t expected = 0) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot read " + path.string());
    auto size = stream.tellg();
    if (size < 0 || (expected && static_cast<size_t>(size) != expected))
        throw std::runtime_error("Unexpected byte length: " + path.string());
    std::vector<uint8_t> data(static_cast<size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(data.data()), size);
    if (!stream) throw std::runtime_error("Incomplete read: " + path.string());
    return data;
}
void write(const std::filesystem::path &path, const std::vector<uint8_t> &data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("Cannot write " + path.string());
    stream.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!stream) throw std::runtime_error("Incomplete write: " + path.string());
}
uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("No Vulkan memory type for spring fixture");
}
struct Buffer { VkBuffer handle = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkDeviceSize size = 0; };
struct Image { VkImage handle = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE; VkFormat format = VK_FORMAT_UNDEFINED; VkImageAspectFlags aspect = 0; };
Buffer buffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
              VkBufferUsageFlags usage, const void *contents = nullptr) {
    Buffer result{}; result.size = size;
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = size; create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device, &create, nullptr, &result.handle), "vkCreateBuffer");
    VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device, result.handle, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memoryType(physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device, &allocate, nullptr, &result.memory), "vkAllocateMemory(buffer)");
    check(vkBindBufferMemory(device, result.handle, result.memory, 0), "vkBindBufferMemory");
    if (contents) {
        void *mapped = nullptr; check(vkMapMemory(device, result.memory, 0, size, 0, &mapped), "vkMapMemory");
        std::memcpy(mapped, contents, static_cast<size_t>(size)); vkUnmapMemory(device, result.memory);
    }
    return result;
}
Image image(VkPhysicalDevice physical, VkDevice device, uint32_t width, uint32_t height,
            VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Image result{}; result.format = format; result.aspect = aspect;
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.imageType = VK_IMAGE_TYPE_2D; create.format = format;
    create.extent = {width, height, 1}; create.mipLevels = 1; create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT; create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(device, &create, nullptr, &result.handle), "vkCreateImage");
    VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device, result.handle, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memoryType(physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &allocate, nullptr, &result.memory), "vkAllocateMemory(image)");
    check(vkBindImageMemory(device, result.handle, result.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = result.handle; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    check(vkCreateImageView(device, &view, nullptr, &result.view), "vkCreateImageView");
    return result;
}
VkShaderModule shader(VkDevice device, const std::filesystem::path &path) {
    auto bytes = read(path);
    require(!bytes.empty() && bytes.size() % 4 == 0, "SPIR-V is not word aligned");
    VkShaderModuleCreateInfo create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    create.codeSize = bytes.size(); create.pCode = reinterpret_cast<const uint32_t *>(bytes.data());
    VkShaderModule result = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &create, nullptr, &result), "vkCreateShaderModule");
    return result;
}
VkSampler sampler(VkDevice device, VkFilter filter) {
    VkSamplerCreateInfo create{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    create.magFilter = filter; create.minFilter = filter;
    create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    create.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    create.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    create.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    create.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler result = VK_NULL_HANDLE;
    check(vkCreateSampler(device, &create, nullptr, &result), "vkCreateSampler");
    return result;
}
std::vector<uint8_t> bottomUp(std::vector<uint8_t> topDown) {
    const size_t row = W * 4;
    std::vector<uint8_t> output(topDown.size());
    for (size_t y = 0; y < H; ++y)
        std::memcpy(output.data() + y * row, topDown.data() + (H - 1 - y) * row, row);
    return output;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 7) {
        std::cerr << "usage: spring_raster_gpu_test <saved-menu-dir> <spring.vert.spv>"
            " <spring.frag.spv> <positive.vert.spv> <positive.frag.spv> <output-dir>\n";
        return 1;
    }
    try {
        check(volkInitialize(), "volkInitialize");
        require(vkCreateInstance != nullptr, "Volk did not load vkCreateInstance");
        const std::filesystem::path fixture = argv[1], output = argv[6];
        std::filesystem::create_directories(output);
        std::array<std::vector<uint8_t>, 3> vertices, indices, uniforms;
        for (int i = 0; i < 3; ++i) {
            vertices[i] = read(fixture / ("vertices-" + std::to_string(i) + ".bin"), 1024);
            indices[i] = read(fixture / ("generated-indices-" + std::to_string(i) + ".bin"), 96);
            uniforms[i] = read(fixture / ("uniform-" + std::to_string(i) + ".bin"), 352);
        }
        auto body = read(fixture / "body-gpu.rgba", 1024);
        auto light = read(fixture / "lightmap-gpu.rgba", 1024);

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "bounded spring raster translation fixture";
        app.apiVersion = VK_API_VERSION_1_4;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instanceInfo.pApplicationInfo = &app;
        VkInstance instance = VK_NULL_HANDLE;
        check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        volkLoadInstance(instance);
        require(vkEnumeratePhysicalDevices != nullptr, "Volk did not load instance functions");
        uint32_t count = 0; check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPU count");
        require(count > 0, "No Vulkan physical device");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate GPUs");
        VkPhysicalDevice physical = VK_NULL_HANDLE; uint32_t family = UINT32_MAX;
        for (auto candidate : devices) {
            VkPhysicalDeviceVulkan12Features indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &indexing;
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            if (!indexing.runtimeDescriptorArray || !indexing.shaderSampledImageArrayNonUniformIndexing) continue;
            uint32_t families = 0; vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> properties(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, properties.data());
            for (uint32_t i = 0; i < families; ++i)
                if (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { physical = candidate; family = i; break; }
            if (physical) break;
        }
        require(physical && family != UINT32_MAX, "No graphics GPU with nonuniform runtime sampler arrays");
        VkPhysicalDeviceProperties gpu{}; vkGetPhysicalDeviceProperties(physical, &gpu);
        std::cout << "GPU=" << gpu.deviceName << " UBO_ALIGNMENT="
                  << gpu.limits.minUniformBufferOffsetAlignment << '\n';
        const uint32_t recordedOffsets[3]{512, 896, 1280};
        for (uint32_t offset : recordedOffsets)
            require(offset % std::max<VkDeviceSize>(1, gpu.limits.minUniformBufferOffsetAlignment) == 0,
                "Captured production dynamic UBO offset violates this GPU alignment");
        float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan12Features indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        indexing.runtimeDescriptorArray = VK_TRUE;
        indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &indexing; deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        VkDevice device = VK_NULL_HANDLE;
        check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        volkLoadDevice(device);
        require(vkCreateGraphicsPipelines != nullptr, "Volk did not load device functions");
        VkQueue queue = VK_NULL_HANDLE; vkGetDeviceQueue(device, family, 0, &queue);

        Image bodyImage = image(physical, device, 16, 16, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        Image lightImage = image(physical, device, 16, 16, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        Image color = image(physical, device, W, H, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        Image depth = image(physical, device, W, H, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkSampler bodySampler = sampler(device, VK_FILTER_NEAREST);
        VkSampler lightSampler = sampler(device, VK_FILTER_LINEAR);
        std::array<uint8_t, 2048> textureBytes{};
        std::memcpy(textureBytes.data(), body.data(), 1024);
        std::memcpy(textureBytes.data() + 1024, light.data(), 1024);
        Buffer upload = buffer(physical, device, textureBytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            textureBytes.data());
        std::array<Buffer, 3> vb{}, ib{};
        for (int i = 0; i < 3; ++i) {
            vb[i] = buffer(physical, device, vertices[i].size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices[i].data());
            ib[i] = buffer(physical, device, indices[i].size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices[i].data());
        }
        std::array<uint8_t, 8192> uniformBytes{};
        for (int i = 0; i < 3; ++i)
            std::memcpy(uniformBytes.data() + recordedOffsets[i], uniforms[i].data(), 352);
        Buffer ubo = buffer(physical, device, uniformBytes.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            uniformBytes.data());
        std::array<uint8_t, 160> postBytes{};
        Buffer postUbo = buffer(physical, device, postBytes.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            postBytes.data());
        Buffer colorRead = buffer(physical, device, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Buffer depthRead = buffer(physical, device, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        VkDescriptorSetLayoutBinding texturesBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            TEXTURE_SLOTS, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo texturesLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        texturesLayoutInfo.bindingCount = 1; texturesLayoutInfo.pBindings = &texturesBinding;
        VkDescriptorSetLayout textureLayout = VK_NULL_HANDLE, uniformLayout = VK_NULL_HANDLE;
        check(vkCreateDescriptorSetLayout(device, &texturesLayoutInfo, nullptr, &textureLayout),
            "texture descriptor layout");
        std::array<VkDescriptorSetLayoutBinding, 2> uniformBindings{{
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}
        }};
        VkDescriptorSetLayoutCreateInfo uniformLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        uniformLayoutInfo.bindingCount = 2; uniformLayoutInfo.pBindings = uniformBindings.data();
        check(vkCreateDescriptorSetLayout(device, &uniformLayoutInfo, nullptr, &uniformLayout),
            "uniform descriptor layout");
        std::array<VkDescriptorPoolSize, 2> poolSizes{{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TEXTURE_SLOTS},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2}
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 2; poolInfo.poolSizeCount = 2; poolInfo.pPoolSizes = poolSizes.data();
        VkDescriptorPool pool = VK_NULL_HANDLE;
        check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "descriptor pool");
        VkDescriptorSetLayout layouts[]{textureLayout, uniformLayout}; VkDescriptorSet sets[2]{};
        VkDescriptorSetAllocateInfo setsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setsInfo.descriptorPool = pool; setsInfo.descriptorSetCount = 2; setsInfo.pSetLayouts = layouts;
        check(vkAllocateDescriptorSets(device, &setsInfo, sets), "descriptor sets");
        std::array<VkDescriptorImageInfo, TEXTURE_SLOTS> images{};
        for (auto &entry : images) entry = {lightSampler, lightImage.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[125] = {bodySampler, bodyImage.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo uboInfo{ubo.handle, 0, 4096};
        VkDescriptorBufferInfo postInfo{postUbo.handle, 0, 160};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (auto &w : writes) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = sets[0]; writes[0].dstBinding = 0;
        writes[0].descriptorCount = TEXTURE_SLOTS;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = images.data();
        writes[1].dstSet = sets[1]; writes[1].dstBinding = 0;
        writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[1].pBufferInfo = &uboInfo;
        writes[2].dstSet = sets[1]; writes[2].dstBinding = 1;
        writes[2].descriptorCount = 1; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[2].pBufferInfo = &postInfo;
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        std::array<VkAttachmentDescription, 2> attachments{};
        for (int i = 0; i < 2; ++i) {
            attachments[i].format = i == 0 ? color.format : depth.format;
            attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[i].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
        VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassInfo.attachmentCount = 2; renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1; renderPassInfo.pSubpasses = &subpass;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        check(vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass), "render pass");
        VkImageView views[]{color.view, depth.view};
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = renderPass; framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = views; framebufferInfo.width = W; framebufferInfo.height = H;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        check(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer), "framebuffer");
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 2; pipelineLayoutInfo.pSetLayouts = layouts;
        pipelineLayoutInfo.pushConstantRangeCount = 1; pipelineLayoutInfo.pPushConstantRanges = &push;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "pipeline layout");
        VkShaderModule vert = shader(device, argv[2]), frag = shader(device, argv[3]);
        VkShaderModule positiveVert = shader(device, argv[4]), positiveFrag = shader(device, argv[5]);
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert; stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";
        auto vertexLayout = vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionColorTexLightNormal>();
        require(vertexLayout.bindingDescriptions.size() == 1
            && vertexLayout.bindingDescriptions[0].stride == 32
            && vertexLayout.attributeDescriptions.size() == 5,
            "Production BLOCK32 Vulkan vertex layout changed");
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexLayout.bindingDescriptions.size());
        vertexInput.pVertexBindingDescriptions = vertexLayout.bindingDescriptions.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexLayout.attributeDescriptions.size());
        vertexInput.pVertexAttributeDescriptions = vertexLayout.attributeDescriptions.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
        VkRect2D scissor{{0, 0}, {W, H}};
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1; viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1; viewportState.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_BACK_BIT;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthState.depthTestEnable = VK_TRUE; depthState.depthWriteEnable = VK_TRUE;
        depthState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_FALSE; blend.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blendState.attachmentCount = 1; blendState.pAttachments = &blend;
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = 2; pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput; pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewportState; pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample; pipelineInfo.pDepthStencilState = &depthState;
        pipelineInfo.pColorBlendState = &blendState; pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        VkPipeline pipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
            "spring graphics pipeline");
        std::array<VkPipelineShaderStageCreateInfo, 2> positiveStages = stages;
        positiveStages[0].module = positiveVert; positiveStages[1].module = positiveFrag;
        VkPipelineVertexInputStateCreateInfo positiveInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineRasterizationStateCreateInfo positiveRaster = raster;
        positiveRaster.cullMode = VK_CULL_MODE_NONE;
        VkGraphicsPipelineCreateInfo positivePipelineInfo = pipelineInfo;
        positivePipelineInfo.pStages = positiveStages.data();
        positivePipelineInfo.pVertexInputState = &positiveInput;
        positivePipelineInfo.pRasterizationState = &positiveRaster;
        VkPipeline positivePipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &positivePipelineInfo,
            nullptr, &positivePipeline), "positive-control graphics pipeline");

#if defined(_WIN32) && defined(SPRING_RASTER_RENDERDOC_CAPTURE)
        RENDERDOC_API_1_6_0 *renderDoc = nullptr;
        if (std::getenv("RADIANCE_SPRING_FIXTURE_RENDERDOC") != nullptr) {
            HMODULE module = GetModuleHandleW(L"renderdoc.dll");
            require(module != nullptr, "RenderDoc capture layer DLL not loaded");
            auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
            require(getApi && getApi(eRENDERDOC_API_Version_1_6_0,
                reinterpret_cast<void **>(&renderDoc)) == 1 && renderDoc,
                "RenderDoc 1.6 application API unavailable");
            auto captureTemplate = (output / "spring-raster").string();
            renderDoc->SetCaptureFilePathTemplate(captureTemplate.c_str());
            renderDoc->StartFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance), nullptr);
            require(renderDoc->IsFrameCapturing() != 0, "RenderDoc refused headless Vulkan capture");
            std::cout << "RENDERDOC_CAPTURE_STARTED " << captureTemplate << std::endl;
        }
#else
        require(std::getenv("RADIANCE_SPRING_FIXTURE_RENDERDOC") == nullptr,
            "RenderDoc was requested but fixture was built without explicit installed SDK");
#endif

        VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commandPoolInfo.queueFamilyIndex = family;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        check(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool), "command pool");
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = commandPool; commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        check(vkAllocateCommandBuffers(device, &commandInfo, &command), "command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        std::array<VkImageMemoryBarrier, 2> toUpload{};
        Image *textureTargets[]{&bodyImage, &lightImage};
        for (int i = 0; i < 2; ++i) {
            auto &b = toUpload[i]; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = textureTargets[i]->handle; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, toUpload.data());
        for (int i = 0; i < 2; ++i) {
            VkBufferImageCopy copy{}; copy.bufferOffset = i * 1024;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {16, 16, 1};
            vkCmdCopyBufferToImage(command, upload.handle, textureTargets[i]->handle,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            toUpload[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toUpload[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toUpload[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toUpload[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 2, toUpload.data());
        std::array<VkClearValue, 2> clear{}; clear[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo renderBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        renderBegin.renderPass = renderPass; renderBegin.framebuffer = framebuffer;
        renderBegin.renderArea = {{0, 0}, {W, H}};
        renderBegin.clearValueCount = 2; renderBegin.pClearValues = clear.data();
        vkCmdBeginRenderPass(command, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, positivePipeline);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const uint32_t opaqueCoverage = 0;
        vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &opaqueCoverage);
        for (int i = 0; i < 3; ++i) {
            uint32_t offsets[]{recordedOffsets[i], 0};
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                0, 2, sets, 2, offsets);
            const VkDeviceSize vertexOffset = 0;
            vkCmdBindVertexBuffers(command, 0, 1, &vb[i].handle, &vertexOffset);
            vkCmdBindIndexBuffer(command, ib[i].handle, 0, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(command, 48, 1, 0, 0, 0);
        }
        vkCmdEndRenderPass(command);
        std::array<VkImageMemoryBarrier, 2> readBarriers{};
        for (int i = 0; i < 2; ++i) {
            auto &b = readBarriers[i]; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = i == 0 ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = i == 0 ? color.handle : depth.handle;
            b.subresourceRange = {static_cast<VkImageAspectFlags>(
                i == 0 ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT), 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, readBarriers.data());
        VkBufferImageCopy colorCopy{}; colorCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        colorCopy.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(command, color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            colorRead.handle, 1, &colorCopy);
        VkBufferImageCopy depthCopy{}; depthCopy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        depthCopy.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(command, depth.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            depthRead.handle, 1, &depthCopy);
        std::array<VkBufferMemoryBarrier, 2> hostBarriers{};
        for (int i = 0; i < 2; ++i) {
            auto &b = hostBarriers[i]; b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.buffer = i == 0 ? colorRead.handle : depthRead.handle; b.size = W * H * 4;
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, nullptr, 2, hostBarriers.data(), 0, nullptr);
        check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
#if defined(_WIN32) && defined(SPRING_RASTER_RENDERDOC_CAPTURE)
        if (renderDoc) {
            require(renderDoc->EndFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance),
                nullptr) != 0, "RenderDoc EndFrameCapture failed");
            const uint32_t count = renderDoc->GetNumCaptures();
            require(count > 0, "RenderDoc did not save a capture");
            uint32_t pathLength = 0;
            require(renderDoc->GetCapture(count - 1, nullptr, &pathLength, nullptr) != 0
                && pathLength > 0 && pathLength < 32768, "RenderDoc capture path unavailable");
            std::vector<char> path(pathLength + 1, '\0');
            require(renderDoc->GetCapture(count - 1, path.data(), &pathLength, nullptr) != 0,
                "RenderDoc capture path retrieval failed");
            std::ofstream capturePath(output / "renderdoc-capture.txt");
            capturePath << path.data() << '\n';
            std::cout << "RENDERDOC_CAPTURE_ENDED " << path.data() << std::endl;
        }
#endif
        std::vector<uint8_t> colorBytes(W * H * 4), depthBytes(W * H * 4);
        void *mapped = nullptr;
        check(vkMapMemory(device, colorRead.memory, 0, colorBytes.size(), 0, &mapped), "map color");
        std::memcpy(colorBytes.data(), mapped, colorBytes.size()); vkUnmapMemory(device, colorRead.memory);
        check(vkMapMemory(device, depthRead.memory, 0, depthBytes.size(), 0, &mapped), "map depth");
        std::memcpy(depthBytes.data(), mapped, depthBytes.size()); vkUnmapMemory(device, depthRead.memory);
        colorBytes = bottomUp(std::move(colorBytes)); depthBytes = bottomUp(std::move(depthBytes));
        write(output / "color.rgba", colorBytes); write(output / "depth.f32", depthBytes);
        std::array<int, 3> coverage{};
        int positivePixels = 0;
        for (int y = 0; y < static_cast<int>(H); ++y)
            for (int x = 0; x < 32; ++x) {
                const size_t pixel = (y * W + x) * 4;
                positivePixels += colorBytes[pixel + 1] > 200 && colorBytes[pixel + 3] > 200;
            }
        for (int i = 0; i < 3; ++i) {
            int center = (4 + i * 4) * 16 + 8;
            for (int y = 34; y < 126; ++y) for (int x = center - 6; x < center + 6; ++x)
                coverage[i] += colorBytes[(y * W + x) * 4 + 3] > 0;
        }
        std::cout << "FENCE_READBACK_OK positivePixels=" << positivePixels << " springROI="
                  << coverage[0] << ',' << coverage[1] << ',' << coverage[2] << std::endl;
        require(positivePixels > 0, "Positive-control triangle did not rasterize");
        require(std::all_of(coverage.begin(), coverage.end(), [](int x) { return x > 0; }),
            "Exact spring raster shader fixture has no spring pixel");
        check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipeline(device, positivePipeline, nullptr);
        vkDestroyShaderModule(device, vert, nullptr); vkDestroyShaderModule(device, frag, nullptr);
        vkDestroyShaderModule(device, positiveVert, nullptr);
        vkDestroyShaderModule(device, positiveFrag, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyFramebuffer(device, framebuffer, nullptr); vkDestroyRenderPass(device, renderPass, nullptr);
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyDescriptorSetLayout(device, textureLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, uniformLayout, nullptr);
        vkDestroySampler(device, bodySampler, nullptr); vkDestroySampler(device, lightSampler, nullptr);
        for (const auto &b : vb) { vkDestroyBuffer(device, b.handle, nullptr); vkFreeMemory(device, b.memory, nullptr); }
        for (const auto &b : ib) { vkDestroyBuffer(device, b.handle, nullptr); vkFreeMemory(device, b.memory, nullptr); }
        for (const auto &b : {upload, ubo, postUbo, colorRead, depthRead}) {
            vkDestroyBuffer(device, b.handle, nullptr); vkFreeMemory(device, b.memory, nullptr);
        }
        for (const auto &im : {bodyImage, lightImage, color, depth}) {
            vkDestroyImageView(device, im.view, nullptr);
            vkDestroyImage(device, im.handle, nullptr); vkFreeMemory(device, im.memory, nullptr);
        }
        vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
        volkFinalize();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "SPRING_RASTER_FIXTURE_FAIL " << error.what() << '\n';
        return 1;
    }
}
