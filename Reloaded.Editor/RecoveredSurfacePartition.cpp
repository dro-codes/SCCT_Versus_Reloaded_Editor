#include "RecoveredSurfacePartition.h"
#include "RecoveredPolygonImport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace RecoveredSurfacePartition
{
    namespace
    {
        Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
        Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
        Vec3 Scale(const Vec3& a, double scale) { return {a.x*scale,a.y*scale,a.z*scale}; }
        double Dot(const Vec3& a, const Vec3& b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
        Vec3 Cross(const Vec3& a, const Vec3& b)
        {
            return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
        }
        bool Finite(const Vec3& point)
        {
            return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
        }
        bool Normalize(Vec3& normal)
        {
            const double length = std::hypot(normal.x,normal.y,normal.z);
            if (!Finite(normal) || !std::isfinite(length) || length < 1e-12)
                return false;
            normal = Scale(normal,1.0/length);
            return true;
        }

        struct Point { double x; double y; };
        using Polygon = std::vector<Point>;

        struct Work
        {
            const Limits& limits;
            std::size_t used = 0;
            std::string& error;
            bool Spend(std::size_t amount)
            {
                if (amount > limits.maxWork || used > limits.maxWork-amount)
                {
                    error = "Surface partitioning exceeded its clipping-work limit.";
                    return false;
                }
                used += amount;
                return true;
            }
        };

        double Side(const Point& point, const Point& a, const Point& b)
        {
            return (b.x-a.x)*(point.y-a.y)-(b.y-a.y)*(point.x-a.x);
        }

        double Area(const Polygon& polygon)
        {
            double area = 0;
            for (std::size_t index = 1; index+1 < polygon.size(); ++index)
                area += Side(polygon[index+1],polygon.front(),polygon[index]);
            return area*0.5;
        }

        void Append(Polygon& polygon, const Point& point)
        {
            if (polygon.empty() || polygon.back().x != point.x || polygon.back().y != point.y)
                polygon.push_back(point);
        }

        void Clean(Polygon& polygon,double flatTolerance=0)
        {
            if (polygon.size() > 1 && polygon.front().x == polygon.back().x
                && polygon.front().y == polygon.back().y)
                polygon.pop_back();
            if (polygon.size() < 3 || Area(polygon) == 0)
                polygon.clear();
            if (flatTolerance>0 && !polygon.empty() && Area(polygon)<0)
            {
                const Point origin=polygon.front();
                Point farthest=origin;
                double longest=0;
                for (const Point point:polygon)
                {
                    const double length=std::hypot(point.x-origin.x,point.y-origin.y);
                    if (length>longest) { longest=length; farthest=point; }
                }
                bool flat=longest<=flatTolerance;
                if (!flat)
                {
                    flat=std::all_of(polygon.begin(),polygon.end(),[&](const Point& point)
                    {
                        return std::fabs(Side(point,origin,farthest))<=flatTolerance*longest;
                    });
                }
                // Only reversed numerical artifacts qualify. Positive thin
                // fragments still contribute to exact complementary coverage
                // and must survive until the whole face is encoded as floats.
                if (flat) polygon.clear();
            }
        }

        struct Frame
        {
            Vec3 origin, normal, u, v;
            Point Project(const Vec3& point) const
            {
                const Vec3 delta = Sub(point,origin);
                return {Dot(delta,u),Dot(delta,v)};
            }
            Vec3 Restore(const Point& point) const
            {
                return Add(origin,Add(Scale(u,point.x),Scale(v,point.y)));
            }
        };

        bool ValidLimits(const Limits& limits, std::string& error)
        {
            if (!std::isfinite(limits.epsilon) || limits.epsilon <= 0
                || !std::isfinite(limits.planeTolerance) || limits.planeTolerance < 0
                || !std::isfinite(limits.normalTolerance) || limits.normalTolerance < 0
                || limits.normalTolerance >= 1 || limits.maxVertices < 3)
            {
                error = "Surface partitioning requires valid tolerances and vertex limits.";
                return false;
            }
            return true;
        }

        bool ValidatePolygon(const Polygon& polygon, Work& work)
        {
            const double area = Area(polygon);
            if (polygon.size() < 3 || !std::isfinite(area) || area <= 0)
            {
                work.error = "A source polygon has zero area or reversed winding.";
                return false;
            }
            for (std::size_t index = 0; index < polygon.size(); ++index)
            {
                if (!work.Spend(polygon.size()))
                    return false;
                const Point a = polygon[index], b = polygon[(index+1)%polygon.size()];
                const double length = std::hypot(b.x-a.x,b.y-a.y);
                if (!std::isfinite(length) || length == 0)
                {
                    work.error = "A source polygon has an invalid or repeated edge.";
                    return false;
                }
                for (std::size_t pointIndex = 0; pointIndex < polygon.size(); ++pointIndex)
                {
                    const Point point = polygon[pointIndex];
                    if (Side(point,a,b) < -work.limits.epsilon*length)
                    {
                        std::ostringstream message;
                        message << "A source polygon is not convex; edge " << index
                                << " places vertex " << pointIndex << " outside by "
                                << -Side(point,a,b)/length << " units.";
                        work.error = message.str();
                        return false;
                    }
                }
            }
            return true;
        }

        bool DisjointBounds(const Polygon& a, const Polygon& b)
        {
            if (a.empty() || b.empty()) return true;
            auto bounds = [](const Polygon& polygon)
            {
                std::array<double,4> result{polygon[0].x,polygon[0].y,polygon[0].x,polygon[0].y};
                for (const Point point : polygon)
                {
                    result[0]=(std::min)(result[0],point.x); result[1]=(std::min)(result[1],point.y);
                    result[2]=(std::max)(result[2],point.x); result[3]=(std::max)(result[3],point.y);
                }
                return result;
            };
            const auto first=bounds(a), second=bounds(b);
            return first[2]<=second[0] || second[2]<=first[0]
                || first[3]<=second[1] || second[3]<=first[1];
        }

        double FloatSourceTolerance(const Surface& source, const Frame& frame)
        {
            // Cooked vertices are float32. Only exact float32 inputs qualify
            // for repairing their near-collinear edge noise; arbitrary double
            // precision polygons retain the caller's strict tolerance.
            Vec3 spacing{};
            for (const Vec3& point : source.vertices)
            {
                const double values[]={point.x,point.y,point.z};
                double* maximum[]={&spacing.x,&spacing.y,&spacing.z};
                for (std::size_t axis=0;axis<3;++axis)
                {
                    if (std::fabs(values[axis])>(std::numeric_limits<float>::max)()) return 0;
                    const float value=static_cast<float>(values[axis]);
                    if (static_cast<double>(value)!=values[axis]) return 0;
                    const float up=std::nextafter(value,std::numeric_limits<float>::infinity());
                    const float down=std::nextafter(value,-std::numeric_limits<float>::infinity());
                    if (!std::isfinite(up) || !std::isfinite(down)) return 0;
                    const double ulp=(std::max)(double(up)-value,double(value)-down);
                    *maximum[axis]=(std::max)(*maximum[axis],ulp);
                }
            }
            const double u=std::fabs(frame.u.x)*spacing.x+std::fabs(frame.u.y)*spacing.y
                +std::fabs(frame.u.z)*spacing.z;
            const double v=std::fabs(frame.v.x)*spacing.x+std::fabs(frame.v.y)*spacing.y
                +std::fabs(frame.v.z)*spacing.z;
            return std::hypot(u,v);
        }

        bool EdgeWithinBoundaryTolerance(const Point& a,const Point& b,
                                         const Polygon& hull,double tolerance)
        {
            // Inside a convex polygon, distance to its boundary is the
            // minimum inward distance to its supporting lines. Find the
            // segment interval strictly deeper than tolerance from ALL lines.
            // An empty interval proves the whole edge remains in the boundary
            // tube, including across joins of near-collinear hull segments.
            double first=0,last=1;
            for (std::size_t edge=0;edge<hull.size();++edge)
            {
                const Point p=hull[edge],q=hull[(edge+1)%hull.size()];
                const double length=std::hypot(q.x-p.x,q.y-p.y);
                const double da=Side(a,p,q)/length,db=Side(b,p,q)/length;
                if (da<=tolerance && db<=tolerance) return true;
                if (da>tolerance && db>tolerance) continue;
                const double crossing=(tolerance-da)/(db-da);
                if (da<=tolerance) first=(std::max)(first,crossing);
                else last=(std::min)(last,crossing);
                if (first>=last) return true;
            }
            return false;
        }

        bool RepairFloatBoundary(Polygon& polygon,double tolerance,Work& work)
        {
            if (!std::isfinite(tolerance) || tolerance<=0 || Area(polygon)<=0) return false;
            Polygon points=polygon;
            std::sort(points.begin(),points.end(),[](const Point& a,const Point& b)
            {
                return a.x<b.x || (a.x==b.x && a.y<b.y);
            });
            points.erase(std::unique(points.begin(),points.end(),[](const Point& a,const Point& b)
            {
                return a.x==b.x && a.y==b.y;
            }),points.end());
            if (points.size()<3) return false;
            Polygon hull;
            for (const Point point : points)
            {
                while (hull.size()>=2 && Side(point,hull[hull.size()-2],hull.back())<=0)
                    hull.pop_back();
                hull.push_back(point);
            }
            const std::size_t lowerSize=hull.size();
            for (std::size_t index=points.size()-1;index-- > 0;)
            {
                const Point point=points[index];
                while (hull.size()>lowerSize && Side(point,hull[hull.size()-2],hull.back())<=0)
                    hull.pop_back();
                hull.push_back(point);
            }
            hull.pop_back();
            if (hull.size()<3) return false;

            double perimeter=0;
            for (std::size_t index=0;index<hull.size();++index)
            {
                const Point a=hull[index],b=hull[(index+1)%hull.size()];
                perimeter+=std::hypot(b.x-a.x,b.y-a.y);
            }
            if (std::fabs(Area(polygon)-Area(hull))>tolerance*perimeter) return false;
            // Every original edge must stay in the hull boundary's precision
            // tube. This admits backtracking across near-collinear joins from
            // cooked BSP welding, but never bridges a real notch or crossing.
            for (std::size_t index=0;index<polygon.size();++index)
            {
                if (!work.Spend(hull.size())) return false;
                const Point a=polygon[index],b=polygon[(index+1)%polygon.size()];
                if (!EdgeWithinBoundaryTolerance(a,b,hull,tolerance)) return false;
            }
            polygon=std::move(hull);
            return true;
        }

        bool RemoveBoundarySpikes(Polygon& polygon,Work& work,double tolerance)
        {
            // Cooked BSP can contain an out-and-back edge with no area.
            // The caller supplies the float bound, or native coincident-point
            // precision when explicitly preparing for the editor.
            for (std::size_t index=0;polygon.size()>=3 && index<polygon.size();)
            {
                if (!work.Spend(1)) return false;
                const Point a=polygon[(index+polygon.size()-1)%polygon.size()];
                const Point b=polygon[index],c=polygon[(index+1)%polygon.size()];
                if (std::fabs(Side(b,a,c))<=tolerance*std::hypot(c.x-a.x,c.y-a.y)
                    && (a.x-b.x)*(c.x-b.x)+(a.y-b.y)*(c.y-b.y)>0)
                {
                    polygon.erase(polygon.begin()+index);
                    index=0;
                }
                else ++index;
            }
            return true;
        }

        bool OnSegment(const Point& point,const Point& a,const Point& b)
        {
            return Side(point,a,b)==0 && point.x>=(std::min)(a.x,b.x)
                && point.x<=(std::max)(a.x,b.x) && point.y>=(std::min)(a.y,b.y)
                && point.y<=(std::max)(a.y,b.y);
        }

        bool SegmentsIntersect(const Point& a,const Point& b,const Point& c,const Point& d)
        {
            const double ac=Side(c,a,b),ad=Side(d,a,b),ca=Side(a,c,d),cb=Side(b,c,d);
            if (((ac<0 && ad>0)||(ac>0 && ad<0)) && ((ca<0 && cb>0)||(ca>0 && cb<0)))
                return true;
            return OnSegment(c,a,b)||OnSegment(d,a,b)||OnSegment(a,c,d)||OnSegment(b,c,d);
        }

        bool TriangulateSimple(const Polygon& polygon,std::vector<Polygon>& triangles,Work& work)
        {
            const double originalArea=Area(polygon);
            if (polygon.size()<3 || !std::isfinite(originalArea) || originalArea<=0)
            {
                work.error="A material polygon has zero area or reversed winding.";
                return false;
            }
            for (std::size_t first=0;first<polygon.size();++first)
            {
                if (!work.Spend(polygon.size())) return false;
                const std::size_t firstNext=(first+1)%polygon.size();
                for (std::size_t second=first+1;second<polygon.size();++second)
                {
                    const std::size_t secondNext=(second+1)%polygon.size();
                    if (firstNext==second || secondNext==first) continue;
                    if (SegmentsIntersect(polygon[first],polygon[firstNext],polygon[second],polygon[secondNext]))
                    {
                        work.error="A material polygon has self-intersecting boundary edges.";
                        return false;
                    }
                }
            }
            std::vector<std::size_t> indices;
            for (std::size_t index=0;index<polygon.size();++index) indices.push_back(index);
            double totalArea=0;
            while (indices.size()>3)
            {
                bool found=false;
                for (std::size_t index=0;index<indices.size();++index)
                {
                    if (!work.Spend(indices.size())) return false;
                    const std::size_t previous=indices[(index+indices.size()-1)%indices.size()];
                    const std::size_t current=indices[index];
                    const std::size_t next=indices[(index+1)%indices.size()];
                    const Point a=polygon[previous],b=polygon[current],c=polygon[next];
                    if (Side(c,a,b)<=0) continue;
                    bool blocked=false;
                    for (const std::size_t other:indices)
                    {
                        if (other==previous || other==current || other==next) continue;
                        const Point point=polygon[other];
                        if (Side(point,a,b)>=0 && Side(point,b,c)>=0 && Side(point,c,a)>=0)
                        {
                            blocked=true;
                            break;
                        }
                    }
                    if (blocked) continue;
                    if (triangles.size()>=work.limits.maxPieces)
                    {
                        work.error="Material polygon triangulation exceeded its polygon budget.";
                        return false;
                    }
                    Polygon triangle{a,b,c};
                    totalArea+=Area(triangle);
                    triangles.push_back(std::move(triangle));
                    indices.erase(indices.begin()+static_cast<std::ptrdiff_t>(index));
                    found=true;
                    break;
                }
                if (!found)
                {
                    work.error="A concave material polygon could not be triangulated without degeneracy.";
                    return false;
                }
            }
            Polygon last{polygon[indices[0]],polygon[indices[1]],polygon[indices[2]]};
            if (Area(last)<0)
            {
                work.error="Material triangulation produced a reversed final triangle.";
                return false;
            }
            if (Area(last)>0)
            {
                if (triangles.size()>=work.limits.maxPieces)
                {
                    work.error="Material polygon triangulation exceeded its polygon budget.";
                    return false;
                }
                totalArea+=Area(last);
                triangles.push_back(std::move(last));
            }
            if (!std::isfinite(totalArea)
                || std::fabs(totalArea-originalArea)>(std::max)(work.limits.epsilon*work.limits.epsilon,originalArea*1e-10))
            {
                work.error="Material triangulation could not preserve the polygon area.";
                return false;
            }
            return true;
        }

        bool MakeFrame(const std::vector<Vec3>& vertices, Vec3 normal,
                       Frame& frame, Polygon& polygon, Work& work)
        {
            if (vertices.size() < 3 || vertices.size() > work.limits.maxVertices
                || !Normalize(normal) || !Finite(vertices.front()))
            {
                work.error = "The source face has an invalid normal or vertex count.";
                return false;
            }
            frame.origin = vertices.front();
            frame.normal = normal;
            const Vec3 reference = std::fabs(normal.x) < 0.8 ? Vec3{1,0,0} : Vec3{0,1,0};
            frame.u = Cross(reference,normal);
            if (!Normalize(frame.u))
            {
                work.error = "The source face has no stable projection basis.";
                return false;
            }
            frame.v = Cross(normal,frame.u);
            for (const Vec3& point : vertices)
            {
                if (!Finite(point) || std::fabs(Dot(normal,Sub(point,frame.origin))) > work.limits.epsilon*8)
                {
                    work.error = "The source face is non-finite or non-planar.";
                    return false;
                }
                const Point projected = frame.Project(point);
                if (!std::isfinite(projected.x) || !std::isfinite(projected.y))
                {
                    work.error = "The source face could not be projected safely.";
                    return false;
                }
                Append(polygon,projected);
            }
            Clean(polygon);
            return ValidatePolygon(polygon,work);
        }

        // A single complementary split, using the SAME exact zero decision
        // and intersection for both sides. Epsilon is never applied twice.
        bool SplitExact(const Polygon& polygon, const Point& a, const Point& b,
                   Polygon& inside, Polygon& outside, Work& work)
        {
            if (!work.Spend(polygon.size()))
                return false;
            for (std::size_t index = 0; index < polygon.size(); ++index)
            {
                const Point p = polygon[index], q = polygon[(index+1)%polygon.size()];
                const double dp = Side(p,a,b), dq = Side(q,a,b);
                if (!std::isfinite(dp) || !std::isfinite(dq))
                {
                    work.error = "Surface clipping produced a non-finite distance.";
                    return false;
                }
                if (dp >= 0) Append(inside,p);
                if (dp <= 0) Append(outside,p);
                if ((dp < 0 && dq > 0) || (dp > 0 && dq < 0))
                {
                    const double t = dp/(dp-dq);
                    const Point intersection{p.x+(q.x-p.x)*t,p.y+(q.y-p.y)*t};
                    if (!std::isfinite(intersection.x) || !std::isfinite(intersection.y))
                    {
                        work.error = "Surface clipping produced a non-finite intersection.";
                        return false;
                    }
                    Append(inside,intersection);
                    Append(outside,intersection);
                }
            }
            Clean(inside,work.limits.epsilon);
            Clean(outside,work.limits.epsilon);
            if ((!inside.empty() && Area(inside) < 0) || (!outside.empty() && Area(outside) < 0))
            {
                work.error = "Surface clipping produced an unstable sliver with reversed area.";
                return false;
            }
            return true;
        }

        bool NativeStable(const Polygon& polygon,const Frame& frame,const Work& work,bool requireSplitWidth=true)
        {
            if (polygon.empty()) return true;
            // FPoly::SplitWithPlane uses a 0.25-unit distance band during
            // ordinary BSP construction (0x110BFFF7 in the supported editor).
            // A narrow paint fragment can import successfully, then become
            // coplanar with a crossing splitter and disappear in the rebuild.
            // Keep material divisions wider than both sides of that band.
            // This only guides complementary material cuts, never the shape
            // or width of an original structural face.
            constexpr double minimumSplitWidth=0.5;
            for (std::size_t i=0;requireSplitWidth && i<polygon.size();++i)
            {
                const Point a=polygon[i],b=polygon[(i+1)%polygon.size()];
                const double length=std::hypot(b.x-a.x,b.y-a.y);
                if (!length) return false;
                double low=0,high=0;
                for (const Point& point:polygon)
                {
                    const double distance=Side(point,a,b)/length;
                    low=(std::min)(low,distance);
                    high=(std::max)(high,distance);
                }
                if (high-low<minimumSplitWidth) return false;
            }
            // Larger polygons are divided during the final encoding pass.
            if (polygon.size()>RecoveredPolygonImport::kMaximumVertices) return true;
            std::vector<Vec3> vertices;
            vertices.reserve(polygon.size());
            for (const Point point:polygon)
            {
                const Vec3 restored=frame.Restore(point);
                vertices.push_back({static_cast<float>(restored.x),static_cast<float>(restored.y),
                    static_cast<float>(restored.z)});
            }
            const auto imported=RecoveredPolygonImport::Prepare(vertices);
            return imported.accepted() && RecoveredPolygonImport::PreservesOutline(vertices,imported)
                && Dot(imported.normal,frame.normal)>=1-work.limits.normalTolerance;
        }

        bool Split(const Polygon& polygon,const Point& a,const Point& b,
                   Polygon& inside,Polygon& outside,Work& work,const Frame& frame)
        {
            if (!SplitExact(polygon,a,b,inside,outside,work)) return false;
            if (!work.limits.matchEditorPrecision) return true;
            if (inside.empty() || outside.empty())
            {
                if (inside.empty()) outside=polygon;
                else inside=polygon;
                return true;
            }
            if (NativeStable(inside,frame,work) && NativeStable(outside,frame,work)) return true;

            // A cut close to an existing corner can be lost during either
            // import cleanup or BSP splitting. Move its material endpoint to
            // that corner, keeping the complete structural face unchanged.
            std::vector<Point> intersections;
            bool adjusted=false;
            const double tolerance=0.25; // Native BSP split distance band.
            auto near=[&](const Point& first,const Point& second)
            {
                const Vec3 start=frame.Restore(first),end=frame.Restore(second);
                const Vec3 delta{double(static_cast<float>(start.x))-static_cast<float>(end.x),
                    double(static_cast<float>(start.y))-static_cast<float>(end.y),
                    double(static_cast<float>(start.z))-static_cast<float>(end.z)};
                return std::fabs(delta.x)<tolerance && std::fabs(delta.y)<tolerance
                    && std::fabs(delta.z)<tolerance;
            };
            for (std::size_t i=0;i<polygon.size();++i)
            {
                if (!work.Spend(1)) return false;
                const Point p=polygon[i],q=polygon[(i+1)%polygon.size()];
                const double dp=Side(p,a,b),dq=Side(q,a,b);
                if (dp==0) intersections.push_back(p);
                if ((dp<0 && dq>0) || (dp>0 && dq<0))
                {
                    const double t=dp/(dp-dq);
                    Point cut{p.x+(q.x-p.x)*t,p.y+(q.y-p.y)*t};
                    if (near(cut,p)) { cut=p; adjusted=true; }
                    else if (near(cut,q)) { cut=q; adjusted=true; }
                    intersections.push_back(cut);
                }
            }
            if (adjusted && intersections.size()>=2)
            {
                const auto along=[&](const Point& point)
                {
                    return (point.x-a.x)*(b.x-a.x)+(point.y-a.y)*(b.y-a.y);
                };
                const auto ends=std::minmax_element(intersections.begin(),intersections.end(),
                    [&](const Point& first,const Point& second) { return along(first)<along(second); });
                if (ends.first->x!=ends.second->x || ends.first->y!=ends.second->y)
                {
                    inside.clear(); outside.clear();
                    if (!SplitExact(polygon,*ends.first,*ends.second,inside,outside,work)) return false;
                }
            }
            if (inside.empty() || outside.empty())
            {
                if (inside.empty()) outside=polygon;
                else inside=polygon;
                return true;
            }
            if (NativeStable(inside,frame,work) && NativeStable(outside,frame,work)) return true;

            // Preserve a representable parent instead of producing a paint
            // fragment below native import/build precision. This changes
            // material assignment only; the two sides still cover the parent.
            // The area bound is tied to native split and normal limits,
            // so a large region with one short edge cannot be reassigned.
            auto microscopic=[&](const Polygon& piece)
            {
                double perimeter=0;
                for (std::size_t i=0;i<piece.size();++i)
                    perimeter+=std::hypot(piece[i].x-piece[(i+1)%piece.size()].x,
                                          piece[i].y-piece[(i+1)%piece.size()].y);
                const double minimumArea=std::sqrt(
                    static_cast<double>(RecoveredPolygonImport::kMinimumNormalSquared))*0.5;
                return Area(piece)<=(std::max)(minimumArea,perimeter*tolerance*std::sqrt(3.0));
            };
            // An original structural face may itself be narrower than the
            // split band. Keep it intact if it imports correctly, rather
            // than subdividing it into fragments the importer discards.
            if (NativeStable(polygon,frame,work,false))
            {
                if (Area(inside)<=Area(outside) && microscopic(inside))
                {
                    inside.clear(); outside=polygon;
                }
                else if (microscopic(outside))
                {
                    outside.clear(); inside=polygon;
                }
            }
            return true;
        }

        bool AppendPiece(const Polygon& polygon, int material, const Frame& frame,
                         std::vector<Piece>& output, Work& work)
        {
            if (output.size() >= work.limits.maxPieces || polygon.size() > work.limits.maxVertices)
            {
                work.error = "Surface reconstruction exceeded its polygon or vertex budget.";
                return false;
            }
            Piece piece;
            piece.materialIndex = material;
            for (const Point point : polygon)
            {
                const Vec3 restored = frame.Restore(point);
                if (!Finite(restored))
                {
                    work.error = "A reconstructed surface vertex is non-finite.";
                    return false;
                }
                piece.vertices.push_back(restored);
            }
            output.push_back(std::move(piece));
            return true;
        }

        struct PaintedPolygon { Polygon vertices; int material; };
        using VertexMap=std::map<std::pair<double,double>,Vec3>;

        bool SamePoint(const Point& a,const Point& b)
        {
            return a.x==b.x && a.y==b.y;
        }

        void RemoveExactRedundancy(Polygon& polygon,const VertexMap* sourceVertices)
        {
            bool changed=true;
            while (changed && polygon.size()>=3)
            {
                changed=false;
                for (std::size_t i=0;i<polygon.size();++i)
                {
                    const Point a=polygon[(i+polygon.size()-1)%polygon.size()];
                    const Point b=polygon[i],c=polygon[(i+1)%polygon.size()];
                    bool collinear3D=true;
                    if (sourceVertices)
                    {
                        const Vec3& pa=sourceVertices->at({a.x,a.y});
                        const Vec3& pb=sourceVertices->at({b.x,b.y});
                        const Vec3& pc=sourceVertices->at({c.x,c.y});
                        const Vec3 cross=Cross(Sub(pb,pa),Sub(pc,pa));
                        collinear3D=cross.x==0 && cross.y==0 && cross.z==0;
                    }
                    // The a,b,a case cancels another segment of the shared
                    // boundary after joining two polygons along one edge.
                    if (SamePoint(a,b) || SamePoint(a,c)
                        || (collinear3D && Side(b,a,c)==0
                            && (b.x-a.x)*(c.x-b.x)+(b.y-a.y)*(c.y-b.y)>=0))
                    {
                        polygon.erase(polygon.begin()+i);
                        changed=true;
                        break;
                    }
                }
            }
        }

        bool SubdivideSharedEdges(const Polygon& input,const Polygon& other,
                                  Polygon& output,Work& work)
        {
            for (std::size_t edge=0;edge<input.size();++edge)
            {
                if (!work.Spend(other.size()+1)) return false;
                const Point a=input[edge],b=input[(edge+1)%input.size()];
                Append(output,a);
                const bool xAxis=std::fabs(b.x-a.x)>=std::fabs(b.y-a.y);
                const double length=xAxis ? b.x-a.x : b.y-a.y;
                if (length==0) continue;
                std::vector<std::pair<double,Point>> points;
                for (const Point point:other)
                {
                    if (Side(point,a,b)!=0) continue;
                    const double t=(xAxis ? point.x-a.x : point.y-a.y)/length;
                    if (t>0 && t<1) points.emplace_back(t,point);
                }
                std::sort(points.begin(),points.end(),[](const auto& first,const auto& second)
                {
                    return first.first<second.first;
                });
                for (const auto& point:points) Append(output,point.second);
            }
            return true;
        }

        bool StrictlyConvex(const Polygon& polygon,Work& work,bool& convex)
        {
            convex=false;
            if (polygon.size()<3 || Area(polygon)<=0) return true;
            for (std::size_t edge=0;edge<polygon.size();++edge)
            {
                if (!work.Spend(polygon.size())) return false;
                const Point a=polygon[edge],b=polygon[(edge+1)%polygon.size()];
                if (SamePoint(a,b)) return true;
                for (const Point point:polygon)
                    if (Side(point,a,b)<0) return true;
            }
            convex=true;
            return true;
        }

        bool JoinConvex(const Polygon& first,const Polygon& second,
                        Polygon& joined,Work& work,const VertexMap* sourceVertices)
        {
            if (sourceVertices)
            {
                if (!work.Spend(first.size()+second.size())) return false;
                const Vec3 origin=sourceVertices->at({first.front().x,first.front().y});
                Vec3 geometricNormal{};
                for (std::size_t i=1;i+1<first.size();++i)
                {
                    geometricNormal=Cross(Sub(sourceVertices->at({first[i].x,first[i].y}),origin),
                        Sub(sourceVertices->at({first[i+1].x,first[i+1].y}),origin));
                    if (geometricNormal.x!=0 || geometricNormal.y!=0 || geometricNormal.z!=0) break;
                }
                if (!Finite(geometricNormal)
                    || (geometricNormal.x==0 && geometricNormal.y==0 && geometricNormal.z==0)) return true;
                for (const Polygon* polygon:{&first,&second}) for (const Point point:*polygon)
                    if (Dot(geometricNormal,Sub(sourceVertices->at({point.x,point.y}),origin))!=0)
                        return true;
            }
            Polygon a,b;
            if (!SubdivideSharedEdges(first,second,a,work)
                || !SubdivideSharedEdges(second,first,b,work)) return false;
            for (std::size_t i=0;i<a.size();++i)
            {
                if (!work.Spend(b.size())) return false;
                for (std::size_t j=0;j<b.size();++j)
                {
                    if (!SamePoint(a[i],b[(j+1)%b.size()])
                        || !SamePoint(a[(i+1)%a.size()],b[j])) continue;
                    Polygon boundary;
                    for (std::size_t k=1;k<=a.size();++k)
                        Append(boundary,a[(i+k)%a.size()]);
                    for (std::size_t k=2;k<b.size();++k)
                        Append(boundary,b[(j+k)%b.size()]);
                    RemoveExactRedundancy(boundary,sourceVertices);
                    if (boundary.size()>work.limits.maxVertices) continue;
                    bool convex=false;
                    if (!StrictlyConvex(boundary,work,convex)) return false;
                    if (!convex) continue;
                    // This is boundary splicing, not a convex-hull fill.
                    // Only shared/retraced edges and exactly collinear
                    // vertices were removed. Check the area identity as an
                    // additional guard, allowing arithmetic error only.
                    const double before=Area(first)+Area(second),after=Area(boundary);
                    const double roundoff=std::numeric_limits<double>::epsilon()*64
                        *static_cast<double>(first.size()+second.size())*std::fabs(before);
                    if (!std::isfinite(after) || std::fabs(after-before)>roundoff) continue;
                    joined=std::move(boundary);
                    return true;
                }
            }
            return true;
        }

        bool Coalesce(std::vector<PaintedPolygon>& polygons,Work& work,
                      const VertexMap* sourceVertices=nullptr,const Frame* nativeFrame=nullptr)
        {
            // Small subdivisions are most vulnerable to the editor's
            // vertex/normal cleanup. Absorb those first when an exact convex
            // union exists; no rejected fragment is discarded here.
            auto bounds=[](const Polygon& polygon)
            {
                std::array<double,4> box{polygon[0].x,polygon[0].y,polygon[0].x,polygon[0].y};
                for (const Point point:polygon)
                {
                    box[0]=(std::min)(box[0],point.x); box[1]=(std::min)(box[1],point.y);
                    box[2]=(std::max)(box[2],point.x); box[3]=(std::max)(box[3],point.y);
                }
                return box;
            };
            std::vector<std::array<double,4>> boxes;
            for (const PaintedPolygon& polygon:polygons) boxes.push_back(bounds(polygon.vertices));
            bool changed=true;
            while (changed)
            {
                changed=false;
                std::vector<std::size_t> order;
                for (std::size_t i=0;i<polygons.size();++i)
                    if (!polygons[i].vertices.empty()) order.push_back(i);
                std::stable_sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b)
                {
                    return Area(polygons[a].vertices)<Area(polygons[b].vertices);
                });
                for (std::size_t i:order)
                {
                    if (polygons[i].vertices.empty()) continue;
                    for (std::size_t j:order)
                    {
                        if (!work.Spend(1)) return false;
                        if (i==j || polygons[j].vertices.empty()
                            || polygons[i].material!=polygons[j].material) continue;
                        const auto& a=boxes[i]; const auto& b=boxes[j];
                        if (a[2]<b[0] || b[2]<a[0] || a[3]<b[1] || b[3]<a[1]) continue;
                        Polygon joined;
                        if (!JoinConvex(polygons[i].vertices,polygons[j].vertices,joined,work,sourceVertices)) return false;
                        if (joined.empty()) continue;
                        if (nativeFrame && (joined.size()>RecoveredPolygonImport::kMaximumVertices
                            || !NativeStable(joined,*nativeFrame,work))) continue;
                        polygons[j].vertices=std::move(joined);
                        boxes[j]=bounds(polygons[j].vertices);
                        polygons[i].vertices.clear();
                        changed=true;
                        break;
                    }
                }
            }
            polygons.erase(std::remove_if(polygons.begin(),polygons.end(),[](const PaintedPolygon& p)
            {
                return p.vertices.empty();
            }),polygons.end());
            return true;
        }
    }

    bool Partition(const RecoveredBspGeometry::Face& face,
                   const std::vector<Surface>& candidates, int fallbackMaterialIndex,
                   std::vector<Piece>& result, std::string& error, const Limits& limits)
    try
    {
        result.clear();
        error.clear();
        if (!ValidLimits(limits,error)) return false;
        Work work{limits,0,error};
        Frame frame;
        Polygon original;
        if (!MakeFrame(face.vertices,face.normal,frame,original,work))
        {
            error = "Target brush face: "+error;
            return false;
        }
        const double originalArea = Area(original);
        std::vector<Polygon> remaining{original};
        std::vector<PaintedPolygon> output;
        double totalArea = 0;
        for (const Surface& candidate : candidates)
        {
            if (!work.Spend(1)) return false;
            if (remaining.empty()) break;
            Vec3 candidateNormal = candidate.normal;
            if (candidate.vertices.size() < 3 || candidate.vertices.size() > limits.maxVertices
                || !Normalize(candidateNormal))
            {
                error = "A material source polygon has an invalid normal or vertex count.";
                return false;
            }
            if (std::fabs(Dot(candidateNormal,frame.normal))
                    < (limits.candidatesPreselected ? 0.5 : 1-limits.normalTolerance))
                continue;
            bool coplanar = true;
            Polygon boundary;
            for (const Vec3& vertex : candidate.vertices)
            {
                if (!Finite(vertex))
                {
                    error = "A material source polygon contains a non-finite vertex.";
                    return false;
                }
                if (!limits.candidatesPreselected
                    && std::fabs(Dot(frame.normal,Sub(vertex,frame.origin))) > limits.planeTolerance)
                    coplanar = false;
                Append(boundary,frame.Project(vertex));
            }
            if (!coplanar) continue;
            Clean(boundary);
            if (DisjointBounds(boundary,original)) continue;
            // Bounding boxes overlap across diagonal faces even when the
            // material is entirely outside. A separating face edge proves
            // no positive-area overlap, including for malformed boundaries.
            bool separated=false;
            for (std::size_t edge=0;edge<original.size() && !separated;++edge)
            {
                if (!work.Spend(boundary.size())) return false;
                const Point a=original[edge],b=original[(edge+1)%original.size()];
                separated=std::all_of(boundary.begin(),boundary.end(),[&](const Point& p)
                { return Side(p,a,b)<=0; });
            }
            if (separated) continue;
            if (Dot(candidateNormal,frame.normal) < 0)
                std::reverse(boundary.begin(),boundary.end());
            std::vector<Polygon> masks;
            if (ValidatePolygon(boundary,work))
                masks.push_back(std::move(boundary));
            else if (RepairFloatBoundary(boundary,FloatSourceTolerance(candidate,frame),work)
                     && ValidatePolygon(boundary,work))
            {
                masks.push_back(std::move(boundary));
                error.clear();
            }
            else if (RemoveBoundarySpikes(boundary,work,(std::max)(FloatSourceTolerance(candidate,frame),
                         limits.matchEditorPrecision ? double(RecoveredPolygonImport::kCoincidentVertexTolerance) : 0.0))
                     && TriangulateSimple(boundary,masks,work))
                error.clear();
            else
            {
                error = "Material candidate "+std::to_string(&candidate-candidates.data())
                    +" (materialIndex="+std::to_string(candidate.materialIndex)+"): "+error;
                return false;
            }
            for (const Polygon& mask:masks)
            {
                if (remaining.empty()) break;
                if (DisjointBounds(mask,original)) continue;
                std::vector<Polygon> nextRemaining;
                for (const Polygon& fragment : remaining)
                {
                    Polygon piece = fragment;
                    std::vector<Polygon> outside;
                    for (std::size_t edge = 0; edge < mask.size() && !piece.empty(); ++edge)
                    {
                        Polygon inside, cutOff;
                        if (!Split(piece,mask[edge],mask[(edge+1)%mask.size()],inside,cutOff,work,frame))
                        {
                            error="Material candidate "+std::to_string(&candidate-candidates.data())
                                +" (materialIndex="+std::to_string(candidate.materialIndex)+"): "+error;
                            return false;
                        }
                        if (!cutOff.empty()) outside.push_back(std::move(cutOff));
                        piece = std::move(inside);
                    }
                    if (piece.empty())
                        nextRemaining.push_back(fragment); // No overlap: avoid pointless fragmentation.
                    else
                    {
                        totalArea += Area(piece);
                        output.push_back({std::move(piece),candidate.materialIndex});
                        for (Polygon& uncovered : outside)
                            nextRemaining.push_back(std::move(uncovered));
                    }
                    if (nextRemaining.size() > limits.maxPieces
                        || output.size() > limits.maxPieces-nextRemaining.size())
                    {
                        error = "Surface reconstruction exceeded its polygon budget.";
                        return false;
                    }
                }
                remaining = std::move(nextRemaining);
            }
        }
        for (const Polygon& fragment : remaining)
        {
            totalArea += Area(fragment);
            output.push_back({fragment,fallbackMaterialIndex});
        }
        if (!std::isfinite(totalArea)
            || std::fabs(totalArea-originalArea) > (std::max)(limits.epsilon*limits.epsilon,originalArea*1e-10))
        {
            error = "Material partitioning could not preserve the source face area.";
            return false;
        }
        if (output.empty() || output.size()>limits.maxPieces)
        {
            error="Surface reconstruction exceeded its polygon budget.";
            return false;
        }
        if (std::all_of(output.begin(),output.end(),[&](const PaintedPolygon& piece)
            { return piece.material==output.front().material; }))
        {
            // The completed partition has proved full coverage. Boundaries
            // between identical material/UV assignments have no meaning;
            // retaining the original face avoids native cleanup of slivers.
            result.push_back({face.vertices,output.front().material});
            return true;
        }
        if (!Coalesce(output,work,nullptr,limits.matchEditorPrecision ? &frame : nullptr)) return false;
        for (const PaintedPolygon& piece:output)
            if (!AppendPiece(piece.vertices,piece.material,frame,result,work))
            {
                result.clear();
                return false;
            }
        return true;
    }
    catch (const std::bad_alloc&)
    {
        result.clear(); error = "There is insufficient memory to partition the recovered surfaces."; return false;
    }
    catch (const std::length_error&)
    {
        result.clear(); error = "The recovered surface partition exceeds container capacity."; return false;
    }

    bool CoalesceCoplanarPieces(const std::vector<Piece>& pieces,const Vec3& normal,
                               std::vector<Piece>& result,std::string& error,const Limits& limits)
    try
    {
        result.clear(); error.clear();
        if (!ValidLimits(limits,error)) return false;
        if (pieces.size()>limits.maxPieces)
        {
            error="Surface coalescing exceeded its polygon budget.";
            return false;
        }
        if (pieces.empty()) return true;
        Work work{limits,0,error};
        Frame frame;
        Polygon first;
        if (!MakeFrame(pieces.front().vertices,normal,frame,first,work)) return false;
        VertexMap sourceVertices;
        std::vector<PaintedPolygon> polygons;
        for (const Piece& piece:pieces)
        {
            Frame checkFrame;
            Polygon checked;
            if (!MakeFrame(piece.vertices,normal,checkFrame,checked,work)) return false;
            PaintedPolygon projected{{},piece.materialIndex};
            for (const Vec3& vertex:piece.vertices)
            {
                if (std::fabs(Dot(frame.normal,Sub(vertex,frame.origin)))>limits.epsilon*8)
                {
                    error="Surface coalescing requires coplanar pieces.";
                    return false;
                }
                const Point point=frame.Project(vertex);
                const auto inserted=sourceVertices.emplace(std::make_pair(point.x,point.y),vertex);
                const Vec3& existing=inserted.first->second;
                if (!inserted.second && (existing.x!=vertex.x || existing.y!=vertex.y || existing.z!=vertex.z))
                {
                    error="Coplanar surface projection maps distinct 3D vertices to the same point.";
                    return false;
                }
                Append(projected.vertices,point);
            }
            Clean(projected.vertices);
            polygons.push_back(std::move(projected));
        }
        if (!Coalesce(polygons,work,&sourceVertices)) return false;
        std::vector<Piece> output;
        for (const PaintedPolygon& polygon:polygons)
        {
            Piece piece{{},polygon.material};
            for (const Point point:polygon.vertices)
            {
                const auto found=sourceVertices.find({point.x,point.y});
                if (found==sourceVertices.end())
                {
                    error="Surface coalescing introduced an unexpected boundary vertex.";
                    return false;
                }
                piece.vertices.push_back(found->second);
            }
            output.push_back(std::move(piece));
        }
        result=std::move(output);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        result.clear(); error="There is insufficient memory to coalesce recovered surfaces."; return false;
    }
    catch (const std::length_error&)
    {
        result.clear(); error="Surface coalescing exceeds container capacity."; return false;
    }

    bool PrepareForEditor(const RecoveredBspGeometry::Face& face,
                          const std::vector<Piece>& pieces,std::size_t maxVertices,
                          std::vector<Piece>& result,std::string& error,const Limits& limits)
    try
    {
        result.clear(); error.clear();
        if (!ValidLimits(limits,error)) return false;
        if (maxVertices<3 || pieces.size()>limits.maxPieces)
        {
            error="Editor surface preparation requires valid polygon and vertex limits.";
            return false;
        }
        Work work{limits,0,error};
        Frame frame;
        Polygon original;
        if (!MakeFrame(face.vertices,face.normal,frame,original,work)) return false;
        auto encode=[&](const std::vector<Vec3>& vertices,std::vector<Vec3>& encodedVertices,
                        Polygon& encoded,double& areaBound,double& displacement)
        {
            if (vertices.size()>limits.maxVertices)
            {
                error="Editor surface preparation exceeded its vertex budget.";
                return false;
            }
            if (!work.Spend(vertices.size())) return false;
            displacement=0;
            double perimeter=0;
            for (std::size_t index=0;index<vertices.size();++index)
            {
                const Vec3& point=vertices[index];
                if (!Finite(point) || std::fabs(point.x)>(std::numeric_limits<float>::max)()
                    || std::fabs(point.y)>(std::numeric_limits<float>::max)()
                    || std::fabs(point.z)>(std::numeric_limits<float>::max)())
                {
                    error="A recovered surface vertex cannot be represented by editor float coordinates.";
                    return false;
                }
                const Vec3 rounded{static_cast<float>(point.x),static_cast<float>(point.y),
                    static_cast<float>(point.z)};
                const Point before=frame.Project(point),after=frame.Project(rounded);
                const Point next=frame.Project(vertices[(index+1)%vertices.size()]);
                displacement=(std::max)(displacement,std::hypot(after.x-before.x,after.y-before.y));
                perimeter+=std::hypot(next.x-before.x,next.y-before.y);
                if (encoded.empty() || encoded.back().x!=after.x || encoded.back().y!=after.y)
                {
                    encoded.push_back(after);
                    encodedVertices.push_back(rounded);
                }
            }
            if (encoded.size()>1 && encoded.front().x==encoded.back().x && encoded.front().y==encoded.back().y)
            {
                encoded.pop_back(); encodedVertices.pop_back();
            }
            // Moving a polygon's vertices by at most d changes its signed
            // area by at most d*perimeter + vertexCount*d*d/2. This includes
            // internal subdivisions, so the conservative bound never assumes
            // that independently rounded boundary points cancel perfectly.
            areaBound=displacement*perimeter+vertices.size()*displacement*displacement*0.5;
            if (!std::isfinite(areaBound))
            {
                error="Editor surface rounding produced a non-finite area bound.";
                return false;
            }
            return true;
        };
        std::vector<Vec3> originalVertices;
        Polygon encodedOriginal;
        double roundingBound=0,originalDisplacement=0;
        if (!encode(face.vertices,originalVertices,encodedOriginal,roundingBound,originalDisplacement)) return false;
        const double originalArea=Area(encodedOriginal);
        if (encodedOriginal.size()<3 || !std::isfinite(originalArea) || originalArea<=0)
        {
            error="An original structural brush face collapses or reverses in editor float coordinates.";
            return false;
        }
        std::vector<Piece> output;
        double totalArea=0;
        for (const Piece& piece:pieces)
        {
            std::vector<Vec3> vertices;
            Polygon encoded;
            double pieceBound=0,displacement=0;
            if (!encode(piece.vertices,vertices,encoded,pieceBound,displacement)) return false;
            roundingBound+=pieceBound;
            const double area=Area(encoded);
            if (encoded.size()<3 || area==0) continue;
            auto withinRoundingWidth=[&]()
            {
                if (piece.vertices.size()<3) return true;
                const Point a=frame.Project(piece.vertices.front());
                Point b=a;
                double longest=0;
                for (const Vec3& point:piece.vertices)
                {
                    const Point p=frame.Project(point);
                    const double length=std::hypot(p.x-a.x,p.y-a.y);
                    if (length>longest) { longest=length; b=p; }
                }
                if (longest==0) return true;
                return std::all_of(piece.vertices.begin(),piece.vertices.end(),[&](const Vec3& point)
                {
                    return std::fabs(Side(frame.Project(point),a,b))<=displacement*longest;
                });
            };
            if (!std::isfinite(area) || area<0)
            {
                if (std::isfinite(area) && withinRoundingWidth()) continue;
                error="A material subdivision reverses winding in editor float coordinates.";
                return false;
            }
            std::vector<Polygon> parts;
            if (encoded.size()<=maxVertices && ValidatePolygon(encoded,work)) parts.push_back(encoded);
            else if (RepairFloatBoundary(encoded,displacement*2,work) && ValidatePolygon(encoded,work))
            {
                if (encoded.size()<=maxVertices) parts.push_back(encoded);
                else if (!TriangulateSimple(encoded,parts,work)) return false;
                error.clear();
            }
            else if (TriangulateSimple(encoded,parts,work)) error.clear();
            else if (withinRoundingWidth()) { error.clear(); continue; }
            else return false;
            for (const Polygon& part:parts)
            {
                if (output.size()>=limits.maxPieces)
                {
                    error="Editor surface preparation exceeded its polygon budget.";
                    return false;
                }
                Piece prepared;
                prepared.materialIndex=piece.materialIndex;
                for (const Point point:part)
                {
                    if (!work.Spend(vertices.size())) return false;
                    const auto found=std::find_if(vertices.begin(),vertices.end(),[&](const Vec3& vertex)
                    {
                        const Point input=frame.Project(vertex);
                        return point.x==input.x && point.y==input.y;
                    });
                    if (found==vertices.end())
                    {
                        error="Editor surface triangulation introduced an unexpected vertex.";
                        return false;
                    }
                    prepared.vertices.push_back(*found);
                }
                totalArea+=Area(part);
                output.push_back(std::move(prepared));
            }
        }
        if (output.empty() || !std::isfinite(totalArea)
            || std::fabs(totalArea-originalArea)>roundingBound+(std::max)(limits.epsilon*limits.epsilon,originalArea*1e-10))
        {
            error="Editor float conversion could not preserve coverage of the original structural face.";
            return false;
        }
        result=std::move(output);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        result.clear(); error="There is insufficient memory to prepare recovered surfaces for the editor."; return false;
    }
    catch (const std::length_error&)
    {
        result.clear(); error="Editor surface preparation exceeds container capacity."; return false;
    }

    bool CanonicalizeBrushForEditor(RecoveredBspGeometry::Brush& brush,
                                    std::string& error,const Limits& limits)
    try
    {
        error.clear();
        if (!ValidLimits(limits,error)) return false;
        if (brush.faces.empty())
        {
            error="A structural brush has no faces to prepare.";
            return false;
        }
        Work work{limits,0,error};
        RecoveredBspGeometry::Brush prepared;
        prepared.subtractive=brush.subtractive;
        std::vector<Vec3> anchors;
        const double tolerance=RecoveredPolygonImport::kCoincidentVertexTolerance;
        bool smallNormalBevel=false;
        for (const auto& face:brush.faces)
        {
            RecoveredBspGeometry::Face result{{},face.normal,face.surfaceIndex};
            for (const Vec3& original:face.vertices)
            {
                if (!Finite(original)
                    || std::fabs(original.x)>(std::numeric_limits<float>::max)()
                    || std::fabs(original.y)>(std::numeric_limits<float>::max)()
                    || std::fabs(original.z)>(std::numeric_limits<float>::max)())
                {
                    error="A structural brush vertex exceeds native float coordinates.";
                    return false;
                }
                if (!work.Spend(anchors.size()+1)) return false;
                const Vec3 encoded{static_cast<float>(original.x),static_cast<float>(original.y),
                    static_cast<float>(original.z)};
                auto found=std::find_if(anchors.begin(),anchors.end(),[&](const Vec3& anchor)
                {
                    return std::fabs(double(static_cast<float>(anchor.x))-encoded.x)<tolerance
                        && std::fabs(double(static_cast<float>(anchor.y))-encoded.y)<tolerance
                        && std::fabs(double(static_cast<float>(anchor.z))-encoded.z)<tolerance;
                });
                Vec3 point;
                if (found==anchors.end())
                {
                    if (anchors.size()>=limits.maxVertices)
                    {
                        error="Native brush preparation exceeded its vertex budget.";
                        return false;
                    }
                    anchors.push_back(original);
                    point=original;
                }
                else point=*found;
                if (result.vertices.empty() || point.x!=result.vertices.back().x
                    || point.y!=result.vertices.back().y || point.z!=result.vertices.back().z)
                    result.vertices.push_back(point);
            }
            if (result.vertices.size()>1 && result.vertices.front().x==result.vertices.back().x
                && result.vertices.front().y==result.vertices.back().y
                && result.vertices.front().z==result.vertices.back().z)
                result.vertices.pop_back();
            const auto imported=RecoveredPolygonImport::Prepare(result.vertices);
            if (imported.reason==RecoveredPolygonImport::Reason::NormalTooSmall)
                smallNormalBevel=true;
            else if (result.vertices.size()>=3) prepared.faces.push_back(face);
        }
        if (prepared.faces.size()==brush.faces.size()) return true;
        if (prepared.faces.empty() || anchors.empty())
        {
            error="A structural brush collapses at native editor precision.";
            return false;
        }
        // Removing a sub-precision bevel must not bend adjoining planes.
        // Recompute their intersections from the retained original planes,
        // then bound the expansion. Three distinct corners can still have
        // too little area for CalcNormal (HELI02). Such bevels use the native
        // BSP split distance band; coincident-corner repairs retain the tighter
        // point-pool bound. Neither case may create a distant intersection.
        const double expansionTolerance=smallNormalBevel ? 0.25 : tolerance;
        std::vector<RecoveredBspGeometry::Node> planes;
        for (std::size_t index=0;index<prepared.faces.size();++index)
        {
            const auto& face=prepared.faces[index];
            const int next=index+1==prepared.faces.size() ? -1 : static_cast<int>(index+1);
            planes.push_back({Scale(face.normal,-1),-Dot(face.normal,face.vertices.front()),
                next,-1,true,face.surfaceIndex});
        }
        RecoveredBspGeometry::Bounds bounds{anchors.front(),anchors.front()};
        for (const Vec3& point:anchors)
        {
            bounds.minimum.x=(std::min)(bounds.minimum.x,point.x);
            bounds.minimum.y=(std::min)(bounds.minimum.y,point.y);
            bounds.minimum.z=(std::min)(bounds.minimum.z,point.z);
            bounds.maximum.x=(std::max)(bounds.maximum.x,point.x);
            bounds.maximum.y=(std::max)(bounds.maximum.y,point.y);
            bounds.maximum.z=(std::max)(bounds.maximum.z,point.z);
        }
        const Vec3 margin{64,64,64};
        bounds.minimum=Sub(bounds.minimum,margin); bounds.maximum=Add(bounds.maximum,margin);
        RecoveredBspGeometry::Result rebuilt;
        if (!RecoveredBspGeometry::Reconstruct(planes,false,bounds,rebuilt,error)
            || rebuilt.brushes.size()!=1)
        {
            error="Native precision cannot retain a closed structural brush: "+error;
            return false;
        }
        for (const auto& face:rebuilt.brushes.front().faces)
            for (const Vec3& point:face.vertices)
            {
                const bool inside=std::all_of(brush.faces.begin(),brush.faces.end(),[&](const auto& oldFace)
                {
                    return Dot(oldFace.normal,Sub(point,oldFace.vertices.front()))<=limits.epsilon;
                });
                if (inside) continue;
                const bool near=std::any_of(anchors.begin(),anchors.end(),[&](const Vec3& old)
                {
                    return std::fabs(double(static_cast<float>(old.x))-static_cast<float>(point.x))<expansionTolerance
                        && std::fabs(double(static_cast<float>(old.y))-static_cast<float>(point.y))<expansionTolerance
                        && std::fabs(double(static_cast<float>(old.z))-static_cast<float>(point.z))<expansionTolerance;
                });
                if (!near)
                {
                    error="Removing an unimportable brush bevel would exceed native precision.";
                    return false;
                }
            }
        prepared=std::move(rebuilt.brushes.front());
        prepared.subtractive=brush.subtractive;
        std::vector<bool> collapsed;
        if (!ValidateEditorBrush(prepared,collapsed,error)) return false;
        std::vector<RecoveredBspGeometry::Face> faces;
        for (std::size_t index=0;index<prepared.faces.size();++index)
            if (!collapsed[index]) faces.push_back(std::move(prepared.faces[index]));
        prepared.faces=std::move(faces);
        brush=std::move(prepared);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        error="There is insufficient memory to prepare native brush vertices."; return false;
    }
    catch (const std::length_error&)
    {
        error="Native brush preparation exceeds container capacity."; return false;
    }

    namespace
    {
    bool ValidateEditorBrushWeld(const RecoveredBspGeometry::Brush& brush,
                                 std::vector<bool>& collapsedFaces,std::string& error,const Limits& limits,
                                 bool nativeWeld)
    try
    {
        collapsedFaces.clear(); error.clear();
        if (!ValidLimits(limits,error)) return false;
        Work work{limits,0,error};
        std::vector<Vec3> points;
        std::vector<std::vector<std::size_t>> faces;
        std::vector<bool> collapsed;
        for (const auto& face:brush.faces)
        {
            Frame frame;
            Polygon polygon;
            if (!MakeFrame(face.vertices,face.normal,frame,polygon,work)) return false;
            std::vector<std::size_t> indices;
            for (const Vec3& source:face.vertices)
            {
                if (std::fabs(source.x)>(std::numeric_limits<float>::max)()
                    || std::fabs(source.y)>(std::numeric_limits<float>::max)()
                    || std::fabs(source.z)>(std::numeric_limits<float>::max)())
                {
                    error="A structural brush vertex exceeds editor float coordinates.";
                    return false;
                }
                const Vec3 point{static_cast<float>(source.x),static_cast<float>(source.y),static_cast<float>(source.z)};
                if (!work.Spend(points.size()+1)) return false;
                const auto found=std::find_if(points.begin(),points.end(),[&](const Vec3& value)
                {
                    // The BSP reconstruction already welds shared vertices
                    // at this tolerance. Near coordinate zero, casting its
                    // double arithmetic noise to float does not remove it.
                    const Vec3 difference=Sub(point,value);
                    // The second pass welds as the T3D writer's point pool
                    // does (RecoveredPolygonImport::VertexPool): corners
                    // within the native coincident tolerance on every axis
                    // are written as one corner.
                    if (nativeWeld)
                    {
                        const double tolerance=RecoveredPolygonImport::kCoincidentVertexTolerance;
                        return std::fabs(difference.x)<tolerance && std::fabs(difference.y)<tolerance
                            && std::fabs(difference.z)<tolerance;
                    }
                    return std::hypot(difference.x,difference.y,difference.z)<=limits.epsilon;
                });
                const std::size_t index=static_cast<std::size_t>(found-points.begin());
                if (found==points.end()) points.push_back(point);
                if (indices.empty() || indices.back()!=index) indices.push_back(index);
            }
            if (indices.size()>1 && indices.front()==indices.back()) indices.pop_back();
            Vec3 area{};
            for (std::size_t index=1;index+1<indices.size();++index)
                area=Add(area,Cross(Sub(points[indices[index]],points[indices[0]]),
                    Sub(points[indices[index+1]],points[indices[0]])));
            const bool zero=area.x==0 && area.y==0 && area.z==0;
            if (!zero && Dot(area,frame.normal)<=0)
            {
                error="An original brush face reverses winding in editor float coordinates.";
                return false;
            }
            collapsed.push_back(zero);
            if (!zero) faces.push_back(std::move(indices));
        }
        if (faces.size()<4 || points.empty())
        {
            error="A recovered structural brush collapses in editor float coordinates.";
            return false;
        }
        std::vector<std::pair<std::size_t,std::size_t>> edges;
        double volumeSix=0;
        const Vec3 reference=points.front();
        for (const auto& face:faces)
        {
            for (std::size_t index=1;index+1<face.size();++index)
                volumeSix+=Dot(Sub(points[face[0]],reference),Cross(Sub(points[face[index]],reference),
                    Sub(points[face[index+1]],reference)));
            for (std::size_t index=0;index<face.size();++index)
            {
                const std::size_t start=face[index],end=face[(index+1)%face.size()];
                const Vec3 a=points[start],delta=Sub(points[end],a);
                const double lengthSquared=Dot(delta,delta);
                std::vector<std::pair<double,std::size_t>> chain{{0,start},{1,end}};
                if (!work.Spend(points.size())) return false;
                for (std::size_t other=0;other<points.size();++other)
                {
                    if (other==start || other==end) continue;
                    const Vec3 offset=Sub(points[other],a),cross=Cross(delta,offset);
                    const double t=Dot(offset,delta)/lengthSquared;
                    if (t>0 && t<1 && cross.x==0 && cross.y==0 && cross.z==0)
                        chain.emplace_back(t,other);
                }
                std::sort(chain.begin(),chain.end());
                for (std::size_t part=1;part<chain.size();++part)
                    edges.emplace_back(chain[part-1].second,chain[part].second);
            }
        }
        if (!std::isfinite(volumeSix) || volumeSix<=0)
        {
            error="A recovered structural brush has zero or reversed editor float volume.";
            return false;
        }
        std::sort(edges.begin(),edges.end());
        for (std::size_t index=0;index<edges.size();++index)
        {
            const auto edge=edges[index];
            if (edge.first==edge.second || (index>0 && edge==edges[index-1])
                || !std::binary_search(edges.begin(),edges.end(),std::make_pair(edge.second,edge.first)))
            {
                error="A recovered structural brush has an open or repeated edge after editor float conversion.";
                return false;
            }
        }
        collapsedFaces=std::move(collapsed);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        collapsedFaces.clear(); error="There is insufficient memory to validate an editor brush."; return false;
    }
    catch (const std::length_error&)
    {
        collapsedFaces.clear(); error="Editor brush validation exceeds container capacity."; return false;
    }
    }

    bool ValidateEditorBrush(const RecoveredBspGeometry::Brush& brush,
                             std::vector<bool>& collapsedFaces,std::string& error,const Limits& limits)
    {
        if (ValidateEditorBrushWeld(brush,collapsedFaces,error,limits,false)) return true;
        // Two float corners a few 1e-5 apart (CineU's subtractive boxes) are
        // one corner once written, so the brush is closed as the editor reads
        // it. Retry only the open-edge failure with the writer's weld.
        static const std::string openEdge=
            "A recovered structural brush has an open or repeated edge after editor float conversion.";
        if (error!=openEdge) return false;
        std::string retryError;
        if (ValidateEditorBrushWeld(brush,collapsedFaces,retryError,limits,true)) { error.clear(); return true; }
        return false;
    }

    bool PrepareSheetForEditor(const Surface& sheet,std::vector<Surface>& result,
                               std::string& error,const Limits& limits)
    try
    {
        result.clear(); error.clear();
        if (!ValidLimits(limits,error)) return false;
        if (sheet.vertices.size()<3 || sheet.vertices.size()>limits.maxVertices)
        {
            error="A non-solid sheet has an invalid vertex count.";
            return false;
        }
        Work work{limits,0,error};
        std::vector<Vec3> vertices;
        for (const Vec3& vertex:sheet.vertices)
        {
            if (!Finite(vertex) || std::fabs(vertex.x)>(std::numeric_limits<float>::max)()
                || std::fabs(vertex.y)>(std::numeric_limits<float>::max)()
                || std::fabs(vertex.z)>(std::numeric_limits<float>::max)())
            {
                error="A non-solid sheet vertex exceeds finite editor float coordinates.";
                return false;
            }
            const Vec3 point{static_cast<float>(vertex.x),static_cast<float>(vertex.y),static_cast<float>(vertex.z)};
            if (vertices.empty() || point.x!=vertices.back().x || point.y!=vertices.back().y || point.z!=vertices.back().z)
                vertices.push_back(point);
        }
        if (vertices.size()>1 && vertices.front().x==vertices.back().x
            && vertices.front().y==vertices.back().y && vertices.front().z==vertices.back().z)
            vertices.pop_back();
        Vec3 normal{};
        for (std::size_t index=1;index+1<vertices.size();++index)
            normal=Add(normal,Cross(Sub(vertices[index],vertices[0]),Sub(vertices[index+1],vertices[0])));
        if (vertices.size()<3 || !Normalize(normal))
        {
            error="A non-solid sheet has no stable area in editor float coordinates.";
            return false;
        }
        Frame frame;
        frame.origin=vertices.front(); frame.normal=normal;
        frame.u=Cross(std::fabs(normal.x)<0.8?Vec3{1,0,0}:Vec3{0,1,0},normal);
        if (!Normalize(frame.u))
        {
            error="A non-solid sheet has no stable projection basis.";
            return false;
        }
        frame.v=Cross(normal,frame.u);
        Polygon boundary;
        for (const Vec3& point:vertices) Append(boundary,frame.Project(point));
        Clean(boundary);
        if (boundary.size()<3)
        {
            error="A non-solid sheet collapses in its triangulation projection.";
            return false;
        }
        const bool convex=ValidatePolygon(boundary,work);
        if (convex && vertices.size()<=16 && boundary.size()==vertices.size())
        {
            // Keep a valid source polygon intact when its stored float32
            // vertices already describe a convex plane. The stricter existing
            // epsilon is used here, not the broad coplanar-material tolerance.
            // No vertex is projected, repaired, snapped, or reordered.
            const double planeTolerance=(std::min)(limits.epsilon,limits.planeTolerance);
            const bool planar=std::all_of(vertices.begin(),vertices.end(),[&](const Vec3& point)
            {
                return std::fabs(Dot(Sub(point,frame.origin),normal))<=planeTolerance;
            });
            bool forwardBoundary=true;
            for (std::size_t index=0;index<boundary.size() && forwardBoundary;++index)
            {
                const Point a=boundary[index],b=boundary[(index+1)%boundary.size()];
                const Point c=boundary[(index+2)%boundary.size()];
                // The validator tolerates tiny concavity from projection.
                // This fast path requires genuine convex turns and excludes
                // collinear edges which double back over the same boundary.
                const double turn=Side(c,a,b);
                forwardBoundary=turn>=0 && (turn!=0
                    || (b.x-a.x)*(c.x-b.x)+(b.y-a.y)*(c.y-b.y)>0);
            }
            if (planar && forwardBoundary)
            {
                if (limits.maxPieces==0)
                {
                    error="Non-solid sheet preparation exceeded its polygon budget.";
                    return false;
                }
                result.push_back({std::move(vertices),normal,sheet.materialIndex});
                return true;
            }
        }
        if (!convex)
        {
            Surface rounded{vertices,normal,sheet.materialIndex};
            if (RepairFloatBoundary(boundary,FloatSourceTolerance(rounded,frame),work)) error.clear();
        }
        std::vector<Polygon> triangles;
        if (!TriangulateSimple(boundary,triangles,work)) return false;
        // Convex validation above is only a fast-path probe. Successful
        // triangulation has independently validated a concave boundary;
        // discard that probe's diagnostic before testing native ear orders.
        error.clear();
        auto nativeTriangles = [&](const std::vector<Polygon>& trial)
        {
            for (const auto& triangle : trial)
            {
                std::vector<Vec3> points;
                for (const Point point : triangle)
                {
                    if (!work.Spend(vertices.size())) return false;
                    const auto found = std::find_if(vertices.begin(), vertices.end(), [&](const Vec3& vertex)
                    {
                        const auto projected = frame.Project(vertex);
                        return projected.x == point.x && projected.y == point.y;
                    });
                    if (found == vertices.end()) return false;
                    points.push_back(*found);
                }
                const auto area = Cross(Sub(points[1],points[0]),Sub(points[2],points[0]));
                if (area.x == 0 && area.y == 0 && area.z == 0) continue;
                const auto native = RecoveredPolygonImport::Prepare(points);
                if (!native.accepted() || !RecoveredPolygonImport::PreservesOutline(points,native)) return false;
            }
            return true;
        };
        if (!nativeTriangles(triangles))
        {
            // A slightly nonplanar cooked sheet can have almost-collinear
            // edge vertices. One valid 2D ear order isolates them as a tiny
            // 3D triangle that native import discards. Try other cyclic ear
            // orders, retaining every original boundary vertex and edge.
            // This changes diagonals only; it never drops a sliver or fills
            // a notch. Work remains covered by the shared triangulation cap.
            Polygon rotated = boundary;
            for (std::size_t attempt = 1; attempt < boundary.size(); ++attempt)
            {
                std::rotate(rotated.begin(),rotated.begin()+1,rotated.end());
                std::vector<Polygon> trial;
                if (!TriangulateSimple(rotated,trial,work)) return false;
                if (nativeTriangles(trial)) { triangles = std::move(trial); break; }
            }
            if (!error.empty()) return false;
        }
        std::vector<Surface> output;
        for (const Polygon& triangle:triangles)
        {
            if (output.size()>=limits.maxPieces)
            {
                error="Non-solid sheet triangulation exceeded its polygon budget.";
                return false;
            }
            Surface part;
            part.materialIndex=sheet.materialIndex;
            for (const Point point:triangle)
            {
                if (!work.Spend(vertices.size())) return false;
                const auto found=std::find_if(vertices.begin(),vertices.end(),[&](const Vec3& vertex)
                {
                    const Point projected=frame.Project(vertex);
                    return projected.x==point.x && projected.y==point.y;
                });
                if (found==vertices.end())
                {
                    error="Non-solid sheet triangulation introduced an unexpected vertex.";
                    return false;
                }
                part.vertices.push_back(*found);
            }
            part.normal=Cross(Sub(part.vertices[1],part.vertices[0]),Sub(part.vertices[2],part.vertices[0]));
            // A mathematically collinear 3D edge can acquire a tiny positive
            // 2D area from projection arithmetic. It contributes no rendered
            // area and must not become an invalid native FPoly triangle.
            if (part.normal.x==0 && part.normal.y==0 && part.normal.z==0) continue;
            if (!Normalize(part.normal) || Dot(part.normal,normal)<=0)
            {
                error="A non-solid sheet triangle has zero area or reversed winding.";
                return false;
            }
            output.push_back(std::move(part));
        }
        if (output.empty())
        {
            error="Non-solid sheet triangulation produced no editable polygons.";
            return false;
        }
        result=std::move(output); error.clear();
        return true;
    }
    catch (const std::bad_alloc&)
    {
        result.clear(); error="There is insufficient memory to triangulate a non-solid sheet."; return false;
    }
    catch (const std::length_error&)
    {
        result.clear(); error="Non-solid sheet triangulation exceeds container capacity."; return false;
    }

    bool SplitForVertexLimit(const std::vector<Vec3>& polygon, const Vec3& normal,
                             std::size_t maxVertices, std::vector<std::vector<Vec3>>& result,
                             std::string& error, const Limits& limits)
    try
    {
        result.clear(); error.clear();
        if (!ValidLimits(limits,error)) return false;
        if (maxVertices < 3)
        {
            error = "A source polygon requires a vertex limit of at least three.";
            return false;
        }
        Work work{limits,0,error};
        Frame frame;
        Polygon projected;
        std::vector<Vec3> importVertices;
        auto sameFloatPoint=[](const Vec3& a,const Vec3& b)
        {
            return static_cast<float>(a.x)==static_cast<float>(b.x)
                && static_cast<float>(a.y)==static_cast<float>(b.y)
                && static_cast<float>(a.z)==static_cast<float>(b.z);
        };
        for (const Vec3& vertex:polygon)
        {
            if (!Finite(vertex) || std::fabs(vertex.x)>(std::numeric_limits<float>::max)()
                || std::fabs(vertex.y)>(std::numeric_limits<float>::max)()
                || std::fabs(vertex.z)>(std::numeric_limits<float>::max)())
            {
                error="A source polygon vertex cannot be represented by the editor's float coordinates.";
                return false;
            }
            if (importVertices.empty() || !sameFloatPoint(importVertices.back(),vertex))
                importVertices.push_back(vertex);
        }
        if (importVertices.size()>1 && sameFloatPoint(importVertices.front(),importVertices.back()))
            importVertices.pop_back();
        if (importVertices.size()<3)
        {
            error="A source polygon collapses to fewer than three vertices in editor float coordinates.";
            return false;
        }
        if (!MakeFrame(importVertices,normal,frame,projected,work)) return false;
        std::vector<std::vector<Vec3>> output;
        if (projected.size() <= maxVertices)
        {
            if (limits.maxPieces == 0)
            {
                error = "Splitting a polygon exceeds the source polygon budget.";
                return false;
            }
            output.emplace_back();
            for (const Point point : projected) output.back().push_back(frame.Restore(point));
        }
        else
        {
            if (projected.size() > limits.maxPieces)
            {
                error = "Splitting a long polygon exceeds the source polygon budget.";
                return false;
            }
            Point center = projected.front();
            for (std::size_t index = 1; index < projected.size(); ++index)
            {
                const double fraction = 1.0/static_cast<double>(index+1);
                center.x += (projected[index].x-center.x)*fraction;
                center.y += (projected[index].y-center.y)*fraction;
            }
            for (std::size_t index = 0; index < projected.size(); ++index)
            {
                const Point a = projected[index], b = projected[(index+1)%projected.size()];
                if (Side(b,center,a) <= 0)
                {
                    error = "Splitting a long polygon would create a degenerate triangle.";
                    return false;
                }
                output.push_back({frame.Restore(center),frame.Restore(a),frame.Restore(b)});
            }
        }
        for (const auto& part:output)
        {
            Polygon encoded;
            for (const Vec3& point:part)
                Append(encoded,frame.Project({static_cast<float>(point.x),
                    static_cast<float>(point.y),static_cast<float>(point.z)}));
            Clean(encoded);
            if (encoded.size()<3 || Area(encoded)<=0)
            {
                error="A source polygon has zero or reversed area after conversion to editor float coordinates.";
                return false;
            }
        }
        result = std::move(output);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        result.clear(); error = "There is insufficient memory to split a recovered polygon."; return false;
    }
    catch (const std::length_error&)
    {
        result.clear(); error = "The recovered polygon exceeds container capacity."; return false;
    }

    bool PlaneBucket(const Vec3& inputNormal, const Vec3& point,
                     std::int64_t& bucket, double bucketWidth)
    {
        Vec3 normal = inputNormal;
        if (!Normalize(normal) || !Finite(point) || !std::isfinite(bucketWidth) || bucketWidth <= 0)
            return false;
        const double key = std::floor(std::fabs(Dot(normal,point))/bucketWidth);
        // A conservative floating comparison also avoids rounded INT64_MAX.
        if (!std::isfinite(key) || key < 0 || key >= 9.0e18)
            return false;
        bucket = static_cast<std::int64_t>(key);
        return true;
    }
}
