#pragma once
// Thumbnails for Working Views and Actor Assemblies: small PNGs of the
// perspective viewport kept in System\ReloadedEditor\thumbnails, one per
// entry, named after the entry's id (never its display name, so a rename
// leaves the picture where it is). This header is the pure part: the file
// names, which files no entry owns any more, the picture's size, and where
// to put a viewport camera so a box of actors fills the shot. No Windows,
// no engine, so the tests compile it alone.
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace Thumbnail
{
    enum class Kind { View, Assembly };

    constexpr int kWidth = 256;            // Saved picture width; height follows the viewport.
    constexpr int kMaxHeight = 256;        // A tall viewport is fitted by height instead.
    constexpr int kCellWidth = 120, kCellHeight = 72; // The list's image cells.
    constexpr std::size_t kMaxFileBytes = 4u << 20; // Anything larger is not one of ours.
    inline const char* Folder() { return "thumbnails"; }

    inline const char* Prefix(Kind kind) { return kind == Kind::View ? "view-" : "assembly-"; }

    // Library ids are 32 hex digits; anything that could leave the folder or
    // clash with another name is refused rather than written.
    inline bool ValidId(const std::string& id)
    {
        if (id.empty() || id.size() > 64) return false;
        for (char c : id)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_')) return false;
        return true;
    }
    inline std::string Lower(std::string text)
    {
        for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return text;
    }

    inline std::string FileName(Kind kind, const std::string& id)
    {
        if (!ValidId(id)) throw std::runtime_error("This entry has no usable id for a thumbnail.");
        return std::string(Prefix(kind)) + Lower(id) + ".png";
    }

    // The entry a file in the folder belongs to. Temporary files left by an
    // interrupted save parse too, flagged so the sweep removes them.
    struct Parsed { Kind kind = Kind::View; std::string id; bool temporary = false; };
    inline bool Parse(const std::string& file, Parsed& out)
    {
        std::string name = Lower(file);
        out = Parsed{};
        const std::string tmp = ".tmp";
        if (name.size() > tmp.size() && name.compare(name.size() - tmp.size(), tmp.size(), tmp) == 0)
        {
            out.temporary = true;
            name.resize(name.size() - tmp.size());
        }
        const std::string png = ".png";
        if (name.size() <= png.size() || name.compare(name.size() - png.size(), png.size(), png) != 0) return false;
        name.resize(name.size() - png.size());
        for (Kind kind : { Kind::View, Kind::Assembly })
        {
            const std::string prefix = Prefix(kind);
            if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
            {
                out.kind = kind;
                out.id = name.substr(prefix.size());
                return ValidId(out.id);
            }
        }
        return false;
    }

    // Files in the thumbnail folder that no saved entry owns: deleted
    // entries, views of a map saved untitled, interrupted saves. Files that
    // are not ours are never listed.
    inline std::vector<std::string> Orphans(const std::vector<std::string>& files, const std::set<std::string>& viewIds, const std::set<std::string>& assemblyIds)
    {
        std::set<std::string> views, assemblies;
        for (const auto& id : viewIds) views.insert(Lower(id));
        for (const auto& id : assemblyIds) assemblies.insert(Lower(id));
        std::vector<std::string> result;
        for (const auto& file : files)
        {
            Parsed parsed;
            if (!Parse(file, parsed)) continue;
            const auto& live = parsed.kind == Kind::View ? views : assemblies;
            if (parsed.temporary || !live.count(parsed.id)) result.push_back(file);
        }
        return result;
    }

    struct Size { int width = 0, height = 0; };
    // The saved picture's size for a viewport: kWidth wide with the aspect
    // kept, or kMaxHeight tall for a viewport taller than it is wide.
    inline Size Fit(int sourceWidth, int sourceHeight, int maxWidth = kWidth, int maxHeight = kMaxHeight)
    {
        if (sourceWidth <= 0 || sourceHeight <= 0 || maxWidth <= 0 || maxHeight <= 0) throw std::runtime_error("The viewport has no size to capture.");
        const double scale = (std::min)(static_cast<double>(maxWidth) / sourceWidth, static_cast<double>(maxHeight) / sourceHeight);
        Size size{ static_cast<int>(std::lround(sourceWidth * scale)), static_cast<int>(std::lround(sourceHeight * scale)) };
        size.width = std::clamp(size.width, 1, maxWidth);
        size.height = std::clamp(size.height, 1, maxHeight);
        return size;
    }

    // A picture at most maxEdge pixels on its longer side, the aspect kept,
    // never enlarged: the MCP bridge's viewport screenshots.
    inline Size Within(int sourceWidth, int sourceHeight, int maxEdge)
    {
        if (sourceWidth <= 0 || sourceHeight <= 0 || maxEdge <= 0) throw std::runtime_error("The viewport has no size to capture.");
        if ((std::max)(sourceWidth, sourceHeight) <= maxEdge) return { sourceWidth, sourceHeight };
        return Fit(sourceWidth, sourceHeight, maxEdge, maxEdge);
    }

    // Where a picture sits inside a list cell: fitted whole, centred.
    struct Rect { int x = 0, y = 0, width = 0, height = 0; };
    inline Rect Letterbox(int imageWidth, int imageHeight, int cellWidth = kCellWidth, int cellHeight = kCellHeight)
    {
        const Size size = Fit(imageWidth, imageHeight, cellWidth, cellHeight);
        return { (cellWidth - size.width) / 2, (cellHeight - size.height) / 2, size.width, size.height };
    }

    // A camera pose that shows a box of actor locations whole. The yaw is
    // the viewport's own, so the shot faces the way the user was looking;
    // the pitch looks down at kPitchDegrees for a three-quarter view. Actor
    // locations are pivots, so padding stands in for the actors' own size.
    constexpr double kPitchDegrees = -25.0, kPadding = 160.0, kMinDistance = 320.0;
    struct Camera { std::array<double, 3> location{}; std::array<int, 3> rotation{}; };
    inline Camera FrameBox(const std::array<double, 3>& low, const std::array<double, 3>& high, int yaw, double fovDegrees, double aspect)
    {
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(low[axis]) || !std::isfinite(high[axis]) || low[axis] > high[axis] || std::abs(low[axis]) > 1e7 || std::abs(high[axis]) > 1e7)
                throw std::runtime_error("The selection has no usable bounds.");
        if (!std::isfinite(fovDegrees) || fovDegrees < 1 || fovDegrees > 170) fovDegrees = 90;
        if (!std::isfinite(aspect) || aspect <= 0) aspect = 4.0 / 3.0;
        const double pi = 3.14159265358979323846;
        std::array<double, 3> centre{};
        for (int axis = 0; axis < 3; ++axis) centre[axis] = (low[axis] + high[axis]) / 2;
        const double dx = high[0] - low[0], dy = high[1] - low[1], dz = high[2] - low[2];
        const double radius = kPadding + std::sqrt(dx * dx + dy * dy + dz * dz) / 2;
        // The narrower of the two half-angles decides: the horizontal FOV
        // narrows vertically on a wide viewport.
        const double halfHorizontal = fovDegrees * pi / 360;
        const double halfVertical = std::atan(std::tan(halfHorizontal) / aspect);
        const double half = (std::min)(halfHorizontal, halfVertical);
        const double distance = (std::max)(kMinDistance, radius / std::sin(half));
        const int pitch = static_cast<int>(std::lround(kPitchDegrees * 65536 / 360));
        const double p = pitch * 2 * pi / 65536, y = yaw * 2 * pi / 65536;
        const std::array<double, 3> forward{ std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p) };
        Camera camera;
        for (int axis = 0; axis < 3; ++axis) camera.location[axis] = centre[axis] - forward[axis] * distance;
        camera.rotation = { pitch & 0xffff, yaw & 0xffff, 0 };
        return camera;
    }
}
