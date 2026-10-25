#pragma once
// The scct.level-spec compiler (LevelSpecModel.h): a declarative level turned
// into one mcp.plan the bridge runs as a single Undo step, plus the scene the
// rules check offline and the scene plan_render draws. Engine-free and
// Windows-free; it reuses the Map Design model for every piece of geometry
// (Design::Geometry, WallDoorway, AttachedPiece, RecountSteps, DesignIssues) and
// the Zones model's conventions for sector missions.
//
// Compiled.plan (mcp.plan arguments, McpPlanModel.h order):
//   pre    "packages"  authoring.apply of load ops for the packages the spec's
//                      materials, meshes and sounds come from (and the spec's
//                      own extra load/texture/save ops)
//   steps  "mapid", "q<n>"   reads, only when an incremental build deletes or
//                            edits actors it built before (their before values)
//          "deletes"         authoring.apply v3: actors and blockout brushes the
//                            spec no longer has
//          "blocks_replaced" design.blockbatch of changed pieces (previous set)
//          "blocks<n>"       design.blockbatch, 64 new pieces at most each,
//                            Carve construction (brushMode "pieces")
//          "actors"          authoring.apply v2 (map "*"): every actor, sized
//                            volume and polygon brush, with $ref between them
//          "lift_<id>"       design.lift for a lift without a mesh
//          "players<n>", "targets<n>"  objectiveplayers.set / .targets, bound
//                            to /identities/<id> of "actors"
//          "ge<n>"           ge.add
//          "settings", "settings_edit"  settings.inspect, then settings.edit
//                            with {"$bind": {"step": "settings"}} as snapshot
//          "unregister", "register"  the Map Design library records
//          "sidecar"         mcp.sidecar.set: the spec, its hash and what each
//                            id became, for the next incremental build
//   post   "build" (design.build), "surface<n>", "check" (mapcheck.run), "save"
//          (map.save), "mapsettings", "interface<n>" (optional), "screenshot"
//
// Compiled.scene (Mcp::Rules::SceneFromJson):
//   {actors:[{path (the id), class, cls, name, tag, event, position:[x,y,z],
//             rotation:[p,y,r], values (the properties as written), pointer}],
//    pieces:[{id, spec, position, rotation, pointer, zone, storey,
//             generated (true on stairwells, lift shafts, their flights and
//             zone portals the compiler adds)}]  (Map Design records),
//    ges:[{kind, command, points}], level:{LevelInfo changes}, mapSettings
//    (SMapSettings fields or null), modes:[story|diskhunt|deathmatch],
//    mapName, mapFile, profile, geometryBuilt:false, live:false,
//    release:{... , interface}, zones:[{id, locationName, spaces}],
//    playerCounts:[{objective, min, max}], targets:[{mission, sizes}],
//    documents:[the whole level as one scct.map-changes v2 document],
//    schema (options.schema, when given), pointers:{id: JSON pointer}}
//
// Compiled.render (Mcp::Render::PlanPng, agent "importers"), top-down, Unreal
// units, X east, Y north:
//   {bounds:{min:[x,y,z], max:[x,y,z]},
//    pieces:[{id, kind (the piece kind: Room|Corridor|...|Doorway), name, storey,
//             box:{min:[x,y,z], max:[x,y,z]}, yaw (degrees), space (bool),
//             zone}, then each polygon brush (geometry.brushes) and sized volume
//             as {id, kind (add|subtract|volume|mover), name, cls, storey, box, yaw 0,
//             outline:[[x,y]...]}],
//    doorways:[{id, at:[x,y,z], width, yaw (degrees), portal}],
//    actors:[{id, cls, kind, at:[x,y,z], position (= at), yaw (degrees), label}]
//      kind: spawn-spy|spawn-merc|objective|flag|dropzone|bomb|disk|camera|
//            camnet|laser|motion|presence|alarm|light|door|lift|button|sound|
//            volume|ingredient|ge|other,
//    lines:[{from:[x,y,z], to:[x,y,z], kind (laser|camnet|alarm|event|lock|
//            ge-<ledge|hoh|pipe|ladder|zipline|fence|pole>|lift), ids:[a,b]}],
//    labels:[{at:[x,y,z], text, kind (zone|room)}]}
//
// Incremental builds (options.sidecar, sync "merge" or "incremental") diff the
// spec against the sidecar the last build stored: unchanged ids are left as
// they are, changed pieces are replaced (design.blockbatch "previous"),
// changed actors are updated or deleted and created again, and ids the spec no
// longer has are deleted. sync "replace" deletes everything the sidecar lists
// and builds the spec afresh. "merge" lays the spec over the sidecar's spec
// first (Merge), which is how the intent tools add to a level.
#include "LevelSpecModel.h"
// The shared models put two statements on one line after a for or an if,
// which GCC's -Wall reads as misleading indentation.
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "../Reloaded.Editor/MapDesignModel.h"
#include "../Reloaded.Editor/StageModel.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#include "../Reloaded.Editor/McpPlanModel.h"
#include <array>
#include <functional>
#include <sstream>
#if !defined(RE_SPEC_NO_RULES)
#if __has_include("LevelRulesModel.h")
#include "LevelRulesModel.h"
#define RE_SPEC_RULES 1
#elif __has_include("../Reloaded.Editor/LevelRulesModel.h")
#include "../Reloaded.Editor/LevelRulesModel.h"
#define RE_SPEC_RULES 1
#endif
#endif

namespace Mcp::Spec
{
    struct CompileOptions
    {
        std::string brushMode = "pieces"; // pieces (Map Design blockouts) | polygons (polygon brushes)
        std::string sync = "merge";       // merge | incremental | replace (with a sidecar)
        Json sidecar;                     // mcp.sidecar.get's sidecar, or null
        Json schema;                      // authoring.schema's result ({classes:{...}}), optional
        bool build = true;
        std::string save;                 // map.save path, "" for none
        bool screenshot = true;
    };
    struct Compiled
    {
        Json plan;
        Json scene;
        Json render;
        Json sidecar;
        std::vector<Diagnostic> diagnostics;
        Json summary;
    };

    namespace Detail
    {
        namespace Design = Workflow::Design;
        namespace Stages = Workflow::Stages;
        using Workflow::Fold;
        using Workflow::Pose;
        using Workflow::Rotation;
        using Workflow::Vector;
        constexpr double kPi = 3.14159265358979323846;

        // Property text: whole numbers without decimals, others to six places.
        inline std::string Num(double v)
        {
            if (!std::isfinite(v)) throw std::runtime_error("A number in the spec is out of range.");
            const double r = std::round(v);
            if (std::abs(v - r) < 1e-6 && std::abs(r) < 1e15) { std::ostringstream o; o << static_cast<long long>(r); return o.str(); }
            std::ostringstream o;
            o.setf(std::ios::fixed);
            o.precision(6);
            o << v;
            auto s = o.str();
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
            return s;
        }
        // Positions to a millionth of a unit, so a turned piece's float noise
        // (2e-14) does not reach the map or the sidecar's comparisons.
        inline Vector Snap(Vector v)
        {
            for (auto& c : v) c = std::abs(c) < 1e-9 ? 0.0 : std::round(c * 1e6) / 1e6;
            return v;
        }
        inline Json Loc(const Vector& v) { return {{"X", Num(v[0])}, {"Y", Num(v[1])}, {"Z", Num(v[2])}}; }
        inline Json Rot(const Rotation& r) { return {{"Pitch", std::to_string(r[0])}, {"Yaw", std::to_string(r[1])}, {"Roll", std::to_string(r[2])}}; }
        inline int YawUnits(double degrees)
        {
            const auto units = static_cast<long long>(std::llround(degrees * 65536.0 / 360.0)) % 65536;
            return static_cast<int>(units < 0 ? units + 65536 : units);
        }
        inline double Degrees(int units) { return units * 360.0 / 65536.0; }
        inline std::string Quoted(const std::string& text) { return Stages::Quoted(text); }
        inline const char* Flag(bool value) { return value ? "True" : "False"; }
        inline Json Ref(const std::string& target) { return {{"$ref", target}}; }
        inline Json Point(const Vector& v) { return Json::array({v[0], v[1], v[2]}); }
        // StaticMesh'Pkg.Group.Name' from either that or Pkg.Group.Name.
        inline std::string Asset(const std::string& kind, const std::string& path)
        {
            if (path.find('\'') != std::string::npos) return path;
            return kind + "'" + path + "'";
        }
        // The package an asset path or reference lives in.
        inline std::string PackageOf(const std::string& path)
        {
            auto text = path;
            const auto quote = text.find('\'');
            if (quote != std::string::npos) text = text.substr(quote + 1, text.find('\'', quote + 1) - quote - 1);
            const auto dot = text.find('.');
            return dot == std::string::npos ? std::string() : text.substr(0, dot);
        }
        // Raw property values: numbers and booleans become property text.
        inline Json Encode(const Json& value)
        {
            if (value.is_boolean()) return Flag(value.get<bool>());
            if (value.is_number()) return Num(value.get<double>());
            if (value.is_array()) { Json out = Json::array(); for (const auto& item : value) out.push_back(Encode(item)); return out; }
            if (value.is_object())
            {
                if (value.contains("$ref")) return value;
                Json out = Json::object();
                for (auto it = value.begin(); it != value.end(); ++it) out[it.key()] = Encode(it.value());
                return out;
            }
            return value;
        }
        // Every $ref inside a value.
        inline void Refs(const Json& value, std::set<std::string>& out)
        {
            if (value.is_object())
            {
                if (value.contains("$ref") && value.at("$ref").is_string()) out.insert(value.at("$ref").get<std::string>());
                for (const auto& item : value) Refs(item, out);
            }
            else if (value.is_array()) for (const auto& item : value) Refs(item, out);
        }
        // $refs to ids in a map replaced by those actors' paths.
        inline Json Rewrite(const Json& value, const std::map<std::string, std::string>& paths)
        {
            if (value.is_object())
            {
                if (value.size() == 1 && value.contains("$ref") && value.at("$ref").is_string())
                {
                    const auto found = paths.find(Fold(value.at("$ref").get<std::string>()));
                    return found == paths.end() ? value : Ref(found->second);
                }
                Json out = Json::object();
                for (auto it = value.begin(); it != value.end(); ++it) out[it.key()] = Rewrite(it.value(), paths);
                return out;
            }
            if (value.is_array()) { Json out = Json::array(); for (const auto& item : value) out.push_back(Rewrite(item, paths)); return out; }
            return value;
        }
        // The wall of a piece facing a compass side, in the piece's own axes, and
        // whether an offset along it runs the piece's way (+1) or against it: the
        // offset is east of centre on a north or south wall, north of it on an
        // east or west wall. False when the piece is not turned a multiple of 90.
        inline bool CompassWall(const Rotation& rotation, const std::string& side, std::string& wall, double& along)
        {
            const double a = rotation[1] * 2 * kPi / 65536;
            const double xx = std::cos(a), xy = std::sin(a), yx = -std::sin(a), yy = std::cos(a);
            double tx = 0, ty = 0;
            if (side == "north") ty = 1;
            else if (side == "south") ty = -1;
            else if (side == "east") tx = 1;
            else if (side == "west") tx = -1;
            else return false;
            struct Candidate { const char* name; double x, y; bool offsetAlongX; };
            const Candidate candidates[] = {{"+X", xx, xy, false}, {"-X", -xx, -xy, false}, {"+Y", yx, yy, true}, {"-Y", -yx, -yy, true}};
            for (const auto& c : candidates)
                if (c.x * tx + c.y * ty > 0.999)
                {
                    wall = c.name;
                    const double ox = c.offsetAlongX ? xx : yx, oy = c.offsetAlongX ? xy : yy;
                    along = (side == "north" || side == "south") ? (ox > 0.5 ? 1.0 : -1.0) : (oy > 0.5 ? 1.0 : -1.0);
                    return true;
                }
            return false;
        }
        inline std::vector<std::string> Strings(const Json& value)
        {
            std::vector<std::string> out;
            if (value.is_string()) out.push_back(value.get<std::string>());
            else if (value.is_array()) for (const auto& item : value) if (item.is_string()) out.push_back(item.get<std::string>());
            return out;
        }
        inline Json Polygons(const std::vector<Design::Face>& faces, const std::string& texture, unsigned flags)
        {
            Json polygons = Json::array();
            auto snap = [](double value) { return std::abs(value) < 1e-6 ? 0.0 : value; };
            for (const auto& face : faces)
            {
                Json polygon{{"vertices", Json::array()}};
                for (const auto& v : face) polygon["vertices"].push_back(Json::array({snap(v[0]), snap(v[1]), snap(v[2])}));
                if (!texture.empty() && Fold(texture) != "none") polygon["texture"] = texture;
                if (flags) polygon["flags"] = flags;
                polygons.push_back(polygon);
            }
            return polygons;
        }
        inline Json BoxPolygons(const Vector& half)
        {
            return Polygons(Design::Box({-half[0], -half[1], -half[2]}, {half[0], half[1], half[2]}).faces, "", 0);
        }
        // Which named face of a brush a polygon is: the way its surface faces,
        // as surface.texture names the BSP surfaces after a build. A subtractive
        // brush's surfaces face into the space it carves, so its polygon on the
        // +X side is the east wall, which faces west (-X), and its top is the
        // ceiling.
        inline std::string FaceName(const Json& vertices, bool subtract)
        {
            double n[3] = {0, 0, 0};
            for (size_t i = 0; i < vertices.size(); ++i)
            {
                const auto& a = vertices[i];
                const auto& b = vertices[(i + 1) % vertices.size()];
                n[0] += (a[1].get<double>() - b[1].get<double>()) * (a[2].get<double>() + b[2].get<double>());
                n[1] += (a[2].get<double>() - b[2].get<double>()) * (a[0].get<double>() + b[0].get<double>());
                n[2] += (a[0].get<double>() - b[0].get<double>()) * (a[1].get<double>() + b[1].get<double>());
            }
            // Newell's normal follows the winding, which points out of the solid.
            int axis = 0;
            for (int k = 1; k < 3; ++k) if (std::abs(n[k]) > std::abs(n[axis])) axis = k;
            const bool facing = (n[axis] > 0) != subtract;
            if (axis == 2) return facing ? "floor" : "ceiling";
            return std::string(facing ? "+" : "-") + (axis == 0 ? "X" : "Y");
        }

        struct Piece
        {
            std::string id, kind, pointer, storey, zone;
            Json spec;
            Pose pose;
            Design::Extent box; // the open space for rooms, corridors and vents; the solid's bounds otherwise
            bool space = false, generated = false;
            std::string joins[2]; // a doorway's two spaces
        };
        struct Actor
        {
            std::string id, cls, role, pointer, label;
            Json properties = Json::object();
            std::string geometry = "point";
            Json polygons;
            Vector at{};
            int yaw = 0;
            bool footprint = false; // drawn by plan_render as its polygons' outline
            std::string storey;     // a footprint's storey, for plan_render's storey filter
        };
        struct Lift { std::string id, pointer; Json args; Vector at{}; };
        // What a spec compiles to, before it becomes a plan.
        struct Lowered
        {
            std::vector<Piece> pieces;
            std::vector<Actor> actors;
            std::vector<Lift> lifts;
            std::vector<Json> links;          // map-changes link ops reaching actors outside the spec
            std::map<std::string, Json> updates; // existing actor path -> {actor, properties}
            std::vector<Json> ges, players, targets, surfaces, interfaceTextures, extraOps, packageOps;
            Json settings = Json::object(), mapSettings;
            std::set<std::string> packages;
            std::vector<std::string> modes;
            Json lines = Json::array(), labels = Json::array();
            Json zones = Json::array();
        };

        class Compiler
        {
        public:
            Compiler(const Json& normalised, const CompileOptions& compileOptions, std::vector<Diagnostic>& diagnostics)
                : input(normalised), options(compileOptions), findings(diagnostics), scale(Scale(normalised)) {}
            Lowered Run()
            {
                // Findings point at the item an id names, not at its "id" field.
                for (const auto& [id, pointer] : AllIds())
                {
                    usedIds.insert(Fold(id));
                    pointerOf[Fold(id)] = pointer.size() > 3 && pointer.compare(pointer.size() - 3, 3, "/id") == 0 ? pointer.substr(0, pointer.size() - 3) : pointer;
                }
                Existing();
                RegisterAnchors(input, "");
                Storeys();
                Pieces();
                Zones();
                Doorways();
                Vertical();
                ZonePortals();
                ZoneInfos();
                Brushes();
                Spawns();
                Environment();
                Lights();
                Mechanisms();
                StoryTriggers();
                Security();
                Story();
                DiskHunt();
                Deathmatch();
                Players();
                Events();
                RoomLights();
                Ges();
                Settings();
                Interface();
                Extra();
                CompleteAll();
                return std::move(low);
            }
            const std::map<std::string, std::string>& Pointers() const { return pointerOf; }

        private:
            const Json& input;
            const CompileOptions& options;
            std::vector<Diagnostic>& findings;
            double scale;
            Lowered low;
            std::map<std::string, size_t> pieceIndex, actorIndex;
            std::map<std::string, std::string> pointerOf;
            std::set<std::string> usedIds;
            std::map<std::string, double> storeyZ;
            struct ExistingRef { std::string path, cls, tag; };
            std::map<std::string, ExistingRef> refs;
            struct AnchorItem { Json at; std::string pointer; };
            std::map<std::string, AnchorItem> anchorItems;
            std::set<std::string> anchorBusy;
            std::map<std::string, std::string> zoneOf;   // piece id -> zone id
            std::map<std::string, bool> zonePortals;     // zone id -> portals wanted
            std::map<std::string, std::string> eventSetBy;
            std::map<std::string, std::vector<std::pair<std::string, std::string>>> triggersOf; // objective -> [(trigger, kind)]
            std::map<std::string, std::string> triggerKind;
            std::vector<std::string> objectiveOrder, detectorIds, doorIds;
            std::map<std::string, Vector> objectivePositions;
            std::set<std::string> lightIds, objectIds, magicIds;
            std::map<std::string, int> alongWall;

            void Diag(const std::string& severity, const std::string& rule, const std::string& pointer, const std::string& message, const std::string& wiki = {}, const Json& fix = Json())
            {
                findings.push_back({severity, rule, pointer, message, wiki, fix});
            }
            // An SMagicEvent entry's Delay, which the engine takes from 0 to
            // 86400 seconds (MagicEventModel.h): its text, or "" after an
            // error at the item's delay, for an entry the caller leaves out.
            std::string Delay(const Json& item, const std::string& pointer, const std::string& rule, const std::string& wiki)
            {
                const double seconds = item.value("delay", 0.0);
                if (seconds >= 0 && seconds <= 86400) return Workflow::Magic::Seconds(seconds);
                Diag("error", rule, Child(pointer, "delay"), "a delay is 0 to 86400 seconds (one day); " + Num(seconds) + " cannot be written, so this entry is left out.", wiki);
                return {};
            }
            double L(const Json& value) const { return value.get<double>() * scale; }
            const Json& Section(std::initializer_list<const char*> path) const
            {
                static const Json empty;
                const Json* at = &input;
                for (auto key : path)
                {
                    if (!at->is_object() || !at->contains(key)) return empty;
                    at = &at->at(key);
                }
                return *at;
            }
            static const Json& List(const Json& value)
            {
                static const Json empty = Json::array();
                return value.is_array() ? value : empty;
            }
            // A temporary (item.value(...)) is kept by value, so a reference to
            // the list never outlives it.
            static Json List(Json&& value) { return value.is_array() ? std::move(value) : Json::array(); }
            std::vector<std::pair<std::string, std::string>> AllIds() const
            {
                std::vector<std::pair<std::string, std::string>> ids;
                CollectIds(input, "", ids);
                return ids;
            }
            // A fresh id from a stem, never one the spec or the compiler already uses.
            std::string NewId(const std::string& stem)
            {
                std::string base;
                for (char c : stem) if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') base += c;
                if (base.empty() || std::isdigit(static_cast<unsigned char>(base[0]))) base = "Id" + base;
                if (base.size() > 56) base.resize(56);
                if (!usedIds.count(Fold(base))) { usedIds.insert(Fold(base)); return base; }
                for (int i = 2; i < 100000; ++i)
                {
                    auto id = base + "_" + std::to_string(i);
                    if (!usedIds.count(Fold(id))) { usedIds.insert(Fold(id)); return id; }
                }
                throw std::runtime_error("Could not allocate an id from " + stem + ".");
            }
            std::string PointerOf(const std::string& id) const
            {
                const auto found = pointerOf.find(Fold(id));
                return found == pointerOf.end() ? std::string() : found->second;
            }

            // --- References ------------------------------------------------------
            void Existing()
            {
                const auto& given = Section({"existing", "refs"});
                if (!given.is_object()) return;
                for (auto it = given.begin(); it != given.end(); ++it)
                    refs[Fold(it.key())] = {it.value().value("path", std::string()), it.value().value("class", std::string()), it.value().value("tag", std::string())};
            }
            bool IsActor(const std::string& id) const { return actorIndex.count(Fold(id)) != 0; }
            Actor* FindActor(const std::string& id)
            {
                const auto found = actorIndex.find(Fold(id));
                return found == actorIndex.end() ? nullptr : &low.actors[found->second];
            }
            int FindPiece(const std::string& id) const
            {
                const auto found = pieceIndex.find(Fold(id));
                return found == pieceIndex.end() ? -1 : static_cast<int>(found->second);
            }
            // The Tag that reaches an id: a spec actor's Tag (its id unless set), a
            // live actor's from existing.refs, or the text itself when nothing in
            // the spec has that id (a Tag the author chose).
            std::string TagOf(const std::string& target, const std::string& pointer, bool mustExist)
            {
                if (const auto* actor = FindActor(target))
                {
                    const auto tag = actor->properties.value("Tag", std::string());
                    return tag.empty() ? actor->id : tag;
                }
                if (const auto found = refs.find(Fold(target)); found != refs.end())
                {
                    if (found->second.tag.empty()) { Diag("error", "ref.tag", pointer, "existing.refs." + target + " has no tag, so nothing can send it an event. Add its Tag to the ref.", "events"); return target; }
                    return found->second.tag;
                }
                if (mustExist) Diag("error", "ref.unresolved", pointer, "Nothing in the spec has the id \"" + target + "\".", "events");
                return target;
            }
            // An object reference to an id: {"$ref": id} for the spec's own actors,
            // {"$ref": path} for live ones.
            Json ObjectRef(const std::string& target, const std::string& pointer)
            {
                if (const auto* actor = FindActor(target)) return Ref(actor->id);
                if (const auto found = refs.find(Fold(target)); found != refs.end())
                {
                    if (found->second.path.empty()) { Diag("error", "ref.path", pointer, "existing.refs." + target + " names a Tag only; an object reference needs its path.", "events"); return "None"; }
                    return Ref(found->second.path);
                }
                Diag("error", "ref.unresolved", pointer, "Nothing in the spec has the id \"" + target + "\".", "events");
                return "None";
            }
            void SetEvent(Actor& actor, const std::string& tag, const std::string& pointer)
            {
                const auto current = actor.properties.value("Event", std::string());
                if (!current.empty() && Fold(current) != "none" && Fold(current) != Fold(tag))
                {
                    Diag("error", "events.conflict", pointer, actor.id + " already sends Event " + current + " (" + eventSetBy[Fold(actor.id)] + "); an actor sends one Event. Send it to an events.magic SMagicEvent that fans out instead.", "events");
                    return;
                }
                actor.properties["Event"] = tag;
                eventSetBy[Fold(actor.id)] = pointer;
            }

