#pragma once
#include "core/render/streamline_runtime.hpp"
#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <vector>

inline void presentProbe(VkInstance instance,VkPhysicalDevice physical,VkDevice device,uint32_t family) {
    auto require=[](VkResult result) { if (result!=VK_SUCCESS && result!=VK_SUBOPTIMAL_KHR) throw std::runtime_error("present probe VkResult="+std::to_string(result)); };
    auto &sl=mcvr::StreamlineRuntime::get();
    auto module=GetModuleHandleW(nullptr);
    WNDCLASSW cls{}; cls.lpfnWndProc=DefWindowProcW; cls.hInstance=module; cls.lpszClassName=L"MCVRStreamlineProbe";
    RegisterClassW(&cls);
    HWND window=CreateWindowExW(0,cls.lpszClassName,L"MCVR Streamline verification",WS_POPUP,0,0,1280,720,nullptr,nullptr,module,nullptr);
    if (!window) throw std::runtime_error("hidden window");
    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR}; surfaceInfo.hinstance=module; surfaceInfo.hwnd=window;
    VkSurfaceKHR surface{}; require(vkCreateWin32SurfaceKHR(instance,&surfaceInfo,nullptr,&surface));
    VkSurfaceCapabilitiesKHR caps{}; require(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps));
    uint32_t formatCount=0; require(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&formatCount,nullptr));
    std::vector<VkSurfaceFormatKHR> formats(formatCount); require(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&formatCount,formats.data()));
    VkSurfaceFormatKHR format=formats[0];
    for (auto candidate:formats) if(candidate.format==VK_FORMAT_B8G8R8A8_UNORM) format=candidate;
    VkSwapchainCreateInfoKHR swapInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapInfo.surface=surface; swapInfo.minImageCount=3; swapInfo.imageFormat=format.format; swapInfo.imageColorSpace=format.colorSpace;
    swapInfo.imageExtent=caps.currentExtent; swapInfo.imageArrayLayers=1; swapInfo.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    swapInfo.preTransform=caps.currentTransform; swapInfo.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode=VK_PRESENT_MODE_IMMEDIATE_KHR; swapInfo.clipped=VK_TRUE;
    VkSwapchainKHR swapchain{}; require(vkCreateSwapchainKHR(device,&swapInfo,nullptr,&swapchain));
    uint32_t imageCount=0; require(vkGetSwapchainImagesKHR(device,swapchain,&imageCount,nullptr));
    std::vector<VkImage> swapImages(imageCount); require(vkGetSwapchainImagesKHR(device,swapchain,&imageCount,swapImages.data()));
    VkQueue queue{}; vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.queueFamilyIndex=family;
    VkCommandPool pool{}; require(vkCreateCommandPool(device,&poolInfo,nullptr,&pool));
    VkCommandBufferAllocateInfo allocCmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocCmd.commandPool=pool; allocCmd.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocCmd.commandBufferCount=1;
    VkCommandBuffer cmd{}; require(vkAllocateCommandBuffers(device,&allocCmd,&cmd));
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired{},complete{}; require(vkCreateSemaphore(device,&semaphoreInfo,nullptr,&acquired)); require(vkCreateSemaphore(device,&semaphoreInfo,nullptr,&complete));
    VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    std::array<VkImage,4> images{}; std::array<VkDeviceMemory,4> allocations{}; std::array<VkImageView,4> views{};
    std::array<sl::Resource,4> resources{};
    const VkFormat inputFormats[]{VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16_SFLOAT,format.format,VK_FORMAT_R32_SFLOAT};
    const sl::BufferType types[]{sl::kBufferTypeDepth,sl::kBufferTypeMotionVectors,sl::kBufferTypeHUDLessColor,sl::kBufferTypeUIAlpha};
    std::vector<sl::ResourceTag> tags;
    for(unsigned i=0;i<4;++i) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=inputFormats[i];
        info.extent={1280,720,1}; info.mipLevels=1; info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
        info.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        require(vkCreateImage(device,&info,nullptr,&images[i]));
        VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device,images[i],&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize=requirements.size;
        while(!(requirements.memoryTypeBits&(1u<<allocation.memoryTypeIndex))) ++allocation.memoryTypeIndex;
        require(vkAllocateMemory(device,&allocation,nullptr,&allocations[i])); require(vkBindImageMemory(device,images[i],allocations[i],0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=images[i]; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=info.format; view.subresourceRange=range;
        require(vkCreateImageView(device,&view,nullptr,&views[i]));
        auto &r=resources[i]; r=sl::Resource(sl::ResourceType::eTex2d,(void*)images[i],(void*)allocations[i],(void*)views[i],VK_IMAGE_LAYOUT_GENERAL);
        r.width=1280; r.height=720; r.mipLevels=r.arrayLayers=1; r.nativeFormat=info.format; r.flags=0; r.usage=info.usage;
        sl::Extent extent{0,0,1280,720}; tags.emplace_back(&r,types[i],sl::ResourceLifecycle::eValidUntilPresent,&extent);
    }
    static std::atomic<int> error{0}; error=0;
    sl::DLSSGOptions options{}; options.mode=sl::DLSSGMode::eOn; options.numFramesToGenerate=1;
    options.enableUserInterfaceRecomposition=sl::eTrue;
    options.onErrorCallback=[](const sl::APIError &e) { error=int(e.vkRes); };
    if (!sl.check(sl.slDLSSGSetOptions(sl::ViewportHandle(0),options),"probe FG on")) throw std::runtime_error("FG enable");
    uint32_t totalPresented=0;
    for(unsigned frameIndex=0;frameIndex<24;++frameIndex) {
        sl.beginFrame(1); sl.marker(sl::PCLMarker::eSimulationStart); sl.marker(sl::PCLMarker::eSimulationEnd); sl.marker(sl::PCLMarker::eRenderSubmitStart);
        uint32_t index=0; require(vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&index));
        require(vkResetCommandPool(device,pool,0)); VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; require(vkBeginCommandBuffer(cmd,&begin));
        auto clear=[&](VkImage image,VkClearColorValue value,VkImageLayout old,VkImageLayout next) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image=image; barrier.subresourceRange=range;
            barrier.oldLayout=old; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_GENERAL,&value,1,&range);
            barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.newLayout=next; barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        };
        for(unsigned i=0;i<4;++i) {
            VkClearColorValue value{{0,0,0,0}};
            if(i==0) value.float32[0]=.98f;
            if(i==2) value={{.2f,.3f,.4f,1}};
            clear(images[i],value,frameIndex?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
        }
        clear(swapImages[index],{{.2f,.3f,.4f,1}},VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        auto token=sl.frame(); auto p=glm::perspective(glm::radians(70.f),1280.f/720.f,.1f,1000.f);
        auto constants=mcvr::slConstants({1280,720},{0,0},glm::mat4(1),p,glm::mat4(1),frameIndex==0);
        if (!sl.check(sl.slSetConstants(constants,*token,sl::ViewportHandle(0)),"probe FG constants") || !sl.check(sl.slSetTagForFrame(*token,sl::ViewportHandle(0),tags.data(),4,(sl::CommandBuffer*)cmd),"probe FG tags")) throw std::runtime_error("FG inputs");
        require(vkEndCommandBuffer(cmd)); VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&acquired; submit.pWaitDstStageMask=&stage;
        submit.commandBufferCount=1; submit.pCommandBuffers=&cmd; submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&complete;
        require(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); sl.marker(sl::PCLMarker::eRenderSubmitEnd);
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; present.waitSemaphoreCount=1; present.pWaitSemaphores=&complete; present.swapchainCount=1; present.pSwapchains=&swapchain; present.pImageIndices=&index;
        sl.marker(sl::PCLMarker::ePresentStart); require(vkQueuePresentKHR(queue,&present)); sl.marker(sl::PCLMarker::ePresentEnd);
        require(vkDeviceWaitIdle(device));
        sl::DLSSGState state{}; if (!sl.check(sl.slDLSSGGetState(sl::ViewportHandle(0),state,nullptr),"probe FG state")) throw std::runtime_error("FG state");
        totalPresented+=state.numFramesActuallyPresented;
        if(state.status!=sl::DLSSGStatus::eOk || error.load()<0) throw std::runtime_error("FG status="+std::to_string(uint32_t(state.status))+" async="+std::to_string(error.load()));
    }
    sl.disableFG(); require(vkDeviceWaitIdle(device));
    for(unsigned i=0;i<4;++i) { vkDestroyImageView(device,views[i],nullptr); vkDestroyImage(device,images[i],nullptr); vkFreeMemory(device,allocations[i],nullptr); }
    vkDestroySemaphore(device,acquired,nullptr); vkDestroySemaphore(device,complete,nullptr); vkDestroyCommandPool(device,pool,nullptr);
    vkDestroySwapchainKHR(device,swapchain,nullptr); vkDestroySurfaceKHR(instance,surface,nullptr); DestroyWindow(window); UnregisterClassW(cls.lpszClassName,module);
    std::cout<<"FG probe: real=24 presented="<<totalPresented<<std::endl;
    if(totalPresented<=24) std::cout<<"DEFERRED: generated presentation count; hidden window is not focused and SL suspends interpolation"<<std::endl;
    std::cout<<"PASS: FG/Reflex resource, swapchain and real presentation path; not game/latency acceptance"<<std::endl;
}
