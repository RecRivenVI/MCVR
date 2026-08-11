#include "core/vulkan/dynamic_pipeline.hpp"

#include "core/logging.hpp"

#include "core/vulkan/descriptor.hpp"
#include "core/vulkan/device.hpp"
#include "core/vulkan/render_pass.hpp"
#include "core/vulkan/shader.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

auto dynamicGraphicsPipelineCout() {
    return mcvr::log::info("GraphicsPipeline");
}

auto dynamicGraphicsPipelineCerr() {
    return mcvr::log::error("GraphicsPipeline");
}

vk::DynamicGraphicsPipeline::DynamicGraphicsPipeline(std::shared_ptr<Device> device, VkPipeline pipeline)
    : device_(device), pipeline_(pipeline) {}

vk::DynamicGraphicsPipeline::~DynamicGraphicsPipeline() {
    vkDestroyPipeline(device_->vkDevice(), pipeline_, nullptr);

#ifdef DEBUG
    dynamicGraphicsPipelineCout() << "graphics pipeline deconstructed" << std::endl;
#endif
}

VkPipeline vk::DynamicGraphicsPipeline::vkPipeline() {
    return pipeline_;
}

vk::DynamicGraphicsPipelineBuilder::ShaderStageBuilder::ShaderStageBuilder(DynamicGraphicsPipelineBuilder &parent)
    : parent(parent) {}

vk::DynamicGraphicsPipelineBuilder::ShaderStageBuilder &
vk::DynamicGraphicsPipelineBuilder::ShaderStageBuilder::defineShaderStage(
    VkPipelineShaderStageCreateInfo shaderStageCreateInfo) {
    shaderStageCreateInfos.push_back(shaderStageCreateInfo);
    return *this;
}

vk::DynamicGraphicsPipelineBuilder::ShaderStageBuilder &
vk::DynamicGraphicsPipelineBuilder::ShaderStageBuilder::defineShaderStage(VkShaderModule shaderModule,
                                                                          VkShaderStageFlagBits shaderStage) {
    VkPipelineShaderStageCreateInfo shaderStageCreateInfo = {};
    shaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStageCreateInfo.stage = shaderStage;
    shaderStageCreateInfo.module = shaderModule;
    shaderStageCreateInfo.pName = "main";

    shaderStageCreateInfos.push_back(shaderStageCreateInfo);
    return *this;
}

vk::DynamicGraphicsPipelineBuilder::ShaderStageBuilder &
vk::DynamicGraphicsPipelineBuilder::ShaderStageBuilder::defineShaderStage(std::shared_ptr<Shader> shader,
                                                                          VkShaderStageFlagBits shaderStage) {
    VkPipelineShaderStageCreateInfo shaderStageCreateInfo = {};
    shaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStageCreateInfo.stage = shaderStage;
    shaderStageCreateInfo.module = shader->vkShaderModule();
    shaderStageCreateInfo.pName = "main";

    shaderStageCreateInfos.push_back(shaderStageCreateInfo);
    return *this;
}

vk::DynamicGraphicsPipelineBuilder &vk::DynamicGraphicsPipelineBuilder::ShaderStageBuilder::endShaderStage() {
    return parent;
}

vk::DynamicGraphicsPipelineBuilder::DynamicGraphicsPipelineBuilder(int numColorAttachments)
    : shaderStageBuilder_(*this) {
    colorBlendAttachmentStates_.resize(numColorAttachments);
    for (int i = 0; i < numColorAttachments; i++) {
        colorBlendAttachmentStates_[i] = VkPipelineColorBlendAttachmentState{};
    }
    colorBlendState_.attachmentCount = numColorAttachments;
    colorBlendState_.pAttachments = colorBlendAttachmentStates_.data();
}

vk::DynamicGraphicsPipelineBuilder &
vk::DynamicGraphicsPipelineBuilder::defineRenderPass(std::shared_ptr<RenderPass> renderPass, uint32_t subpassIndex) {
    renderPass_ = renderPass->vkRenderPass();
    subpassIndex_ = subpassIndex;
    return *this;
}

vk::DynamicGraphicsPipelineBuilder::ShaderStageBuilder &vk::DynamicGraphicsPipelineBuilder::beginShaderStage() {
    return shaderStageBuilder_;
}

