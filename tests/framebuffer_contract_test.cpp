#include "core/render/framebuffers.hpp"

#include <array>
#include <iostream>
#include <span>
#include <vector>

namespace fb = mcvr::framebuffer;

namespace {
bool expect(uint32_t actual, uint32_t expected, const char *label) {
    if (actual == expected) return true;
    std::cerr << label << ": expected 0x" << std::hex << expected << ", got 0x" << actual << std::dec << std::endl;
    return false;
}

fb::AttachmentContract color(uint32_t attachment,
                             uint32_t key,
                             uint32_t width = 128,
                             uint32_t height = 64,
                             VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
    return {
        .attachment = attachment,
        .allocated = true,
        .compatible = true,
        .resourceNamespace = 1,
        .resourceId = key,
        .extent = {width, height},
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = samples,
    };
}
} // namespace

int main() {
    bool passed = true;

    passed &= expect(fb::evaluateStatus({}, {}, fb::NONE), fb::FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT,
                     "missing attachments");

    auto incomplete = color(fb::COLOR_ATTACHMENT0, 1);
    incomplete.allocated = false;
    passed &= expect(fb::evaluateStatus(std::span{&incomplete, 1}, std::array{fb::COLOR_ATTACHMENT0},
                                        fb::COLOR_ATTACHMENT0),
                     fb::FRAMEBUFFER_INCOMPLETE_ATTACHMENT, "unallocated attachment");
    incomplete.allocated = true;
    incomplete.compatible = false;
    passed &= expect(fb::evaluateStatus(std::span{&incomplete, 1}, std::array{fb::COLOR_ATTACHMENT0},
                                        fb::COLOR_ATTACHMENT0),
                     fb::FRAMEBUFFER_INCOMPLETE_ATTACHMENT, "incompatible attachment format");

    std::vector attachments{color(fb::COLOR_ATTACHMENT0, 1)};
    passed &= expect(fb::evaluateStatus(attachments, std::array{fb::COLOR_ATTACHMENT0}, fb::COLOR_ATTACHMENT0),
                     fb::FRAMEBUFFER_COMPLETE, "single color attachment");

    attachments = {color(fb::COLOR_ATTACHMENT0 + 1, 2)};
    passed &= expect(fb::evaluateStatus(attachments, std::array{fb::COLOR_ATTACHMENT0}, fb::NONE),
                     fb::FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER, "missing active draw attachment");
    passed &= expect(fb::evaluateStatus(attachments, std::array{fb::COLOR_ATTACHMENT0 + 1}, fb::COLOR_ATTACHMENT0),
                     fb::FRAMEBUFFER_INCOMPLETE_READ_BUFFER, "missing active read attachment");
    passed &= expect(fb::evaluateStatus(attachments, std::array{fb::COLOR_ATTACHMENT0 + 1},
                                        fb::COLOR_ATTACHMENT0 + 1),
                     fb::FRAMEBUFFER_COMPLETE, "explicit color one selection");

    attachments = {color(fb::COLOR_ATTACHMENT0, 1), color(fb::COLOR_ATTACHMENT0 + 1, 2, 64, 64)};
    passed &= expect(fb::evaluateStatus(attachments, std::array{fb::COLOR_ATTACHMENT0}, fb::COLOR_ATTACHMENT0),
                     fb::FRAMEBUFFER_INCOMPLETE_DIMENSIONS, "extent mismatch");

    attachments = {color(fb::COLOR_ATTACHMENT0, 1),
                   color(fb::COLOR_ATTACHMENT0 + 1, 2, 128, 64, VK_SAMPLE_COUNT_2_BIT)};
    passed &= expect(fb::evaluateStatus(attachments, std::array{fb::COLOR_ATTACHMENT0}, fb::COLOR_ATTACHMENT0),
                     fb::FRAMEBUFFER_INCOMPLETE_MULTISAMPLE, "sample mismatch");

    attachments = {color(fb::COLOR_ATTACHMENT0, 1, 128, 64, VK_SAMPLE_COUNT_2_BIT)};
    passed &= expect(fb::evaluateStatus(attachments, std::array{fb::COLOR_ATTACHMENT0}, fb::COLOR_ATTACHMENT0),
                     fb::FRAMEBUFFER_INCOMPLETE_MULTISAMPLE, "unsupported multisample storage");

    attachments = {{
                       .attachment = fb::DEPTH_ATTACHMENT,
                       .allocated = true,
                       .compatible = true,
                       .resourceNamespace = 1,
                       .resourceId = 10,
                       .extent = {128, 64},
                       .format = VK_FORMAT_D24_UNORM_S8_UINT,
                   },
                   {
                       .attachment = fb::STENCIL_ATTACHMENT,
                       .allocated = true,
                       .compatible = true,
                       .resourceNamespace = 1,
                       .resourceId = 11,
                       .extent = {128, 64},
                       .format = VK_FORMAT_D24_UNORM_S8_UINT,
                   }};
    passed &= expect(fb::evaluateStatus(attachments, std::array{fb::NONE}, fb::NONE), fb::FRAMEBUFFER_UNSUPPORTED,
                     "separate depth and stencil images");
    attachments[1].resourceId = 10;
    attachments[1].level = 1;
    passed &= expect(fb::evaluateStatus(attachments, std::array{fb::NONE}, fb::NONE), fb::FRAMEBUFFER_UNSUPPORTED,
                     "separate depth and stencil mip levels");
    attachments[1].level = 0;
    passed &= expect(fb::evaluateStatus(attachments, std::array{fb::NONE}, fb::NONE), fb::FRAMEBUFFER_COMPLETE,
                     "combined depth stencil image");

    passed &= fb::formatAspects(VK_FORMAT_R8G8B8A8_UNORM) == VK_IMAGE_ASPECT_COLOR_BIT;
    passed &= fb::formatAspects(VK_FORMAT_D32_SFLOAT) == VK_IMAGE_ASPECT_DEPTH_BIT;
    passed &= fb::formatAspects(VK_FORMAT_S8_UINT) == VK_IMAGE_ASPECT_STENCIL_BIT;
    passed &= fb::formatAspects(VK_FORMAT_D24_UNORM_S8_UINT) ==
              (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    return passed ? 0 : 1;
}
