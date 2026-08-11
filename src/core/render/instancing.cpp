#include "core/render/instancing.hpp"
#include "core/render/instancing_contract.hpp"

#include "core/render/buffers.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>
#include <utility>

namespace {
struct CanonicalVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 color;
    int32_t overlay;
    int32_t light;
};
static_assert(sizeof(CanonicalVertex) == 56);

constexpr uint32_t COLOR_MULTIPLY = 1u;
constexpr uint32_t COLOR_REPLACE = 1u << 1u;
constexpr uint32_t UV_OFFSET = 1u << 2u;
constexpr uint32_t FLUID_UV = 1u << 3u;
constexpr uint32_t SHADOW_UV = 1u << 4u;
constexpr uint32_t OVERRIDE_OVERLAY = 1u << 5u;
constexpr uint32_t OVERRIDE_LIGHT = 1u << 6u;
constexpr uint32_t OVERRIDE_TEXTURE = 1u << 7u;
constexpr uint32_t CRUMBLING = 1u << 8u;
constexpr uint32_t EMBEDDED = 1u << 10u;
constexpr uint32_t CONSTANT_AMBIENT_LIGHT = 1u << 11u;
constexpr uint32_t MATERIAL_POLYGON_OFFSET = 1u << 3u;
constexpr uint32_t MATERIAL_TRANSPARENCY_SHIFT = 16u;

glm::vec4 normalizedColor(const uint8_t *p) {
    return glm::vec4(p[0], p[1], p[2], p[3]) / 255.0f;
}
glm::ivec2 packedShorts(const uint8_t *p) {
    int32_t value;
    std::memcpy(&value, p, sizeof(value));
    return {value & 0xffff, (value >> 16) & 0xffff};
}
float f32(const std::vector<uint8_t> &data, size_t offset) {
    float out;
    std::memcpy(&out, data.data() + offset, sizeof(out));
    return out;
}
glm::vec2 vec2(const std::vector<uint8_t> &data, size_t offset) {
    return {f32(data, offset), f32(data, offset + 4)};
}
glm::vec3 vec3(const std::vector<uint8_t> &data, size_t offset) {
    return {f32(data, offset), f32(data, offset + 4), f32(data, offset + 8)};
}
glm::quat quat(const std::vector<uint8_t> &data, size_t offset) {
    return {f32(data, offset + 12), f32(data, offset), f32(data, offset + 4), f32(data, offset + 8)};
}
glm::mat4 mat4(const std::vector<uint8_t> &data, size_t offset) {
    return glm::make_mat4(reinterpret_cast<const float *>(data.data() + offset));
}