            // --- Positions ---------------------------------------------------------
            struct Height { bool ceiling = false; double offset = 0; };
            static Height Floor(double offset) { return {false, offset}; }
            static Height Ceiling(double offset) { return {true, offset}; }
            Height HeightOf(const Json& text, const std::string& pointer) const
            {
                if (text.is_number()) return {false, L(text)};
                static const std::regex pattern("^\\s*(floor|ceiling)\\s*(([+-])\\s*([0-9]+(\\.[0-9]+)?))?\\s*$");
                std::smatch m;
                const auto value = text.get<std::string>();
                if (!std::regex_match(value, m, pattern)) throw SpecError(pointer, "write a height as \"floor+N\" or \"ceiling-N\".");
                double n = m[4].matched ? std::stod(m[4].str()) * scale : 0;
                if (m[3].matched && m[3].str() == "-") n = -n;
                return m[1].str() == "ceiling" ? Height{true, -n} : Height{false, n};
            }
            // The room, corridor or vent under a point: of several, the one whose
            // span holds z when z is known, else the lowest.
            int SpaceAt(double x, double y, double z, bool useZ) const
            {
                int best = -1;
                for (size_t i = 0; i < low.pieces.size(); ++i)
                {
                    const auto& p = low.pieces[i];
                    if (!p.space) continue;
                    if (x < p.box.lo[0] - 0.5 || x > p.box.hi[0] + 0.5 || y < p.box.lo[1] - 0.5 || y > p.box.hi[1] + 0.5) continue;
                    if (useZ && (z < p.box.lo[2] - 1 || z > p.box.hi[2] + 1)) continue;
                    if (best < 0 || p.box.lo[2] < low.pieces[static_cast<size_t>(best)].box.lo[2]) best = static_cast<int>(i);
                }
                return best;
            }
            double LevelIn(int room, const Height& h) const
            {
                if (room < 0) return h.ceiling ? 256 - h.offset : h.offset;
                const auto& p = low.pieces[static_cast<size_t>(room)];
                return h.ceiling ? p.box.hi[2] - h.offset : p.box.lo[2] + h.offset;
            }
            struct Where { Vector at{}; int room = -1; int yaw = 0; bool hasYaw = false; };
            Where Locate(const Json& at, const std::string& pointer, const Height& usual)
            {
                Where w;
                if (at.is_array())
                {
                    if (at.size() < 2 || !at[0].is_number() || !at[1].is_number()) throw SpecError(pointer, "x and y are numbers; only z may be \"floor+N\" or \"ceiling-N\".");
                    w.at = {L(at[0]), L(at[1]), 0};
                    if (at.size() == 3 && at[2].is_number())
                    {
                        w.at[2] = L(at[2]);
                        w.room = SpaceAt(w.at[0], w.at[1], w.at[2], true);
                        return w;
                    }
                    w.room = SpaceAt(w.at[0], w.at[1], 0, false);
                    w.at[2] = LevelIn(w.room, at.size() == 3 ? HeightOf(at[2], Child(pointer, 2)) : usual);
                    if (w.room < 0) Diag("warning", "position.outside", pointer, "This point is not inside any room, corridor or vent of the spec, so its height counts from Z 0.");
                    return w;
                }
                if (!at.is_object()) throw SpecError(pointer, "a position is [x, y, z], {in, u, v, h}, {near, offset} or {doorway}.");
                if (at.contains("in"))
                {
                    const auto id = at.at("in").get<std::string>();
                    const int index = FindPiece(id);
                    if (index < 0) throw SpecError(Child(pointer, "in"), "no geometry piece has the id \"" + id + "\".");
                    const auto& p = low.pieces[static_cast<size_t>(index)];
                    const double w0 = p.spec.at("width"), l0 = p.spec.at("length"), h0 = p.spec.at("height");
                    const auto h = at.contains("h") ? HeightOf(at.at("h"), Child(pointer, "h")) : usual;
                    const Vector local{at.value("u", 0.0) * w0, at.value("v", 0.0) * l0, h.ceiling ? h0 - h.offset : h.offset};
                    w.at = Workflow::TransformPoint(local, p.pose);
                    w.room = p.space ? index : SpaceAt(w.at[0], w.at[1], w.at[2], true);
                    return w;
                }
                if (at.contains("near"))
                {
                    w.at = Anchor(at.at("near").get<std::string>(), Child(pointer, "near"));
                    if (at.contains("offset"))
                    {
                        const auto& offset = at.at("offset");
                        for (size_t k = 0; k < offset.size() && k < 3; ++k) w.at[k] += L(offset[k]);
                    }
                    w.room = SpaceAt(w.at[0], w.at[1], w.at[2], true);
                    return w;
                }
                if (at.contains("doorway"))
                {
                    const auto id = at.at("doorway").get<std::string>();
                    const int index = FindPiece(id);
                    if (index < 0 || low.pieces[static_cast<size_t>(index)].kind != "Doorway") throw SpecError(Child(pointer, "doorway"), "no doorway has the id \"" + id + "\".");
                    const auto& p = low.pieces[static_cast<size_t>(index)];
                    w.at = p.pose.position;
                    if (at.contains("h"))
                    {
                        const auto h = HeightOf(at.at("h"), Child(pointer, "h"));
                        w.at[2] += h.ceiling ? p.spec.at("height").get<double>() - h.offset : h.offset;
                    }
                    w.yaw = p.pose.rotation[1];
                    w.hasYaw = true;
                    w.room = SpaceAt(w.at[0], w.at[1], w.at[2] + 1, true);
                    return w;
                }
                throw SpecError(pointer, "a position is [x, y, z], {in, u, v, h}, {near, offset} or {doorway}.");
            }
            void RegisterAnchors(const Json& value, const std::string& at)
            {
                if (value.is_object())
                {
                    if (value.contains("id") && value.at("id").is_string() && value.contains("at")) anchorItems[Fold(value.at("id").get<std::string>())] = {value.at("at"), Child(at, "at")};
                    for (auto it = value.begin(); it != value.end(); ++it)
                        if (it.key() != "props" && it.key() != "properties" && it.key() != "document" && it.key() != "extra") RegisterAnchors(it.value(), Child(at, it.key()));
                }
                else if (value.is_array()) for (size_t i = 0; i < value.size(); ++i) RegisterAnchors(value[i], Child(at, i));
            }
            // Where an id is, for {near} and {toward}: a piece's floor centre, an
            // actor's position, or the item's own position worked out on demand.
            Vector Anchor(const std::string& id, const std::string& pointer)
            {
                if (const int index = FindPiece(id); index >= 0) return low.pieces[static_cast<size_t>(index)].pose.position;
                if (const auto* actor = FindActor(id)) return actor->at;
                const auto item = anchorItems.find(Fold(id));
                if (item != anchorItems.end())
                {
                    if (!anchorBusy.insert(Fold(id)).second) throw SpecError(pointer, "positions refer to each other in a loop through \"" + id + "\".");
                    const auto found = Locate(item->second.at, item->second.pointer, Floor(0)).at;
                    anchorBusy.erase(Fold(id));
                    return found;
                }
                if (refs.count(Fold(id))) throw SpecError(pointer, "\"" + id + "\" is a live actor (existing.refs); its position is not in the spec, so place things near spec ids or by coordinates.");
                throw SpecError(pointer, "nothing in the spec has the id \"" + id + "\".");
            }
            int Facing(const Json& facing, const Vector& from, const std::string& pointer, int fallback)
            {
                if (facing.is_null()) return fallback;
                if (facing.is_number()) return YawUnits(facing.get<double>());
                if (facing.contains("yaw")) return YawUnits(facing.at("yaw").get<double>());
                const auto& toward = facing.at("toward");
                const Vector target = toward.is_string() ? Anchor(toward.get<std::string>(), Child(pointer, "toward")) : Locate(toward, Child(pointer, "toward"), Floor(0)).at;
                try { return Workflow::Security::YawTowards(from, target); }
                catch (const std::exception&) { Diag("warning", "position.facing", pointer, "It is turned toward a point right above or below it, so it keeps its yaw."); return fallback; }
            }
            int YawOf(const Json& item, const Vector& from, const std::string& pointer, int fallback)
            {
                if (item.contains("facing")) return Facing(item.at("facing"), from, Child(pointer, "facing"), fallback);
                if (item.contains("yaw")) return YawUnits(item.at("yaw").get<double>());
                return fallback;
            }
            // The middle of the spec's rooms, which starts face by default.
            Vector Centre() const
            {
                bool any = false;
                Vector lo{}, hi{};
                for (const auto& p : low.pieces)
                {
                    if (!p.space) continue;
                    for (int k = 0; k < 3; ++k)
                    {
                        lo[k] = any ? std::min(lo[k], p.box.lo[k]) : p.box.lo[k];
                        hi[k] = any ? std::max(hi[k], p.box.hi[k]) : p.box.hi[k];
                    }
                    any = true;
                }
                return {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]};
            }
            // Where a logic actor with no position of its own goes: inside the
            // first room, so it is in the level.
            Vector Fallback(double above = 64) const
            {
                for (const auto& p : low.pieces)
                    if (p.space) return {p.pose.position[0], p.pose.position[1], p.box.lo[2] + above};
                return {0, 0, above};
            }

            // --- Actors --------------------------------------------------------------
            Actor& Add(const std::string& id, const std::string& cls, const std::string& role, const std::string& pointer, const Vector& at, int yaw, bool rotated = true)
            {
                if (actorIndex.count(Fold(id))) throw SpecError(pointer, "the id \"" + id + "\" is used twice.");
                Design::CheckVector(at);
                const auto snapped = Snap(at);
                Actor actor;
                actor.id = id;
                actor.cls = ClassName(cls);
                actor.role = role;
                actor.pointer = pointer;
                actor.at = snapped;
                actor.yaw = yaw;
                actor.label = id;
                actor.properties["Location"] = Loc(snapped);
                if (rotated) actor.properties["Rotation"] = Rot({0, yaw, 0});
                actorIndex[Fold(id)] = low.actors.size();
                if (!pointerOf.count(Fold(id))) pointerOf[Fold(id)] = pointer;
                usedIds.insert(Fold(id));
                low.actors.push_back(std::move(actor));
                return low.actors.back();
            }
            void Props(Actor& actor, const Json& item)
            {
                if (!item.is_object()) return;
                for (const char* key : {"props", "properties"})
                    if (item.contains(key) && item.at(key).is_object())
                        for (auto it = item.at(key).begin(); it != item.at(key).end(); ++it) actor.properties[it.key()] = Encode(it.value());
            }
            void Mesh(Actor& actor, const std::string& mesh)
            {
                actor.properties["StaticMesh"] = Asset("StaticMesh", mesh);
                actor.properties["DrawType"] = "DT_StaticMesh";
                Package(Asset("StaticMesh", mesh));
            }
            void Package(const std::string& path)
            {
                const auto package = PackageOf(path);
                static const std::set<std::string> loaded{"engine", "core", "sbase", "echelon", "editor", "unrealed", "mylevel"};
                if (!package.empty() && !loaded.count(Fold(package))) low.packages.insert(package);
            }
            // A class from the live schema when it is given, so a package the
            // compiler only guessed (Echelon.ERainVolume) takes the loaded one.
            // Entries the bridge could not load ({error}) say nothing about the
            // class, so the search goes on to one that was loaded.
            std::string ClassName(const std::string& cls) const
            {
                const auto& classes = SchemaClasses();
                if (!classes.is_object() || classes.empty()) return cls;
                const auto loaded = [](const Json& entry) { return entry.is_object() && !entry.contains("error"); };
                const auto shortName = Fold(cls.substr(cls.find_last_of('.') + 1));
                for (auto it = classes.begin(); it != classes.end(); ++it)
                    if (loaded(it.value()) && Fold(it.key()) == Fold(cls)) return it.value().value("class", it.key());
                for (auto it = classes.begin(); it != classes.end(); ++it)
                {
                    if (!loaded(it.value())) continue;
                    const auto full = it.value().value("class", it.key());
                    if (Fold(full.substr(full.find_last_of('.') + 1)) == shortName) return full;
                }
                return cls;
            }
            const Json& SchemaClasses() const
            {
                static const Json empty = Json::object();
                if (!options.schema.is_object()) return empty;
                if (options.schema.contains("classes") && options.schema.at("classes").is_object()) return options.schema.at("classes");
                return options.schema;
            }
            const Json* PropertySchema(const std::string& cls, const std::string& property) const
            {
                const auto& classes = SchemaClasses();
                for (auto it = classes.begin(); it != classes.end(); ++it)
                {
                    if (!it.value().is_object() || it.value().contains("error")) continue;
                    if (Fold(it.value().value("class", it.key())) != Fold(cls) && Fold(it.key()) != Fold(cls)) continue;
                    const auto& schema = it.value().contains("schema") ? it.value().at("schema") : it.value();
                    if (!schema.is_object()) return nullptr;
                    for (auto p = schema.begin(); p != schema.end(); ++p) if (Fold(p.key()) == Fold(property)) return &p.value();
                    return nullptr;
                }
                return nullptr;
            }
            bool ClassKnown(const std::string& cls) const
            {
                const auto& classes = SchemaClasses();
                for (auto it = classes.begin(); it != classes.end(); ++it)
                    if (it.value().is_object() && !it.value().contains("error") && (Fold(it.value().value("class", it.key())) == Fold(cls) || Fold(it.key()) == Fold(cls))) return true;
                return false;
            }
            size_t Dimension(const std::string& cls, const std::string& property, size_t fallback) const
            {
                if (const auto* schema = PropertySchema(cls, property); schema && schema->value("kind", std::string()) == "FixedArray") return schema->value("dimension", fallback);
                return fallback;
            }
            // A value completed against the live schema: structs gain the fields
            // left out (at their defaults), fixed arrays their full length, and a
            // rotation given as a struct for an int property becomes its yaw.
            Json Complete(const Json& value, const Json& schema, const std::string& where, const std::string& pointer)
            {
                const auto kind = schema.value("kind", std::string());
                if (kind == "StructProperty" && value.is_object() && !value.contains("$ref") && schema.contains("fields"))
                {
                    Json out = Json::object();
                    for (auto it = schema.at("fields").begin(); it != schema.at("fields").end(); ++it)
                    {
                        auto given = value.find(it.key());
                        out[it.key()] = given == value.end() ? Workflow::Magic::Default(it.value()) : Complete(*given, it.value(), where + "." + it.key(), pointer);
                    }
                    for (auto it = value.begin(); it != value.end(); ++it)
                        if (!schema.at("fields").contains(it.key())) Diag("warning", "schema.field", pointer, where + " has no field " + it.key() + " in the loaded class; it is left out.");
                    return out;
                }
                if ((kind == "FixedArray" || kind == "ArrayProperty") && value.is_array() && schema.contains("inner"))
                {
                    Json out = Json::array();
                    for (size_t i = 0; i < value.size(); ++i) out.push_back(Complete(value[i], schema.at("inner"), where + "[" + std::to_string(i) + "]", pointer));
                    if (kind == "FixedArray")
                    {
                        const auto size = schema.value("dimension", out.size());
                        if (out.size() > size) Diag("error", "schema.array", pointer, where + " holds " + std::to_string(out.size()) + " entries but the loaded class has " + std::to_string(size) + ".");
                        while (out.size() < size) out.push_back(Workflow::Magic::Default(schema.at("inner")));
                    }
                    return out;
                }
                if (value.is_object() && !value.contains("$ref") && kind != "StructProperty" && value.contains("Yaw")) return value.at("Yaw");
                if (value.is_string() && schema.contains("choices") && schema.at("choices").is_array() && !schema.at("choices").empty())
                {
                    for (const auto& choice : schema.at("choices")) if (choice.is_string() && Fold(choice.get<std::string>()) == Fold(value.get<std::string>())) return choice;
                    std::string list;
                    for (const auto& choice : schema.at("choices")) list += (list.empty() ? "" : ", ") + choice.get<std::string>();
                    Diag("error", "schema.enum", pointer, where + " is " + value.get<std::string>() + ", which the loaded class does not offer. Use one of " + list + ".");
                }
                return value;
            }
            // A class the schema lacks stays a warning: its package may be one
            // the plan loads first. A property a loaded class's schema lacks is
            // an error, because authoring.schema lists exactly what Import Map
            // from JSON writes and it refuses the rest ("unsupported property");
            // only a new player start's TeamNumber, which the bridge pastes,
            // is written without being listed.
            void CompleteAll()
            {
                if (SchemaClasses().empty()) return;
                for (auto& actor : low.actors)
                {
                    if (!ClassKnown(actor.cls)) { Diag("warning", "schema.class", actor.pointer, "The schema given has no class " + actor.cls + "; its properties are written unchecked."); continue; }
                    const bool start = Fold(actor.cls).find("playerstart") != std::string::npos;
                    for (auto it = actor.properties.begin(); it != actor.properties.end(); ++it)
                    {
                        if (it.key() == "Location" || it.key() == "Rotation" || it.key() == "Tag" || (start && it.key() == "TeamNumber")) continue;
                        const auto* schema = PropertySchema(actor.cls, it.key());
                        if (!schema)
                        {
                            Diag("error", "schema.property", actor.pointer, actor.id + ": " + actor.cls + " has no property " + it.key() + " that Map JSON writes, so the editor would refuse the whole actors document. Remove it, or set it by hand in the editor.");
                            continue;
                        }
                        if (schema->contains("writable") && !schema->value("writable", true) && !schema->value("create", false))
                            Diag("warning", "schema.writable", actor.pointer, actor.cls + "." + it.key() + ": " + schema->value("reason", std::string("Map JSON does not write this property.")));
                        it.value() = Complete(it.value(), *schema, actor.id + "." + it.key(), actor.pointer);
                    }
                }
            }
            std::string Enum(const std::string& cls, const std::string& property, const std::string& wanted, const std::string& hint, const std::string& pointer)
            {
                const auto* schema = PropertySchema(cls, property);
                if (!schema || !schema->contains("choices") || !schema->at("choices").is_array() || schema->at("choices").empty()) return wanted;
                for (const auto& choice : schema->at("choices")) if (Fold(choice.get<std::string>()) == Fold(wanted)) return choice.get<std::string>();
                for (const auto& choice : schema->at("choices")) if (!hint.empty() && Fold(choice.get<std::string>()).find(Fold(hint)) != std::string::npos) return choice.get<std::string>();
                Diag("error", "schema.enum", pointer, cls + "." + property + " has no " + wanted + " in the loaded class.", "game-modes");
                return wanted;
            }

