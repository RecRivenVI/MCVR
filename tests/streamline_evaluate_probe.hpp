#pragma once
#include "core/render/streamline_runtime.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

inline void evaluateProbe(VkPhysicalDevice physical, VkDevice device, uint32_t family) {
    auto require=[](VkResult result) { if (result!=VK_SUCCESS) throw std::runtime_error("Vulkan result="+std::to_string(result)); };
    auto &sl=mcvr::StreamlineRuntime::get();
    VkQueue queue{}; vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.queueFamilyIndex=family;
    VkCommandPool pool{}; require(vkCreateCommandPool(device,&poolInfo,nullptr,&pool));
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    VkCommandBuffer cmd{}; require(vkAllocateCommandBuffers(device,&allocation,&cmd));
    VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    for (bool rr:{false,true}) {
        const sl::ViewportHandle viewport(rr?2:1);
        uint32_t width{},height{};
        if (rr) {
            sl::DLSSDOptions o{}; o.mode=sl::DLSSMode::eBalanced; o.outputWidth=1280; o.outputHeight=720;
            o.balancedPreset=sl::DLSSDPreset::ePresetD; o.normalRoughnessMode=sl::DLSSDNormalRoughnessMode::ePacked;
            o.worldToCameraView=o.cameraViewToWorld=mcvr::slMatrix(glm::mat4(1));
            sl::DLSSDOptimalSettings s{};
            if (!sl.check(sl.slDLSSDGetOptimalSettings(o,s),"probe RR size") || !sl.check(sl.slDLSSDSetOptions(viewport,o),"probe RR options")) throw std::runtime_error("RR setup");
            width=s.optimalRenderWidth; height=s.optimalRenderHeight;
        } else {
            sl::DLSSOptions o{}; o.mode=sl::DLSSMode::eBalanced; o.outputWidth=1280; o.outputHeight=720;
            sl::DLSSOptimalSettings s{};
            if (!sl.check(sl.slDLSSGetOptimalSettings(o,s),"probe SR size") || !sl.check(sl.slDLSSSetOptions(viewport,o),"probe SR options")) throw std::runtime_error("SR setup");
            width=s.optimalRenderWidth; height=s.optimalRenderHeight;
        }
        const sl::BufferType types[]{sl::kBufferTypeScalingInputColor,sl::kBufferTypeScalingOutputColor,sl::kBufferTypeMotionVectors,
            rr?sl::kBufferTypeLinearDepth:sl::kBufferTypeDepth,sl::kBufferTypeAlbedo,sl::kBufferTypeSpecularAlbedo,sl::kBufferTypeNormalRoughness,sl::kBufferTypeSpecularHitDistance};
        std::array<VkImage,8> images{}; std::array<VkDeviceMemory,8> allocations{}; std::array<VkImageView,8> views{};
        std::array<sl::Resource,8> resources{}; std::vector<sl::ResourceTag> tags;
        const unsigned count=rr?8:4;
        require(vkResetCommandPool(device,pool,0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; require(vkBeginCommandBuffer(cmd,&begin));
        for (unsigned i=0;i<count;++i) {
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; imageInfo.imageType=VK_IMAGE_TYPE_2D;
            imageInfo.format=(i==3 || i==7)?VK_FORMAT_R32_SFLOAT:i==2?VK_FORMAT_R16G16_SFLOAT:VK_FORMAT_R16G16B16A16_SFLOAT;
            imageInfo.extent={i==1?1280:width,i==1?720:height,1}; imageInfo.mipLevels=1; imageInfo.arrayLayers=1;
            imageInfo.samples=VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling=VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            require(vkCreateImage(device,&imageInfo,nullptr,&images[i]));
            VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device,images[i],&requirements);
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; alloc.allocationSize=requirements.size;
            while (!(requirements.memoryTypeBits&(1u<<alloc.memoryTypeIndex))) ++alloc.memoryTypeIndex;
            require(vkAllocateMemory(device,&alloc,nullptr,&allocations[i])); require(vkBindImageMemory(device,images[i],allocations[i],0));
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=images[i]; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=imageInfo.format; view.subresourceRange=range;
            require(vkCreateImageView(device,&view,nullptr,&views[i]));
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image=images[i]; barrier.subresourceRange=range;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkClearColorValue clear{{.2f,.2f,.2f,1}};
            if (i==2) clear={{0,0,0,0}};
            if (i==3 || i==7) clear={{rr?5.f:.98f,0,0,0}};
            if (i==6) clear={{0,0,1,.5f}};
            vkCmdClearColorImage(cmd,images[i],VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
            barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            auto &r=resources[i]; r=sl::Resource(sl::ResourceType::eTex2d,(void*)images[i],(void*)allocations[i],(void*)views[i],VK_IMAGE_LAYOUT_GENERAL);
            r.width=imageInfo.extent.width; r.height=imageInfo.extent.height; r.nativeFormat=imageInfo.format; r.mipLevels=1; r.arrayLayers=1; r.flags=0; r.usage=imageInfo.usage;
            sl::Extent extent{0,0,r.width,r.height}; tags.emplace_back(&r,types[i],sl::ResourceLifecycle::eValidUntilEvaluate,&extent);
        }
        sl.beginFrame(1); auto frame=sl.frame();
        auto p=glm::perspective(glm::radians(70.f),1280.f/720.f,.1f,1000.f);
        auto constants=mcvr::slConstants({width,height},{0,0},glm::mat4(1),p,glm::mat4(1),true);
        if (!sl.check(sl.slSetConstants(constants,*frame,viewport),"probe constants") ||
            !sl.check(sl.slSetTagForFrame(*frame,viewport,tags.data(),uint32_t(tags.size()),(sl::CommandBuffer*)cmd),"probe tags")) throw std::runtime_error("probe inputs");
        const sl::BaseStructure *inputs[]{&viewport};
        if (!sl.check(sl.slEvaluateFeature(rr?sl::kFeatureDLSS_RR:sl::kFeatureDLSS,*frame,inputs,1,(sl::CommandBuffer*)cmd),"probe evaluate")) throw std::runtime_error("probe evaluate");
        require(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
        require(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); require(vkQueueWaitIdle(queue));
        sl.slFreeResources(rr?sl::kFeatureDLSS_RR:sl::kFeatureDLSS,viewport);
        for (unsigned i=0;i<count;++i) { vkDestroyImageView(device,views[i],nullptr); vkDestroyImage(device,images[i],nullptr); vkFreeMemory(device,allocations[i],nullptr); }
        std::cout<<"PASS: "<<(rr?"RR model D":"SR Auto")<<" evaluate + GPU completion"<<std::endl;
    }
    vkDestroyCommandPool(device,pool,nullptr);
}
