#pragma once

#include "core/render/entities.hpp"

#include <cstdint>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Framework;

using InstancingAppearance = vk::VertexFormat::InstanceAppearance;

struct InstancingModel {
    int id = 0;
    int expectedMeshes = 0;
    std::vector<std::vector<vk::VertexFormat::PBRVertex>> vertices;
    std::vector<std::vector<uint32_t>> indices;
    std::vector<World::GeometryTypes> geometryTypes;
    std::vector<std::string> groupNames;
    std::vector<std::string> materialKeys;
    std::vector<uint32_t> materialFlags;
    std::shared_ptr<EntityBuildDataBatch> buildBatch;
    std::shared_ptr<Entity> geometry;
};

struct InstancingInstance {
    uint64_t id = 0;
    int model = 0;
    int adapter = 0;
    int bias = 0;
    bool visible = true;
    bool embedded = false;
    std::vector<uint8_t> data;
    glm::mat4 embeddingPose{1.0f};
    glm::mat3 embeddingNormal{1.0f};
    bool explicitLighting = false;
    int lightScene = 0;
    float skyScale = 1.0f;
    glm::mat4 lightSceneMatrix{1.0f};
};

struct PreparedInstance {
    std::shared_ptr<InstancingModel> model;
    uint64_t id;
    int bias;
    glm::mat4 transform;
    glm::ivec3 renderOrigin;
    InstancingAppearance appearance;
    bool crumbling = false;
    uint64_t engine = 0;
};

class Instancing : public SharedObject<Instancing> {
  public:
    explicit Instancing(std::shared_ptr<Framework> framework);
    uint64_t createEngine();
    std::shared_ptr<Instancing> engine(uint64_t id);
    void deleteEngine(uint64_t id);
    int createModel(int meshCount);
    void uploadModelMesh(int model, int meshIndex, const void *vertices, int vertexCount,
                         const uint32_t *indices, int indexCount, int textureId,
                         int alphaMode, int materialFlags, const char *materialKey);
    void finishModel(int model);
    void updateInstance(uint64_t id, int model, int adapter, int bias, bool visible,
                        const void *data, int dataSize, const float *embeddingPose,
                        const float *embeddingNormal, bool embedded);
    void deleteInstance(uint64_t id);
    void updateInstanceLighting(uint64_t id, int scene, float skyScale, const float *matrix);
    void beginFrame(glm::ivec3 renderOrigin, double renderTicks);
    void setShaderLights(const float *directions, int lightTextureId, bool constantAmbientLight);
    void render();
    void renderCrumbling(const uint64_t *instances, const int64_t *positions,
                         const int32_t *progress, const int32_t *textures, int count);
    std::vector<std::shared_ptr<vk::BLASBatchBuilder>> drainPendingBlasBuilders();
    std::vector<PreparedInstance> preparedInstances() const;
    void close();

  private:
    std::shared_ptr<Framework> framework_;
    uint64_t engineId_ = 0;
    std::map<uint64_t, std::shared_ptr<Instancing>> engines_;
    int nextModelId_ = 1;
    glm::ivec3 renderOrigin_{0};
    double renderTicks_ = 0.0;
    glm::vec3 shaderLight0_{0.0f};
    glm::vec3 shaderLight1_{0.0f};
    bool shaderLightsSet_ = false;
    uint32_t lightTextureId_ = 0;
    bool constantAmbientLight_ = false;
    std::unordered_map<int, std::shared_ptr<InstancingModel>> models_;
    std::unordered_map<uint64_t, InstancingInstance> instances_;
    std::vector<std::shared_ptr<vk::BLASBatchBuilder>> pendingBlasBuilders_;
    struct Crumbling { int64_t position; int progress; int texture; };
    std::unordered_map<uint64_t, Crumbling> crumbling_;
};
