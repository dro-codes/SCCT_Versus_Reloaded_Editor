#pragma once
// Wavefront OBJ and 3ds Max ASE meshes as assets to import, never as CSG: a
// mesh becomes a static mesh (assets_import), which a spec then places.
// Engine-free.
//
// The report's assets list holds, per file: kind "staticmesh", format,
// name, vertices, faces, triangles, groups, materials, mtllib, bounds (in the
// file's own axes), unrealBounds and size (in Unreal axes and units), and
// suggestedPackage/suggestedName for a map-owned package. The spec only
// records where it came from, unless options.mesh names the imported mesh
// (StaticMesh'Package.Group.Name'): then geometry.meshes places it at
// options.at ([0, 0, 0]).
// OBJ axes: options.up "y" (default: OBJ's usual Y-up, right-handed; Unreal
// gets (x, z, y)) or "z" (Z-up, right-handed; Unreal gets (x, -y, z)). ASE
// files are Z-up, and SCCT's importer flips their Y. options.scale (1)
// multiplies.
#include "Common.h"
#include <functional>

namespace Mcp::Import
{
    namespace MeshDetail
    {
        struct Mesh
        {
            std::vector<Vector> vertices;
            long long faces = 0, triangles = 0;
            std::vector<std::string> groups, materials;
            std::string mtllib;
        };
        inline Json Bounds(const std::vector<Vector>& points)
        {
            if (points.empty()) return Json();
            Vector lo = points[0], hi = points[0];
            for (const auto& p : points) for (size_t k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
            return {{"min", Point(lo[0], lo[1], lo[2])}, {"max", Point(hi[0], hi[1], hi[2])}};
        }
        inline void AddUnique(std::vector<std::string>& list, const std::string& value)
        {
            if (!value.empty() && std::find(list.begin(), list.end(), value) == list.end()) list.push_back(value);
        }
        inline Json Asset(const Mesh& mesh, const std::string& format, const Json& options, const std::function<Vector(const Vector&)>& toUnreal, Json& report)
        {
            const auto path = Option(options, "path", "");
            const auto stem = Stem(path);
            const auto name = Ids::Clean(stem.empty() ? std::string("Mesh") : stem);
            std::vector<Vector> unreal;
            unreal.reserve(mesh.vertices.size());
            for (const auto& v : mesh.vertices) unreal.push_back(toUnreal(v));
            Json asset{{"kind", "staticmesh"}, {"format", format}, {"name", name.empty() ? std::string("Mesh") : name}, {"vertices", mesh.vertices.size()}, {"faces", mesh.faces},
                       {"triangles", mesh.triangles}, {"groups", mesh.groups}, {"materials", mesh.materials}, {"bounds", Bounds(mesh.vertices)}, {"unrealBounds", Bounds(unreal)}};
            if (!path.empty()) asset["file"] = path;
            if (!mesh.mtllib.empty()) asset["mtllib"] = mesh.mtllib;
            if (!unreal.empty())
            {
                const auto& b = asset.at("unrealBounds");
                asset["size"] = Json::array({Num(b.at("max")[0].get<double>() - b.at("min")[0].get<double>()), Num(b.at("max")[1].get<double>() - b.at("min")[1].get<double>()),
                                             Num(b.at("max")[2].get<double>() - b.at("min")[2].get<double>())});
            }
            // Map-owned names: <map file stem>_STM, as the release rules want.
            const auto map = Option(options, "mapFile", "");
            if (!map.empty()) asset["suggestedPackage"] = Ids::Clean(map) + "_STM";
            asset["suggestedName"] = asset.at("name");
            List(report, {"assets"}).push_back(asset);
            Count(report, "vertices", static_cast<long long>(mesh.vertices.size()));
            Count(report, "triangles", mesh.triangles);
            if (mesh.vertices.empty()) Warn(report, "The mesh has no vertices.");
            Json spec = NewSpec(options, format);
            spec["provenance"]["source"]["asset"] = asset;
            const auto placed = Option(options, "mesh", "");
            if (!placed.empty())
            {
                Json at = options.contains("at") ? options.at("at") : Json::array({0, 0, 0});
                List(spec, {"geometry", "meshes"}).push_back({{"id", Ids::Clean(asset.at("name").get<std::string>()).empty() ? std::string("Mesh1") : Ids::Clean(asset.at("name").get<std::string>())}, {"mesh", placed}, {"at", at}});
                Count(report, "meshes");
            }
            else Assume(report, "A mesh is imported as a static mesh asset (assets_import), not as level geometry; pass mesh (its StaticMesh path) once imported to place it.");
            spec["provenance"]["notes"] = report.at("assumptions");
            return spec;
        }
    }
    inline Json ImportObj(const std::string& text, const Json& options, Json& report)
    {
        using namespace MeshDetail;
        Report(report);
        const double scale = Option(options, "scale", 1.0, 1e-6, 1e6);
        const auto up = Lower(Option(options, "up", "y"));
        if (up != "y" && up != "z") throw std::runtime_error("Option up is y or z.");
        Mesh mesh;
        long long badFaces = 0;
        int number = 0;
        for (const auto& raw : Lines(text))
        {
            ++number;
            const auto line = Trim(raw);
            if (line.empty() || line[0] == '#') continue;
            const auto space = line.find_first_of(" \t");
            const auto key = line.substr(0, space);
            const auto rest = space == std::string::npos ? std::string() : Trim(line.substr(space + 1));
            if (key == "v")
            {
                const auto n = Numbers(rest);
                if (n.size() < 3) throw std::runtime_error("OBJ line " + std::to_string(number) + ": a vertex needs x, y and z.");
                mesh.vertices.push_back({n[0], n[1], n[2]});
            }
            else if (key == "f")
            {
                size_t corners = 0;
                bool ok = true;
                std::string token;
                auto corner = [&](const std::string& t)
                {
                    if (t.empty()) return;
                    double index = 0;
                    if (!ParseNumber(t.substr(0, t.find('/')), index) || index == 0) { ok = false; return; }
                    const double resolved = index < 0 ? static_cast<double>(mesh.vertices.size()) + index + 1 : index;
                    if (resolved < 1 || resolved > static_cast<double>(mesh.vertices.size())) ok = false;
                    ++corners;
                };
                for (char c : rest) { if (std::isspace(static_cast<unsigned char>(c))) { corner(token); token.clear(); } else token += c; }
                corner(token);
                if (!ok || corners < 3) { ++badFaces; continue; }
                ++mesh.faces;
                mesh.triangles += static_cast<long long>(corners) - 2;
            }
            else if (key == "o" || key == "g") AddUnique(mesh.groups, rest);
            else if (key == "usemtl") AddUnique(mesh.materials, rest);
            else if (key == "mtllib") mesh.mtllib = rest;
        }
        if (badFaces) Warn(report, std::to_string(badFaces) + " faces refer to vertices the file does not have (or have fewer than 3 corners) and were not counted.");
        const auto toUnreal = [&](const Vector& v) { return up == "y" ? Vector{v[0] * scale, v[2] * scale, v[1] * scale} : Vector{v[0] * scale, -v[1] * scale, v[2] * scale}; };
        Assume(report, up == "y" ? "The OBJ is Y-up and right-handed: Unreal X, Y, Z are its x, z, y." : "The OBJ is Z-up and right-handed: Unreal X, Y, Z are its x, -y, z.");
        return Asset(mesh, "obj", options, toUnreal, report);
    }
    inline Json ImportAse(const std::string& text, const Json& options, Json& report)
    {
        using namespace MeshDetail;
        Report(report);
        const double scale = Option(options, "scale", 1.0, 1e-6, 1e6);
        if (text.find("*3DSMAX_ASCIIEXPORT") == std::string::npos) Warn(report, "The file does not start like an ASE export (*3DSMAX_ASCIIEXPORT).");
        Mesh mesh;
        for (const auto& raw : Lines(text))
        {
            const auto line = Trim(raw);
            if (StartsWith(line, "*MESH_VERTEX ") || StartsWith(line, "*MESH_VERTEX\t"))
            {
                const auto n = Numbers(line.substr(12));
                if (n.size() >= 4) mesh.vertices.push_back({n[1], n[2], n[3]});
            }
            else if (StartsWith(line, "*MESH_FACE ") || StartsWith(line, "*MESH_FACE\t")) { ++mesh.faces; ++mesh.triangles; }
            else if (StartsWith(line, "*NODE_NAME")) AddUnique(mesh.groups, Trim(line.substr(10)).substr(0, 128));
            else if (StartsWith(line, "*MATERIAL_NAME")) AddUnique(mesh.materials, Trim(line.substr(14)).substr(0, 128));
        }
        for (auto& g : mesh.groups) if (g.size() >= 2 && g.front() == '"' && g.back() == '"') g = g.substr(1, g.size() - 2);
        for (auto& m : mesh.materials) if (m.size() >= 2 && m.front() == '"' && m.back() == '"') m = m.substr(1, m.size() - 2);
        Assume(report, "ASE files are Z-up; SCCT's static mesh importer flips their Y, so Unreal X, Y, Z are x, -y, z.");
        return Asset(mesh, "ase", options, [&](const Vector& v) { return Vector{v[0] * scale, -v[1] * scale, v[2] * scale}; }, report);
    }
}
