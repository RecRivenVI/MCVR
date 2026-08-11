#include "core/diagnostics/gpu_profile.hpp"
#include <stdexcept>
#include <string>
#include <vector>
static void require(bool c){if(!c)throw std::runtime_error("GPU query observer contract");}
static int creates=0,resets=0,writes=0,destroys=0;
static bool available=true;
static VkResult injected=VK_SUCCESS;
static std::vector<std::string> labels;
static VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice,const VkQueryPoolCreateInfo *info,const VkAllocationCallbacks *,VkQueryPool *pool){
    require(info->queryCount==192);++creates;*pool=(VkQueryPool)1;return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL reset(VkCommandBuffer,VkQueryPool,uint32_t first,uint32_t count){require(first==0 && count==192);++resets;}
static VKAPI_ATTR void VKAPI_CALL write(VkCommandBuffer,VkPipelineStageFlagBits,VkQueryPool,uint32_t){++writes;}
static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice,VkQueryPool,const VkAllocationCallbacks *){++destroys;}
static VKAPI_ATTR VkResult VKAPI_CALL results(VkDevice,VkQueryPool,uint32_t,uint32_t count,size_t bytes,void *data,VkDeviceSize stride,VkQueryResultFlags flags){
    if(injected!=VK_SUCCESS)return injected;
    require(!(flags&VK_QUERY_RESULT_WAIT_BIT) && stride==16 && bytes==count*16);
    auto values=static_cast<uint64_t*>(data);
    for(uint32_t i=0;i<count;++i){values[i*2]=100+i*20;values[i*2+1]=available;}
    return available ? VK_SUCCESS : VK_NOT_READY;
}
PFN_vkCreateQueryPool vkCreateQueryPool=create;
PFN_vkCmdResetQueryPool vkCmdResetQueryPool=reset;
PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp=write;
PFN_vkDestroyQueryPool vkDestroyQueryPool=destroy;
PFN_vkGetQueryPoolResults vkGetQueryPoolResults=results;
static void sample(uint64_t owner,uint32_t domain,const char *label,uint64_t ns,uint64_t){
    labels.emplace_back(label);
    if(domain==3)require(owner==22 && ns==40);
}
int main(){
    const McvrProfileSink sink{MCVR_PROFILE_ABI,sizeof(McvrProfileSink),sample};require(mcvr::profile::install(&sink));
    mcvr::profile::GpuFrame gpu;
    gpu.reset({}, {},true);require(creates==0);
    mcvr::profile::active=true;gpu.reset({}, {},true);
    auto slot=gpu.begin({},"world",3);gpu.end({},slot);gpu.owner=22;gpu.submitted=true;
    gpu.collect({},64,2);require(labels.back()=="world" && writes==2 && !gpu.submitted);
    gpu.reset({}, {},true);require(creates==1 && resets==2);
    gpu.end({},gpu.begin({},"pending",3));gpu.owner=22;gpu.submitted=true;available=false;
    gpu.collect({},64,2);require(labels.back()=="gpu-query-unavailable");
    gpu.invalidate();require(gpu.begin({},"discarded")==-1 && !gpu.submitted);
    gpu.submitted=true;injected=VK_ERROR_DEVICE_LOST;
    gpu.collect({},64,2);require(gpu.lastResult==VK_ERROR_DEVICE_LOST); // Owner must propagate the real fault.
    gpu.close({});require(destroys==1);
}
