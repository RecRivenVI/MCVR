#include "core/logging.hpp"
#include <jni.h>
#include "common/shared.hpp"
#include "core/middleware/jni_exception.hpp"
#include "core/middleware/jni_string.hpp"
#include "core/render/renderer.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/ponder_budget.hpp"
#include "core/render/ui_frame_batch.hpp"
#include "core/render/ui_geometry_cache.hpp"
#include "core/render/ui_history_retirement.hpp"
#include "core/render/scene_scope.hpp"
#include "core/render/scene_camera.hpp"
#include "core/render/world.hpp"
#include "core/render/entities.hpp"
#include "core/render/buffers.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/modules/world/ray_tracing/ray_tracing_module.hpp"
#include "core/render/textures.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <typeinfo>


struct PonderCachedGeometry {
    std::vector<vk::VertexFormat::PBRVertex> vertices;
    std::vector<int> counts;
    std::vector<int> faces;
    std::vector<std::string> names;
    std::shared_ptr<Entity> entity;
};

class PonderSceneRenderer {
public:
    struct CompositeFrame {
        std::shared_ptr<vk::DeviceLocalImage> output;
        std::shared_ptr<vk::DescriptorTable> descriptor;
    };
    uint64_t id = 0;
    uint32_t slot = 0;
    uint32_t width = 0, height = 0;
    glm::mat4 pose{1}, view{1}, projection{1};
    std::vector<vk::VertexFormat::PBRVertex> vertices;
    std::vector<int> counts;
    std::vector<int> faces;
    std::vector<std::string> names;
    std::shared_ptr<Buffers> buffers;
    std::shared_ptr<vk::DeviceLocalImage> target;
    std::shared_ptr<vk::Sampler> sampler;
    std::vector<CompositeFrame> compositeFrames;
};

class PonderSharedWorld : public std::enable_shared_from_this<PonderSharedWorld> {
public:
    std::shared_ptr<World> world;
    SceneRecordingState state;
    std::shared_ptr<WorldPipeline> pipeline;
    std::weak_ptr<WorldPipeline> parentPipeline;
    std::map<uint64_t, PonderCachedGeometry> geometries;
    std::vector<uint64_t> collected;
    std::vector<uint64_t> lastParticipants;
    mcvr::ui::FrameBatch batch;
    uint32_t width = 0, height = 0;
    uint64_t anchor = 0, frame = 0;
    std::weak_ptr<FrameworkContext> lastRecordingContext;
    uint64_t lastRecordingGeneration = 0;
    bool ready = false;
    void execute(const std::shared_ptr<UIModule> &module, const std::shared_ptr<FrameworkContext> &mainFrame);
    void close() {
        if (!ready) return;
        ready = false;
        for (auto &part : pipeline->worldModules()) part->preClose();
    }
    ~PonderSharedWorld() { close(); }
};

void UIModule::closePonderScenes() {
    for (auto &weak : ponderSceneLifetimes) if (auto engine = weak.lock()) engine->close();
    ponderScenes.clear();
    ponderSceneLifetimes.clear();
    ponderSharedWorld.reset();
    uiPtCollecting = false;
}

namespace {
void retireReconstructionViews(const std::shared_ptr<UIModule> &module,
                               const std::shared_ptr<Framework> &framework,
                               const std::shared_ptr<FrameworkContext> &current) {
    mcvr::ui::retireHistories(module->ponderSceneLifetimes, [&](const auto &old) {
        return old.lastRecordingContext.lock() == current &&
            old.lastRecordingGeneration == current->uiPtRecordingGeneration;
    }, [&] {
    for (const auto &pending : framework->contexts()) {
        if (!pending->frameSubmitted) continue;
        const auto fence = pending->commandFinishedFence->vkFence();
        const auto result = vkWaitForFences(framework->device()->vkDevice(), 1, &fence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            framework->recordFailure(result, "vkWaitForFences(UI PT replacement)");
            mcvr::failure::throwIfFatal();
        }
    }
    }, [](auto &old) { old.close(); });
}

void transition(const std::shared_ptr<vk::CommandBuffer> &commands,
                const std::shared_ptr<vk::DeviceLocalImage> &image, VkImageLayout layout) {
    commands->barriersBufferImage({}, {{
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = image->imageLayout(), .newLayout = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = image->fullSubresourceRange(),
    }});
    image->imageLayout() = layout;
}
}