std::pair<glm::mat4, InstancingAppearance> decode(const InstancingInstance &instance,
                                                  double renderTicks, glm::ivec3 renderOrigin) {
    const auto &d = instance.data;
    glm::mat4 transform(1.0f);
    InstancingAppearance appearance{};
    appearance.colorMultiply = glm::vec4(1.0f);
    appearance.colorReplace = glm::vec4(1.0f);
    appearance.uv = glm::vec4(0.0f);
    appearance.shadow = glm::vec4(0.0f);
    appearance.overlay = glm::ivec2(0);
    appearance.light = glm::ivec2(0);
    appearance.flags = 0;
    appearance.textureOverride = 0;
    appearance.fluidProgress = 1.0f;
    appearance.lightScene = 0;
    auto colored = [&](size_t color, size_t light, size_t overlay, bool multiply = true) {
        if (multiply) {
            appearance.flags |= COLOR_MULTIPLY;
            appearance.colorMultiply = normalizedColor(d.data() + color);
        }
        appearance.flags |= OVERRIDE_LIGHT | OVERRIDE_OVERLAY;
        appearance.light = packedShorts(d.data() + light);
        appearance.overlay = packedShorts(d.data() + overlay);
    };
    switch (instance.adapter) {
        case 0: colored(0, 8, 4); transform = mat4(d, 12); break;
        case 1: colored(0, 8, 4); transform = mat4(d, 12); break;
        case 2: {
            colored(0, 8, 4);
            auto position = vec3(d, 12), pivot = vec3(d, 24);
            transform = glm::translate(glm::mat4(1), position + pivot) * glm::mat4_cast(quat(d, 36)) *
                        glm::translate(glm::mat4(1), -pivot);
            break;
        }
        case 3: {
            auto pos = vec3(d, 0);
            auto size = vec2(d, 20);
            transform = glm::translate(glm::mat4(1), pos) *
                        glm::scale(glm::mat4(1), glm::vec3(size.x, 1.0f, size.y));
            appearance.flags |= SHADOW_UV;
            appearance.uv = {size.x, size.y, pos.x - f32(d, 12), pos.z - f32(d, 16)};
            appearance.shadow = {f32(d, 32), f32(d, 28), 0.0f, 0.0f};
            break;
        }
        case 4: {
            colored(0, 4, 8);
            auto pos = vec3(d, 28);
            glm::vec3 axis(static_cast<int8_t>(d[48]) / 127.0f,
                           static_cast<int8_t>(d[49]) / 127.0f,
                           static_cast<int8_t>(d[50]) / 127.0f);
            float degrees = f32(d, 44) + static_cast<float>(renderTicks / 20.0 * f32(d, 40));
            glm::quat kinetic = glm::angleAxis(glm::radians(degrees), glm::normalize(axis));
            transform = glm::translate(glm::mat4(1), pos + glm::vec3(0.5f)) *
                        glm::mat4_cast(kinetic * quat(d, 12)) *
                        glm::translate(glm::mat4(1), glm::vec3(-0.5f));
            break;
        }
        case 5: {
            colored(0, 4, 8, false);
            auto pos = vec3(d, 12);
            transform = glm::translate(glm::mat4(1), pos + glm::vec3(0.5f)) *
                        glm::mat4_cast(quat(d, 24)) *
                        glm::translate(glm::mat4(1), glm::vec3(-0.5f));
            glm::vec2 speed = vec2(d, 40), diff = vec2(d, 48), scale = vec2(d, 56), offset = vec2(d, 64);
            glm::vec2 scroll = glm::fract(speed * static_cast<float>(renderTicks) + offset) * scale;
            appearance.flags |= UV_OFFSET;
            appearance.uv = {diff + scroll, 0, 0};
            break;
        }
        case 6: {
            colored(64, 68, 72, false);
            transform = mat4(d, 0);
            glm::vec2 speed = vec2(d, 76), diff = vec2(d, 84), scale = vec2(d, 92), offset = vec2(d, 100);
            glm::vec2 scroll = glm::fract(speed * static_cast<float>(renderTicks) + offset) * scale;
            appearance.flags |= UV_OFFSET;
            appearance.uv = {diff + scroll, 0, 0};
            break;
        }
        case 7: {
            colored(64, 68, 72, false);
            appearance.flags |= COLOR_REPLACE | FLUID_UV;
            appearance.colorReplace = normalizedColor(d.data() + 64);
            appearance.fluidProgress = f32(d, 76);
            appearance.uv.z = f32(d, 80);
            appearance.uv.w = f32(d, 84);
            transform = mat4(d, 0) * glm::scale(glm::mat4(1), glm::vec3(1, appearance.fluidProgress, 1));
            break;
        }
        default: throw std::runtime_error("Unsupported Flywheel instance adapter");
    }
    glm::mat3 localNormal;
    if (instance.adapter == 1) localNormal = glm::make_mat3(reinterpret_cast<const float *>(d.data() + 76));
    else if (instance.adapter == 3) localNormal = glm::mat3(1.0f);
    else if (instance.adapter == 7) localNormal = glm::transpose(glm::inverse(glm::mat3(mat4(d, 0))));
    else localNormal = glm::transpose(glm::inverse(glm::mat3(transform)));
    appearance.crumblingTransform = transform;
    appearance.crumblingNormal0 = glm::vec4(localNormal[0], 0.0f);
    appearance.crumblingNormal1 = glm::vec4(localNormal[1], 0.0f);
    appearance.crumblingNormal2 = glm::vec4(localNormal[2], 0.0f);
    if (instance.explicitLighting) {
        appearance.lightTransform = mcvr::instancing::instanceLightTransform(
            instance.lightScene, renderOrigin, instance.lightSceneMatrix, transform);
        appearance.flags |= 1u << 12u;
        appearance.lightScene = instance.lightScene;
    }
    transform = instance.embeddingPose * transform;
    const auto correction = mcvr::instancing::normalCorrection(transform, instance.embeddingNormal * localNormal);
    appearance.normalCorrection0 = glm::vec4(correction[0], 0.0f);
    appearance.normalCorrection1 = glm::vec4(correction[1], 0.0f);
    appearance.normalCorrection2 = glm::vec4(correction[2], 0.0f);
    appearance.flags |= 1u << 9u;
    if (instance.embedded) appearance.flags |= EMBEDDED;
    return {transform, appearance};
}
} // namespace

