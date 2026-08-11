#pragma once
#include "audit_sink.hpp"
#include "core/logging.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/buffers.hpp"
#include "core/render/scene_scope.hpp"
#include "core/vulkan/image_format.hpp"
#include <filesystem>
#include <fstream>
#include <chrono>

namespace mcvr::diagnostics {
// Temporary, opt-in capture. Retained until the recorded frame's fence retires.
// A GPU event also guards against writing unsubmitted/aborted capture buffers.
struct PonderCapture {
    struct Image {
        std::string name;
        std::shared_ptr<vk::HostVisibleBuffer> buffer;
        std::shared_ptr<vk::DeviceLocalImage> source;
    };
    std::shared_ptr<vk::Device> device;
    std::filesystem::path directory;
    std::vector<Image> images;
    VkEvent completed = VK_NULL_HANDLE;
    bool sealed = false;

    ~PonderCapture() noexcept {
        try {
            std::ofstream status(directory / "status.txt");
            if (!sealed || vkGetEventStatus(device->vkDevice(), completed) != VK_EVENT_SET) {
                status << "INCOMPLETE: GPU completion marker not reached\n";
            } else {
                for (auto &image : images) {
                    image.buffer->downloadFromBuffer();
                    std::ofstream file(directory / (image.name + ".bin"), std::ios::binary);
                    file.write(static_cast<const char *>(image.buffer->mappedPtr()), image.buffer->size());
                    file.close();
                    if (!file) throw std::runtime_error("Capture write failed");
                }
                status << "COMPLETE\n";
            }
        } catch (const std::exception &e) {
            mcvr::log::error("PonderCapture") << "[PonderCapture] " << e.what() << std::endl;
        }
        if (completed) vkDestroyEvent(device->vkDevice(), completed, nullptr);
    }

    void copy(const std::shared_ptr<Framework> &framework,
              const std::shared_ptr<vk::CommandBuffer> &commands,
              const char *name, const std::shared_ptr<vk::DeviceLocalImage> &source) {
        const size_t bytes = static_cast<size_t>(source->width()) * source->height() * vk::formatToByte(source->vkFormat());
        auto buffer = vk::HostVisibleBuffer::create(framework->vma(), device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto original = source->imageLayout();
        auto transition = [&](VkImageLayout before, VkImageLayout after) {
            commands->barriersBufferImage({}, {{
                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                .oldLayout = before, .newLayout = after,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source, .subresourceRange = vk::wholeColorSubresourceRange,
            }});
        };
        transition(original, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {source->width(), source->height(), 1};
        vkCmdCopyImageToBuffer(commands->vkCommandBuffer(), source->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              buffer->vkBuffer(), 1, &region);
        transition(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, original);
        std::ofstream manifest(directory / "images.tsv", std::ios::app);
        manifest << name << '\t' << source->width() << '\t' << source->height() << '\t'
                 << static_cast<int>(source->vkFormat()) << '\t' << bytes << '\n';
        images.push_back({name, buffer, source});
    }

    void seal(const std::shared_ptr<vk::CommandBuffer> &commands, NVSDK_NGX_Result result,
              bool isRR = true) {
        if (isRR) std::ofstream(directory / "rr-result.txt") << static_cast<unsigned int>(result) << '\n';
        else std::ofstream(directory / "dispatch.txt") << "FSR dispatch returned; wrapper has no result value. Check runtime error log.\n";
        sealCommands(commands);
    }

    void sealCommands(const std::shared_ptr<vk::CommandBuffer> &commands) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commands->vkCommandBuffer(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        vkCmdSetEvent(commands->vkCommandBuffer(), completed, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        sealed = true;
    }

    static std::shared_ptr<PonderCapture> begin(const std::shared_ptr<Framework> &framework,
                                                const std::shared_ptr<Buffers> &buffers,
                                                const char *backend = "DLSS-RR") {
        if (!mcvr::audit::enabled(MCVR_AUDIT_CAPTURE)) return {};
        // Render-thread only. Explicit request files keep world, UI and FG captures independent.
        const bool fg = std::string_view(backend) == "FG";
        const bool ui = SceneRecordingScope::active() != nullptr;
        struct Request {
            bool accepted = false;
            unsigned frame = 0;
            std::weak_ptr<Buffers> selected;
            std::filesystem::path root;
        };
        static Request requests[3];
        auto &[accepted, frame, selected, root] = requests[fg ? 1 : (ui ? 0 : 2)];
        const auto limit = fg ? 1u : 8u;
        const char *request = fg ? "radiance-fg-capture.request" :
            (ui ? "radiance-ponder-capture.request" : "radiance-world-capture.request");
        // A completed burst (or a destroyed scene) can accept a new request.
        // Request files are moved on acceptance, so this never auto-retriggers.
        if (accepted && (frame >= limit || selected.expired())) accepted = false;
        if (!accepted) {
            std::error_code ec;
            if (!std::filesystem::exists(request, ec)) return {};
            const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            root = std::filesystem::path(fg ? "radiance-fg-captures" :
                (ui ? "radiance-ponder-captures" : "radiance-world-captures")) / std::to_string(stamp);
            std::filesystem::create_directories(root);
            std::filesystem::rename(request, root / "request.accepted");
            selected = buffers;
            frame = 0;
            accepted = true;
            mcvr::log::info("PonderCapture") << "[PonderCapture] Accepted: " << root << std::endl;
        }
        // Capture both views of a transition, not just the first/current view.
        // Eight evaluations total keeps readback memory bounded (four paired frames).
        if (frame >= limit) return {};
        auto capture = std::make_shared<PonderCapture>();
        capture->device = framework->device();
        capture->directory = root / ("frame-" + std::to_string(++frame));
        std::filesystem::create_directories(capture->directory);
        std::ofstream(capture->directory / "backend.txt") << backend << '\n';
        VkEventCreateInfo info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        if (vkCreateEvent(capture->device->vkDevice(), &info, nullptr, &capture->completed) != VK_SUCCESS)
            throw std::runtime_error("Could not create capture completion event");
        auto writeUbo = [&](const char *name, const auto &buffer) {
            auto *ubo = static_cast<const vk::Data::WorldUBO *>(buffer->mappedPtr());
            std::ofstream file(capture->directory / name);
            file.precision(9);
            file << "jitter " << ubo->cameraJitter.x << ' ' << ubo->cameraJitter.y << '\n';
            file << "uiOwner " << ubo->uiPrimaryOwner << '\n';
            for (const auto &matrix : {ubo->cameraViewMat, ubo->cameraEffectedViewMat, ubo->cameraProjMat}) {
                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 4; ++c) file << matrix[c][r] << ' ';
                    file << '\n';
                }
            }
        };
        writeUbo("current-camera.txt", buffers->worldUniformBuffer());
        writeUbo("previous-camera.txt", buffers->lastWorldUniformBuffer());
        framework->frameResourceRetainer().retain(capture);
        return capture;
    }
};
}