extern "C" JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_UiPathTracingProxy_trace(
    JNIEnv *env, jclass, jlong sceneId, jlong source, jint count, jlong matrices,
    jint targetId, jint width, jint height, jint sequence, jobjectArray groups, jintArray counts, jintArray faces) {
    jni::invokeVoid(env, "Ponder world pipeline", [&] {
        if (!source || !matrices || count <= 0 || count > 1000000 || width <= 0 || height <= 0)
            throw std::invalid_argument("Invalid Ponder scene");
        if (!groups || !counts || !faces || env->GetArrayLength(faces) != env->GetArrayLength(counts) || env->GetArrayLength(groups) != env->GetArrayLength(counts))
            throw std::invalid_argument("Invalid Ponder geometry groups");
        int geometryCount = env->GetArrayLength(groups);
        if (geometryCount <= 0) throw std::invalid_argument("Empty Ponder geometry groups");
        std::vector<int> vertexCounts(geometryCount), faceStates(geometryCount);
        env->GetIntArrayRegion(faces, 0, geometryCount, faceStates.data());
        if (env->ExceptionCheck()) return;
        env->GetIntArrayRegion(counts, 0, geometryCount, vertexCounts.data());
        std::vector<std::string> groupNames;
        groupNames.reserve(geometryCount);
        int64_t totalVertices = 0;
        for (int i = 0; i < geometryCount; ++i) {
            if (vertexCounts[i] <= 0 || vertexCounts[i] % 3) throw std::invalid_argument("Invalid Ponder triangle group");
            totalVertices += vertexCounts[i];
            jni::LocalRef name(env, static_cast<jstring>(env->GetObjectArrayElement(groups, i)));
            if (env->ExceptionCheck()) throw std::runtime_error("Cannot read Ponder geometry name");
            if (!name) throw std::invalid_argument("Null Ponder geometry name");
            const auto utf = jni::copyUtf8(env, name.get());
            if (!utf) throw std::runtime_error("Cannot read Ponder geometry name");
            groupNames.push_back(*utf);
        }
        if (totalVertices != int64_t(count) * 3) throw std::invalid_argument("Ponder triangle count mismatch");

        auto &renderer = Renderer::instance();
        auto framework = renderer.framework();
        std::lock_guard lock(framework->recreateMtx());
        auto mainFrame = framework->safeAcquireCurrentContext();
        auto module = framework->pipeline()->uiModule();
        if (!module->uiPtCollecting || !module->ponderSharedWorld)
            throw std::logic_error("UI PT collect requires an open batch");
        auto &engine = *module->ponderSharedWorld;
        if (engine.collected.size() >= 2 || std::find(engine.collected.begin(), engine.collected.end(), sceneId) != engine.collected.end())
            throw std::logic_error("UI PT batch allows two distinct views; repeated views require a distinct view id");
        engine.batch.receive(sceneId);
        engine.collected.push_back(sceneId);
        auto &scene = module->ponderScenes[sceneId];
        if (!scene) {
            uint32_t slot = 0;
            for (const auto &[id, view] : module->ponderScenes) if (view && view->slot == slot) ++slot;
            if (slot >= 2) throw std::logic_error("UI PT scene slots were not retired before collection");
            scene = std::make_shared<PonderSceneRenderer>();
            scene->id = sceneId; scene->slot = slot;
            ++module->uiPtViewCreates;
        }
        if (scene->width != static_cast<uint32_t>(width) || scene->height != static_cast<uint32_t>(height)) {
            for (auto &old : scene->compositeFrames) {
                framework->frameResourceRetainer().retain(old.output);
                framework->frameResourceRetainer().retain(old.descriptor);
            }
            scene->compositeFrames.assign(framework->swapchain()->imageCount(), {});
        }
        scene->width = width; scene->height = height;
        auto values = reinterpret_cast<const float *>(matrices);
        std::memcpy(&scene->pose, values, sizeof(glm::mat4));
        std::memcpy(&scene->view, values + 16, sizeof(glm::mat4));
        std::memcpy(&scene->projection, values + 32, sizeof(glm::mat4));
        scene->vertices.resize(static_cast<size_t>(count) * 3);
        std::memcpy(scene->vertices.data(), reinterpret_cast<void *>(source), scene->vertices.size() * sizeof(scene->vertices[0]));
        scene->counts = std::move(vertexCounts);
        scene->faces = std::move(faceStates);
        scene->names = std::move(groupNames);
        // Capture native ownership while Java's texture-owner monitor still covers this call.
        scene->target = renderer.textures()->texture(targetId);
        scene->sampler = renderer.textures()->sampler(targetId);
        if (!scene->target || !scene->sampler) throw std::runtime_error("UI PT output texture is unavailable");
        framework->frameResourceRetainer().retain(scene->target);
        framework->frameResourceRetainer().retain(scene->sampler);
    });
}

