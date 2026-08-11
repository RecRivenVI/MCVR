#pragma once
#include "core/render/geometry_topology.hpp"
#include <atomic>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

namespace mcvr::audit {
inline std::atomic<uint64_t> submissionFrame{0};
inline void beginFrame() { if(mcvr::geometry::isolatedAudit()) ++submissionFrame; }
inline void geometry(const std::string &source, int64_t owner, size_t part,
                     const std::vector<vk::VertexFormat::PBRVertex> &vertices,
                     const std::vector<uint32_t> &indices,
                     const std::vector<uint32_t> &flags, uint32_t sourceFaces,
                     const topology::Report &report, const char *executionPath="cpu-final",
                     double originX=0, double originY=0, double originZ=0, uint64_t frameToken=0) {
    if (!mcvr::geometry::isolatedAudit()) return;
    static std::mutex mutex;
    static uint32_t count=0;
    static uint32_t chunkCount=0;
    static std::unordered_set<uint64_t> seen;
    static std::unordered_map<std::string,uint32_t> sourceCounts;
    std::lock_guard lock(mutex);
    // A crowded model city must not consume the entire audit before factory,
    // water, particles, text and external sections have a chance to report.
    if(count>=512 || sourceCounts[source]>=32 || (source.starts_with("chunk/") && chunkCount>=64)) return;
    uint64_t key=std::hash<std::string>{}(source)^static_cast<uint64_t>(owner)^part;
    for(auto index:indices) {key^=index;key*=1099511628211ull;}
    for(auto flag:flags) {key^=flag;key*=1099511628211ull;}
    if(!seen.insert(key).second) return;
    ++sourceCounts[source];
    if(source.starts_with("chunk/")) ++chunkCount;
    const uint32_t ordinal=count++;
    auto folder=std::filesystem::current_path()/"geometry-audit";
    std::filesystem::create_directories(folder);
    std::ofstream out(folder/(std::to_string(ordinal)+".json"));
    if(!out) throw std::runtime_error("Isolated geometry audit output");
    out<<std::setprecision(9)<<"{\"source\":"<<std::quoted(source)<<",\"owner\":"<<owner
       <<",\"part\":"<<part<<",\"sourceFaces\":"<<sourceFaces
       <<",\"executionPath\":"<<std::quoted(executionPath)
       <<",\"origin\":["<<originX<<','<<originY<<','<<originZ<<"],\"frameToken\":"<<frameToken
       <<",\"submissionFrame\":"<<submissionFrame.load()
       <<",\"quads\":"<<report.quads<<",\"pairedQuads\":"<<report.pairedQuads
       <<",\"pairedTriangles\":"<<report.pairedTriangles<<",\"nonfinite\":"<<report.nonfinite
       <<",\"correctedQuads\":"<<report.correctedQuads<<",\"sameWinding\":"<<report.sameWinding
       <<",\"ambiguous\":"<<report.ambiguous<<",\"degenerate\":"<<report.degenerate
       <<",\"vertices\":[";
    for(size_t i=0;i<vertices.size();++i) {
        if(i) out<<',';
        const auto &v=vertices[i];
        out<<"{\"p\":["<<v.pos.x<<','<<v.pos.y<<','<<v.pos.z<<"],\"n\":["
           <<v.norm.x<<','<<v.norm.y<<','<<v.norm.z<<"],\"uv\":["<<v.textureUV.x<<','<<v.textureUV.y
           <<"],\"policy\":"<<(flags.empty()?0:flags.at(i))<<"}";
    }
    out<<"],\"indices\":[";
    for(size_t i=0;i<indices.size();++i) {if(i)out<<',';out<<indices[i];}
    out<<"]}";
    if(!out) throw std::runtime_error("Isolated geometry audit write failed");
}
} // namespace mcvr::audit
