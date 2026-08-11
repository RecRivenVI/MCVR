#include "core/loading/loading_renderer.hpp"
#include "core/middleware/jni_exception.hpp"
#include "core/middleware/jni_string.hpp"
#include "core/render/renderer.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/modules/ui_module.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
void checked(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
struct Vertex { float x, y, u, v; uint32_t rgba; };
struct Batch { int32_t first, count, role, texture; };
struct Parameters { float width, height; int32_t role; float opacity; };
static_assert(sizeof(Vertex) == 20 && sizeof(Batch) == 16 && sizeof(Parameters) == 16);

void barrier(VkCommandBuffer cmd, const std::shared_ptr<vk::DeviceLocalImage> &image, VkImageLayout next) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = image->imageLayout() == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = image->imageLayout(); b.newLayout = next;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image->vkImage(); b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    image->imageLayout() = next;
}

// Every object is owned by the SERVICE runtime; GAME only transfers frame ownership.
class LoadingRenderer {
    struct Texture {
        std::shared_ptr<vk::DeviceLocalImage> image;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
    };
    struct Slot {
        Texture canvas;
        VkFramebuffer canvasFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer outputFramebuffer = VK_NULL_HANDLE;
        std::shared_ptr<vk::DeviceLocalImage> output;
        std::shared_ptr<vk::HostVisibleBuffer> vertices;
        std::shared_ptr<vk::HostVisibleBuffer> composite;
    };
    std::shared_ptr<Framework> framework;
    VkDevice device;
    VkCommandPool transferPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkRenderPass clearPass = VK_NULL_HANDLE, loadPass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE, copyPipeline = VK_NULL_HANDLE;
    VkSampler linearSampler = VK_NULL_HANDLE, nearestSampler = VK_NULL_HANDLE;
    VkSampler linearRepeatSampler = VK_NULL_HANDLE, nearestRepeatSampler = VK_NULL_HANDLE;
    std::map<int, Texture> textures;
    std::map<uint32_t, Slot> slots;
    uint32_t lastSlot = 0;
    bool submitted = false;
    bool needsAcquire = false;