Instancing::Instancing(std::shared_ptr<Framework> framework) : framework_(std::move(framework)) {}

uint64_t Instancing::createEngine() {
    // Handles remain distinct across world recreation; a stale GAME handle cannot
    // accidentally address the first engine in a newly created native world.
    static std::atomic_uint64_t nextEngine{1};
    const uint64_t id = nextEngine++;
    auto child = std::make_shared<Instancing>(framework_);
    child->engineId_ = id;
    engines_.emplace(id, std::move(child));
    return id;
}

std::shared_ptr<Instancing> Instancing::engine(uint64_t id) {
    auto found = engines_.find(id);
    if (id == 0 || found == engines_.end()) throw std::runtime_error("Stale Flywheel engine handle");
    return found->second;
}

void Instancing::deleteEngine(uint64_t id) {
    auto found = engines_.find(id);
    if (found == engines_.end()) return;
    found->second->close();
    engines_.erase(found);
}

int Instancing::createModel(int meshCount) {
    if (meshCount < 0) throw std::runtime_error("Negative Flywheel mesh count");
    int id = nextModelId_++;
    auto model = std::make_shared<InstancingModel>();
    model->id = id; model->expectedMeshes = meshCount;
    model->vertices.resize(meshCount); model->indices.resize(meshCount);
    model->geometryTypes.resize(meshCount); model->groupNames.resize(meshCount);
    model->materialKeys.resize(meshCount);
    model->materialFlags.resize(meshCount);
    models_[id] = model;
    return id;
}

void Instancing::uploadModelMesh(int modelId, int meshIndex, const void *vertices, int vertexCount,
                                 const uint32_t *indices, int indexCount, int textureId,
                                 int alphaMode, int materialFlags, const char *materialKey) {
    auto model = models_.at(modelId);
    if (meshIndex < 0 || meshIndex >= model->expectedMeshes || vertices == nullptr ||
        indices == nullptr || vertexCount <= 0 || indexCount <= 0 || indexCount % 3 != 0)
        throw std::runtime_error("Invalid Flywheel mesh upload");
    const auto *source = static_cast<const CanonicalVertex *>(vertices);
    auto &out = model->vertices[meshIndex]; out.resize(vertexCount);
    for (int i = 0; i < vertexCount; ++i) {
        auto &v = out[i];
        v.pos = source[i].position;
        if ((materialFlags & MATERIAL_POLYGON_OFFSET) != 0)
            v.pos += source[i].normal * 1.0e-4f;
        v.useNorm = 1; v.norm = source[i].normal;
        v.useColorLayer = 1; v.colorLayer = source[i].color;
        v.useTexture = textureId != 0; v.textureID = textureId; v.textureUV = source[i].uv;
        v.useOverlay = (materialFlags & 1) != 0; v.overlayUV = {source[i].overlay & 0xffff, (source[i].overlay >> 16) & 0xffff};
        v.useLight = (materialFlags & 2) != 0; v.lightUV = {source[i].light & 0xffff, (source[i].light >> 16) & 0xffff};
        v.coordinate = World::WORLD; v.alphaMode = alphaMode;
    }
    model->indices[meshIndex].assign(indices, indices + indexCount);
    // Keep Flywheel geometry non-opaque so per-geometry culling and material transparency
    // are evaluated by the hit shader instead of being lost in the instance-wide TLAS flags.
    model->geometryTypes[meshIndex] = World::WORLD_TRANSPARENT;
    const uint32_t transparency = (static_cast<uint32_t>(materialFlags) >> MATERIAL_TRANSPARENCY_SHIFT) & 0x7u;
    // Every built-in pack maps the existing "lightning" group to the shared
    // transparent-only any/closest-hit pair; there is no group literally named
    // "transparent_only" in the SBT config.
    model->groupNames[meshIndex] = transparency == 0 ? "default" : "lightning";
    model->materialKeys[meshIndex] = materialKey == nullptr ? "" : materialKey;
    model->materialFlags[meshIndex] = static_cast<uint32_t>(materialFlags);
}