            // --- Geometry --------------------------------------------------------
            void Storeys()
            {
                const auto& list = List(Section({"storeys"}));
                for (size_t i = 0; i < list.size(); ++i) storeyZ[Fold(list[i].at("id").get<std::string>())] = L(list[i].at("z"));
            }
            std::string StoreyAt(double z) const
            {
                for (const auto& item : List(Section({"storeys"})))
                    if (std::abs(L(item.at("z")) - z) < 1) return item.at("id").get<std::string>();
                return {};
            }
            // The storey a height is on: the highest whose floor is not above it.
            std::string StoreyBelow(double z) const
            {
                std::string best;
                double bestZ = 0;
                for (const auto& item : List(Section({"storeys"})))
                {
                    const double at = L(item.at("z"));
                    if (at <= z + 1 && (best.empty() || at > bestZ)) { best = item.at("id").get<std::string>(); bestZ = at; }
                }
                return best;
            }
            static Design::Extent Interior(const Json& spec, const Pose& pose)
            {
                const double w = spec.at("width"), l = spec.at("length"), h = spec.at("height");
                std::vector<Vector> corners;
                for (int c = 0; c < 8; ++c) corners.push_back(Workflow::TransformPoint({c & 1 ? w / 2 : -w / 2, c & 2 ? l / 2 : -l / 2, c & 4 ? h : 0.0}, pose));
                return Design::Bounds(corners);
            }
            static bool SpaceKind(const std::string& kind) { return kind == "Room" || kind == "Corridor" || kind == "Vent" || kind == "Crawlway"; }
            Json PieceSpec(const std::string& kind, double w, double l, double h, double t, const std::string& name, const Json& item)
            {
                Json spec{{"kind", kind}, {"construction", item.value("construction", Section({"defaults"}).value("construction", std::string("Carve")))},
                          {"width", w}, {"length", l}, {"height", h}, {"thickness", t}, {"steps", 8}, {"ceiling", item.value("ceiling", kind == "Room")},
                          {"portal", item.value("portal", false)}, {"name", name}};
                if (item.contains("steps")) spec["steps"] = item.at("steps");
                else if (Design::StairKind(kind)) Design::RecountSteps(spec);
                std::string material = item.value("material", Section({"defaults"}).value("material", std::string()));
                if (!material.empty()) { spec["material"] = material; Package(material); }
                return spec;
            }
            size_t AddPiece(Piece piece)
            {
                piece.pose.position = Snap(piece.pose.position);
                piece.space = SpaceKind(piece.kind);
                piece.box = piece.space ? Interior(piece.spec, piece.pose) : Design::PieceExtent(piece.spec, piece.pose);
                if (piece.storey.empty()) piece.storey = StoreyAt(piece.pose.position[2]);
                pieceIndex[Fold(piece.id)] = low.pieces.size();
                if (!pointerOf.count(Fold(piece.id))) pointerOf[Fold(piece.id)] = piece.pointer;
                usedIds.insert(Fold(piece.id));
                low.pieces.push_back(std::move(piece));
                return low.pieces.size() - 1;
            }
            void Pieces()
            {
                const auto& list = List(Section({"geometry", "pieces"}));
                std::map<std::string, size_t> byId;
                for (size_t i = 0; i < list.size(); ++i) byId[Fold(list[i].at("id").get<std::string>())] = i;
                std::vector<int> state(list.size(), 0);
                std::function<void(size_t)> place = [&](size_t i)
                {
                    if (state[i] == 2) return;
                    const auto pointer = Child("/geometry/pieces", i);
                    if (state[i] == 1) throw SpecError(Child(pointer, "attach"), "pieces attach to each other in a loop.");
                    state[i] = 1;
                    const auto& item = list[i];
                    if (item.contains("attach"))
                    {
                        const auto host = item.at("attach").at("to").get<std::string>();
                        const auto found = byId.find(Fold(host));
                        if (found == byId.end()) throw SpecError(Child(Child(pointer, "attach"), "to"), "no piece has the id \"" + host + "\".");
                        place(found->second);
                    }
                    PlacePiece(item, pointer);
                    state[i] = 2;
                };
                for (size_t i = 0; i < list.size(); ++i) place(i);
            }
            void PlacePiece(const Json& item, const std::string& pointer)
            {
                const auto id = item.at("id").get<std::string>();
                const auto kind = item.at("kind").get<std::string>();
                const auto& size = item.at("size");
                double w = L(size[0]), l = L(size[1]);
                const double h = L(size[2]);
                if (kind == "Spiral" && std::abs(w - l) > 1e-6)
                {
                    Diag("warning", "geometry.spiral", Child(pointer, "size"), "A spiral is round: its length is set to its width.");
                    l = w;
                }
                const double t = item.contains("thickness") ? L(item.at("thickness")) : L(Section({"defaults"}).value("thickness", Json(16)));
                Piece piece;
                piece.id = id;
                piece.kind = kind;
                piece.pointer = pointer;
                piece.storey = item.value("storey", std::string());
                piece.spec = PieceSpec(kind, w, l, h, t, item.value("name", id), item);
                Rotation rotation{0, YawUnits(item.value("yaw", 0.0)), 0};
                Vector position{0, 0, 0};
                if (!piece.storey.empty())
                {
                    const auto found = storeyZ.find(Fold(piece.storey));
                    if (found == storeyZ.end()) throw SpecError(Child(pointer, "storey"), "no storey has the id \"" + piece.storey + "\".");
                    position[2] = found->second;
                }
                if (item.contains("at"))
                {
                    const auto& at = item.at("at");
                    if (!at[0].is_number() || !at[1].is_number() || (at.size() == 3 && !at[2].is_number())) throw SpecError(Child(pointer, "at"), "a piece stands at [x, y] or [x, y, z] in numbers.");
                    position[0] = L(at[0]);
                    position[1] = L(at[1]);
                    if (at.size() == 3) position[2] = L(at[2]);
                }
                try
                {
                    Design::Geometry(piece.spec);
                    if (item.contains("attach"))
                    {
                        const auto& attach = item.at("attach");
                        const int hostIndex = FindPiece(attach.at("to").get<std::string>());
                        if (hostIndex < 0) { Diag("error", "geometry.attach", Child(Child(pointer, "attach"), "to"), id + " attaches to " + attach.at("to").get<std::string>() + ", which could not be placed."); return; }
                        const auto& host = low.pieces[static_cast<size_t>(hostIndex)];
                        std::string wall;
                        double along = 1;
                        if (!CompassWall(host.pose.rotation, attach.at("side").get<std::string>(), wall, along))
                        {
                            Diag("error", "geometry.attach", Child(pointer, "attach"), host.id + " is not turned a multiple of 90 degrees, so its walls do not face the compass. Place " + id + " with at instead.");
                            return;
                        }
                        const auto pose = Design::AttachedPiece(host.spec, host.pose, piece.spec, wall, attach.contains("offset") ? L(attach.at("offset")) * along : 0.0);
                        position = pose.position;
                        rotation = pose.rotation;
                        if (item.contains("yaw"))
                        {
                            const auto extra = YawUnits(item.at("yaw").get<double>());
                            if (extra % 16384) { Diag("error", "geometry.attach", Child(pointer, "yaw"), "an attached piece turns by multiples of 90 degrees only."); return; }
                            rotation[1] = (rotation[1] + extra) % 65536;
                        }
                        if (piece.storey.empty()) piece.storey = host.storey;
                    }
                }
                catch (const SpecError&) { throw; }
                catch (const std::exception& e)
                {
                    Diag("error", "geometry.piece", pointer, id + ": " + e.what());
                    return;
                }
                piece.pose = {position, rotation};
                AddPiece(std::move(piece));
            }
            // Doorways cut through a host's wall (WallDoorway), on a compass side
            // or between two pieces that share a wall.
            void Doorways()
            {
                const auto& list = List(Section({"geometry", "doorways"}));
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& item = list[i];
                    const auto pointer = Child("/geometry/doorways", i);
                    const auto id = item.at("id").get<std::string>();
                    double w = 96, h = 128;
                    if (item.contains("size")) { w = L(item.at("size")[0]); h = L(item.at("size")[1]); }
                    std::string hostId, otherId, side;
                    double offset = item.contains("offset") ? L(item.at("offset")) : 0.0, sill = item.contains("sill") ? L(item.at("sill")) : 0.0, gap = -1;
                    bool sillGiven = item.contains("sill");
                    if (item.contains("between"))
                    {
                        hostId = item.at("between")[0].get<std::string>();
                        otherId = item.at("between")[1].get<std::string>();
                        const int a = FindPiece(hostId), b = FindPiece(otherId);
                        if (a < 0 || b < 0) { Diag("error", "geometry.doorway", Child(pointer, "between"), "between names pieces that are not in geometry.pieces."); continue; }
                        const auto& A = low.pieces[static_cast<size_t>(a)];
                        const auto& B = low.pieces[static_cast<size_t>(b)];
                        if (!A.space || !B.space) { Diag("error", "geometry.doorway", Child(pointer, "between"), "a doorway joins rooms, corridors or vents."); continue; }
                        double bestGap = 1e18, bestOffset = 0;
                        for (int k = 0; k < 2; ++k)
                        {
                            const int o = 1 - k;
                            const double lo = std::max(A.box.lo[o], B.box.lo[o]), hi = std::min(A.box.hi[o], B.box.hi[o]);
                            if (hi - lo < w - 1e-6) continue;
                            const double gaps[2] = {B.box.lo[k] - A.box.hi[k], A.box.lo[k] - B.box.hi[k]};
                            for (int s = 0; s < 2; ++s)
                                if (gaps[s] >= -1 && gaps[s] <= 512 && gaps[s] < bestGap)
                                {
                                    bestGap = gaps[s];
                                    side = k == 0 ? (s == 0 ? "east" : "west") : (s == 0 ? "north" : "south");
                                    bestOffset = (lo + hi) / 2 - A.pose.position[o];
                                }
                        }
                        if (side.empty()) { Diag("error", "geometry.doorway", Child(pointer, "between"), hostId + " and " + otherId + " share no wall at least " + Num(w) + " units long with a gap of 512 units or less between them."); continue; }
                        gap = std::max(0.0, bestGap);
                        if (!item.contains("offset")) offset = bestOffset;
                        if (!sillGiven) sill = std::max(0.0, B.box.lo[2] - A.box.lo[2]);
                    }
                    else if (item.contains("host") && item.contains("side"))
                    {
                        hostId = item.at("host").get<std::string>();
                        side = item.at("side").get<std::string>();
                    }
                    else { Diag("error", "geometry.doorway", pointer, "a doorway needs between [a, b], or host and side."); continue; }
                    const int hostIndex = FindPiece(hostId);
                    if (hostIndex < 0) { Diag("error", "geometry.doorway", Child(pointer, "host"), "no piece has the id \"" + hostId + "\"."); continue; }
                    const auto host = low.pieces[static_cast<size_t>(hostIndex)];
                    std::string wall;
                    double along = 1;
                    if (!CompassWall(host.pose.rotation, side, wall, along)) { Diag("error", "geometry.doorway", pointer, host.id + " is not turned a multiple of 90 degrees, so its walls do not face the compass."); continue; }
                    const double t = host.spec.value("thickness", 16.0);
                    const double depth = item.contains("depth") ? L(item.at("depth")) : std::max(t, gap);
                    Piece piece;
                    piece.id = id;
                    piece.kind = "Doorway";
                    piece.pointer = pointer;
                    piece.storey = host.storey;
                    piece.spec = {{"kind", "Doorway"}, {"construction", "Carve"}, {"width", w}, {"length", depth}, {"height", h}, {"thickness", t}, {"steps", 8},
                                  {"ceiling", false}, {"portal", true}, {"name", item.value("name", id)}};
                    try { piece.pose = Design::WallDoorway(host.spec, host.pose, piece.spec, wall, offset * along, sill); }
                    catch (const std::exception& e) { Diag("error", "geometry.doorway", pointer, id + ": " + e.what()); continue; }
                    piece.joins[0] = host.id;
                    piece.joins[1] = otherId.empty() ? Beyond(piece, host.id) : otherId;
                    if (item.contains("portal")) piece.spec["portal"] = item.at("portal").get<bool>();
                    else if (!zoneOf.empty())
                    {
                        const auto za = ZoneOfPiece(piece.joins[0]), zb = ZoneOfPiece(piece.joins[1]);
                        piece.spec["portal"] = !za.empty() && !zb.empty() && Fold(za) != Fold(zb) && PortalsWanted(za, zb);
                    }
                    AddPiece(std::move(piece));
                }
            }
            // The space on the far side of a doorway from its host.
            std::string Beyond(const Piece& doorway, const std::string& hostId) const
            {
                const double depth = doorway.spec.at("length"), h = doorway.spec.at("height");
                for (double s : {1.0, -1.0})
                {
                    const auto probe = Workflow::TransformPoint({0, s * (depth / 2 + 8), h / 2}, doorway.pose);
                    for (const auto& p : low.pieces)
                        if (p.space && Fold(p.id) != Fold(hostId) && probe[0] >= p.box.lo[0] && probe[0] <= p.box.hi[0] && probe[1] >= p.box.lo[1] && probe[1] <= p.box.hi[1] &&
                            probe[2] >= p.box.lo[2] && probe[2] <= p.box.hi[2])
                            return p.id;
                }
                return {};
            }
            std::string ZoneOfPiece(const std::string& id) const
            {
                const auto found = zoneOf.find(Fold(id));
                return found == zoneOf.end() ? std::string() : found->second;
            }
            bool PortalsWanted(const std::string& a, const std::string& b) const
            {
                const auto wanted = [&](const std::string& zone) { const auto found = zonePortals.find(Fold(zone)); return found == zonePortals.end() || found->second; };
                return wanted(a) && wanted(b);
            }
            // Stairs and lifts between storeys, built as the Map Design floor tools
            // build them: a stairwell cut to the stairs' footprint, or a shaft and
            // a lift platform that rises when stood on.
            void Vertical()
            {
                const auto& list = List(Section({"geometry", "vertical"}));
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& item = list[i];
                    const auto pointer = Child("/geometry/vertical", i);
                    const auto from = item.at("from").get<std::string>(), to = item.at("to").get<std::string>();
                    const auto a = storeyZ.find(Fold(from)), b = storeyZ.find(Fold(to));
                    if (a == storeyZ.end() || b == storeyZ.end()) { Diag("error", "geometry.vertical", pointer, "from and to name storeys in the storeys list."); continue; }
                    const double base = std::min(a->second, b->second), spacing = std::abs(b->second - a->second);
                    if (spacing < 32) { Diag("error", "geometry.vertical", pointer, "the two storeys are less than 32 units apart."); continue; }
                    Where where;
                    try { where = Locate(item.at("at"), Child(pointer, "at"), Floor(0)); }
                    catch (const SpecError& e) { Diag("error", "geometry.vertical", e.pointer, e.what()); continue; }
                    const Vector at{where.at[0], where.at[1], base};
                    const auto kind = item.at("kind").get<std::string>();
                    const auto storey = a->second <= b->second ? from : to;
                    if (kind == "stairs")
                    {
                        const auto shape = item.value("shape", std::string("Stairs"));
                        const auto id = item.contains("id") ? item.at("id").get<std::string>() : NewId("Stairs");
                        const int steps = std::max(2, static_cast<int>(std::ceil(spacing / 16)));
                        const double width = item.contains("width") ? L(item.at("width")) : 128;
                        Json stairs{{"kind", shape}, {"construction", "Carve"}, {"width", width}, {"height", spacing}, {"thickness", 16}, {"steps", steps}, {"ceiling", false}, {"portal", false}, {"name", "Stairs up"}};
                        double shaftWidth = width, shaftLength = steps * 32.0, shaftX = 0;
                        if (shape == "Stairs") stairs["length"] = steps * 32.0;
                        else if (shape == "Stairs L" || shape == "Stairs U")
                        {
                            const int n1 = (steps + 1) / 2;
                            const double run = n1 * 32.0;
                            stairs["length"] = run + width;
                            shaftLength = run + width;
                            if (shape == "Stairs L") { shaftWidth = width + run; shaftX = run / 2; }
                            else shaftWidth = 2 * width;
                        }
                        else
                        {
                            stairs["width"] = 384;
                            stairs["length"] = 384;
                            stairs["steps"] = std::max(steps, 12);
                            shaftWidth = 384 + 48;
                            shaftLength = 384 + 48;
                        }
                        Json well{{"kind", "Room"}, {"construction", "Carve"}, {"width", shaftWidth}, {"length", shaftLength}, {"height", spacing + 8}, {"thickness", 16}, {"steps", 8}, {"ceiling", true}, {"portal", false}, {"name", "Stairwell"}};
                        Piece shaft;
                        shaft.id = NewId(id + "_Well");
                        shaft.kind = "Room";
                        shaft.pointer = pointer;
                        shaft.storey = storey;
                        shaft.generated = true;
                        shaft.spec = well;
                        shaft.pose = {{at[0] + shaftX, at[1], base}, {}};
                        AddPiece(std::move(shaft));
                        Piece flight;
                        flight.id = id;
                        flight.kind = shape;
                        flight.pointer = pointer;
                        flight.storey = storey;
                        flight.generated = true;
                        flight.spec = stairs;
                        flight.pose = {at, {}};
                        try { Design::Geometry(stairs); }
                        catch (const std::exception& e) { Diag("error", "geometry.vertical", pointer, e.what()); continue; }
                        AddPiece(std::move(flight));
                    }
                    else
                    {
                        const auto id = item.contains("id") ? item.at("id").get<std::string>() : NewId("Lift");
                        Piece shaft;
                        shaft.id = NewId(id + "_Shaft");
                        shaft.kind = "Room";
                        shaft.pointer = pointer;
                        shaft.storey = storey;
                        shaft.generated = true;
                        shaft.spec = {{"kind", "Room"}, {"construction", "Carve"}, {"width", 208}, {"length", 208}, {"height", spacing + 24}, {"thickness", 16}, {"steps", 8}, {"ceiling", true}, {"portal", false}, {"name", "Lift shaft"}};
                        shaft.pose = {{at[0], at[1], base - 16}, {}};
                        AddPiece(std::move(shaft));
                        const double moveTime = std::clamp(spacing / 160.0, 1.5, 8.0);
                        usedIds.insert(Fold(id));
                        pointerOf[Fold(id)] = pointer;
                        low.lifts.push_back({id, pointer, {{"position", Point({at[0], at[1], base - 8})}, {"width", 192}, {"length", 192}, {"thickness", 16}, {"rise", spacing}, {"moveTime", moveTime}}, {at[0], at[1], base}});
                    }
                }
            }
            void Zones()
            {
                const auto& list = List(Section({"zones"}));
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& zone = list[i];
                    const auto id = zone.at("id").get<std::string>();
                    zonePortals[Fold(id)] = zone.value("portals", true);
                    const auto& spaces = zone.at("spaces");
                    for (size_t k = 0; k < spaces.size(); ++k)
                    {
                        const auto space = spaces[k].get<std::string>();
                        const auto pointer = Child(Child(Child("/zones", i), "spaces"), k);
                        const int index = FindPiece(space);
                        if (index < 0) { Diag("error", "zones.space", pointer, "no geometry piece has the id \"" + space + "\".", "zones-and-volumes"); continue; }
                        if (!low.pieces[static_cast<size_t>(index)].space) { Diag("error", "zones.space", pointer, space + " is not a room, corridor or vent, so it holds no zone.", "zones-and-volumes"); continue; }
                        if (zoneOf.count(Fold(space))) { Diag("error", "zones.space", pointer, space + " is already in zone " + zoneOf.at(Fold(space)) + "; a space belongs to one zone.", "zones-and-volumes"); continue; }
                        zoneOf[Fold(space)] = id;
                        low.pieces[static_cast<size_t>(index)].zone = id;
                    }
                }
                if (!list.empty())
                    for (const auto& p : low.pieces)
                        if (p.space && !zoneOf.count(Fold(p.id)))
                            Diag("warning", "zones.unzoned", p.pointer, p.id + " is in no zone, so the HUD shows the location of whichever zone it falls into.", "zones-and-volumes");
            }
            // A corridor, vent or crawlway is open at its ends; where an end opens
            // into a space of another zone, a portal doorway seals the zone there.
            void ZonePortals()
            {
                if (zoneOf.empty()) return;
                std::set<std::string> done;
                const size_t count = low.pieces.size();
                for (size_t i = 0; i < count; ++i)
                {
                    const auto p = low.pieces[i];
                    if (!p.space || p.kind == "Room" || p.zone.empty()) continue;
                    const double w = p.spec.at("width"), l = p.spec.at("length"), h = p.spec.at("height"), t = p.spec.value("thickness", 16.0);
                    for (double s : {1.0, -1.0})
                    {
                        const auto probe = Workflow::TransformPoint({0, s * (l / 2 + t + 4), h / 2}, p.pose);
                        for (size_t j = 0; j < count; ++j)
                        {
                            const auto& other = low.pieces[j];
                            if (j == i || !other.space || probe[0] < other.box.lo[0] || probe[0] > other.box.hi[0] || probe[1] < other.box.lo[1] || probe[1] > other.box.hi[1] ||
                                probe[2] < other.box.lo[2] || probe[2] > other.box.hi[2])
                                continue;
                            if (other.zone.empty() || Fold(other.zone) == Fold(p.zone) || !PortalsWanted(p.zone, other.zone)) continue;
                            const auto key = Fold(std::min(p.id, other.id)) + "|" + Fold(std::max(p.id, other.id));
                            if (!done.insert(key + (s > 0 ? "+" : "-")).second) continue;
                            Piece portal;
                            portal.id = NewId(p.id + "_Portal");
                            portal.kind = "Doorway";
                            portal.pointer = p.pointer;
                            portal.storey = p.storey;
                            portal.generated = true;
                            portal.spec = {{"kind", "Doorway"}, {"construction", "Carve"}, {"width", w}, {"length", t}, {"height", h}, {"thickness", t}, {"steps", 8},
                                           {"ceiling", false}, {"portal", true}, {"name", p.spec.value("name", p.id) + " portal"}};
                            portal.pose = {Workflow::TransformPoint({0, s * (l / 2 + t / 2), 0}, p.pose), p.pose.rotation};
                            portal.joins[0] = p.id;
                            portal.joins[1] = other.id;
                            AddPiece(std::move(portal));
                            break;
                        }
                    }
                }
            }
            void ZoneInfos()
            {
                const auto& list = List(Section({"zones"}));
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& zone = list[i];
                    const auto id = zone.at("id").get<std::string>();
                    const int index = FindPiece(zone.at("spaces")[0].get<std::string>());
                    if (index < 0) continue;
                    const auto& p = low.pieces[static_cast<size_t>(index)];
                    const Vector at{p.pose.position[0], p.pose.position[1], p.box.lo[2] + std::min(128.0, (p.box.hi[2] - p.box.lo[2]) / 2)};
                    auto& info = Add(id, "Engine.ZoneInfo", "other", Child("/zones", i), at, 0, false);
                    const auto name = zone.value("locationName", id);
                    info.properties["LocationName"] = Quoted(name);
                    info.label = name;
                    Json spaces = Json::array();
                    for (const auto& s : zone.at("spaces")) spaces.push_back(s);
                    low.zones.push_back({{"id", id}, {"locationName", name}, {"spaces", spaces}});
                    Vector lo = p.box.lo, hi = p.box.hi;
                    for (const auto& s : zone.at("spaces"))
                        if (const int k = FindPiece(s.get<std::string>()); k >= 0)
                            for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], low.pieces[static_cast<size_t>(k)].box.lo[a]); hi[a] = std::max(hi[a], low.pieces[static_cast<size_t>(k)].box.hi[a]); }
                    low.labels.push_back({{"at", Point({(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]})}, {"text", name}, {"kind", "zone"}});
                }
            }
            // Polygon brushes as written, and every piece as polygon brushes when
            // brushMode is "polygons".
            void Brushes()
            {
                const auto& list = List(Section({"geometry", "brushes"}));
                std::map<std::string, size_t> brushes;
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& item = list[i];
                    const auto pointer = Child("/geometry/brushes", i);
                    Vector at{0, 0, 0};
                    if (item.contains("at"))
                    {
                        const auto& p = item.at("at");
                        if (!p[0].is_number() || !p[1].is_number() || (p.size() == 3 && !p[2].is_number())) throw SpecError(Child(pointer, "at"), "a brush stands at [x, y] or [x, y, z] in numbers.");
                        at = {L(p[0]), L(p[1]), p.size() == 3 ? L(p[2]) : 0.0};
                    }
                    auto& actor = Add(item.at("id").get<std::string>(), FullClass(item.value("class", std::string("Engine.Brush"))), "other", pointer, at, 0, false);
                    actor.geometry = "polygons";
                    actor.footprint = true;
                    actor.polygons = Json::array();
                    for (const auto& polygon : item.at("polygons"))
                    {
                        Json out = polygon;
                        out["vertices"] = Json::array();
                        for (const auto& v : polygon.at("vertices")) out["vertices"].push_back(Json::array({L(v[0]), L(v[1]), L(v[2])}));
                        if (polygon.contains("origin")) out["origin"] = Json::array({L(polygon.at("origin")[0]), L(polygon.at("origin")[1]), L(polygon.at("origin")[2])});
                        if (polygon.contains("texture")) Package(polygon.at("texture").get<std::string>());
                        actor.polygons.push_back(out);
                    }
                    double bottom = 1e18;
                    for (const auto& polygon : actor.polygons) for (const auto& v : polygon.at("vertices")) bottom = std::min(bottom, v[2].get<double>() + at[2]);
                    actor.storey = StoreyBelow(bottom);
                    actor.properties["CsgOper"] = item.value("csg", std::string("CSG_Add"));
                    if (item.contains("polyFlags")) actor.properties["PolyFlags"] = std::to_string(item.at("polyFlags").get<long long>());
                    Props(actor, item);
                    brushes[Fold(actor.id)] = low.actors.size() - 1;
                }
                std::map<std::string, std::vector<size_t>> pieceBrushes;
                if (options.brushMode == "polygons")
                    for (const auto& p : low.pieces)
                    {
                        std::vector<Design::Solid> solids;
                        try { solids = Design::Geometry(p.spec); }
                        catch (const std::exception&) { continue; }
                        const auto material = p.spec.value("material", std::string());
                        for (size_t k = 0; k < solids.size(); ++k)
                        {
                            std::vector<Design::Face> faces;
                            for (const auto& face : solids[k].faces)
                            {
                                Design::Face turned;
                                for (const auto& v : face) turned.push_back(p.pose.rotation == Rotation{} ? v : Workflow::TransformPoint(v, {{}, p.pose.rotation}));
                                faces.push_back(turned);
                            }
                            auto& actor = Add(NewId(p.id + "_Brush" + std::to_string(k)), "Engine.Brush", "other", p.pointer, p.pose.position, 0, false);
                            actor.geometry = "polygons";
                            actor.polygons = Polygons(faces, material, solids[k].flags);
                            actor.properties["CsgOper"] = solids[k].subtract ? "CSG_Subtract" : "CSG_Add";
                            if (solids[k].flags) actor.properties["PolyFlags"] = std::to_string(solids[k].flags);
                            pieceBrushes[Fold(p.id)].push_back(low.actors.size() - 1);
                        }
                    }
                const auto& surfaces = List(Section({"geometry", "surfaces"}));
                for (size_t i = 0; i < surfaces.size(); ++i)
                {
                    const auto& item = surfaces[i];
                    const auto pointer = Child("/geometry/surfaces", i);
                    const auto target = item.at("brush").get<std::string>();
                    const auto texture = item.at("texture").get<std::string>();
                    Package(texture);
                    std::vector<size_t> owners;
                    if (const auto found = brushes.find(Fold(target)); found != brushes.end()) owners.push_back(found->second);
                    else if (const auto piece = pieceBrushes.find(Fold(target)); piece != pieceBrushes.end()) owners = piece->second;
                    else if (FindPiece(target) >= 0)
                    {
                        // A Map Design piece is retextured after the build, by face.
                        low.surfaces.push_back({{"piece", target}, {"faces", item.at("faces")}, {"texture", texture}, {"pointer", pointer}});
                        continue;
                    }
                    else { Diag("error", "geometry.surface", Child(pointer, "brush"), "no brush or piece has the id \"" + target + "\"."); continue; }
                    std::set<std::string> faces;
                    for (const auto& f : item.at("faces")) faces.insert(f.get<std::string>());
                    size_t painted = 0;
                    for (auto owner : owners)
                    {
                        auto& actor = low.actors[owner];
                        const bool subtract = actor.properties.value("CsgOper", std::string()) == "CSG_Subtract";
                        for (auto& polygon : actor.polygons)
                            if (faces.count(FaceName(polygon.at("vertices"), subtract))) { polygon["texture"] = texture; ++painted; }
                    }
                    if (!painted) Diag("warning", "geometry.surface", pointer, "no face of " + target + " points that way.");
                }
            }

            // --- Spawns ----------------------------------------------------------
            // Starts on a lattice centred in each room, floor+48, clear of the walls.
            std::vector<Vector> Lattice(const Piece& room, int count, double spacing, double above)
            {
                std::vector<Vector> out;
                if (count < 1) return out;
                const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count)))), rows = (count + cols - 1) / cols;
                const double cx = (room.box.lo[0] + room.box.hi[0]) / 2, cy = (room.box.lo[1] + room.box.hi[1]) / 2;
                const double margin = 40;
                double sx = spacing, sy = spacing;
                const double spanX = room.box.hi[0] - room.box.lo[0] - 2 * margin, spanY = room.box.hi[1] - room.box.lo[1] - 2 * margin;
                if (cols > 1 && sx * (cols - 1) > spanX) sx = std::max(0.0, spanX / (cols - 1));
                if (rows > 1 && sy * (rows - 1) > spanY) sy = std::max(0.0, spanY / (rows - 1));
                for (int n = 0; n < count; ++n)
                {
                    const int c = n % cols, r = n / cols;
                    out.push_back({cx + (c - (cols - 1) / 2.0) * sx, cy + (r - (rows - 1) / 2.0) * sy, room.box.lo[2] + above});
                }
                return out;
            }
            void Spawns()
            {
                const char* teams[2] = {"spy", "merc"};
                for (int team = 0; team < 2; ++team)
                {
                    const auto& group = Section({"spawns", teams[team]});
                    if (group.is_null()) continue;
                    const auto pointer = std::string("/spawns/") + teams[team];
                    const std::string stem = team == 0 ? "SpyStart" : "MercStart";
                    auto make = [&](const std::string& id, const Vector& at, int yaw, const std::string& where)
                    {
                        auto& start = Add(id, "Engine.PlayerStart", team == 0 ? "spawn-spy" : "spawn-merc", where, at, yaw);
                        start.properties["TeamNumber"] = team == 0 ? "0" : "1";
                    };
                    if (group.is_array())
                    {
                        for (size_t i = 0; i < group.size(); ++i)
                        {
                            const auto where = Child(pointer, i);
                            const auto at = Locate(group[i].at("at"), Child(where, "at"), Floor(48)).at;
                            int fallback = 0;
                            try { fallback = Workflow::Security::YawTowards(at, Centre()); } catch (const std::exception&) {}
                            make(group[i].at("id").get<std::string>(), at, YawOf(group[i], at, where, fallback), where);
                        }
                        continue;
                    }
                    const auto rooms = Strings(group.at("in"));
                    const int count = group.value("count", Section({"defaults"}).value("startsPerTeam", 4));
                    const double spacing = group.contains("spacing") ? L(group.at("spacing")) : 128;
                    const auto centre = Centre();
                    int made = 0;
                    for (size_t r = 0; r < rooms.size(); ++r)
                    {
                        const int index = FindPiece(rooms[r]);
                        if (index < 0 || !low.pieces[static_cast<size_t>(index)].space) { Diag("error", "spawns.room", Child(pointer, "in"), "\"" + rooms[r] + "\" is not a room, corridor or vent of the spec.", "spawn-points"); continue; }
                        const auto& room = low.pieces[static_cast<size_t>(index)];
                        const int here = count / static_cast<int>(rooms.size()) + (static_cast<int>(r) < count % static_cast<int>(rooms.size()) ? 1 : 0);
                        for (const auto& at : Lattice(room, here, spacing, 48))
                        {
                            int fallback = 0;
                            try { fallback = Workflow::Security::YawTowards(room.pose.position, centre); } catch (const std::exception&) {}
                            make(NewId(stem + std::to_string(++made)), at, group.contains("facing") ? Facing(group.at("facing"), at, Child(pointer, "facing"), fallback) : fallback, pointer);
                        }
                    }
                }
                const auto& toggles = List(Section({"spawns", "toggles"}));
                for (size_t i = 0; i < toggles.size(); ++i)
                {
                    const auto& item = toggles[i];
                    const auto pointer = Child("/spawns/toggles", i);
                    Vector at;
                    if (item.contains("at")) at = Locate(item.at("at"), Child(pointer, "at"), Floor(48)).at;
                    else if (item.contains("in")) at = Locate({{"in", item.at("in")}}, pointer, Floor(48)).at;
                    else { Diag("error", "spawns.toggle", pointer, "a toggled start needs at or in.", "spawn-points"); continue; }
                    const bool spy = item.at("team") == "spy";
                    auto& start = Add(item.at("id").get<std::string>(), "SBase.STriggerPlayerStart", spy ? "spawn-spy" : "spawn-merc", pointer, at, YawOf(item, at, pointer, 0));
                    start.properties["TeamNumber"] = spy ? "0" : "1";
                    if (item.contains("enabledBy")) start.properties["Tag"] = item.at("enabledBy").get<std::string>();
                    start.label = start.id + " (toggled)";
                }
            }

            // --- Story: objectives and their triggers --------------------------------
            void StoryTriggers()
            {
                const auto& objectives = List(Section({"gameplay", "story", "objectives"}));
                for (size_t i = 0; i < objectives.size(); ++i)
                {
                    const auto& objective = objectives[i];
                    const auto pointer = Child("/gameplay/story/objectives", i);
                    const auto id = objective.at("id").get<std::string>();
                    const auto name = objective.value("name", id);
                    const auto& triggers = List(objective.value("triggers", Json::array()));
                    Json links = Json::array();
                    Vector sum{};
                    int placed = 0, room = -1;
                    std::string objKind;
                    for (size_t k = 0; k < triggers.size(); ++k)
                    {
                        const auto& trigger = triggers[k];
                        const auto where = Child(Child(pointer, "triggers"), k);
                        const auto kind = trigger.at("kind").get<std::string>();
                        const auto triggerId = trigger.at("id").get<std::string>();
                        if (!trigger.contains("at")) { Diag("error", "story.trigger", where, triggerId + " needs at.", "game-modes"); continue; }
                        const auto spot = Locate(trigger.at("at"), Child(where, "at"), Floor(kind == "terminal" ? 78 : 64));
                        const auto& cls = kind == "terminal" ? "SBase.SComputerObjectiveTrigger" : kind == "flag" ? "SBase.SFlag" : "SBase.SBombTargetObjectiveTrigger";
                        auto& actor = Add(triggerId, cls, kind == "terminal" ? "objective" : kind, where, spot.at, YawOf(trigger, spot.at, where, 0));
                        actor.label = kind + " " + triggerId;
                        actor.properties["TriggerName"] = Quoted(trigger.value("triggerName", name));
                        if (kind == "terminal")
                        {
                            actor.properties["DominationCaptureTime"] = Num(trigger.value("dominationCaptureTime", 10.0));
                            if (trigger.contains("captureTime")) Diag("warning", "story.capture-time", Child(where, "captureTime"), "CaptureTime does not affect a computer terminal; only dominationCaptureTime is written.", "game-modes");
                            const bool explicitZ = trigger.at("at").is_array() && trigger.at("at").size() == 3 && trigger.at("at")[2].is_number();
                            if (explicitZ && spot.room >= 0)
                            {
                                const double above = spot.at[2] - low.pieces[static_cast<size_t>(spot.room)].box.lo[2];
                                if (std::abs(above - 78) > 8) Diag("warning", "story.terminal-height", Child(where, "at"), triggerId + " stands " + Num(above) + " units above the floor; terminals are captured at about 78.", "game-modes");
                            }
                        }
                        else if (kind == "flag")
                        {
                            actor.properties["ResetTime"] = Num(trigger.value("resetTime", 30.0));
                            const auto& zones = List(trigger.value("dropZones", Json::array()));
                            if (zones.empty() || zones.size() > 5) Diag("error", "story.flag-dropzones", Child(where, "dropZones"), triggerId + " needs 1 to 5 drop zones.", "game-modes");
                            const auto flagId = actor.id;
                            Json slots = Json::array();
                            for (size_t z = 0; z < zones.size() && z < 5; ++z)
                            {
                                const auto zoneWhere = Child(Child(where, "dropZones"), z);
                                // {ref} shares a zone another entry made: flags may share drop zones.
                                if (zones[z].contains("ref"))
                                {
                                    zoneRefs.push_back({zones[z].at("ref").get<std::string>(), flagId, Child(zoneWhere, "ref"), z});
                                    slots.push_back(Ref(zones[z].at("ref").get<std::string>()));
                                    continue;
                                }
                                const auto zoneId = zones[z].contains("id") ? zones[z].at("id").get<std::string>() : NewId(flagId + "_Drop" + std::to_string(z + 1));
                                const auto zoneAt = Locate(zones[z].at("at"), Child(zoneWhere, "at"), Floor(64)).at;
                                Add(zoneId, "SBase.SFlagDropZone", "dropzone", zoneWhere, zoneAt, 0);
                                slots.push_back(Ref(zoneId));
                            }
                            for (size_t a = 0; a < slots.size(); ++a)
                                for (size_t b = a + 1; b < slots.size(); ++b)
                                    if (Fold(slots[a].at("$ref").get<std::string>()) == Fold(slots[b].at("$ref").get<std::string>()))
                                        Diag("error", "story.flag-dropzones", Child(Child(where, "dropZones"), b), triggerId + " lists the drop zone " + slots[b].at("$ref").get<std::string>() + " twice.", "game-modes");
                            auto* flag = FindActor(flagId);
                            const auto size = Dimension(flag->cls, "DropZone", 5);
                            while (slots.size() < size) slots.push_back("None");
                            flag->properties["DropZone"] = slots;
                        }
                        else actor.properties["CaptureTime"] = Num(trigger.value("captureTime", 30.0));
                        if (auto* made = FindActor(triggerId)) Props(*made, trigger);
                        links.push_back(Ref(triggerId));
                        triggersOf[Fold(id)].push_back({triggerId, kind});
                        triggerKind[Fold(triggerId)] = kind;
                        // Representative kind for the objective's default coaching text (bomb/modem wins, then flag).
                        if (objKind.empty()) objKind = kind;
                        else if (kind == "bomb") objKind = "bomb";
                        else if (kind == "flag" && objKind == "terminal") objKind = "flag";
                        for (int a = 0; a < 3; ++a) sum[a] += spot.at[a];
                        if (room < 0) room = spot.room;
                        ++placed;
                    }
                    if (triggers.empty()) Diag("warning", "story.objective", pointer, id + " has no triggers, so it can never be completed.", "game-modes");
                    Vector at;
                    if (objective.contains("at")) at = Locate(objective.at("at"), Child(pointer, "at"), Floor(64)).at;
                    else if (placed)
                    {
                        at = {sum[0] / placed, sum[1] / placed, 0};
                        const int under = SpaceAt(at[0], at[1], sum[2] / placed, true);
                        at[2] = LevelIn(under >= 0 ? under : room, Floor(64));
                    }
                    else at = Fallback();
                    auto& actor = Add(id, "SBase.SObjective", "objective", pointer, at, 0, false);
                    actor.label = "objective " + name;
                    actor.properties["ObjectiveName"] = Quoted(name);
                    // Default objective text teaches the mechanic in the briefing/HUD when the author gives none.
                    // bomb = the ND133 modem-sabotage objective; flag = hard-disk steal; terminal = neutralize in place.
                    const std::string spyGoal = objKind == "bomb" ? "Plant a modem on " + name + " and fall back while it hacks the ND133."
                                               : objKind == "flag" ? "Steal " + name + " and carry it to a drop zone."
                                               : "Neutralize " + name;
                    const std::string mercGoal = objKind == "bomb" ? "Defend " + name + "; find and remove any modem the spies plant."
                                                : objKind == "flag" ? "Stop the spies stealing " + name + "."
                                                : "Protect " + name;
                    actor.properties["Description"] = Quoted(objective.value("description", spyGoal));
                    actor.properties["DescriptionDEF"] = Quoted(objective.value("descriptionDef", mercGoal));
                    actor.properties["Triggers"] = links;
                    if (objective.contains("visibilityZone"))
                    {
                        const auto zone = objective.at("visibilityZone").get<std::string>();
                        auto* info = FindActor(zone);
                        if (!info || info->cls.find("ZoneInfo") == std::string::npos) Diag("error", "story.visibility-zone", Child(pointer, "visibilityZone"), "\"" + zone + "\" is not a zone of the spec (TriggersERVisibilityZone takes a ZoneInfo).", "game-modes");
                        else actor.properties["TriggersERVisibilityZone"] = Ref(zone);
                    }
                    Props(actor, objective);
                    objectiveOrder.push_back(id);
                    objectivePositions[Fold(id)] = at;
                }
                // Shared drop zones, once every flag has made its own: the slot
                // takes the zone's own spelling of its id, unless props replaced it.
                for (const auto& shared : zoneRefs)
                {
                    const auto* zone = FindActor(shared.ref);
                    if (!zone || zone->cls.find("SFlagDropZone") == std::string::npos)
                    {
                        Diag("error", "story.flag-dropzones", shared.pointer, "\"" + shared.ref + "\" is not a drop zone of the spec; {ref} names the id of another flag's dropZones entry, one with at.", "game-modes");
                        continue;
                    }
                    const auto spelling = zone->id;
                    auto& slots = FindActor(shared.flag)->properties["DropZone"];
                    if (slots.is_array() && shared.slot < slots.size() && slots[shared.slot] == Ref(shared.ref)) slots[shared.slot] = Ref(spelling);
                }
            }
            struct ZoneRef { std::string ref, flag, pointer; size_t slot; };
            std::vector<ZoneRef> zoneRefs;
            // The story mission, and the sectors the Zones window would build:
            // a chained GM_Multi mission holding one GM_Undefined mission per
            // sector, each sector's completion event, announcement and sounds.
            void Story()
            {
                const auto& story = Section({"gameplay", "story"});
                if (story.is_null()) return;
                const auto mission = story.value("mission", Json::object());
                const auto id = mission.contains("id") ? mission.at("id").get<std::string>() : NewId("Mission");
                const auto& sectors = List(story.value("sectors", Json::array()));
                Json objectives = Json::array();
                int minimum = 0;
                Vector sum{};
                for (const auto& o : objectiveOrder) for (int a = 0; a < 3; ++a) sum[a] += objectivePositions[Fold(o)][a];
                if (!sectors.empty())
                {
                    std::map<std::string, std::string> sectorOf;
                    for (size_t n = 1; n <= sectors.size(); ++n)
                    {
                        const auto& sector = sectors[n - 1];
                        const auto pointer = Child("/gameplay/story/sectors", n - 1);
                        const auto sectorId = sector.at("id").get<std::string>();
                        Json members = Json::array();
                        Vector anchor{};
                        int total = 0;
                        for (size_t k = 0; k < sector.at("objectives").size(); ++k)
                        {
                            const auto o = sector.at("objectives")[k].get<std::string>();
                            const auto where = Child(Child(pointer, "objectives"), k);
                            if (!objectivePositions.count(Fold(o))) { Diag("error", "story.sectors", where, "\"" + o + "\" is not one of the story's objectives.", "game-modes"); continue; }
                            if (sectorOf.count(Fold(o))) { Diag("error", "story.sectors", where, o + " is already in sector " + sectorOf.at(Fold(o)) + "; an objective belongs to one sector.", "game-modes"); continue; }
                            sectorOf[Fold(o)] = sectorId;
                            members.push_back(Ref(o));
                            for (int a = 0; a < 3; ++a) anchor[a] += objectivePositions[Fold(o)][a];
                            ++total;
                        }
                        if (total) for (auto& c : anchor) c /= total;
                        else anchor = Fallback();
                        anchor[2] += 96;
                        const int required = sector.value("required", 0);
                        if (required > total) Diag("error", "story.sectors", Child(pointer, "required"), sectorId + " needs " + std::to_string(required) + " completions but holds " + std::to_string(total) + " objectives.", "game-modes");
                        const int needed = required ? std::min(required, total) : total;
                        minimum += needed;
                        const auto& actions = List(sector.value("actions", Json::array()));
                        const auto fires = actions.empty() ? std::string("None") : NewId(Stages::CompleteTag(static_cast<int>(n)));
                        auto& zone = Add(sectorId, "SBase.SMission", "other", pointer, anchor, 0, false);
                        zone.label = "sector " + sector.value("name", sectorId);
                        zone.properties["Objectives"] = members;
                        zone.properties["MinimumObjectives"] = std::to_string(needed);
                        zone.properties["bChained"] = "False";
                        zone.properties["GameMode"] = Enum(zone.cls, "GameMode", "GM_Undefined", "undefined", pointer);
                        zone.properties["ObjectiveName"] = Quoted(sector.value("name", "Zone " + std::to_string(n)));
                        zone.properties["Description"] = Quoted(sector.value("brief", std::string("Complete the objectives")));
                        zone.properties["DescriptionDEF"] = Quoted(sector.value("briefDefend", std::string("Defend the objectives")));
                        zone.properties["Event"] = fires;
                        objectives.push_back(Ref(sectorId));
                        if (!actions.empty()) SectorActions(static_cast<int>(n), fires, actions, anchor, Child(pointer, "actions"));
                    }
                    for (const auto& o : objectiveOrder)
                        if (!sectorOf.count(Fold(o))) Diag("error", "story.sectors", PointerOf(o), o + " is in no sector. With sectors, every objective belongs to one, so the chained mission reaches it.", "game-modes");
                }
                else
                {
                    for (const auto& o : objectiveOrder) objectives.push_back(Ref(o));
                    minimum = static_cast<int>(objectiveOrder.size());
                }
                if (objectives.empty() && mission.empty()) return;
                Vector at;
                if (mission.contains("at")) at = Locate(mission.at("at"), "/gameplay/story/mission/at", Floor(64)).at;
                else if (!objectiveOrder.empty())
                {
                    const auto n = static_cast<double>(objectiveOrder.size());
                    at = {sum[0] / n, sum[1] / n, sum[2] / n};
                }
                else at = Fallback();
                auto& top = Add(id, "SBase.SMission", "other", mission.contains("id") ? "/gameplay/story/mission" : "/gameplay/story", at, 0, false);
                const auto name = mission.value("name", std::string("Mission"));
                top.label = "mission " + name;
                top.properties["ObjectiveName"] = Quoted(name);
                top.properties["Description"] = Quoted(mission.value("description", std::string("Complete the mission")));
                top.properties["DescriptionDEF"] = Quoted(mission.value("descriptionDef", std::string("Defend the objectives")));
                top.properties["GameMode"] = Enum(top.cls, "GameMode", "GM_Multi", "multi", top.pointer);
                top.properties["bChained"] = Flag(!sectors.empty() || mission.value("chained", false));
                top.properties["MinimumObjectives"] = std::to_string(mission.value("minimumObjectives", minimum));
                top.properties["Objectives"] = objectives;
                if (mission.contains("atResetEvent")) top.properties["AtResetEvent"] = mission.at("atResetEvent").get<std::string>();
                if (mission.value("minimumObjectives", minimum) > minimum)
                    Diag("error", "story.minimum", "/gameplay/story/mission/minimumObjectives", "The mission needs " + std::to_string(mission.value("minimumObjectives", minimum)) + " objectives to win but completing everything gives " + std::to_string(minimum) + ".", "game-modes");
                Props(top, mission);
            }
            void SectorActions(int n, const std::string& fires, const Json& actions, const Vector& anchor, const std::string& pointer)
            {
                auto entry = [](const std::string& tag, const std::string& delay, bool untrigger)
                {
                    return Json{{"Delay", delay}, {"Event", tag}, {"Type", untrigger ? "EVT_Untrigger" : "EVT_Trigger"}, {"ValidOn", "EVT_Trigger"}};
                };
                Json entries = Json::array();
                int sounds = 0;
                for (size_t i = 0; i < actions.size(); ++i)
                {
                    const auto& action = actions[i];
                    const auto where = Child(pointer, i);
                    const auto kind = action.at("kind").get<std::string>();
                    const auto delay = Delay(action, where, "story.action", "events");
                    if (delay.empty()) continue;
                    const auto target = action.value("target", std::string());
                    if (kind == "open")
                    {
                        if (target.empty()) { Diag("error", "story.action", where, "an open action needs a target door.", "game-modes"); continue; }
                        entries.push_back(entry(TagOf(target, Child(where, "target"), true), delay, false));
                        // Held open is TriggerToggle, which every other Trigger the
                        // door gets would swap shut again.
                        if (auto* door = FindActor(target); door && action.value("hold", true))
                        {
                            const auto state = Fold(door->properties.value("InitialState", std::string()));
                            if (const auto other = OtherTrigger(door->id); !other.empty())
                                Diag("error", "story.action-door-trigger", where, door->id + " also opens from " + other + ", so held open (TriggerToggle) it would be toggled shut by that. Set hold false on this action (the door opens for its stayOpen time), or give " +
                                     door->id + " trigger {kind: \"event\"} so only the sector opens it, and the other team a door of its own.", "movers");
                            else
                            {
                                if (state.empty() || state.find("timed") != std::string::npos) door->properties["InitialState"] = "TriggerToggle";
                                heldDoors.insert(Fold(door->id));
                            }
                        }
                    }
                    else if (kind == "light")
                    {
                        if (target.empty()) { Diag("error", "story.action", where, "a light action needs a target light.", "game-modes"); continue; }
                        if (auto* light = FindActor(target); light && light->cls == "Engine.Light")
                        {
                            light->cls = ClassName("SBase.STriggerLight");
                            Diag("info", "story.action", where, target + " becomes a switchable STriggerLight so the sector can switch it.", "lighting");
                        }
                        entries.push_back(entry(TagOf(target, Child(where, "target"), true), delay, false));
                    }
                    else if (kind == "sound")
                    {
                        if (action.contains("sound"))
                        {
                            const auto id = NewId(Stages::SoundTag(n, ++sounds));
                            auto& trigger = Add(id, "Engine.SoundTrigger", "sound", where, {anchor[0], anchor[1] + 48.0 * sounds, anchor[2]}, 0, false);
                            trigger.properties["Sound"] = Asset("Sound", action.at("sound").get<std::string>());
                            Package(trigger.properties["Sound"].get<std::string>());
                            entries.push_back(entry(id, delay, false));
                        }
                        else if (!target.empty()) entries.push_back(entry(TagOf(target, Child(where, "target"), true), delay, false));
                        else Diag("error", "story.action", where, "a sound action needs a sound asset or a target sound trigger.", "game-modes");
                    }
                    else if (kind == "announce")
                    {
                        const auto id = NewId(Stages::AnnounceTag(n));
                        auto& alarm = Add(id, "SBase.SAlarm", "alarm", where, {anchor[0], anchor[1] - 48, anchor[2]}, 0, false);
                        alarm.properties["AlarmName"] = Quoted(action.value("title", std::string()));
                        alarm.properties["AlarmDescription"] = Quoted(action.value("merc", std::string()));
                        alarm.properties["AlarmDescriptionSpy"] = Quoted(action.value("spy", std::string()));
                        alarm.properties["Duration"] = std::to_string(static_cast<int>(action.value("seconds", 8.0)));
                        alarm.properties["Events"] = Json::array();
                        entries.push_back(entry(id, delay, action.value("untrigger", false)));
                    }
                    else
                    {
                        const auto tag = !target.empty() ? TagOf(target, Child(where, "target"), true) : action.value("tag", std::string());
                        if (tag.empty()) { Diag("error", "story.action", where, "a trigger action needs a target or a tag.", "game-modes"); continue; }
                        entries.push_back(entry(tag, delay, action.value("untrigger", false)));
                    }
                }
                auto& event = Add(fires, "SBase.SMagicEvent", "other", pointer, {anchor[0] + 48, anchor[1], anchor[2]}, 0, false);
                event.properties["Groups"] = Json::array({{{"EventGroup", entries}, {"Repeat", "0"}, {"Sequence", "False"}, {"SequenceIndex", "0"}}});
                magicIds.insert(Fold(fires));
            }
            // Disks spread over rooms on a lattice clear of the walls, Group 1, 2, 3
            // in turn, under a Disk Hunt mission that lists them as Triggers.
            void DiskHunt()
            {
                const auto& hunt = Section({"gameplay", "diskhunt"});
                if (hunt.is_null()) return;
                const auto& disks = hunt.value("disks", Json());
                Json triggers = Json::array();
                const double reset = hunt.value("resetTime", 30.0);
                int made = 0;
                auto disk = [&](const std::string& id, const Vector& at, int group, const std::string& pointer)
                {
                    auto& actor = Add(id, "SBase.SDisk", "disk", pointer, at, 0);
                    actor.properties["Group"] = std::to_string(group);
                    actor.properties["ResetTime"] = Num(reset);
                    actor.label = "disk " + std::to_string(group);
                    triggers.push_back(Ref(id));
                    ++made;
                };
                if (disks.is_array())
                    for (size_t i = 0; i < disks.size(); ++i)
                    {
                        const auto where = Child("/gameplay/diskhunt/disks", i);
                        disk(disks[i].at("id").get<std::string>(), Locate(disks[i].at("at"), Child(where, "at"), Floor(48)).at, disks[i].value("group", made % 3 + 1), where);
                    }
                else if (disks.is_object())
                {
                    const auto rooms = Strings(disks.at("in"));
                    std::vector<const Piece*> spaces;
                    for (const auto& r : rooms)
                    {
                        const int index = FindPiece(r);
                        if (index < 0 || !low.pieces[static_cast<size_t>(index)].space) { Diag("error", "diskhunt.room", "/gameplay/diskhunt/disks/in", "\"" + r + "\" is not a room, corridor or vent of the spec.", "game-modes"); continue; }
                        spaces.push_back(&low.pieces[static_cast<size_t>(index)]);
                    }
                    const int count = disks.value("count", std::max(3, static_cast<int>(spaces.size()) * 3));
                    std::vector<std::vector<Vector>> spots;
                    for (size_t r = 0; r < spaces.size(); ++r)
                    {
                        const int here = count / static_cast<int>(spaces.size()) + (static_cast<int>(r) < count % static_cast<int>(spaces.size()) ? 1 : 0);
                        spots.push_back(Lattice(*spaces[r], here, 192, 48));
                    }
                    // Round the rooms, so each group draws from every room.
                    for (size_t k = 0; !spaces.empty(); ++k)
                    {
                        bool any = false;
                        for (auto& list : spots)
                            if (k < list.size()) { disk(NewId("Disk" + std::to_string(made + 1)), list[k], made % 3 + 1, "/gameplay/diskhunt/disks"); any = true; }
                        if (!any) break;
                    }
                }
                const auto mission = hunt.value("mission", Json::object());
                const auto id = mission.contains("id") ? mission.at("id").get<std::string>() : NewId("DiskHuntMission");
                const auto at = mission.contains("at") ? Locate(mission.at("at"), "/gameplay/diskhunt/mission/at", Floor(64)).at : Fallback(80);
                auto& actor = Add(id, "SBase.SMission", "other", "/gameplay/diskhunt", at, 0, false);
                actor.label = "Disk Hunt mission";
                actor.properties["ObjectiveName"] = Quoted(mission.value("name", std::string("Disk Hunt")));
                actor.properties["Description"] = Quoted(mission.value("description", std::string("Collect the disks")));
                actor.properties["DescriptionDEF"] = Quoted(mission.value("descriptionDef", std::string("Stop the spies collecting the disks")));
                actor.properties["GameMode"] = Enum(actor.cls, "GameMode", "GM_DiskHunt", "disk", "/gameplay/diskhunt");
                actor.properties["Triggers"] = triggers;
                if (mission.contains("atResetEvent")) actor.properties["AtResetEvent"] = mission.at("atResetEvent").get<std::string>();
                else if (hunt.value("disableSecurity", false)) { const auto off = SecurityOff(); if (!off.empty()) FindActor(id)->properties["AtResetEvent"] = off; }
                Props(*FindActor(id), mission);
                diskCount = made;
            }
            int diskCount = -1;
            std::string securityOff;
            // One SMagicEvent that untriggers every detector, for the modes with no
            // story objectives to guard.
            std::string SecurityOff()
            {
                if (!securityOff.empty()) return securityOff;
                if (detectorIds.empty()) return {};
                securityOff = NewId("SecurityOff");
                Json entries = Json::array();
                for (const auto& d : detectorIds) entries.push_back({{"Delay", "0"}, {"Event", TagOf(d, "", false)}, {"Type", "EVT_Untrigger"}, {"ValidOn", "EVT_Trigger"}});
                auto& event = Add(securityOff, "SBase.SMagicEvent", "other", "/gameplay", Fallback(96), 0, false);
                event.label = "security off";
                event.properties["Groups"] = Json::array({{{"EventGroup", entries}, {"Repeat", "0"}, {"Sequence", "False"}, {"SequenceIndex", "0"}}});
                magicIds.insert(Fold(securityOff));
                return securityOff;
            }
            void Deathmatch()
            {
                const auto& match = Section({"gameplay", "deathmatch"});
                if (match.is_null()) return;
                const auto mission = match.value("mission", Json::object());
                const auto id = mission.contains("id") ? mission.at("id").get<std::string>() : NewId("DeathmatchMission");
                const auto at = mission.contains("at") ? Locate(mission.at("at"), "/gameplay/deathmatch/mission/at", Floor(64)).at : Fallback(96);
                std::string reset = mission.value("atResetEvent", std::string());
                if (reset.empty() && match.value("disableSecurity", true)) reset = SecurityOff();
                auto& actor = Add(id, "SBase.SMission", "other", "/gameplay/deathmatch", at, 0, false);
                actor.label = "Deathmatch mission";
                actor.properties["ObjectiveName"] = Quoted(mission.value("name", std::string("Deathmatch")));
                actor.properties["Description"] = Quoted(mission.value("description", std::string("Eliminate the mercenaries")));
                actor.properties["DescriptionDEF"] = Quoted(mission.value("descriptionDef", std::string("Eliminate the spies")));
                actor.properties["GameMode"] = Enum(actor.cls, "GameMode", "GM_DeathMatch", "death", "/gameplay/deathmatch");
                if (!reset.empty()) actor.properties["AtResetEvent"] = reset;
                Props(actor, mission);
            }
            void Players()
            {
                const auto& counts = List(Section({"gameplay", "playerCounts"}));
                for (size_t i = 0; i < counts.size(); ++i)
                {
                    const auto& item = counts[i];
                    const auto pointer = Child("/gameplay/playerCounts", i);
                    const auto target = item.at("objective").get<std::string>();
                    if (!IsActor(target)) { Diag("error", "players.subject", Child(pointer, "objective"), "\"" + target + "\" is not an objective, sector or mover of the spec."); continue; }
                    const int lo = item.value("min", 0), hi = item.value("max", 0);
                    if (lo && hi && lo > hi) { Diag("error", "players.range", pointer, "min is more than max."); continue; }
                    low.players.push_back({{"objective", target}, {"min", lo}, {"max", hi}, {"open", item.value("open", false)}, {"pointer", pointer}});
                }
                const auto& targets = Section({"gameplay", "targets"});
                const Json list = targets.is_object() ? Json::array({targets}) : List(targets);
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto target = list[i].at("mission").get<std::string>();
                    const auto pointer = targets.is_object() ? std::string("/gameplay/targets") : Child("/gameplay/targets", i);
                    const auto* actor = FindActor(target);
                    if (!actor || actor->cls.find("SMission") == std::string::npos) { Diag("error", "players.mission", Child(pointer, "mission"), "\"" + target + "\" is not a mission of the spec."); continue; }
                    low.targets.push_back({{"mission", target}, {"sizes", list[i].at("sizes")}, {"pointer", pointer}});
                }
            }

            // --- Security --------------------------------------------------------
            std::string Detected(const std::string& who) const
            {
                if (who == "merc") return "Class'SBase.SPawnDefense'";
                if (who == "both") return "Class'SBase.SBasePawn'";
                return "Class'SBase.SPawnAttaque'";
            }
            void Security()
            {
                std::map<std::string, Vector> fed;
                const auto& devices = List(Section({"security", "devices"}));
                for (size_t i = 0; i < devices.size(); ++i)
                {
                    const auto& item = devices[i];
                    const auto pointer = Child("/security/devices", i);
                    const auto id = item.at("id").get<std::string>();
                    const auto kind = item.at("kind").get<std::string>();
                    Actor* made = nullptr;
                    if (kind == "motion")
                    {
                        Vector lo{}, hi{};
                        if (!BoxOf(item, pointer, 180, lo, hi)) continue;
                        const Vector centre{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
                        auto& volume = Add(NewId(id + "_Volume"), "Engine.Volume", "volume", pointer, centre, 0, false);
                        volume.geometry = "polygons";
                        volume.footprint = true;
                        volume.storey = StoreyBelow(lo[2]);
                        volume.polygons = BoxPolygons({(hi[0] - lo[0]) / 2, (hi[1] - lo[1]) / 2, (hi[2] - lo[2]) / 2});
                        const auto volumeId = volume.id;
                        auto& sensor = Add(id, "SBase.SVolumetricSensor", "motion", pointer, {centre[0], centre[1], lo[2]}, 0, false);
                        sensor.properties["Volumes"] = Json::array({Ref(volumeId)});
                        sensor.properties["DetectedClass"] = Detected(item.value("detected", std::string("spy")));
                        low.lines.push_back({{"from", Point(lo)}, {"to", Point(hi)}, {"kind", "motion"}, {"ids", {id, volumeId}}});
                        made = &sensor;
                    }
                    else
                    {
                        const double usual = kind == "camera" ? 200 : kind == "mine" ? 0 : 48;
                        Where spot;
                        bool spanRoom = false;
                        if (item.contains("at")) spot = Locate(item.at("at"), Child(pointer, "at"), Floor(usual));
                        else if (item.contains("in"))
                        {
                            const auto room = item.at("in").get<std::string>();
                            if (FindPiece(room) < 0) { Diag("error", "security.device", Child(pointer, "in"), "no piece has the id \"" + room + "\".", "alarms"); continue; }
                            spanRoom = kind == "laser" && !item.contains("facing");
                            spot = Locate(spanRoom ? Json{{"in", room}, {"u", -0.5}} : Json{{"in", room}}, Child(pointer, "in"), Floor(usual));
                            if (spanRoom)
                            {
                                const auto& p = low.pieces[static_cast<size_t>(FindPiece(room))];
                                spot.at = Workflow::TransformPoint({-p.spec.at("width").get<double>() / 2 + 4, 0, usual}, p.pose);
                                spot.yaw = p.pose.rotation[1];
                                spot.hasYaw = true;
                            }
                        }
                        else { Diag("error", "security.device", pointer, id + " needs at or in.", "alarms"); continue; }
                        if (kind == "camera" && spot.room >= 0) spot.at[2] = std::min(spot.at[2], low.pieces[static_cast<size_t>(spot.room)].box.hi[2] - 24);
                        const int yaw = YawOf(item, spot.at, pointer, spot.hasYaw ? spot.yaw : 0);
                        const auto& cls = kind == "laser" ? "SBase.SLaserTrigger" : kind == "camera" ? "SBase.SCamera" : kind == "presence" ? "SBase.SPresenceTrigger" : "SBase.SMineProx";
                        auto& actor = Add(id, cls, kind == "mine" ? "other" : kind, pointer, spot.at, yaw);
                        actor.label = kind + " " + id;
                        if (kind == "laser")
                        {
                            double length = 300;
                            if (item.contains("length")) length = L(item.at("length"));
                            else if (spanRoom) length = low.pieces[static_cast<size_t>(FindPiece(item.at("in").get<std::string>()))].spec.at("width").get<double>() - 8;
                            else if (item.contains("facing") && item.at("facing").is_object() && item.at("facing").contains("toward"))
                            {
                                const auto& toward = item.at("facing").at("toward");
                                const auto end = toward.is_string() ? Anchor(toward.get<std::string>(), Child(pointer, "facing")) : Locate(toward, Child(Child(pointer, "facing"), "toward"), Floor(usual)).at;
                                length = std::hypot(end[0] - spot.at[0], end[1] - spot.at[1]);
                            }
                            if (length < 8 || length > 65536) Diag("error", "security.laser", pointer, "a laser beam is 8 to 65536 units long.", "alarms");
                            actor.properties["LaserLength"] = std::to_string(static_cast<int>(std::lround(length)));
                            actor.properties["DetectedClass"] = Detected(item.value("detected", std::string("spy")));
                            if (item.contains("hideLaser")) actor.properties["bHideLaserInNormalVision"] = Flag(item.at("hideLaser").get<bool>());
                            if (item.contains("spawnPercent")) actor.properties["SpawnPerCent"] = Num(item.at("spawnPercent").get<double>());
                            low.lines.push_back({{"from", Point(spot.at)}, {"to", Point(Workflow::Security::BeamEnd(spot.at, yaw, length))}, {"kind", "laser"}, {"ids", {id}}});
                        }
                        else if (kind == "camera")
                        {
                            if (item.contains("coneAngle")) actor.properties["VisibilityConeAngle"] = Num(item.at("coneAngle").get<double>());
                            if (item.contains("maxDistance")) actor.properties["VisibilityMaxDistance"] = Num(L(item.at("maxDistance")));
                            if (item.contains("patrolAngle")) actor.properties["PatrolAngle"] = Num(item.at("patrolAngle").get<double>());
                            if (item.contains("rotationVelocity")) actor.properties["RotationVelocity"] = Num(item.at("rotationVelocity").get<double>());
                        }
                        else if (kind == "presence")
                        {
                            actor.properties["CollisionRadius"] = Num(item.contains("radius") ? L(item.at("radius")) : 128);
                            actor.properties["CollisionHeight"] = Num(item.contains("height") ? L(item.at("height")) : 64);
                            actor.properties["DetectedClass"] = Detected(item.value("detected", std::string("spy")));
                        }
                        made = &actor;
                    }
                    if (item.contains("detectionDelay")) made->properties["DetectionDelay"] = Num(item.at("detectionDelay").get<double>());
                    Props(*made, item);
                    detectorIds.push_back(id);
                    if (item.contains("alarm"))
                    {
                        const auto alarm = item.at("alarm").get<std::string>();
                        SetEvent(*made, alarm, Child(pointer, "alarm"));
                        if (!fed.count(Fold(alarm))) fed[Fold(alarm)] = made->at;
                    }
                }
                const auto& alarms = List(Section({"security", "alarms"}));
                std::set<std::string> alarmIds;
                for (size_t i = 0; i < alarms.size(); ++i)
                {
                    const auto& item = alarms[i];
                    const auto pointer = Child("/security/alarms", i);
                    const auto id = item.at("id").get<std::string>();
                    alarmIds.insert(Fold(id));
                    Vector at;
                    if (item.contains("at")) at = Locate(item.at("at"), Child(pointer, "at"), Floor(64)).at;
                    else if (fed.count(Fold(id))) { at = fed.at(Fold(id)); at[2] += 64; }
                    else at = Fallback();
                    auto& alarm = Add(id, "SBase.SAlarm", "alarm", pointer, at, 0, false);
                    const auto name = item.value("name", id);
                    alarm.label = "alarm " + name;
                    alarm.properties["AlarmName"] = Quoted(name);
                    alarm.properties["AlarmDescription"] = Quoted(item.value("description", name + " is active"));
                    alarm.properties["AlarmDescriptionSpy"] = Quoted(item.value("descriptionSpy", std::string("You've been detected")));
                    alarm.properties["Duration"] = Num(item.value("duration", 60.0));
                    const bool locks = !List(item.value("lockMovers", Json::array())).empty() || !List(item.value("lockObjectives", Json::array())).empty();
                    alarm.properties["MessagePriority"] = std::to_string(item.value("priority", locks ? 5 : 3));
                    Json events = Json::array();
                    for (const auto& output : List(item.value("outputs", Json::array()))) events.push_back(TagOf(output.get<std::string>(), Child(pointer, "outputs"), false));
                    alarm.properties["Events"] = events;
                    Json movers = Json::array();
                    const auto& lockMovers = List(item.value("lockMovers", Json::array()));
                    for (size_t k = 0; k < lockMovers.size(); ++k)
                    {
                        const auto mover = lockMovers[k].at("mover").get<std::string>();
                        const auto where = Child(Child(pointer, "lockMovers"), k);
                        const auto* target = FindActor(mover);
                        if (target && target->cls.find("Mover") == std::string::npos && target->cls.find("SLift") == std::string::npos)
                            Diag("error", "alarm.lock-mover", where, mover + " is not a mover.", "alarms");
                        movers.push_back({{"ActionBeforeLock", lockMovers[k].value("action", std::string("ActionBeforeLock_None"))}, {"MoverToLock", ObjectRef(mover, where)}});
                        if (target) low.lines.push_back({{"from", Point(at)}, {"to", Point(target->at)}, {"kind", "lock"}, {"ids", {id, mover}}});
                    }
                    if (!movers.empty()) alarm.properties["MoversToLock"] = movers;
                    Json locked = Json::array();
                    const auto& lockObjectives = List(item.value("lockObjectives", Json::array()));
                    for (size_t k = 0; k < lockObjectives.size(); ++k)
                    {
                        const auto target = lockObjectives[k].get<std::string>();
                        const auto where = Child(Child(pointer, "lockObjectives"), k);
                        std::vector<std::pair<std::string, std::string>> list;
                        if (triggersOf.count(Fold(target))) list = triggersOf.at(Fold(target));
                        else if (triggerKind.count(Fold(target))) list.push_back({target, triggerKind.at(Fold(target))});
                        else { Diag("error", "alarm.lock-objective", where, "\"" + target + "\" is not a story objective or objective trigger of the spec.", "alarms"); continue; }
                        for (const auto& [trigger, kind] : list)
                        {
                            if (kind == "bomb")
                            {
                                if (!triggersOf.count(Fold(target))) Diag("error", "alarm.lock-bomb", where, trigger + " is a bomb target, which an alarm cannot lock.", "alarms");
                                continue;
                            }
                            locked.push_back(Ref(trigger));
                        }
                    }
                    if (!locked.empty()) alarm.properties["ObjectiveTriggersToLock"] = locked;
                    Props(alarm, item);
                }
                for (const auto& d : detectorIds)
                {
                    const auto* actor = FindActor(d);
                    if (!actor) continue;
                    const auto event = actor->properties.value("Event", std::string());
                    if (event.empty()) continue;
                    if (!alarmIds.count(Fold(event)) && !refs.count(Fold(event)) && !IsActor(event))
                        Diag("error", "alarm.unresolved", PointerOf(d) + "/alarm", d + " sends Event " + event + " but no alarm has that id.", "alarms");
                    else if (const auto* alarm = FindActor(event))
                        low.lines.push_back({{"from", Point(actor->at)}, {"to", Point(alarm->at)}, {"kind", "alarm"}, {"ids", {d, event}}});
                }
                CamNet();
            }
            bool BoxOf(const Json& item, const std::string& pointer, double height, Vector& lo, Vector& hi)
            {
                Json box = item.value("box", Json());
                if (box.is_null() && item.contains("in")) box = {{"in", item.at("in")}};
                if (box.is_null()) { Diag("error", "volume.box", pointer, "needs box {min, max} or {in}.", "zones-and-volumes"); return false; }
                if (box.contains("in"))
                {
                    const int index = FindPiece(box.at("in").get<std::string>());
                    if (index < 0) { Diag("error", "volume.box", Child(Child(pointer, "box"), "in"), "no piece has the id \"" + box.at("in").get<std::string>() + "\".", "zones-and-volumes"); return false; }
                    const auto& p = low.pieces[static_cast<size_t>(index)];
                    lo = p.box.lo;
                    hi = p.box.hi;
                    const double h = box.contains("height") ? L(box.at("height")) : height;
                    if (h > 0) hi[2] = std::min(hi[2], lo[2] + h);
                    return true;
                }
                const auto a = Locate(box.at("min"), Child(Child(pointer, "box"), "min"), Floor(0)).at;
                const auto bWhere = Locate(box.at("max"), Child(Child(pointer, "box"), "max"), Floor(height > 0 ? height : 0));
                auto b = bWhere.at;
                for (int k = 0; k < 3; ++k) { lo[k] = std::min(a[k], b[k]); hi[k] = std::max(a[k], b[k]); }
                for (int k = 0; k < 3; ++k)
                    if (hi[k] - lo[k] < 8) { Diag("error", "volume.box", Child(pointer, "box"), "the box is less than 8 units across on one axis.", "zones-and-volumes"); return false; }
                return true;
            }
            // The Mercs' camera network: one closed loop in the order listed,
            // exactly one first camera, each blocking players.
            void CamNet()
            {
                const auto& list = List(Section({"security", "camnet"}));
                if (list.empty()) return;
                int first = -1;
                for (size_t i = 0; i < list.size(); ++i)
                    if (list[i].value("first", false))
                    {
                        if (first >= 0) Diag("error", "camnet.first", Child(Child("/security/camnet", i), "first"), "only one camera of the network is the first (bFirstCam).", "level-ingredients");
                        else first = static_cast<int>(i);
                    }
                if (first < 0) first = 0;
                std::vector<std::string> ids;
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& item = list[i];
                    const auto pointer = Child("/security/camnet", i);
                    const auto id = item.at("id").get<std::string>();
                    Where spot = item.contains("at") ? Locate(item.at("at"), Child(pointer, "at"), Floor(200)) : Where{Fallback(200), -1, 0, false};
                    if (spot.room >= 0) spot.at[2] = std::min(spot.at[2], low.pieces[static_cast<size_t>(spot.room)].box.hi[2] - 32);
                    int fallback = 0;
                    if (spot.room >= 0) try { fallback = Workflow::Security::YawTowards(spot.at, low.pieces[static_cast<size_t>(spot.room)].pose.position); } catch (const std::exception&) {}
                    auto& camera = Add(id, "SBase.SCamNetwork", "camnet", pointer, spot.at, YawOf(item, spot.at, pointer, fallback));
                    camera.label = "CamNet " + item.value("camName", id);
                    camera.properties["CamName"] = Quoted(item.value("camName", id));
                    camera.properties["bFirstCam"] = Flag(static_cast<int>(i) == first);
                    camera.properties["bBlockActors"] = "True";
                    camera.properties["bBlockPlayers"] = "True";
                    if (item.contains("constraints"))
                    {
                        const auto& c = item.at("constraints");
                        for (const auto& [key, property] : std::vector<std::pair<const char*, const char*>>{{"pitch", "PitchConstraint"}, {"yaw", "YawConstraint"}, {"roll", "RollConstraint"}})
                            if (c.contains(key)) camera.properties[property] = {{"X", "0"}, {"Y", Num(c.at(key)[0].get<double>())}, {"Z", Num(c.at(key)[1].get<double>())}};
                    }
                    Props(camera, item);
                    ids.push_back(id);
                }
                for (size_t i = 0; i < ids.size(); ++i)
                {
                    auto* camera = FindActor(ids[i]);
                    const auto& next = ids[(i + 1) % ids.size()];
                    const auto& prev = ids[(i + ids.size() - 1) % ids.size()];
                    camera->properties["NextCam"] = Ref(next);
                    camera->properties["PrevCam"] = Ref(prev);
                    low.lines.push_back({{"from", Point(camera->at)}, {"to", Point(FindActor(next)->at)}, {"kind", "camnet"}, {"ids", {ids[i], next}}});
                }
            }

            // --- Mechanisms ------------------------------------------------------
            std::string Usable(const std::string& who) const
            {
                if (who == "spy") return "U_SPAWNOFFENSE";
                if (who == "merc") return "U_SPAWNDEFENSE";
                return "U_BOTH";
            }
            Json Keys(const std::string& cls, const std::vector<Vector>& offsets, const std::vector<Rotation>& turns, Json& rotations)
            {
                const auto size = std::max<size_t>(Dimension(cls, "KeyPos", 8), offsets.size());
                Json positions = Json::array();
                rotations = Json::array();
                for (size_t k = 0; k < size; ++k)
                {
                    positions.push_back(Loc(k < offsets.size() ? offsets[k] : Vector{}));
                    rotations.push_back(Rot(k < turns.size() ? turns[k] : Rotation{}));
                }
                return positions;
            }
            // An SInterrupteur at the wiki's interaction values.
            Actor& Switch(const std::string& id, const std::string& kind, const std::string& pointer, const Vector& at, int yaw, const std::string& usable, double hackTime, const std::string& text)
            {
                auto& actor = Add(id, "SBase.SInterrupteur", "button", pointer, at, yaw);
                actor.label = kind + " " + id;
                actor.properties["fInteractionHeight"] = "120";
                actor.properties["fInteractionRadius"] = "60";
                actor.properties["bInteractInFront"] = "True";
                actor.properties["UseRelativeLocation"] = kind == "panel" ? Json{{"X", "39.5"}, {"Y", "3"}, {"Z", "0"}} : Json{{"X", "55"}, {"Y", "0"}, {"Z", "0"}};
                actor.properties["UseRelativeRotation"] = Rot({0, 32768, 0});
                // A whole number of seconds: the engine's property is an int.
                actor.properties["iHackingTime"] = std::to_string(std::max(0L, std::lround(hackTime)));
                actor.properties["UsableBy"] = usable;
                actor.properties["sDescriptionText"] = Quoted(text);
                return actor;
            }
            void Mechanisms()
            {
                const auto& doors = List(Section({"mechanisms", "doors"}));
                for (size_t i = 0; i < doors.size(); ++i)
                {
                    const auto& item = doors[i];
                    const auto pointer = Child("/mechanisms/doors", i);
                    const auto id = item.at("id").get<std::string>();
                    if (!item.contains("at")) { Diag("error", "door.at", pointer, id + " needs at ({doorway: id} or a position).", "movers"); continue; }
                    const auto spot = Locate(item.at("at"), Child(pointer, "at"), Floor(0));
                    const int yaw = item.contains("yaw") ? YawUnits(item.at("yaw").get<double>()) : spot.hasYaw ? spot.yaw : 0;
                    double width = 96, height = 128, depth = 16;
                    Workflow::Pose doorway{spot.at, {0, yaw, 0}};
                    if (item.at("at").is_object() && item.at("at").contains("doorway"))
                    {
                        const auto& p = low.pieces[static_cast<size_t>(FindPiece(item.at("at").at("doorway").get<std::string>()))];
                        width = p.spec.at("width");
                        height = p.spec.at("height");
                        depth = p.spec.at("length");
                        doorway = p.pose;
                    }
                    auto& door = Add(id, "Engine.Mover", "door", pointer, spot.at, yaw);
                    door.label = "door " + id;
                    Mesh(door, item.at("mesh").get<std::string>());
                    door.properties["InitialState"] = "TriggerOpenTimed";
                    door.properties["MoverEncroachType"] = "ME_WaitAndRetryWhenEncroach";
                    door.properties["MoveTime"] = Num(item.value("moveTime", 1.0));
                    door.properties["StayOpenTime"] = Num(item.value("stayOpen", 3.0));
                    door.properties["KeyNum"] = "0";
                    door.properties["NumKeys"] = "2";
                    Vector offset{0, 0, height};
                    Rotation turn{};
                    if (item.contains("open"))
                    {
                        const auto& open = item.at("open");
                        if (open.contains("offset")) offset = {L(open.at("offset")[0]), L(open.at("offset")[1]), L(open.at("offset")[2])};
                        else { offset = {}; turn = {0, YawUnits(open.at("yaw").get<double>()), 0}; }
                    }
                    Json rotations;
                    door.properties["KeyPos"] = Keys(door.cls, {Vector{}, offset}, {Rotation{}, turn}, rotations);
                    door.properties["KeyRot"] = rotations;
                    Props(door, item);
                    doorIds.push_back(id);
                    const auto trigger = item.value("trigger", Json{{"kind", "presence"}});
                    const auto kind = trigger.value("kind", std::string("presence"));
                    if (kind != "event") doorTriggers[Fold(id)] = "its own " + kind + " trigger";
                    const auto usable = trigger.value("usableBy", std::string(kind == "panel" ? "spy" : "both"));
                    const auto where = Child(pointer, "trigger");
                    if (kind == "button" || kind == "panel")
                    {
                        Vector at;
                        int facing = 0;
                        if (trigger.contains("at"))
                        {
                            at = Locate(trigger.at("at"), Child(where, "at"), Floor(100)).at;
                            facing = (doorway.rotation[1] + 65536 - 16384) % 65536;
                        }
                        else
                        {
                            // Beside the opening, on whichever face of the wall is open
                            // space (a doorway's local Y points out of some walls and into
                            // others), facing into that room.
                            double side = -1;
                            for (double s : {-1.0, 1.0})
                            {
                                const auto probe = Workflow::TransformPoint({-(width / 2 + 48), s * (depth / 2 + 24), 100}, doorway);
                                if (SpaceAt(probe[0], probe[1], probe[2], true) >= 0) { side = s; break; }
                            }
                            at = Workflow::TransformPoint({-(width / 2 + 48), side * (depth / 2 + 2), 100}, doorway);
                            facing = (doorway.rotation[1] + 65536 + (side > 0 ? 16384 : -16384)) % 65536;
                        }
                        auto& button = Switch(NewId(id + (kind == "panel" ? "_Panel" : "_Button")), kind, where, at, facing, Usable(usable), trigger.value("hackTime", kind == "panel" ? 5.0 : 0.0), trigger.value("text", std::string(kind == "panel" ? "Hack" : "Open")));
                        SetEvent(button, id, where);
                        low.lines.push_back({{"from", Point(at)}, {"to", Point(spot.at)}, {"kind", "event"}, {"ids", {button.id, id}}});
                    }
                    else if (kind == "presence")
                    {
                        Vector at = trigger.contains("at") ? Locate(trigger.at("at"), Child(where, "at"), Floor(48)).at : Vector{spot.at[0], spot.at[1], spot.at[2] + 48};
                        auto& sensor = Add(NewId(id + "_Sensor"), "SBase.SPresenceTrigger", "presence", where, at, 0, false);
                        sensor.properties["CollisionRadius"] = Num(trigger.contains("radius") ? L(trigger.at("radius")) : 128);
                        sensor.properties["CollisionHeight"] = "64";
                        sensor.properties["DetectedClass"] = Detected(usable);
                        SetEvent(sensor, id, where);
                    }
                }
                const auto& movers = List(Section({"mechanisms", "movers"}));
                for (size_t i = 0; i < movers.size(); ++i)
                {
                    const auto& item = movers[i];
                    const auto pointer = Child("/mechanisms/movers", i);
                    if (!item.contains("at")) { Diag("error", "mover.at", pointer, "a mover needs at.", "movers"); continue; }
                    const auto spot = Locate(item.at("at"), Child(pointer, "at"), Floor(0));
                    const int yaw = YawOf(item, spot.at, pointer, spot.hasYaw ? spot.yaw : 0);
                    auto& mover = Add(item.at("id").get<std::string>(), FullClass(item.value("class", std::string("Engine.Mover"))), "door", pointer, spot.at, yaw);
                    Mesh(mover, item.at("mesh").get<std::string>());
                    mover.properties["InitialState"] = item.value("initialState", std::string("TriggerOpenTimed"));
                    mover.properties["MoverEncroachType"] = item.value("encroach", std::string("ME_WaitAndRetryWhenEncroach"));
                    mover.properties["MoveTime"] = Num(item.value("moveTime", 1.0));
                    mover.properties["StayOpenTime"] = Num(item.value("stayOpen", 3.0));
                    std::vector<Vector> offsets{Vector{}};
                    std::vector<Rotation> turns{Rotation{}};
                    for (const auto& k : List(item.value("keys", Json::array())))
                    {
                        const auto o = k.value("offset", Json::array({0, 0, 0}));
                        offsets.push_back({L(o[0]), L(o[1]), L(o[2])});
                        turns.push_back({YawUnits(k.value("pitch", 0.0)), YawUnits(k.value("yaw", 0.0)), YawUnits(k.value("roll", 0.0))});
                    }
                    mover.properties["KeyNum"] = "0";
                    mover.properties["NumKeys"] = std::to_string(offsets.size());
                    Json rotations;
                    mover.properties["KeyPos"] = Keys(mover.cls, offsets, turns, rotations);
                    mover.properties["KeyRot"] = rotations;
                    Props(mover, item);
                    doorIds.push_back(mover.id);
                }
                Lifts();
                const auto& buttons = List(Section({"mechanisms", "buttons"}));
                for (size_t i = 0; i < buttons.size(); ++i)
                {
                    const auto& item = buttons[i];
                    const auto pointer = Child("/mechanisms/buttons", i);
                    if (!item.contains("at")) { Diag("error", "button.at", pointer, "a button needs at.", "buttons-and-hacking-panels"); continue; }
                    const auto kind = item.value("kind", std::string("button"));
                    const auto spot = Locate(item.at("at"), Child(pointer, "at"), Floor(100));
                    const auto usable = item.value("usableBy", std::string(kind == "panel" ? "spy" : "both"));
                    if (kind == "panel" && usable != "spy") Diag("warning", "button.hack", Child(pointer, "usableBy"), "Mercenaries have no hacking animation; a hacking panel is meant for spies.", "buttons-and-hacking-panels");
                    auto& button = Switch(item.at("id").get<std::string>(), kind, pointer, spot.at, YawOf(item, spot.at, pointer, spot.hasYaw ? spot.yaw : 0), Usable(usable),
                                          item.value("hackTime", kind == "panel" ? 5.0 : 0.0), item.value("text", std::string(kind == "panel" ? "Hack" : "Use")));
                    if (item.contains("mesh")) Mesh(button, item.at("mesh").get<std::string>());
                    Props(button, item);
                    pendingTargets.push_back({button.id, item.value("targets", Json::array()), pointer});
                }
            }
            struct Pending { std::string id; Json targets; std::string pointer; };
            std::vector<Pending> pendingTargets;
            std::map<std::string, std::string> doorTriggers; // door id -> its own presence, button or panel trigger
            std::set<std::string> heldDoors;                 // doors a sector holds open (TriggerToggle)
            // What besides the sector's own event triggers a door: its own
            // trigger, a button's targets, a link, an alarm's outputs or a sector
            // that holds it open already. Each of those would toggle a held door.
            std::string OtherTrigger(const std::string& door) const
            {
                const auto id = Fold(door);
                if (const auto found = doorTriggers.find(id); found != doorTriggers.end()) return found->second;
                if (heldDoors.count(id)) return "an earlier sector that holds it open";
                for (const auto& pending : pendingTargets)
                    for (const auto& t : Strings(pending.targets)) if (Fold(t) == id) return "the button " + pending.id;
                for (const auto& link : List(Section({"events", "links"})))
                    if (Fold(link.value("to", std::string())) == id) return "the link from " + link.value("from", std::string());
                for (const auto& alarm : List(Section({"security", "alarms"})))
                    for (const auto& output : Strings(alarm.value("outputs", Json::array()))) if (Fold(output) == id) return "the alarm " + alarm.value("id", std::string());
                return {};
            }
            // Buttons fire their one target, or an SMagicEvent that fires them all;
            // done once everything they may name exists.
            void ButtonTargets()
            {
                for (const auto& pending : pendingTargets)
                {
                    const auto targets = Strings(pending.targets);
                    auto* button = FindActor(pending.id);
                    if (!button || targets.empty()) continue;
                    if (targets.size() == 1) { SetEvent(*button, TagOf(targets[0], Child(pending.pointer, "targets"), false), Child(pending.pointer, "targets")); continue; }
                    const auto fan = NewId(pending.id + "_Targets");
                    Json entries = Json::array();
                    for (const auto& t : targets) entries.push_back({{"Delay", "0"}, {"Event", TagOf(t, Child(pending.pointer, "targets"), false)}, {"Type", "EVT_Trigger"}, {"ValidOn", "EVT_Trigger"}});
                    const auto at = FindActor(pending.id)->at;
                    auto& event = Add(fan, "SBase.SMagicEvent", "other", Child(pending.pointer, "targets"), {at[0], at[1], at[2] + 48}, 0, false);
                    event.properties["Groups"] = Json::array({{{"EventGroup", entries}, {"Repeat", "0"}, {"Sequence", "False"}, {"SequenceIndex", "0"}}});
                    magicIds.insert(Fold(fan));
                    SetEvent(*FindActor(pending.id), fan, Child(pending.pointer, "targets"));
                }
            }
            // A storey's opening onto a lift shaft: the doorway of the spec that
            // joins them with its sill nearest the storey's floor, or the one a
            // lift's doors entry names. inward is the side of its local Y the
            // shaft is on (+1 or -1).
            struct Opening { bool found = false; Pose pose; double width = 96, height = 128, depth = 16, inward = 1; };
            Opening ShaftOpening(const Piece& shaft, double storeyZ0, const Json& given, const std::string& pointer)
            {
                Opening o;
                int door = -1;
                if (given.is_object() && given.contains("doorway"))
                {
                    door = FindPiece(given.at("doorway").get<std::string>());
                    if (door < 0 || low.pieces[static_cast<size_t>(door)].kind != "Doorway")
                    {
                        Diag("error", "elevator.door", Child(pointer, "doorway"), "no doorway has the id \"" + given.at("doorway").get<std::string>() + "\".", "elevators");
                        return o;
                    }
                }
                else if (given.is_object() && given.contains("at"))
                {
                    const auto spot = Locate(given.at("at"), Child(pointer, "at"), Floor(0));
                    o.pose = {spot.at, {0, given.contains("yaw") ? YawUnits(given.at("yaw").get<double>()) : spot.hasYaw ? spot.yaw : 0, 0}};
                    o.found = true;
                }
                else
                    for (size_t k = 0; k < low.pieces.size(); ++k)
                    {
                        const auto& p = low.pieces[k];
                        if (p.kind != "Doorway" || (Fold(p.joins[0]) != Fold(shaft.id) && Fold(p.joins[1]) != Fold(shaft.id))) continue;
                        const double above = p.pose.position[2] - storeyZ0;
                        if (above < -1 || above > 128) continue;
                        if (door < 0 || std::abs(above) < std::abs(low.pieces[static_cast<size_t>(door)].pose.position[2] - storeyZ0)) door = static_cast<int>(k);
                    }
                if (door >= 0)
                {
                    const auto& p = low.pieces[static_cast<size_t>(door)];
                    o.pose = p.pose;
                    o.width = p.spec.at("width");
                    o.height = p.spec.at("height");
                    o.depth = p.spec.at("length");
                    o.found = true;
                }
                if (!o.found) return o;
                const double a = o.pose.rotation[1] * 2 * kPi / 65536;
                o.inward = -std::sin(a) * (shaft.pose.position[0] - o.pose.position[0]) + std::cos(a) * (shaft.pose.position[1] - o.pose.position[1]) >= 0 ? 1 : -1;
                return o;
            }
            // A lift door: a Mover (or the car's SLiftDoor) whose Key 0 is open
            // and Key 1 closed, the reverse of an ordinary door. Location is its
            // Key 0 pose, as for every mover the compiler writes; keyNum 1 puts it
            // closed.
            Actor& LiftDoor(const std::string& id, const std::string& cls, const std::string& pointer, const std::string& mesh, const Vector& closed, int yaw, const Json& item, double height, int keyNum)
            {
                Vector offset{0, 0, height};
                int turn = 0;
                if (item.contains("doorOpen"))
                {
                    const auto& open = item.at("doorOpen");
                    if (open.contains("offset")) offset = {L(open.at("offset")[0]), L(open.at("offset")[1]), L(open.at("offset")[2])};
                    else { offset = {}; turn = YawUnits(open.at("yaw").get<double>()); }
                }
                auto& door = Add(id, cls, "door", pointer, {closed[0] + offset[0], closed[1] + offset[1], closed[2] + offset[2]}, (yaw + turn) % 65536);
                Mesh(door, mesh);
                door.properties["MoverEncroachType"] = "ME_WaitAndRetryWhenEncroach";
                door.properties["MoveTime"] = Num(item.value("doorMoveTime", 1.0));
                door.properties["KeyNum"] = std::to_string(keyNum);
                door.properties["NumKeys"] = "2";
                Json rotations;
                door.properties["KeyPos"] = Keys(door.cls, {Vector{}, Vector{-offset[0], -offset[1], -offset[2]}}, {Rotation{}, Rotation{0, -turn, 0}}, rotations);
                door.properties["KeyRot"] = rotations;
                return door;
            }
            // SLift (scct://wiki/elevators): one key per storey, the start storey
            // first. Each storey's opening onto the shaft gets an external door
            // (Key 0 open, Key 1 closed; KeyNum 0 on the start storey, 1 on the
            // others) and a call switch beside it; the car gets an SLiftDoor on
            // each side a door opens and a switch that rides with it. Every
            // switch is TriggerToggle, sending the lift's Tag.
            // DoorsAndSwitches[k] lists storey k's door and the switches that
            // work while the car is there: the car's and every other storey's
            // call switch, so those are the ones usable at the start.
            void Lifts()
            {
                const auto& lifts = List(Section({"mechanisms", "lifts"}));
                for (size_t i = 0; i < lifts.size(); ++i)
                {
                    const auto& item = lifts[i];
                    const auto pointer = Child("/mechanisms/lifts", i);
                    const auto id = item.at("id").get<std::string>();
                    const int shaftIndex = FindPiece(item.at("shaft").get<std::string>());
                    if (shaftIndex < 0) { Diag("error", "elevator.shaft", Child(pointer, "shaft"), "no piece has the id \"" + item.at("shaft").get<std::string>() + "\".", "elevators"); continue; }
                    const auto shaft = low.pieces[static_cast<size_t>(shaftIndex)];
                    std::vector<std::pair<std::string, double>> floors;
                    for (const auto& s : item.at("storeys"))
                    {
                        const auto found = storeyZ.find(Fold(s.get<std::string>()));
                        if (found == storeyZ.end()) { Diag("error", "elevator.storey", Child(pointer, "storeys"), "\"" + s.get<std::string>() + "\" is not a storey.", "elevators"); continue; }
                        floors.push_back({s.get<std::string>(), found->second});
                    }
                    if (floors.size() < 2) continue;
                    if (item.contains("start"))
                    {
                        const auto start = Fold(item.at("start").get<std::string>());
                        auto found = std::find_if(floors.begin(), floors.end(), [&](const auto& f) { return Fold(f.first) == start; });
                        if (found == floors.end()) Diag("error", "elevator.start", Child(pointer, "start"), "start is not one of the lift's storeys.", "elevators");
                        else std::rotate(floors.begin(), found, found + 1);
                    }
                    double rise = 0;
                    for (const auto& f : floors) rise = std::max(rise, std::abs(f.second - floors[0].second));
                    const double moveTime = item.value("moveTime", std::clamp(rise / 160.0, 1.5, 8.0));
                    const Vector base{shaft.pose.position[0], shaft.pose.position[1], floors[0].second};
                    if (!item.contains("mesh"))
                    {
                        if (floors.size() != 2) { Diag("error", "elevator.mesh", pointer, "a lift without a mesh is a two-key brush platform (design.lift); give mesh for a lift of more storeys.", "elevators"); continue; }
                        if (item.contains("doorMesh")) Diag("warning", "elevator.doors", Child(pointer, "doorMesh"), id + " has no mesh, so it is a design.lift platform, which has no doors; doorMesh is not used.", "elevators");
                        usedIds.insert(Fold(id));
                        low.lifts.push_back({id, pointer, {{"position", Point({base[0], base[1], std::min(floors[0].second, floors[1].second) - 8})}, {"width", 192}, {"length", 192}, {"thickness", 16}, {"rise", std::abs(floors[1].second - floors[0].second)}, {"moveTime", moveTime}}, base});
                        continue;
                    }
                    {
                        auto& lift = Add(id, "SBase.SLift", "lift", pointer, base, 0);
                        lift.label = "lift " + id;
                        Mesh(lift, item.at("mesh").get<std::string>());
                        lift.properties["MoveTime"] = Num(moveTime);
                        lift.properties["KeyNum"] = "0";
                        lift.properties["NumKeys"] = std::to_string(floors.size());
                        std::vector<Vector> offsets;
                        for (const auto& f : floors) offsets.push_back({0, 0, f.second - floors[0].second});
                        Json rotations;
                        lift.properties["KeyPos"] = Keys(lift.cls, offsets, {}, rotations);
                        lift.properties["KeyRot"] = rotations;
                        Props(lift, item);
                    }
                    low.lines.push_back({{"from", Point(base)}, {"to", Point({base[0], base[1], base[2] + rise})}, {"kind", "lift"}, {"ids", {id}}});
                    const auto liftTag = TagOf(id, pointer, true);
                    // Each storey's opening, from doors[] or the doorways onto the shaft.
                    std::map<std::string, Json> given;
                    const auto& doorList = List(item.value("doors", Json::array()));
                    for (size_t k = 0; k < doorList.size(); ++k)
                    {
                        const auto storey = doorList[k].at("storey").get<std::string>();
                        if (std::none_of(floors.begin(), floors.end(), [&](const auto& f) { return Fold(f.first) == Fold(storey); }))
                            Diag("error", "elevator.door", Child(Child(Child(pointer, "doors"), k), "storey"), "\"" + storey + "\" is not one of " + id + "'s storeys.", "elevators");
                        else { given[Fold(storey)] = doorList[k]; given[Fold(storey)]["pointer"] = Child(Child(pointer, "doors"), k); }
                    }
                    std::vector<Opening> openings;
                    for (const auto& f : floors)
                    {
                        const auto found = given.find(Fold(f.first));
                        const Json entry = found == given.end() ? Json() : found->second;
                        openings.push_back(ShaftOpening(shaft, f.second, entry, entry.is_object() ? entry.at("pointer").get<std::string>() : pointer));
                        if (!openings.back().found)
                            Diag("warning", "elevator.opening", pointer, "storey " + f.first + " has no doorway onto " + shaft.id + ", so the lift gives it no door or call switch. Add a doorway between a room and the shaft, or doors[{storey, doorway | at}].", "elevators");
                    }
                    const auto doorMesh = item.value("doorMesh", std::string());
                    Json doors = Json::array(), liftDoors = Json::array();
                    std::vector<std::string> doorless;
                    for (size_t k = 0; k < floors.size(); ++k)
                    {
                        Json here = Json::array();
                        const auto& o = openings[k];
                        const auto found = given.find(Fold(floors[k].first));
                        const auto mesh = found != given.end() && found->second.contains("mesh") ? found->second.at("mesh").get<std::string>() : doorMesh;
                        if (o.found && mesh.empty()) doorless.push_back(floors[k].first);
                        if (o.found && !mesh.empty())
                        {
                            auto& door = LiftDoor(NewId(id + "_Door_" + floors[k].first), "Engine.Mover", pointer, mesh, o.pose.position, o.pose.rotation[1], item, o.height, k == 0 ? 0 : 1);
                            door.label = "lift door " + floors[k].first;
                            // The lift moves it; a bump or a stray event must not.
                            door.properties["InitialState"] = "TriggerControl";
                            here.push_back(Ref(door.id));
                        }
                        doors.push_back(here);
                    }
                    if (!doorless.empty())
                    {
                        std::string list;
                        for (const auto& s : doorless) list += (list.empty() ? "" : ", ") + s;
                        Diag("warning", "elevator.doors", pointer, id + " has no doorMesh, so " + list + (doorless.size() == 1 ? " gets" : " get") + " no door: players can step into the shaft where the car is not. Give doorMesh, a static mesh, for the storeys' doors and the car's SLiftDoor.", "elevators");
                    }
                    // The car's doors: one SLiftDoor on each side a storey's door opens,
                    // just inside the opening, at the start storey where the car waits.
                    const auto carMesh = item.value("carDoorMesh", doorMesh);
                    std::set<std::pair<long, long>> sides;
                    for (size_t k = 0; k < floors.size() && !carMesh.empty(); ++k)
                    {
                        const auto& o = openings[k];
                        if (!o.found || doors[k].empty()) continue;
                        const double a = o.pose.rotation[1] * 2 * kPi / 65536;
                        const double nx = -std::sin(a) * o.inward, ny = std::cos(a) * o.inward;
                        if (!sides.insert({std::lround(nx * 8), std::lround(ny * 8)}).second) continue;
                        const double in = o.depth / 2 + 12;
                        const Vector closed{o.pose.position[0] + nx * in, o.pose.position[1] + ny * in, floors[0].second + (o.pose.position[2] - floors[k].second)};
                        auto& carDoor = LiftDoor(NewId(id + "_CarDoor" + std::to_string(sides.size())), "SBase.SLiftDoor", pointer, carMesh, closed, o.pose.rotation[1], item, o.height, 0);
                        carDoor.label = "car door";
                        liftDoors.push_back(Ref(carDoor.id));
                    }
                    // Switches: a call switch beside each storey's opening, outside the
                    // shaft facing the room, and one in the car.
                    std::vector<std::string> calls(floors.size());
                    std::string car;
                    if (item.value("switches", true))
                    {
                        for (size_t k = 0; k < floors.size(); ++k)
                        {
                            const auto& o = openings[k];
                            if (!o.found) continue;
                            const double side = -o.inward;
                            const auto at = Workflow::TransformPoint({-(o.width / 2 + 48), side * (o.depth / 2 + 2), 100}, o.pose);
                            auto& button = Switch(NewId(id + "_Switch_" + floors[k].first), "button", pointer, at, (o.pose.rotation[1] + 65536 + (side > 0 ? 16384 : -16384)) % 65536, "U_BOTH", 0, "Call lift");
                            button.properties["TriggerMethode"] = "TriggerToggle";
                            button.properties["bInitiallyUsable"] = Flag(k != 0);
                            SetEvent(button, liftTag, pointer);
                            calls[k] = button.id;
                        }
                        if (item.value("carSwitch", true))
                        {
                            const auto& o = openings[0];
                            Vector at{};
                            int facing = 0;
                            if (o.found)
                            {
                                at = Workflow::TransformPoint({-(o.width / 2 + 32), o.inward * (o.depth / 2 + 16), 100}, o.pose);
                                facing = (o.pose.rotation[1] + 65536 + (o.inward > 0 ? 16384 : -16384)) % 65536;
                            }
                            else
                            {
                                at = Workflow::TransformPoint({shaft.spec.at("width").get<double>() / 2 - 24, 0, floors[0].second - shaft.pose.position[2] + 100}, shaft.pose);
                                facing = (shaft.pose.rotation[1] + 32768) % 65536;
                            }
                            auto& button = Switch(NewId(id + "_CarSwitch"), "button", pointer, at, facing, "U_BOTH", 0, "Lift");
                            button.properties["TriggerMethode"] = "TriggerToggle";
                            button.properties["bInitiallyUsable"] = "True";
                            // It rides with the car: an actor whose AttachTag is a
                            // mover's Tag is based on that mover.
                            button.properties["AttachTag"] = liftTag;
                            SetEvent(button, liftTag, pointer);
                            car = button.id;
                        }
                    }
                    Json floorsJson = Json::array();
                    for (size_t k = 0; k < floors.size(); ++k)
                    {
                        Json switches = Json::array();
                        if (!car.empty()) switches.push_back(Ref(car));
                        for (size_t j = 0; j < floors.size(); ++j) if (j != k && !calls[j].empty()) switches.push_back(Ref(calls[j]));
                        floorsJson.push_back({{"Doors", doors[k]}, {"Switches", switches}});
                    }
                    // Props given for the lift win over what is worked out here.
                    auto& lift = *FindActor(id);
                    const auto own = item.value("props", Json::object());
                    if (!own.contains("DoorsAndSwitches")) lift.properties["DoorsAndSwitches"] = floorsJson;
                    if (!liftDoors.empty() && !own.contains("LiftDoors")) lift.properties["LiftDoors"] = liftDoors;
                }
            }

            // --- Environment -----------------------------------------------------
            void Environment()
            {
                Volumes();
                Sounds();
                Ingredients();
                Objects();
                Generic("props", "Engine.StaticMeshActor", "other");
                Generic("fish", "SBase.SFish", "other");
                Generic("softbody", "SoftBody.ESBPatchActor", "other");
                Generic("animated", "SBase.SAnimatedMesh", "other");
                Generic("tutorials", "SBase.SZoneTips", "other");
                Meshes();
            }
            void LightsOf(const Json& item, const std::string& pointer, const std::string& id, const Vector& at, int roomIndex)
            {
                const auto preset = item.value("preset", std::string("ceiling"));
                const double radius = item.contains("radius") ? L(item.at("radius")) : 400;
                const bool switchable = item.value("switchable", false);
                int pitch = 0;
                if (preset == "spot" || preset == "echelon-spot") pitch = YawUnits(item.value("pitch", -90.0));
                else if (preset == "sun") pitch = YawUnits(item.value("pitch", -45.0));
                else if (item.contains("pitch")) pitch = YawUnits(item.at("pitch").get<double>());
                auto& light = Add(id, switchable ? "SBase.STriggerLight" : "Engine.Light", "light", pointer, at, 0);
                light.properties["Rotation"] = Rot({pitch, item.contains("yaw") ? YawUnits(item.at("yaw").get<double>()) : 0, 0});
                light.label = preset + " light";
                light.properties["LightRadius"] = Num(std::round(radius / 25 * 1000) / 1000);
                light.properties["EchelonRange"] = Num(radius);
                light.properties["LightBrightness"] = Num(item.value("brightness", preset == "ceiling" ? 64.0 : 96.0));
                light.properties["LightHue"] = Num(item.value("hue", 0.0));
                light.properties["LightSaturation"] = Num(item.value("saturation", 255.0));
                light.properties["LightType"] = "LT_Steady";
                if (preset == "echelon-spot")
                {
                    light.properties["LightEffect"] = "LE_ESpotShadow";
                    light.properties["EchelonNormalInfluence"] = "0.9";
                    light.properties["NearPlaneRatio"] = "0.05";
                }
                else light.properties["LightEffect"] = preset == "spot" ? "LE_Spotlight" : preset == "sun" ? "LE_Sunlight" : "LE_None";
                // LightCone shapes LE_Spotlight's beam only; without cone it keeps the class default.
                if (item.contains("cone"))
                {
                    if (preset == "spot") light.properties["LightCone"] = std::to_string(item.at("cone").get<long long>());
                    else Diag("warning", "light.cone", Child(pointer, "cone"), "cone (LightCone) shapes the spot preset's LE_Spotlight beam only, so it is not written for a " + preset + " light.", "lighting");
                }
                if (preset == "sun") Diag("info", "light.sun", pointer, "LE_Sunlight shines from a Fake Backdrop surface; the zone needs one.", "lighting");
                if (item.value("corona", false)) light.properties["bCorona"] = "True";
                Props(light, item);
                const auto effect = light.properties.value("LightEffect", std::string());
                static const std::set<std::string> allowed{"le_none", "le_spotlight", "le_sunlight", "le_espotshadow"};
                if (!allowed.count(Fold(effect))) Diag("warning", "light.effect", pointer, id + " uses LightEffect " + effect + "; static lights work with LE_None, LE_Spotlight or LE_Sunlight only.", "lighting");
                lightIds.insert(Fold(id));
                if (roomIndex >= 0) litRooms.insert(Fold(low.pieces[static_cast<size_t>(roomIndex)].id));
            }
            std::set<std::string> litRooms;
            void Lights()
            {
                const auto& lights = List(Section({"environment", "lights"}));
                for (size_t i = 0; i < lights.size(); ++i)
                {
                    const auto& item = lights[i];
                    const auto pointer = Child("/environment/lights", i);
                    if (FindActor(item.at("id").get<std::string>())) continue; // made early for a trigger light
                    Where spot;
                    if (item.contains("at")) spot = Locate(item.at("at"), Child(pointer, "at"), Ceiling(24));
                    else if (item.contains("in")) spot = Locate({{"in", item.at("in")}}, Child(pointer, "in"), Ceiling(24));
                    else { Diag("error", "light.at", pointer, "a light needs at or in.", "lighting"); continue; }
                    LightsOf(item, pointer, item.at("id").get<std::string>(), spot.at, spot.room);
                }
            }
            // A ceiling light in each room no light of the spec is in.
            void RoomLights()
            {
                if (Section({"defaults"}).value("lights", std::string("rooms")) != "rooms") return;
                for (const auto& p : low.pieces)
                {
                    if (p.kind != "Room" || p.generated || Design::SpansFloors(p.spec.value("name", std::string())) || litRooms.count(Fold(p.id))) continue;
                    const Vector at{p.pose.position[0], p.pose.position[1], p.box.hi[2] - 24};
                    LightsOf(Json::object(), p.pointer, NewId(p.id + "_Light"), at, -1);
                }
            }
            void Sounds()
            {
                const auto& sounds = List(Section({"environment", "sounds"}));
                for (size_t i = 0; i < sounds.size(); ++i)
                {
                    const auto& item = sounds[i];
                    const auto pointer = Child("/environment/sounds", i);
                    if (!item.contains("at")) { Diag("error", "sound.at", pointer, "a sound needs at.", "sound-actors"); continue; }
                    const auto cls = item.value("class", std::string("Engine.AmbientSound"));
                    const auto shortName = Fold(cls.substr(cls.find_last_of('.') + 1));
                    const auto at = Locate(item.at("at"), Child(pointer, "at"), Floor(128)).at;
                    auto& sound = Add(item.at("id").get<std::string>(), cls.find('.') == std::string::npos ? (shortName == "soundtrigger" || shortName == "ambientsound" || shortName == "musictrigger" ? "Engine." + cls : "SBase." + cls) : cls, "sound", pointer, at, 0, false);
                    const auto asset = Asset("Sound", item.at("sound").get<std::string>());
                    Package(asset);
                    sound.properties[shortName == "musictrigger" ? "Song" : shortName == "soundtrigger" ? "Sound" : "AmbientSound"] = asset;
                    if (item.contains("radius")) sound.properties["SoundRadius"] = Num(L(item.at("radius")));
                    if (item.contains("volume")) sound.properties["SoundVolume"] = Num(item.at("volume").get<double>());
                    Props(sound, item);
                }
            }
            void Volumes()
            {
                const auto& volumes = List(Section({"environment", "volumes"}));
                for (size_t i = 0; i < volumes.size(); ++i)
                {
                    const auto& item = volumes[i];
                    const auto pointer = Child("/environment/volumes", i);
                    Vector lo{}, hi{};
                    if (!BoxOf(item, pointer, 0, lo, hi)) continue;
                    auto cls = item.value("class", std::string("Volume"));
                    if (cls.find('.') == std::string::npos)
                    {
                        if (cls == "Volume" || cls == "BlockingVolume") cls = "Engine." + cls;
                        else if (cls == "ERainVolume") cls = "Echelon." + cls;
                        else cls = "SBase." + cls;
                    }
                    if (Fold(cls).find("salarmzoneinfo") != std::string::npos) Diag("warning", "volume.alarm-zone", pointer, "SAlarmZoneInfo is never used in shipped maps; use a Volume with an SVolumetricSensor.", "zones-and-volumes");
                    const Vector centre{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
                    auto& volume = Add(item.at("id").get<std::string>(), cls, "volume", pointer, centre, 0, false);
                    volume.geometry = "polygons";
                    volume.footprint = true;
                    volume.storey = StoreyBelow(lo[2]);
                    volume.polygons = BoxPolygons({(hi[0] - lo[0]) / 2, (hi[1] - lo[1]) / 2, (hi[2] - lo[2]) / 2});
                    if (item.contains("locationName")) { volume.properties["LocationName"] = Quoted(item.at("locationName").get<std::string>()); volume.label = item.at("locationName").get<std::string>(); }
                    if (item.contains("priority")) volume.properties["LocationPriority"] = Num(item.at("priority").get<double>());
                    if (Fold(cls).find("erainvolume") != std::string::npos)
                        volume.properties["m_RainSize"] = Loc({(hi[0] - lo[0]) / 2, (hi[1] - lo[1]) / 2, hi[2] - lo[2]});
                    Props(volume, item);
                }
            }
            void Ingredients()
            {
                struct Kind { const char* name; const char* cls; const char* stem; double above; };
                static const Kind kinds[] = {{"ammo", "SBase.SAmmoStock", "AmmoStock", 0}, {"infirmary", "SBase.SInfirmary", "Infirmary", 64}, {"glass", "SBase.SVitre", "Window", 0},
                                             {"extinguisher", "SBase.SExtincteur", "Extinguisher", 0}, {"projector", "SBase.SProjoFixe", "Spotlight", 0},
                                             {"sprinkler", "SBase.SSprinkler", "Sprinkler", 0}, {"puddle", "SBase.SFlaque", "Puddle", 0}, {"balltrap", "SBase.SBallTrap", "BallTrap", 0}};
                const auto& list = List(Section({"environment", "ingredients"}));
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& item = list[i];
                    const auto pointer = Child("/environment/ingredients", i);
                    const auto kind = item.at("kind").get<std::string>();
                    const Kind* found = nullptr;
                    for (const auto& k : kinds) if (kind == k.name) found = &k;
                    Where spot;
                    int facing = 0;
                    if (item.contains("at")) spot = Locate(item.at("at"), Child(pointer, "at"), Floor(found->above));
                    else if (item.contains("in"))
                    {
                        // Against the room's +Y wall, spread along it, facing in:
                        // clear of the starts a spawn room holds in its middle.
                        const auto room = item.at("in").get<std::string>();
                        const int index = FindPiece(room);
                        if (index < 0) { Diag("error", "ingredient.at", Child(pointer, "in"), "no piece has the id "" + room + "".", "level-ingredients"); continue; }
                        const auto& p = low.pieces[static_cast<size_t>(index)];
                        const int k = alongWall[Fold(room)]++;
                        const double w = p.spec.at("width"), l = p.spec.at("length");
                        const double u = k % 3 == 0 ? 0.0 : k % 3 == 1 ? -0.25 : 0.25;
                        spot.at = Workflow::TransformPoint({u * w, l / 2 - 32, found->above}, p.pose);
                        spot.room = p.space ? index : -1;
                        facing = (p.pose.rotation[1] + 65536 - 16384) % 65536;
                    }
                    else { Diag("error", "ingredient.at", pointer, "an ingredient needs at or in.", "level-ingredients"); continue; }
                    const auto id = item.contains("id") ? item.at("id").get<std::string>() : NewId(found->stem);
                    auto& actor = Add(id, found->cls, "ingredient", pointer, spot.at, YawOf(item, spot.at, pointer, facing));
                    actor.label = kind;
                    if (kind == "ammo") { actor.properties["fInteractionHeight"] = "60"; actor.properties["fInteractionRadius"] = "120"; actor.properties["bInteractInFront"] = "True"; }
                    else if (kind == "infirmary")
                    {
                        actor.properties["bBlockActors"] = "True";
                        actor.properties["bBlockPlayers"] = "True";
                        actor.properties["fInteractionHeight"] = "120";
                        actor.properties["fInteractionRadius"] = "60";
                        actor.properties["bInteractInFront"] = "True";
                    }
                    else if (kind == "sprinkler") actor.properties["bHidden"] = "True";
                    Props(actor, item);
                }
            }
            void Objects()
            {
                const auto& list = List(Section({"environment", "objects"}));
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& item = list[i];
                    const auto pointer = Child("/environment/objects", i);
                    const auto at = Locate(item.at("at"), Child(pointer, "at"), Floor(0)).at;
                    auto& object = Add(item.at("id").get<std::string>(), "SBase.SGamePlayObject", "other", pointer, at, YawOf(item, at, pointer, 0));
                    Mesh(object, item.at("mesh").get<std::string>());
                    object.properties["bPickable"] = "False";
                    if (item.contains("damageable")) object.properties["bDamageable"] = Flag(item.at("damageable").get<bool>());
                    if (item.contains("hitPoints")) object.properties["HitPoints"] = Num(item.at("hitPoints").get<double>());
                    Props(object, item);
                    if (Fold(object.properties.value("bPickable", std::string())) == "true") Diag("warning", "object.pickable", pointer, "bPickable is unfinished in the game; leave it False.", "gameplay-objects");
                    objectIds.insert(Fold(object.id));
                }
            }
            // A class written without its package: Engine's own actors, the
            // SoftBody package's ESB classes (SoftBody.ESBPatchActor, as the
            // engine names it), Echelon's other E classes, SBase for the rest
            // (the live schema corrects a guess). The wiki's ESBStripeDoorActor
            // is the engine's ESBStripDoorActor, and an ESB class is never in
            // Echelon, which earlier builds guessed.
            static std::string FullClass(const std::string& given)
            {
                const auto dot = given.find_last_of('.');
                std::string package = dot == std::string::npos ? std::string() : given.substr(0, dot), cls = given.substr(dot == std::string::npos ? 0 : dot + 1);
                const auto shortName = Fold(cls);
                if (shortName == "esbstripedooractor") cls = "ESBStripDoorActor";
                const bool softBody = shortName.rfind("esb", 0) == 0;
                if (softBody && (package.empty() || Fold(package) == "echelon")) return "SoftBody." + cls;
                if (!package.empty()) return package + "." + cls;
                static const std::set<std::string> engine{"actor", "staticmeshactor", "mover", "light", "playerstart", "volume", "blockingvolume", "zoneinfo", "ambientsound",
                                                          "soundtrigger", "musictrigger", "note", "brush", "emitter", "trigger"};
                if (engine.count(shortName)) return "Engine." + cls;
                if (cls.size() > 1 && cls[0] == 'E' && std::isupper(static_cast<unsigned char>(cls[1]))) return "Echelon." + cls;
                return "SBase." + cls;
            }
            void Generic(const char* section, const char* cls, const char* role)
            {
                const std::string kind = section;
                const auto& list = List(Section({"environment", section}));
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& item = list[i];
                    const auto pointer = Child(std::string("/environment/") + section, i);
                    if (!item.contains("at")) { Diag("error", "actor.at", pointer, "needs at."); continue; }
                    const auto type = FullClass(item.value("class", std::string(cls)));
                    const auto shortType = Fold(type.substr(type.find_last_of('.') + 1));
                    if (kind == "softbody" && shortType == "esoftbodyactor")
                    {
                        Diag("error", "softbody.class", Child(pointer, "class"), "ESoftBodyActor is the SoftBody base class and cannot be placed; use ESBPatchActor (the usual cloth), ESBStripDoorActor (the wiki's ESBStripeDoorActor), ESBRopeActor or ESBChainActor.", "softbody-actors");
                        continue;
                    }
                    const auto at = Locate(item.at("at"), Child(pointer, "at"), Floor(0)).at;
                    auto& actor = Add(item.at("id").get<std::string>(), type, role, pointer, at, YawOf(item, at, pointer, 0));
                    if (item.contains("mesh"))
                    {
                        const auto mesh = item.at("mesh").get<std::string>();
                        const auto given = Fold(mesh.substr(0, mesh.find('\'')));
                        if (kind == "softbody")
                        {
                            // A SoftBody draws its own cloth (DT_SoftBody); a mesh is a
                            // chain's links, and only ESBChainActor has LinkMesh.
                            if (shortType == "esbpatchactor" || shortType == "esbstripdooractor" || shortType == "esbropeactor")
                                Diag("error", "softbody.mesh", Child(pointer, "mesh"), type + " has no mesh: it draws its cloth with the texture in properties.Texture. Only ESBChainActor takes a mesh (LinkMesh, its links).", "softbody-actors");
                            else
                            {
                                actor.properties["LinkMesh"] = Asset("StaticMesh", mesh);
                                Package(Asset("StaticMesh", mesh));
                            }
                        }
                        else if ((kind == "animated" || kind == "fish") && (mesh.find('\'') == std::string::npos || given != "staticmesh"))
                        {
                            // An animated mesh, not a static one.
                            actor.properties["Mesh"] = Asset("Mesh", mesh);
                            actor.properties["DrawType"] = "DT_Mesh";
                            Package(Asset("Mesh", mesh));
                        }
                        else Mesh(actor, mesh);
                    }
                    if (kind == "tutorials")
                    {
                        const auto shortName = Fold(type.substr(type.find_last_of('.') + 1));
                        // Shown in the editor as a sprite, never in game, and only in
                        // Visit a Map's tutorials (scct://wiki/map-tutorials).
                        if (shortName == "szonetips" || shortName == "sendzonetip")
                        {
                            actor.properties["DrawType"] = "DT_Sprite";
                            actor.properties["bHidden"] = "True";
                            actor.properties["bShowOnlyWithIngameHelp"] = "True";
                            actor.properties["bStayPermanently"] = "False";
                        }
                    }
                    if (item.contains("scale"))
                    {
                        const auto& s = item.at("scale");
                        if (s.is_number()) actor.properties["DrawScale"] = Num(s.get<double>());
                        else actor.properties["DrawScale3D"] = Loc({s[0].get<double>(), s[1].get<double>(), s[2].get<double>()});
                    }
                    Props(actor, item);
                    // Two entries alike in everything but their id are one cloth twice.
                    if (kind == "softbody")
                        for (size_t k = 0; k < i; ++k)
                        {
                            auto a = list[k], b = item;
                            a.erase("id");
                            b.erase("id");
                            if (a == b) Diag("warning", "softbody.duplicate", pointer, "this SoftBody repeats " + list[k].at("id").get<std::string>() + " in the same place with the same properties. Each SoftBody is its own actor (never a duplicate of another); place this one where its own cloth goes.", "softbody-actors");
                        }
                }
            }
            void Meshes()
            {
                const auto& list = List(Section({"geometry", "meshes"}));
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& item = list[i];
                    const auto pointer = Child("/geometry/meshes", i);
                    const auto at = Locate(item.at("at"), Child(pointer, "at"), Floor(0)).at;
                    auto& mesh = Add(item.at("id").get<std::string>(), "Engine.StaticMeshActor", "other", pointer, at, YawOf(item, at, pointer, 0));
                    Mesh(mesh, item.at("mesh").get<std::string>());
                    mesh.label = item.at("mesh").get<std::string>();
                    if (item.contains("scale"))
                    {
                        const auto& s = item.at("scale");
                        if (s.is_number()) mesh.properties["DrawScale"] = Num(s.get<double>());
                        else mesh.properties["DrawScale3D"] = Loc({s[0].get<double>(), s[1].get<double>(), s[2].get<double>()});
                    }
                    Props(mesh, item);
                }
            }

            // --- Events ----------------------------------------------------------
            void Events()
            {
                const auto& magic = List(Section({"events", "magic"}));
                for (size_t i = 0; i < magic.size(); ++i)
                {
                    const auto& item = magic[i];
                    const auto pointer = Child("/events/magic", i);
                    const auto& document = item.at("document");
                    if (document.value("format", std::string()) != "scct.smagic-event" || document.value("version", 0) != 1 || !document.contains("properties") || !document.at("properties").is_object())
                    { Diag("error", "events.magic", Child(pointer, "document"), "document is an scct.smagic-event version 1 file with properties (SMagicEvent Workbench > Export JSON).", "events"); continue; }
                    const auto at = item.contains("at") ? Locate(item.at("at"), Child(pointer, "at"), Floor(64)).at : Fallback(112);
                    auto& event = Add(item.at("id").get<std::string>(), document.value("class", std::string("SBase.SMagicEvent")), "other", pointer, at, 0, false);
                    for (auto it = document.at("properties").begin(); it != document.at("properties").end(); ++it)
                        if (it.key() != "Tag" && it.key() != "Location") event.properties[it.key()] = it.value();
                    if (!event.properties.contains("Groups")) event.properties["Groups"] = Json::array();
                    magicIds.insert(Fold(event.id));
                }
                const auto& messages = List(Section({"events", "messages"}));
                for (size_t i = 0; i < messages.size(); ++i)
                {
                    const auto& item = messages[i];
                    const auto pointer = Child("/events/messages", i);
                    const auto at = item.contains("at") ? Locate(item.at("at"), Child(pointer, "at"), Floor(64)).at : Fallback(128);
                    auto& message = Add(item.at("id").get<std::string>(), "SBase.SMessageTrigger", "other", pointer, at, 0, false);
                    message.properties["MessageSpy"] = Quoted(item.value("spy", std::string()));
                    message.properties["MessageMerc"] = Quoted(item.value("merc", std::string()));
                    message.properties["DisplayTime"] = Num(item.value("time", 5.0));
                    message.properties["Priority"] = std::to_string(item.value("priority", 0));
                }
                for (const auto& [section, cls] : std::vector<std::pair<const char*, const char*>>{{"shakers", "SBase.SShaker"}, {"noises", "SBase.SNotifyNoise"}})
                {
                    const auto& list = List(Section({"events", section}));
                    for (size_t i = 0; i < list.size(); ++i)
                    {
                        const auto& item = list[i];
                        const auto pointer = Child(std::string("/events/") + section, i);
                        const auto at = item.contains("at") ? Locate(item.at("at"), Child(pointer, "at"), Floor(64)).at : Fallback(144);
                        auto& actor = Add(item.at("id").get<std::string>(), FullClass(item.value("class", std::string(cls))), "other", pointer, at, YawOf(item, at, pointer, 0));
                        Props(actor, item);
                    }
                }
                ButtonTargets();
                const auto& links = List(Section({"events", "links"}));
                for (size_t i = 0; i < links.size(); ++i)
                {
                    const auto& item = links[i];
                    const auto pointer = Child("/events/links", i);
                    const auto from = item.at("from").get<std::string>(), to = item.at("to").get<std::string>();
                    const bool fromMagic = magicIds.count(Fold(from)) != 0, toMagic = magicIds.count(Fold(to)) != 0;
                    auto* source = FindActor(from);
                    const auto delay = Delay(item, pointer, "events.delay", "events");
                    if (delay.empty()) continue;
                    if (source && fromMagic)
                    {
                        auto& groups = source->properties["Groups"];
                        if (!groups.is_array()) groups = Json::array();
                        if (groups.empty()) groups.push_back({{"EventGroup", Json::array()}, {"Repeat", "0"}, {"Sequence", "False"}, {"SequenceIndex", "0"}});
                        if (!refs.count(Fold(to)) || !refs.at(Fold(to)).tag.empty())
                            groups[0]["EventGroup"].push_back({{"Delay", delay}, {"Event", TagOf(to, Child(pointer, "to"), true)}, {"Type", item.value("untrigger", false) ? "EVT_Untrigger" : "EVT_Trigger"}, {"ValidOn", "EVT_Trigger"}});
                        else if (refs.count(Fold(to)))
                            low.links.push_back({{"op", "link"}, {"event", from}, {"target", refs.at(Fold(to)).path}, {"trigger", false}, {"delay", delay}});
                    }
                    else if (source)
                    {
                        if (item.contains("delay") && item.at("delay").get<double>() > 0) Diag("warning", "events.delay", Child(pointer, "delay"), "a delay belongs on an SMagicEvent's action; " + from + " fires at once.", "events");
                        SetEvent(*source, TagOf(to, Child(pointer, "to"), true), pointer);
                    }
                    else if (refs.count(Fold(from)))
                    {
                        if (!toMagic) { Diag("error", "events.link", pointer, "a live actor (existing.refs) can only be linked to an SMagicEvent of the spec; send it to an events.magic id.", "events"); continue; }
                        if (refs.at(Fold(from)).path.empty()) { Diag("error", "ref.path", Child(pointer, "from"), "existing.refs." + from + " needs its path for a link.", "events"); continue; }
                        low.links.push_back({{"op", "link"}, {"event", to}, {"target", refs.at(Fold(from)).path}, {"trigger", true}});
                    }
                    else { Diag("error", "ref.unresolved", Child(pointer, "from"), "Nothing in the spec has the id \"" + from + "\".", "events"); continue; }
                    if (source)
                        if (const auto* target = FindActor(to)) low.lines.push_back({{"from", Point(source->at)}, {"to", Point(target->at)}, {"kind", "event"}, {"ids", {from, to}}});
                }
                const auto& triggerLights = List(Section({"events", "triggerLights"}));
                for (size_t i = 0; i < triggerLights.size(); ++i)
                {
                    const auto& item = triggerLights[i];
                    const auto pointer = Child("/events/triggerLights", i);
                    PrepareLight(item.at("light").get<std::string>(), Child(pointer, "light"));
                    auto* light = FindActor(item.at("light").get<std::string>());
                    if (!light || !lightIds.count(Fold(light->id))) { Diag("error", "events.trigger-light", Child(pointer, "light"), "\"" + item.at("light").get<std::string>() + "\" is not a light of environment.lights.", "gameplay-objects"); continue; }
                    light->cls = ClassName("SBase.STriggerLight");
                    if (item.contains("initiallyOn")) light->properties["bInitiallyOn"] = Flag(item.at("initiallyOn").get<bool>());
                    if (item.contains("object"))
                    {
                        auto* object = FindActor(item.at("object").get<std::string>());
                        if (!object || !objectIds.count(Fold(object->id))) { Diag("error", "events.trigger-light", Child(pointer, "object"), "\"" + item.at("object").get<std::string>() + "\" is not an environment.objects SGamePlayObject.", "gameplay-objects"); continue; }
                        auto& list = object->properties["ObjectLights"];
                        if (!list.is_array()) list = Json::array();
                        list.push_back(Ref(light->id));
                    }
                }
                const auto& vision = List(Section({"environment", "vision"}));
                for (size_t i = 0; i < vision.size(); ++i)
                {
                    const auto& item = vision[i];
                    const auto pointer = Child("/environment/vision", i);
                    Json properties = Json::object();
                    for (const auto& [key, property] : std::vector<std::pair<const char*, const char*>>{{"heatIntensity", "HeatIntensity"}, {"heatOpacity", "HeatOpacity"}, {"heatRadius", "HeatRadius"}, {"emfIntensity", "EMFIntensity"}, {"emfOpacity", "EMFOpacity"}})
                        if (item.contains(key)) properties[property] = Num(item.at(key).get<double>());
                    // Visible only in Thermal/EMF Vision: the base of the "event-gated reveal" pattern (pair with bHidden + an event un-hide).
                    if (item.contains("hideInNormalVision")) properties["bHideInNormalVision"] = item.at("hideInNormalVision").get<bool>() ? "True" : "False";
                    if (item.contains("properties")) for (auto it = item.at("properties").begin(); it != item.at("properties").end(); ++it) properties[it.key()] = Encode(it.value());
                    const auto target = item.at("target").get<std::string>();
                    if (auto* actor = FindActor(target)) { for (auto it = properties.begin(); it != properties.end(); ++it) actor->properties[it.key()] = it.value(); }
                    else if (refs.count(Fold(target)) && !refs.at(Fold(target)).path.empty())
                    {
                        auto& update = low.updates[refs.at(Fold(target)).path];
                        update["actor"] = {{"path", refs.at(Fold(target)).path}, {"class", refs.at(Fold(target)).cls}};
                        if (!update.contains("properties")) update["properties"] = Json::object();
                        for (auto it = properties.begin(); it != properties.end(); ++it) update["properties"][it.key()] = it.value();
                        if (refs.at(Fold(target)).cls.empty()) Diag("error", "ref.class", Child(pointer, "target"), "existing.refs." + target + " needs its class to be edited.", "vision-mode-properties");
                    }
                    else Diag("error", "ref.unresolved", Child(pointer, "target"), "\"" + target + "\" is not an actor of the spec or a live actor with a path.", "vision-mode-properties");
                }
                const auto& triggers = List(Section({"geTriggers"}));
                for (size_t i = 0; i < triggers.size(); ++i)
                {
                    const auto& item = triggers[i];
                    const auto pointer = Child("/geTriggers", i);
                    const auto at = item.contains("at") ? Locate(item.at("at"), Child(pointer, "at"), Floor(64)).at : Fallback(160);
                    auto& trigger = Add(item.at("id").get<std::string>(), "SBase.SGETrigger", "ge", pointer, at, 0, false);
                    trigger.properties["bToggle"] = Flag(item.value("toggle", false));
                    trigger.properties["GEMask"] = std::to_string(item.at("geMask").get<long long>());
                }
            }
            // A light a trigger-light entry names before environment.lights runs.
            void PrepareLight(const std::string& id, const std::string& pointer)
            {
                if (FindActor(id)) return;
                const auto& lights = List(Section({"environment", "lights"}));
                for (size_t i = 0; i < lights.size(); ++i)
                {
                    if (Fold(lights[i].at("id").get<std::string>()) != Fold(id)) continue;
                    const auto where = Child("/environment/lights", i);
                    const auto& item = lights[i];
                    Where spot;
                    if (item.contains("at")) spot = Locate(item.at("at"), Child(where, "at"), Ceiling(24));
                    else if (item.contains("in")) spot = Locate({{"in", item.at("in")}}, Child(where, "in"), Ceiling(24));
                    else return;
                    LightsOf(item, where, id, spot.at, spot.room);
                    return;
                }
                (void)pointer;
            }

            // --- GEs, settings, interface, raw operations --------------------------
            void Ges()
            {
                struct Kind { const char* name; const char* command; size_t points; };
                static const Kind kinds[] = {{"ledge", "LG", 2}, {"hoh", "HOH", 2}, {"pipe", "PIPE", 3}, {"ladder", "LADDER", 3}, {"zipline", "ZL", 2}, {"fence", "FENCE", 2}, {"pole", "POLE", 2}};
                const auto& list = List(Section({"ges"}));
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& item = list[i];
                    const auto pointer = Child("/ges", i);
                    const auto kind = item.at("kind").get<std::string>();
                    const Kind* found = nullptr;
                    for (const auto& k : kinds) if (kind == k.name) found = &k;
                    const auto& given = item.at("points");
                    if (given.size() != found->points) { Diag("error", "ge.points", Child(pointer, "points"), "a " + kind + " takes " + std::to_string(found->points) + " points" + (found->points == 3 ? ": both ends and the side the player climbs from." : ": its two ends."), "geometric-events"); continue; }
                    std::vector<Vector> points;
                    for (size_t k = 0; k < given.size(); ++k) points.push_back(Locate(given[k], Child(Child(pointer, "points"), k), Floor(0)).at);
                    // A pipe or ladder starts at its lower end, its side point at that height.
                    if (found->points == 3)
                    {
                        if (points[0][2] > points[1][2]) std::swap(points[0], points[1]);
                        points[2][2] = points[0][2];
                    }
                    Json out = Json::array();
                    for (const auto& p : points) { Design::CheckVector(p); out.push_back(Point(p)); }
                    low.ges.push_back({{"kind", kind}, {"command", found->command}, {"points", out}, {"pointer", pointer}});
                    low.lines.push_back({{"from", Point(points[0])}, {"to", Point(points[1])}, {"kind", "ge-" + kind}, {"ids", Json::array()}});
                }
            }
            void Settings()
            {
                const auto& level = Section({"level"});
                Json changes = Json::object();
                if (level.contains("song")) changes["Song"] = level.at("song");
                if (level.contains("deathPawnHeight")) changes["fDeathPawnHeight"] = Num(L(level.at("deathPawnHeight")));
                // The Level Properties sheet's headings (Audio, Death, Events,
                // MapSnapshotInfo, MotionTrackingTweak) are categories, not
                // properties: Motion Tracking Vision is MTVOffset and MTVSlope.
                if (level.contains("motionTracking"))
                {
                    const bool enhanced = level.at("motionTracking") == "enhanced";
                    changes["MTVOffset"] = enhanced ? "8" : "52";
                    changes["MTVSlope"] = enhanced ? "8" : "85";
                }
                if (level.contains("geActivationMask")) changes["GEActivationMask"] = std::to_string(level.at("geActivationMask").get<long long>());
                // A LevelInfo sends one Event (Actor's), at SendAtTime: an int,
                // seconds counted back from the end of the match when negative
                // (the stock tests/maps/OffsE saves both at the LevelInfo's top
                // level). Several level events go through one SMagicEvent sent at
                // the earliest time, each delayed by how much later it is due, so
                // they all hang off the earliest: an SMagicEvent delays by a day
                // at most, and a match shorter than the earliest time fires none.
                struct LevelEvent { std::string tag; double at; std::string pointer; };
                std::vector<LevelEvent> events;
                const Json levelEvents = level.is_object() ? List(level.value("events", Json::array())) : Json::array();
                const std::pair<const Json*, const char*> sources[] = {{&levelEvents, "/level/events"}, {&List(Section({"events", "level"})), "/events/level"}};
                for (const auto& [source, base] : sources)
                    for (size_t i = 0; i < source->size(); ++i)
                    {
                        const auto& item = (*source)[i];
                        const auto where = Child(base, i);
                        const double given = item.at("sendAtTime").get<double>(), at = std::round(given);
                        if (at >= 0) Diag("warning", "level.send-at-time", Child(where, "sendAtTime"), "SendAtTime counts back from the end of the match: a time of 0 or more never fires. Use a negative time.", "level-properties");
                        if (at != given) Diag("warning", "level.send-at-time", Child(where, "sendAtTime"), "SendAtTime is a whole number of seconds; " + Num(given) + " is written as " + Num(at) + ".", "level-properties");
                        events.push_back({TagOf(item.at("event").get<std::string>(), Child(where, "event"), false), at, Child(where, "sendAtTime")});
                    }
                if (events.size() == 1) { changes["Event"] = events[0].tag; changes["SendAtTime"] = Num(events[0].at); }
                else if (events.size() > 1)
                {
                    double first = events[0].at;
                    for (const auto& e : events) first = std::min(first, e.at);
                    Json entries = Json::array();
                    for (const auto& e : events)
                    {
                        if (e.at - first > 86400)
                        {
                            Diag("error", "level.send-at-time", e.pointer, e.tag + " is due " + Num(e.at - first) + " seconds after the earliest level event (SendAtTime " + Num(first) +
                                 "). Level events go out through one SMagicEvent the LevelInfo sends at the earliest time, which delays each by at most 86400 seconds, so this one is left out: keep the times within a day of each other.", "level-properties");
                            continue;
                        }
                        entries.push_back({{"Delay", Workflow::Magic::Seconds(e.at - first)}, {"Event", e.tag}, {"Type", "EVT_Trigger"}, {"ValidOn", "EVT_Trigger"}});
                    }
                    const auto fan = NewId("LevelEvents");
                    auto& event = Add(fan, "SBase.SMagicEvent", "other", levelEvents.empty() ? "/events/level" : "/level/events", Fallback(176), 0, false);
                    event.label = "level events";
                    event.properties["Groups"] = Json::array({{{"EventGroup", entries}, {"Repeat", "0"}, {"Sequence", "False"}, {"SequenceIndex", "0"}}});
                    magicIds.insert(Fold(fan));
                    changes["Event"] = fan;
                    changes["SendAtTime"] = Num(first);
                }
                for (auto it = changes.begin(); it != changes.end(); ++it)
                    if (const auto* schema = PropertySchema("Engine.LevelInfo", it.key())) it.value() = Complete(it.value(), *schema, "LevelInfo." + it.key(), "/level");
                low.settings = changes;
                const auto& settings = Section({"settings"});
                const auto& gameplay = Section({"gameplay"});
                if (gameplay.is_object()) for (const auto& m : List(gameplay.value("modes", Json::array()))) low.modes.push_back(m.get<std::string>());
                if (low.modes.empty())
                {
                    if (gameplay.contains("story")) low.modes.push_back("story");
                    if (gameplay.contains("diskhunt")) low.modes.push_back("diskhunt");
                    if (gameplay.contains("deathmatch")) low.modes.push_back("deathmatch");
                }
                for (const auto& m : low.modes)
                    if (!gameplay.contains(m)) Diag("warning", "modes.missing", "/gameplay/modes", "gameplay.modes lists " + m + " but gameplay has no " + m + " section, so the map has no mission for it.", "game-modes");
                if (!settings.is_object()) return;
                const auto mapName = settings.value("mapName", Section({"map"}).value("name", std::string()));
                Json map{{"MapName", Quoted(mapName)}};
                if (settings.contains("briefSpy")) map["ExpliMapInBriefSpy"] = Quoted(settings.at("briefSpy").get<std::string>());
                if (settings.contains("briefMerc")) map["ExpliMapInBriefSentry"] = Quoted(settings.at("briefMerc").get<std::string>());
                if (settings.contains("legend")) map["LegendMap"] = Quoted(settings.at("legend").get<std::string>());
                if (settings.contains("legendInBrief")) map["LegendMapInBrief"] = Quoted(settings.at("legendInBrief").get<std::string>());
                if (mapName != Upper(mapName)) Diag("warning", "settings.caps", "/settings/mapName", "MapName is in capitals in every Ubisoft map.", "map-settings");
                Json modes = Json::array(), strings = Json::array();
                auto list = List(settings.value("modes", Json::array()));
                if (list.empty()) for (const auto& m : low.modes) list.push_back({{"mode", m}});
                static const std::map<std::string, std::string> legends{
                    {"story", "The spies must accomplish a certain number of mission objectives. The objectives can be of three kinds. The Mercenaries must prevent them or eliminate the spies."},
                    {"diskhunt", "The spies must go through the level and collect a certain number of disks. The Mercenaries must prevent them or eliminate the spies."},
                    {"deathmatch", "Both spies and mercenaries must eliminate the opposing team."}};
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& mode = list[i];
                    const auto pointer = Child("/settings/modes", i);
                    const auto kind = mode.at("mode").get<std::string>();
                    const auto check = [&](const char* key, double lo, double hi)
                    {
                        if (mode.contains(key) && (mode.at(key).get<double>() < lo || mode.at(key).get<double>() > hi))
                            Diag("warning", "settings.range", Child(pointer, key), std::string(key) + " is supported from " + Num(lo) + " to " + Num(hi) + ".", "map-settings");
                    };
                    check("respawn", 5, 15);
                    check("spiesLives", 0, 10);
                    check("sentriesLives", 0, 10);
                    check("timeLimit", 0, 20);
                    Json entry{{"GameModeId", kind == "story" ? "GM_Multi" : kind == "diskhunt" ? "GM_DiskHunt" : "GM_DeathMatch"}, {"RespawnTime", Num(mode.value("respawn", 10.0))},
                               {"TimeLimit", Num(mode.value("timeLimit", 10.0))}, {"SpiesLives", std::to_string(mode.value("spiesLives", 0))}, {"SentriesLives", std::to_string(mode.value("sentriesLives", 0))}};
                    if (kind == "diskhunt")
                    {
                        const int disks = std::max(diskCount, 0);
                        const int score = mode.value("scoreLimit", std::max(1, std::min(disks, 10)));
                        entry["ScoreLimit"] = std::to_string(score);
                        entry["Handicap"] = std::to_string(mode.value("handicap", disks));
                        entry["NumDisksMax"] = std::to_string(mode.value("numDisksMax", disks));
                        if (diskCount >= 0 && diskCount < score) Diag("error", "diskhunt.count", Child(pointer, "scoreLimit"), "the map has " + std::to_string(diskCount) + " disks but the spies need " + std::to_string(score) + " to win.", "game-modes");
                        if (diskCount >= 0 && mode.value("handicap", disks) > diskCount) Diag("error", "diskhunt.count", Child(pointer, "handicap"), "more active disks (Handicap) than disks placed.", "map-settings");
                    }
                    modes.push_back(entry);
                    const auto s = mode.value("strings", Json::object());
                    strings.push_back({{"ExplicationModeSpy", Quoted(s.value("explicationSpy", std::string()))}, {"ExplicationModeSentry", Quoted(s.value("explicationMerc", std::string()))},
                                       {"ExpliModeInBriefSpy", Quoted(s.value("briefSpy", std::string()))}, {"ExpliModeInBriefSentry", Quoted(s.value("briefMerc", std::string()))},
                                       {"LegendModeInBriefSpy", Quoted(s.value("legendSpy", legends.at(kind)))}, {"LegendModeInBriefSentry", Quoted(s.value("legendMerc", legends.at(kind)))}});
                }
                map["Modes"] = modes;
                map["ModesStrings"] = strings;
                low.mapSettings = map;
            }
            static std::string Upper(std::string text)
            {
                for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                return text;
            }
            void Interface()
            {
                const auto& textures = Section({"interface"});
                for (const auto& [key, slot] : std::vector<std::pair<const char*, const char*>>{{"menu", "Menu"}, {"briefing", "Briefing"}, {"loadingAtt", "LoadingATT0"}, {"loadingDef", "LoadingDEF0"}})
                    if (textures.contains(key)) low.interfaceTextures.push_back({{"slot", slot}, {"file", textures.at(key)}});
            }
            void Extra()
            {
                const auto& list = List(Section({"extra", "operations"}));
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& op = list[i];
                    const auto kind = op.value("op", std::string());
                    if (kind == "load" || kind == "texture" || kind == "save") { low.packageOps.push_back(op); continue; }
                    if (kind == "delete") { Diag("error", "extra.delete", Child("/extra/operations", i), "deletes come from removing items from the spec (an incremental build), not from extra operations."); continue; }
                    if ((kind == "create" || kind == "component") && op.contains("id") && op.at("id").is_string())
                    {
                        const auto id = op.at("id").get<std::string>();
                        if (IsActor(id) || FindPiece(id) >= 0) { Diag("error", "extra.id", Child(Child("/extra/operations", i), "id"), "the id \"" + id + "\" is already used by the spec."); continue; }
                        usedIds.insert(Fold(id));
                    }
                    low.extraOps.push_back(op);
                }
                for (const auto& op : low.packageOps)
                    if (op.value("op", std::string()) == "texture") low.packages.erase(op.value("package", std::string()));
            }
        };

        // --- From lowered spec to plan ---------------------------------------------
        inline Json CreateOp(const Actor& actor)
        {
            Json op{{"op", "create"}, {"id", actor.id}, {"class", actor.cls}, {"properties", actor.properties}};
            if (actor.geometry != "point") { op["geometry"] = actor.geometry; op["polygons"] = actor.polygons; }
            return op;
        }
        inline Json Item(const Piece& piece, const Json& previous = Json())
        {
            return {{"spec", piece.spec}, {"position", piece.pose.position}, {"rotation", piece.pose.rotation}, {"previous", previous}};
        }
        inline Json Bind(const std::string& step, const std::string& pointer = {})
        {
            Json bind{{"step", step}};
            if (!pointer.empty()) bind["ptr"] = pointer;
            return {{"$bind", bind}};
        }
        inline Json Step(const std::string& id, const std::string& op, const Json& args, bool optional = false)
        {
            Json step{{"id", id}, {"op", op}, {"args", args}};
            if (optional) step["optional"] = true;
            return step;
        }
        inline Json Bounds(const Lowered& low)
        {
            bool any = false;
            Vector lo{}, hi{};
            auto take = [&](const Vector& a, const Vector& b)
            {
                for (int k = 0; k < 3; ++k) { lo[k] = any ? std::min(lo[k], a[k]) : a[k]; hi[k] = any ? std::max(hi[k], b[k]) : b[k]; }
                any = true;
            };
            for (const auto& p : low.pieces) take(p.box.lo, p.box.hi);
            for (const auto& a : low.actors) take(a.at, a.at);
            return {{"min", Point(lo)}, {"max", Point(hi)}};
        }
        // A polygon brush's top-down outline (the hull of its vertices) and
        // bounds, as a plan_render piece: add, subtract, volume or mover.
        inline Json Footprint(const Actor& a)
        {
            std::vector<Vector> points;
            for (const auto& polygon : a.polygons)
                for (const auto& v : polygon.at("vertices")) points.push_back({v[0].get<double>() + a.at[0], v[1].get<double>() + a.at[1], v[2].get<double>() + a.at[2]});
            if (points.empty()) return Json();
            Vector lo = points[0], hi = points[0];
            for (const auto& p : points) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
            std::sort(points.begin(), points.end(), [](const Vector& x, const Vector& y) { return x[0] < y[0] || (x[0] == y[0] && x[1] < y[1]); });
            auto turn = [](const Vector& o, const Vector& x, const Vector& y) { return (x[0] - o[0]) * (y[1] - o[1]) - (x[1] - o[1]) * (y[0] - o[0]); };
            std::vector<Vector> hull(points.size() * 2);
            size_t n = 0;
            for (const auto& p : points) { while (n >= 2 && turn(hull[n - 2], hull[n - 1], p) <= 0) --n; hull[n++] = p; }
            for (size_t i = points.size() - 1, lower = n + 1; i > 0; --i)
            {
                while (n >= lower && turn(hull[n - 2], hull[n - 1], points[i - 1]) <= 0) --n;
                hull[n++] = points[i - 1];
            }
            Json outline = Json::array();
            for (size_t i = 0; i + 1 < n; ++i) outline.push_back(Json::array({hull[i][0], hull[i][1]}));
            const auto cls = Fold(a.cls);
            const std::string kind = cls.find("volume") != std::string::npos ? "volume" : cls.find("mover") != std::string::npos ? "mover" :
                                     a.properties.value("CsgOper", std::string()) == "CSG_Subtract" ? "subtract" : "add";
            Json piece{{"id", a.id}, {"kind", kind}, {"name", a.label}, {"cls", a.cls}, {"storey", a.storey}, {"box", {{"min", Point(lo)}, {"max", Point(hi)}}}, {"yaw", 0}};
            if (outline.size() >= 3) piece["outline"] = outline;
            return piece;
        }
        inline Json Render(const Lowered& low)
        {
            Json pieces = Json::array(), doorways = Json::array(), actors = Json::array();
            for (const auto& p : low.pieces)
            {
                pieces.push_back({{"id", p.id}, {"kind", p.kind}, {"name", p.spec.value("name", p.id)}, {"storey", p.storey}, {"box", {{"min", Point(p.box.lo)}, {"max", Point(p.box.hi)}}},
                                  {"yaw", Degrees(p.pose.rotation[1])}, {"space", p.space}, {"zone", p.zone}});
                if (p.kind == "Doorway")
                    doorways.push_back({{"id", p.id}, {"at", Point(p.pose.position)}, {"width", p.spec.at("width")}, {"yaw", Degrees(p.pose.rotation[1])}, {"portal", p.spec.value("portal", false)}});
            }
            // Polygon brushes and sized volumes are drawn as their outlines
            // (Map Design pieces above; in brushMode polygons those are the pieces).
            for (const auto& a : low.actors)
                if (a.footprint) if (auto piece = Footprint(a); !piece.is_null()) pieces.push_back(std::move(piece));
            for (const auto& a : low.actors)
                actors.push_back({{"id", a.id}, {"cls", a.cls}, {"kind", a.role}, {"at", Point(a.at)}, {"position", Point(a.at)}, {"yaw", Degrees(a.yaw)}, {"label", a.label}});
            for (const auto& lift : low.lifts)
                actors.push_back({{"id", lift.id}, {"cls", "SBase.SLift"}, {"kind", "lift"}, {"at", Point(lift.at)}, {"position", Point(lift.at)}, {"yaw", 0}, {"label", "lift " + lift.id}});
            for (const auto& ge : low.ges)
                actors.push_back({{"id", ge.at("kind")}, {"cls", "GE"}, {"kind", "ge"}, {"at", ge.at("points")[0]}, {"position", ge.at("points")[0]}, {"yaw", 0}, {"label", ge.at("kind")}});
            Json labels = low.labels;
            for (const auto& p : low.pieces)
                if (p.kind == "Room" && !p.generated) labels.push_back({{"at", Point({p.pose.position[0], p.pose.position[1], p.box.lo[2]})}, {"text", p.spec.value("name", p.id)}, {"kind", "room"}});
            Json bounds = Bounds(low);
            for (const auto& p : pieces)
                for (size_t k = 0; k < 3; ++k)
                {
                    bounds["min"][k] = std::min(bounds["min"][k].get<double>(), p.at("box").at("min")[k].get<double>());
                    bounds["max"][k] = std::max(bounds["max"][k].get<double>(), p.at("box").at("max")[k].get<double>());
                }
            return {{"bounds", bounds}, {"pieces", pieces}, {"doorways", doorways}, {"actors", actors}, {"lines", low.lines}, {"labels", labels}};
        }
        inline Json Scene(const Lowered& low, const Json& spec, const std::map<std::string, std::string>& pointers, const Json& schema)
        {
            Json actors = Json::array(), pieces = Json::array(), ges = Json::array(), pointerJson = Json::object();
            for (const auto& a : low.actors)
            {
                const auto tag = a.properties.value("Tag", a.id);
                actors.push_back({{"path", a.id}, {"class", a.cls}, {"cls", a.cls}, {"name", a.id}, {"tag", tag}, {"event", a.properties.value("Event", std::string("None"))},
                                  {"position", Point(a.at)}, {"rotation", Json::array({0, a.yaw, 0})}, {"values", a.properties}, {"pointer", a.pointer}});
            }
            for (const auto& lift : low.lifts)
                actors.push_back({{"path", lift.id}, {"class", "SBase.SLift"}, {"cls", "SBase.SLift"}, {"name", lift.id}, {"tag", lift.id}, {"event", "None"}, {"position", Point(lift.at)},
                                  {"rotation", Json::array({0, 0, 0})}, {"values", {{"NumKeys", "2"}}}, {"pointer", lift.pointer}});
            // What the compiler adds on purpose (stairwells, lift shafts and
            // flights, zone portals) is marked generated: it carves into what it
            // joins, which the rules expect of it.
            for (const auto& p : low.pieces)
            {
                Json piece{{"id", p.id}, {"spec", p.spec}, {"position", p.pose.position}, {"rotation", p.pose.rotation}, {"pointer", p.pointer}, {"zone", p.zone}, {"storey", p.storey}};
                if (p.generated) piece["generated"] = true;
                pieces.push_back(piece);
            }
            for (const auto& ge : low.ges) ges.push_back({{"kind", ge.at("kind")}, {"command", ge.at("command")}, {"points", ge.at("points")}});
            for (const auto& [id, pointer] : pointers) pointerJson[id] = pointer;
            Json players = Json::array(), targets = Json::array(), operations = Json::array();
            for (const auto& p : low.players) players.push_back({{"objective", p.at("objective")}, {"min", p.at("min")}, {"max", p.at("max")}});
            for (const auto& t : low.targets) targets.push_back({{"mission", t.at("mission")}, {"sizes", t.at("sizes")}});
            // The whole level as one Map JSON document, for Authoring::Validate.
            for (const auto& a : low.actors) operations.push_back(CreateOp(a));
            for (const auto& link : low.links) operations.push_back(link);
            for (const auto& op : low.extraOps) operations.push_back(op);
            Json documents = Json::array();
            if (!operations.empty()) documents.push_back({{"format", "scct.map-changes"}, {"version", 2}, {"map", "*"}, {"description", "The actors of " + spec.at("map").value("name", std::string())}, {"operations", operations}});
            Json release = spec.value("release", Json::object());
            if (spec.contains("interface") && release.is_object() && !release.contains("interface")) release["interface"] = spec.at("interface");
            Json scene{{"actors", actors}, {"pieces", pieces}, {"ges", ges}, {"level", low.settings}, {"mapSettings", low.mapSettings}, {"modes", low.modes},
                       {"mapName", spec.at("map").value("name", std::string())}, {"mapFile", spec.at("map").value("file", std::string())}, {"profile", spec.value("profile", std::string("draft"))},
                       {"geometryBuilt", false}, {"live", false}, {"release", release}, {"zones", low.zones}, {"pointers", pointerJson},
                       {"playerCounts", players}, {"targets", targets}, {"documents", documents}};
            if (!schema.is_null()) scene["schema"] = schema;
            return scene;
        }
        // The wiki checks the compiler makes itself when the rules engine
        // (LevelRulesModel.h) is not part of the build.
        inline void WikiChecks(const Lowered& low, const Json& spec, std::vector<Diagnostic>& out)
        {
            const bool release = spec.value("profile", std::string("draft")) == "release";
            int spies = 0, mercs = 0, cameras = 0, firsts = 0;
            for (const auto& a : low.actors)
            {
                if (a.cls.find("PlayerStart") != std::string::npos) (a.properties.value("TeamNumber", std::string()) == "0" ? spies : mercs)++;
                if (a.cls.find("SCamNetwork") != std::string::npos) { ++cameras; if (a.properties.value("bFirstCam", std::string()) == "True") ++firsts; }
            }
            for (const auto& [count, team] : {std::pair<int, const char*>{spies, "spy"}, std::pair<int, const char*>{mercs, "merc"}})
                if (count < 3)
                    out.push_back({release ? "error" : "warning", "spawns.count", std::string("/spawns/") + team,
                                   std::string("The ") + team + " team has " + std::to_string(count) + " start(s); place at least 3 per team so 2v2 and 3v1 matches can spawn.", "spawn-points"});
            if (!cameras)
                out.push_back({release ? "error" : "warning", "camnet.missing", "/security/camnet", "There is no SCamNetwork camera: a Merc who opens the camera network gadget is softlocked. Add security.camnet.", "level-ingredients"});
            else if (firsts != 1)
                out.push_back({"error", "camnet.first", "/security/camnet", "Exactly one CamNet camera has bFirstCam.", "level-ingredients"});
        }
        // Map Design check findings that are the point of a generated piece: a
        // stairwell or lift shaft carves into the rooms it joins and needs no
        // doorway, and a zone portal has its corridor's cross-section.
        inline bool Expected(const Piece& piece, const std::string& text, const std::vector<Piece>& pieces)
        {
            auto passes = [](const Piece& p) { return p.generated || Design::SpansFloors(p.spec.value("name", std::string())); };
            if (piece.generated && piece.kind == "Doorway") return true;
            if (text.find("cannot be measured") != std::string::npos) return false;
            if (passes(piece) && text.find("has no doorway") != std::string::npos) return true;
            if (text.find("carve into each other") == std::string::npos) return false;
            for (const auto& p : pieces)
            {
                if (!passes(p)) continue;
                const auto name = p.spec.value("name", p.id);
                if (text.compare(0, name.size() + 5, name + " and ") == 0 || text.find(" and " + name + " carve") != std::string::npos) return true;
            }
            return false;
        }
        inline std::string DiagnosticPointer(const std::vector<std::string>& actors, const std::map<std::string, std::string>& pointers)
        {
            for (const auto& a : actors)
            {
                auto key = Fold(a.substr(a.find_last_of('.') + 1));
                if (const auto found = pointers.find(key); found != pointers.end()) return found->second;
            }
            return {};
        }
    }

    // A spec compiled to a plan, scenes and diagnostics. Throws SpecError when
    // the spec does not have the format's shape; anything else it finds is a
    // diagnostic, so an agent sees every problem at once.
    inline Compiled Compile(const Json& spec, const CompileOptions& options = {})
    {
        using namespace Detail;
        Compiled out;
        const auto& sidecar = options.sidecar;
        const bool haveSidecar = sidecar.is_object() && sidecar.value("format", std::string()) == "scct.level-sidecar" && sidecar.contains("spec") && sidecar.at("spec").is_object();
        if (sidecar.is_object() && !sidecar.empty() && !haveSidecar)
            out.diagnostics.push_back({"warning", "incremental.sidecar", "", "The sidecar given is not an scct.level-sidecar; the spec is built as new.", ""});
        if (options.sync != "merge" && options.sync != "incremental" && options.sync != "replace") throw SpecError("", "sync is merge, incremental or replace.");
        if (options.brushMode != "pieces" && options.brushMode != "polygons") throw SpecError("", "brushMode is pieces or polygons.");
        const Json input = haveSidecar && options.sync == "merge" ? Merge(sidecar.at("spec"), spec) : spec;
        const Json normalised = Parse(input);
        Compiler compiler(normalised, options, out.diagnostics);
        Lowered now = compiler.Run();
        const auto pointers = compiler.Pointers();
        Lowered before;
        bool incremental = false;
        if (haveSidecar)
        {
            try
            {
                std::vector<Diagnostic> ignored;
                const Json last = Parse(sidecar.at("spec")); // the compiler keeps a reference to it
                Compiler previous(last, options, ignored);
                before = previous.Run();
                incremental = true;
            }
            catch (const std::exception& e)
            {
                out.diagnostics.push_back({"warning", "incremental.sidecar", "", std::string("The sidecar's spec no longer compiles (") + e.what() + "), so nothing it built is replaced or deleted.", ""});
            }
        }
        const auto oldPieces = haveSidecar ? sidecar.value("pieces", Json::object()) : Json::object();
        const auto oldActors = haveSidecar ? sidecar.value("actors", Json::object()) : Json::object();
        auto recorded = [](const Json& map, const std::string& id) -> Json
        {
            for (auto it = map.begin(); it != map.end(); ++it) if (Fold(it.key()) == Fold(id)) return it.value();
            return Json();
        };
        const std::string mapName = normalised.at("map").value("name", std::string());
        Json pre = Json::array(), steps = Json::array(), post = Json::array();
        Json sidecarPieces = Json::object(), sidecarActors = Json::object();
        // Decide each id: kept as built, replaced or updated, deleted, or new.
        std::map<std::string, Json> replacedPieces;
        std::vector<const Piece*> newPieces, replacing;
        std::vector<Json> removedPieceRecords;
        std::map<std::string, std::string> keptPaths; // id -> live path of a kept actor
        std::vector<const Actor*> creates;
        std::vector<std::pair<const Actor*, Json>> updates; // actor, changed properties
        std::vector<Json> deleteIdentities;
        std::set<std::string> recreated;
        size_t keptCount = 0;
        const bool replaceAll = options.sync == "replace";
        if (!incremental)
        {
            for (const auto& p : now.pieces) if (options.brushMode == "pieces") newPieces.push_back(&p);
            for (const auto& a : now.actors) creates.push_back(&a);
        }
        else
        {
            std::map<std::string, const Piece*> oldPieceById;
            for (const auto& p : before.pieces) oldPieceById[Fold(p.id)] = &p;
            std::map<std::string, const Actor*> oldActorById;
            for (const auto& a : before.actors) oldActorById[Fold(a.id)] = &a;
            std::set<std::string> seen;
            if (options.brushMode == "pieces")
                for (const auto& p : now.pieces)
                {
                    seen.insert(Fold(p.id));
                    const auto old = oldPieceById.find(Fold(p.id));
                    const auto record = recorded(oldPieces, p.id);
                    if (old == oldPieceById.end() || record.is_null()) { newPieces.push_back(&p); continue; }
                    if (!replaceAll && Item(*old->second) == Item(p)) { sidecarPieces[p.id] = record; ++keptCount; continue; }
                    replacedPieces[Fold(p.id)] = record;
                    replacing.push_back(&p);
                }
            for (auto it = oldPieces.begin(); it != oldPieces.end(); ++it)
                if (!seen.count(Fold(it.key())))
                {
                    removedPieceRecords.push_back(it.value());
                    for (const auto& member : it.value().value("members", Json::array())) deleteIdentities.push_back(member);
                }
            seen.clear();
            for (const auto& a : now.actors)
            {
                seen.insert(Fold(a.id));
                const auto old = oldActorById.find(Fold(a.id));
                const auto identity = recorded(oldActors, a.id);
                if (old == oldActorById.end() || identity.is_null() || replaceAll)
                {
                    if (!identity.is_null()) { deleteIdentities.push_back(identity); recreated.insert(Fold(a.id)); }
                    creates.push_back(&a);
                    continue;
                }
                const auto oldOp = CreateOp(*old->second), newOp = CreateOp(a);
                if (oldOp == newOp) { keptPaths[Fold(a.id)] = identity.value("path", std::string()); sidecarActors[a.id] = identity; ++keptCount; continue; }
                bool subset = oldOp.at("class") == newOp.at("class") && oldOp.value("geometry", Json()) == newOp.value("geometry", Json()) && oldOp.value("polygons", Json()) == newOp.value("polygons", Json());
                Json changed = Json::object();
                for (auto it = oldOp.at("properties").begin(); subset && it != oldOp.at("properties").end(); ++it)
                    if (!newOp.at("properties").contains(it.key())) subset = false;
                for (auto it = newOp.at("properties").begin(); subset && it != newOp.at("properties").end(); ++it)
                    if (!oldOp.at("properties").contains(it.key()) || oldOp.at("properties").at(it.key()) != it.value())
                    {
                        // A start's team is written only when it is created, and a Tag
                        // is how other actors reach it: those recreate the actor.
                        if (it.key() == "TeamNumber" || it.key() == "Tag") subset = false;
                        changed[it.key()] = it.value();
                    }
                if (subset) { keptPaths[Fold(a.id)] = identity.value("path", std::string()); sidecarActors[a.id] = identity; updates.push_back({&a, changed}); continue; }
                deleteIdentities.push_back(identity);
                recreated.insert(Fold(a.id));
                creates.push_back(&a);
            }
            for (auto it = oldActors.begin(); it != oldActors.end(); ++it)
                if (!seen.count(Fold(it.key())))
                {
                    bool lift = false;
                    for (const auto& l : now.lifts) lift = lift || Fold(l.id) == Fold(it.key());
                    if (!lift) deleteIdentities.push_back(it.value());
                }
            // A kept actor whose references reach one that is made again points at
            // the old one: it is updated with the same references, resolved anew.
            for (const auto& a : now.actors)
            {
                if (!keptPaths.count(Fold(a.id))) continue;
                Json changed = Json::object();
                for (auto it = a.properties.begin(); it != a.properties.end(); ++it)
                {
                    std::set<std::string> reached;
                    Refs(it.value(), reached);
                    for (const auto& r : reached) if (recreated.count(Fold(r))) { changed[it.key()] = it.value(); break; }
                }
                if (changed.empty()) continue;
                auto found = std::find_if(updates.begin(), updates.end(), [&](const auto& u) { return u.first == &a; });
                if (found == updates.end()) updates.push_back({&a, changed});
                else for (auto it = changed.begin(); it != changed.end(); ++it) found->second[it.key()] = it.value();
            }
        }
        // Lifts made by design.lift: kept when their arguments match, else made
        // again in place of the old one.
        std::set<std::string> keptLifts;
        for (const auto& lift : now.lifts)
        {
            const auto record = recorded(oldActors, lift.id);
            bool same = false;
            for (const auto& l : before.lifts) same = same || (Fold(l.id) == Fold(lift.id) && l.args == lift.args);
            if (incremental && same && !record.is_null() && !replaceAll) keptLifts.insert(Fold(lift.id));
            else if (!record.is_null()) deleteIdentities.push_back(record);
        }
        // Reads for the before values of what is deleted or edited.
        std::map<std::string, std::string> queryOf; // folded path -> step id
        auto query = [&](const std::string& path)
        {
            const auto key = Fold(path);
            if (const auto found = queryOf.find(key); found != queryOf.end()) return found->second;
            const auto id = "q" + std::to_string(queryOf.size() + 1);
            queryOf[key] = id;
            steps.push_back(Step(id, "actors.query", {{"names", Json::array({path})}, {"fields", "all"}, {"limit", 1}}));
            return id;
        };
        const size_t reads = deleteIdentities.size() + updates.size() + now.updates.size() + now.links.size();
        if (reads > 160)
            out.diagnostics.push_back({"error", "incremental.size", "", "This build would delete or edit " + std::to_string(reads) + " actors it built before; a plan holds 256 steps. Start a new map (map_file new) and build the spec there.", ""});
        // pre: packages.
        Json packageOps = Json::array();
        for (const auto& package : now.packages) packageOps.push_back({{"op", "load"}, {"package", package}});
        for (const auto& op : now.packageOps) packageOps.push_back(op);
        if (!packageOps.empty())
            pre.push_back(Step("packages", "authoring.apply", {{"document", {{"format", "scct.map-changes"}, {"version", 2}, {"map", "*"}, {"description", "Packages for " + mapName}, {"operations", packageOps}}}}));
        // steps: deletes first, then replacements, new blockouts and the actors.
        if (!deleteIdentities.empty())
        {
            steps.push_back(Step("mapid", "authoring.mapid", Json::object()));
            Json operations = Json::array();
            for (const auto& identity : deleteIdentities)
            {
                const auto path = identity.value("path", std::string());
                if (path.empty()) continue;
                operations.push_back({{"op", "delete"}, {"actor", {{"path", path}, {"class", identity.value("class", std::string())}}}, {"before", Bind(query(path), "/actors/0/values")}});
            }
            steps.push_back(Step("deletes", "authoring.apply", {{"document", {{"format", "scct.map-changes"}, {"version", 3}, {"map", Bind("mapid", "/map")}, {"description", "Remove what " + mapName + " no longer has"}, {"allowDeletes", true}, {"operations", operations}}}}));
        }
        Json registerPieces = Json::array(), unregister = Json::array();
        if (!replacing.empty())
        {
            if (replacing.size() > 64) out.diagnostics.push_back({"error", "incremental.replace", "/geometry/pieces", "More than 64 pieces changed; build them in two steps or start a new map.", ""});
            Json items = Json::array();
            for (size_t i = 0; i < replacing.size() && i < 64; ++i)
            {
                const auto& record = replacedPieces.at(Fold(replacing[i]->id));
                items.push_back(Item(*replacing[i], record));
                for (const auto& member : record.value("members", Json::array())) unregister.push_back(member);
                sidecarPieces[replacing[i]->id] = Bind("blocks_replaced", "/" + std::to_string(i));
            }
            steps.push_back(Step("blocks_replaced", "design.blockbatch", {{"items", items}}));
            registerPieces.push_back(Bind("blocks_replaced"));
        }
        for (const auto& record : removedPieceRecords) for (const auto& member : record.value("members", Json::array())) unregister.push_back(member);
        for (size_t start = 0, chunk = 1; start < newPieces.size(); start += 64, ++chunk)
        {
            Json items = Json::array();
            const auto id = "blocks" + std::to_string(chunk);
            for (size_t i = start; i < newPieces.size() && i < start + 64; ++i)
            {
                items.push_back(Item(*newPieces[i]));
                sidecarPieces[newPieces[i]->id] = Bind(id, "/" + std::to_string(i - start));
            }
            steps.push_back(Step(id, "design.blockbatch", {{"items", items}}));
            registerPieces.push_back(Bind(id));
        }
        // The actors document: creates (with kept ids' references resolved to their
        // live paths), updates of kept actors, links and raw operations.
        Json operations = Json::array(), expect = Json::object();
        for (const auto* a : creates)
        {
            auto op = CreateOp(*a);
            op["properties"] = Rewrite(op.at("properties"), keptPaths);
            operations.push_back(op);
            sidecarActors[a->id] = Bind("actors", "/identities/" + a->id);
        }
        for (const auto& [actor, changed] : updates)
        {
            const auto path = keptPaths.at(Fold(actor->id));
            operations.push_back({{"op", "update"}, {"actor", recorded(oldActors, actor->id)}, {"before", Bind(query(path), "/actors/0/values")}, {"properties", Rewrite(changed, keptPaths)}});
        }
        // Live actors outside the spec: an edit or a link the last build already
        // made is not made again (a link appends an action each time it runs).
        for (const auto& [path, update] : now.updates)
        {
            if (incremental && before.updates.count(path) && before.updates.at(path) == update) continue;
            operations.push_back({{"op", "update"}, {"actor", update.at("actor")}, {"before", Bind(query(path), "/actors/0/values")}, {"properties", update.at("properties")}});
        }
        for (auto link : now.links)
        {
            if (incremental && keptPaths.count(Fold(link.at("event").get<std::string>())) && std::find(before.links.begin(), before.links.end(), link) != before.links.end()) continue;
            const auto target = link.at("target").get<std::string>();
            if (keptPaths.count(Fold(link.at("event").get<std::string>()))) link["event"] = keptPaths.at(Fold(link.at("event").get<std::string>()));
            if (!expect.contains(target)) expect[target] = Bind(query(target), "/actors/0/values");
            operations.push_back(link);
        }
        for (const auto& op : now.extraOps) operations.push_back(op);
        if (!operations.empty())
        {
            Json document{{"format", "scct.map-changes"}, {"version", 2}, {"map", "*"}, {"description", "The actors of " + mapName}, {"operations", operations}};
            if (!expect.empty()) document["expect"] = expect;
            steps.push_back(Step("actors", "authoring.apply", {{"document", document}}));
        }
        if (operations.size() > 20000)
            out.diagnostics.push_back({"error", "plan.size", "", "The actors document has " + std::to_string(operations.size()) + " operations; Map JSON takes 20000 at most.", ""});
        // The identity a later step binds to.
        auto identity = [&](const std::string& id) -> Json
        {
            if (keptPaths.count(Fold(id))) return recorded(oldActors, id);
            for (const auto& lift : now.lifts) if (Fold(lift.id) == Fold(id)) return keptLifts.count(Fold(id)) ? recorded(oldActors, id) : Bind("lift_" + lift.id);
            return Bind("actors", "/identities/" + id);
        };
        for (const auto& lift : now.lifts)
        {
            if (keptLifts.count(Fold(lift.id))) { sidecarActors[lift.id] = recorded(oldActors, lift.id); ++keptCount; continue; }
            steps.push_back(Step("lift_" + lift.id, "design.lift", lift.args));
            sidecarActors[lift.id] = Bind("lift_" + lift.id);
        }
        auto changedList = [&](const std::vector<Json>& a, const std::vector<Json>& b) { return !incremental || a != b; };
        if (changedList(now.players, before.players))
            for (size_t i = 0; i < now.players.size(); ++i)
            {
                const auto& p = now.players[i];
                steps.push_back(Step("players" + std::to_string(i + 1), "objectiveplayers.set", {{"objective", identity(p.at("objective").get<std::string>())}, {"min", p.at("min")}, {"max", p.at("max")}, {"open", p.at("open")}}));
            }
        if (changedList(now.targets, before.targets))
            for (size_t i = 0; i < now.targets.size(); ++i)
                steps.push_back(Step("targets" + std::to_string(i + 1), "objectiveplayers.targets", {{"mission", identity(now.targets[i].at("mission").get<std::string>())}, {"sizes", now.targets[i].at("sizes")}}));
        Json gesBuilt = haveSidecar ? sidecar.value("ges", Json::array()) : Json::array();
        size_t geCount = 0;
        for (const auto& ge : now.ges)
        {
            const Json record{{"command", ge.at("command")}, {"points", ge.at("points")}};
            if (std::find(gesBuilt.begin(), gesBuilt.end(), record) != gesBuilt.end() && !replaceAll) continue;
            steps.push_back(Step("ge" + std::to_string(++geCount), "ge.add", record));
            gesBuilt.push_back(record);
        }
        if (incremental)
            for (const auto& old : before.ges)
            {
                const Json record{{"command", old.at("command")}, {"points", old.at("points")}};
                if (std::none_of(now.ges.begin(), now.ges.end(), [&](const Json& g) { return Json{{"command", g.at("command")}, {"points", g.at("points")}} == record; }))
                    out.diagnostics.push_back({"warning", "incremental.ge", "/ges", "A " + old.at("kind").get<std::string>() + " GE the spec no longer has stays in the map: the bridge cannot delete GEs. Delete it in GE mode.", "geometric-events"});
            }
        if (!now.settings.empty() && (!incremental || now.settings != before.settings))
        {
            steps.push_back(Step("settings", "settings.inspect", Json::object()));
            steps.push_back(Step("settings_edit", "settings.edit", {{"snapshot", Bind("settings")}, {"changes", now.settings}}));
        }
        if (!unregister.empty()) steps.push_back(Step("unregister", "design.unregister", {{"members", unregister}}));
        if (!registerPieces.empty()) steps.push_back(Step("register", "design.register", {{"pieces", registerPieces}}));
        out.sidecar = {{"format", "scct.level-sidecar"}, {"version", 1}, {"map", mapName}, {"hash", Hash(normalised)}, {"spec", normalised},
                       {"brushMode", options.brushMode}, {"pieces", sidecarPieces}, {"actors", sidecarActors}, {"ges", gesBuilt}};
        steps.push_back(Step("sidecar", "mcp.sidecar.set", {{"sidecar", out.sidecar}}));
        // post: build, check, save, settings files, screenshot.
        if (options.build)
        {
            post.push_back(Step("build", "design.build", Json::object()));
            // A piece's faces are textured on its brushes once they are built: the
            // members of the record its blockout step returned, or kept.
            for (size_t i = 0; i < now.surfaces.size(); ++i)
            {
                const auto& surface = now.surfaces[i];
                const auto record = sidecarPieces.value(surface.at("piece").get<std::string>(), Json());
                Json brushes = record.is_object() ? record.value("members", Json::array()) : Json::array();
                if (Plan::IsBind(record))
                {
                    brushes = record;
                    brushes["$bind"]["ptr"] = record.at("$bind").value("ptr", std::string()) + "/members";
                }
                post.push_back(Step("surface" + std::to_string(i + 1), "surface.texture", {{"texture", surface.at("texture")}, {"brushes", brushes}, {"faces", surface.at("faces")}}, true));
            }
            post.push_back(Step("check", "mapcheck.run", Json::object()));
        }
        else if (!now.surfaces.empty())
            out.diagnostics.push_back({"info", "geometry.surface", "/geometry/surfaces", "Surfaces of Map Design pieces are textured after a geometry build; this build has none.", ""});
        if (!options.save.empty()) post.push_back(Step("save", "map.save", {{"path", options.save}}));
        if (!now.mapSettings.is_null())
        {
            post.push_back(Step("mapsettings", "mapsettings.set", {{"changes", now.mapSettings}}, true));
            out.diagnostics.push_back({"info", "settings.restart", "/settings", "SMapSettings exist only once the map is saved, closed and opened again; if mapsettings.set reports none, use editor_restart and build again.", "map-settings"});
        }
        for (size_t i = 0; i < now.interfaceTextures.size(); ++i)
            post.push_back(Step("interface" + std::to_string(i + 1), "interface.texture", now.interfaceTextures[i], true));
        if (options.screenshot) post.push_back(Step("screenshot", "view.screenshot", {{"viewport", "top"}, {"maxEdge", 1024}}));
        Json plan{{"label", "level_build " + mapName}};
        if (!pre.empty()) plan["pre"] = pre;
        plan["steps"] = steps;
        if (!post.empty()) plan["post"] = post;
        if (steps.size() + pre.size() + post.size() > Bridge::kMaxPlanSteps)
            out.diagnostics.push_back({"error", "plan.size", "", "The plan has " + std::to_string(steps.size() + pre.size() + post.size()) + " steps; a plan holds 256. Split the GEs or the build.", ""});
        try { Plan::Check(plan); }
        catch (const std::exception& e) { out.diagnostics.push_back({"error", "plan.order", "", std::string("The compiled plan is not valid: ") + e.what(), ""}); }
        out.plan = plan;
        out.scene = Scene(now, normalised, pointers, options.schema);
        out.render = Render(now);
        // Layout problems a plan view hides, from the Map Design check (the
        // rules engine runs the same check on the scene when it is built in).
#ifndef RE_SPEC_RULES
        {
            Json pieces = Json::array();
            for (const auto& p : now.pieces) pieces.push_back({{"spec", p.spec}, {"position", p.pose.position}, {"rotation", p.pose.rotation}});
            for (const auto& issue : Design::DesignIssues(pieces, Json::object()))
            {
                const Piece* piece = issue.piece >= 0 && static_cast<size_t>(issue.piece) < now.pieces.size() ? &now.pieces[static_cast<size_t>(issue.piece)] : nullptr;
                if (piece && Expected(*piece, issue.text, now.pieces)) continue;
                out.diagnostics.push_back({issue.severity, "geometry.design", piece ? piece->pointer : std::string(), issue.text, ""});
            }
        }
#endif
#ifdef RE_SPEC_RULES
        try
        {
            for (const auto& finding : Rules::Check(Rules::SceneFromJson(out.scene), normalised.value("profile", std::string("draft"))))
                out.diagnostics.push_back({finding.severity, finding.rule, DiagnosticPointer(finding.actors, pointers), finding.message, finding.wikiPage, finding.fix});
        }
        catch (const std::exception& e) { out.diagnostics.push_back({"warning", "rules.failed", "", std::string("The rules could not check the scene: ") + e.what(), ""}); }
#else
        WikiChecks(now, normalised, out.diagnostics);
#endif
        // Counts by kind.
        std::map<std::string, size_t> byClass, byKind;
        for (const auto& a : now.actors) { ++byClass[a.cls]; ++byKind[a.role]; }
        size_t errors = 0, warnings = 0;
        for (const auto& d : out.diagnostics)
        {
            if (d.severity == "error") ++errors;
            if (d.severity == "warning") ++warnings;
        }
        size_t doorways = 0;
        for (const auto& p : now.pieces) if (p.kind == "Doorway") ++doorways;
        Json classes = Json::array();
        for (const auto& [cls, n] : byClass) { (void)n; classes.push_back(cls); }
        out.summary = {{"map", mapName}, {"pieces", now.pieces.size()}, {"doorways", doorways}, {"actors", now.actors.size()}, {"lifts", now.lifts.size()}, {"ges", now.ges.size()},
                       {"byClass", byClass}, {"byKind", byKind}, {"classes", classes}, {"packages", now.packages}, {"modes", now.modes},
                       {"steps", {{"pre", pre.size()}, {"steps", steps.size()}, {"post", post.size()}}},
                       {"incremental", incremental}, {"creates", creates.size()}, {"updates", updates.size()}, {"deletes", deleteIdentities.size()},
                       {"replacedPieces", replacing.size()}, {"newPieces", newPieces.size()}, {"kept", keptCount}, {"errors", errors}, {"warnings", warnings}};
        return out;
    }
}
