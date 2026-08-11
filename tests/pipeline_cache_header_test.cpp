#include "core/vulkan/pipeline_cache_format.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

std::vector<std::byte> makeCacheData(const vk::PipelineCacheIdentity &identity) {
    VkPipelineCacheHeaderVersionOne header{
        .headerSize = sizeof(VkPipelineCacheHeaderVersionOne),
        .headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE,
        .vendorID = identity.vendorId,
        .deviceID = identity.deviceId,
    };
    std::copy(identity.uuid.begin(), identity.uuid.end(), header.pipelineCacheUUID);
    std::vector<std::byte> data(sizeof(header) + 16);
    std::memcpy(data.data(), &header, sizeof(header));
    return data;
}

bool expect(vk::PipelineCacheDataStatus actual,
            vk::PipelineCacheDataStatus expected,
            const char *label) {
    if (actual == expected) return true;
    std::cerr << label << ": expected " << vk::pipelineCacheDataStatusName(expected)
              << ", got " << vk::pipelineCacheDataStatusName(actual) << std::endl;
    return false;
}

} // namespace

int main() {
    vk::PipelineCacheIdentity identity{
        .vendorId = 0x10de,
        .deviceId = 0x2684,
        .uuid = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    };
    bool passed = true;

    auto data = makeCacheData(identity);
    passed &= expect(vk::validatePipelineCacheData(data, identity),
                     vk::PipelineCacheDataStatus::Valid, "valid header");
    passed &= expect(vk::validatePipelineCacheData(
                         std::span<const std::byte>(data.data(), sizeof(VkPipelineCacheHeaderVersionOne) - 1),
                         identity),
                     vk::PipelineCacheDataStatus::TooSmall, "truncated header");

    auto corrupt = data;
    auto *header = reinterpret_cast<VkPipelineCacheHeaderVersionOne *>(corrupt.data());
    header->headerSize = sizeof(VkPipelineCacheHeaderVersionOne) - 1;
    passed &= expect(vk::validatePipelineCacheData(corrupt, identity),
                     vk::PipelineCacheDataStatus::InvalidHeaderSize, "short header size");

    corrupt = data;
    header = reinterpret_cast<VkPipelineCacheHeaderVersionOne *>(corrupt.data());
    header->headerSize = static_cast<uint32_t>(corrupt.size() + 1);
    passed &= expect(vk::validatePipelineCacheData(corrupt, identity),
                     vk::PipelineCacheDataStatus::InvalidHeaderSize, "oversized header");

    corrupt = data;
    header = reinterpret_cast<VkPipelineCacheHeaderVersionOne *>(corrupt.data());
    header->headerVersion = static_cast<VkPipelineCacheHeaderVersion>(99);
    passed &= expect(vk::validatePipelineCacheData(corrupt, identity),
                     vk::PipelineCacheDataStatus::UnsupportedHeaderVersion, "header version");

    auto otherIdentity = identity;
    otherIdentity.vendorId++;
    passed &= expect(vk::validatePipelineCacheData(data, otherIdentity),
                     vk::PipelineCacheDataStatus::VendorMismatch, "vendor");
    otherIdentity = identity;
    otherIdentity.deviceId++;
    passed &= expect(vk::validatePipelineCacheData(data, otherIdentity),
                     vk::PipelineCacheDataStatus::DeviceMismatch, "device");
    otherIdentity = identity;
    otherIdentity.uuid[7] ^= 0xff;
    passed &= expect(vk::validatePipelineCacheData(data, otherIdentity),
                     vk::PipelineCacheDataStatus::UuidMismatch, "UUID");

    return passed ? 0 : 1;
}