void Instancing::finishModel(int modelId) {
    auto model = models_.at(modelId);
    if (model->expectedMeshes == 0) return;
    for (int i = 0; i < model->expectedMeshes; ++i) {
        if (model->vertices[i].empty() || model->indices[i].empty()) throw std::runtime_error("Incomplete Flywheel model");
    }
    std::vector<uint32_t> overlays(model->expectedMeshes, 0);
    std::vector<std::string> shaderKeys(model->expectedMeshes, "flywheel");
    auto data = EntityBuildData::create(0, 0, 0, 0, 1, 0, -1, World::WORLD,
        static_cast<uint32_t>(model->expectedMeshes), std::move(model->geometryTypes),
        std::move(model->groupNames), std::vector<std::string>(model->materialKeys),
        std::move(model->vertices), std::move(model->indices), std::move(overlays),
        0, 0, 0, 0, 0, std::move(shaderKeys), std::move(model->materialKeys));
    data->geometryMaterialFlags = model->materialFlags;
    model->buildBatch = EntityBuildDataBatch::create();
    model->buildBatch->addData(data); model->buildBatch->build();
    Renderer::instance().buffers()->queueImportantWorldUpload(model->buildBatch->indexBuffer);
    Renderer::instance().buffers()->queueImportantWorldUpload(model->buildBatch->positionBuffer);
    Renderer::instance().buffers()->queueImportantWorldUpload(model->buildBatch->materialBuffer);
    pendingBlasBuilders_.push_back(model->buildBatch->blasBatchBuilder);
    auto batch = EntityBatch::create(model->buildBatch);
    model->geometry = batch->entities.front();
}

