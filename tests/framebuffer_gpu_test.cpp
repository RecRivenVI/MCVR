#include "core/render/ui_coverage.hpp"
#include "core/render/ui_blur.hpp"
#include "core/render/post_color_sync.hpp"
#include <vulkan/vulkan.h>
#include "core/vulkan/image_format.hpp"
#include "core/vulkan/inline_update.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int SKIP_RETURN_CODE = 77;
constexpr uint32_t WIDTH = 8;
constexpr uint32_t HEIGHT = 6;

class SkipError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

void checked(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult=" + std::to_string(result));
    }
}

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

uint8_t unorm(float value) {
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

bool nearByte(uint8_t actual, uint8_t expected) {
    return std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= 1;
}

std::vector<uint32_t> readSpirv(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open()) throw std::runtime_error("Cannot open framebuffer shader: " + path.string());
    const auto length = input.tellg();
    if (length <= 0 || length % static_cast<std::streamoff>(sizeof(uint32_t)) != 0) {
        throw std::runtime_error("Framebuffer shader is not valid word-aligned SPIR-V: " + path.string());
    }
    std::vector<uint32_t> words(static_cast<size_t>(length) / sizeof(uint32_t));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(words.data()), static_cast<std::streamsize>(length));
    if (!input) throw std::runtime_error("Cannot read framebuffer shader: " + path.string());
    return words;
}

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspects = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