vk::DynamicGraphicsPipelineBuilder &
vk::DynamicGraphicsPipelineBuilder::definePipelineLayout(std::shared_ptr<DescriptorTable> descriptorTable) {
    pipelineLayout_ = descriptorTable->vkPipelineLayout();
    pipelineLayoutKeepAlive_ = descriptorTable->pipelineLayoutKeepAlive();
    return *this;
}

vk::DynamicGraphicsPipelineBuilder &
vk::DynamicGraphicsPipelineBuilder::defineInputAssemblyState(VkPrimitiveTopology topology) {
    inputAssemblyStateCreateInfo_.topology = topology;
    return *this;
}

vk::DynamicGraphicsPipelineBuilder &vk::DynamicGraphicsPipelineBuilder::definePatchControlPoints(uint32_t count) {
    if (count == 0) throw std::invalid_argument("Tessellation patch cannot be empty");
    tessellationStateCreateInfo_.patchControlPoints = count;
    inputAssemblyStateCreateInfo_.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    return *this;
}

std::shared_ptr<vk::DynamicGraphicsPipeline> vk::DynamicGraphicsPipelineBuilder::build(std::shared_ptr<Device> device) {
    // Filter dynamic states based on device features
    std::vector<VkDynamicState> filteredDynamicStates;
    for (uint32_t i = 0; i < sizeof(dynamicState_) / sizeof(*dynamicState_); i++) {
        VkDynamicState state = dynamicState_[i];
        // Skip LOGIC_OP states if feature is not enabled
        if ((state == VK_DYNAMIC_STATE_LOGIC_OP_EXT || state == VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT) &&
            !device->hasExtendedDynamicState2LogicOp()) {
            continue;
        }
        filteredDynamicStates.push_back(state);
    }

    VkPipelineDynamicStateCreateInfo filteredDynamicStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32_t>(filteredDynamicStates.size()),
        .pDynamicStates = filteredDynamicStates.data(),
    };

    VkGraphicsPipelineCreateInfo pipelineCreateInfo = {};
    pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCreateInfo.stageCount = shaderStageBuilder_.shaderStageCreateInfos.size();
    pipelineCreateInfo.pStages = shaderStageBuilder_.shaderStageCreateInfos.data();
    pipelineCreateInfo.pVertexInputState = &vertexInputStateCreateInfo_;
    pipelineCreateInfo.pInputAssemblyState = &inputAssemblyStateCreateInfo_;
    if (inputAssemblyStateCreateInfo_.topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) {
        if (!device->hasTessellation() || tessellationStateCreateInfo_.patchControlPoints == 0)
            throw std::runtime_error("Tessellation requires an enabled device feature and patch size");
        pipelineCreateInfo.pTessellationState = &tessellationStateCreateInfo_;
    }
    pipelineCreateInfo.pViewportState = &viewportStateCreateInfo_;
    pipelineCreateInfo.pRasterizationState = &rasterizationStateCreateInfo_;
    pipelineCreateInfo.pMultisampleState = &multisampleStateCreateInfo_;
    pipelineCreateInfo.pDepthStencilState = &depthStencilState_;
    pipelineCreateInfo.pColorBlendState = &colorBlendState_;
    pipelineCreateInfo.pDynamicState = &filteredDynamicStateCreateInfo;
    pipelineCreateInfo.layout = pipelineLayout_;
    pipelineCreateInfo.renderPass = renderPass_;
    pipelineCreateInfo.subpass = subpassIndex_;
    pipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineCreateInfo.basePipelineIndex = -1;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = device->createGraphicsPipelines(1, &pipelineCreateInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device->vkDevice(), pipeline, nullptr);
        throw std::runtime_error("Failed to create dynamic graphics pipeline: VkResult=" + std::to_string(result));
    } else {
#ifdef DEBUG
        dynamicGraphicsPipelineCout() << "created dynamic graphics pipeline" << std::endl;
#endif
    }

    auto resultPipeline = std::make_shared<DynamicGraphicsPipeline>(device, pipeline);
    resultPipeline->pipelineLayoutKeepAlive_ = pipelineLayoutKeepAlive_;
    return resultPipeline;
}
