#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace mcvr::topology {
using Point = std::array<float, 3>;
using Corner = std::array<uint32_t, 3>;
using Key = std::array<Corner, 4>;
inline constexpr uint32_t paired = 1u;
inline constexpr uint32_t alternateDiagonal = 2u;

struct Report {
    uint32_t quads = 0, pairedQuads = 0, pairedTriangles = 0, correctedQuads = 0;
    uint32_t sameWinding = 0, ambiguous = 0, degenerate = 0, nonfinite = 0;
};
struct Result {
    std::vector<uint32_t> indices;
    // Each entry belongs to one output triangle, independently of vertex sharing.
    std::vector<uint32_t> primitiveFlags;
    Report report;
};
inline Corner bits(Point p) {
    return {std::bit_cast<uint32_t>(p[0]), std::bit_cast<uint32_t>(p[1]), std::bit_cast<uint32_t>(p[2])};
}
struct KeyHash {
    template<size_t N> size_t operator()(const std::array<Corner,N> &key) const noexcept {
        uint64_t hash = 1469598103934665603ull;
        for (const auto &corner : key) for (auto value : corner) {
            hash ^= value;
            hash *= 1099511628211ull;
        }
        return static_cast<size_t>(hash);
    }
};
// Admission probe for deferred GPU conversion. Common small ModelParts require
// no heap allocation, no generated index list, and no material reconstruction.
// This detects candidates only; reconcile still validates/repairs final CPU data.
template<size_t N, typename Fetch>
bool hasReversePolygonPair(size_t count, Fetch fetch) {
    if(count%N) throw std::invalid_argument("Incomplete deferred surface primitive");
    using Polygon=std::array<Corner,N>;
    const auto reverse=[](const Polygon &a,const Polygon &b) {
        for(size_t shift=0;shift<N;++shift) {
            bool match=true;
            for(size_t i=0;i<N;++i) match &= b[i]==a[(shift+N-i)%N];
            if(match) return true;
        }
        return false;
    };
    std::array<Polygon,64> small{};
    std::unordered_map<Polygon,Polygon,KeyHash> large;
    const bool smallCase=count/N<=small.size();
    if(!smallCase) large.reserve(count/N);
    for(size_t at=0;at<count;at+=N) {
        Polygon ordered;
        for(size_t i=0;i<N;++i) {
            auto point=fetch(at+i);
            for(auto value:point) if(!std::isfinite(value)) throw std::invalid_argument("Nonfinite deferred surface position");
            ordered[i]=bits(point);
        }
        auto key=ordered;std::sort(key.begin(),key.end());
        if(std::adjacent_find(key.begin(),key.end())!=key.end()) continue;
        if(smallCase) {
            for(size_t j=0;j<at/N;++j) if(reverse(small[j],ordered)) return true;
            small[at/N]=ordered;
        } else {
            const auto [it,inserted]=large.try_emplace(key,ordered);
            if(!inserted && reverse(it->second,ordered)) return true;
        }
    }
    return false;
}
inline std::array<double, 3> subtract(Point a, Point b) {
    return {double(a[0])-b[0], double(a[1])-b[1], double(a[2])-b[2]};
}
inline std::array<double, 3> cross(std::array<double, 3> a, std::array<double, 3> b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
inline double dot(std::array<double, 3> a, std::array<double, 3> b) {
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}
struct Quad {
    size_t firstIndex;
    std::array<uint32_t, 4> vertices;
    Key ordered;
    bool warped;
};

// The caller supplies FINAL positions (including positional offsets). Only
// explicit canonical six-index QUADS are reconstructed. Arbitrary triangle,
// strip/fan and line contracts are never guessed from a coincidental count.
inline Result reconcile(std::span<const Point> positions, std::span<const uint32_t> indices,
                        bool quadContract, bool repair, double planarityEpsilon = 1e-6) {
    if (indices.size() % 3 != 0) throw std::invalid_argument("Surface triangle count");
    if (!(planarityEpsilon >= 0) || !std::isfinite(planarityEpsilon))
        throw std::invalid_argument("Quad planarity epsilon");
    Result out{{indices.begin(), indices.end()}, std::vector<uint32_t>(indices.size()/3), {}};
    for (auto i : indices) if (i >= positions.size()) throw std::out_of_range("Surface vertex index");
    if (!quadContract) {
        using TriKey=std::array<Corner,3>;
        std::map<TriKey,std::vector<size_t>> triangles;
        for(size_t i=0;i<indices.size();i+=3) {
            TriKey key{bits(positions[indices[i]]),bits(positions[indices[i+1]]),bits(positions[indices[i+2]])};
            bool finite=true;
            for(size_t j=0;j<3;++j) for(auto v:positions[indices[i+j]]) finite&=std::isfinite(v);
            if(!finite) {++out.report.nonfinite;continue;}
            auto n=cross(subtract(positions[indices[i+1]],positions[indices[i]]),
                         subtract(positions[indices[i+2]],positions[indices[i]]));
            if(dot(n,n)<=1e-24) {++out.report.degenerate;continue;}
            std::sort(key.begin(),key.end());triangles[key].push_back(i);
        }
        for(const auto &[key,members]:triangles) {
            if(members.size()<2) continue;
            if(members.size()!=2) {++out.report.ambiguous;continue;}
            std::array<int,3> order{};
            for(int i=0;i<3;++i) {
                const auto point=bits(positions[indices[members[1]+i]]);
                for(int j=0;j<3;++j) if(point==bits(positions[indices[members[0]+j]])) order[i]=j;
            }
            bool reverse=true;
            for(int i=0;i<3;++i) reverse &= order[(i+1)%3]==(order[i]+2)%3;
            if(reverse) {
                out.primitiveFlags[members[0]/3]|=paired;
                out.primitiveFlags[members[1]/3]|=paired;
                out.report.pairedTriangles+=2;
            } else ++out.report.sameWinding;
        }
        return out;
    }
    if (indices.size() % 6 != 0) throw std::invalid_argument("Quad index count");
    std::vector<Quad> quads;
    std::unordered_map<Key, std::vector<size_t>, KeyHash> groups;
    quads.reserve(indices.size()/6);
    for (size_t at = 0; at < indices.size(); at += 6) {
        if (indices[at+3] != indices[at+2] || indices[at+5] != indices[at])
            throw std::invalid_argument("Unrecognized explicit QUADS expansion");
        std::array<uint32_t,4> vertices{indices[at], indices[at+1], indices[at+2], indices[at+4]};
        ++out.report.quads;
        Key ordered;
        bool finite = true;
        for (size_t i=0;i<4;++i) {
            ordered[i] = bits(positions[vertices[i]]);
            for (auto value : positions[vertices[i]]) finite &= std::isfinite(value);
        }
        if (!finite) { ++out.report.nonfinite; continue; }
        Key key = ordered;
        std::sort(key.begin(), key.end());
        auto p0=positions[vertices[0]], p1=positions[vertices[1]], p2=positions[vertices[2]], p3=positions[vertices[3]];
        auto n0=cross(subtract(p1,p0),subtract(p2,p0));
        auto n1=cross(subtract(p3,p2),subtract(p0,p2));
        if (std::adjacent_find(key.begin(),key.end())!=key.end()
            || dot(n0,n0)<=1e-24 || dot(n1,n1)<=1e-24) {
            ++out.report.degenerate; continue;
        }
        const double deviation=std::abs(dot(n0,subtract(p3,p0)))/std::sqrt(dot(n0,n0));
        auto id=quads.size();
        quads.push_back({at,vertices,ordered,deviation>planarityEpsilon});
        groups[key].push_back(id);
    }
    for (const auto &[key,members] : groups) {
        if (members.size()<2) continue;
        if (members.size()!=2) { ++out.report.ambiguous; continue; }
        const auto &first=quads[members[0]], &second=quads[members[1]];
        std::array<int,4> map{};
        for (int i=0;i<4;++i)
            map[i]=int(std::find(first.ordered.begin(),first.ordered.end(),second.ordered[i])-first.ordered.begin());
        bool reverse=true, forward=true;
        for (int i=0;i<4;++i) {
            reverse &= map[(i+1)%4]==(map[i]+3)%4;
            forward &= map[(i+1)%4]==(map[i]+1)%4;
        }
        if (forward) { ++out.report.sameWinding; continue; }
        if (!reverse) { ++out.report.ambiguous; continue; }
        out.report.pairedQuads+=2;
        for (auto q : {first.firstIndex,second.firstIndex}) {
            out.primitiveFlags[q/3]|=paired;
            out.primitiveFlags[q/3+1]|=paired;
        }
        const bool sameDiagonal=(map[0]==0 && map[2]==2)||(map[0]==2 && map[2]==0);
        if (repair && (first.warped || second.warped) && !sameDiagonal) {
            const auto &v=second.vertices;
            const std::array<uint32_t,6> replacement{v[1],v[2],v[3],v[3],v[0],v[1]};
            std::copy(replacement.begin(),replacement.end(),out.indices.begin()+second.firstIndex);
            out.primitiveFlags[second.firstIndex/3]|=alternateDiagonal;
            out.primitiveFlags[second.firstIndex/3+1]|=alternateDiagonal;
            ++out.report.correctedQuads;
        }
    }
    return out;
}
inline std::vector<uint32_t> sequentialQuads(size_t count) {
    if (count%4 || count>std::numeric_limits<uint32_t>::max()) throw std::invalid_argument("Quad vertex count");
    std::vector<uint32_t> out;
    out.reserve(count/4*6);
    for (uint32_t i=0;i<count;i+=4) out.insert(out.end(),{i,i+1,i+2,i+2,i+3,i});
    return out;
}
inline bool compatibleHistory(uint32_t current, uint32_t previous) {
    return (current & alternateDiagonal)==(previous & alternateDiagonal);
}
inline bool canonicalQuadExpansion(std::span<const uint32_t> indices) {
    if (indices.empty() || indices.size()%6) return false;
    for (size_t i=0;i<indices.size();i+=6)
        if(indices[i+3]!=indices[i+2] || indices[i+5]!=indices[i]) return false;
    return true;
}
inline Result reconcileAuthored(std::span<const Point> positions, std::span<const uint32_t> indices,
                                bool authoredQuads, bool repair) {
    // A legal tetrahedron can have exactly the same four triangles as the
    // warped-twin fixture. Index shape alone is not source QUADS provenance.
    return reconcile(positions,indices,authoredQuads && canonicalQuadExpansion(indices),repair);
}
// Only the leading vertex's padding carries a triangle policy. Clone it when
// indexed triangles share that vertex but require different policies; attributes
// and the source position are copied exactly, never reconstructed or blended.
template <typename Vertex>
std::vector<uint32_t> attachPrimitiveFlags(std::vector<Vertex> &vertices,
                                         std::vector<uint32_t> &indices,
                                         std::span<const uint32_t> flags) {
    if (flags.size()!=indices.size()/3) throw std::invalid_argument("Primitive policy count");
    std::vector<uint32_t> result(vertices.size(),0);
    std::vector<bool> assigned(vertices.size(),false);
    for(size_t t=0;t<flags.size();++t) {
        auto &index=indices[t*3];
        if(!assigned.at(index)) {assigned[index]=true;result[index]=flags[t];}
        else if(result[index]!=flags[t]) {
            if(vertices.size()>=UINT32_MAX) throw std::length_error("Policy vertex clone count");
            Vertex copy=vertices[index];
            index=static_cast<uint32_t>(vertices.size());vertices.push_back(copy);
            result.push_back(flags[t]);assigned.push_back(true);
        }
    }
    return result;
}
} // namespace mcvr::topology
