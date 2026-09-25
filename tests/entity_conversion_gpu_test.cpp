#include "core/render/entity_conversion.hpp"
#include "core/render/entity_conversion_commands.hpp"
#include <cmath>
#include <bit>
#include <atomic>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <volk.h>
using namespace mcvr;
static std::atomic<uint32_t> validationErrors{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL validationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validationErrors;
    std::cerr << data->pMessage << '\n'; return VK_FALSE;
}
void check(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("Vulkan result " + std::to_string(r));
}
struct Buffer {
    VkBuffer handle;
    VkDeviceMemory memory;
    void *mapped;
    VkDeviceAddress address;
};
struct AS {
    VkAccelerationStructureKHR handle;
    VkDeviceAddress address;
};
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 1;
        check(volkInitialize());
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.apiVersion = VK_API_VERSION_1_4;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &ai;
        const char *debugExtension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        ici.enabledExtensionCount=1;ici.ppEnabledExtensionNames=&debugExtension;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        debug.pfnUserCallback=validationMessage;ici.pNext=&debug;
        VkInstance instance;
        check(vkCreateInstance(&ici, nullptr, &instance));
        volkLoadInstance(instance);
        VkDebugUtilsMessengerEXT messenger;
        check(vkCreateDebugUtilsMessengerEXT(instance,&debug,nullptr,&messenger));
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        VkPhysicalDevice physical{};
        for (auto d : devices) {
            VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
            VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
            VkPhysicalDeviceBufferDeviceAddressFeatures addressFeatures{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
            rq.pNext = &asFeatures;
            asFeatures.pNext = &addressFeatures;
            VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f.pNext = &rq;
            vkGetPhysicalDeviceFeatures2(d, &f);
            if (rq.rayQuery && asFeatures.accelerationStructure && addressFeatures.bufferDeviceAddress) {
                physical = d;
                break;
            }
        }
        if (!physical) {
            vkDestroyInstance(instance, nullptr);
            return 77;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> qs(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, qs.data());
        uint32_t family = 0;
        while (!(qs.at(family).queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        rq.rayQuery = VK_TRUE;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR af{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        af.accelerationStructure = VK_TRUE;
        af.pNext = &rq;
        VkPhysicalDeviceBufferDeviceAddressFeatures bf{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        bf.bufferDeviceAddress = VK_TRUE;
        bf.pNext = &af;
        const char *extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.maintenance4 = VK_TRUE; f13.pNext = &bf;
        di.pNext = &f13;
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = 3;
        di.ppEnabledExtensionNames = extensions;
        VkDevice device;
        check(vkCreateDevice(physical, &di, nullptr, &device));
        volkLoadDevice(device);
        VkQueue queue;
        vkGetDeviceQueue(device, family, 0, &queue);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(physical, &mp);
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &asProperties;
        vkGetPhysicalDeviceProperties2(physical, &properties);
        const VkDeviceSize scratchAlignment = asProperties.minAccelerationStructureScratchOffsetAlignment;
        std::vector<Buffer> buffers;
        std::vector<AS> structures;
        auto buffer = [&](VkDeviceSize bytes, VkBufferUsageFlags usage, const void *data = nullptr) {
            Buffer b{};
            VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            ci.size = bytes;
            ci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            check(vkCreateBuffer(device, &ci, nullptr, &b.handle));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device, b.handle, &req);
            uint32_t type = 0;
            constexpr auto hostFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            while (type < mp.memoryTypeCount && (!(req.memoryTypeBits & (1u << type)) ||
                                                 (mp.memoryTypes[type].propertyFlags & hostFlags) != hostFlags))
                ++type;
            if (type == mp.memoryTypeCount) throw std::runtime_error("Fixture requires host-coherent buffer memory");
            VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
            flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.pNext = &flags;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = type;
            check(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
            check(vkBindBufferMemory(device, b.handle, b.memory, 0));
            check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
            if (data) std::memcpy(b.mapped, data, bytes);
            VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            address.buffer = b.handle;
            b.address = vkGetBufferDeviceAddress(device, &address);
            buffers.push_back(b);
            return b;
        };
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = family;
        pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool pool;
        check(vkCreateCommandPool(device, &pc, nullptr, &pool));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool;
        ca.commandBufferCount = 1;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        VkCommandBuffer cmd;
        check(vkAllocateCommandBuffers(device, &ca, &cmd));
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        check(vkCreateFence(device, &fc, nullptr, &fence));
        auto submit = [&](auto record) {
            check(vkResetCommandBuffer(cmd, 0));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            check(vkBeginCommandBuffer(cmd, &begin));
            record();
            check(vkEndCommandBuffer(cmd));
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            check(vkQueueSubmit(queue, 1, &si, fence));
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000ull));
            check(vkResetFences(device, 1, &fence));
        };
        // Exercise the production admission seam, including each CPU-consumer exclusion.
        EntityConversionInput admission{12,7,260,false,false,false,false,false,false};
        if (!rawEntityConversionEligible(admission)) throw std::runtime_error("PBR quads not admitted");
        for (int exclusion=0; exclusion<6; ++exclusion) {
            auto copy=admission;
            bool *flags[] = {&copy.post,&copy.external,&copy.explicitIndices,&copy.eyeLayer,&copy.cached,&copy.prebuilt};
            *flags[exclusion]=true;
            if (rawEntityConversionEligible(copy)) throw std::runtime_error("CPU semantic consumer bypassed");
        }
        for (int mode : {0,1,2,3,5,6}) {
            auto copy=admission; copy.drawMode=mode;
            if (rawEntityConversionEligible(copy)) throw std::runtime_error("Topology incorrectly admitted");
        }
        admission.count=259;
        if (rawEntityConversionEligible(admission)) throw std::runtime_error("Incomplete quad admitted");
        admission={12,4,33,false,false,false,false,false,false};
        if (!rawEntityConversionEligible(admission)) throw std::runtime_error("PBR triangles not admitted");

        std::array<VkDescriptorSetLayoutBinding,5> bindings{};
        for (uint32_t i=0;i<5;++i) bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount=5; li.pBindings=bindings.data();
        VkDescriptorSetLayout layout; check(vkCreateDescriptorSetLayout(device,&li,nullptr,&layout));
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,10};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets=2; dpi.poolSizeCount=1; dpi.pPoolSizes=&ps;
        VkDescriptorPool poolDescriptors; check(vkCreateDescriptorPool(device,&dpi,nullptr,&poolDescriptors));
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount=1; pli.pSetLayouts=&layout; pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&push;
        VkPipelineLayout pipelineLayout; check(vkCreatePipelineLayout(device,&pli,nullptr,&pipelineLayout));
        std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
        if (!file) throw std::runtime_error("Missing production shader");
        size_t bytes=size_t(file.tellg()); file.seekg(0);
        std::vector<uint32_t> spirv(bytes/4); file.read(reinterpret_cast<char*>(spirv.data()),bytes);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize=bytes; smi.pCode=spirv.data(); VkShaderModule sm;
        check(vkCreateShaderModule(device,&smi,nullptr,&sm));
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.layout=pipelineLayout;
        pci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,sm,"main",nullptr};
        VkPipeline pipeline; check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline));
        struct Batch {
            std::vector<uint32_t> input;
            std::vector<EntityConvertJob> jobs;
            std::vector<vk::VertexFormat::PositionVertex> positions;
            std::vector<vk::VertexFormat::MaterialVertex> materials;
            std::vector<uint32_t> indices;
            std::vector<bool> usedVertices,usedIndices;
            std::array<Buffer,5> gpu;
            VkDescriptorSet descriptors;
            VkAccelerationStructureBuildGeometryInfoKHR build{};
            std::vector<VkAccelerationStructureGeometryKHR> geometries;
            std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        };
        std::array<Batch,2> batches;
        for (uint32_t generation=0;generation<2;++generation) {
            auto &b=batches[generation];
            const uint32_t counts[]={257,260,33};
            std::vector<EntityConvertJob> geometryJobs;
            uint32_t vo=1,io=1;
            for (uint32_t g=0;g<3;++g) {
                std::vector<vk::VertexFormat::PBRVertex> source(counts[g]);
                for (uint32_t i=0;i<source.size();++i) {
                    auto &v=source[i];
                    v.pos={float(i%3)+generation*10.0f,float((i/3)%3),float(i)*.01f};
                    v.norm={.5f,1.0f,-.25f}; v.useNorm=i%2;
                    v.colorLayer={.2f,-.3f,.8f,float(i%11)/10}; v.useColorLayer=i%3;
                    v.textureUV={-.25f,float(i)}; v.textureID=i%4095;
                    v.useTexture=1; v.overlayUV={int(i),-2}; v.useOverlay=i%2;
                    v.glintUV={.8f,-.75f}; v.useGlint=i%4; v.glintTexture=11+generation;
                    v.lightUV={int(i%256),240}; v.useLight=1;
                    v.alphaMode=i%25; v.coordinate=i%3; v.albedoEmission=float(i%7)/6;
                }
                EntityConvertJob job{};
                job.source=uint32_t(b.input.size());
                const size_t old=b.input.size(); b.input.resize(old+source.size()*32);
                std::memcpy(b.input.data()+old,source.data(),source.size()*128);
                job.indexSource=uint32_t(b.input.size()); job.deferred=g==0?0:1;
                job.vertexOffset=vo; job.indexOffset=io; job.vertexCount=counts[g];
                job.quadIndices=g==1; job.indexCount=g==1?counts[g]/4*6:(g==0?255:counts[g]);
                job.normalOffset=g==1; job.coordinate=2u;
                job.emissionPolicy=g==1?1:2; job.emissionBits=std::bit_cast<uint32_t>(.9f);
                job.emissiveOverlay=17+generation;
                b.positions.resize(vo+counts[g]+1); b.materials.resize(vo+counts[g]+1);
                b.usedVertices.resize(vo+counts[g]+1); b.indices.resize(io+job.indexCount+1);
                b.usedIndices.resize(io+job.indexCount+1);
                for (uint32_t i=0;i<counts[g];++i) {
                    auto vertex=source[i];
                    if (g!=0) {
                        if (job.normalOffset && vertex.useNorm) vertex.pos+=.00001f*glm::normalize(vertex.norm);
                        vertex.coordinate=2;
                        if (job.emissionPolicy&1) vertex.albedoEmission=std::max(vertex.albedoEmission,.9f);
                        if (job.emissionPolicy&2) vertex.albedoEmission=1;
                    }
                    b.positions[vo+i]=vk::Vertex::makePositionVertex(vertex);
                    b.materials[vo+i]=vk::Vertex::makeMaterialVertex(vertex);
                    b.materials[vo+i].emissiveOverlayTextureID=job.emissiveOverlay;
                    b.usedVertices[vo+i]=true;
                }
                const uint32_t pattern[]={0,1,2,2,3,0};
                for (uint32_t i=0;i<job.indexCount;++i) {
                    uint32_t value=g==1?(i/6)*4+pattern[i%6]:i;
                    if (g==0) {value=254-i; b.input.push_back(value);}
                    b.indices[io+i]=value; b.usedIndices[io+i]=true;
                }
                appendEntityTiles(b.jobs,job); geometryJobs.push_back(job);
                vo+=counts[g]+2; io+=job.indexCount+2;
            }
            b.gpu={buffer(b.input.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,b.input.data()),
                buffer(b.jobs.size()*sizeof(EntityConvertJob),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,b.jobs.data()),
                buffer(b.positions.size()*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR),
                buffer(b.materials.size()*80,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
                buffer(b.indices.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR)};
            std::memset(b.gpu[2].mapped,0xcd,b.positions.size()*16);
            std::memset(b.gpu[3].mapped,0xcd,b.materials.size()*80);
            std::memset(b.gpu[4].mapped,0xcd,b.indices.size()*4);
            VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            da.descriptorPool=poolDescriptors; da.descriptorSetCount=1; da.pSetLayouts=&layout;
            check(vkAllocateDescriptorSets(device,&da,&b.descriptors));
            std::array<VkDescriptorBufferInfo,5> bis{};
            std::array<VkWriteDescriptorSet,5> ws{};
            for(uint32_t i=0;i<5;++i) {
                bis[i]={b.gpu[i].handle,0,VK_WHOLE_SIZE};
                ws[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; ws[i].dstSet=b.descriptors;
                ws[i].dstBinding=i; ws[i].descriptorCount=1; ws[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                ws[i].pBufferInfo=&bis[i];
            }
            vkUpdateDescriptorSets(device,5,ws.data(),0,nullptr);
            std::vector<uint32_t> primitives;
            for(auto j:geometryJobs) {
                VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                geom.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                auto &t=geom.geometry.triangles; t.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                t.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT; t.vertexData.deviceAddress=b.gpu[2].address+j.vertexOffset*16;
                t.vertexStride=16; t.maxVertex=j.vertexCount-1; t.indexType=VK_INDEX_TYPE_UINT32;
                t.indexData.deviceAddress=b.gpu[4].address+j.indexOffset*4;
                b.geometries.push_back(geom); primitives.push_back(j.indexCount/3);
                b.ranges.push_back({j.indexCount/3,0,0,0});
            }
            b.build={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            b.build.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; b.build.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            b.build.geometryCount=uint32_t(b.geometries.size()); b.build.pGeometries=b.geometries.data();
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&b.build,primitives.data(),&sizes);
            auto storage=buffer(sizes.accelerationStructureSize,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
            auto scratch=buffer(sizes.buildScratchSize+scratchAlignment-1,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            AS as{}; VkAccelerationStructureCreateInfoKHR ac{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            ac.buffer=storage.handle; ac.size=sizes.accelerationStructureSize; ac.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            check(vkCreateAccelerationStructureKHR(device,&ac,nullptr,&as.handle)); structures.push_back(as);
            b.build.dstAccelerationStructure=as.handle;
            b.build.scratchData.deviceAddress=(scratch.address+scratchAlignment-1)&~(scratchAlignment-1);
        }
        submit([&] {
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT; host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&host,0,nullptr,0,nullptr);
            for(auto &b:batches) {
                // maxGroups=2 deliberately exercises multi-dispatch tiles and push offsets.
                recordEntityConversion(cmd,pipeline,pipelineLayout,b.descriptors,uint32_t(b.jobs.size()),2,
                    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                const auto *range=b.ranges.data(); vkCmdBuildAccelerationStructuresKHR(cmd,1,&b.build,&range);
            }
            VkMemoryBarrier read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            read.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; read.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&read,0,nullptr,0,nullptr);
        });
        for (const auto &b:batches) {
            for(size_t i=0;i<b.positions.size();++i) {
                auto p=static_cast<const vk::VertexFormat::PositionVertex*>(b.gpu[2].mapped)+i;
                auto m=static_cast<const vk::VertexFormat::MaterialVertex*>(b.gpu[3].mapped)+i;
                if(b.usedVertices[i]) {
                    for(int k=0;k<3;++k) if(std::abs(p->pos[k]-b.positions[i].pos[k])>2e-6f)
                        throw std::runtime_error("GPU position mismatch");
                    if(p->pad0 || std::memcmp(m,&b.materials[i],80)) throw std::runtime_error("GPU packed material mismatch at "+std::to_string(i));
                } else {
                    for(size_t byte=0;byte<16;++byte) if(reinterpret_cast<const unsigned char*>(p)[byte]!=0xcd) throw std::runtime_error("Position guard overwritten");
                    for(size_t byte=0;byte<80;++byte) if(reinterpret_cast<const unsigned char*>(m)[byte]!=0xcd) throw std::runtime_error("Material guard overwritten");
                }
            }
            for(size_t i=0;i<b.indices.size();++i)
                if(static_cast<uint32_t*>(b.gpu[4].mapped)[i]!=(b.usedIndices[i]?b.indices[i]:0xcdcdcdcdu))
                    throw std::runtime_error("GPU index/guard mismatch");
        }
        vkDestroyPipeline(device,pipeline,nullptr); vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
        vkDestroyShaderModule(device,sm,nullptr); vkDestroyDescriptorPool(device,poolDescriptors,nullptr);
        vkDestroyDescriptorSetLayout(device,layout,nullptr);
        for(auto as:structures) vkDestroyAccelerationStructureKHR(device,as.handle,nullptr);
        for(auto b:buffers) {vkUnmapMemory(device,b.memory);vkDestroyBuffer(device,b.handle,nullptr);vkFreeMemory(device,b.memory,nullptr);}
        vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,pool,nullptr);
        vkDestroyDevice(device,nullptr);vkDestroyDebugUtilsMessengerEXT(instance,messenger,nullptr);vkDestroyInstance(instance,nullptr);
        if(validationErrors) throw std::runtime_error("Vulkan validation reported errors");
        std::cout<<"Production GPU PBR conversion: CPU packing parity, deferred quads/triangles, guards, two immutable batches, split dispatch, compute-to-BLAS passed\n";
        return 0;
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
