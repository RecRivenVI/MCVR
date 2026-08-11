#pragma once

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace vk {

inline constexpr std::size_t MAX_PIPELINE_CACHE_BYTES = 64U * 1024U * 1024U;

struct PipelineCacheIdentity {
    uint32_t vendorId{};
    uint32_t deviceId{};
    std::array<uint8_t, VK_UUID_SIZE> uuid{};
};

enum class PipelineCacheDataStatus {
    Valid,
    TooSmall,
    TooLarge,
    InvalidHeaderSize,
    UnsupportedHeaderVersion,
    VendorMismatch,
    DeviceMismatch,
    UuidMismatch,
};

inline PipelineCacheDataStatus validatePipelineCacheData(
    std::span<const std::byte> data,
    const PipelineCacheIdentity &identity) noexcept {
    if (data.size() < sizeof(VkPipelineCacheHeaderVersionOne)) {
        return PipelineCacheDataStatus::TooSmall;
    }
    if (data.size() > MAX_PIPELINE_CACHE_BYTES) {
        return PipelineCacheDataStatus::TooLarge;
    }

    VkPipelineCacheHeaderVersionOne header{};
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.headerSize < sizeof(VkPipelineCacheHeaderVersionOne) ||
        header.headerSize > data.size()) {
        return PipelineCacheDataStatus::InvalidHeaderSize;
    }
    if (header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) {
        return PipelineCacheDataStatus::UnsupportedHeaderVersion;
    }
    if (header.vendorID != identity.vendorId) {
        return PipelineCacheDataStatus::VendorMismatch;
    }
    if (header.deviceID != identity.deviceId) {
        return PipelineCacheDataStatus::DeviceMismatch;
    }
    if (std::memcmp(header.pipelineCacheUUID, identity.uuid.data(), VK_UUID_SIZE) != 0) {
        return PipelineCacheDataStatus::UuidMismatch;
    }
    return PipelineCacheDataStatus::Valid;
}

inline const char *pipelineCacheDataStatusName(PipelineCacheDataStatus status) noexcept {
    switch (status) {
    case PipelineCacheDataStatus::Valid:
        return "valid";
    case PipelineCacheDataStatus::TooSmall:
        return "too small";
    case PipelineCacheDataStatus::TooLarge:
        return "too large";
    case PipelineCacheDataStatus::InvalidHeaderSize:
        return "invalid header size";
    case PipelineCacheDataStatus::UnsupportedHeaderVersion:
        return "unsupported header version";
    case PipelineCacheDataStatus::VendorMismatch:
        return "vendor mismatch";
    case PipelineCacheDataStatus::DeviceMismatch:
        return "device mismatch";
    case PipelineCacheDataStatus::UuidMismatch:
        return "pipeline cache UUID mismatch";
    }
    return "unknown";
}

} // namespace vk