void Instancing::updateInstance(uint64_t id, int model, int adapter, int bias, bool visible,
                                const void *data, int dataSize, const float *embeddingPose,
                                const float *embeddingNormal, bool embedded) {
    if (!models_.contains(model) || data == nullptr || !mcvr::instancing::validInstanceBytes(adapter, dataSize) || embeddingPose == nullptr || embeddingNormal == nullptr)
        throw std::runtime_error("Invalid Flywheel instance update");
    auto &instance = instances_[id];
    instance.id = id; instance.model = model; instance.adapter = adapter; instance.bias = bias;
    instance.visible = visible; instance.embedded = embedded;
    instance.explicitLighting = false;
    instance.lightScene = 0;
    instance.skyScale = 1.0f;
    instance.data.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + dataSize);
    instance.embeddingPose = glm::make_mat4(embeddingPose);
    instance.embeddingNormal = glm::make_mat3(embeddingNormal);
}
void Instancing::deleteInstance(uint64_t id) { instances_.erase(id); crumbling_.erase(id); }
void Instancing::updateInstanceLighting(uint64_t id, int scene, float skyScale, const float *matrix) {
    if (matrix == nullptr || scene < -1 || !std::isfinite(skyScale) || skyScale < 0.0f || skyScale > 1.0f)
        throw std::runtime_error("Invalid Flywheel lighting scene metadata");
    for (int i = 0; i < 16; ++i) if (!std::isfinite(matrix[i]))
        throw std::runtime_error("Non-finite Flywheel lighting matrix");
    auto &instance = instances_.at(id);
    instance.explicitLighting = true;
    instance.lightScene = scene;
    instance.skyScale = skyScale;
    instance.lightSceneMatrix = glm::make_mat4(matrix);
}
void Instancing::beginFrame(glm::ivec3 origin, double ticks) { renderOrigin_ = origin; renderTicks_ = ticks; crumbling_.clear(); }
void Instancing::setShaderLights(const float *directions, int lightTextureId, bool constantAmbientLight) {
    if (directions == nullptr || lightTextureId <= 0)
        throw std::runtime_error("Missing Minecraft shader light state");
    shaderLight0_ = glm::make_vec3(directions);
    shaderLight1_ = glm::make_vec3(directions + 3);
    if (!std::isfinite(glm::length(shaderLight0_)) || !std::isfinite(glm::length(shaderLight1_)) ||
        glm::dot(shaderLight0_, shaderLight0_) == 0.0f || glm::dot(shaderLight1_, shaderLight1_) == 0.0f)
        throw std::runtime_error("Invalid Minecraft shader light directions");
    shaderLight0_ = glm::normalize(shaderLight0_);
    shaderLight1_ = glm::normalize(shaderLight1_);
    shaderLightsSet_ = true;
    lightTextureId_ = static_cast<uint32_t>(lightTextureId);
    constantAmbientLight_ = constantAmbientLight;
}
void Instancing::render() {
    if (!shaderLightsSet_) throw std::runtime_error("Minecraft shader lights were not uploaded before Flywheel render");
}
void Instancing::renderCrumbling(const uint64_t *ids, const int64_t *positions, const int32_t *progress,
                                 const int32_t *textures, int count) {
    for (int i = 0; i < count; ++i) if (instances_.contains(ids[i])) crumbling_[ids[i]] = {positions[i], progress[i], textures[i]};
}
std::vector<std::shared_ptr<vk::BLASBatchBuilder>> Instancing::drainPendingBlasBuilders() {
    auto builders = std::exchange(pendingBlasBuilders_, {});
    for (const auto &[id, child] : engines_) {
        auto pending = child->drainPendingBlasBuilders();
        builders.insert(builders.end(), pending.begin(), pending.end());
    }
    return builders;
}
std::vector<PreparedInstance> Instancing::preparedInstances() const {
    std::vector<PreparedInstance> out;
    for (const auto &[id, child] : engines_) {
        auto prepared = child->preparedInstances();
        out.insert(out.end(), prepared.begin(), prepared.end());
    }
    for (const auto &[id, instance] : instances_) {
        auto model = models_.at(instance.model);
        if (!instance.visible || model->geometry == nullptr) continue;
        auto [transform, appearance] = decode(instance, renderTicks_, renderOrigin_);
        appearance.entityLight0 = glm::vec4(shaderLight0_, instance.skyScale);
        appearance.entityLight1 = glm::vec4(shaderLight1_, 0.0f);
        appearance.materialState = lightTextureId_;
        if (constantAmbientLight_) appearance.flags |= CONSTANT_AMBIENT_LIGHT;
        if (auto it = crumbling_.find(id); it != crumbling_.end()) {
            appearance.flags |= CRUMBLING;
            appearance.textureOverride = it->second.texture;
        }
        out.push_back({model, id, instance.bias, transform, renderOrigin_, appearance,
                       (appearance.flags & CRUMBLING) != 0, engineId_});
    }
    std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.bias < b.bias; });
    return out;
}
void Instancing::close() {
    for (auto &[id, child] : engines_) child->close();
    engines_.clear();
    instances_.clear(); models_.clear(); pendingBlasBuilders_.clear();
    crumbling_.clear();
    shaderLightsSet_ = false;
    lightTextureId_ = 0;
    constantAmbientLight_ = false;
}