extern "C" JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_UiPathTracingProxy_beginFrame(
    JNIEnv *env, jclass, jlongArray visibleIds) {
    jni::invokeVoid(env, "Begin UI PT collection", [&] {
        auto framework = Renderer::instance().framework();
        std::lock_guard lock(framework->recreateMtx());
        auto module = framework->pipeline()->uiModule();
        if (module->uiPtCollecting) throw std::logic_error("Nested UI PT batch");
        if (!visibleIds || env->GetArrayLength(visibleIds) > 2) throw std::invalid_argument("Invalid UI PT view set");
        std::vector<jlong> ids(env->GetArrayLength(visibleIds));
        env->GetLongArrayRegion(visibleIds, 0, ids.size(), ids.data());
        if (env->ExceptionCheck()) return;
        for (auto it = module->ponderScenes.begin(); it != module->ponderScenes.end();) {
            if (std::find(ids.begin(), ids.end(), static_cast<jlong>(it->first)) != ids.end()) { ++it; continue; }
            framework->frameResourceRetainer().retain(it->second);
            if (module->ponderSharedWorld) module->ponderSharedWorld->geometries.erase(it->first);
            it = module->ponderScenes.erase(it);
        }
        if (!module->ponderSharedWorld) {
            module->ponderSharedWorld = std::make_shared<PonderSharedWorld>();
            std::erase_if(module->ponderSceneLifetimes, [](const auto &weak) { return weak.expired(); });
            module->ponderSceneLifetimes.push_back(module->ponderSharedWorld);
        }
        std::vector<uint64_t> views(ids.begin(), ids.end());
        module->ponderSharedWorld->batch.begin(views);
        module->ponderSharedWorld->collected.clear();
        module->uiPtCollecting = true;
    });
}

extern "C" JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_UiPathTracingProxy_endFrame(
    JNIEnv *env, jclass, jboolean commit) {
    jni::invokeVoid(env, "Execute UI PT batch", [&] {
        auto framework = Renderer::instance().framework();
        std::lock_guard lock(framework->recreateMtx());
        auto module = framework->pipeline()->uiModule();
        if (!module->uiPtCollecting) throw std::logic_error("UI PT batch is not open");
        module->uiPtCollecting = false;
        module->ponderSharedWorld->batch.finish(commit, [&] {
            if (!module->ponderSharedWorld->collected.empty())
                module->ponderSharedWorld->execute(module, framework->safeAcquireCurrentContext());
        });
        module->ponderSharedWorld->collected.clear();
    });
}

