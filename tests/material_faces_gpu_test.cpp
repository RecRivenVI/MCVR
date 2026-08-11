#include "core/render/material_faces.hpp"
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
        ai.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &ai;
        VkInstance instance;
        check(vkCreateInstance(&ici, nullptr, &instance));
        volkLoadInstance(instance);
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
        di.pNext = &bf;
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
        auto build = [&](VkAccelerationStructureTypeKHR type, std::vector<VkAccelerationStructureGeometryKHR> geoms,
                         std::vector<uint32_t> sizes) {
            VkAccelerationStructureBuildGeometryInfoKHR bi{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            bi.type = type;
            bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            bi.geometryCount = uint32_t(geoms.size());
            bi.pGeometries = geoms.data();
            VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi,
                                                    sizes.data(), &sz);
            auto store = buffer(sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
            auto scratch = buffer(sz.buildScratchSize + scratchAlignment - 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            AS as{};
            VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            ci.buffer = store.handle;
            ci.size = sz.accelerationStructureSize;
            ci.type = type;
            check(vkCreateAccelerationStructureKHR(device, &ci, nullptr, &as.handle));
            bi.dstAccelerationStructure = as.handle;
            bi.scratchData.deviceAddress = (scratch.address + scratchAlignment - 1) & ~(scratchAlignment - 1);
            std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(geoms.size());
            for (size_t i = 0; i < ranges.size(); i++) ranges[i].primitiveCount = sizes[i];
            const auto *pr = ranges.data();
            submit([&] {
                VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
                host.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                                     VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &host, 0, nullptr, 0,
                                     nullptr);
                vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bi, &pr);
                VkMemoryBarrier done{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                done.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                done.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                     VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     0, 1, &done, 0, nullptr, 0, nullptr);
            });
            VkAccelerationStructureDeviceAddressInfoKHR addr{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
            addr.accelerationStructure = as.handle;
            as.address = vkGetAccelerationStructureDeviceAddressKHR(device, &addr);
            structures.push_back(as);
            return as;
        };
        std::array<float, 18> vertices = {-1, -1, 0, 1, -1, 0, 0, 1, 0, 2, -1, 0, 4, -1, 0, 3, 1, 0};
        auto vb = buffer(sizeof(vertices), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                         vertices.data());
        std::vector<std::array<uint32_t, 4>> cases;
        std::vector<VkAccelerationStructureInstanceKHR> instances;
        for (auto model : std::array<std::array<uint32_t, 2>, 5>{{{faces::back, 0},
                                                                  {faces::front, 0},
                                                                  {faces::back, faces::back},
                                                                  {faces::back | faces::clockwise, 0},
                                                                  {faces::back | faces::front, 0}}}) {
            std::vector<VkAccelerationStructureGeometryKHR> geoms;
            for (int g = 0; g < 2; g++) {
                VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                geom.flags = faces::needsAnyHit(model[g], model) ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
                auto &t = geom.geometry.triangles;
                t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                t.vertexData.deviceAddress = vb.address + g * 9 * sizeof(float);
                t.vertexStride = 12;
                t.maxVertex = 2;
                t.indexType = VK_INDEX_TYPE_NONE_KHR;
                geoms.push_back(geom);
            }
            auto blas = build(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geoms, {1, 1});
            for (int mirror = 0; mirror < 2; mirror++) {
                VkAccelerationStructureInstanceKHR i{};
                i.transform.matrix[0][0] = mirror ? -1 : 1;
                i.transform.matrix[1][1] = i.transform.matrix[2][2] = 1;
                i.transform.matrix[1][3] = float(cases.size()) * 4;
                i.mask = 255;
                i.flags = faces::instanceFlags(model, i.transform);
                i.accelerationStructureReference = blas.address;
                instances.push_back(i);
                cases.push_back({model[0], model[1], uint32_t(mirror), faces::uniform(model)});
            }
        }
        auto ib = buffer(instances.size() * sizeof(instances[0]),
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, instances.data());
        VkAccelerationStructureGeometryKHR ig{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        ig.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        ig.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        ig.geometry.instances.data.deviceAddress = ib.address;
        auto tlas = build(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, {ig}, {uint32_t(instances.size())});
        auto out = buffer(cases.size() * 4 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        auto input = buffer(cases.size() * sizeof(cases[0]), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, cases.data());
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{
            {{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
             {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
             {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 3;
        li.pBindings = bindings.data();
        VkDescriptorSetLayout layout;
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout));
        std::array<VkDescriptorPoolSize, 2> ps{
            {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = ps.data();
        VkDescriptorPool dp;
        check(vkCreateDescriptorPool(device, &dpi, nullptr, &dp));
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = dp;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &layout;
        VkDescriptorSet ds;
        check(vkAllocateDescriptorSets(device, &da, &ds));
        VkWriteDescriptorSetAccelerationStructureKHR aw{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        aw.accelerationStructureCount = 1;
        aw.pAccelerationStructures = &tlas.handle;
        std::array<VkDescriptorBufferInfo, 2> bis{{{out.handle, 0, VK_WHOLE_SIZE}, {input.handle, 0, VK_WHOLE_SIZE}}};
        std::array<VkWriteDescriptorSet, 3> ws{};
        for (int i = 0; i < 3; i++) {
            ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ws[i].dstSet = ds;
            ws[i].dstBinding = i;
            ws[i].descriptorCount = 1;
            ws[i].descriptorType = bindings[i].descriptorType;
            if (i)
                ws[i].pBufferInfo = &bis[i - 1];
            else
                ws[i].pNext = &aw;
        }
        vkUpdateDescriptorSets(device, 3, ws.data(), 0, nullptr);
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Missing shader");
        size_t bytes = size_t(file.tellg());
        file.seekg(0);
        std::vector<uint32_t> words(bytes / 4);
        file.read((char *)words.data(), bytes);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = bytes;
        smi.pCode = words.data();
        VkShaderModule sm;
        check(vkCreateShaderModule(device, &smi, nullptr, &sm));
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &layout;
        VkPipelineLayout pl;
        check(vkCreatePipelineLayout(device, &pli, nullptr, &pl));
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.layout = pl;
        pci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                     nullptr,
                     0,
                     VK_SHADER_STAGE_COMPUTE_BIT,
                     sm,
                     "main",
                     nullptr};
        VkPipeline pipeline;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline));
        submit([&] {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
            vkCmdDispatch(cmd, uint32_t(cases.size()) * 4, 1, 1);
            VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0,
                                 nullptr, 0, nullptr);
        });
        bool passed = true;
        for (size_t c = 0; c < cases.size(); c++)
            for (int g = 0; g < 2; g++)
                for (int side = 0; side < 2; side++) {
                    bool expected = faces::accepts(cases[c][g], (side == 0) != (cases[c][2] != 0));
                    uint32_t actual = ((uint32_t *)out.mapped)[c * 4 + g * 2 + side];
                    if (actual != (uint32_t(expected) | 58u)) {
                        std::cerr << "case=" << c << " geometry=" << g << " side=" << side << " expected=" << expected
                                  << " actual=" << actual << '\n';
                        passed = false;
                    }
                }
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pl, nullptr);
        vkDestroyShaderModule(device, sm, nullptr);
        vkDestroyDescriptorPool(device, dp, nullptr);
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
        for (auto as : structures) vkDestroyAccelerationStructureKHR(device, as.handle, nullptr);
        for (auto b : buffers) {
            vkUnmapMemory(device, b.memory);
            vkDestroyBuffer(device, b.handle, nullptr);
            vkFreeMemory(device, b.memory, nullptr);
        }
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        if (!passed) return 1;
        std::cout
            << "40 GPU rays: mixed opaque/double-sided, uniform hardware, front/back/both, clockwise, mirrored instances; UI primary/continuation owner rules passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
