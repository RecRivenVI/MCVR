#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int SKIP_RETURN_CODE = 77;
constexpr uint32_t HISTOGRAM_BIN_COUNT = 256;
constexpr uint32_t TEST_BIN = 224;
constexpr uint32_t TEST_SAMPLE_COUNT = 4096;

struct ToneMappingModuleExposureData {
    float exposure;
    float avgLogLum;
    uint32_t historyValid;
    uint32_t padding;
};

struct ToneMappingModulePushConstant {
    float log2Min;
    float log2Max;
    float epsilon;
    float lowPercent;
    float highPercent;
    float middleGrey;
    float dt;
    float speedUp;
    float speedDown;
    float minExposure;
    float maxExposure;
    float manualExposure;
    float exposureBias;
    float whitePoint;
    float saturation;
    int32_t toneMappingMethod;
    int32_t autoExposure;
    int32_t clampOutput;
    int32_t exposureMeteringMode;
    float centerMeteringPercent;
    float padding0;
    float padding1;
    float padding2;
};

static_assert(sizeof(ToneMappingModuleExposureData) == 16);
static_assert(offsetof(ToneMappingModuleExposureData, historyValid) == 8);
static_assert(sizeof(ToneMappingModulePushConstant) == 92);
static_assert(offsetof(ToneMappingModulePushConstant, exposureMeteringMode) == 72);
static_assert(offsetof(ToneMappingModulePushConstant, centerMeteringPercent) == 76);

class SkipError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

void checked(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult=" + std::to_string(result));
    }
}

std::vector<uint32_t> readSpirv(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open()) { throw std::runtime_error("Cannot open exposure shader: " + path.string()); }
    const auto length = input.tellg();
    if (length <= 0 || length % static_cast<std::streamoff>(sizeof(uint32_t)) != 0) {
        throw std::runtime_error("Exposure shader is not valid word-aligned SPIR-V: " + path.string());
    }
    std::vector<uint32_t> words(static_cast<size_t>(length) / sizeof(uint32_t));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(words.data()), static_cast<std::streamsize>(length));
    if (!input) { throw std::runtime_error("Cannot read exposure shader: " + path.string()); }
    return words;
}

bool near(float actual, float expected, float absoluteTolerance = 2.0e-5f) {
    return std::isfinite(actual) && std::abs(actual - expected) <= absoluteTolerance;
}

void require(bool condition, const std::string &message) {
    if (!condition) { throw std::runtime_error(message); }
}

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;
};

class ExposureHarness {
  public:
    explicit ExposureHarness(const std::filesystem::path &shaderPath) {
        createInstance();
        selectDevice();
        createDevice();
        histogram_ = createBuffer(HISTOGRAM_BIN_COUNT * sizeof(uint32_t));
        exposure_ = createBuffer(sizeof(ToneMappingModuleExposureData));
        createDescriptors();
        createPipeline(shaderPath);
        createCommands();
    }

