#include "RecoveredBspGeometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace RecoveredBspGeometry
{
    namespace
    {
        Vec3 Add(const Vec3& a, const Vec3& b)
        {
            return { a.x + b.x, a.y + b.y, a.z + b.z };
        }

        Vec3 Subtract(const Vec3& a, const Vec3& b)
        {
            return { a.x - b.x, a.y - b.y, a.z - b.z };
        }

        Vec3 Scale(const Vec3& p, double scale)
        {
            return { p.x * scale, p.y * scale, p.z * scale };
        }

        double Dot(const Vec3& a, const Vec3& b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        Vec3 Cross(const Vec3& a, const Vec3& b)
        {
            return { a.y * b.z - a.z * b.y,
                     a.z * b.x - a.x * b.z,
                     a.x * b.y - a.y * b.x };
        }

        bool Finite(const Vec3& p)
        {
            return std::isfinite(p.x) && std::isfinite(p.y)
                && std::isfinite(p.z);
        }

        bool Near(const Vec3& a, const Vec3& b, double epsilon)
        {
            const Vec3 delta = Subtract(a, b);
            return Dot(delta, delta) <= epsilon * epsilon;
        }

        struct CellFace
        {
            Face face;
            bool artificialBoundary = false;
        };

        using Cell = std::vector<CellFace>;

        struct Work
        {
            const Limits& limits;
            std::size_t count = 0;
            std::string& error;
            bool exhausted = false;
            // Set by Clip when the fault is this cell's shape (no closed cap,
            // too many faces), not the input or the work budget.
            bool degenerate = false;

            bool Spend(std::size_t amount)
            {
                if (amount > limits.maxClippingWork
                    || count > limits.maxClippingWork - amount)
                {
                    exhausted = true;
                    error = "BSP reconstruction exceeded its clipping-work limit. "
                            "This map needs a less fragmented source reconstruction.";
                    return false;
                }
                count += amount;
                return true;
            }
        };

        Cell MakeBox(const Bounds& bounds)
        {
            const double x = bounds.minimum.x, X = bounds.maximum.x;
            const double y = bounds.minimum.y, Y = bounds.maximum.y;
            const double z = bounds.minimum.z, Z = bounds.maximum.z;
            Cell cell;
            cell.push_back({ { { {x,y,z}, {x,y,Z}, {x,Y,Z}, {x,Y,z} }, {-1,0,0}, -1 }, true });
            cell.push_back({ { { {X,y,z}, {X,Y,z}, {X,Y,Z}, {X,y,Z} }, {1,0,0}, -1 }, true });
            cell.push_back({ { { {x,y,z}, {X,y,z}, {X,y,Z}, {x,y,Z} }, {0,-1,0}, -1 }, true });
            cell.push_back({ { { {x,Y,z}, {x,Y,Z}, {X,Y,Z}, {X,Y,z} }, {0,1,0}, -1 }, true });
            cell.push_back({ { { {x,y,z}, {x,Y,z}, {X,Y,z}, {X,y,z} }, {0,0,-1}, -1 }, true });
            cell.push_back({ { { {x,y,Z}, {X,y,Z}, {X,Y,Z}, {x,Y,Z} }, {0,0,1}, -1 }, true });
            return cell;
        }

        void AppendUnique(std::vector<Vec3>& points, const Vec3& point,
                          double epsilon)
        {
            if (points.empty() || !Near(points.back(), point, epsilon))
                points.push_back(point);
        }

        void CleanPolygon(std::vector<Vec3>& vertices, double epsilon)
        {
            if (vertices.size() > 1 && Near(vertices.front(), vertices.back(), epsilon))
                vertices.pop_back();
            // Keep collinear vertices: removing them independently on adjacent
            // faces introduces T-junctions. The caller may triangulate long
            // FPolys while retaining all edge points.
        }

        bool HasArea(const std::vector<Vec3>& points, const Vec3& normal,
                     double epsilon)
        {
            if (points.size() < 3)
                return false;
            Vec3 area{};
            for (std::size_t index = 1; index + 1 < points.size(); ++index)
                area = Add(area, Cross(Subtract(points[index], points[0]),
                                       Subtract(points[index + 1], points[0])));
            const double orientedArea = Dot(area, normal);
            return Finite(area) && std::isfinite(orientedArea)
                && orientedArea > epsilon * epsilon;
        }

        // Keep the negative half-space n.p <= d. Existing boundary polygons
        // retain their identity; the new cap receives the splitter's surface.
        bool Clip(const Cell& input, const Vec3& normal, double distance,
                  int surfaceIndex, Cell& output, Work& work)
        {
            const double epsilon = work.limits.epsilon;
            double minimum = std::numeric_limits<double>::infinity();
            double maximum = -minimum;
            for (const CellFace& face : input)
            {
                if (!work.Spend(face.face.vertices.size()))
                    return false;
                for (const Vec3& point : face.face.vertices)
                {
                    const double side = Dot(normal, point) - distance;
                    if (!std::isfinite(side))
                    {
                        work.error = "BSP clipping produced a non-finite plane distance.";
                        return false;
                    }
                    minimum = (std::min)(minimum, side);
                    maximum = (std::max)(maximum, side);
                }
            }
            output.clear();
            if (minimum >= -epsilon)
                return true; // Empty, or a cell collapsed to a plane.
            if (maximum <= epsilon)
            {
                output = input;
                return true;
            }

            std::vector<Vec3> cap;
            output.reserve(input.size() + 1);
            for (const CellFace& face : input)
            {
                if (!work.Spend(face.face.vertices.size()))
                    return false;
                CellFace clipped;
                clipped.face.normal = face.face.normal;
                clipped.face.surfaceIndex = face.face.surfaceIndex;
                clipped.artificialBoundary = face.artificialBoundary;
                const auto& vertices = face.face.vertices;
                for (std::size_t index = 0; index < vertices.size(); ++index)
                {
                    const Vec3& a = vertices[index];
                    const Vec3& b = vertices[(index + 1) % vertices.size()];
                    const double da = Dot(normal, a) - distance;
                    const double db = Dot(normal, b) - distance;
                    if (da <= 0)
                        AppendUnique(clipped.face.vertices, a, epsilon);
                    if ((da < 0 && db > 0) || (da > 0 && db < 0))
                    {
                        const double t = da / (da - db);
                        const Vec3 intersection = Add(a, Scale(Subtract(b, a), t));
                        if (!Finite(intersection))
                        {
                            work.error = "BSP clipping produced a non-finite vertex.";
                            return false;
                        }
                        AppendUnique(clipped.face.vertices, intersection, epsilon);
                        cap.push_back(intersection);
                    }
                    else if (da == 0)
                        cap.push_back(a);
                }
                CleanPolygon(clipped.face.vertices, epsilon);
                if (HasArea(clipped.face.vertices, clipped.face.normal, epsilon))
                    output.push_back(std::move(clipped));
            }

            std::vector<Vec3> uniqueCap;
            for (const Vec3& point : cap)
            {
                if (!work.Spend(uniqueCap.size()))
                    return false;
                if (std::none_of(uniqueCap.begin(), uniqueCap.end(),
                    [&](const Vec3& other) { return Near(point, other, epsilon); }))
                    uniqueCap.push_back(point);
            }
            if (uniqueCap.size() < 3)
            {
                work.degenerate = true;
                work.error = "A BSP split could not produce a closed cap. "
                            "The cooked geometry is degenerate at the reconstruction tolerance.";
                return false;
            }

            Vec3 center = uniqueCap.front();
            for (std::size_t index = 1; index < uniqueCap.size(); ++index)
                center = Add(center, Scale(Subtract(uniqueCap[index], center),
                                          1.0 / static_cast<double>(index + 1)));
            const Vec3 reference = std::fabs(normal.x) < 0.8
                ? Vec3{1,0,0} : Vec3{0,1,0};
            Vec3 u = Cross(reference, normal);
            u = Scale(u, 1.0 / std::sqrt(Dot(u, u)));
            const Vec3 v = Cross(normal, u);
            std::sort(uniqueCap.begin(), uniqueCap.end(),
                [&](const Vec3& a, const Vec3& b)
                {
                    const Vec3 da = Subtract(a, center), db = Subtract(b, center);
                    return std::atan2(Dot(v, da), Dot(u, da))
                         < std::atan2(Dot(v, db), Dot(u, db));
                });
            if (!HasArea(uniqueCap, normal, epsilon))
            {
                work.degenerate = true;
                work.error = "A BSP split produced a zero-area or reversed cap.";
                return false;
            }
            output.push_back({ { std::move(uniqueCap), normal, surfaceIndex }, false });
            if (output.size() > work.limits.maxFacesPerBrush)
            {
                work.degenerate = true;
                work.error = "A reconstructed BSP cell exceeded the brush-face limit.";
                return false;
            }
            return true;
        }

        bool ValidateNodes(const std::vector<Node>& input, std::vector<Node>& nodes,
                           const Limits& limits, std::string& error)
        {
            if (input.size() > limits.maxNodes
                || input.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
            {
                error = "The cooked BSP exceeds the reconstruction node limit.";
                return false;
            }
            nodes = input;
            for (Node& node : nodes)
            {
                const double length = std::hypot(node.normal.x, node.normal.y, node.normal.z);
                if (!Finite(node.normal) || !std::isfinite(node.distance)
                    || !std::isfinite(length) || length < 1e-12)
                {
                    error = "The cooked BSP contains an invalid splitting plane.";
                    return false;
                }
                node.normal = Scale(node.normal, 1.0 / length);
                node.distance /= length;
                if (!std::isfinite(node.distance))
                {
                    error = "A cooked BSP plane could not be normalized safely.";
                    return false;
                }
                if (node.front < -1 || node.back < -1
                    || node.front >= static_cast<int>(nodes.size())
                    || node.back >= static_cast<int>(nodes.size()))
                {
                    error = "The cooked BSP contains an invalid child index.";
                    return false;
                }
            }
            if (nodes.empty())
                return true;

            // A visited node in a front/back tree can never be encountered a
            // second time. This catches both cycles and shared-child graphs,
            // including paths whose geometry will later clip to nothing.
            std::vector<unsigned char> visited(nodes.size(), 0);
            std::vector<int> pending{0};
            while (!pending.empty())
            {
                const int index = pending.back();
                pending.pop_back();
                if (visited[static_cast<std::size_t>(index)] != 0)
                {
                    error = "The cooked BSP front/back tree contains a cycle or repeated node.";
                    return false;
                }
                visited[static_cast<std::size_t>(index)] = 1;
                const Node& node = nodes[static_cast<std::size_t>(index)];
                if (node.front >= 0)
                    pending.push_back(node.front);
                if (node.back >= 0)
                    pending.push_back(node.back);
            }
            return true;
        }

        bool ValidateClosedCell(const Cell& cell, Work& work)
        {
            const double epsilon = work.limits.epsilon;
            std::vector<Vec3> points;
            std::vector<std::pair<std::size_t, std::size_t>> edges;
            double volumeSix = 0;
            const Vec3 reference = cell.front().face.vertices.front();
            for (const CellFace& cellFace : cell)
            {
                const Face& face = cellFace.face;
                if (face.vertices.size() < 3 || !HasArea(face.vertices, face.normal, epsilon))
                {
                    work.error = "A reconstructed brush contains a degenerate face.";
                    return false;
                }
                std::vector<std::size_t> indices;
                for (const Vec3& point : face.vertices)
                {
                    if (!work.Spend(points.size() + 1))
                        return false;
                    if (!Finite(point) || std::fabs(Dot(face.normal,
                            Subtract(point, face.vertices.front()))) > epsilon * 8)
                    {
                        work.error = "A reconstructed brush contains a non-planar face.";
                        return false;
                    }
                    std::size_t index = 0;
                    while (index < points.size() && !Near(points[index], point, epsilon))
                        ++index;
                    if (index == points.size())
                        points.push_back(point);
                    indices.push_back(index);
                }
                for (std::size_t index = 0; index < indices.size(); ++index)
                    edges.emplace_back(indices[index], indices[(index + 1) % indices.size()]);
                for (std::size_t index = 1; index + 1 < face.vertices.size(); ++index)
                    volumeSix += Dot(Subtract(face.vertices[0], reference),
                        Cross(Subtract(face.vertices[index], reference),
                              Subtract(face.vertices[index + 1], reference)));
            }
            if (!std::isfinite(volumeSix) || volumeSix <= epsilon * epsilon * epsilon)
            {
                work.error = "A reconstructed brush has zero or reversed volume.";
                return false;
            }
            std::sort(edges.begin(), edges.end());
            for (std::size_t index = 0; index < edges.size(); ++index)
            {
                const auto edge = edges[index];
                if (edge.first == edge.second
                    || (index != 0 && edge == edges[index - 1])
                    || !std::binary_search(edges.begin(), edges.end(),
                                          std::make_pair(edge.second, edge.first)))
                {
                    work.error = "A reconstructed brush is not a closed two-sided edge manifold. "
                                 "The cooked BSP is degenerate at the reconstruction tolerance.";
                    return false;
                }
            }
            return true;
        }

        Bounds FaceBounds(const Face& face)
        {
            Bounds result{face.vertices.front(),face.vertices.front()};
            for (const Vec3& point:face.vertices)
            {
                result.minimum.x=(std::min)(result.minimum.x,point.x);
                result.minimum.y=(std::min)(result.minimum.y,point.y);
                result.minimum.z=(std::min)(result.minimum.z,point.z);
                result.maximum.x=(std::max)(result.maximum.x,point.x);
                result.maximum.y=(std::max)(result.maximum.y,point.y);
                result.maximum.z=(std::max)(result.maximum.z,point.z);
            }
            return result;
        }

        double FaceArea(const Face& face)
        {
            double twice=0;
            for (std::size_t index=1;index+1<face.vertices.size();++index)
                twice+=Dot(face.normal,Cross(Subtract(face.vertices[index],face.vertices.front()),
                    Subtract(face.vertices[index+1],face.vertices.front())));
            return twice*0.5;
        }

        bool ContainsFace(const Face& outer,const Face& inner,Work& work)
        {
            for (std::size_t index=0;index<outer.vertices.size();++index)
            {
                if (!work.Spend(inner.vertices.size())) return false;
                const Vec3& start=outer.vertices[index];
                const Vec3 edge=Subtract(outer.vertices[(index+1)%outer.vertices.size()],start);
                const double length=std::hypot(edge.x,edge.y,edge.z);
                if (length<=work.limits.epsilon) return false;
                for (const Vec3& point:inner.vertices)
                    if (Dot(outer.normal,Cross(edge,Subtract(point,start))) < -work.limits.epsilon*length)
                        return false;
            }
            return true;
        }

        bool BehindPlanes(const Brush& planes,const Brush& points,std::size_t excluded,Work& work)
        {
            for (std::size_t index=0;index<planes.faces.size();++index)
            {
                if (index==excluded) continue;
                const Face& plane=planes.faces[index];
                for (const Face& face:points.faces)
                {
                    if (!work.Spend(face.vertices.size())) return false;
                    for (const Vec3& point:face.vertices)
                        if (Dot(plane.normal,Subtract(point,plane.vertices.front()))>work.limits.epsilon)
                            return false;
                }
            }
            return true;
        }

        double BrushVolume(const Brush& brush,const Vec3& reference)
        {
            double six=0;
            for (const Face& face:brush.faces)
                for (std::size_t index=1;index+1<face.vertices.size();++index)
                    six+=Dot(Subtract(face.vertices[0],reference),Cross(Subtract(face.vertices[index],reference),
                        Subtract(face.vertices[index+1],reference)));
            return six/6.0;
        }

        bool TryMerge(const Brush& a,std::size_t aFace,const Brush& b,std::size_t bFace,
                       Brush& merged,Work& work)
        {
            const Face& first=a.faces[aFace];
            const Face& second=b.faces[bFace];
            if (a.subtractive!=b.subtractive || !Near(first.normal,Scale(second.normal,-1),1e-12)) return false;
            for (const Vec3& point:second.vertices)
                if (std::fabs(Dot(first.normal,Subtract(point,first.vertices.front())))>work.limits.epsilon)
                    return false;
            if (!ContainsFace(first,second,work) || !ContainsFace(second,first,work)
                || !BehindPlanes(a,b,aFace,work) || !BehindPlanes(b,a,bFace,work)) return false;

            Bounds bounds=FaceBounds(first);
            double area=0;
            for (const Brush* brush:{&a,&b}) for (const Face& face:brush->faces)
            {
                area+=FaceArea(face);
                const Bounds part=FaceBounds(face);
                bounds.minimum.x=(std::min)(bounds.minimum.x,part.minimum.x);
                bounds.minimum.y=(std::min)(bounds.minimum.y,part.minimum.y);
                bounds.minimum.z=(std::min)(bounds.minimum.z,part.minimum.z);
                bounds.maximum.x=(std::max)(bounds.maximum.x,part.maximum.x);
                bounds.maximum.y=(std::max)(bounds.maximum.y,part.maximum.y);
                bounds.maximum.z=(std::max)(bounds.maximum.z,part.maximum.z);
            }
            const Vec3 reference=bounds.minimum;
            bounds.minimum=Subtract(bounds.minimum,{1,1,1});
            bounds.maximum=Add(bounds.maximum,{1,1,1});
            Cell cell=MakeBox(bounds);
            for (const Brush* brush:{&a,&b}) for (std::size_t index=0;index<brush->faces.size();++index)
            {
                if (index==(brush==&a?aFace:bFace)) continue;
                const Face& face=brush->faces[index];
                Cell next;
                if (!Clip(cell,face.normal,Dot(face.normal,face.vertices.front()),face.surfaceIndex,next,work)) return false;
                cell=std::move(next);
                if (cell.empty() || cell.size()>work.limits.maxFacesPerBrush) return false;
            }
            if (std::any_of(cell.begin(),cell.end(),[](const CellFace& face){return face.artificialBoundary;})) return false;
            // A candidate that cannot retain a closed boundary is simply not
            // merged; the original independently valid cells remain intact.
            const std::string previousError=work.error;
            if (!ValidateClosedCell(cell,work))
            {
                if (!work.exhausted) work.error=previousError;
                return false;
            }
            merged.subtractive=a.subtractive;
            for (CellFace& face:cell) merged.faces.push_back(std::move(face.face));
            const double before=BrushVolume(a,reference)+BrushVolume(b,reference);
            const double after=BrushVolume(merged,reference);
            if (!std::isfinite(before) || !std::isfinite(after) || before<=0 || after<=0
                || std::fabs(after-before)>work.limits.epsilon*area+std::fabs(before)*1e-12)
            {
                merged.faces.clear();
                return false;
            }
            return true;
        }
    }

    bool MergeAdjacentConvexBrushes(Result& result,std::string& error,const Limits& limits)
    try
    {
        error.clear();
        if (!std::isfinite(limits.epsilon) || limits.epsilon<=0 || result.brushes.size()>limits.maxBrushes)
        {
            error="Convex brush merging requires valid geometry limits.";
            return false;
        }
        Work work{limits,0,error};
        std::size_t faceCount=0;
        for (const Brush& brush:result.brushes)
        {
            if (brush.faces.size()<4 || brush.faces.size()>limits.maxFacesPerBrush
                || brush.faces.size()>limits.maxTotalFaces-faceCount)
            {
                error="A brush exceeds the convex merge face limits.";
                return false;
            }
            faceCount+=brush.faces.size();
            Cell cell;
            for (const Face& face:brush.faces)
            {
                if (face.vertices.size()<3 || !Finite(face.normal) || std::fabs(Dot(face.normal,face.normal)-1)>1e-10)
                {
                    error="Convex brush merging requires finite unit face normals.";
                    return false;
                }
                cell.push_back({face,false});
            }
            if (!ValidateClosedCell(cell,work)) return false;
            if (!BehindPlanes(brush,brush,brush.faces.size(),work))
            {
                if (!work.exhausted) error="Convex brush merging received a non-convex brush.";
                return false;
            }
        }
        std::vector<Brush> brushes=result.brushes;
        using Key=std::tuple<double,double,double,long long>;
        struct Reference { std::size_t brush,face; Bounds bounds; };
        struct Candidate { std::size_t a,b,aFace,bFace; double area; };
        // Disjoint merges in each pass permit deterministic ordering without
        // invalidating queued face indices. The cap bounds even hostile data.
        for (unsigned pass=0;pass<64;++pass)
        {
            std::map<Key,std::vector<Reference>> groups;
            std::vector<Candidate> candidates;
            for (std::size_t bi=0;bi<brushes.size();++bi)
                for (std::size_t fi=0;fi<brushes[bi].faces.size();++fi)
                {
                    const Face& face=brushes[bi].faces[fi];
                    Vec3 normal=face.normal;
                    if (normal.x<0 || (normal.x==0 && normal.y<0)
                        || (normal.x==0 && normal.y==0 && normal.z<0)) normal=Scale(normal,-1);
                    const double distance=std::floor(Dot(normal,face.vertices.front()));
                    if (!std::isfinite(distance) || std::fabs(distance)>=9e18)
                    {
                        error="A brush plane exceeds convex merge indexing limits.";
                        return false;
                    }
                    const auto bucket=static_cast<long long>(distance);
                    const Bounds bounds=FaceBounds(face);
                    for (long long offset=-1;offset<=1;++offset)
                    {
                        const auto found=groups.find({normal.x,normal.y,normal.z,bucket+offset});
                        if (found==groups.end()) continue;
                        for (const Reference& other:found->second)
                        {
                            if (!work.Spend(1)) return false;
                            if (other.brush==bi || brushes[other.brush].subtractive!=brushes[bi].subtractive
                                || !Near(bounds.minimum,other.bounds.minimum,limits.epsilon)
                                || !Near(bounds.maximum,other.bounds.maximum,limits.epsilon)) continue;
                            const Face& old=brushes[other.brush].faces[other.face];
                            if (Dot(old.normal,face.normal)>0) continue;
                            if (candidates.size()>=500000)
                            {
                                error="Convex brush merging exceeded its candidate budget.";
                                return false;
                            }
                            candidates.push_back({other.brush,bi,other.face,fi,FaceArea(face)});
                        }
                    }
                    groups[{normal.x,normal.y,normal.z,bucket}].push_back({bi,fi,bounds});
                }
            std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b)
            {
                if (a.area!=b.area) return a.area>b.area;
                return std::tie(a.a,a.b,a.aFace,a.bFace)<std::tie(b.a,b.b,b.aFace,b.bFace);
            });
            std::vector<bool> changed(brushes.size(),false),removed(brushes.size(),false);
            std::size_t mergedCount=0;
            for (const Candidate& candidate:candidates)
            {
                if (changed[candidate.a] || changed[candidate.b]) continue;
                Brush merged;
                if (!TryMerge(brushes[candidate.a],candidate.aFace,brushes[candidate.b],candidate.bFace,merged,work))
                {
                    if (work.exhausted) return false;
                    error.clear();
                    continue;
                }
                brushes[candidate.a]=std::move(merged);
                removed[candidate.b]=true;
                changed[candidate.a]=changed[candidate.b]=true;
                ++mergedCount;
            }
            if (!mergedCount) break;
            std::vector<Brush> remaining;
            remaining.reserve(brushes.size()-mergedCount);
            for (std::size_t index=0;index<brushes.size();++index)
                if (!removed[index]) remaining.push_back(std::move(brushes[index]));
            brushes=std::move(remaining);
        }
        result.brushes=std::move(brushes);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        error="There is insufficient memory to merge recovered brushes."; return false;
    }
    catch (const std::length_error&)
    {
        error="Convex brush merging exceeds container capacity."; return false;
    }

    namespace
    {
        Bounds BrushBounds(const Brush& brush)
        {
            Bounds result=FaceBounds(brush.faces.front());
            for (const Face& face:brush.faces)
            {
                const Bounds part=FaceBounds(face);
                result.minimum.x=(std::min)(result.minimum.x,part.minimum.x);
                result.minimum.y=(std::min)(result.minimum.y,part.minimum.y);
                result.minimum.z=(std::min)(result.minimum.z,part.minimum.z);
                result.maximum.x=(std::max)(result.maximum.x,part.maximum.x);
                result.maximum.y=(std::max)(result.maximum.y,part.maximum.y);
                result.maximum.z=(std::max)(result.maximum.z,part.maximum.z);
            }
            return result;
        }

        bool BoundsMeet(const Bounds& a,const Bounds& b,double margin)
        {
            return a.minimum.x<=b.maximum.x+margin && b.minimum.x<=a.maximum.x+margin
                && a.minimum.y<=b.maximum.y+margin && b.minimum.y<=a.maximum.y+margin
                && a.minimum.z<=b.maximum.z+margin && b.minimum.z<=a.maximum.z+margin;
        }

        Cell ToCell(const Brush& brush)
        {
            Cell cell;
            cell.reserve(brush.faces.size());
            for (const Face& face:brush.faces) cell.push_back({face,false});
            return cell;
        }

        double CellVolume(const Cell& cell)
        {
            double six=0;
            const Vec3 reference=cell.front().face.vertices.front();
            for (const CellFace& face:cell)
                for (std::size_t index=1;index+1<face.face.vertices.size();++index)
                    six+=Dot(Subtract(face.face.vertices[0],reference),
                        Cross(Subtract(face.face.vertices[index],reference),
                              Subtract(face.face.vertices[index+1],reference)));
            return six/6.0;
        }

        // Two brushes share a face region: coplanar, opposite-facing faces
        // whose bounds overlap. Brushes meeting only at an edge or corner are
        // left to pairs that do share a face.
        bool ShareFace(const Brush& a,const Brush& b,double epsilon)
        {
            for (const Face& first:a.faces)
            {
                const Bounds firstBounds=FaceBounds(first);
                for (const Face& second:b.faces)
                {
                    if (Dot(first.normal,second.normal)>-1+1e-9) continue;
                    if (std::fabs(Dot(first.normal,Subtract(second.vertices.front(),first.vertices.front())))>epsilon)
                        continue;
                    if (BoundsMeet(firstBounds,FaceBounds(second),epsilon)) return true;
                }
            }
            return false;
        }

        // Uniform grid over the reference cells for hull emptiness queries.
        struct CellGrid
        {
            Bounds extent;
            int size[3]{1,1,1};
            double step[3]{1,1,1};
            std::vector<std::vector<std::size_t>> buckets;

            void Range(const Bounds& box,int low[3],int high[3]) const
            {
                const double minimum[3]={box.minimum.x,box.minimum.y,box.minimum.z};
                const double maximum[3]={box.maximum.x,box.maximum.y,box.maximum.z};
                const double origin[3]={extent.minimum.x,extent.minimum.y,extent.minimum.z};
                for (int axis=0;axis<3;++axis)
                {
                    low[axis]=static_cast<int>(std::floor((minimum[axis]-origin[axis])/step[axis]));
                    high[axis]=static_cast<int>(std::floor((maximum[axis]-origin[axis])/step[axis]));
                    low[axis]=(std::max)(0,(std::min)(size[axis]-1,low[axis]));
                    high[axis]=(std::max)(0,(std::min)(size[axis]-1,high[axis]));
                }
            }
        };
    }

    bool GrowConvexBrushes(const Result& reference,Result& result,bool allowOverlap,GrowStatistics& statistics,
                           std::string& error,const Limits& limits)
    try
    {
        statistics={};
        error.clear();
        if (!std::isfinite(limits.epsilon) || limits.epsilon<=0 || result.brushes.size()>limits.maxBrushes
            || reference.brushes.size()>limits.maxBrushes)
        {
            error="Convex brush growth requires valid geometry limits.";
            return false;
        }
        if (result.brushes.empty()) return true;
        const bool subtractive=result.brushes.front().subtractive;
        const std::vector<Brush>* sets[]={&reference.brushes,&result.brushes};
        for (const std::vector<Brush>* set:sets)
            for (const Brush& brush:*set)
                if (brush.subtractive!=subtractive || brush.faces.size()<4
                    || brush.faces.size()>limits.maxFacesPerBrush
                    || std::any_of(brush.faces.begin(),brush.faces.end(),[](const Face& face)
                        { return face.vertices.size()<3 || !Finite(face.normal); }))
                {
                    error="Convex brush growth requires closed brushes of a single CSG kind.";
                    return false;
                }

        Work work{limits,0,error};
        CellGrid grid;
        std::vector<Bounds> cellBounds;
        cellBounds.reserve(reference.brushes.size());
        for (const Brush& cell:reference.brushes) cellBounds.push_back(BrushBounds(cell));
        if (!cellBounds.empty())
        {
            grid.extent=cellBounds.front();
            for (const Bounds& box:cellBounds)
            {
                grid.extent.minimum.x=(std::min)(grid.extent.minimum.x,box.minimum.x);
                grid.extent.minimum.y=(std::min)(grid.extent.minimum.y,box.minimum.y);
                grid.extent.minimum.z=(std::min)(grid.extent.minimum.z,box.minimum.z);
                grid.extent.maximum.x=(std::max)(grid.extent.maximum.x,box.maximum.x);
                grid.extent.maximum.y=(std::max)(grid.extent.maximum.y,box.maximum.y);
                grid.extent.maximum.z=(std::max)(grid.extent.maximum.z,box.maximum.z);
            }
            const double span[3]={grid.extent.maximum.x-grid.extent.minimum.x,
                grid.extent.maximum.y-grid.extent.minimum.y,grid.extent.maximum.z-grid.extent.minimum.z};
            const int divisions=static_cast<int>((std::min)(48.0,(std::max)(1.0,std::cbrt(double(cellBounds.size())))));
            for (int axis=0;axis<3;++axis)
            {
                grid.size[axis]=divisions;
                grid.step[axis]=(std::max)(span[axis]/divisions,limits.epsilon*16);
            }
            grid.buckets.resize(static_cast<std::size_t>(grid.size[0])*grid.size[1]*grid.size[2]);
            for (std::size_t index=0;index<cellBounds.size();++index)
            {
                int low[3],high[3];
                grid.Range(cellBounds[index],low,high);
                for (int x=low[0];x<=high[0];++x) for (int y=low[1];y<=high[1];++y) for (int z=low[2];z<=high[2];++z)
                    grid.buckets[(static_cast<std::size_t>(x)*grid.size[1]+y)*grid.size[2]+z].push_back(index);
            }
        }
        std::vector<unsigned> stamp(reference.brushes.size(),0);
        std::vector<double> cellVolumes;
        cellVolumes.reserve(reference.brushes.size());
        for (const Brush& cell:reference.brushes) cellVolumes.push_back(Volume(cell));
        unsigned query=0;
        // Cheap rejection before measuring volumes: a point of the candidate
        // region that no reference cell contains proves it reaches solid space.
        auto insideReference=[&](const Vec3& point)
        {
            if (grid.buckets.empty()) return false;
            int low[3],high[3];
            grid.Range({point,point},low,high);
            for (std::size_t cellIndex:grid.buckets[(static_cast<std::size_t>(low[0])*grid.size[1]+low[1])*grid.size[2]+low[2]])
            {
                const Bounds& box=cellBounds[cellIndex];
                if (point.x<box.minimum.x-limits.epsilon || point.x>box.maximum.x+limits.epsilon
                    || point.y<box.minimum.y-limits.epsilon || point.y>box.maximum.y+limits.epsilon
                    || point.z<box.minimum.z-limits.epsilon || point.z>box.maximum.z+limits.epsilon) continue;
                const Brush& cell=reference.brushes[cellIndex];
                if (std::all_of(cell.faces.begin(),cell.faces.end(),[&](const Face& face)
                    { return Dot(face.normal,Subtract(point,face.vertices.front()))<=limits.epsilon; }))
                    return true;
            }
            return false;
        };

        std::vector<Brush> brushes=result.brushes;
        // Identities survive the per-pass compaction, so a pair already shown
        // not to fit is not measured again until one of its brushes changes.
        std::vector<std::uint64_t> identity(brushes.size());
        std::vector<std::vector<std::size_t>> members(brushes.size());
        for (std::size_t index=0;index<members.size();++index) members[index]={index};
        std::uint64_t nextIdentity=0;
        for (auto& value:identity) value=nextIdentity++;
        std::set<std::pair<std::uint64_t,std::uint64_t>> rejected;
        bool stopped=false;
        for (unsigned pass=0;pass<64 && !stopped;++pass)
        {
            std::vector<Bounds> bounds;
            std::vector<double> volume;
            bounds.reserve(brushes.size());
            volume.reserve(brushes.size());
            for (const Brush& brush:brushes)
            {
                bounds.push_back(BrushBounds(brush));
                volume.push_back(BrushVolume(brush,brush.faces.front().vertices.front()));
            }
            // Sweep along X for brushes whose bounds meet, then require a
            // shared face region.
            std::vector<std::size_t> order(brushes.size());
            for (std::size_t index=0;index<order.size();++index) order[index]=index;
            std::sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b)
            {
                return bounds[a].minimum.x<bounds[b].minimum.x
                    || (bounds[a].minimum.x==bounds[b].minimum.x && a<b);
            });
            std::vector<std::pair<std::size_t,std::size_t>> pairs;
            std::vector<std::vector<std::size_t>> neighbours(brushes.size());
            for (std::size_t i=0;i<order.size() && !stopped;++i)
                for (std::size_t j=i+1;j<order.size();++j)
                {
                    const std::size_t a=order[i],b=order[j];
                    if (bounds[b].minimum.x>bounds[a].maximum.x+limits.epsilon) break;
                    if (!work.Spend(1)) { stopped=true; break; }
                    if (!BoundsMeet(bounds[a],bounds[b],limits.epsilon)
                        || !ShareFace(brushes[a],brushes[b],limits.epsilon)) continue;
                    neighbours[a].push_back(b);
                    neighbours[b].push_back(a);
                    pairs.emplace_back((std::min)(a,b),(std::max)(a,b));
                }
            if (stopped) break;
            // Brushes on either side of a common neighbour can also span it:
            // the arms of a crossing become one brush running through the
            // brush that crosses them.
            for (const auto& around:neighbours)
                for (std::size_t i=0;i<around.size() && !stopped;++i)
                    for (std::size_t j=i+1;j<around.size();++j)
                    {
                        if (pairs.size()>=2000000 || !work.Spend(1)) { stopped=true; break; }
                        pairs.emplace_back((std::min)(around[i],around[j]),(std::max)(around[i],around[j]));
                    }
            if (stopped) break;
            std::sort(pairs.begin(),pairs.end());
            pairs.erase(std::unique(pairs.begin(),pairs.end()),pairs.end());
            // Largest combined volume first: rooms absorb their fragments
            // before small neighbours can claim them.
            std::sort(pairs.begin(),pairs.end(),[&](const auto& x,const auto& y)
            {
                const double vx=volume[x.first]+volume[x.second],vy=volume[y.first]+volume[y.second];
                if (vx!=vy) return vx>vy;
                return x<y;
            });
            std::vector<bool> changed(brushes.size(),false),removed(brushes.size(),false);
            std::size_t merged=0;
            for (const auto& [a,b]:pairs)
            {
                if (changed[a] || changed[b] || removed[a] || removed[b]) continue;
                const std::pair<std::uint64_t,std::uint64_t> key{(std::min)(identity[a],identity[b]),
                                                                  (std::max)(identity[a],identity[b])};
                if (rejected.count(key)) continue;
                struct Reject
                {
                    std::set<std::pair<std::uint64_t,std::uint64_t>>& set;
                    std::pair<std::uint64_t,std::uint64_t> key;
                    bool active=true;
                    ~Reject() { if (active) set.insert(key); }
                } reject{rejected,key};
                Bounds box=bounds[a];
                box.minimum.x=(std::min)(box.minimum.x,bounds[b].minimum.x)-1;
                box.minimum.y=(std::min)(box.minimum.y,bounds[b].minimum.y)-1;
                box.minimum.z=(std::min)(box.minimum.z,bounds[b].minimum.z)-1;
                box.maximum.x=(std::max)(box.maximum.x,bounds[b].maximum.x)+1;
                box.maximum.y=(std::max)(box.maximum.y,bounds[b].maximum.y)+1;
                box.maximum.z=(std::max)(box.maximum.z,bounds[b].maximum.z)+1;
                Cell hull=MakeBox(box);
                bool usable=true;
                for (const Brush* self:{&brushes[a],&brushes[b]})
                {
                    const Brush& other=self==&brushes[a] ? brushes[b] : brushes[a];
                    for (const Face& face:self->faces)
                    {
                        if (!work.Spend(1)) { usable=false; stopped=true; break; }
                        bool supporting=true;
                        for (const Face& otherFace:other.faces)
                        {
                            for (const Vec3& point:otherFace.vertices)
                                if (Dot(face.normal,Subtract(point,face.vertices.front()))>limits.epsilon)
                                {
                                    supporting=false;
                                    break;
                                }
                            if (!supporting) break;
                        }
                        if (!supporting) continue;
                        Cell next;
                        if (!Clip(hull,face.normal,Dot(face.normal,face.vertices.front()),face.surfaceIndex,next,work))
                        {
                            usable=false;
                            if (work.exhausted) stopped=true;
                            break;
                        }
                        hull=std::move(next);
                        if (hull.empty() || hull.size()>limits.maxFacesPerBrush) { usable=false; break; }
                    }
                    if (!usable) break;
                }
                if (stopped) { reject.active=false; break; }
                // A face of the clipping box left over means the supporting
                // planes do not close the region: it is not a candidate.
                if (!usable || std::any_of(hull.begin(),hull.end(),[](const CellFace& face)
                        { return face.artificialBoundary; }))
                {
                    error.clear();
                    continue;
                }
                const std::string previousError=error;
                if (!ValidateClosedCell(hull,work))
                {
                    if (work.exhausted) { reject.active=false; stopped=true; break; }
                    error=previousError;
                    continue;
                }
                {
                    Vec3 centre{};
                    std::size_t corners=0;
                    for (const CellFace& face:hull)
                        for (const Vec3& point:face.face.vertices) { centre=Add(centre,point); ++corners; }
                    centre=Scale(centre,1.0/static_cast<double>(corners));
                    // Corners, edge midpoints and face centres pulled slightly
                    // inwards, then points part-way to the centre.
                    bool plausible=insideReference(centre);
                    for (std::size_t faceIndex=0;faceIndex<hull.size() && plausible;++faceIndex)
                    {
                        const Face& face=hull[faceIndex].face;
                        Vec3 middle{};
                        for (std::size_t index=0;index<face.vertices.size() && plausible;++index)
                        {
                            const Vec3& point=face.vertices[index];
                            const Vec3 edge=Scale(Add(point,face.vertices[(index+1)%face.vertices.size()]),0.5);
                            middle=Add(middle,point);
                            for (const Vec3& sample:{point,edge})
                                for (double depth:{1e-3,0.25,0.5,0.75})
                                    if (plausible && !insideReference(Add(sample,Scale(Subtract(centre,sample),depth))))
                                        plausible=false;
                        }
                        middle=Scale(middle,1.0/static_cast<double>(face.vertices.size()));
                        for (double depth:{1e-3,0.25,0.5,0.75})
                            if (plausible && !insideReference(Add(middle,Scale(Subtract(centre,middle),depth))))
                                plausible=false;
                    }
                    if (!plausible) continue;
                }
                double hullArea=0;
                for (const CellFace& face:hull) hullArea+=FaceArea(face.face);
                const double hullVolume=CellVolume(hull);
                const Bounds hullBounds=[&]
                {
                    Bounds result=FaceBounds(hull.front().face);
                    for (const CellFace& face:hull)
                    {
                        const Bounds part=FaceBounds(face.face);
                        result.minimum.x=(std::min)(result.minimum.x,part.minimum.x);
                        result.minimum.y=(std::min)(result.minimum.y,part.minimum.y);
                        result.minimum.z=(std::min)(result.minimum.z,part.minimum.z);
                        result.maximum.x=(std::max)(result.maximum.x,part.maximum.x);
                        result.maximum.y=(std::max)(result.maximum.y,part.maximum.y);
                        result.maximum.z=(std::max)(result.maximum.z,part.maximum.z);
                    }
                    return result;
                }();
                // The grown region must be covered by the original cells. They
                // are disjoint, so the clipped volumes add up to the region's
                // own volume exactly when no part of it reaches outside them.
                double covered=0;
                bool measured=true;
                if (++query==0) { std::fill(stamp.begin(),stamp.end(),0u); query=1; }
                if (!grid.buckets.empty())
                {
                    int low[3],high[3];
                    grid.Range(hullBounds,low,high);
                    for (int x=low[0];x<=high[0] && measured;++x)
                        for (int y=low[1];y<=high[1] && measured;++y)
                            for (int z=low[2];z<=high[2] && measured;++z)
                                for (std::size_t cellIndex:grid.buckets[(static_cast<std::size_t>(x)*grid.size[1]+y)*grid.size[2]+z])
                                {
                                    if (stamp[cellIndex]==query) continue;
                                    stamp[cellIndex]=query;
                                    if (!BoundsMeet(cellBounds[cellIndex],hullBounds,-limits.epsilon)) continue;
                                    // Whole cells inside or outside need no clipping.
                                    const Brush& cell=reference.brushes[cellIndex];
                                    bool outside=false,within=true;
                                    for (const CellFace& face:hull)
                                    {
                                        double nearest=std::numeric_limits<double>::infinity(),farthest=-nearest;
                                        const double distance=Dot(face.face.normal,face.face.vertices.front());
                                        for (const Face& cellFace:cell.faces)
                                            for (const Vec3& point:cellFace.vertices)
                                            {
                                                const double side=Dot(face.face.normal,point)-distance;
                                                nearest=(std::min)(nearest,side);
                                                farthest=(std::max)(farthest,side);
                                            }
                                        if (nearest>=-limits.epsilon) { outside=true; break; }
                                        if (farthest>limits.epsilon) within=false;
                                    }
                                    if (outside) continue;
                                    if (within) { covered+=cellVolumes[cellIndex]; continue; }
                                    Cell clipped=ToCell(cell);
                                    for (const CellFace& face:hull)
                                    {
                                        Cell next;
                                        if (!Clip(clipped,face.face.normal,Dot(face.face.normal,face.face.vertices.front()),
                                                  -1,next,work))
                                        {
                                            measured=false;
                                            break;
                                        }
                                        clipped=std::move(next);
                                        if (clipped.empty()) break;
                                    }
                                    if (!measured) break;
                                    if (!clipped.empty()) covered+=CellVolume(clipped);
                                }
                }
                if (!measured)
                {
                    if (work.exhausted) { reject.active=false; stopped=true; break; }
                    error.clear();
                    continue;
                }
                if (!std::isfinite(covered) || !std::isfinite(hullVolume) || hullVolume<=0
                    || std::fabs(covered-hullVolume)>limits.epsilon*hullArea+hullVolume*1e-12)
                    continue;
                if (!allowOverlap)
                {
                    // Every other brush must lie wholly inside the region (it
                    // is then absorbed) or wholly outside it, so the brushes
                    // stay disjoint: the region is then exactly their union.
                    bool partial=false;
                    std::vector<Vec3> hullPoints;
                    for (const CellFace& face:hull)
                        hullPoints.insert(hullPoints.end(),face.face.vertices.begin(),face.face.vertices.end());
                    for (std::size_t other=0;other<brushes.size() && !partial;++other)
                    {
                        if (other==a || other==b || removed[other]) continue;
                        const Bounds otherBounds=changed[other] ? BrushBounds(brushes[other]) : bounds[other];
                        if (!BoundsMeet(otherBounds,hullBounds,-limits.epsilon)) continue;
                        const Brush& candidate=brushes[other];
                        auto beyond=[&](const Face& plane,const auto& points)
                        {
                            for (const Vec3& point:points)
                                if (Dot(plane.normal,Subtract(point,plane.vertices.front()))<-limits.epsilon) return false;
                            return true;
                        };
                        std::vector<Vec3> candidatePoints;
                        for (const Face& face:candidate.faces)
                            candidatePoints.insert(candidatePoints.end(),face.vertices.begin(),face.vertices.end());
                        bool within=true;
                        for (const Vec3& point:candidatePoints)
                        {
                            for (const CellFace& face:hull)
                                if (Dot(face.face.normal,Subtract(point,face.face.vertices.front()))>limits.epsilon)
                                {
                                    within=false;
                                    break;
                                }
                            if (!within) break;
                        }
                        if (within) continue;
                        if (std::any_of(hull.begin(),hull.end(),[&](const CellFace& face){ return beyond(face.face,candidatePoints); })
                            || std::any_of(candidate.faces.begin(),candidate.faces.end(),[&](const Face& face){ return beyond(face,hullPoints); }))
                            continue;
                        // No separating face: measure the shared volume.
                        Cell clipped=ToCell(candidate);
                        for (const CellFace& face:hull)
                        {
                            Cell next;
                            if (!Clip(clipped,face.face.normal,Dot(face.face.normal,face.face.vertices.front()),-1,next,work))
                            {
                                partial=true;
                                break;
                            }
                            clipped=std::move(next);
                            if (clipped.empty()) break;
                        }
                        if (work.exhausted) break;
                        if (!partial && !clipped.empty() && CellVolume(clipped)>limits.epsilon*hullArea) partial=true;
                    }
                    if (work.exhausted) { reject.active=false; stopped=true; break; }
                    error.clear();
                    if (partial) continue;
                }
                reject.active=false;
                Brush grown;
                grown.subtractive=subtractive;
                grown.faces.reserve(hull.size());
                for (CellFace& face:hull) grown.faces.push_back(std::move(face.face));
                brushes[a]=std::move(grown);
                identity[a]=nextIdentity++;
                changed[a]=true;
                removed[b]=true;
                members[a].insert(members[a].end(),members[b].begin(),members[b].end());
                ++merged;
                ++statistics.grown;
                // Any other brush now wholly inside the grown one is redundant.
                const Bounds& grownBounds=hullBounds;
                for (std::size_t other=0;other<brushes.size();++other)
                {
                    if (other==a || removed[other] || !BoundsMeet(bounds[other],grownBounds,limits.epsilon)) continue;
                    bool inside=true;
                    for (const Face& face:brushes[other].faces)
                    {
                        for (const Vec3& point:face.vertices)
                        {
                            for (const Face& plane:brushes[a].faces)
                                if (Dot(plane.normal,Subtract(point,plane.vertices.front()))>limits.epsilon)
                                {
                                    inside=false;
                                    break;
                                }
                            if (!inside) break;
                        }
                        if (!inside) break;
                    }
                    if (!inside) continue;
                    removed[other]=true;
                    members[a].insert(members[a].end(),members[other].begin(),members[other].end());
                    ++merged;
                    ++statistics.absorbed;
                }
            }
            if (stopped) work.error.clear();
            std::vector<Brush> remaining;
            std::vector<std::uint64_t> remainingIdentity;
            std::vector<std::vector<std::size_t>> remainingMembers;
            remaining.reserve(brushes.size());
            for (std::size_t index=0;index<brushes.size();++index)
                if (!removed[index])
                {
                    remaining.push_back(std::move(brushes[index]));
                    remainingIdentity.push_back(identity[index]);
                    remainingMembers.push_back(std::move(members[index]));
                }
            brushes=std::move(remaining);
            identity=std::move(remainingIdentity);
            members=std::move(remainingMembers);
            if (!merged) break;
        }
        statistics.budgetReached=stopped;
        for (auto& list:members) std::sort(list.begin(),list.end());
        statistics.members=std::move(members);
        error.clear();
        result.brushes=std::move(brushes);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        error="There is insufficient memory to grow recovered brushes."; return false;
    }
    catch (const std::length_error&)
    {
        error="Convex brush growth exceeds container capacity."; return false;
    }

    double Volume(const Brush& brush)
    {
        if (brush.faces.empty() || brush.faces.front().vertices.empty()) return 0;
        return BrushVolume(brush,brush.faces.front().vertices.front());
    }

    double MinimumWidth(const Brush& brush)
    {
        double narrowest=std::numeric_limits<double>::infinity();
        for (const Face& face:brush.faces)
        {
            if (face.vertices.empty()) continue;
            double deepest=0;
            for (const Face& other:brush.faces)
                for (const Vec3& point:other.vertices)
                    deepest=(std::max)(deepest,-Dot(face.normal,Subtract(point,face.vertices.front())));
            narrowest=(std::min)(narrowest,deepest);
        }
        return std::isfinite(narrowest) ? narrowest : 0;
    }

    bool OrderForRebuild(Result& result,std::string& error)
    try
    {
        error.clear();
        std::vector<std::pair<double,std::size_t>> order;
        order.reserve(result.brushes.size());
        for (std::size_t index=0;index<result.brushes.size();++index)
        {
            const Brush& brush=result.brushes[index];
            if (brush.subtractive!=result.brushes.front().subtractive)
            {
                error="Mixed additive and subtractive brushes cannot be reordered safely.";
                return false;
            }
            if (brush.faces.empty() || brush.faces.front().vertices.empty())
            {
                error="A recovered brush has no geometry for rebuild ordering.";
                return false;
            }
            const double volume=BrushVolume(brush,brush.faces.front().vertices.front());
            if (!std::isfinite(volume) || volume<=0)
            {
                error="A recovered brush has invalid volume for rebuild ordering.";
                return false;
            }
            order.emplace_back(volume,index);
        }
        std::stable_sort(order.begin(),order.end(),[](const auto& a,const auto& b)
        {
            return a.first>b.first;
        });
        std::vector<Brush> ordered;
        ordered.reserve(result.brushes.size());
        for (const auto& entry:order)
            ordered.push_back(std::move(result.brushes[entry.second]));
        result.brushes=std::move(ordered);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        error="There is insufficient memory to order the recovered brushes."; return false;
    }
    catch (const std::length_error&)
    {
        error="Rebuild brush ordering exceeds container capacity."; return false;
    }

    bool Reconstruct(const std::vector<Node>& input, bool rootOutside,
                     const Bounds& extractionBounds, Result& result,
                     std::string& error, const Limits& limits)
    try
    {
        result = {};
        error.clear();
        if (!std::isfinite(limits.epsilon) || limits.epsilon <= 0
            || !Finite(extractionBounds.minimum) || !Finite(extractionBounds.maximum))
        {
            error = "BSP reconstruction requires finite bounds and a positive tolerance.";
            return false;
        }
        const Vec3 extent = Subtract(extractionBounds.maximum, extractionBounds.minimum);
        if (!Finite(extent) || extent.x <= limits.epsilon
            || extent.y <= limits.epsilon || extent.z <= limits.epsilon)
        {
            error = "BSP reconstruction bounds must enclose a nonzero three-dimensional region.";
            return false;
        }
        if (limits.maxFacesPerBrush < 6 || limits.maxPendingCells == 0)
        {
            error = "BSP reconstruction limits cannot accommodate the initial clipping volume.";
            return false;
        }

        std::vector<Node> nodes;
        if (!ValidateNodes(input, nodes, limits, error))
            return false;

        struct PendingCell
        {
            int nodeIndex;
            bool outside;
            Cell cell;
        };
        std::vector<PendingCell> pending;
        pending.push_back({ nodes.empty() ? -1 : 0, rootOutside, MakeBox(extractionBounds) });
        Work work{ limits, 0, error };
        Result reconstructed;
        std::size_t totalFaces = 0;
        while (!pending.empty())
        {
            PendingCell current = std::move(pending.back());
            pending.pop_back();
            if (!work.Spend(1))
                return false;
            if (current.cell.empty())
                continue;
            if (current.nodeIndex == -1)
            {
                if (current.outside)
                    ++reconstructed.emptyLeafCount;
                else
                    ++reconstructed.solidLeafCount;
                if (current.outside == rootOutside)
                    continue;
                if (std::any_of(current.cell.begin(), current.cell.end(),
                    [](const CellFace& face) { return face.artificialBoundary; }))
                {
                    if (limits.skipUnusableCells)
                    {
                        ++reconstructed.unboundedCells;
                        continue;
                    }
                    error = "A recovered structural cell reaches the extraction bounds. "
                            "Its space is unbounded or the bounds do not enclose the map. "
                            "Expand the bounds or repair the source BSP; no artificial walls were created.";
                    return false;
                }
                if (!ValidateClosedCell(current.cell, work))
                {
                    if (!limits.skipUnusableCells || work.exhausted)
                        return false;
                    error.clear();
                    ++reconstructed.degenerateCells;
                    continue;
                }
                if (reconstructed.brushes.size() >= limits.maxBrushes
                    || current.cell.size() > limits.maxTotalFaces
                    || totalFaces > limits.maxTotalFaces - current.cell.size())
                {
                    error = "The reconstructed map exceeds the brush or total-face limit. "
                            "This map needs a less fragmented source reconstruction.";
                    return false;
                }
                Brush brush;
                brush.subtractive = !rootOutside;
                brush.faces.reserve(current.cell.size());
                for (CellFace& face : current.cell)
                    brush.faces.push_back(std::move(face.face));
                if (limits.minimumCellWidth > 0 && MinimumWidth(brush) < limits.minimumCellWidth)
                {
                    ++reconstructed.sliverCells;
                    continue;
                }
                totalFaces += brush.faces.size();
                reconstructed.brushes.push_back(std::move(brush));
                continue;
            }

            const Node& node = nodes[static_cast<std::size_t>(current.nodeIndex)];
            Cell front, back;
            // A side whose cap cannot be closed is left out when unusable
            // cells are skipped; the other side is still followed.
            auto clip = [&](const Vec3& normal, double distance, Cell& side)
            {
                work.degenerate = false;
                if (Clip(current.cell, normal, distance, node.surfaceIndex, side, work))
                    return true;
                if (!limits.skipUnusableCells || !work.degenerate || work.exhausted)
                    return false;
                error.clear();
                side.clear();
                ++reconstructed.degenerateCells;
                return true;
            };
            if (!clip(Scale(node.normal, -1), -node.distance, front)
                || !clip(node.normal, node.distance, back))
                return false;
            const std::size_t additional = (front.empty() ? 0u : 1u)
                                         + (back.empty() ? 0u : 1u);
            if (additional > limits.maxPendingCells
                || pending.size() > limits.maxPendingCells - additional)
            {
                error = "BSP reconstruction exceeded its pending-cell limit.";
                return false;
            }
            if (!back.empty())
                pending.push_back({ node.back, current.outside && !node.isCsg, std::move(back) });
            if (!front.empty())
                pending.push_back({ node.front, current.outside || node.isCsg, std::move(front) });
        }
        result = std::move(reconstructed);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        result = {};
        error = "The editor ran out of memory while reconstructing BSP cells. "
                "The original map has not been converted.";
        return false;
    }
    catch (const std::length_error&)
    {
        result = {};
        error = "The BSP reconstruction exceeds the container capacity on this platform.";
        return false;
    }
}
