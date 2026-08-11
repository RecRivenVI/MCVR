#include "core/vulkan/pipeline_cache.hpp"

#include "core/logging.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

auto pipelineCacheCout() {
    return mcvr::log::info("PipelineCache");
}

auto pipelineCacheCerr() {
    return mcvr::log::error("PipelineCache");
}

uint64_t fnv1a(std::span<const std::byte> bytes) noexcept {
    uint64_t hash = 14695981039346656037ULL;
    for (std::byte value : bytes) {
        hash ^= static_cast<uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hexadecimal(uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::string deviceKey(const vk::PipelineCacheIdentity &identity) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(8) << identity.vendorId
           << "-" << std::setw(8) << identity.deviceId << "-";
    for (uint8_t value : identity.uuid) {
        stream << std::setw(2) << static_cast<unsigned int>(value);
    }
    return stream.str();
}

bool contentHashMatches(const std::filesystem::path &path,
                        std::span<const std::byte> data) {
    constexpr std::string_view prefix = "pipeline-v1-";
    const std::string stem = path.stem().string();
    if (!stem.starts_with(prefix) || stem.size() < prefix.size() + 16) {
        return false;
    }
    return stem.substr(prefix.size(), 16) == hexadecimal(fnv1a(data));
}

bool fileEquals(const std::filesystem::path &path,
                std::span<const std::byte> expected) noexcept {
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error ||
            std::filesystem::file_size(path, error) != expected.size() || error) {
            return false;
        }
        std::vector<std::byte> actual(expected.size());
        std::ifstream input(path, std::ios::binary);
        return input.read(reinterpret_cast<char *>(actual.data()),
                          static_cast<std::streamsize>(actual.size())) &&
               std::equal(actual.begin(), actual.end(), expected.begin());
    } catch (...) {
        return false;
    }
}

const VkPipelineCreationFeedbackCreateInfo *findCreationFeedback(
    const void *chain) noexcept {
    auto current = static_cast<const VkBaseInStructure *>(chain);
    while (current != nullptr) {
        if (current->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) {
            return reinterpret_cast<const VkPipelineCreationFeedbackCreateInfo *>(current);
        }
        current = current->pNext;
    }
    return nullptr;
}

} // namespace

vk::PipelineCache::PipelineCache(VkDevice device,
                                 const VkPhysicalDeviceProperties &properties,
                                 std::filesystem::path cacheRoot)
    : device_(device) {
    identity_.vendorId = properties.vendorID;
    identity_.deviceId = properties.deviceID;
    std::copy_n(properties.pipelineCacheUUID, VK_UUID_SIZE, identity_.uuid.begin());
    if (!cacheRoot.empty()) {
        cacheDirectory_ = std::filesystem::absolute(cacheRoot / deviceKey(identity_)).lexically_normal();
#if defined(_WIN32)
        auto nativePath = cacheDirectory_.native();
        if (!nativePath.starts_with(L"\\\\?\\")) {
            nativePath = nativePath.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + nativePath.substr(2)
                                                          : L"\\\\?\\" + nativePath;
        }
        cacheDirectory_ = std::move(nativePath);
#endif
    }

    std::vector<std::byte> initialData = loadLatest();
    VkPipelineCacheCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = initialData.size(),
        .pInitialData = initialData.empty() ? nullptr : initialData.data(),
    };
    VkResult result = vkCreatePipelineCache(device_, &createInfo, nullptr, &cache_);
    if (result != VK_SUCCESS && !initialData.empty()) {
        pipelineCacheCerr() << "driver rejected persisted cache (" << result
                            << "); falling back to an empty cache" << std::endl;
        cache_ = VK_NULL_HANDLE;
        createInfo.initialDataSize = 0;
        createInfo.pInitialData = nullptr;
        result = vkCreatePipelineCache(device_, &createInfo, nullptr, &cache_);
    }
    if (result != VK_SUCCESS) {
        cache_ = VK_NULL_HANDLE;
        throw std::runtime_error("vkCreatePipelineCache failed with VkResult " +
                                 std::to_string(result));
    }
}