void PonderSharedWorld::execute(const std::shared_ptr<UIModule> &module,
                               const std::shared_ptr<FrameworkContext> &mainFrame) {
    auto &renderer = Renderer::instance();
    auto framework = renderer.framework();
    auto mainPipeline = framework->pipeline()->worldPipeline();
    auto mainBuffers = renderer.buffers();
    if (!mainPipeline || !mainBuffers->worldUniformBuffer() || !mainBuffers->skyUniformBuffer() || !mainBuffers->textureMappingBuffer())
        throw std::runtime_error("UI PT requires world material and lighting uniforms");
    mainFrame->beginUiPtCommands(); // Uploads and cached-world execution below record here.
    auto sourceUbo = *static_cast<vk::Data::WorldUBO *>(mainBuffers->worldUniformBuffer()->mappedPtr());
    auto sky = *static_cast<vk::Data::SkyUBO *>(mainBuffers->skyUniformBuffer()->mappedPtr());
    auto mapping = *static_cast<vk::Data::TextureMapping *>(mainBuffers->textureMappingBuffer()->mappedPtr());
    uint32_t requestedWidth = 0, requestedHeight = 0;
    for (auto id : collected) {
        auto scene = module->ponderScenes.at(id);
        requestedWidth = std::max(requestedWidth, scene->width);
        requestedHeight = std::max(requestedHeight, scene->height);
    }
    auto extent = mcvr::ponder::boundedExtent(requestedWidth, requestedHeight);
    if (pipeline && (parentPipeline.lock() != mainPipeline || extent.width > width || extent.height > height)) {
        // Streamline has a finite number of live reconstruction viewports. Keeping
        // old image objects until retirement is safe, but retaining their SDK
        // histories while creating two more viewports exceeds that limit.
        // Wait only at this rare capacity/parent replacement boundary, on actual
        // submitted frame fences (never destroy an in-flight SDK history).
        retireReconstructionViews(module, framework, mainFrame);
        auto next = std::make_shared<PonderSharedWorld>();
        next->collected = collected;
        next->geometries = std::move(geometries);
        module->ponderSharedWorld = next;
        std::erase_if(module->ponderSceneLifetimes, [](const auto &weak) { return weak.expired(); });
        module->ponderSceneLifetimes.push_back(next);
        framework->frameResourceRetainer().retain(shared_from_this());
        for (auto &[id, view] : module->ponderScenes) view->buffers.reset();
        next->execute(module, mainFrame);
        return;
    }
    if (!pipeline) retireReconstructionViews(module, framework, mainFrame);
    const uint32_t frames = framework->swapchain()->imageCount();
    if (state.contexts.empty()) {
        state.viewCount = 2;
        world = World::create(framework); state.world = world;
        for (uint32_t slot = 0; slot < 2 * frames; ++slot) {
            auto context = FrameworkContext::create(framework, slot % frames);
            context->frameIndex = slot;
            context->worldCommandBuffer = framework->contexts()[slot % frames]->uiPtCommandStorage();
            context->uploadCommandBuffer = context->worldCommandBuffer;
            context->overlayCommandBuffer = context->worldCommandBuffer;
            state.contexts.push_back(context);
        }
        width = extent.width; height = extent.height;
    }
    auto first = module->ponderScenes.at(collected.front());
    lastRecordingContext = mainFrame;
    lastRecordingGeneration = mainFrame->uiPtRecordingGeneration;
    glm::mat4 basePose = first->pose;
    // Anchor all worlds in one physical coordinate system, preserving THIS frame's relative transforms.
    bool reset = anchor != first->id;
    anchor = first->id;
    SceneRecordingScope scope(state);
    for (auto &[id, scene] : module->ponderScenes) {
        if (!scene->buffers) { scene->buffers = Buffers::create(framework); reset = true; }
    }
    state.buffers = first->buffers;
    state.current = state.contexts.at(first->slot * frames + mainFrame->frameIndex);
    if (!pipeline) {
        for (uint32_t slot = 0; slot < 2 * frames; ++slot) {
            state.current = state.contexts[slot];
            for (auto &[id, scene] : module->ponderScenes) {
                scene->buffers->setAndUploadWorldUniformBuffer(sourceUbo);
                scene->buffers->setAndUploadSkyUniformBuffer(sky);
                scene->buffers->setAndUploadTextureMappingBuffer(mapping);
            }
        }
        state.current = state.contexts.at(first->slot * frames + mainFrame->frameIndex);
        pipeline = WorldPipeline::create();
        parentPipeline = mainPipeline;
        pipeline->init(framework, framework->pipeline(), nullptr, {}, {width, height});
        ready = true; ++module->uiPtPipelineCreates;
        reset = true;
    }
    if (reset) {
        pipeline->onResourceReload();
        for (auto &[id, scene] : module->ponderScenes) scene->buffers->invalidateWorldHistory();
    }
    // Start the geometry-upload batch before Entities::build queues its payloads.
    state.buffers->resetFrame();
    std::vector<std::shared_ptr<Entity>> combined;
    for (auto id : collected) {
        auto scene = module->ponderScenes.at(id);
        const auto inversePose = glm::inverse(scene->pose);
        const auto normalTransform = glm::transpose(glm::mat3(scene->pose));
        for (auto &vertex : scene->vertices) {
            vertex.pos = glm::vec3(inversePose * glm::vec4(vertex.pos, 1));
            if (vertex.useNorm && glm::length(vertex.norm) > 1e-6f) vertex.norm = glm::normalize(normalTransform * vertex.norm);
            vertex.coordinate = World::WORLD;
        }
        auto &geometry = geometries[id];
        if (!geometry.entity || geometry.counts != scene->counts || geometry.names != scene->names || geometry.faces != scene->faces ||
            !mcvr::ui::equivalentGeometry(geometry.vertices, scene->vertices)) {
            int geometryCount = scene->counts.size();
            std::vector<int> types(geometryCount, World::WORLD_SOLID), textures(geometryCount, 0);
            std::vector<int> formats(geometryCount, World::PBR_TRIANGLE), modes(geometryCount, static_cast<int>(World::DrawMode::TRIANGLES));
            std::vector<void *> data;
            std::vector<const char *> names, contents(geometryCount, "radiance:ui/scene");
            size_t offset = 0;
            for (int i = 0; i < geometryCount; ++i) {
                data.push_back(scene->vertices.data() + offset); names.push_back(scene->names[i].c_str());
                for (int j = 0; j < scene->counts[i]; ++j) if (scene->vertices[offset+j].alphaMode != 0) types[i] = World::WORLD_TRANSPARENT;
                types[i] |= scene->faces[i] << 8;
                offset += scene->counts[i];
            }
            int hashCode = scene->slot + 1, flag = 1, zero = 0, prebuilt = -1;
            double origin = 0;
            EntitiesBuildTask task{};
            task.coordinate = World::WORLD; task.entityCount = 1;
            task.entityHashCodes = &hashCode; task.entityXs = task.entityYs = task.entityZs = &origin;
            task.entityRayTracingFlags = &flag; task.entityPostRenderFlags = &zero;
            task.entityPrebuiltBLASs = &prebuilt; task.entityPosts = &zero;
            task.entityGeometryCounts = &geometryCount; task.geometryTypes = types.data();
            task.geometryGroupNames = names.data(); task.geometryContentNames = contents.data();
            task.geometryTextures = textures.data(); task.vertexFormats = formats.data();
            task.indexFormats = modes.data(); task.vertexCounts = scene->counts.data(); task.vertices = data.data();
            world->entities()->resetFrame(); world->entities()->queueBuild(task);
            std::shared_ptr<Entity> materialUpdate;
            if (geometry.entity && geometry.counts == scene->counts && geometry.names == scene->names && geometry.faces == scene->faces &&
                mcvr::ui::equivalentPositions(geometry.vertices, scene->vertices))
                materialUpdate = world->entities()->buildUiMaterialUpdate(geometry.entity);
            if (materialUpdate) {
                geometry.entity = std::move(materialUpdate);
                ++module->uiPtMaterialUpdates;
            } else {
                world->entities()->build();
                geometry.entity = world->entities()->entityBatch()->entities.at(0);
                ++module->uiPtGeometryBuilds;
            }
            state.buffers->performQueuedUpload();
            if (auto builder = world->entities()->blasBatchBuilder()) builder->submit(mainFrame->beginUiPtCommands());
            geometry.vertices = scene->vertices; geometry.counts = scene->counts; geometry.names = scene->names; geometry.faces = scene->faces;
        }
        auto instance = std::make_shared<Entity>(*geometry.entity);
        instance->instanceTransform = glm::inverse(basePose) * scene->pose;
        instance->uiSceneOwner = scene->slot + 1;
        combined.push_back(instance);
    }
    world->entities()->publishCachedEntities(std::move(combined));
    world->setCameraPos(glm::dvec3(0));
    state.batchTlas.reset(); state.batchTlasBuilds = 0;
    state.resetHistory = reset;
    const bool participantsChanged = collected != lastParticipants;
    for (auto id : collected) {
        auto scene = module->ponderScenes.at(id);
        state.current = state.contexts.at(scene->slot * frames + mainFrame->frameIndex);
        state.buffers = scene->buffers;
        auto buffers = scene->buffers;
        auto ubo = sourceUbo;
        auto view = scene->view * glm::inverse(scene->pose) * basePose;
        auto projection = scene->projection;
        normalizeSceneCamera(view, projection);
        ubo.cameraViewMat = view; ubo.cameraEffectedViewMat = view; ubo.cameraProjMat = projection;
        ubo.isFirstPerson = 0; ubo.uiPrimaryOwner = scene->slot + 1;
        ubo.fogStart = 1e8f; ubo.fogEnd = 1e9f; ubo.cameraJitter = glm::vec2(0);
        ubo.chunkGridInfo = glm::ivec4(0); ubo.chunkStorageSectionPos = glm::ivec4(0);
        buffers->resetFrame(); buffers->setAndUploadWorldUniformBuffer(ubo);
        buffers->setAndUploadSkyUniformBuffer(sky); buffers->setAndUploadTextureMappingBuffer(mapping);
        buffers->performQueuedUpload();
        renderer.textures()->bindWorldTextures(pipeline);
        mainFrame->beginUiPtCommands()->barriersMemory({{
            .srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT}});
        auto pipelineContext = pipeline->contexts().at(state.current->frameIndex);
        pipelineContext->render();
        auto width = scene->width, height = scene->height;
        if (frame < 3 || frame % 300 == 0 || participantsChanged)
            mcvr::log::info("UiPathTracing") << "view=" << id << " historySlot=" << scene->slot
                << " inputViewport=" << width << 'x' << height << " traceExtent=" << this->width << 'x' << this->height
                << " cameraReset=" << reset << std::endl;
        std::shared_ptr<vk::DeviceLocalImage> depth;
        for (const auto &context : pipelineContext->worldModuleContexts) {
            if (auto rt = std::dynamic_pointer_cast<RayTracingModuleContext>(context)) depth = rt->firstHitDepthImage;
        }
        if (!depth) throw std::runtime_error("Ponder world pipeline has no primary hit depth");
        auto color = pipelineContext->outputImage;
        auto device = framework->device();
        auto &composite = scene->compositeFrames.at(mainFrame->frameIndex);
        if (!composite.output) {
            ++module->uiPtCompositeCreates;
            composite.output = vk::DeviceLocalImage::create(device, framework->vma(),
                static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1,
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            vk::DescriptorTableBuilder builder;
            auto &bindings = builder.beginDescriptorLayoutSet().beginDescriptorLayoutSetBinding();
            bindings.defineDescriptorLayoutSetBinding({0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                1, VK_SHADER_STAGE_COMPUTE_BIT});
            bindings.defineDescriptorLayoutSetBinding({1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                1, VK_SHADER_STAGE_COMPUTE_BIT});
            bindings.defineDescriptorLayoutSetBinding({2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT});
            bindings.endDescriptorLayoutSetBinding().endDescriptorLayoutSet();
            builder.definePushConstant({VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(glm::vec2)});
            composite.descriptor = builder.build(device);
        }
        auto output = composite.output;
        auto descriptor = composite.descriptor;
        auto commands = mainFrame->beginUiPtCommands();
        transition(commands, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(commands, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(commands, output, VK_IMAGE_LAYOUT_GENERAL);
        auto sampler = scene->sampler;
        descriptor->bindSamplerImage(sampler, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0, 0);
        descriptor->bindSamplerImage(sampler, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0);
        descriptor->bindImage(output, VK_IMAGE_LAYOUT_GENERAL, 0, 2);
        if (!module->ponderPathPipeline) {
            auto shader = vk::Shader::create(device, (Renderer::folderPath / "shaders/preview/ponder_composite_comp.spv").string());
            module->ponderPathPipeline = vk::ComputePipelineBuilder{}.defineShader(shader).definePipelineLayout(descriptor).build(device);
        }
        auto command = commands->vkCommandBuffer();
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, module->ponderPathPipeline->vkPipeline());
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, descriptor->vkPipelineLayout(), 0, 1,
            descriptor->descriptorSet().data(), 0, nullptr);
        // The color output has been temporally resolved, whereas primary depth
        // still uses the input pixel jitter. Align its coverage to output pixels.
        vkCmdPushConstants(command, descriptor->vkPipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(glm::vec2), &ubo.cameraJitter);
        vkCmdDispatch(command, (width + 7) / 8, (height + 7) / 8, 1);
        auto target = scene->target;
        transition(commands, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.dstSubresource = copy.srcSubresource;
        copy.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        vkCmdCopyImage(command, output->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            target->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(commands, target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        auto &retainer = framework->frameResourceRetainer();
        retainer.retain(shared_from_this()); retainer.retain(scene); retainer.retain(target);
        retainer.retain(module->ponderPathPipeline);

    }
    ++frame;
    if (frame <= 3 || frame % 300 == 0 || participantsChanged) {
        mcvr::log::info("UiPathTracing") << "frame=" << frame << " views=" << collected.size()
            << " scene0=" << collected[0] << " scene1=" << (collected.size()>1 ? collected[1] : 0)
            << " pipelines=" << (pipeline ? 1 : 0)
            << " livePipelines=" << std::count_if(module->ponderSceneLifetimes.begin(), module->ponderSceneLifetimes.end(),
                [](const auto &weak) { auto engine = weak.lock(); return engine && engine->pipeline; })
            << " pipelineCreates=" << module->uiPtPipelineCreates
            << " compositeCreates=" << module->uiPtCompositeCreates
            << " uniqueDispatchImages=" << pipeline->uniqueDispatchImageCount()
            << " slots=" << state.contexts.size() << " extent=" << width << 'x' << height
            << " batchTlasBuilds=" << state.batchTlasBuilds
            << " geometryBuilds=" << module->uiPtGeometryBuilds
            << " materialUpdates=" << module->uiPtMaterialUpdates << std::endl;
    }
    lastParticipants = collected;
}

extern "C" JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_UiPathTracingProxy_releaseScenes(JNIEnv *env, jclass) {
    jni::invokeVoid(env, "Release UI PT scenes", [&] {
        auto framework = Renderer::instance().framework();
        std::lock_guard lock(framework->recreateMtx());
        auto module = framework->pipeline()->uiModule();
        mcvr::log::info("UiPathTracing") << "Releasing " << module->resourceDiagnostics() << std::endl;
        framework->frameResourceRetainer().retain(module->ponderSharedWorld);
        for (auto &[id, scene] : module->ponderScenes) framework->frameResourceRetainer().retain(scene);
        module->ponderScenes.clear(); module->ponderSharedWorld.reset(); module->uiPtCollecting = false;
    });
}

extern "C" JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_UiPathTracingProxy_emptyView(JNIEnv *env, jclass, jlong id) {
    jni::invokeVoid(env, "Empty UI PT participant", [&] {
        auto framework = Renderer::instance().framework();
        std::lock_guard lock(framework->recreateMtx());
        auto module = framework->pipeline()->uiModule();
        if (!module->uiPtCollecting) throw std::logic_error("No UI PT batch");
        module->ponderSharedWorld->batch.receive(id);
    });
}