class FramebufferHarness {
  public:
    FramebufferHarness(const std::filesystem::path &vertexPath,
                       const std::filesystem::path &fragmentPath,
                       const std::filesystem::path &mrtFragmentPath, bool validateSync = false)
        : validateSync_(validateSync) {
        createInstance();
        selectDevice();
        createDevice();
        vertexShader_ = createShader(vertexPath);
        fragmentShader_ = createShader(fragmentPath);
        mrtFragmentShader_ = createShader(mrtFragmentPath);
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        checked(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");
        createCommands();
    }

    ~FramebufferHarness() {
        if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
        if (device_ != VK_NULL_HANDLE && fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
        if (device_ != VK_NULL_HANDLE && commandPool_ != VK_NULL_HANDLE)
            vkDestroyCommandPool(device_, commandPool_, nullptr);
        for (VkFramebuffer framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        for (VkPipeline pipeline : pipelines_) vkDestroyPipeline(device_, pipeline, nullptr);
        for (VkRenderPass renderPass : renderPasses_) vkDestroyRenderPass(device_, renderPass, nullptr);
        for (VkDescriptorPool pool : descriptorPools_) vkDestroyDescriptorPool(device_, pool, nullptr);
        for (VkSampler sampler : samplers_) vkDestroySampler(device_, sampler, nullptr);
        for (VkPipelineLayout layout : extraPipelineLayouts_) vkDestroyPipelineLayout(device_, layout, nullptr);
        for (VkDescriptorSetLayout layout : descriptorSetLayouts_)
            vkDestroyDescriptorSetLayout(device_, layout, nullptr);
        if (device_ != VK_NULL_HANDLE && pipelineLayout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        for (VkShaderModule shader : shaders_) vkDestroyShaderModule(device_, shader, nullptr);
        for (const auto &image : images_) {
            if (image.view != VK_NULL_HANDLE) vkDestroyImageView(device_, image.view, nullptr);
            if (image.handle != VK_NULL_HANDLE) vkDestroyImage(device_, image.handle, nullptr);
            if (image.memory != VK_NULL_HANDLE) vkFreeMemory(device_, image.memory, nullptr);
        }
        for (const auto &buffer : buffers_) {
            if (buffer.mapped != nullptr) vkUnmapMemory(device_, buffer.memory);
            if (buffer.handle != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffer.handle, nullptr);
            if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(device_, buffer.memory, nullptr);
        }
        if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
        if (messenger_ != VK_NULL_HANDLE) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(instance_, messenger_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    }

    const VkPhysicalDeviceProperties &properties() const { return properties_; }

    void postColorSyncCase() {
        Image color = createImage(4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Buffer output = createBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkRenderPass pass = createColorRenderPass({color.format});
        VkFramebuffer target = createFramebuffer(pass, {color.view}, 4, 4);
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 15;
        VkPipeline pipeline = createGraphicsPipeline(pass, fragmentShader_, {blend}, false, false);
        submit([&](VkCommandBuffer cmd) {
            prepareColor(cmd, color, {{0, 0, 0, 0}});
            draw(cmd, pass, target, pipeline, {4,4}, {{0,0},{4,4}}, {}, false);
            // Same producer scope used by the LDR->post blit and post-pass finalization.
            transition(cmd, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                static_cast<VkPipelineStageFlags>(mcvr::postColorSourceStages(
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT)),
                VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageToBuffer(cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, output);
        });
        verifySolidRgba(output, 4, 4, {255,255,255,255}, "post color handoff");
        require(validationErrors_ == 0, "Post color synchronization validation failed");
        std::cout << "[PASS] raster color write -> production post barrier -> readback; synchronization validation active\n";
    }

#include "fg_composition_gpu_cases.inl"

    void backgroundModulationCase() {
        for (const bool inverse : {false, true})
        for (const float initialAlpha : {0.0f, 0.5f}) {
            Image color = createImage(4,4,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            Buffer output = createBuffer(64,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            VkRenderPass pass = createColorRenderPass({color.format});
            VkFramebuffer target = createFramebuffer(pass,{color.view},4,4);
            VkColorBlendEquationEXT equation{VK_BLEND_FACTOR_ZERO,VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,VK_BLEND_OP_ADD,
                VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_ZERO,VK_BLEND_OP_ADD};
            if (inverse) equation.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            mcvr::ui::coverageEquation(equation,false);
            VkPipelineColorBlendAttachmentState blend{};
            blend.blendEnable=VK_TRUE;
            blend.srcColorBlendFactor=equation.srcColorBlendFactor; blend.dstColorBlendFactor=equation.dstColorBlendFactor;
            blend.colorBlendOp=equation.colorBlendOp; blend.srcAlphaBlendFactor=equation.srcAlphaBlendFactor;
            blend.dstAlphaBlendFactor=equation.dstAlphaBlendFactor; blend.alphaBlendOp=equation.alphaBlendOp;
            blend.colorWriteMask=15;
            VkPipeline pipeline=createGraphicsPipeline(pass,fragmentShader_,{blend},false,false);
            submit([&](VkCommandBuffer cmd) {
                transition(cmd,color,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,
                    VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkClearColorValue value{{.8f,.4f,.2f,initialAlpha}};
                VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                vkCmdClearColorImage(cmd,color.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);
                transition(cmd,color,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                draw(cmd,pass,target,pipeline,{4,4},{{1,1},{2,2}},{},false);
                transition(cmd,color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
                copyImageToBuffer(cmd,color,VK_IMAGE_ASPECT_COLOR_BIT,output);
            });
            const auto *pixels=static_cast<const uint8_t*>(output.mapped);
            for(int i=0;i<16;++i) {
                const bool inside = i%4 >= 1 && i%4 <= 2 && i/4 >= 1 && i/4 <= 2;
                const float background[]{.8f,.4f,.2f};
                for(int c=0;c<3;++c) require(nearByte(pixels[4*i+c], unorm(inside ? (inverse ? 1-background[c] : 0) : background[c])),
                    "Local affine RGB/scissor differs from original blend");
                require(nearByte(pixels[4*i+3],unorm(initialAlpha)),"Vignette made GUI coverage opaque");
            }
        }
        std::cout << "[PASS] local multiplication/inversion preserve fractional coverage and original scissor/RGB on GPU\n";
    }

    void dlssUiCoverageCase(const std::filesystem::path &shaderPath, const std::filesystem::path &resetPath = {}) {
        constexpr uint32_t w=17, h=3; // Exercise partial compute workgroups.
        const auto flags=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Image real=createImage(w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,flags);
        Image scene=createImage(w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,flags);
        Image coverage=createImage(w,h,VK_FORMAT_R32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT,flags);
        Buffer upload=createBuffer(w*h*8,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer readback=createBuffer(w*h*sizeof(float),VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Buffer colorReadback=createBuffer(w*h*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto *bytes=static_cast<uint8_t *>(upload.mapped);
        for (uint32_t i=0;i<w*h;++i) {
            for (uint32_t c=0;c<4;++c) bytes[4*i+c]=bytes[4*w*h+4*i+c]=uint8_t(30+i+c);
            // Fractional coverage must survive even when UI RGB equals the scene.
            bytes[4*i+3] = std::array<uint8_t,5>{0,64,128,192,255}[i%5];
            if(i%4==1) ++bytes[4*i];
            if(i%4==2) bytes[4*i+1]=255;

        }
        std::array<VkDescriptorSetLayoutBinding,3> bindings{};
        for(uint32_t i=0;i<3;++i) bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount=3; setInfo.pBindings=bindings.data();
        VkDescriptorSetLayout setLayout;
        checked(vkCreateDescriptorSetLayout(device_,&setInfo,nullptr,&setLayout),"UI coverage set layout");
        descriptorSetLayouts_.push_back(setLayout);
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount=1; layoutInfo.pSetLayouts=&setLayout;
        VkPipelineLayout layout;
        checked(vkCreatePipelineLayout(device_,&layoutInfo,nullptr,&layout),"UI coverage pipeline layout");
        extraPipelineLayouts_.push_back(layout);
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets=1; poolInfo.poolSizeCount=1; poolInfo.pPoolSizes=&poolSize;
        VkDescriptorPool pool;
        checked(vkCreateDescriptorPool(device_,&poolInfo,nullptr,&pool),"UI coverage descriptor pool");
        descriptorPools_.push_back(pool);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool=pool; allocation.descriptorSetCount=1; allocation.pSetLayouts=&setLayout;
        VkDescriptorSet set;
        checked(vkAllocateDescriptorSets(device_,&allocation,&set),"UI coverage descriptor set");
        std::array<VkDescriptorImageInfo,3> infos{{{VK_NULL_HANDLE,real.view,VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE,scene.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,coverage.view,VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet,3> writes{};
        for(uint32_t i=0;i<3;++i) {
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=set; writes[i].dstBinding=i;
            writes[i].descriptorCount=1; writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[i].pImageInfo=&infos[i];
        }
        vkUpdateDescriptorSets(device_,3,writes.data(),0,nullptr);
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        compute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; compute.stage.module=createShader(shaderPath);
        compute.stage.pName="main"; compute.layout=layout;
        VkPipeline pipeline;
        checked(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&compute,nullptr,&pipeline),"UI coverage compute pipeline");
        pipelines_.push_back(pipeline);
        VkPipeline resetPipeline = VK_NULL_HANDLE;
        if (!resetPath.empty()) {
            compute.stage.module = createShader(resetPath);
            checked(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&compute,nullptr,&resetPipeline),"UI coverage reset pipeline");
            pipelines_.push_back(resetPipeline);
        }
        submit([&](VkCommandBuffer cmd) {
            for(auto pair : {std::pair{&real,VkDeviceSize(0)},std::pair{&scene,VkDeviceSize(w*h*4)}}) {
                transition(cmd,*pair.first,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy copy{}; copy.bufferOffset=pair.second; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={w,h,1};
                vkCmdCopyBufferToImage(cmd,upload.handle,pair.first->handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
                transition(cmd,*pair.first,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            }
            transition(cmd,coverage,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            if (resetPipeline) {
                transition(cmd,real,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resetPipeline);
                vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
                vkCmdDispatch(cmd,(w+15)/16,(h+15)/16,1);
                transition(cmd,real,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            }
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
            vkCmdDispatch(cmd,(w+15)/16,(h+15)/16,1);
            transition(cmd,coverage,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageToBuffer(cmd,coverage,VK_IMAGE_ASPECT_COLOR_BIT,readback);
            transition(cmd,real,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageToBuffer(cmd,real,VK_IMAGE_ASPECT_COLOR_BIT,colorReadback);
        });
        const auto *result=static_cast<const float *>(readback.mapped);
        for(uint32_t i=0;i<w*h;++i)
            require(std::abs(result[i] - (resetPipeline ? 0 : bytes[4*i+3]/255.0f)) < 1e-6f,"FG coverage lost fractional alpha or retained world alpha");
        const auto *rgb = static_cast<const uint8_t *>(colorReadback.mapped);
        for(uint32_t i=0;i<w*h;++i) for(uint32_t c=0;c<3;++c)
            require(rgb[4*i+c] == bytes[4*i+c], "Coverage initialization changed world RGB");
        std::cout << "[PASS] DLSS UI coverage: 0/25/50/75/100 percent alpha, independent RGB, partial workgroups\n";
    }

    void ponderCoverageCase(const std::filesystem::path &shaderPath) {
        constexpr uint32_t sw=4, sh=4, w=8, h=8;
        Image color=createImage(sw,sh,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Image depth=createImage(sw,sh,VK_FORMAT_R32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Image output=createImage(w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        Buffer upload=createBuffer(sw*sh*sizeof(float),VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer readback=createBuffer(w*h*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto depths=static_cast<float *>(upload.mapped);
        for(uint32_t y=0;y<sh;++y) for(uint32_t x=0;x<sw;++x)
            depths[y*sw+x]=x<2 ? 10.f : std::numeric_limits<float>::infinity();
        depths[3]=std::numeric_limits<float>::quiet_NaN();
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter=samplerInfo.minFilter=VK_FILTER_LINEAR;
        samplerInfo.addressModeU=samplerInfo.addressModeV=samplerInfo.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkSampler sampler;checked(vkCreateSampler(device_,&samplerInfo,nullptr,&sampler),"Ponder sampler");samplers_.push_back(sampler);
        std::array<VkDescriptorSetLayoutBinding,3> bindings{{
            {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT},
            {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT},
            {2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT}}};
        VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        si.bindingCount=uint32_t(bindings.size());si.pBindings=bindings.data();
        VkDescriptorSetLayout setLayout;checked(vkCreateDescriptorSetLayout(device_,&si,nullptr,&setLayout),"Ponder set layout");descriptorSetLayouts_.push_back(setLayout);
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,8};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&setLayout;li.pushConstantRangeCount=1;li.pPushConstantRanges=&range;
        VkPipelineLayout layout;checked(vkCreatePipelineLayout(device_,&li,nullptr,&layout),"Ponder pipeline layout");extraPipelineLayouts_.push_back(layout);
        std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=uint32_t(sizes.size());pi.pPoolSizes=sizes.data();
        VkDescriptorPool pool;checked(vkCreateDescriptorPool(device_,&pi,nullptr,&pool),"Ponder pool");descriptorPools_.push_back(pool);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout;
        VkDescriptorSet set;checked(vkAllocateDescriptorSets(device_,&ai,&set),"Ponder coverage set");
        std::array<VkDescriptorImageInfo,3> images{{{sampler,color.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler,depth.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,output.view,VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet,3> writes{};
        for(int i=0;i<3;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=bindings[i].descriptorType;writes[i].pImageInfo=&images[i];}
        vkUpdateDescriptorSets(device_,3,writes.data(),0,nullptr);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=createShader(shaderPath);ci.stage.pName="main";ci.layout=layout;
        VkPipeline pipeline;checked(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline),"Ponder coverage pipeline");pipelines_.push_back(pipeline);
        const std::array<float,2> jitter{.25f,-.25f};
        submit([&](VkCommandBuffer cmd){
            for (const auto &image : {color,depth}) transition(cmd,image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearColorValue red{{1,0,0,1}};VkImageSubresourceRange sub{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(cmd,color.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&red,1,&sub);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={sw,sh,1};
            vkCmdCopyBufferToImage(cmd,upload.handle,depth.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            for (const auto &image : {color,depth}) transition(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            transition(cmd,output,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
            vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,jitter.data());vkCmdDispatch(cmd,1,1,1);
            transition(cmd,output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageToBuffer(cmd,output,VK_IMAGE_ASPECT_COLOR_BIT,readback);
        });
        auto hit=[&](int x,int y) { return x>=0 && x<int(sw) && y>=0 && y<int(sh) && std::isfinite(depths[y*sw+x]) ? 1.f : 0.f; };
        auto actual=static_cast<const uint8_t *>(readback.mapped);
        bool fractional=false;
        for(int y=0;y<int(h);++y) for(int x=0;x<int(w);++x) {
            float sx=(x+.5f)*sw/w-.5f-jitter[0],sy=(y+.5f)*sh/h-.5f-jitter[1];
            int ix=int(std::floor(sx)),iy=int(std::floor(sy));float fx=sx-ix,fy=sy-iy;
            float expected=std::lerp(std::lerp(hit(ix,iy),hit(ix+1,iy),fx),std::lerp(hit(ix,iy+1),hit(ix+1,iy+1),fx),fy);
            fractional |= expected>0 && expected<1;
            require(nearByte(actual[4*(y*w+x)+3],unorm(expected)),"Ponder coverage leaked, erased a finite sample, or ignored input jitter");
            require(actual[4*(y*w+x)]==255,"Ponder changed the resolved color");
        }
        require(fractional,"Ponder test did not exercise fractional coverage");
        std::cout << "[PASS] Ponder coverage: finite/infinite/NaN, fractional edges, jitter and no wraparound\n";
    }

    void hudlessBlurCase(const std::filesystem::path &shaderPath) {
        constexpr uint32_t w=17, h=3;
        Image source=createImage(w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Image output=createImage(w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        Buffer upload=createBuffer(w*h*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer readback=createBuffer(w*h*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto bytes=static_cast<uint8_t *>(upload.mapped);
        for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) {
            auto i=4*(y*w+x); bytes[i]=uint8_t(x*13); bytes[i+1]=uint8_t(220-x*11); bytes[i+2]=uint8_t(y*60); bytes[i+3]=0;
        }
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter=samplerInfo.minFilter=VK_FILTER_LINEAR;
        samplerInfo.addressModeU=samplerInfo.addressModeV=samplerInfo.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkSampler sampler; checked(vkCreateSampler(device_,&samplerInfo,nullptr,&sampler),"FG blur sampler"); samplers_.push_back(sampler);
        std::array<VkDescriptorSetLayoutBinding,2> bindings{{
            {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT}}};
        VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; si.bindingCount=2;si.pBindings=bindings.data();
        VkDescriptorSetLayout setLayout; checked(vkCreateDescriptorSetLayout(device_,&si,nullptr,&setLayout),"FG blur layout"); descriptorSetLayouts_.push_back(setLayout);
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,16};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&setLayout;li.pushConstantRangeCount=1;li.pPushConstantRanges=&range;
        VkPipelineLayout layout; checked(vkCreatePipelineLayout(device_,&li,nullptr,&layout),"FG blur pipeline layout");extraPipelineLayouts_.push_back(layout);
        std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=2;pi.pPoolSizes=sizes.data();
        VkDescriptorPool pool;checked(vkCreateDescriptorPool(device_,&pi,nullptr,&pool),"FG blur pool");descriptorPools_.push_back(pool);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout;
        VkDescriptorSet set;checked(vkAllocateDescriptorSets(device_,&ai,&set),"FG blur set");
        std::array<VkDescriptorImageInfo,2> images{{{sampler,source.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,output.view,VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet,2> writes{};
        for(int i=0;i<2;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=bindings[i].descriptorType;writes[i].pImageInfo=&images[i];}
        vkUpdateDescriptorSets(device_,2,writes.data(),0,nullptr);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=createShader(shaderPath);ci.stage.pName="main";ci.layout=layout;
        VkPipeline pipeline;checked(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline),"FG blur pipeline");pipelines_.push_back(pipeline);
        submit([&](VkCommandBuffer cmd){
            transition(cmd,source,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={w,h,1};
            vkCmdCopyBufferToImage(cmd,upload.handle,source.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            transition(cmd,source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            transition(cmd,output,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            const std::array<float,4> parameters{1.0f/w,0,2,0};
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
            vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,16,parameters.data());vkCmdDispatch(cmd,(w+7)/8,(h+7)/8,1);
            transition(cmd,output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageToBuffer(cmd,output,VK_IMAGE_ASPECT_COLOR_BIT,readback);
        });
        auto actual=static_cast<const uint8_t *>(readback.mapped);
        for(int y=0;y<int(h);++y) for(int x=0;x<int(w);++x) for(int c=0;c<4;++c) {
            float expected=0; for(int k=-2;k<=2;++k) expected+=bytes[4*(y*w+std::clamp(x+k,0,int(w)-1))+c]/5.0f;
            require(std::abs(actual[4*(y*w+x)+c]-expected)<=1.1f,"HUD-less blur diverged from the final-color box filter or made alpha opaque");
        }
        std::cout << "[PASS] HUD-less GPU blur: exact five-tap box filter, clamped edges, zero UI coverage, partial groups\n";
    }

    void colorMaskAndScissorCase() {
        Image color = createImage(WIDTH, HEIGHT, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Buffer readback = createBuffer(WIDTH * HEIGHT * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkRenderPass renderPass = createColorRenderPass({color.format});
        VkFramebuffer framebuffer = createFramebuffer(renderPass, {color.view}, WIDTH, HEIGHT);
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_B_BIT;
        VkPipeline pipeline = createGraphicsPipeline(renderPass, fragmentShader_, {blend}, false, false);

        const VkClearColorValue initial{{0.1f, 0.2f, 0.3f, 0.4f}};
        const std::array<float, 4> replacement{0.8f, 0.6f, 0.4f, 0.2f};
        const VkRect2D scissor{{2, 1}, {3, 2}};
        submit([&](VkCommandBuffer commands) {
            transition(commands, color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(commands, color.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &initial, 1, &range);
            transition(commands, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            draw(commands, renderPass, framebuffer, pipeline, {WIDTH, HEIGHT}, scissor, replacement, false);
            transition(commands, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageToBuffer(commands, color, VK_IMAGE_ASPECT_COLOR_BIT, readback);
        });

        const auto *pixels = static_cast<const uint8_t *>(readback.mapped);
        const std::array<uint8_t, 4> outside{unorm(0.1f), unorm(0.2f), unorm(0.3f), unorm(0.4f)};
        const std::array<uint8_t, 4> inside{unorm(0.8f), unorm(0.2f), unorm(0.4f), unorm(0.4f)};
        for (uint32_t y = 0; y < HEIGHT; ++y) {
            for (uint32_t x = 0; x < WIDTH; ++x) {
                const bool covered = x >= 2 && x < 5 && y >= 1 && y < 3;
                const auto &expected = covered ? inside : outside;
                const size_t base = (y * WIDTH + x) * 4;
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    require(nearByte(pixels[base + channel], expected[channel]),
                            "Scissor/color-mask clear changed the wrong pixel or channel");
                }
            }
        }
        std::cout << "[PASS] scissor + partial RGBA write mask used formal clear shader\n";
    }

    void mrtCase() {
        Image first = createImage(4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Image second = createImage(4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Buffer firstReadback = createBuffer(4 * 4 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Buffer secondReadback = createBuffer(4 * 4 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkRenderPass renderPass = createColorRenderPass({first.format, second.format});
        VkFramebuffer framebuffer = createFramebuffer(renderPass, {first.view, second.view}, 4, 4);
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipeline pipeline = createGraphicsPipeline(renderPass, mrtFragmentShader_, {blend, blend}, false, false);
        const VkClearColorValue firstInitial{{0.0f, 0.1f, 0.2f, 0.3f}};
        const VkClearColorValue secondInitial{{0.3f, 0.2f, 0.1f, 0.0f}};
        const std::array<float, 4> replacement{0.25f, 0.5f, 0.75f, 1.0f};
        const VkRect2D scissor{{0, 0}, {4, 4}};
        submit([&](VkCommandBuffer commands) {
            prepareColor(commands, first, firstInitial);
            prepareColor(commands, second, secondInitial);
            draw(commands, renderPass, framebuffer, pipeline, {4, 4}, scissor, replacement, false);
            finishColor(commands, first, firstReadback);
            finishColor(commands, second, secondReadback);
        });
        const std::array<uint8_t, 4> expected{unorm(0.25f), unorm(0.5f), unorm(0.75f), unorm(1.0f)};
        verifySolidRgba(firstReadback, 4, 4, expected, "MRT attachment zero");
        verifySolidRgba(secondReadback, 4, 4, expected, "MRT attachment one");
        std::cout << "[PASS] two color attachments cleared by clear_2_frag.spv\n";
    }

    void depthStencilCase() {
        constexpr VkFormat format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &formatProperties);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((formatProperties.optimalTilingFeatures & required) != required) {
            std::cout << "[SKIP] depth/stencil masked clear: D32_SFLOAT_S8_UINT attachment/transfer unsupported\n";
            return;
        }

        constexpr uint32_t width = 4, height = 4;
        Image depthStencil = createImage(width, height, format,
                                         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Buffer depthReadback = createBuffer(width * height * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Buffer stencilReadback = createBuffer(width * height, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkRenderPass renderPass = createDepthStencilRenderPass(format);
        VkFramebuffer framebuffer = createFramebuffer(renderPass, {depthStencil.view}, width, height);
        VkPipeline pipeline = createGraphicsPipeline(renderPass, fragmentShader_, {}, true, true);
        const VkClearDepthStencilValue initial{0.25f, 0xAA};
        const VkRect2D scissor{{1, 1}, {2, 2}};
        submit([&](VkCommandBuffer commands) {
            transition(commands, depthStencil, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageSubresourceRange range{
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(commands, depthStencil.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        &initial, 1, &range);
            transition(commands, depthStencil, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
            // Re-enter an attachment after sampling, as main-target/HUD consumers do.
            // The device deliberately does not enable separateDepthStencilLayouts.
            transition(commands, depthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
            transition(commands, depthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
            draw(commands, renderPass, framebuffer, pipeline, {width, height}, scissor,
                 {0.0f, 0.0f, 0.0f, 0.0f}, true);
            transition(commands, depthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageToBuffer(commands, depthStencil, VK_IMAGE_ASPECT_DEPTH_BIT, depthReadback);
            copyImageToBuffer(commands, depthStencil, VK_IMAGE_ASPECT_STENCIL_BIT, stencilReadback);
        });

        const auto *depthValues = static_cast<const float *>(depthReadback.mapped);
        const auto *stencilValues = static_cast<const uint8_t *>(stencilReadback.mapped);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const bool covered = x >= 1 && x < 3 && y >= 1 && y < 3;
                const size_t index = y * width + x;
                require(std::abs(depthValues[index] - (covered ? 0.75f : 0.25f)) <= 1.0e-6f,
                        "Depth masked clear wrote the wrong value or region");
                require(stencilValues[index] == (covered ? 0xA5 : 0xAA),
                        "Stencil write mask did not preserve the untouched high nibble");
            }
        }
        std::cout << "[PASS] depth viewport value and stencil 0x0f write mask\n";
    }

    void blitFlipCase() {
        constexpr uint32_t width = 4, height = 4;
        Image source = createImage(width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Image destination = createImage(width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Buffer upload = createBuffer(width * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer readback = createBuffer(width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto *sourcePixels = static_cast<uint8_t *>(upload.mapped);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t base = (y * width + x) * 4;
                sourcePixels[base + 0] = static_cast<uint8_t>(10 * y + x);
                sourcePixels[base + 1] = static_cast<uint8_t>(100 + 10 * y + x);
                sourcePixels[base + 2] = 0;
                sourcePixels[base + 3] = 255;
            }
        }

        submit([&](VkCommandBuffer commands) {
            transition(commands, source, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy uploadRegion{};
            uploadRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            uploadRegion.imageExtent = {width, height, 1};
            vkCmdCopyBufferToImage(commands, upload.handle, source.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &uploadRegion);
            transition(commands, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            transition(commands, destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(commands, destination.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                                 &range);

            VkImageBlit region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffsets[0] = {1, 1, 0};
            region.srcOffsets[1] = {3, 3, 1};
            region.dstOffsets[0] = {3, 1, 0};
            region.dstOffsets[1] = {1, 3, 1};
            vkCmdBlitImage(commands, source.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
            transition(commands, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageToBuffer(commands, destination, VK_IMAGE_ASPECT_COLOR_BIT, readback);
        });

        const auto *pixels = static_cast<const uint8_t *>(readback.mapped);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t base = (y * width + x) * 4;
                if (x >= 1 && x < 3 && y >= 1 && y < 3) {
                    const uint32_t sourceX = x == 1 ? 2 : 1;
                    require(pixels[base] == 10 * y + sourceX && pixels[base + 1] == 100 + 10 * y + sourceX &&
                                pixels[base + 2] == 0 && pixels[base + 3] == 255,
                            "Region blit did not preserve the requested horizontal flip");
                } else {
                    require(pixels[base] == 0 && pixels[base + 1] == 0 && pixels[base + 2] == 0 &&
                                pixels[base + 3] == 0,
                            "Region blit modified a pixel outside the destination rectangle");
                }
            }
        }
        std::cout << "[PASS] nearest color blit region with horizontal flip\n";
    }

    void mainDepthProjectionCase(const std::filesystem::path &shaderPath, bool blit = false) {
        constexpr uint32_t sourceWidth = 2, sourceHeight = 2, targetWidth = 4, targetHeight = 4;
        const VkImageAspectFlags sourceAspect = blit ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        const auto sampledLayout = blit ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        Image source = createImage(sourceWidth, sourceHeight, blit ? VK_FORMAT_D16_UNORM : VK_FORMAT_R16_SFLOAT, sourceAspect,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       (blit ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0));
        Image target = createImage(targetWidth, targetHeight, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT,
                                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Buffer upload = createBuffer(sourceWidth * sourceHeight * sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer readback = createBuffer(targetWidth * targetHeight * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const std::array<uint16_t, 4> halfDepths = blit ? std::array<uint16_t, 4>{8192, 16384, 32768, 49152}
            : std::array<uint16_t, 4>{0x3c00, 0x4000, 0x4400, 0x4800}; // 1, 2, 4, 8
        std::memcpy(upload.mapped, halfDepths.data(), sizeof(halfDepths));

        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 1.0f;
        VkSampler sampler = VK_NULL_HANDLE;
        checked(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler), "vkCreateSampler(main depth)");
        samplers_.push_back(sampler);

        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                             VK_SHADER_STAGE_FRAGMENT_BIT};
        VkDescriptorSetLayoutCreateInfo setLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setLayoutInfo.bindingCount = 1;
        setLayoutInfo.pBindings = &binding;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        checked(vkCreateDescriptorSetLayout(device_, &setLayoutInfo, nullptr, &setLayout),
                "vkCreateDescriptorSetLayout(main depth)");
        descriptorSetLayouts_.push_back(setLayout);
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        checked(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool(main depth)");
        descriptorPools_.push_back(pool);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &setLayout;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        checked(vkAllocateDescriptorSets(device_, &allocate, &descriptor), "vkAllocateDescriptorSets(main depth)");
        VkDescriptorImageInfo imageInfo{sampler, source.view, sampledLayout};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptor;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

        struct PushConstant {
            std::array<float, 4> projection;
            std::array<float, 2> targetSize;
            std::array<float, 2> padding;
        } push{{-1.001001001f, -0.100100100f, -1.0f, 0.0f},
               {static_cast<float>(targetWidth), static_cast<float>(targetHeight)}, {0.0f, 0.0f}};
        static_assert(sizeof(PushConstant) == 32);
        struct BlitPush {
            std::array<float, 4> sourceRect;
            std::array<float, 4> destinationRect;
            std::array<float, 2> sourceSize;
            std::array<float, 2> destinationSize;
            int level;
        } blitPush{{-1, 0, 2, 2}, {4, 0, 0, 4}, {2, 2}, {4, 4}, 0};
        const uint32_t pushSize = blit ? sizeof(BlitPush) : sizeof(PushConstant);
        VkPushConstantRange pushRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        checked(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &layout),
                "vkCreatePipelineLayout(main depth)");
        extraPipelineLayouts_.push_back(layout);
        VkShaderModule fragment = createShader(shaderPath);
        VkRenderPass renderPass = createDepthStencilRenderPass(target.format);
        VkFramebuffer framebuffer = createFramebuffer(renderPass, {target.view}, targetWidth, targetHeight);
        VkPipeline pipeline = createGraphicsPipeline(renderPass, fragment, {}, true, false, layout);

        auto produceDepth = [&](VkCommandBuffer commands) {
            transition(commands, source, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy uploadRegion{};
            uploadRegion.imageSubresource = {sourceAspect, 0, 0, 1};
            uploadRegion.imageExtent = {sourceWidth, sourceHeight, 1};
            vkCmdCopyBufferToImage(commands, upload.handle, source.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &uploadRegion);
            transition(commands, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       sampledLayout, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        };
        // Match the renderer: record the HUD consumer first, then the world
        // producer, and submit producer before consumer without an idle wait.
        submit([&](VkCommandBuffer commands) {
            transition(commands, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearDepthStencilValue clear{1.0f, 0};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(commands, target.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            transition(commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass = renderPass;
            begin.framebuffer = framebuffer;
            begin.renderArea = {{0, 0}, {targetWidth, targetHeight}};
            vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptor, 0,
                                    nullptr);
            vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize,
                               blit ? static_cast<const void *>(&blitPush) : static_cast<const void *>(&push));
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(targetWidth), static_cast<float>(targetHeight),
                                0.0f, 1.0f};
            VkRect2D scissor = blit ? VkRect2D{{0, 1}, {3, 2}} : VkRect2D{{0, 0}, {targetWidth, targetHeight}};
            vkCmdSetViewport(commands, 0, 1, &viewport);
            vkCmdSetScissor(commands, 0, 1, &scissor);
            const std::array<float, 4> unusedBlendConstants{};
            vkCmdSetBlendConstants(commands, unusedBlendConstants.data());
            vkCmdDraw(commands, 3, 1, 0, 0);
            vkCmdEndRenderPass(commands);
            transition(commands, target, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageToBuffer(commands, target, VK_IMAGE_ASPECT_DEPTH_BIT, readback);
        }, produceDepth);

        const auto *values = static_cast<const float *>(readback.mapped);
        const std::array<float, 4> sourceDepths{1.0f, 2.0f, 4.0f, 8.0f};
        for (uint32_t y = 0; y < targetHeight; ++y) {
            for (uint32_t x = 0; x < targetWidth; ++x) {
                float linear = sourceDepths[(y / 2) * 2 + (x / 2)];
                float viewZ = -linear;
                float expected = std::clamp((push.projection[0] * viewZ + push.projection[1]) /
                                                (push.projection[2] * viewZ + push.projection[3]),
                                            0.0f, 1.0f);
                if (blit) {
                    const double sourceX = -1.0 + ((x + 0.5 - 4.0) / -4.0) * 3.0;
                    expected = 1.0f;
                    if (x < 3 && y >= 1 && y < 3 && sourceX >= 0 && sourceX < 2) {
                        expected = halfDepths[(y / 2) * 2 + static_cast<uint32_t>(std::floor(sourceX))] / 65535.0f;
                    }
                }
                require(std::abs(values[y * targetWidth + x] - expected) <= 2.0e-6f,
                        std::string(blit ? "Depth blit" : "Main depth") + " pixel " + std::to_string(x) + "," +
                        std::to_string(y) + " actual=" + std::to_string(values[y * targetWidth + x]) +
                        " expected=" + std::to_string(expected));
            }
        }
        std::cout << (blit ? "[PASS] fractional clipped/flipped D16-to-D32 blit preserves scissor exterior\n" : "[PASS] normalized first-hit sampling and projection depth conversion\n");
    }

  private:
    void createInstance() {
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        if (enumerateVersion != nullptr) checked(enumerateVersion(&loaderVersion), "vkEnumerateInstanceVersion");
        if (loaderVersion < VK_API_VERSION_1_4) throw SkipError("Vulkan 1.4 loader is unavailable");
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "MCVR framebuffer GPU test";
        application.applicationVersion = 1;
        application.pEngineName = "MCVR test";
        application.engineVersion = 1;
        application.apiVersion = VK_API_VERSION_1_4;
        VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        createInfo.pApplicationInfo = &application;
        const char *layer = "VK_LAYER_KHRONOS_validation";
        const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        VkValidationFeatureEnableEXT synchronization = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validation.enabledValidationFeatureCount = 1;
        validation.pEnabledValidationFeatures = &synchronization;
        if (validateSync_) {
            createInfo.enabledLayerCount = 1; createInfo.ppEnabledLayerNames = &layer;
            createInfo.enabledExtensionCount = 2; createInfo.ppEnabledExtensionNames = extensions;
            createInfo.pNext = &validation;
        }
        VkResult result = vkCreateInstance(&createInfo, nullptr, &instance_);
        if (validateSync_ && (result == VK_ERROR_LAYER_NOT_PRESENT || result == VK_ERROR_EXTENSION_NOT_PRESENT))
            throw SkipError("Synchronization validation layer/extensions unavailable");
        if (result == VK_ERROR_INCOMPATIBLE_DRIVER) throw SkipError("Vulkan 1.4 instance is unavailable");
        checked(result, "vkCreateInstance");
        if (validateSync_) {
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pUserData = &validationErrors_;
            info.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                const VkDebugUtilsMessengerCallbackDataEXT *data, void *errors) -> VkBool32 {
                ++*static_cast<std::atomic<unsigned> *>(errors);
                std::fprintf(stderr, "[VVL] %s\n", data->pMessage);
                return VK_FALSE;
            };
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
            checked(create(instance_, &info, nullptr, &messenger_), "vkCreateDebugUtilsMessengerEXT");
        }
    }

    void selectDevice() {
        uint32_t count = 0;
        checked(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices(count)");
        if (count == 0) throw SkipError("No Vulkan physical device is available");
        std::vector<VkPhysicalDevice> devices(count);
        checked(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices");
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_4) continue;
            uint32_t queueCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queueCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
            for (uint32_t i = 0; i < queueCount; ++i) {
                if (queues[i].queueCount != 0 && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                    physicalDevice_ = candidate;
                    properties_ = properties;
                    queueFamily_ = i;
                    return;
                }
            }
        }
        throw SkipError("No Vulkan 1.4 graphics queue is available");
    }

    void createDevice() {
        VkPhysicalDeviceVulkan13Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &supported;
        vkGetPhysicalDeviceFeatures2(physicalDevice_, &query);
        if (!supported.shaderDemoteToHelperInvocation)
            throw SkipError("Framebuffer depth-blit shader requires shaderDemoteToHelperInvocation");
        VkPhysicalDeviceVulkan13Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        enabled.shaderDemoteToHelperInvocation = VK_TRUE;
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        createInfo.pNext = &enabled;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;
        checked(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "vkCreateDevice");
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    }

    void createCommands() {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        checked(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = commandPool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        checked(vkAllocateCommandBuffers(device_, &allocate, &commandBuffer_), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        checked(vkCreateFence(device_, &fenceInfo, nullptr, &fence_), "vkCreateFence");
    }

    uint32_t memoryType(uint32_t typeBits, VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred = 0) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &properties);
        uint32_t fallback = UINT32_MAX;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) == 0 ||
                (properties.memoryTypes[i].propertyFlags & required) != required) {
                continue;
            }
            if ((properties.memoryTypes[i].propertyFlags & preferred) == preferred) return i;
            if (fallback == UINT32_MAX) fallback = i;
        }
        if (fallback == UINT32_MAX) throw SkipError("Required Vulkan memory type is unavailable");
        return fallback;
    }

  public:
    void diagramSurfaceCase(const std::filesystem::path &shaderPath) {
        constexpr uint32_t samples = 257 * 32 * 32;
        auto output = createBuffer(samples * 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 1; layoutInfo.pBindings = &binding;
        VkDescriptorSetLayout setLayout;
        checked(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout), "diagram test layout");
        descriptorSetLayouts_.push_back(setLayout);
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1; poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &size;
        VkDescriptorPool pool;
        checked(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool), "diagram test pool");
        descriptorPools_.push_back(pool);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &setLayout;
        VkDescriptorSet set;
        checked(vkAllocateDescriptorSets(device_, &allocate, &set), "diagram test set");
        VkDescriptorBufferInfo data{output.handle, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &data;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &setLayout;
        VkPipelineLayout layout;
        checked(vkCreatePipelineLayout(device_, &pl, nullptr, &layout), "diagram test pipeline layout");
        extraPipelineLayouts_.push_back(layout);
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; info.stage.module = createShader(shaderPath);
        info.stage.pName = "main"; info.layout = layout;
        VkPipeline pipeline;
        checked(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "diagram test pipeline");
        pipelines_.push_back(pipeline);
        submit([&](VkCommandBuffer command) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(command, (samples + 63) / 64, 1, 1);
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        });
        auto values = static_cast<const float *>(output.mapped);
        for (uint32_t i = 0; i < samples; ++i) {
            float coverage = float(i / 1024) / 256.0f;
            float luminosity = std::floor(std::clamp((coverage - .5f + .18f) * 1.8f + .5f, 0.0f, 1.0f) * 32) / 32;
            float scaled = std::max(luminosity - .00001f, 0.0f) * 7;
            unsigned rank = 0, x = i % 32, y = (i / 32) % 32;
            constexpr unsigned digits[2][2] = {{0, 2}, {3, 1}};
            for (int bit = 0; bit < 5; ++bit) rank = rank * 4 + digits[(y >> bit) & 1][(x >> bit) & 1];
            float threshold = float(rank >> 2) / 255 * .99f + .005f;
            float reference = (std::floor(scaled) + (scaled - std::floor(scaled) >= threshold ? 1 : 0)) / 7;
            require(std::abs(values[i * 4] - reference) < 1e-5f, "Diagram fade differs from Simulated gradient reference");
            if (i >= 1024) require(values[i * 4] + 1e-5f >= values[(i-1024) * 4], "Diagram fade reverses direction");
            require(std::abs(values[i * 4 + 1] - (.2f + .8f * coverage)) < 1e-5f && values[i * 4 + 2] < 1e-5f,
                "Spring stress and hurt surface recoloring disagree");
            require(std::abs(values[i * 4 + 3] - 229.75f) < 1e-5f, "Diagram GL/UI origin mismatch");
        }
        std::cout << "[PASS] 263168 diagram/stress/hurt samples, monotone fade and GUI origin\n";
    }

    void inlineUpdateCase() {
        auto shared = createBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto readback = createBuffer(8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        submit([&](VkCommandBuffer commands) {
            std::array<uint32_t, 1> value{17};
            vk::recordInlineUpdate(commands, shared.handle, value);
            VkBufferCopy first{0, 0, 4};
            vkCmdCopyBuffer(commands, shared.handle, readback.handle, 1, &first);
            value[0] = 93;
            vk::recordInlineUpdate(commands, shared.handle, value);
            VkBufferCopy second{0, 4, 4};
            vkCmdCopyBuffer(commands, shared.handle, readback.handle, 1, &second);
            value[0] = 0; // stack contents change before submission; recorded values must survive.
            VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = readback.handle; host.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                0, 0, nullptr, 1, &host, 0, nullptr);
        });
        auto result = static_cast<const uint32_t *>(readback.mapped);
        require(result[0] == 17 && result[1] == 93, "Per-dispatch constants aliased another view");
        std::cout << "[PASS] sequential view constants retain their recorded values\n";
    }
  private:
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        Buffer buffer{.size = size};
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checked(vkCreateBuffer(device_, &info, nullptr, &buffer.handle), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, buffer.handle, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        checked(vkAllocateMemory(device_, &allocation, nullptr, &buffer.memory), "vkAllocateMemory(buffer)");
        checked(vkBindBufferMemory(device_, buffer.handle, buffer.memory, 0), "vkBindBufferMemory");
        checked(vkMapMemory(device_, buffer.memory, 0, size, 0, &buffer.mapped), "vkMapMemory");
        buffers_.push_back(buffer);
        return buffer;
    }

    Image createImage(uint32_t width, uint32_t height, VkFormat format, VkImageAspectFlags aspects,
                      VkImageUsageFlags usage) {
        Image image{.format = format, .aspects = aspects, .width = width, .height = height};
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        checked(vkCreateImage(device_, &info, nullptr, &image.handle), "vkCreateImage");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, image.handle, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex =
            memoryType(requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(vkAllocateMemory(device_, &allocation, nullptr, &image.memory), "vkAllocateMemory(image)");
        checked(vkBindImageMemory(device_, image.handle, image.memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image.handle;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {aspects, 0, 1, 0, 1};
        if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            checked(vkCreateImageView(device_, &viewInfo, nullptr, &image.view), "vkCreateImageView");
        }
        images_.push_back(image);
        return image;
    }

    VkShaderModule createShader(const std::filesystem::path &path) {
        const auto code = readSpirv(path);
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size() * sizeof(uint32_t);
        info.pCode = code.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        checked(vkCreateShaderModule(device_, &info, nullptr, &shader), "vkCreateShaderModule");
        shaders_.push_back(shader);
        return shader;
    }

    VkRenderPass createColorRenderPass(const std::vector<VkFormat> &formats) {
        std::vector<VkAttachmentDescription> descriptions(formats.size());
        std::vector<VkAttachmentReference> references(formats.size());
        for (uint32_t i = 0; i < formats.size(); ++i) {
            descriptions[i] = {.format = formats[i],
                               .samples = VK_SAMPLE_COUNT_1_BIT,
                               .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                               .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                               .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                               .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            references[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = static_cast<uint32_t>(references.size());
        subpass.pColorAttachments = references.data();
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = static_cast<uint32_t>(descriptions.size());
        info.pAttachments = descriptions.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        checked(vkCreateRenderPass(device_, &info, nullptr, &renderPass), "vkCreateRenderPass(color)");
        renderPasses_.push_back(renderPass);
        return renderPass;
    }

    VkRenderPass createDepthStencilRenderPass(VkFormat format) {
        VkAttachmentDescription description{.format = format,
                                            .samples = VK_SAMPLE_COUNT_1_BIT,
                                            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                                            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &reference;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = 1;
        info.pAttachments = &description;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        checked(vkCreateRenderPass(device_, &info, nullptr, &renderPass), "vkCreateRenderPass(depth stencil)");
        renderPasses_.push_back(renderPass);
        return renderPass;
    }

    VkFramebuffer createFramebuffer(VkRenderPass renderPass, const std::vector<VkImageView> &views,
                                    uint32_t width, uint32_t height) {
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = renderPass;
        info.attachmentCount = static_cast<uint32_t>(views.size());
        info.pAttachments = views.data();
        info.width = width;
        info.height = height;
        info.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        checked(vkCreateFramebuffer(device_, &info, nullptr, &framebuffer), "vkCreateFramebuffer");
        framebuffers_.push_back(framebuffer);
        return framebuffer;
    }

    VkPipeline createGraphicsPipeline(VkRenderPass renderPass, VkShaderModule fragment,
                                      const std::vector<VkPipelineColorBlendAttachmentState> &blendAttachments,
                                      bool depth, bool stencil, VkPipelineLayout layout = VK_NULL_HANDLE) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_VERTEX_BIT, vertexShader_, "main"};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main"};
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = depth;
        depthStencil.depthWriteEnable = depth;
        depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        depthStencil.stencilTestEnable = stencil;
        depthStencil.front = {VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                              VK_COMPARE_OP_ALWAYS, ~0u, 0x0fu, 0x55u};
        depthStencil.back = depthStencil.front;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
        blend.pAttachments = blendAttachments.data();
        std::array<VkDynamicState, 5> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                                    VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                                    VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                                    VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layout == VK_NULL_HANDLE ? pipelineLayout_ : layout;
        info.renderPass = renderPass;
        info.subpass = 0;
        VkPipeline pipeline = VK_NULL_HANDLE;
        checked(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
                "vkCreateGraphicsPipelines");
        pipelines_.push_back(pipeline);
        return pipeline;
    }

    void submit(const std::function<void(VkCommandBuffer)> &record,
                const std::function<void(VkCommandBuffer)> &producer = {}) {
        checked(vkResetFences(device_, 1, &fence_), "vkResetFences");
        checked(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(commandBuffer_, &begin), "vkBeginCommandBuffer");
        record(commandBuffer_);
        checked(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");
        VkCommandBuffer producerBuffer = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> ordered;
        if (producer) {
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = commandPool_;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            checked(vkAllocateCommandBuffers(device_, &allocate, &producerBuffer), "vkAllocateCommandBuffers(producer)");
            checked(vkBeginCommandBuffer(producerBuffer, &begin), "vkBeginCommandBuffer(producer)");
            producer(producerBuffer);
            checked(vkEndCommandBuffer(producerBuffer), "vkEndCommandBuffer(producer)");
            ordered.push_back(producerBuffer);
        }
        ordered.push_back(commandBuffer_);
        // A regressed recording must fail before invalid work reaches the GPU.
        require(validationErrors_ == 0, "Validation rejected recording before submission");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = static_cast<uint32_t>(ordered.size());
        submit.pCommandBuffers = ordered.data();
        checked(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");
        checked(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        if (producerBuffer != VK_NULL_HANDLE) vkFreeCommandBuffers(device_, commandPool_, 1, &producerBuffer);
    }

    static void transition(VkCommandBuffer commands, const Image &image, VkImageLayout oldLayout,
                           VkImageLayout newLayout, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                           VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = sourceAccess;
        barrier.dstAccessMask = destinationAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.handle;
        barrier.subresourceRange = vk::formatSubresourceRange(image.format, 1, 1, 1);
        vkCmdPipelineBarrier(commands, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    static void draw(VkCommandBuffer commands, VkRenderPass renderPass, VkFramebuffer framebuffer,
                     VkPipeline pipeline, VkExtent2D extent, VkRect2D scissor,
                     const std::array<float, 4> &blendConstants, bool depthStencil) {
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = renderPass;
        begin.framebuffer = framebuffer;
        begin.renderArea = {{0, 0}, extent};
        vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                            depthStencil ? 0.75f : 0.0f, depthStencil ? 0.75f : 1.0f};
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
        vkCmdSetBlendConstants(commands, blendConstants.data());
        if (depthStencil) {
            vkCmdSetStencilReference(commands, VK_STENCIL_FACE_FRONT_AND_BACK, 0x55);
            vkCmdSetStencilWriteMask(commands, VK_STENCIL_FACE_FRONT_AND_BACK, 0x0f);
        }
        vkCmdDraw(commands, 3, 1, 0, 0);
        vkCmdEndRenderPass(commands);
    }

    static void copyImageToBuffer(VkCommandBuffer commands, const Image &image, VkImageAspectFlags aspect,
                                  const Buffer &buffer) {
        VkBufferImageCopy copy{};
        copy.imageSubresource = {aspect, 0, 0, 1};
        copy.imageExtent = {image.width, image.height, 1};
        vkCmdCopyImageToBuffer(commands, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.handle, 1,
                               &copy);
        VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostRead.buffer = buffer.handle;
        hostRead.offset = 0;
        hostRead.size = buffer.size;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                             1, &hostRead, 0, nullptr);
    }

    static void prepareColor(VkCommandBuffer commands, const Image &image, const VkClearColorValue &initial) {
        transition(commands, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(commands, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &initial, 1, &range);
        transition(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

    static void finishColor(VkCommandBuffer commands, const Image &image, const Buffer &readback) {
        transition(commands, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        copyImageToBuffer(commands, image, VK_IMAGE_ASPECT_COLOR_BIT, readback);
    }

    static void verifySolidRgba(const Buffer &buffer, uint32_t width, uint32_t height,
                                const std::array<uint8_t, 4> &expected, const char *label) {
        const auto *pixels = static_cast<const uint8_t *>(buffer.mapped);
        for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
            for (uint32_t channel = 0; channel < 4; ++channel) {
                require(nearByte(pixels[pixel * 4 + channel], expected[channel]),
                        std::string(label) + " did not receive the clear value");
            }
        }
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    bool validateSync_ = false;
    std::atomic<unsigned> validationErrors_{0};
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    uint32_t queueFamily_ = UINT32_MAX;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkShaderModule vertexShader_ = VK_NULL_HANDLE;
    VkShaderModule fragmentShader_ = VK_NULL_HANDLE;
    VkShaderModule mrtFragmentShader_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::vector<Buffer> buffers_;
    std::vector<Image> images_;
    std::vector<VkShaderModule> shaders_;
    std::vector<VkRenderPass> renderPasses_;
    std::vector<VkPipeline> pipelines_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkDescriptorSetLayout> descriptorSetLayouts_;
    std::vector<VkDescriptorPool> descriptorPools_;
    std::vector<VkSampler> samplers_;
    std::vector<VkPipelineLayout> extraPipelineLayouts_;
};
} // namespace

int main(int argc, char **argv) {
    if (argc != 6 && argc != 7 && argc != 8 && argc != 9 && argc != 10) {
        std::cerr << "[FAIL] expected clear vertex, single-output fragment, two-output fragment, main-depth, and depth-blit fragment paths\n";
        return 1;
    }
    try {
        FramebufferHarness harness(std::filesystem::absolute(argv[1]), std::filesystem::absolute(argv[2]),
                                   std::filesystem::absolute(argv[3]), std::string_view(argv[4]) == "--post-sync");
        if (std::string_view(argv[4]) == "--post-sync") {
            harness.postColorSyncCase();
            return 0;
        }
        if (std::string_view(argv[4]) == "--diagram-surface") {
            harness.diagramSurfaceCase(std::filesystem::absolute(argv[5]));
            return 0;
        }
        if (std::string_view(argv[4]) == "--execution") {
            harness.executionDescriptorCase(std::filesystem::absolute(argv[5]));
            return 0;
        }
        if (std::string_view(argv[4]) == "--ponder-coverage") {
            harness.ponderCoverageCase(std::filesystem::absolute(argv[5]));
            return 0;
        }
        if (std::string_view(argv[4]) == "--translated-coverage") {
            harness.translatedCoverageCase(std::filesystem::absolute(argv[5]));
            return 0;
        }
        if (argc >= 8) {
            harness.inlineUpdateCase();
            harness.backgroundModulationCase();
            harness.dlssUiCoverageCase(std::filesystem::absolute(argv[6]));
            if (argc >= 9) harness.dlssUiCoverageCase(std::filesystem::absolute(argv[6]), std::filesystem::absolute(argv[8]));
            harness.hudlessBlurCase(std::filesystem::absolute(argv[7]));
            if (argc == 10) harness.weightedBlurCase(std::filesystem::absolute(argv[7]), std::filesystem::absolute(argv[9]));
            return 0;
        }
        if (argc == 7) {
            harness.dlssUiCoverageCase(std::filesystem::absolute(argv[6]));
            return 0;
        }
        std::cout << "[GPU] " << harness.properties().deviceName << " API "
                  << VK_API_VERSION_MAJOR(harness.properties().apiVersion) << '.'
                  << VK_API_VERSION_MINOR(harness.properties().apiVersion) << '.'
                  << VK_API_VERSION_PATCH(harness.properties().apiVersion) << '\n';
        harness.colorMaskAndScissorCase();
        harness.mrtCase();
        harness.depthStencilCase();
        harness.blitFlipCase();
        harness.mainDepthProjectionCase(std::filesystem::absolute(argv[4]));
        harness.mainDepthProjectionCase(std::filesystem::absolute(argv[5]), true);
        return 0;
    } catch (const SkipError &skip) {
        std::cout << "[SKIP] " << skip.what() << '\n';
        return SKIP_RETURN_CODE;
    } catch (const std::exception &failure) {
        std::cerr << "[FAIL] " << failure.what() << '\n';
        return 1;
    }
}
