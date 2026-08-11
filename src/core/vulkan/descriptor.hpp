#pragma once

#include "core/all_extern.hpp"

#include <cstddef>
#include <functional>
#include <unordered_map>
#include <vector>

namespace vk {
class Device;
class DescriptorTableBuilder;
class Buffer;
class Image;
class Sampler;
class TLAS;

class DescriptorPool {};

class DescriptorTable : public SharedObject<DescriptorTable> {
    friend DescriptorTableBuilder;

  public:
    DescriptorTable(std::shared_ptr<Device> device,
                    VkDescriptorPool descriptorPool,
                    std::vector<VkDescriptorSetLayout> tableLayout,
                    std::vector<VkDescriptorSet> table,
                    std::vector<std::vector<VkDescriptorType>> tableTypes,
                    std::vector<VkPushConstantRange> pushConstantRanges);
    ~DescriptorTable();

    uint32_t setCount();
    uint32_t dynamicDescriptorCount() const;
    std::vector<VkDescriptorSet> &descriptorSet();
    std::vector<VkDescriptorSetLayout> &descriptorSetLayout();
    VkPipelineLayout &vkPipelineLayout();
    // Strong ownership of the pipeline layout and its descriptor set layouts. Pipelines
    // created with this layout hold this token so the layout outlives the table.
    std::shared_ptr<void> pipelineLayoutKeepAlive();
    // Bind an independently owned, layout-compatible immutable set. The caller must retain
    // each generation used by recorded commands until its submission retires.
    void useExternalSet(uint32_t set, std::shared_ptr<DescriptorTable> owner, uint32_t sourceSet);

    std::shared_ptr<DescriptorTable> bindImage(
        std::shared_ptr<Image> image, VkImageLayout layout, uint32_t set, uint32_t binding, uint32_t viewIndex = 0);
    std::shared_ptr<DescriptorTable> bindSamplerImage(std::shared_ptr<Sampler> sampler,
                                                      std::shared_ptr<Image> image,
                                                      VkImageLayout layout,
                                                      uint32_t set,
                                                      uint32_t binding,
                                                      uint32_t index,
                                                      uint32_t viewIndex = 0);
    std::shared_ptr<DescriptorTable>
    bindImageForShader(std::shared_ptr<Image> image, uint32_t set, uint32_t binding, uint32_t viewIndex = 0);
    std::shared_ptr<DescriptorTable> bindSamplerImageForShader(std::shared_ptr<Sampler> sampler,
                                                               std::shared_ptr<Image> image,
                                                               uint32_t set,
                                                               uint32_t binding,
                                                               uint32_t viewIndex = 0);

    std::shared_ptr<DescriptorTable> bindBuffer(std::shared_ptr<Buffer> buffer, uint32_t set, uint32_t binding);
    std::shared_ptr<DescriptorTable>
    bindBuffer(std::shared_ptr<Buffer> buffer, uint32_t set, uint32_t binding, uint32_t index);
    std::shared_ptr<DescriptorTable> bindBufferRange(std::shared_ptr<Buffer> buffer,
                                                     uint32_t set,
                                                     uint32_t binding,
                                                     VkDeviceSize offset,
                                                     VkDeviceSize range);
    std::shared_ptr<DescriptorTable> bindBufferRange(std::shared_ptr<Buffer> buffer,
                                                     uint32_t set,
                                                     uint32_t binding,
                                                     uint32_t index,
                                                     VkDeviceSize offset,
                                                     VkDeviceSize range);
    std::shared_ptr<DescriptorTable>
    bindBuffers(std::vector<std::shared_ptr<Buffer>> buffers, uint32_t set, uint32_t binding);

    std::shared_ptr<DescriptorTable> bindAS(std::shared_ptr<TLAS> buffer, uint32_t set, uint32_t binding);

  private:
    struct ResourceBindingKey {
        uint32_t set;
        uint32_t binding;
        uint32_t index;

        bool operator==(const ResourceBindingKey &other) const noexcept {
            return set == other.set && binding == other.binding && index == other.index;
        }
    };

    struct ResourceBindingKeyHash {
        std::size_t operator()(const ResourceBindingKey &key) const noexcept {
            std::size_t hash = std::hash<uint32_t>{}(key.set);
            hash ^= std::hash<uint32_t>{}(key.binding) + static_cast<std::size_t>(0x9e3779b9U) + (hash << 6) +
                    (hash >> 2);
            hash ^= std::hash<uint32_t>{}(key.index) + static_cast<std::size_t>(0x9e3779b9U) + (hash << 6) +
                    (hash >> 2);
            return hash;
        }
    };

    void retainDescriptorResources(uint32_t set,
                                   uint32_t binding,
                                   uint32_t index,
                                   std::vector<std::shared_ptr<void>> resources);

    std::shared_ptr<Device> device_;

    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayout> tableLayout_;
    std::vector<VkDescriptorSet> table_;
    std::vector<std::shared_ptr<DescriptorTable>> externalSets_;
    std::vector<std::vector<VkDescriptorType>> tableTypes_;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    std::shared_ptr<void> pipelineLayoutKeepAlive_;
    std::vector<VkPushConstantRange> pushConstantRanges_;
    // Keep every resource generation written to this table until the table retires.
    // UPDATE_AFTER_BIND permits a bound table to be updated before submission.
    std::unordered_map<ResourceBindingKey, std::vector<std::shared_ptr<void>>, ResourceBindingKeyHash>
        resourceKeepAlive_;
};

class DescriptorTableBuilder {
    friend DescriptorTable;

  public:
    struct DescriptorLayoutSetBuilder;

    struct DescriptorLayoutSetBindingBuilder {
        DescriptorLayoutSetBuilder &parent;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<VkDescriptorType> types;

        DescriptorLayoutSetBindingBuilder(DescriptorLayoutSetBuilder &parent);

        DescriptorLayoutSetBindingBuilder &defineDescriptorLayoutSetBinding(VkDescriptorSetLayoutBinding binding);
        DescriptorLayoutSetBuilder &endDescriptorLayoutSetBinding();
    };

    struct DescriptorLayoutSetBuilder {
        DescriptorTableBuilder &parent;
        std::vector<DescriptorLayoutSetBindingBuilder> setBindingBuilders;

        DescriptorLayoutSetBuilder(DescriptorTableBuilder &parent);

        DescriptorLayoutSetBindingBuilder &beginDescriptorLayoutSetBinding();
        DescriptorTableBuilder &endDescriptorLayoutSet();
    };

  public:
    DescriptorTableBuilder();

    DescriptorLayoutSetBuilder &beginDescriptorLayoutSet();
    DescriptorTableBuilder &definePushConstant(VkPushConstantRange pushConstantRange);
    std::shared_ptr<DescriptorTable> build(std::shared_ptr<Device> device);

  private:
    DescriptorLayoutSetBuilder setBuilders;

    std::vector<VkPushConstantRange> pushConstantRanges;
};
}; // namespace vk