    void immediate(const std::function<void(VkCommandBuffer)> &record) {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = transferPool; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 1;
        VkCommandBuffer cmd;
        checked(vkAllocateCommandBuffers(device, &alloc, &cmd), "allocate loading transfer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(cmd, &begin), "begin loading transfer");
        record(cmd);
        checked(vkEndCommandBuffer(cmd), "end loading transfer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence;
        checked(vkCreateFence(device, &fenceInfo, nullptr, &fence), "create loading transfer fence");
        auto result = vkQueueSubmit(framework->device()->mainVkQueue(), 1, &submit, fence);
        if (result == VK_SUCCESS) result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, transferPool, 1, &cmd);
        checked(result, "loading transfer");
    }

    std::shared_ptr<vk::DeviceLocalImage> image(int w, int h, VkImageUsageFlags usage) {
        return vk::DeviceLocalImage::create(framework->device(), framework->vma(), false,
            uint32_t(w), uint32_t(h), 1u, VK_FORMAT_R8G8B8A8_UNORM, usage);
    }
    VkDescriptorSet descriptor(const std::shared_ptr<vk::DeviceLocalImage> &image, bool linear, bool clamp = true) {
        VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        a.descriptorPool = descriptorPool; a.descriptorSetCount = 1; a.pSetLayouts = &descriptorLayout;
        VkDescriptorSet set;
        checked(vkAllocateDescriptorSets(device, &a, &set), "allocate loading texture descriptor");
        VkSampler sampler = clamp ? (linear ? linearSampler : nearestSampler) : (linear ? linearRepeatSampler : nearestRepeatSampler);
        VkDescriptorImageInfo info{sampler, image->vkImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = set;
        write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        return set;
    }
    VkFramebuffer framebuffer(VkRenderPass pass, const std::shared_ptr<vk::DeviceLocalImage> &image) {
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; info.renderPass = pass;
        info.attachmentCount = 1; info.pAttachments = &image->vkImageView();
        info.width = image->width(); info.height = image->height(); info.layers = 1;
        VkFramebuffer result; checked(vkCreateFramebuffer(device, &info, nullptr, &result), "create loading framebuffer"); return result;
    }
    VkRenderPass renderPass(bool clear) {
        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = 1; info.pAttachments = &attachment; info.subpassCount = 1; info.pSubpasses = &subpass;
        VkRenderPass result; checked(vkCreateRenderPass(device, &info, nullptr, &result), "create loading render pass"); return result;
    }
    VkShaderModule shader(const char *name) {
        auto path = Renderer::folderPath / "shaders" / "loading" / name;
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("Missing loading shader: " + path.string());
        auto size = input.tellg();
        if (size <= 0 || static_cast<size_t>(size) % 4) throw std::runtime_error("Invalid loading SPIR-V");
        std::vector<uint32_t> code(static_cast<size_t>(size) / 4); input.seekg(0); input.read(reinterpret_cast<char *>(code.data()), size);
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = code.size() * 4; info.pCode = code.data();
        VkShaderModule module; checked(vkCreateShaderModule(device, &info, nullptr, &module), "create loading shader"); return module;
    }
    void makePipeline() {
        auto vertex = shader("element_vert.spv"); auto fragment = shader("element_frag.spv");
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto &stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment;
        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[] = {{0,0,VK_FORMAT_R32G32_SFLOAT,0},{1,0,VK_FORMAT_R32G32_SFLOAT,8},{2,0,VK_FORMAT_R8G8B8A8_UNORM,16}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &binding; vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{}; blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD; blend.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo colour{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; colour.attachmentCount = 1; colour.pAttachments = &blend;
        VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamics;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; info.stageCount = 2; info.pStages = stages;
        info.pVertexInputState = &vi; info.pInputAssemblyState = &ia; info.pViewportState = &viewport; info.pRasterizationState = &raster;
        info.pMultisampleState = &samples; info.pColorBlendState = &colour; info.pDynamicState = &dynamic;
        info.layout = pipelineLayout; info.renderPass = clearPass;
        auto result = framework->device()->createGraphicsPipelines(1, &info, nullptr, &pipeline);
        if (result == VK_SUCCESS) {
            blend.blendEnable = VK_FALSE;
            result = framework->device()->createGraphicsPipelines(1, &info, nullptr, &copyPipeline);
        }
        vkDestroyShaderModule(device, vertex, nullptr); vkDestroyShaderModule(device, fragment, nullptr);
        checked(result, "create loading graphics pipeline");
    }
    void destroySlot(Slot &s) {
        if (s.canvasFramebuffer) vkDestroyFramebuffer(device, s.canvasFramebuffer, nullptr);
        if (s.outputFramebuffer) vkDestroyFramebuffer(device, s.outputFramebuffer, nullptr);
        if (s.canvas.descriptor) vkFreeDescriptorSets(device, descriptorPool, 1, &s.canvas.descriptor);
        s = {};
    }
    void beginPass(VkCommandBuffer cmd, VkRenderPass pass, VkFramebuffer fb, int w, int h, uint32_t colour) {
        VkClearValue clear{};
        for (int i=0; i<4; ++i) clear.color.float32[i] = ((colour >> (8*i)) & 255) / 255.f;
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = pass; begin.framebuffer = fb;
        begin.renderArea.extent = {uint32_t(w),uint32_t(h)}; begin.clearValueCount = 1; begin.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0,0,float(w),float(h),0,1}; VkRect2D scissor{{0,0},{uint32_t(w),uint32_t(h)}};
        vkCmdSetViewport(cmd,0,1,&viewport); vkCmdSetScissor(cmd,0,1,&scissor);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    }
    void draw(VkCommandBuffer cmd, VkDescriptorSet set, int first, int count, const Parameters &p) {
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&set,0,nullptr);
        vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(p),&p);
        vkCmdDraw(cmd,count,1,first,0);
    }
public:
    explicit LoadingRenderer(std::shared_ptr<Framework> f) : framework(std::move(f)), device(framework->device()->vkDevice()) {
        try {
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex = framework->physicalDevice()->mainQueueIndex();
            pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            checked(vkCreateCommandPool(device,&pool,nullptr,&transferPool),"create loading transfer pool");
            VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
            VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; layout.bindingCount = 1; layout.pBindings = &binding;
            checked(vkCreateDescriptorSetLayout(device,&layout,nullptr,&descriptorLayout),"create loading descriptor layout");
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,128};
            VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; descriptors.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            descriptors.maxSets = 128; descriptors.poolSizeCount = 1; descriptors.pPoolSizes = &size;
            checked(vkCreateDescriptorPool(device,&descriptors,nullptr,&descriptorPool),"create loading descriptors");
            VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(Parameters)};
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &descriptorLayout;
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &range;
            checked(vkCreatePipelineLayout(device,&pl,nullptr,&pipelineLayout),"create loading pipeline layout");
            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            checked(vkCreateSampler(device,&sampler,nullptr,&linearSampler),"create loading sampler");
            sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
            checked(vkCreateSampler(device,&sampler,nullptr,&nearestSampler),"create loading nearest sampler");
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            checked(vkCreateSampler(device,&sampler,nullptr,&nearestRepeatSampler),"create loading repeat sampler");
            sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
            checked(vkCreateSampler(device,&sampler,nullptr,&linearRepeatSampler),"create loading linear repeat sampler");
            clearPass = renderPass(true); loadPass = renderPass(false); makePipeline();
            uint32_t white = ~0u; upload(-1,1,1,&white,false,true);
        } catch (...) { close(); throw; }
    }
    ~LoadingRenderer() { close(); }
    void close() {
        if (!device) return;
        if (!mcvr::failure::isDeviceLost()) {
            const VkResult idleResult = vkDeviceWaitIdle(device);
            if (idleResult == VK_ERROR_DEVICE_LOST) {
                mcvr::failure::record(mcvr::failure::Kind::deviceLost, idleResult,
                                      "vkDeviceWaitIdle(LoadingRenderer::close)");
            }
        }
        for (auto &[i,s] : slots) destroySlot(s);
        slots.clear(); textures.clear();
        if (pipeline) vkDestroyPipeline(device,pipeline,nullptr);
        if (copyPipeline) vkDestroyPipeline(device,copyPipeline,nullptr);
        if (clearPass) vkDestroyRenderPass(device,clearPass,nullptr);
        if (loadPass) vkDestroyRenderPass(device,loadPass,nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device,descriptorPool,nullptr);
        if (descriptorLayout) vkDestroyDescriptorSetLayout(device,descriptorLayout,nullptr);
        if (linearSampler) vkDestroySampler(device,linearSampler,nullptr);
        if (nearestSampler) vkDestroySampler(device,nearestSampler,nullptr);
        if (linearRepeatSampler) vkDestroySampler(device,linearRepeatSampler,nullptr);
        if (nearestRepeatSampler) vkDestroySampler(device,nearestRepeatSampler,nullptr);
        if (transferPool) vkDestroyCommandPool(device,transferPool,nullptr);
        device = VK_NULL_HANDLE;
    }
    void upload(int key, int width, int height, void *rgba, bool linear, bool clamp) {
        if (textures.contains(key)) throw std::runtime_error("Loading texture key already uploaded");
        auto staging = vk::HostVisibleBuffer::create(framework->vma(),framework->device(),size_t(width)*height*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        staging->uploadToBuffer(rgba);
        auto img = image(width,height,VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        immediate([&](VkCommandBuffer cmd) {
            barrier(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {uint32_t(width),uint32_t(height),1};
            vkCmdCopyBufferToImage(cmd,staging->vkBuffer(),img->vkImage(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            barrier(cmd,img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        textures.emplace(key,Texture{img,descriptor(img,linear,clamp)});
    }
    void render(const void *vertices, size_t bytes, const Batch *batches, int count, int cw, int ch,
                uint32_t background, float opacity, bool game, bool paintBackground) {
        if (needsAcquire) {
            auto result = framework->acquireContext();
            if (result == VK_NOT_READY) return;
            checked(result,"acquire resumed early frame"); needsAcquire = false;
        }
        auto context = framework->safeAcquireCurrentContext();
        if (!context) throw std::runtime_error("Loading frame has no renderer context");
        auto ui = framework->pipeline()->contexts().at(context->frameIndex)->uiModuleContext;
        auto cmd = context->overlayCommandBuffer->vkCommandBuffer();
        ui->end();
        auto &slot = slots[context->frameIndex];
        if (!slot.canvas.image || slot.canvas.image->width() != cw || slot.canvas.image->height() != ch) {
            checked(vkDeviceWaitIdle(device),"resize loading canvas"); destroySlot(slot);
            slot.canvas.image = image(cw,ch,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            slot.canvas.descriptor = descriptor(slot.canvas.image,false);
            slot.canvasFramebuffer = framebuffer(clearPass,slot.canvas.image);
        }
        if (!slot.vertices || slot.vertices->size() < std::max(size_t(20),bytes))
            slot.vertices = vk::HostVisibleBuffer::create(framework->vma(),framework->device(),std::max(size_t(20),bytes),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (bytes) slot.vertices->uploadToBuffer(const_cast<void *>(vertices),bytes,0);
        barrier(cmd,slot.canvas.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        beginPass(cmd,clearPass,slot.canvasFramebuffer,cw,ch,background);
        VkDeviceSize offset = 0; vkCmdBindVertexBuffers(cmd,0,1,&slot.vertices->vkBuffer(),&offset);
        for (int i=0;i<count;++i) {
            const auto &b = batches[i]; auto texture = textures.find(b.role == 2 ? -1 : b.texture);
            if (texture == textures.end()) throw std::runtime_error("Unknown loading texture key");
            draw(cmd,texture->second.descriptor,b.first,b.count,{float(cw),float(ch),b.role,1});
        }
        vkCmdEndRenderPass(cmd);
        barrier(cmd,slot.canvas.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        auto target = ui->overlayDrawColorImage;
        if (slot.output != target) {
            if (slot.outputFramebuffer) vkDestroyFramebuffer(device,slot.outputFramebuffer,nullptr);
            slot.output = target; slot.outputFramebuffer = framebuffer(loadPass,target);
        }
        const float w = float(target->width()), h = float(target->height());
        const float scale = (float(cw)/854.f)*std::min(w/854.f,h/480.f)/2.f;
        float left = std::clamp(w/2-scale*854.f,0.f,w), right = std::clamp(w/2+scale*854.f,0.f,w);
        float top = std::clamp(h/2-scale*480.f,0.f,h), bottom = std::clamp(h/2+scale*480.f,0.f,h);
        if (!game) {
            // EarlyFramebuffer.glBlitFramebuffer uses integer bounds and bottom-origin destinations.
            left = float(int(left)); right = float(int(right));
            const float oldTop = top;
            top = h - float(int(bottom)); bottom = h - float(int(oldTop));
        }
        const uint32_t alpha = uint32_t(std::clamp(opacity,0.f,1.f)*255.f)<<24;
        std::vector<Vertex> composite;
        auto quad = [&](float x0,float y0,float x1,float y1,uint32_t colour) {
            std::array<Vertex,4> v{{{x0,y0,0,0,colour},{x0,y1,0,1,colour},{x1,y0,1,0,colour},{x1,y1,1,1,colour}}};
            for (int index : {0,1,2,1,3,2}) composite.push_back(v[index]);
        };
        // Preserve the official overlay's actual quad coverage, including its overlapping side strips.
        if (paintBackground && game) {
            auto colour = (background & 0xffffff) | alpha;
            quad(0,0,w,bottom,colour); quad(0,bottom,w,h,colour);
            quad(0,top,left,bottom,colour); quad(right,top,w,bottom,colour);
        }
        const int canvasFirst = int(composite.size()); quad(left,top,right,bottom,0xffffff | alpha);
        if (!slot.composite) slot.composite = vk::HostVisibleBuffer::create(framework->vma(),framework->device(),30*sizeof(Vertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        slot.composite->uploadToBuffer(composite.data(),composite.size()*sizeof(Vertex),0);
        barrier(cmd,target,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        beginPass(cmd,game ? loadPass : clearPass,slot.outputFramebuffer,int(w),int(h),background);
        if (!game) vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,copyPipeline);
        vkCmdBindVertexBuffers(cmd,0,1,&slot.composite->vkBuffer(),&offset);
        if (canvasFirst) draw(cmd,textures.at(-1).descriptor,0,canvasFirst,{w,h,2,opacity});
        draw(cmd,slot.canvas.descriptor,canvasFirst,6,{w,h,1,game ? opacity : 1.f});
        vkCmdEndRenderPass(cmd);
        target->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        context->overlayCommandBuffer->bindDescriptorTable(ui->overlayDescriptorTable,VK_PIPELINE_BIND_POINT_GRAPHICS);
        ui->syncToCommandBuffer();
        lastSlot = context->frameIndex;
        if (!game) {
            checked(framework->submitCommand(),"submit early loading frame");
            needsAcquire = true;
            auto presented = framework->present();
            submitted = true;
            if (presented == VK_NOT_READY) return;
            checked(presented,"present early loading frame");
            auto acquired = framework->acquireContext();
            if (acquired == VK_NOT_READY) return;
            checked(acquired,"acquire next early loading frame"); needsAcquire = false;
        }
    }
    void handoff() {
        framework->nonBlockingResize = false;
        if (needsAcquire) {
            checked(framework->acquireContext(),"acquire handoff frame"); needsAcquire = false;
        }
        checked(framework->waitDeviceIdle(),"wait early ownership transfer");
        framework->nonBlockingResize = true;
    }
    std::vector<unsigned char> capture() {
        if (!submitted || !slots.contains(lastSlot)) throw std::runtime_error("No submitted loading frame to capture");
        checked(vkDeviceWaitIdle(device),"wait loading capture");
        auto canvas = slots.at(lastSlot).canvas.image;
        auto size = size_t(canvas->width())*canvas->height()*4;
        auto buffer = vk::HostVisibleBuffer::create(framework->vma(),framework->device(),size,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        immediate([&](VkCommandBuffer cmd) {
            barrier(cmd,canvas,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {canvas->width(),canvas->height(),1};
            vkCmdCopyImageToBuffer(cmd,canvas->vkImage(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer->vkBuffer(),1,&copy);
            barrier(cmd,canvas,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        buffer->downloadFromBuffer();
        std::vector<unsigned char> result(size); std::memcpy(result.data(),buffer->mappedPtr(),size); return result;
    }
};

std::recursive_mutex loadingMutex;
std::unique_ptr<LoadingRenderer> loading;
bool gameOwnsWindow = false;
bool gameFramesStarted = false;
std::thread::id gameRenderThread;
template<class F> void guarded(JNIEnv *env, F &&operation,
                               jni::BoundaryPolicy policy = jni::BoundaryPolicy::normal) {
    jni::invokeVoid(env, "Radiance loading renderer", [&] {
        std::lock_guard lock(loadingMutex);
        operation();
    }, policy);
}
std::string string(JNIEnv *env, jstring value) {
    if (!value) throw std::runtime_error("Null native string");
    auto result = jni::copyUtf8(env, value);
    if (!result) throw std::runtime_error("Cannot read native string");
    return std::move(*result);
}
}

void releaseLoadingRenderer() {
    std::lock_guard lock(loadingMutex);
    loading.reset();
}

extern "C" {
JNIEXPORT void JNICALL Java_com_radiance_bootstrap_NativeRuntime_initialize(JNIEnv *env,jclass,jstring directory,jobjectArray candidates,jlong handle) {
    if (!Renderer::is_initialized()) mcvr::failure::clearForInitialization();
    guarded(env,[&] {
        if (loading || Renderer::is_initialized()) throw std::runtime_error("Renderer already initialized");
        auto path = jni::copyUtf16(env, directory);
        if (!path) throw std::runtime_error("Cannot read renderer directory");
        Renderer::folderPath = *path;
        if (!bindLoadedGlfw(env,candidates)) return;
        Renderer::init(reinterpret_cast<GLFWwindow *>(intptr_t(handle)));
        auto f = Renderer::instance().framework(); f->nonBlockingResize = true;
        checked(f->acquireContext(),"acquire first early loading frame");
        loading = std::make_unique<LoadingRenderer>(f);
    }, jni::BoundaryPolicy::initialization);
}
JNIEXPORT void JNICALL Java_com_radiance_bootstrap_NativeRuntime_uploadTexture(JNIEnv *env,jclass,jint key,jint w,jint h,jobject pixels,jboolean linear,jboolean clamp) {
    guarded(env,[&] {
        auto data = env->GetDirectBufferAddress(pixels); auto capacity = env->GetDirectBufferCapacity(pixels);
        if (!loading || !data || w<=0 || h<=0 || int64_t(w)*h*4 > capacity) throw std::runtime_error("Invalid loading texture");
        std::lock_guard lock(Renderer::instance().framework()->recreateMtx()); loading->upload(key,w,h,data,linear,clamp);
    });
}
JNIEXPORT void JNICALL Java_com_radiance_bootstrap_NativeRuntime_renderFrame(JNIEnv *env,jclass,jobject vertices,jobject batches,jint count,jint cw,jint ch,jint bg,jint fw,jint fh,jfloat opacity,jboolean game,jboolean paintBackground) {
    guarded(env,[&] {
        auto v = env->GetDirectBufferAddress(vertices); auto b = static_cast<Batch *>(env->GetDirectBufferAddress(batches));
        auto vb = env->GetDirectBufferCapacity(vertices); auto bb = env->GetDirectBufferCapacity(batches);
        if (!loading || !v || !b || count<0 || int64_t(count)*sizeof(Batch)>bb || cw<=0 || ch<=0) throw std::runtime_error("Invalid loading frame buffers");
        if (game && !gameOwnsWindow) throw std::runtime_error("GAME frame before window handoff");
        if (gameOwnsWindow && std::this_thread::get_id() != gameRenderThread)
            throw std::runtime_error("Only the handoff thread may repaint the game window");
        if (!game && gameFramesStarted) throw std::runtime_error("Early repaint after GAME frames started");
        if (fw<=0 || fh<=0) return;
        size_t bytes = 0;
        for (int i=0;i<count;++i) {
            if (b[i].first<0 || b[i].count<0 || b[i].count%3 || b[i].role<0 || b[i].role>2 || (int64_t(b[i].first)+b[i].count)*sizeof(Vertex)>vb)
                throw std::runtime_error("Invalid loading batch range");
            bytes = std::max(bytes,size_t(b[i].first+b[i].count)*sizeof(Vertex));
        }
        std::lock_guard lock(Renderer::instance().framework()->recreateMtx());
        if (game) {
            gameFramesStarted = true;
            Renderer::instance().framework()->nonBlockingResize = false;
        }
        loading->render(v,bytes,b,count,cw,ch,uint32_t(bg),opacity,game,paintBackground);
    });
}
JNIEXPORT void JNICALL Java_com_radiance_bootstrap_NativeRuntime_handoff(JNIEnv *env,jclass,jlong window) {
    guarded(env,[&] {
        if (!loading || Renderer::instance().framework()->window()->window()!=reinterpret_cast<GLFWwindow *>(intptr_t(window))) throw std::runtime_error("Wrong handoff window");
        loading->handoff(); gameRenderThread = std::this_thread::get_id(); gameOwnsWindow = true;
    });
}
JNIEXPORT void JNICALL Java_com_radiance_bootstrap_NativeRuntime_bindGameNatives(JNIEnv *env,jclass,jclass type,jobjectArray names,jobjectArray descriptors,jobjectArray symbols) {
    guarded(env,[&] {
        auto count = env->GetArrayLength(names);
        if (count!=env->GetArrayLength(descriptors) || count!=env->GetArrayLength(symbols)) throw std::runtime_error("JNI registration array length mismatch");
#if defined(_WIN32)
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&Java_com_radiance_bootstrap_NativeRuntime_bindGameNatives),&module)) throw std::runtime_error("Cannot locate core library");
#else
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void *>(&Java_com_radiance_bootstrap_NativeRuntime_bindGameNatives),&info)) throw std::runtime_error("Cannot locate core library");
        auto module = dlopen(info.dli_fname,RTLD_NOW | RTLD_NOLOAD);
#endif
        std::vector<std::string> n,d; std::vector<JNINativeMethod> methods;
        n.reserve(count); d.reserve(count); methods.reserve(count);
        for (int i=0;i<count;++i) {
            jni::LocalRef jn(env, static_cast<jstring>(env->GetObjectArrayElement(names,i)));
            if (env->ExceptionCheck()) throw std::runtime_error("Cannot read JNI method name");
            jni::LocalRef jd(env, static_cast<jstring>(env->GetObjectArrayElement(descriptors,i)));
            if (env->ExceptionCheck()) throw std::runtime_error("Cannot read JNI method descriptor");
            jni::LocalRef js(env, static_cast<jstring>(env->GetObjectArrayElement(symbols,i)));
            if (env->ExceptionCheck()) throw std::runtime_error("Cannot read JNI symbol name");
            n.push_back(string(env,jn.get())); d.push_back(string(env,jd.get())); auto symbol = string(env,js.get());
#if defined(_WIN32)
            auto address = reinterpret_cast<void *>(GetProcAddress(module,symbol.c_str()));
#else
            auto address = dlsym(module,symbol.c_str());
#endif
            if (!address) throw std::runtime_error("Missing game JNI symbol: " + symbol);
            methods.push_back({n.back().data(),d.back().data(),address});
        }
        if (env->RegisterNatives(type,methods.data(),count)!=JNI_OK) throw std::runtime_error("Game JNI registration failed");
    });
}
JNIEXPORT jlongArray JNICALL Java_com_radiance_bootstrap_NativeRuntime_identities(JNIEnv *env,jclass) {
    jlongArray result = nullptr;
    guarded(env,[&] {
        auto f = Renderer::instance().framework();
        jlong values[] = {(jlong)(intptr_t)f->window()->window(),(jlong)(intptr_t)f->instance()->vkInstance(),(jlong)(intptr_t)f->physicalDevice()->vkPhysicalDevice(),(jlong)(intptr_t)f->device()->vkDevice(),(jlong)(intptr_t)f->swapchain()->vkSwapchain()};
        result = env->NewLongArray(5); if (result) env->SetLongArrayRegion(result,0,5,values);
    }); return result;
}
JNIEXPORT jbyteArray JNICALL Java_com_radiance_bootstrap_NativeRuntime_captureCanvas(JNIEnv *env,jclass) {
    jbyteArray result = nullptr;
    guarded(env,[&] {
        if (!loading) throw std::runtime_error("Loading resources are released");
        std::lock_guard lock(Renderer::instance().framework()->recreateMtx()); auto pixels = loading->capture();
        result = env->NewByteArray(jsize(pixels.size())); if (result) env->SetByteArrayRegion(result,0,jsize(pixels.size()),reinterpret_cast<const jbyte *>(pixels.data()));
    }); return result;
}
JNIEXPORT void JNICALL Java_com_radiance_bootstrap_NativeRuntime_releaseLoading(JNIEnv *env,jclass) {
    guarded(env,[]{ loading.reset(); }, jni::BoundaryPolicy::allowAfterFatal);
}
JNIEXPORT void JNICALL Java_com_radiance_bootstrap_NativeRuntime_closeBeforeGame(JNIEnv *env,jclass) {
    guarded(env,[]{ if (gameOwnsWindow) return; loading.reset(); if (Renderer::is_initialized()) Renderer::instance().close(); },
            jni::BoundaryPolicy::allowAfterFatal);
}
}
