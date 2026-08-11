#pragma once

#include "core/render/textures.hpp"

#include <array>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

struct ChunkBuildData;

struct LightInfo {
    glm::vec3 p0;
    glm::vec3 p1;
    glm::vec3 p2;
    glm::vec3 p3;

    glm::vec3 normal;
    glm::vec3 radiance;
    float area;

    uint32_t textureID;
    uint32_t sourceFaceFlags = 0;
    uint64_t stableID;
};

struct EmissionCellUpload {
    float u0;
    float v0;
    float u1;
    float v1;
    float avgEmission;
    float avgR;
    float avgG;
    float avgB;
};

struct EmissionCell {
    glm::vec2 uvMin;
    glm::vec2 uvMax;
    float avgEmission;
    glm::vec3 avgColor;
    uint64_t stableKey;
};

class EmissionCellRTree {
  public:
    using Value = std::shared_ptr<EmissionCell>;

    struct Rect {
        float minX;
        float minY;
        float maxX;
        float maxY;
    };

    void clear();
    void insert(const Rect &rect, const Value &value);
    bool remove(const Rect &rect, const Value &value);
    void search(const Rect &rect, std::vector<std::shared_ptr<const EmissionCell>> &out) const;

  private:
    static constexpr size_t kMaxEntries = 16;
    static constexpr size_t kMinEntries = 8;

    struct Node;

    struct Entry {
        Rect rect{};
        Value value{};
        std::unique_ptr<Node> child{};
    };

    struct Node {
        bool leaf = true;
        std::vector<Entry> entries;
    };

    using EntryList = std::vector<Entry>;

    static Rect combineRect(const Rect &a, const Rect &b);
    static bool intersects(const Rect &a, const Rect &b);
    static float area(const Rect &rect);
    static Rect computeNodeRect(const Node &node);
    static Rect computeEntriesRect(const EntryList &entries);
    static float enlargement(const Rect &original, const Rect &extra);

    static Entry makeLeafEntry(const Rect &rect, const Value &value);
    static Entry makeChildEntry(std::unique_ptr<Node> child);
    static void collectLeafEntries(Node &node, EntryList &out);
    static std::unique_ptr<Node> splitNode(Node &node);
    static size_t chooseSubtree(const Node &node, const Rect &rect);
    static bool removeRecursive(Node &node, const Rect &rect, const Value &value, EntryList &reinserts);
    static void searchRecursive(const Node &node,
                                const Rect &rect,
                                std::vector<std::shared_ptr<const EmissionCell>> &out);

    void insertEntry(Entry entry);
    void insertRecursive(Node &node, Entry entry, std::unique_ptr<Node> &splitNodeOut);

  private:
    std::unique_ptr<Node> root_;
};

class Emission : public SharedObject<Emission> {
  public:
    static constexpr int kMaxTextures = 4096;

    explicit Emission(std::weak_ptr<Textures> textures);

    void reset();
    void resetTexture(uint32_t textureID);
    void updateTile(uint32_t textureID, uint64_t tileKey, const EmissionCellUpload *cells, int cellCount);

    void collectCells(uint32_t textureID,
                      const glm::vec2 &uvMin,
                      const glm::vec2 &uvMax,
                      std::vector<std::shared_ptr<const EmissionCell>> &out) const;

  private:
    struct TextureState {
        std::unordered_map<uint64_t, std::vector<std::shared_ptr<EmissionCell>>> tiles;
        std::unique_ptr<EmissionCellRTree> tree;
        uint32_t version = 0;
    };

    static uint64_t hashCombine64(uint64_t seed, uint64_t value);
    static EmissionCellRTree::Rect buildRect(const EmissionCell &cell);
    static EmissionCellRTree::Rect buildRect(const glm::vec2 &uvMin, const glm::vec2 &uvMax);
    void clearTextureState(TextureState &state);

  private:
    std::weak_ptr<Textures> textures_;
    mutable std::shared_mutex mtx_;
    std::array<TextureState, kMaxTextures> texturesState_{};
};
