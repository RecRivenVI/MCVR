#pragma once
#include "frame_profile.hpp"
#include <volk.h>
#include <array>
#include <cstring>
#include <limits>

namespace mcvr::profile {
// One pool per acquired image. Reset/reuse ONLY after its existing frame fence.
// No additional fence, idle or WAIT query; resources follow FrameworkContext lifetime.
struct GpuFrame {
    static constexpr uint32_t Capacity = 96;
    struct Span { char label[80]{}; uint32_t domain=2; bool ended=false; };
    VkQueryPool pool = VK_NULL_HANDLE;
    std::array<Span, Capacity> spans{};
    uint32_t count=0;
    uint64_t owner=0;
    bool recording=false, submitted=false, unavailable=false;
    VkResult lastResult=VK_SUCCESS;
    void close(VkDevice device) noexcept { if(pool) vkDestroyQueryPool(device,pool,nullptr); pool=VK_NULL_HANDLE; }
    void reset(VkDevice device, VkCommandBuffer upload, bool supported) noexcept {
        recording=false; submitted=false; count=0; owner=0; lastResult=VK_SUCCESS;
        if(!enabled() || !supported || unavailable) return;
        if(!pool) {
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType=VK_QUERY_TYPE_TIMESTAMP; info.queryCount=Capacity*2;
            auto result=vkCreateQueryPool(device,&info,nullptr,&pool);
            lastResult=result;
            if(result!=VK_SUCCESS) { unavailable=true; emit(frame,8,"gpu-query-pool-unavailable",1,0); return; }
        }
        vkCmdResetQueryPool(upload,pool,0,Capacity*2);
        recording=true;
    }
    int begin(VkCommandBuffer command, const char *label, uint32_t domain=2) noexcept {
        if(!recording) return -1;
        if(count==Capacity) { emit(frame,8,"gpu-span-capacity",1,0); return -1; }
        uint32_t slot=count++;
        auto &span=spans[slot]; span={}; span.domain=domain;
        std::strncpy(span.label,label,sizeof(span.label)-1);
        vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool,slot*2);
        return static_cast<int>(slot);
    }
    void end(VkCommandBuffer command, int slot) noexcept {
        if(!recording || slot<0) return;
        vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool,slot*2+1);
        spans[slot].ended=true;
    }
    void invalidate() noexcept {
        if(recording) emit(frame,8,"gpu-partial-readback-omitted",1,0);
        recording=false; submitted=false;
    }
    void collect(VkDevice device, uint32_t validBits, double period) noexcept {
        lastResult=VK_SUCCESS;
        if(!submitted || !pool) return;
        submitted=false;
        std::array<uint64_t,Capacity*4> results{}; // value, availability, value, availability
        auto result=vkGetQueryPoolResults(device,pool,0,count*2,count*4*sizeof(uint64_t),results.data(),
            2*sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        lastResult=result;
        if(result!=VK_SUCCESS && result!=VK_NOT_READY) { emit(owner,8,"gpu-query-read-failed",1,0); return; }
        uint64_t mask=validBits>=64 ? UINT64_MAX : (uint64_t{1}<<validBits)-1;
        for(uint32_t i=0;i<count;++i) {
            if(!spans[i].ended || !results[i*4+1] || !results[i*4+3]) { emit(owner,8,"gpu-query-unavailable",1,0); continue; }
            auto ns=static_cast<uint64_t>(((results[i*4+2]-results[i*4])&mask)*period);
            emit(owner,spans[i].domain,spans[i].label,ns,ns);
        }
    }
};
}