    ~ExposureHarness() {
        if (device_ != VK_NULL_HANDLE) { vkDeviceWaitIdle(device_); }
        if (device_ != VK_NULL_HANDLE && fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
        if (device_ != VK_NULL_HANDLE && commandPool_ != VK_NULL_HANDLE)
            vkDestroyCommandPool(device_, commandPool_, nullptr);
        if (device_ != VK_NULL_HANDLE && pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (device_ != VK_NULL_HANDLE && shader_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, shader_, nullptr);
        if (device_ != VK_NULL_HANDLE && pipelineLayout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (device_ != VK_NULL_HANDLE && descriptorPool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (device_ != VK_NULL_HANDLE && descriptorSetLayout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        destroyBuffer(exposure_);
        destroyBuffer(histogram_);
        if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
        if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    }

    ToneMappingModuleExposureData dispatch(const std::array<uint32_t, HISTOGRAM_BIN_COUNT> &histogram,
                                           ToneMappingModuleExposureData exposure,
                                           const ToneMappingModulePushConstant &pushConstant) {
        std::memcpy(histogram_.mapped, histogram.data(), histogram_.size);
        std::memcpy(exposure_.mapped, &exposure, sizeof(exposure));

        checked(vkResetFences(device_, 1, &fence_), "vkResetFences");
        checked(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(commandBuffer_, &beginInfo), "vkBeginCommandBuffer");

        std::array<VkBufferMemoryBarrier, 2> hostWrites{};
        hostWrites[0] = bufferBarrier(histogram_, VK_ACCESS_HOST_WRITE_BIT,
                                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        hostWrites[1] = bufferBarrier(exposure_, VK_ACCESS_HOST_WRITE_BIT,
                                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, static_cast<uint32_t>(hostWrites.size()), hostWrites.data(), 0, nullptr);

        vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1,
                                &descriptorSet_, 0, nullptr);
        vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(pushConstant), &pushConstant);
        vkCmdDispatch(commandBuffer_, 1, 1, 1);

        VkBufferMemoryBarrier hostRead =
            bufferBarrier(exposure_, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                             0, nullptr, 1, &hostRead, 0, nullptr);
        checked(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        checked(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");
        checked(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");

        ToneMappingModuleExposureData result{};
        std::memcpy(&result, exposure_.mapped, sizeof(result));
        return result;
    }

    const VkPhysicalDeviceProperties &properties() const { return properties_; }

  private:
    void createInstance() {
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        if (enumerateVersion != nullptr) checked(enumerateVersion(&loaderVersion), "vkEnumerateInstanceVersion");
        if (loaderVersion < VK_API_VERSION_1_4) { throw SkipError("Vulkan 1.4 loader is unavailable"); }

        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "MCVR exposure GPU test";
        application.applicationVersion = 1;
        application.pEngineName = "MCVR test";
        application.engineVersion = 1;
        application.apiVersion = VK_API_VERSION_1_4;
        VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        createInfo.pApplicationInfo = &application;
        VkResult result = vkCreateInstance(&createInfo, nullptr, &instance_);
        if (result == VK_ERROR_INCOMPATIBLE_DRIVER) { throw SkipError("Vulkan 1.4 instance is unavailable"); }
        checked(result, "vkCreateInstance");
    }

    void selectDevice() {
        uint32_t deviceCount = 0;
        checked(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "vkEnumeratePhysicalDevices(count)");
        if (deviceCount == 0) { throw SkipError("No Vulkan physical device is available"); }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        checked(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices");

        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceFeatures features{};
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceFeatures(candidate, &features);
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (!features.shaderFloat64 || properties.apiVersion < VK_API_VERSION_1_4) continue;

            uint32_t queueCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queueCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
            for (uint32_t i = 0; i < queueCount; ++i) {
                if (queues[i].queueCount != 0 && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
                    physicalDevice_ = candidate;
                    queueFamily_ = i;
                    properties_ = properties;
                    return;
                }
            }
        }
        throw SkipError("No Vulkan 1.4 compute device with shaderFloat64 is available");
    }

    void createDevice() {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures features{};
        features.shaderFloat64 = VK_TRUE;
        VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;
        createInfo.pEnabledFeatures = &features;
        checked(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "vkCreateDevice");
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    }

    Buffer createBuffer(VkDeviceSize size) {
        Buffer result{.size = size};
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checked(vkCreateBuffer(device_, &bufferInfo, nullptr, &result.buffer), "vkCreateBuffer");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, result.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);
        uint32_t memoryType = UINT32_MAX;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags & required) == required) {
                memoryType = i;
                break;
            }
        }
        if (memoryType == UINT32_MAX) { throw SkipError("No host-visible coherent storage-buffer memory type"); }

        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        checked(vkAllocateMemory(device_, &allocation, nullptr, &result.memory), "vkAllocateMemory");
        checked(vkBindBufferMemory(device_, result.buffer, result.memory, 0), "vkBindBufferMemory");
        checked(vkMapMemory(device_, result.memory, 0, size, 0, &result.mapped), "vkMapMemory");
        return result;
    }

    void destroyBuffer(Buffer &buffer) {
        if (device_ == VK_NULL_HANDLE) return;
        if (buffer.mapped != nullptr) vkUnmapMemory(device_, buffer.memory);
        if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffer.buffer, nullptr);
        if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(device_, buffer.memory, nullptr);
        buffer = {};
    }

    void createDescriptors() {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        bindings[1] = {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        checked(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_),
                "vkCreateDescriptorSetLayout");

        VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        checked(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool");

        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptorPool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &descriptorSetLayout_;
        checked(vkAllocateDescriptorSets(device_, &allocate, &descriptorSet_), "vkAllocateDescriptorSets");

        VkDescriptorBufferInfo histogramInfo{.buffer = histogram_.buffer, .offset = 0, .range = histogram_.size};
        VkDescriptorBufferInfo exposureInfo{.buffer = exposure_.buffer, .offset = 0, .range = exposure_.size};
        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet_, 1, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &histogramInfo, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet_, 2, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &exposureInfo, nullptr};
        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    void createPipeline(const std::filesystem::path &shaderPath) {
        const std::vector<uint32_t> code = readSpirv(shaderPath);
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = code.size() * sizeof(uint32_t);
        shaderInfo.pCode = code.data();
        checked(vkCreateShaderModule(device_, &shaderInfo, nullptr, &shader_), "vkCreateShaderModule");

        VkPushConstantRange pushRange{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                      .offset = 0, .size = sizeof(ToneMappingModulePushConstant)};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout_;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        checked(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_),
                "vkCreatePipelineLayout");

        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader_;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = stage;
        pipelineInfo.layout = pipelineLayout_;
        checked(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_),
                "vkCreateComputePipelines");
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

    static VkBufferMemoryBarrier bufferBarrier(const Buffer &buffer, VkAccessFlags source, VkAccessFlags target) {
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = source;
        barrier.dstAccessMask = target;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer.buffer;
        barrier.offset = 0;
        barrier.size = buffer.size;
        return barrier;
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    uint32_t queueFamily_ = UINT32_MAX;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    Buffer histogram_{};
    Buffer exposure_{};
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkShaderModule shader_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

ToneMappingModulePushConstant testPushConstant() {
    ToneMappingModulePushConstant result{};
    result.log2Min = -12.0f;
    result.log2Max = 4.0f;
    result.epsilon = 1.0e-6f;
    result.lowPercent = 0.005f;
    result.highPercent = 0.99f;
    result.middleGrey = 0.18f;
    result.dt = 0.25f;
    result.speedUp = 3.0f;
    result.speedDown = 2.0f;
    result.minExposure = 1.0e-4f;
    result.maxExposure = 1.2f;
    result.manualExposure = 0.75f;
    result.whitePoint = 11.2f;
    result.saturation = 1.0f;
    result.toneMappingMethod = 3;
    result.autoExposure = 1;
    result.clampOutput = 1;
    result.exposureMeteringMode = 0;
    result.centerMeteringPercent = 0.2f;
    return result;
}

void runCases(ExposureHarness &harness) {
    const ToneMappingModulePushConstant pc = testPushConstant();
    std::array<uint32_t, HISTOGRAM_BIN_COUNT> histogram{};
    histogram[TEST_BIN] = TEST_SAMPLE_COUNT;

    const float t = (static_cast<float>(TEST_BIN) + 0.5f) / static_cast<float>(HISTOGRAM_BIN_COUNT);
    const float expectedAvgLogLum = pc.log2Min + t * (pc.log2Max - pc.log2Min);
    const float targetExposure = std::clamp(pc.middleGrey / std::max(std::exp2(expectedAvgLogLum), 1.0e-6f),
                                            pc.minExposure, pc.maxExposure);

    const auto first = harness.dispatch(histogram, {1000000.0f, -99.0f, 0, 0}, pc);
    require(first.historyValid == 1, "First populated histogram did not establish valid exposure history");
    require(near(first.avgLogLum, expectedAvgLogLum), "First-frame average log luminance is wrong");
    require(near(first.exposure, targetExposure), "First frame smoothed from invalid seeded exposure");

    const float previousExposure = 0.02f;
    const float speed = targetExposure > previousExposure ? pc.speedUp : pc.speedDown;
    const float k = 1.0f - std::exp(-pc.dt * speed);
    const float expectedAdapted = std::clamp(
        previousExposure + (targetExposure - previousExposure) * std::clamp(k, 0.0f, 1.0f),
        pc.minExposure, pc.maxExposure);
    const auto adapted = harness.dispatch(histogram, {previousExposure, 0.0f, 1, 0}, pc);
    require(adapted.historyValid == 1, "Valid exposure history was lost");
    require(near(adapted.exposure, expectedAdapted), "Valid history did not use exponential adaptation");

    auto rebuiltPc = pc;
    rebuiltPc.dt = 0.0f;
    const auto inherited = harness.dispatch(histogram, {previousExposure, 0.0f, 1, 0}, rebuiltPc);
    require(inherited.historyValid == 1 && near(inherited.exposure, previousExposure),
            "First rebuilt frame changed inherited exposure while excluding rebuild time");

    const auto nanHistory = harness.dispatch(
        histogram, {std::numeric_limits<float>::quiet_NaN(), 0.0f, 1, 0}, pc);
    const auto infinityHistory = harness.dispatch(
        histogram, {std::numeric_limits<float>::infinity(), 0.0f, 1, 0}, pc);
    require(nanHistory.historyValid == 1 && near(nanHistory.exposure, targetExposure),
            "NaN history polluted first valid exposure");
    require(infinityHistory.historyValid == 1 && near(infinityHistory.exposure, targetExposure),
            "Infinite history polluted first valid exposure");

    histogram.fill(0);
    const float stableExposure = 0.25f;
    const auto emptyWithHistory = harness.dispatch(histogram, {stableExposure, 2.0f, 1, 0}, pc);
    require(emptyWithHistory.historyValid == 1, "Empty histogram discarded valid history");
    require(near(emptyWithHistory.exposure, stableExposure), "Empty histogram changed valid exposure");

    const auto emptyWithoutHistory = harness.dispatch(histogram, {1000000.0f, 2.0f, 0, 0}, pc);
    require(emptyWithoutHistory.historyValid == 0, "Empty histogram incorrectly established exposure history");
    require(std::isfinite(emptyWithoutHistory.exposure) && emptyWithoutHistory.exposure >= pc.minExposure &&
                emptyWithoutHistory.exposure <= pc.maxExposure,
            "Empty histogram left invalid exposure data");

    std::cout << "[PASS] first target=" << first.exposure << " avgLogLum=" << first.avgLogLum
              << " historyValid=" << first.historyValid << '\n';
    std::cout << "[PASS] adapted previous=" << previousExposure << " expected=" << expectedAdapted
              << " actual=" << adapted.exposure << '\n';
    std::cout << "[PASS] inherited first-frame exposure=" << inherited.exposure << '\n';
    std::cout << "[PASS] invalid history nan=" << nanHistory.exposure
              << " inf=" << infinityHistory.exposure << '\n';
    std::cout << "[PASS] empty valid=" << emptyWithHistory.exposure
              << " empty invalid seed=" << emptyWithoutHistory.exposure
              << " historyValid=" << emptyWithoutHistory.historyValid << '\n';
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "[FAIL] expected absolute exposure shader path\n";
        return 1;
    }
    try {
        const std::filesystem::path shaderPath = std::filesystem::absolute(argv[1]);
        ExposureHarness harness(shaderPath);
        std::cout << "[GPU] " << harness.properties().deviceName << " API "
                  << VK_API_VERSION_MAJOR(harness.properties().apiVersion) << '.'
                  << VK_API_VERSION_MINOR(harness.properties().apiVersion) << '.'
                  << VK_API_VERSION_PATCH(harness.properties().apiVersion) << '\n';
        runCases(harness);
        return 0;
    } catch (const SkipError &skip) {
        std::cout << "[SKIP] " << skip.what() << '\n';
        return SKIP_RETURN_CODE;
    } catch (const std::exception &failure) {
        std::cerr << "[FAIL] " << failure.what() << '\n';
        return 1;
    }
}
