#pragma once
// One entry point for every importer (import_to_spec): the format named or
// found from the file's extension and contents, its importer run, and the
// result checked against the scct.level-spec schema. Engine-free and
// Windows-free.
//
//   Json spec = Mcp::Import::Import("auto", "C:\\plans\\ship.svg", bytes, options, report);
//
// options go to the importer (each header lists its own); name sets the
// map's name (default: the file's stem) and path is filled from the path.
// report is {format, file, warnings, counts, assumptions, ...}.
#include "../LevelSpecModel.h"
#include "Authoring.h"
#include "Csv.h"
#include "GeoJson.h"
#include "Grid.h"
#include "ImagePlan.h"
#include "Obj.h"
#include "QuakeMap.h"
#include "R6vPlacements.h"
#include "Svg.h"
#include "T3d.h"
#include "Tiled.h"
#include "Vmf.h"

namespace Mcp::Import
{
    struct Format { const char* name; const char* extensions; const char* description; };
    inline const std::vector<Format>& Formats()
    {
        static const std::vector<Format> formats = {
            {"level-spec", ".json", "A scct.level-spec: checked and returned with its defaults filled in."},
            {"map-changes", ".json", "A scct.map-changes document: its operations become the spec's extra.operations."},
            {"map-authoring", ".json", "A map-authoring export (Export Map to JSON, authoring.export): existing.refs to edit the map in place (mode refs), or a spec that rebuilds it (mode recreate)."},
            {"grid", ".txt .grid .csv", "An ASCII or CSV grid, one legend character per cell (# solid, . floor, S M starts, O F Z B K objectives, D doors, V vents, C N X L A H security and props); '--- storey N ---' separates storeys."},
            {"image", ".png .bmp .tga", "A raster floor plan read by colour (black solid, white floor, coloured markers) into the grid importer."},
            {"svg", ".svg", "An SVG plan: rects and polygons are rooms, data-kind or class marks starts, objectives, cameras, doors and geometric events; layers are storeys."},
            {"tiled", ".tmj .tmx", "A Tiled map: tile layers are the grid (tile kinds from tileset properties), object layers add rooms and markers."},
            {"geojson", ".geojson .json", "GeoJSON: Polygons are rooms (holes are pillars), Points markers, LineStrings lines, by properties.kind; storey from properties.storey."},
            {"csv", ".csv .tsv", "A table of actors (class, x, y, z, yaw, id, tag, event, then any properties): created as written."},
            {"t3d", ".t3d", "Unreal T3D text: CSG brushes as polygon brushes, volumes, starts, lights, static meshes and other actors."},
            {"vmf", ".vmf", "A Hammer map: convex brushes inverted for SCCT's solid world (a hull carved, the brushes added back), plus starts and lights."},
            {"quake", ".map", "A Quake-family map (Quake, Half-Life, Quake 3, Doom 3): convex brushes inverted for SCCT's solid world, plus starts and lights."},
            {"obj", ".obj", "A Wavefront OBJ mesh: its size and parts as a static mesh asset to import (never level geometry)."},
            {"ase", ".ase", "A 3ds Max ASE mesh: its size and parts as a static mesh asset to import."},
            {"r6v", ".json", "R6 Vegas converter placements (placements.json or placement-inventory.json): StaticMeshActor props."}};
        return formats;
    }
    namespace ImportDetail
    {
        inline std::string Canonical(const std::string& format)
        {
            const auto f = Lower(Trim(format));
            static const std::map<std::string, std::string> aliases{
                {"", "auto"}, {"auto", "auto"}, {"spec", "level-spec"}, {"level-spec", "level-spec"}, {"scct.level-spec", "level-spec"}, {"changes", "map-changes"},
                {"map-changes", "map-changes"}, {"scct.map-changes", "map-changes"}, {"authoring", "map-authoring"}, {"map-authoring", "map-authoring"}, {"export", "map-authoring"},
                {"live", "map-authoring"}, {"scct.map-authoring", "map-authoring"}, {"grid", "grid"}, {"ascii", "grid"}, {"text", "grid"}, {"image", "image"}, {"png", "image"},
                {"bmp", "image"}, {"tga", "image"}, {"raster", "image"}, {"svg", "svg"}, {"tiled", "tiled"}, {"tmj", "tiled"}, {"tmx", "tiled"}, {"geojson", "geojson"},
                {"csv", "csv"}, {"actors", "csv"}, {"table", "csv"}, {"t3d", "t3d"}, {"unreal", "t3d"}, {"vmf", "vmf"}, {"hammer", "vmf"}, {"quake", "quake"}, {"map", "quake"},
                {"halflife", "quake"}, {"q3", "quake"}, {"doom3", "quake"}, {"obj", "obj"}, {"wavefront", "obj"}, {"ase", "ase"}, {"r6v", "r6v"}, {"placements", "r6v"}};
            const auto found = aliases.find(f);
            if (found == aliases.end())
            {
                std::string names;
                for (const auto& fmt : Formats()) names += (names.empty() ? "" : ", ") + std::string(fmt.name);
                throw std::runtime_error("Unknown import format \"" + format + "\"; use auto or one of " + names + ".");
            }
            return found->second;
        }
        inline std::string Head(const std::string& bytes, size_t n = 4096) { return StripBom(bytes.substr(0, std::min(bytes.size(), n))); }
        inline std::string JsonKind(const Json& doc)
        {
            if (doc.is_object())
            {
                const auto format = doc.value("format", std::string());
                if (format == "scct.level-spec") return "level-spec";
                if (format == "scct.map-changes") return "map-changes";
                if (format == "scct.map-authoring") return "map-authoring";
                static const std::set<std::string> geo{"FeatureCollection", "Feature", "GeometryCollection", "Polygon", "MultiPolygon", "Point", "MultiPoint", "LineString", "MultiLineString"};
                if (geo.count(doc.value("type", std::string()))) return "geojson";
                if (doc.contains("tiledversion") || (doc.contains("layers") && (doc.contains("tilewidth") || doc.contains("tileheight")))) return "tiled";
            }
            if (doc.is_array() && !doc.empty() && doc[0].is_object() && (doc[0].contains("native_mesh") || (doc[0].contains("component") && doc[0].contains("mesh")))) return "r6v";
            return {};
        }
        inline bool StartsT3d(const std::string& head)
        {
            const auto t = Lower(Trim(head));
            return StartsWith(t, "begin map") || StartsWith(t, "begin actor") || StartsWith(t, "begin level") || StartsWith(t, "begin brush");
        }
    }
    // The format a file is, from its extension and contents.
    inline std::string Detect(const std::string& path, const std::string& bytes)
    {
        using namespace ImportDetail;
        const auto ext = Extension(path);
        const auto head = Head(bytes);
        const auto trimmed = Trim(head);
        static const std::set<std::string> text{"json", "tmj", "geojson", "svg", "tmx", "t3d", "vmf", "map", "obj", "ase", "csv", "tsv", "txt", "grid", "asc"};
        if (ext == "tga" || ext == "png" || ext == "bmp" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "webp" || ext == "dds") return "image";
        if (!text.count(ext) && !Image::Format(bytes).empty() && Image::Format(bytes) != "tga") return "image";
        if (ext == "json" || ext == "tmj" || ext == "geojson" || (!trimmed.empty() && (trimmed[0] == '{' || trimmed[0] == '[') && ext != "map" && ext != "vmf"))
        {
            try
            {
                const auto kind = JsonKind(Bridge::Parse(StripBom(bytes)));
                if (!kind.empty()) return kind;
                if (ext == "tmj") return "tiled";
                if (ext == "geojson") return "geojson";
                if (ext == "json") throw std::runtime_error("The JSON file is not a level spec, map-changes, map-authoring export, GeoJSON, Tiled map or R6V placements; name its format.");
            }
            catch (const Workflow::Json::exception&)
            {
                if (ext == "json" || ext == "tmj" || ext == "geojson") throw;
            }
        }
        if (ext == "svg") return "svg";
        if (ext == "tmx") return "tiled";
        if (ext == "t3d") return "t3d";
        if (ext == "vmf") return "vmf";
        if (ext == "map") return StartsT3d(head) ? "t3d" : "quake";
        if (ext == "obj") return "obj";
        if (ext == "ase") return "ase";
        if (ext == "csv" || ext == "tsv") return IsActorTable(bytes) ? "csv" : "grid";
        if (ext == "txt" || ext == "grid" || ext == "asc") return StartsT3d(head) ? "t3d" : "grid";
        // No telling extension: the contents.
        if (StartsWith(trimmed, "<"))
        {
            const auto lower = Lower(head);
            if (lower.find("<svg") != std::string::npos) return "svg";
            if (lower.find("<map") != std::string::npos) return "tiled";
        }
        if (StartsT3d(head)) return "t3d";
        if (StartsWith(Lower(trimmed), "versioninfo") || StartsWith(Lower(trimmed), "world")) return "vmf";
        if (head.find("*3DSMAX_ASCIIEXPORT") != std::string::npos) return "ase";
        if ((StartsWith(trimmed, "{") || StartsWith(trimmed, "//")) && Lower(head).find("classname") != std::string::npos) return "quake";
        {
            // OBJ: every line a comment or an OBJ statement, and some vertices.
            static const std::set<std::string> statements{"v", "vt", "vn", "vp", "f", "l", "p", "o", "g", "s", "mtllib", "usemtl"};
            bool vertices = false, other = false;
            size_t seen = 0;
            for (const auto& line : Lines(head))
            {
                const auto t = Trim(line);
                if (t.empty() || t[0] == '#') continue;
                if (++seen > 40) break;
                const auto word = t.substr(0, t.find_first_of(" \t"));
                if (!statements.count(word)) { other = true; break; }
                vertices = vertices || word == "v";
            }
            if (vertices && !other) return "obj";
        }
        if (Image::Format(bytes) == "tga") return "image";
        if (IsActorTable(head)) return "csv";
        return "grid";
    }
    inline Json Import(const std::string& format, const std::string& path, const std::string& bytes, const Json& options, Json& report)
    {
        using namespace ImportDetail;
        Report(report);
        if (bytes.size() > 256u * 1024 * 1024) throw std::runtime_error("The file is over 256 MB.");
        if (!options.is_null() && !options.is_object()) throw std::runtime_error("Import options are a JSON object.");
        auto kind = Canonical(format);
        if (kind == "auto") kind = Detect(path, bytes);
        Json opts = options.is_object() ? options : Json::object();
        if (!opts.contains("path") && !path.empty()) opts["path"] = path;
        if (!opts.contains("name") && !opts.contains("mapName") && !path.empty() && kind != "map-authoring") opts["name"] = MapName(Stem(path));
        report["format"] = kind;
        if (!path.empty()) report["file"] = path;
        const auto text = StripBom(bytes);
        Json spec;
        if (kind == "level-spec") return Spec::Parse(Bridge::Parse(text));
        if (kind == "map-changes")
        {
            const auto doc = Bridge::Parse(text);
            if (!doc.is_object() || doc.value("format", std::string()) != "scct.map-changes" || !doc.contains("operations") || !doc.at("operations").is_array())
                throw std::runtime_error("This is not a scct.map-changes document with operations.");
            spec = NewSpec(opts, "map-changes");
            auto& operations = List(spec, {"extra", "operations"});
            long long deletes = 0;
            for (const auto& op : doc.at("operations"))
            {
                operations.push_back(op);
                if (op.is_object() && op.value("op", std::string()) == "delete") ++deletes;
            }
            if (deletes) Warn(report, std::to_string(deletes) + " delete operations came across; deletes need the exported map and allowDeletes, which a level build does not give them. Use map_changes_apply for those.");
            if (doc.contains("description") && doc.at("description").is_string()) spec["provenance"]["notes"] = Json::array({doc.at("description")});
            Count(report, "operations", static_cast<long long>(operations.size()));
        }
        else if (kind == "map-authoring") spec = ImportAuthoring(text, opts, report);
        else if (kind == "grid") spec = ImportGrid(text, opts, report);
        else if (kind == "image") spec = ImportImagePlan(bytes, opts, report);
        else if (kind == "svg") spec = ImportSvg(text, opts, report);
        else if (kind == "tiled") spec = ImportTiled(text, opts, report);
        else if (kind == "geojson") spec = ImportGeoJson(text, opts, report);
        else if (kind == "csv") spec = ImportCsv(text, opts, report);
        else if (kind == "t3d") spec = ImportT3d(text, opts, report);
        else if (kind == "vmf") spec = ImportVmf(text, opts, report);
        else if (kind == "quake") spec = ImportQuakeMap(text, opts, report);
        else if (kind == "obj") spec = ImportObj(text, opts, report);
        else if (kind == "ase") spec = ImportAse(text, opts, report);
        else if (kind == "r6v") spec = ImportR6vPlacements(text, opts, report);
        else throw std::runtime_error("No importer for " + kind + ".");
        // What an importer writes must be a valid spec; anything else is a
        // fault here, not in the file.
        try { return Spec::Parse(spec); }
        catch (const Spec::SpecError& e) { throw std::runtime_error(std::string("The ") + kind + " importer made a spec that does not check (" + e.what() + "); report this with the file."); }
    }
    // The formats as JSON, for tool descriptions and errors.
    inline Json FormatList()
    {
        Json list = Json::array();
        for (const auto& f : Formats()) list.push_back({{"name", f.name}, {"extensions", f.extensions}, {"description", f.description}});
        return list;
    }
}