vk::PipelineCache::~PipelineCache() {
    if (cache_ != VK_NULL_HANDLE) {
        {
            std::lock_guard lock(mutex_);
            const double totalMilliseconds =
                static_cast<double>(creationStats_.totalDurationNanoseconds) / 1'000'000.0;
            const double missMilliseconds =
                static_cast<double>(creationStats_.cacheMissDurationNanoseconds) / 1'000'000.0;
            pipelineCacheCout()
                << "creation summary: calls=" << creationStats_.calls
                << ", failedCalls=" << creationStats_.failedCalls
                << ", requestedPipelines=" << creationStats_.requestedPipelines
                << ", validFeedback=" << creationStats_.validFeedbackPipelines
                << ", applicationCacheHits=" << creationStats_.applicationCacheHits
                << ", driverDurationMs=" << totalMilliseconds
                << ", nonApplicationHitDurationMs=" << missMilliseconds << std::endl;
        }
        persist();
        vkDestroyPipelineCache(device_, cache_, nullptr);
        cache_ = VK_NULL_HANDLE;
    }
}

VkResult vk::PipelineCache::createGraphicsPipelines(
    uint32_t count,
    const VkGraphicsPipelineCreateInfo *createInfos,
    const VkAllocationCallbacks *allocator,
    VkPipeline *pipelines) {
    std::lock_guard lock(mutex_);
    std::vector<VkGraphicsPipelineCreateInfo> copies;
    if (count > 0) copies.assign(createInfos, createInfos + count);
    std::vector<VkPipelineCreationFeedback> pipelineFeedback(count);
    std::vector<std::vector<VkPipelineCreationFeedback>> stageFeedback(count);
    std::vector<VkPipelineCreationFeedbackCreateInfo> ownedFeedback(count);
    std::vector<const VkPipelineCreationFeedbackCreateInfo *> feedbackSources(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (const auto *callerFeedback = findCreationFeedback(copies[i].pNext)) {
            feedbackSources[i] = callerFeedback;
            continue;
        }
        stageFeedback[i].resize(copies[i].stageCount);
        ownedFeedback[i] = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
            .pNext = copies[i].pNext,
            .pPipelineCreationFeedback = &pipelineFeedback[i],
            .pipelineStageCreationFeedbackCount = copies[i].stageCount,
            .pPipelineStageCreationFeedbacks =
                stageFeedback[i].empty() ? nullptr : stageFeedback[i].data(),
        };
        copies[i].pNext = &ownedFeedback[i];
        feedbackSources[i] = &ownedFeedback[i];
    }
    VkResult result = vkCreateGraphicsPipelines(
        device_, cache_, count, copies.data(), allocator, pipelines);
    recordCreationFeedback(result, count, feedbackSources);
    return result;
}

VkResult vk::PipelineCache::createComputePipelines(
    uint32_t count,
    const VkComputePipelineCreateInfo *createInfos,
    const VkAllocationCallbacks *allocator,
    VkPipeline *pipelines) {
    std::lock_guard lock(mutex_);
    std::vector<VkComputePipelineCreateInfo> copies;
    if (count > 0) copies.assign(createInfos, createInfos + count);
    std::vector<VkPipelineCreationFeedback> pipelineFeedback(count);
    std::vector<std::array<VkPipelineCreationFeedback, 1>> stageFeedback(count);
    std::vector<VkPipelineCreationFeedbackCreateInfo> ownedFeedback(count);
    std::vector<const VkPipelineCreationFeedbackCreateInfo *> feedbackSources(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (const auto *callerFeedback = findCreationFeedback(copies[i].pNext)) {
            feedbackSources[i] = callerFeedback;
            continue;
        }
        ownedFeedback[i] = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
            .pNext = copies[i].pNext,
            .pPipelineCreationFeedback = &pipelineFeedback[i],
            .pipelineStageCreationFeedbackCount = 1,
            .pPipelineStageCreationFeedbacks = stageFeedback[i].data(),
        };
        copies[i].pNext = &ownedFeedback[i];
        feedbackSources[i] = &ownedFeedback[i];
    }
    VkResult result = vkCreateComputePipelines(
        device_, cache_, count, copies.data(), allocator, pipelines);
    recordCreationFeedback(result, count, feedbackSources);
    return result;
}

