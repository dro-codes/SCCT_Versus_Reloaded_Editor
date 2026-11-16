#pragma once
// The R6 Vegas map converter's static mesh placements
// (tools/R6VMapConverter/Backend) as level-spec props. Engine-free.
//
// placement-inventory.json (written by prepare_placements) is exact: each
// entry carries the native mesh path and the native properties (Location,
// Rotation, DrawScale3D, Skins, collision and lighting flags) the converter
// worked out, and they are kept as given.
// placements.json (the scan) gives each component's source mesh and actor
// transform only: the native mesh is options.meshes {sourcePath: native
// path}, else options.package (and options.group, default "Meshes") with the
// source mesh's name; component offsets, pivots and mirroring are not
// applied, so prefer the inventory when it exists.
// Each placement becomes an environment.props StaticMeshActor.
#include "../../Reloaded.Editor/McpBridgeModel.h"
#include "Common.h"

namespace Mcp::Import
{
    inline Json ImportR6vPlacements(const std::string& text, const Json& options, Json& report)
    {
        Report(report);
        const auto rows = Bridge::Parse(text);
        if (!rows.is_array()) throw std::runtime_error("R6V placements are a JSON array (placements.json or placement-inventory.json).");
        const auto package = Option(options, "package", ""), group = Option(options, "group", "Meshes");
        const Json meshes = options.is_object() && options.contains("meshes") && options.at("meshes").is_object() ? options.at("meshes") : Json::object();
        Json spec = NewSpec(options, "r6v-placements", "R6V");
        Ids ids;
        long long placed = 0, skipped = 0, approximate = 0, mirrored = 0;
        auto strings = [](const Json& v) -> Json
        {
            // Native properties are text already; numbers from the scan become text.
            if (v.is_number()) return Num(v.get<double>()).dump();
            return v;
        };
        for (const auto& row : rows)
        {
            if (!row.is_object()) { ++skipped; continue; }
            const auto actor = row.value("actor", row.contains("source") ? row.at("source").value("actor", std::string()) : std::string());
            const auto shortName = actor.substr(actor.find_last_of('.') == std::string::npos ? 0 : actor.find_last_of('.') + 1);
            if (row.contains("native_properties"))
            {
                // placement-inventory.json
                const auto& props = row.at("native_properties");
                const auto loc = props.value("Location", Json::object());
                double x = 0, y = 0, z = 0;
                if (!ParseNumber(loc.value("X", std::string()), x) || !ParseNumber(loc.value("Y", std::string()), y) || !ParseNumber(loc.value("Z", std::string()), z)) { ++skipped; continue; }
                std::string mesh = row.value("native_mesh", std::string());
                if (mesh.empty()) mesh = props.value("StaticMesh", std::string());
                if (mesh.empty()) { ++skipped; continue; }
                Json properties = Json::object();
                for (auto it = props.begin(); it != props.end(); ++it)
                    if (it.key() != "Location" && it.key() != "StaticMesh" && it.key() != "DrawType") properties[it.key()] = it.value();
                double yawUnits = 0;
                ParseNumber(props.value("Rotation", Json::object()).value("Yaw", std::string()), yawUnits);
                Json item{{"id", ids.Make(shortName.empty() ? std::string() : "Placed_" + shortName, "Placed")}, {"class", "Engine.StaticMeshActor"}, {"mesh", mesh},
                          {"at", Point(x, y, z)}, {"yaw", Num(std::round(std::fmod(yawUnits * 360.0 / 65536.0 + 720, 360) * 10) / 10)}, {"properties", properties}};
                List(spec, {"environment", "props"}).push_back(item);
                ++placed;
                continue;
            }
            // placements.json
            const auto source = row.value("mesh", std::string());
            if (source.empty() || source == "None") { ++skipped; continue; }
            std::string native;
            if (meshes.contains(source) && meshes.at(source).is_string()) native = meshes.at(source).get<std::string>();
            else if (!package.empty()) native = package + "." + group + "." + Ids::Clean(source.substr(source.find_last_of('.') == std::string::npos ? 0 : source.find_last_of('.') + 1));
            if (native.empty()) { ++skipped; Warn(report, "Placements whose source mesh has no SCCT mesh were left out; map them with options.meshes or set options.package."); continue; }
            const auto& location = row.value("location", Json::array());
            const auto& rotation = row.value("rotation", Json::array({0, 0, 0}));
            if (!location.is_array() || location.size() != 3 || !location[0].is_number() || !location[1].is_number() || !location[2].is_number()) { ++skipped; continue; }
            const double scale = row.value("scale", 1.0);
            Json scale3d = row.value("scale3d", Json::array({1, 1, 1}));
            Json draw = Json::object();
            const char* axes[3] = {"X", "Y", "Z"};
            for (size_t k = 0; k < 3; ++k)
            {
                const double s = (scale3d.is_array() && scale3d.size() == 3 && scale3d[k].is_number() ? scale3d[k].get<double>() : 1.0) * scale;
                if (s < 0) ++mirrored;
                draw[axes[k]] = Num(std::abs(s)).dump();
            }
            Json properties{{"DrawScale3D", draw}};
            if (rotation.is_array() && rotation.size() == 3)
                properties["Rotation"] = {{"Pitch", strings(rotation[0])}, {"Yaw", strings(rotation[1])}, {"Roll", strings(rotation[2])}};
            const double yawUnits = rotation.is_array() && rotation.size() == 3 && rotation[1].is_number() ? rotation[1].get<double>() : 0.0;
            if (row.contains("component_properties") && row.at("component_properties").is_object() && !row.at("component_properties").empty()) ++approximate;
            Json item{{"id", ids.Make(shortName.empty() ? std::string() : "Placed_" + shortName, "Placed")}, {"class", "Engine.StaticMeshActor"}, {"mesh", native},
                      {"at", Point(location[0].get<double>(), location[1].get<double>(), location[2].get<double>())},
                      {"yaw", Num(std::round(std::fmod(yawUnits * 360.0 / 65536.0 + 720, 360) * 10) / 10)}, {"properties", properties}};
            List(spec, {"environment", "props"}).push_back(item);
            ++placed;
        }
        Count(report, "placements", placed);
        if (skipped) Count(report, "skipped", skipped);
        if (approximate) Warn(report, std::to_string(approximate) + " placements have component offsets or rotations that placements.json does not apply; use placement-inventory.json for exact transforms.");
        if (mirrored) Warn(report, std::to_string(mirrored) + " placement axes were mirrored in the source; the converter's mirrored mesh variants (placement-inventory.json) are needed for those.");
        Assume(report, "Each placement is a StaticMeshActor whose mesh package must already be imported (the converter's batch does that).");
        spec["provenance"]["notes"] = report.at("assumptions");
        Prune(spec);
        return spec;
    }
}
