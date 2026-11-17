#pragma once
// Which compiled polygons may give a recovered brush face its material, and
// whether a face is a wall at all. These are the rules the OpenSCCT Editor's
// brush recovery settled on after measuring every stock map:
//
//  - A compiled polygon covers a face only when it faces the way the face's
//    surface does: into the empty side. A thin wall has a polygon on each
//    side within the plane tolerance; the far one belongs to the other room.
//  - "On the face's plane" is measured at the face, not at the origin: the
//    polygon's node plane passes within the tolerance of every corner of the
//    face, or the polygon's own points lie within it of the face's plane. A
//    split plane tilted by 1e-4 is a unit off 10,000 units out, and cooked
//    polygon points stray up to 0.22 units from their node's plane.
//  - The tolerance widens in tiers while part of the face stays uncovered:
//    0.02 units, then the BSP's 0.25-unit coplanar band, and for a wall still
//    bare, polygons within a unit and about 2.5 degrees.
//  - A face no polygon covers that has solid on one side and empty space on
//    the other is a wall the compiled map drew nothing on. It stays invisible.
//
// No Windows or engine calls, so the tests compile it alone.
#include "RecoveredBspGeometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace RecoveredSurfaceMatch
{
    using Vec3 = RecoveredBspGeometry::Vec3;

    constexpr double kPlaneTolerance = 0.02;
    constexpr double kCoplanarBand = 0.25;
    constexpr double kNormalTolerance = 0.00001;
    constexpr double kLooseTolerance = 1.0;
    constexpr double kLooseNormalTolerance = 0.001;
    // How far beside a surface solid and empty space are sampled.
    constexpr double kProbeOffset = 0.5;
    // Cells narrower than this are not made into brushes.
    constexpr double kSliverWidth = 0.01;
    // A face counts as covered when less than this area (square units), or a
    // millionth of the face, is left bare.
    constexpr double kBareArea = 0.01;

    inline Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    inline double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    inline double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

    // Newell's vector: twice the polygon's area along its normal.
    inline Vec3 Newell(const std::vector<Vec3>& points)
    {
        Vec3 sum;
        for (std::size_t index = 0; index < points.size(); ++index)
        {
            const Vec3& a = points[index];
            const Vec3& b = points[(index + 1) % points.size()];
            sum.x += (a.y - b.y) * (a.z + b.z);
            sum.y += (a.z - b.z) * (a.x + b.x);
            sum.z += (a.x - b.x) * (a.y + b.y);
        }
        return sum;
    }

    inline double Area(const std::vector<Vec3>& points, const Vec3& normal)
    {
        return points.size() < 3 ? 0 : Dot(Newell(points), normal) / 2;
    }

    inline Vec3 Centre(const std::vector<Vec3>& points)
    {
        Vec3 sum;
        for (const Vec3& point : points) { sum.x += point.x; sum.y += point.y; sum.z += point.z; }
        const double count = points.empty() ? 1.0 : static_cast<double>(points.size());
        return {sum.x / count, sum.y / count, sum.z / count};
    }

    // The polygon without corners that double back along one line (a spike
    // out and back: its two edges point opposite ways within 1e-3 of a
    // radian) or run straight on (within 1e-6), repeated until none is left;
    // corners within 1e-3 units of the one before go too. Some cooked
    // polygons fold back on themselves (z -448 -> -453 -> -450 -> -467): the
    // spike has no area, and its backward edge reads as a wall facing the
    // wrong way. Fewer than three points may come back.
    inline std::vector<Vec3> Unfold(std::vector<Vec3> points)
    {
        bool changed = true;
        while (changed && points.size() >= 3)
        {
            changed = false;
            for (std::size_t index = 0; index < points.size() && points.size() >= 3; ++index)
            {
                const std::size_t count = points.size();
                const Vec3 ab = Sub(points[index], points[(index + count - 1) % count]);
                const Vec3 bc = Sub(points[(index + 1) % count], points[index]);
                const double lab = Length(ab), lbc = Length(bc);
                bool drop = lab < 1e-3;
                if (!drop && lbc >= 1e-3)
                {
                    const double cross = Length(Cross(ab, bc)) / (lab * lbc);
                    drop = Dot(ab, bc) < 0 ? cross <= 1e-3 : cross <= 1e-6;
                }
                if (!drop) continue;
                points.erase(points.begin() + static_cast<std::ptrdiff_t>(index));
                changed = true;
                --index;
            }
        }
        return points;
    }

    // The compiled node tree, for asking whether a point is in empty space.
    // Node planes need not be unit length: only the side matters.
    struct Tree
    {
        std::vector<RecoveredBspGeometry::Node> nodes;
        bool rootOutside = false;

        // The engine's walk: front when n.p >= d; the state turns empty in
        // front of a CSG node and stays as it was behind a non-CSG one.
        bool IsOutside(const Vec3& point) const
        {
            bool outside = rootOutside;
            int index = nodes.empty() ? -1 : 0;
            for (std::size_t visited = 0; index >= 0 && visited <= nodes.size(); ++visited)
            {
                if (static_cast<std::size_t>(index) >= nodes.size()) break;
                const auto& node = nodes[static_cast<std::size_t>(index)];
                const bool front = Dot(node.normal, point) >= node.distance;
                outside = front ? outside || node.isCsg : outside && !node.isCsg;
                index = front ? node.front : node.back;
            }
            return outside;
        }

        // True when a face lies between solid and empty space: sampled half
        // a unit either side of its middle.
        bool Between(const std::vector<Vec3>& face, const Vec3& normal) const
        {
            const Vec3 mid = Centre(face);
            const Vec3 ahead{mid.x + normal.x * kProbeOffset, mid.y + normal.y * kProbeOffset, mid.z + normal.z * kProbeOffset};
            const Vec3 behind{mid.x - normal.x * kProbeOffset, mid.y - normal.y * kProbeOffset, mid.z - normal.z * kProbeOffset};
            return IsOutside(ahead) != IsOutside(behind);
        }
    };

    // The structural polygons of a compiled map, findable by plane.
    class Index
    {
    public:
        // id is the caller's handle, returned by Find. normal is the surface's
        // facing; planeNormal and planeDistance are the polygon's node plane
        // (dot(planeNormal, p) = planeDistance), any length. False when the
        // plane cannot be used; the polygon is then not indexed.
        bool Add(std::size_t id, const std::vector<Vec3>& points, const Vec3& normal,
                 Vec3 planeNormal, double planeDistance)
        {
            const double length = Length(planeNormal);
            if (points.size() < 3 || !std::isfinite(length) || length < 1e-12 || !std::isfinite(planeDistance))
                return false;
            Entry entry;
            entry.id = id;
            entry.points = points;
            entry.normal = normal;
            entry.planeNormal = {planeNormal.x / length, planeNormal.y / length, planeNormal.z / length};
            entry.planeDistance = planeDistance / length;
            entry.low = entry.high = points.front();
            for (const Vec3& point : points) Grow(entry.low, entry.high, point);
            const double key = std::floor(std::fabs(entry.planeDistance));
            if (!(key < 9.0e15)) return false;
            byDistance_[static_cast<std::int64_t>(key)].push_back(entries_.size());
            entries_.push_back(std::move(entry));
            return true;
        }

        std::size_t Size() const { return entries_.size(); }

        // The polygons that can cover a convex face with this outward normal,
        // in the order they were added. facing is -1 for a subtractive brush
        // (its walls' polygons face into it, against its faces) and +1 for an
        // additive one. thorough looks at every polygon, not only those whose
        // plane, at the origin, lies within reach of the face's.
        std::vector<std::size_t> Find(const std::vector<Vec3>& face, const Vec3& normal, int facing,
                                      double tolerance, double normalTolerance, bool thorough) const
        {
            std::vector<std::size_t> found;
            if (face.size() < 3) return found;
            const double distance = Dot(normal, face.front());
            Vec3 low = face.front(), high = face.front();
            double furthest = 0;
            for (const Vec3& point : face)
            {
                Grow(low, high, point);
                furthest = (std::max)(furthest, Length(point));
            }
            const double margin = (std::max)(tolerance, kCoplanarBand);
            std::vector<std::size_t> entries;
            auto consider = [&](std::size_t entryIndex)
            {
                const Entry& entry = entries_[entryIndex];
                const double towards = Dot(entry.normal, normal);
                if ((towards > 0 ? 1 : towards < 0 ? -1 : 0) != facing) return;
                if (std::fabs(Dot(entry.planeNormal, normal)) < 1 - normalTolerance) return;
                if (entry.high.x < low.x - margin || entry.low.x > high.x + margin
                    || entry.high.y < low.y - margin || entry.low.y > high.y + margin
                    || entry.high.z < low.z - margin || entry.low.z > high.z + margin) return;
                // The node's plane decides first: the polygon's own points may
                // stray within the coplanar band.
                const bool planeAtFace = std::all_of(face.begin(), face.end(), [&](const Vec3& point)
                    { return std::fabs(Dot(entry.planeNormal, point) - entry.planeDistance) <= tolerance; });
                if (planeAtFace || std::all_of(entry.points.begin(), entry.points.end(), [&](const Vec3& point)
                    { return std::fabs(Dot(normal, point) - distance) <= tolerance; }))
                    entries.push_back(entryIndex);
            };
            if (thorough)
                for (std::size_t index = 0; index < entries_.size(); ++index) consider(index);
            else
            {
                // A plane within the normal tolerance of the face's can lie
                // this much further from the origin than the face's, measured
                // there, and still pass through the face.
                const double reach = std::sqrt(2 * normalTolerance) * furthest + tolerance;
                const double key = std::floor(std::fabs(distance));
                if (!(key < 9.0e15) || !(reach < 1.0e6)) return found;
                const std::int64_t centre = static_cast<std::int64_t>(key);
                const std::int64_t span = static_cast<std::int64_t>(std::ceil(reach));
                for (std::int64_t bucket = (std::max<std::int64_t>)(0, centre - span); bucket <= centre + span; ++bucket)
                {
                    const auto group = byDistance_.find(bucket);
                    if (group != byDistance_.end())
                        for (std::size_t entryIndex : group->second) consider(entryIndex);
                }
                std::sort(entries.begin(), entries.end());
            }
            found.reserve(entries.size());
            for (std::size_t entryIndex : entries) found.push_back(entries_[entryIndex].id);
            return found;
        }

    private:
        struct Entry
        {
            std::size_t id = 0;
            std::vector<Vec3> points;
            Vec3 normal, planeNormal, low, high;
            double planeDistance = 0;
        };

        static void Grow(Vec3& low, Vec3& high, const Vec3& point)
        {
            low = {(std::min)(low.x, point.x), (std::min)(low.y, point.y), (std::min)(low.z, point.z)};
            high = {(std::max)(high.x, point.x), (std::max)(high.y, point.y), (std::max)(high.z, point.z)};
        }

        std::vector<Entry> entries_;
        std::unordered_map<std::int64_t, std::vector<std::size_t>> byDistance_;
    };

    // How many solid/empty samples a rebuild may leave different and still be
    // kept (and reported): one in a hundred, as the OpenSCCT Editor accepts a
    // map's own brushes. More than that is a rebuild that went wrong.
    inline bool AcceptableMisses(std::size_t misses, std::size_t probes)
    {
        return misses * 100 <= probes;
    }
}