VkResult vk::PipelineCache::createRayTracingPipelines(
    VkDeferredOperationKHR deferredOperation,
    uint32_t count,
    const VkRayTracingPipelineCreateInfoKHR *createInfos,
    const VkAllocationCallbacks *allocator,
    VkPipeline *pipelines) {
    if (deferredOperation != VK_NULL_HANDLE) {
        // Deferred creation may outlive this call. Do not attach stack-owned feedback
        // or expose this shared cache to an operation whose completion is caller-owned.
        return vkCreateRayTracingPipelinesKHR(device_, deferredOperation, VK_NULL_HANDLE,
                                              count, createInfos, allocator, pipelines);
    }
    std::lock_guard lock(mutex_);
    std::vector<VkRayTracingPipelineCreateInfoKHR> copies;
    if (count > 0) copies.assign(createInfos, createInfos + count);
    std::vector<VkPipelineCreationFeedback> pipelineFeedback(count);
    std::vector<std::vector<VkPipelineCreationFeedback>> stageFeedback(count);
    std::vector<VkPipelineCreationFeedbackCreateInfo> ownedFeedback(count);
    std::vector<const VkPipelineCreationFeedbackCreateInfo *> feedbackSources(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (const auto *callerFeedback = findCreationFeedback(copies[i].pNext)) {
            feedbackSources[i] = callerFeedback;
            continue;
        }
        stageFeedback[i].resize(copies[i].stageCount);
        ownedFeedback[i] = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
            .pNext = copies[i].pNext,
            .pPipelineCreationFeedback = &pipelineFeedback[i],
            .pipelineStageCreationFeedbackCount = copies[i].stageCount,
            .pPipelineStageCreationFeedbacks =
                stageFeedback[i].empty() ? nullptr : stageFeedback[i].data(),
        };
        copies[i].pNext = &ownedFeedback[i];
        feedbackSources[i] = &ownedFeedback[i];
    }
    VkResult result = vkCreateRayTracingPipelinesKHR(
        device_, deferredOperation, cache_, count, copies.data(), allocator, pipelines);
    recordCreationFeedback(result, count, feedbackSources);
    return result;
}

void vk::PipelineCache::recordCreationFeedback(
    VkResult result,
    uint32_t requestedPipelines,
    std::span<const VkPipelineCreationFeedbackCreateInfo *const> feedbackInfos) noexcept {
    ++creationStats_.calls;
    if (result != VK_SUCCESS) {
        ++creationStats_.failedCalls;
    }
    creationStats_.requestedPipelines += requestedPipelines;
    if (result != VK_SUCCESS) return;
    for (const auto *info : feedbackInfos) {
        if (info == nullptr || info->pPipelineCreationFeedback == nullptr) {
            continue;
        }
        const VkPipelineCreationFeedback &feedback = *info->pPipelineCreationFeedback;
        if ((feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) == 0) {
            continue;
        }
        ++creationStats_.validFeedbackPipelines;
        creationStats_.totalDurationNanoseconds += feedback.duration;
        if ((feedback.flags &
             VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0) {
            ++creationStats_.applicationCacheHits;
        } else {
            creationStats_.cacheMissDurationNanoseconds += feedback.duration;
        }
    }
}

std::vector<std::byte> vk::PipelineCache::loadLatest() const noexcept {
    if (cacheDirectory_.empty()) {
        return {};
    }

    try {
        std::error_code error;
        if (!std::filesystem::is_directory(cacheDirectory_, error)) {
            return {};
        }

        struct Candidate {
            std::filesystem::path path;
            std::filesystem::file_time_type modified;
        };
        std::vector<Candidate> candidates;
        for (const auto &entry : std::filesystem::directory_iterator(
                 cacheDirectory_, std::filesystem::directory_options::skip_permission_denied)) {
            std::error_code entryError;
            if (!entry.is_regular_file(entryError) || entryError ||
                entry.path().extension() != ".bin") {
                continue;
            }
            auto modified = entry.last_write_time(entryError);
            if (entryError) {
                continue;
            }
            candidates.push_back({entry.path(), modified});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate &left, const Candidate &right) {
                      return left.modified > right.modified;
                  });

        for (const Candidate &candidate : candidates) {
            std::error_code fileError;
            const uintmax_t size = std::filesystem::file_size(candidate.path, fileError);
            if (fileError || size > MAX_PIPELINE_CACHE_BYTES ||
                size < sizeof(VkPipelineCacheHeaderVersionOne)) {
                continue;
            }
            std::vector<std::byte> data(static_cast<std::size_t>(size));
            std::ifstream input(candidate.path, std::ios::binary);
            if (!input.read(reinterpret_cast<char *>(data.data()),
                            static_cast<std::streamsize>(data.size()))) {
                continue;
            }
            PipelineCacheDataStatus status = validatePipelineCacheData(data, identity_);
            if (status == PipelineCacheDataStatus::Valid &&
                contentHashMatches(candidate.path, data)) {
                pipelineCacheCout() << "loaded " << data.size() << " bytes from "
                                    << candidate.path << std::endl;
                return data;
            }
            pipelineCacheCerr() << "ignored " << candidate.path << ": ";
            if (status != PipelineCacheDataStatus::Valid) {
                pipelineCacheCerr() << pipelineCacheDataStatusName(status);
            } else {
                pipelineCacheCerr() << "content hash mismatch";
            }
            pipelineCacheCerr() << std::endl;
        }
    } catch (const std::exception &exception) {
        pipelineCacheCerr() << "unable to load persisted cache: " << exception.what()
                            << std::endl;
    }
    return {};
}

