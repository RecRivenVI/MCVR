
#pragma once
#include <atomic>

#include "common/shared.hpp"
#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/vulkan/all_core_vulkan.hpp"

#include <map>
#include <set>
#include <vector>

class Framework;

class Buffers : public SharedObject<Buffers> {
  public:
    Buffers(std::shared_ptr<Framework> framework);

    void resetFrame();
    uint32_t allocateBuffer();
    uint32_t allocatePersistentBuffer();
    void releasePersistentBuffer(uint32_t id);
    void initializeBuffer(uint32_t id, uint32_t size, VkBufferUsageFlags usageFlags);
    void buildIndexBuffer(uint32_t dstId, int type, int drawMode, int vertexCount, int expectedIndexCount);
    void queueOverlayUpload(uint8_t *srcPointer, uint32_t dstId);
    void queuePersistentUploadRange(uint8_t *srcPointer, uint32_t size, uint32_t dstId,
                                    uint32_t dstOffset);
    void queueImportantWorldUpload(std::shared_ptr<vk::DeviceLocalBuffer> buffer);
    void queueImportantWorldUpload(std::shared_ptr<vk::DeviceLocalBuffer> vertexBuffer,
                                   std::shared_ptr<vk::DeviceLocalBuffer> indexBuffer);
    void performQueuedUpload();

    bool registerOverlayDrawUniformSize(uint32_t size);
    void appendOverlayDrawUniform(uint8_t *srcPointer, uint32_t size, uint32_t &uniformOffset);
    void appendOverlayPostUniform(vk::Data::OverlayPostUBO &ubo);
    vk::Data::OverlayPostUBO recordedOverlayPostUniform(uint32_t offset);
    void buildAndUploadOverlayUniformBuffer();

    void setAndUploadWorldUniformBuffer(vk::Data::WorldUBO &ubo);
    void invalidateWorldHistory() { worldHistoryValid_.store(false, std::memory_order_release); }
    void setAndUploadSkyUniformBuffer(vk::Data::SkyUBO &ubo);
    void setAndUploadTextureMappingBuffer(vk::Data::TextureMapping &mapping);
    void setAndUploadExposureDataBuffer(vk::Data::ExposureData &exposureData);

    int getPostID();

    std::shared_ptr<vk::DeviceLocalBuffer> getBuffer(uint32_t id);

    std::shared_ptr<vk::HostVisibleBuffer> overlayDrawUniformBuffer();
    uint32_t overlayDrawUniformDescriptorRange();
    std::shared_ptr<vk::HostVisibleBuffer> overlayPostUniformBuffer();
    uint32_t overlayPostUniformDescriptorRange();
    uint32_t overlayPostUniformOffset(int postID);

    std::shared_ptr<vk::HostVisibleBuffer> worldUniformBuffer();
    std::shared_ptr<vk::HostVisibleBuffer> lastWorldUniformBuffer();
    std::shared_ptr<vk::HostVisibleBuffer> skyUniformBuffer();
    std::shared_ptr<vk::HostVisibleBuffer> textureMappingBuffer();
    std::shared_ptr<vk::HostVisibleBuffer> exposureDataBuffer();

    void setUseJitter(bool useJitter);

  private:
    static constexpr uint32_t baseBlockSize = 16 * 1024;
    static constexpr uint32_t overlayPostUniformInitialSize = 512 * 1024;
    static constexpr uint32_t overlayDrawUniformInitialSize = 8 * 1024 * 1024;
    static constexpr uint32_t overlayDrawUniformInitialDescriptorRange = 4 * 1024;

    bool ensureOverlayDrawUniformBufferCapacityLocked(std::shared_ptr<Framework> framework,
                                                      uint32_t frameIndex,
                                                      uint32_t requiredBufferSize);

    std::vector<std::map<uint32_t, int32_t>> validOverlayIndex_;
    std::vector<std::map<uint32_t, std::shared_ptr<vk::DeviceLocalBuffer>>> overlayIndexVertexBuffer_;
    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> overlayDrawUniformBuffer_;
    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> overlayPostUniformBuffer_;
    uint32_t overlayNextID_;
    std::vector<std::vector<uint8_t>> overlayDrawUniformData_;
    std::vector<uint32_t> overlayDrawUniformWriteOffset_;
    uint32_t overlayDrawUniformAlignment_ = 1;
    uint32_t overlayDrawUniformDeviceLimit_ = 1;
    uint32_t overlayDrawUniformDescriptorRange_ = 1;
    std::vector<std::vector<uint8_t>> overlayPostUniformData_;
    std::vector<uint32_t> overlayPostUniformCount_;
    uint32_t overlayPostUniformStride_ = 1;
    uint32_t overlayPostUniformDescriptorRange_ = 1;

    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> worldUniformBuffer_;
    vk::Data::WorldUBO lastWorldUbo_{};
    std::atomic<bool> worldHistoryValid_{false};
    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> lastWorldUniformBuffer_;
    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> skyUniformBuffer_;
    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> textureMappingBuffer_;
    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> exposureDataBuffer_;

    std::shared_ptr<std::vector<std::shared_ptr<vk::DeviceLocalBuffer>>> importantIndexVertexBuffer_;

    bool useJitter_ = true;
    size_t jitterSequenceIndex_ = 0;
    std::recursive_mutex mtx_;
    struct PersistentBuffer {
        std::shared_ptr<vk::DeviceLocalBuffer> buffer;
        uint32_t size = 0;
        VkBufferUsageFlags usage = 0;
        bool uploadReady = false;
        bool rangeWriteOpen = false;
    };
    std::map<uint32_t, PersistentBuffer> persistentBuffers_;
    std::vector<std::shared_ptr<vk::DeviceLocalBuffer>> pendingPersistentUploads_;
    uint32_t nextPersistentId_ = 0x40000000u;
};
