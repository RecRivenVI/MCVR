#include "core/render/quad_topology.hpp"
#include <iostream>
#include <set>

using namespace mcvr::topology;
void require(bool value) { if (!value) throw std::runtime_error("Quad topology regression"); }
using Triangle = std::array<Corner,3>;
std::multiset<Triangle> triangles(const std::vector<Point> &points, const std::vector<uint32_t> &indices, size_t begin) {
    std::multiset<Triangle> out;
    for (size_t t=begin;t<begin+6;t+=3) {
        Triangle triangle{bits(points[indices[t]]),bits(points[indices[t+1]]),bits(points[indices[t+2]])};
        std::sort(triangle.begin(),triangle.end());out.insert(triangle);
    }
    return out;
}
int main() {
    std::array<Point,4> corners{{{0,0,0},{1,0,.1f},{1,1,0},{0,1,0}}};
    unsigned cases=0;
    for (bool planar : {false,true}) for (bool reversed : {false,true}) for (size_t rotation=0;rotation<4;++rotation) {
        std::vector<Point> points(corners.begin(),corners.end());
        if (planar) points[1][2]=0;
        for (size_t i=0;i<4;++i) points.push_back(points[(rotation+(reversed ? 4-i:i))%4]);
        const auto original=sequentialQuads(points.size());
        auto result=reconcile(points,original,true,true);
        require(hasReversePolygonPair<4>(points.size(),[&](size_t i){return points[i];})==reversed);
        require(std::equal(original.begin(),original.begin()+6,result.indices.begin()));
        if (!reversed) { require(result.report.sameWinding==1 && result.report.pairedQuads==0); }
        else {
            require(result.report.pairedQuads==2);
            if (!planar) require(triangles(points,result.indices,0)==triangles(points,result.indices,6));
            else require(result.indices==original && result.report.correctedQuads==0);
            require(reconcile(points,original,true,true).indices==result.indices);
        }
        ++cases;
    }
    {
        std::vector<Point> points(corners.begin(),corners.end());
        for (int i=3;i>=0;--i) points.push_back(corners[i]);
        points[4][0]=std::nextafter(points[4][0],1.0f);
        auto result=reconcile(points,sequentialQuads(points.size()),true,true);
        require(result.report.pairedQuads==0 && result.report.correctedQuads==0);
    }
    {
        std::vector<Point> points(corners.begin(),corners.end());
        for (int i=3;i>=0;--i) points.push_back(corners[i]);
        auto original=sequentialQuads(points.size());
        auto source=reconcile(points,original,true,false);
        auto repaired=reconcile(points,original,true,true);
        require(source.indices==original && source.report.correctedQuads==0);
        require(repaired.report.correctedQuads==1);
        require(!compatibleHistory(source.primitiveFlags[2],repaired.primitiveFlags[2]));
        require(compatibleHistory(repaired.primitiveFlags[2],repaired.primitiveFlags[2]));
        require(reconcile(points,original,false,true).indices==original);
        auto tetrahedron=reconcileAuthored(points,original,false,true);
        require(tetrahedron.indices==original && tetrahedron.report.pairedQuads==0);
        require(reconcileAuthored(points,original,true,true).report.correctedQuads==1);
        points.insert(points.end(),corners.begin(),corners.end());
        require(reconcile(points,sequentialQuads(points.size()),true,true).report.ambiguous==1);
    }
    {
        std::vector<Point> points{{0,0,0},{1,0,0},{1,0,0},{0,1,0}};
        require(reconcile(points,sequentialQuads(4),true,true).report.degenerate==1);
        points[2][0]=std::numeric_limits<float>::quiet_NaN();
        require(reconcile(points,sequentialQuads(4),true,true).report.nonfinite==1);
        bool rejectedNaN=false;
        try {hasReversePolygonPair<4>(points.size(),[&](size_t i){return points[i];});}
        catch(const std::invalid_argument &) {rejectedNaN=true;}
        require(rejectedNaN);
    }
    {
        std::vector<Point> points{{0,0,0},{1,0,0},{0,1,0},{0,1,0},{1,0,0},{0,0,0}};
        std::vector<uint32_t> indices{0,1,2,3,4,5};
        auto result=reconcile(points,indices,false,true);
        require(result.report.pairedTriangles==2 && result.indices==indices);
        std::vector<uint32_t> shared{0,1,2,0,2,1};
        const std::array<uint32_t,2> different{0,3};
        auto flags=attachPrimitiveFlags(points,shared,different);
        require(shared[0]!=shared[3] && points[shared[0]]==points[shared[3]]);
        require(flags[shared[0]]==0 && flags[shared[3]]==3);
    }
    {
        std::vector<Point> large;
        for(int q=0;q<80;++q) for(auto p:corners) {p[0]+=q*3;large.push_back(p);}
        for(int j=3;j>=0;--j) large.push_back(large[j]);
        require(hasReversePolygonPair<4>(large.size(),[&](size_t i){return large[i];}));
    }
    bool rejected=false;
    try { sequentialQuads(5); } catch(const std::invalid_argument &) { rejected=true; }
    require(rejected);
    std::cout<<cases<<" cyclic planar/warped/orientation cases; exact bits, ownership ambiguity and history checks passed\n";
}