void vk::PipelineCache::persist() noexcept {
    if (cacheDirectory_.empty()) {
        return;
    }

    try {
        std::lock_guard lock(mutex_);
        std::size_t size = 0;
        VkResult result = vkGetPipelineCacheData(device_, cache_, &size, nullptr);
        if (result != VK_SUCCESS || size < sizeof(VkPipelineCacheHeaderVersionOne) ||
            size > MAX_PIPELINE_CACHE_BYTES) {
            pipelineCacheCerr() << "not persisting invalid cache size/result: size="
                                << size << ", result=" << result << std::endl;
            return;
        }

        std::vector<std::byte> data(size);
        result = vkGetPipelineCacheData(device_, cache_, &size, data.data());
        if (result != VK_SUCCESS) {
            pipelineCacheCerr() << "vkGetPipelineCacheData failed with " << result
                                << std::endl;
            return;
        }
        data.resize(size);
        PipelineCacheDataStatus status = validatePipelineCacheData(data, identity_);
        if (status != PipelineCacheDataStatus::Valid) {
            pipelineCacheCerr() << "driver returned unusable cache data: "
                                << pipelineCacheDataStatusName(status) << std::endl;
            return;
        }

        std::error_code error;
        std::filesystem::create_directories(cacheDirectory_, error);
        if (error) {
            pipelineCacheCerr() << "cannot create " << cacheDirectory_ << ": "
                                << error.message() << std::endl;
            return;
        }

        const uint64_t hash = fnv1a(data);
        const auto sequence = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string baseStem = "pipeline-v1-" + hexadecimal(hash);
        for (const auto &entry : std::filesystem::directory_iterator(
                 cacheDirectory_, std::filesystem::directory_options::skip_permission_denied)) {
            if (entry.path().extension() == ".bin" && fileEquals(entry.path(), data)) {
                pipelineCacheCout() << "cache data already persisted as "
                                    << entry.path() << std::endl;
                return;
            }
        }
        std::string stem = baseStem;
        if (std::filesystem::exists(cacheDirectory_ / (stem + ".bin"), error)) {
            stem += "-" + std::to_string(sequence);
        }
        const std::string temporaryStem = stem + "-" + std::to_string(sequence);
        const std::filesystem::path temporary = cacheDirectory_ / (temporaryStem + ".part");
        const std::filesystem::path destination = cacheDirectory_ / (stem + ".bin");
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char *>(data.data()),
                         static_cast<std::streamsize>(data.size()));
            output.flush();
            if (!output) {
                pipelineCacheCerr() << "failed writing " << temporary << std::endl;
                return;
            }
        }
        std::filesystem::rename(temporary, destination, error);
        if (error) {
            pipelineCacheCerr() << "failed publishing " << destination << ": "
                                << error.message() << std::endl;
            return;
        }
        pipelineCacheCout() << "persisted " << data.size() << " bytes to "
                            << destination << std::endl;
    } catch (const std::exception &exception) {
        pipelineCacheCerr() << "unable to persist cache: " << exception.what()
                            << std::endl;
    }
}
