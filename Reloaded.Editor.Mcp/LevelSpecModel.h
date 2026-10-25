#pragma once
// scct.level-spec, version 1: a whole Versus level as one declarative JSON
// document, which LevelSpecCompiler.h turns into one mcp.plan. This header
// holds the format: its JSON Schema (Schema(), also published as
// Resources/schemas/level-spec.schema.json), the shape check (Parse), the
// fragment merge the intent tools use (Merge) and the hash the sidecar keeps.
// Engine-free and Windows-free.
//
// Units: lengths are in units{unit} (uu by default; m and ft use uuPerMetre),
// yaw is in degrees (east 0, north 90), Z is up. Every "id" names one thing in
// the level: [A-Za-z_][A-Za-z0-9_]{0,62}, unique ignoring case, and it becomes
// the actor's create id and its Tag.
#include "../Reloaded.Editor/WorkflowModel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace Mcp::Spec
{
    using Json = Workflow::Json;
    // One finding about a spec: severity error|warning|info, a rule id, the JSON
    // pointer of the spec part it is about, a sentence and the wiki page slug
    // (scct://wiki/<slug>) the rule comes from, "" when it is not a wiki rule.
    struct Diagnostic { std::string severity, rule, pointer, message, wikiPage; Json fix = Json(); };
    // A spec that does not have the format's shape. pointer is where; what()
    // starts with it too.
    struct SpecError : std::runtime_error
    {
        std::string pointer;
        SpecError(const std::string& where, const std::string& message) : std::runtime_error((where.empty() ? std::string("/") : where) + ": " + message), pointer(where) {}
    };

    constexpr const char* kFormat = "scct.level-spec";
    constexpr int kVersion = 1;
    inline const std::vector<std::string>& PieceKinds()
    {
        static const std::vector<std::string> kinds{"Room", "Corridor", "Vent", "Crawlway", "Doorway", "Platform", "Stairs", "Stairs L", "Stairs U", "Spiral", "Ramp"};
        return kinds;
    }
    inline bool ValidId(const std::string& id)
    {
        static const std::regex pattern("[A-Za-z_][A-Za-z0-9_]{0,62}");
        return std::regex_match(id, pattern);
    }
    // A JSON pointer one level down.
    inline std::string Child(const std::string& pointer, const std::string& key)
    {
        std::string token;
        for (char c : key) token += c == '~' ? std::string("~0") : c == '/' ? std::string("~1") : std::string(1, c);
        return pointer + "/" + token;
    }
    inline std::string Child(const std::string& pointer, size_t index) { return pointer + "/" + std::to_string(index); }

    // --- The JSON Schema ------------------------------------------------------
    namespace Shape
    {
        inline Json Type(const char* type, const char* description = nullptr)
        {
            Json s{{"type", type}};
            if (description) s["description"] = description;
            return s;
        }
        inline Json Str(const char* description = nullptr) { return Type("string", description); }
        inline Json Num(const char* description = nullptr) { return Type("number", description); }
        inline Json Int(const char* description = nullptr) { return Type("integer", description); }
        inline Json Bool(const char* description = nullptr) { return Type("boolean", description); }
        inline Json Range(Json s, double low, double high) { s["minimum"] = low; s["maximum"] = high; return s; }
        inline Json Ref(const char* name) { return {{"$ref", std::string("#/$defs/") + name}}; }
        inline Json Enum(std::initializer_list<const char*> values, const char* description = nullptr)
        {
            Json s{{"type", "string"}, {"enum", Json::array()}};
            for (auto v : values) s["enum"].push_back(v);
            if (description) s["description"] = description;
            return s;
        }
        inline Json Arr(Json items, int low = -1, int high = -1, const char* description = nullptr)
        {
            Json s{{"type", "array"}, {"items", std::move(items)}};
            if (low >= 0) s["minItems"] = low;
            if (high >= 0) s["maxItems"] = high;
            if (description) s["description"] = description;
            return s;
        }
        // A closed object: a key it does not list is refused.
        inline Json Obj(Json properties, std::initializer_list<const char*> required = {}, const char* description = nullptr)
        {
            Json s{{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
            if (required.size()) { s["required"] = Json::array(); for (auto r : required) s["required"].push_back(r); }
            if (description) s["description"] = description;
            return s;
        }
        inline Json Open(const char* description) { return {{"type", "object"}, {"description", description}}; }
        inline Json Any(std::vector<Json> forms, const char* description = nullptr)
        {
            Json s{{"anyOf", Json(std::move(forms))}};
            if (description) s["description"] = description;
            return s;
        }
    }
    // The format as a JSON Schema (draft 2020-12). Parse checks a spec against
    // exactly this, so the published schema and the parser cannot drift apart.
    inline const Json& Schema()
    {
        using namespace Shape;
        static const Json schema = []
        {
            Json defs;
            defs["id"] = {{"type", "string"}, {"pattern", "^[A-Za-z_][A-Za-z0-9_]{0,62}$"},
                          {"description", "Names one thing in the level; unique ignoring case; becomes its create id and Tag."}};
            defs["ids"] = Arr(Ref("id"));
            defs["height"] = {{"type", "string"}, {"pattern", "^\\s*(floor|ceiling)\\s*([+-]\\s*[0-9]+(\\.[0-9]+)?)?\\s*$"},
                              {"description", "A height in a room: \"floor+N\" or \"ceiling-N\"."}};
            defs["coordinate"] = Any({Num(), Ref("height")});
            defs["vec2"] = Arr(Num(), 2, 2);
            defs["vec3"] = Arr(Num(), 3, 3);
            defs["point"] = Arr(Ref("coordinate"), 2, 3, "[x, y] or [x, y, z]; z may be \"floor+N\" or \"ceiling-N\" of the room under the point, and a missing z takes the usual height for the thing placed.");
            defs["inPosition"] = Obj({{"in", Ref("id")}, {"u", Range(Num("Across the piece's width, -0.5 to 0.5."), -0.5, 0.5)},
                                      {"v", Range(Num("Along the piece's length, -0.5 to 0.5."), -0.5, 0.5)}, {"h", Any({Num(), Ref("height")})}},
                                     {"in"}, "{in: piece, u, v, h}: a point inside a geometry piece.");
            defs["nearPosition"] = Obj({{"near", Ref("id")}, {"offset", Arr(Num(), 2, 3)}}, {"near"}, "{near: id, offset}: beside another thing in the spec.");
            defs["doorwayPosition"] = Obj({{"doorway", Ref("id")}, {"h", Any({Num(), Ref("height")})}}, {"doorway"}, "{doorway: id}: in a doorway, on its sill.");
            defs["position"] = Any({Ref("point"), Ref("inPosition"), Ref("nearPosition"), Ref("doorwayPosition")},
                                   "[x, y, z] | {in, u, v, h} | {near, offset} | {doorway}");
            defs["target"] = Any({Ref("id"), Ref("position")});
            defs["facing"] = Any({Num("Yaw in degrees (east 0, north 90)."), Obj({{"toward", Ref("target")}}, {"toward"}), Obj({{"yaw", Num()}}, {"yaw"})},
                                 "A yaw in degrees, {yaw}, or {toward: id | position}.");
            defs["props"] = Open("Raw map-changes property values, written as given: leaves are strings, structs objects, arrays arrays, {\"$ref\": id} for actors.");
            defs["generic"] = Obj({{"id", Ref("id")}, {"class", Str("Actor class, Package.Class.")}, {"mesh", Str("StaticMesh'Package.Group.Name' or Package.Group.Name.")},
                                   {"at", Ref("position")}, {"yaw", Num()}, {"scale", Any({Num(), Ref("vec3")})}, {"properties", Ref("props")}},
                                  {"id"}, "Any actor: its class, where it is and raw properties.");
            defs["mission"] = Obj({{"id", Ref("id")}, {"name", Str()}, {"description", Str("The Spy text.")}, {"descriptionDef", Str("The Merc text.")},
                                   {"minimumObjectives", Range(Int(), 0, 64)}, {"chained", Bool()}, {"atResetEvent", Str("Event sent once when the mode starts.")},
                                   {"at", Ref("position")}, {"props", Ref("props")}});

            const auto piece = Obj({{"id", Ref("id")}, {"kind", Enum({"Room", "Corridor", "Vent", "Crawlway", "Doorway", "Platform", "Stairs", "Stairs L", "Stairs U", "Spiral", "Ramp"})},
                                    {"size", Arr(Num(), 3, 3, "[width (local X), length (local Y), height]; a corridor, vent or crawlway is open at both ends of its length.")},
                                    {"at", Ref("point")}, {"storey", Ref("id")}, {"yaw", Num("Degrees; multiples of 90 keep walls on the compass.")}, {"name", Str()},
                                    {"material", Str("Package.Group.Name")}, {"construction", Enum({"Carve", "Shell"})}, {"ceiling", Bool()},
                                    {"steps", Range(Int(), 1, 128)}, {"thickness", Num()}, {"portal", Bool()},
                                    {"attach", Obj({{"to", Ref("id")}, {"side", Enum({"north", "south", "east", "west"})}, {"offset", Num("Along the wall: east of the centre on a north or south wall, north of it on an east or west wall.")}},
                                                   {"to", "side"}, "Flush against the outside of another piece's wall, running away from it.")}},
                                   {"id", "kind", "size"});
            const auto doorway = Obj({{"id", Ref("id")}, {"between", Arr(Ref("id"), 2, 2)}, {"host", Ref("id")}, {"side", Enum({"north", "south", "east", "west"})},
                                      {"offset", Num()}, {"size", Arr(Num(), 2, 2, "[width, height]")}, {"sill", Num()}, {"depth", Num()},
                                      {"portal", Bool("A zone portal in the opening. Default: true, or with zones only where it joins two zones.")}, {"name", Str()}},
                                     {"id"});
            const auto vertical = Obj({{"id", Ref("id")}, {"kind", Enum({"stairs", "lift"})}, {"from", Ref("id")}, {"to", Ref("id")}, {"at", Ref("inPosition")},
                                       {"shape", Enum({"Stairs", "Stairs L", "Stairs U", "Spiral"})}, {"width", Num()}},
                                      {"kind", "from", "to", "at"});
            const auto polygon = Obj({{"texture", Str()}, {"flags", Int()}, {"vertices", Arr(Ref("vec3"), 3, 16)}, {"origin", Ref("vec3")},
                                      {"textureU", Ref("vec3")}, {"textureV", Ref("vec3")}, {"pan", Arr(Int(), 2, 2)}},
                                     {"vertices"});
            const auto brush = Obj({{"id", Ref("id")}, {"class", Str()}, {"csg", Enum({"CSG_Add", "CSG_Subtract"})}, {"polyFlags", Int()}, {"at", Ref("point")},
                                    {"polygons", Arr(polygon, 1, 16384)}, {"props", Ref("props")}},
                                   {"id", "polygons"});
            const auto surface = Obj({{"brush", Ref("id")}, {"faces", Arr(Enum({"floor", "ceiling", "+X", "-X", "+Y", "-Y"}), 1, 6, "Faces by the way their surface faces: a carved room's east wall faces west (-X).")}, {"texture", Str()}},
                                     {"brush", "faces", "texture"});
            const auto mesh = Obj({{"id", Ref("id")}, {"mesh", Str()}, {"at", Ref("position")}, {"yaw", Num()}, {"scale", Any({Num(), Ref("vec3")})},
                                   {"fit", Enum({"floor", "none"})}, {"props", Ref("props")}},
                                  {"id", "mesh", "at"});
            const auto geometry = Obj({{"pieces", Arr(piece)}, {"doorways", Arr(doorway)}, {"vertical", Arr(vertical)}, {"brushes", Arr(brush)},
                                       {"surfaces", Arr(surface)}, {"meshes", Arr(mesh)}});

            const auto start = Obj({{"id", Ref("id")}, {"at", Ref("position")}, {"yaw", Num()}, {"facing", Ref("facing")}}, {"id", "at"});
            const auto team = Any({Obj({{"in", Any({Ref("id"), Arr(Ref("id"), 1)})}, {"count", Range(Int(), 1, 32)}, {"facing", Ref("facing")}, {"spacing", Num()}}, {"in"},
                                       "Starts on a 128-unit lattice at floor+48 in one or more rooms."),
                                   Arr(start, 1, 32, "Starts placed one by one.")});
            const auto toggle = Obj({{"id", Ref("id")}, {"team", Enum({"spy", "merc"})}, {"in", Ref("id")}, {"at", Ref("position")}, {"yaw", Num()},
                                     {"facing", Ref("facing")}, {"enabledBy", Str("The event Tag that toggles it.")}},
                                    {"id", "team"});
            const auto spawns = Obj({{"spy", team}, {"merc", team}, {"toggles", Arr(toggle)}});

            const auto dropZone = Any({Obj({{"id", Ref("id")}, {"at", Ref("position")}}, {"at"}, "{id, at}: a new SFlagDropZone; give it an id to share it."),
                                       Obj({{"ref", Ref("id")}}, {"ref"}, "{ref: id}: the drop zone another flag's entry made, shared.")},
                                      "{id, at} for a new SFlagDropZone, or {ref: id} to share one another flag made (flags may share drop zones).");
            const auto trigger = Obj({{"id", Ref("id")}, {"kind", Enum({"terminal", "flag", "bomb"})}, {"at", Ref("position")}, {"yaw", Num()}, {"facing", Ref("facing")},
                                      {"dominationCaptureTime", Num("Terminal capture time.")}, {"captureTime", Num("Bomb fuse.")}, {"resetTime", Num("Flag reset.")},
                                      {"dropZones", Arr(dropZone, 1, 5)}, {"triggerName", Str()}, {"props", Ref("props")}},
                                     {"id", "kind"});
            const auto objective = Obj({{"id", Ref("id")}, {"name", Str()}, {"description", Str()}, {"descriptionDef", Str()}, {"visibilityZone", Ref("id")},
                                        {"at", Ref("position")}, {"triggers", Arr(trigger)}, {"props", Ref("props")}},
                                       {"id"});
            const auto action = Obj({{"kind", Enum({"open", "light", "sound", "announce", "trigger"})}, {"target", Ref("id")}, {"tag", Str()}, {"sound", Str()},
                                     {"title", Str()}, {"merc", Str()}, {"spy", Str()}, {"seconds", Num()}, {"delay", Num()}, {"hold", Bool()}, {"untrigger", Bool()}},
                                    {"kind"});
            const auto sector = Obj({{"id", Ref("id")}, {"name", Str()}, {"brief", Str()}, {"briefDefend", Str()}, {"objectives", Arr(Ref("id"), 1)},
                                     {"required", Range(Int(), 0, 64)}, {"actions", Arr(action)}},
                                    {"id", "objectives"});
            const auto story = Obj({{"mission", Ref("mission")}, {"objectives", Arr(objective)}, {"sectors", Arr(sector)}});
            const auto disk = Obj({{"id", Ref("id")}, {"at", Ref("position")}, {"group", Range(Int(), 1, 3)}}, {"id", "at"});
            const auto diskhunt = Obj({{"disks", Any({Obj({{"in", Any({Ref("id"), Arr(Ref("id"), 1)})}, {"count", Range(Int(), 1, 128)}}, {"in"}), Arr(disk, 1, 128)})},
                                       {"resetTime", Num()}, {"mission", Ref("mission")}, {"disableSecurity", Bool()}});
            const auto deathmatch = Obj({{"disableSecurity", Bool()}, {"mission", Ref("mission")}});
            const auto players = Obj({{"objective", Ref("id")}, {"min", Range(Int(), 0, 16)}, {"max", Range(Int(), 0, 16)}, {"open", Bool()}}, {"objective"});
            const auto targetSizes = Obj({{"mission", Ref("id")}, {"sizes", Arr(Range(Int(), 0, 32), 8, 8, "Objectives to win at 1v1 .. 8v8; 0 keeps the mission's own.")}},
                                         {"mission", "sizes"});
            const auto gameplay = Obj({{"modes", {{"type", "array"}, {"items", Enum({"story", "diskhunt", "deathmatch"})}, {"uniqueItems", true}}},
                                       {"story", story}, {"diskhunt", diskhunt}, {"deathmatch", deathmatch}, {"playerCounts", Arr(players)},
                                       {"targets", Any({targetSizes, Arr(targetSizes)})}});

            const auto alarm = Obj({{"id", Ref("id")}, {"name", Str()}, {"description", Str("The Merc HUD text.")}, {"descriptionSpy", Str()}, {"duration", Num()},
                                    {"priority", Range(Int(), 0, 5)}, {"outputs", Arr(Str("An id or a Tag."))},
                                    {"lockMovers", Arr(Obj({{"mover", Ref("id")}, {"action", Str("ActionBeforeLock value.")}}, {"mover"}))},
                                    {"lockObjectives", Arr(Ref("id"))}, {"at", Ref("position")}, {"props", Ref("props")}},
                                   {"id"});
            const auto box = Any({Obj({{"min", Ref("point")}, {"max", Ref("point")}}, {"min", "max"}), Obj({{"in", Ref("id")}, {"height", Num()}}, {"in"})});
            const auto device = Obj({{"id", Ref("id")}, {"kind", Enum({"laser", "camera", "motion", "presence", "mine"})}, {"at", Ref("position")}, {"in", Ref("id")},
                                     {"box", box}, {"facing", Ref("facing")}, {"alarm", Ref("id")}, {"detected", Enum({"spy", "merc", "both"})},
                                     {"length", Num()}, {"radius", Num()}, {"height", Num()}, {"coneAngle", Num()}, {"maxDistance", Num()}, {"patrolAngle", Num()},
                                     {"rotationVelocity", Num()}, {"detectionDelay", Num()}, {"hideLaser", Bool()}, {"spawnPercent", Range(Num(), 0, 100)},
                                     {"props", Ref("props")}},
                                    {"id", "kind"});
            const auto camnet = Obj({{"id", Ref("id")}, {"camName", Str()}, {"at", Ref("position")}, {"yaw", Num()}, {"facing", Ref("facing")}, {"first", Bool()},
                                     {"constraints", Obj({{"pitch", Ref("vec2")}, {"yaw", Ref("vec2")}, {"roll", Ref("vec2")}})}, {"props", Ref("props")}},
                                    {"id"});
            const auto security = Obj({{"alarms", Arr(alarm)}, {"devices", Arr(device)}, {"camnet", Arr(camnet)}});

            const auto usable = Enum({"spy", "merc", "both"});
            const auto doorTrigger = Obj({{"kind", Enum({"button", "panel", "presence", "event"})}, {"hackTime", Num()}, {"usableBy", usable}, {"at", Ref("position")},
                                          {"radius", Num()}, {"text", Str()}});
            const auto door = Obj({{"id", Ref("id")}, {"mesh", Str()}, {"at", Ref("position")}, {"yaw", Num()},
                                   {"open", Any({Obj({{"offset", Ref("vec3")}}, {"offset"}), Obj({{"yaw", Num()}}, {"yaw"})})},
                                   {"moveTime", Num()}, {"stayOpen", Num()}, {"trigger", doorTrigger}, {"props", Ref("props")}},
                                  {"id", "mesh"});
            const auto key = Obj({{"offset", Ref("vec3")}, {"yaw", Num()}, {"pitch", Num()}, {"roll", Num()}});
            const auto mover = Obj({{"id", Ref("id")}, {"class", Str()}, {"mesh", Str()}, {"at", Ref("position")}, {"yaw", Num()}, {"keys", Arr(key, 1, 24)},
                                    {"moveTime", Num()}, {"stayOpen", Num()}, {"initialState", Str()}, {"encroach", Str()}, {"props", Ref("props")}},
                                   {"id", "mesh"});
            const auto liftDoor = Obj({{"storey", Ref("id")}, {"doorway", Ref("id")}, {"at", Ref("position")}, {"yaw", Num()}, {"mesh", Str()}},
                                      {"storey"}, "Where one storey's external door goes, when no doorway of the spec joins that storey to the shaft (or another than the one found).");
            const auto lift = Obj({{"id", Ref("id")}, {"shaft", Ref("id")}, {"storeys", Arr(Ref("id"), 2, 24)}, {"start", Ref("id")}, {"mesh", Str("The car: an SLift static mesh. Without it a two-storey lift is a design.lift platform.")},
                                   {"moveTime", Num()}, {"switches", Bool("A call switch on each storey and one in the car. Default true.")},
                                   {"carSwitch", Bool("The switch inside the car, which rides with it (AttachTag). Default true.")},
                                   {"doorMesh", Str("Static mesh of each storey's external door (a Mover: Key 0 open, Key 1 closed). Without it the lift has no doors.")},
                                   {"carDoorMesh", Str("Static mesh of the car's SLiftDoor; default doorMesh.")},
                                   {"doorOpen", Any({Obj({{"offset", Ref("vec3")}}, {"offset"}), Obj({{"yaw", Num()}}, {"yaw"})}, "How a lift door opens from closed: {offset} or {yaw}. Default: up by its height.")},
                                   {"doorMoveTime", Num()}, {"doors", Arr(liftDoor)}, {"props", Ref("props")}},
                                  {"id", "shaft", "storeys"});
            const auto button = Obj({{"id", Ref("id")}, {"kind", Enum({"button", "panel"})}, {"at", Ref("position")}, {"yaw", Num()}, {"facing", Ref("facing")},
                                     {"targets", Arr(Str("An id or a Tag."), 1)}, {"hackTime", Num()}, {"usableBy", usable}, {"text", Str()}, {"mesh", Str()},
                                     {"props", Ref("props")}},
                                    {"id"});
            const auto mechanisms = Obj({{"doors", Arr(door)}, {"movers", Arr(mover)}, {"lifts", Arr(lift)}, {"buttons", Arr(button)}});

            const auto link = Obj({{"from", Ref("id")}, {"to", Ref("id")}, {"delay", Num()}, {"untrigger", Bool()}}, {"from", "to"});
            const auto magic = Obj({{"id", Ref("id")}, {"document", Open("An scct.smagic-event document (SMagicEvent Workbench export).")}, {"at", Ref("position")}}, {"id", "document"});
            const auto message = Obj({{"id", Ref("id")}, {"spy", Str()}, {"merc", Str()}, {"time", Num()}, {"priority", Int()}, {"at", Ref("position")}}, {"id"});
            const auto levelEvent = Obj({{"event", Str()}, {"sendAtTime", Num("Seconds; negative counts back from the end of the match.")}}, {"event", "sendAtTime"});
            const auto triggerLight = Obj({{"light", Ref("id")}, {"object", Ref("id")}, {"initiallyOn", Bool()}}, {"light"});
            const auto events = Obj({{"links", Arr(link)}, {"magic", Arr(magic)}, {"messages", Arr(message)}, {"shakers", Arr(Ref("generic"))},
                                     {"noises", Arr(Ref("generic"))}, {"level", Arr(levelEvent)}, {"triggerLights", Arr(triggerLight)}});

            const auto light = Obj({{"id", Ref("id")}, {"preset", Enum({"ceiling", "spot", "sun", "echelon-spot"})}, {"in", Ref("id")}, {"at", Ref("position")},
                                    {"radius", Num()}, {"brightness", Range(Num(), 0, 255)}, {"hue", Range(Num(), 0, 255)}, {"saturation", Range(Num(), 0, 255)},
                                    {"corona", Bool()}, {"switchable", Bool("An STriggerLight events can switch.")}, {"yaw", Num()}, {"pitch", Num()},
                                    {"cone", Range(Int("LightCone of the spot preset: the beam's width, larger is wider. Default: the class's."), 0, 255)}, {"props", Ref("props")}},
                                   {"id"});
            const auto sound = Obj({{"id", Ref("id")}, {"class", Str()}, {"sound", Str("Sound'Package.Group.Name'")}, {"at", Ref("position")}, {"radius", Num()},
                                    {"volume", Range(Num(), 0, 255)}, {"props", Ref("props")}},
                                   {"id", "sound"});
            const auto volume = Obj({{"id", Ref("id")}, {"class", Str("Volume, SDamageVolume, ERainVolume, BlockingVolume or a full class name.")}, {"box", box},
                                     {"locationName", Str()}, {"priority", Num("LocationPriority.")}, {"props", Ref("props")}},
                                    {"id", "box"});
            const auto ingredient = Obj({{"id", Ref("id")}, {"kind", Enum({"ammo", "infirmary", "glass", "extinguisher", "projector", "sprinkler", "puddle", "balltrap"})},
                                         {"in", Ref("id")}, {"at", Ref("position")}, {"yaw", Num()}, {"facing", Ref("facing")}, {"props", Ref("props")}},
                                        {"kind"});
            const auto object = Obj({{"id", Ref("id")}, {"mesh", Str()}, {"at", Ref("position")}, {"yaw", Num()}, {"damageable", Bool()}, {"hitPoints", Num()},
                                     {"props", Ref("props")}},
                                    {"id", "mesh", "at"});
            const auto vision = Obj({{"target", Ref("id")}, {"heatIntensity", Num()}, {"heatOpacity", Num()}, {"heatRadius", Num()}, {"emfIntensity", Num()},
                                     {"emfOpacity", Num()}, {"hideInNormalVision", Bool()}, {"properties", Ref("props")}},
                                    {"target"});
            const auto environment = Obj({{"lights", Arr(light)}, {"sounds", Arr(sound)}, {"volumes", Arr(volume)}, {"ingredients", Arr(ingredient)},
                                          {"objects", Arr(object)}, {"vision", Arr(vision)}, {"props", Arr(Ref("generic"))}, {"fish", Arr(Ref("generic"))},
                                          {"softbody", Arr(Ref("generic"))}, {"animated", Arr(Ref("generic"))}, {"tutorials", Arr(Ref("generic"))}});

            const auto ge = Obj({{"id", Ref("id")}, {"kind", Enum({"ledge", "hoh", "pipe", "ladder", "zipline", "fence", "pole"})}, {"points", Arr(Ref("position"), 2, 3)}},
                                {"kind", "points"});
            const auto geTrigger = Obj({{"id", Ref("id")}, {"toggle", Bool()}, {"geMask", Range(Int(), 0, 2147483647)}, {"at", Ref("position")}}, {"id", "geMask"});
            const auto modeStrings = Obj({{"explicationSpy", Str()}, {"explicationMerc", Str()}, {"briefSpy", Str()}, {"briefMerc", Str()}, {"legendSpy", Str()}, {"legendMerc", Str()}});
            const auto mode = Obj({{"mode", Enum({"story", "diskhunt", "deathmatch"})}, {"respawn", Num()}, {"timeLimit", Num()}, {"spiesLives", Int()}, {"sentriesLives", Int()},
                                   {"scoreLimit", Int()}, {"handicap", Int()}, {"numDisksMax", Int()}, {"strings", modeStrings}},
                                  {"mode"});
            const auto settings = Obj({{"mapName", Str()}, {"briefSpy", Str()}, {"briefMerc", Str()}, {"legend", Str("Use | for line breaks.")}, {"legendInBrief", Str()},
                                       {"modes", Arr(mode, 0, 3)}});
            const auto level = Obj({{"song", Str()}, {"deathPawnHeight", Num()}, {"motionTracking", Enum({"standard", "enhanced"})}, {"events", Arr(levelEvent)},
                                    {"geActivationMask", Int()}});
            const auto interfaceTextures = Obj({{"menu", Str()}, {"briefing", Str()}, {"loadingAtt", Str()}, {"loadingDef", Str()}});
            const auto existingRef = Any({Obj({{"path", Str()}, {"class", Str()}, {"tag", Str()}}, {"path"}), Obj({{"tag", Str()}}, {"tag"})});
            const auto existing = Obj({{"refs", {{"type", "object"}, {"additionalProperties", existingRef}, {"propertyNames", Ref("id")}}}});
            const auto extra = Obj({{"operations", Arr(Open("A raw scct.map-changes operation."))}});
            const auto provenance = Obj({{"source", {{"description", "Where the level came from."}}}, {"frames", Arr(Json::object())},
                                         {"notes", Any({Str(), Arr(Str())})}});

            Json top{{"$schema", "https://json-schema.org/draft/2020-12/schema"}, {"$id", "scct://schema/level-spec"},
                     {"title", "scct.level-spec version 1"},
                     {"description", "A whole SCCT Versus level for the RE+ MCP level_build tool. Lengths are in units{unit} (Unreal units by default), yaw in degrees, Z up, east +X, north +Y."},
                     {"type", "object"}, {"additionalProperties", false}, {"required", Json::array({"format", "version"})}};
            top["properties"] = {
                {"format", {{"const", kFormat}}}, {"version", {{"const", kVersion}}},
                {"map", Obj({{"name", Str("The map's name, in capitals.")}, {"file", {{"type", "string"}, {"pattern", "^[A-Za-z0-9_]{1,60}$"}, {"description", "File stem; default the first four letters of the name and D."}}}})},
                {"units", Obj({{"unit", Enum({"uu", "m", "ft"})}, {"uuPerMetre", Range(Num(), 1, 10000)}})},
                {"profile", Enum({"draft", "release"})},
                {"defaults", Obj({{"construction", Enum({"Carve", "Shell"})}, {"thickness", Range(Num(), 1, 1024)}, {"material", Str()},
                                  {"lights", Enum({"rooms", "none"})}, {"startsPerTeam", Range(Int(), 1, 32)}})},
                {"storeys", Arr(Obj({{"id", Ref("id")}, {"z", Num()}, {"name", Str()}}, {"id", "z"}))},
                {"zones", Arr(Obj({{"id", Ref("id")}, {"locationName", Str()}, {"spaces", Arr(Ref("id"), 1)}, {"portals", Bool("Portals where its spaces meet other zones; default true.")}},
                                  {"id", "spaces"}))},
                {"geometry", geometry}, {"spawns", spawns}, {"gameplay", gameplay}, {"security", security}, {"mechanisms", mechanisms},
                {"events", events}, {"environment", environment}, {"ges", Arr(ge)}, {"geTriggers", Arr(geTrigger)},
                {"settings", settings}, {"level", level}, {"interface", interfaceTextures},
                {"release", Open("Release notes and options, kept with the spec.")}, {"existing", existing}, {"extra", extra}, {"provenance", provenance}};
            top["$defs"] = defs;
            return top;
        }();
        return schema;
    }

    // --- Checking against the schema -------------------------------------------
    namespace Check
    {
        // A schema pattern, compiled once. MS STL's ^ and $ also match beside
        // every '\n' (libstdc++'s only at the ends), so a pattern anchored at
        // both ends is matched against the whole string without its anchors:
        // regex_match anchors alike on both, and "Room1\nX" is no id anywhere.
        // A | outside any group or set ("^a|b$") anchors each side alone, so
        // such a pattern is searched as written.
        struct Compiled { std::regex regex; bool whole = false; };
        inline bool Anchored(const std::string& text)
        {
            if (text.size() < 2 || text.front() != '^' || text.back() != '$' || text[text.size() - 2] == '\\') return false;
            int depth = 0;
            bool inSet = false;
            for (size_t i = 1; i + 1 < text.size(); ++i)
            {
                const char c = text[i];
                if (c == '\\') { ++i; continue; }
                if (inSet) { inSet = c != ']'; continue; }
                if (c == '[') inSet = true;
                else if (c == '(') ++depth;
                else if (c == ')') --depth;
                else if (c == '|' && depth == 0) return false;
            }
            return true;
        }
        inline const Compiled& Pattern(const std::string& text)
        {
            thread_local std::map<std::string, Compiled> cache;
            auto found = cache.find(text);
            if (found == cache.end())
            {
                const bool whole = Anchored(text);
                found = cache.emplace(text, Compiled{std::regex(whole ? "(?:" + text.substr(1, text.size() - 2) + ")" : text), whole}).first;
            }
            return found->second;
        }
        inline bool Matches(const std::string& value, const std::string& pattern)
        {
            const auto& compiled = Pattern(pattern);
            return compiled.whole ? std::regex_match(value, compiled.regex) : std::regex_search(value, compiled.regex);
        }
        inline std::string Name(const Json& value)
        {
            if (value.is_object()) return "an object";
            if (value.is_array()) return "an array";
            if (value.is_string()) return "the text " + value.dump();
            if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
            if (value.is_null()) return "null";
            return "the number " + value.dump();
        }
        inline bool IsType(const Json& value, const std::string& type)
        {
            if (type == "object") return value.is_object();
            if (type == "array") return value.is_array();
            if (type == "string") return value.is_string();
            if (type == "boolean") return value.is_boolean();
            if (type == "null") return value.is_null();
            if (type == "number") return value.is_number() && std::isfinite(value.get<double>());
            if (type == "integer") return value.is_number_integer() || (value.is_number_float() && std::isfinite(value.get<double>()) && std::floor(value.get<double>()) == value.get<double>());
            return false;
        }
        inline const Json& Resolve(const Json& schema, const Json& root)
        {
            const auto ref = schema.at("$ref").get<std::string>();
            const std::string prefix = "#/$defs/";
            if (ref.compare(0, prefix.size(), prefix) != 0 || !root.at("$defs").contains(ref.substr(prefix.size()))) throw std::runtime_error("Unknown schema reference " + ref);
            return root.at("$defs").at(ref.substr(prefix.size()));
        }
        inline void Value(const Json& value, const Json& schema, const std::string& at, const Json& root);
        inline void Forms(const Json& value, const Json& schema, const std::string& at, const Json& root)
        {
            // The form that got furthest names the problem; when none got past
            // this value, the forms themselves are listed.
            std::vector<SpecError> errors;
            for (const auto& form : schema.at("anyOf"))
            {
                try { Value(value, form, at, root); return; }
                catch (const SpecError& e) { errors.push_back(e); }
            }
            const SpecError* best = nullptr;
            for (const auto& e : errors) if (!best || e.pointer.size() > best->pointer.size()) best = &e;
            if (best && best->pointer.size() > at.size()) throw *best;
            std::string forms;
            for (const auto& form : schema.at("anyOf"))
            {
                const auto& shown = form.contains("$ref") ? Resolve(form, root) : form;
                std::string text = shown.value("description", std::string());
                if (text.empty()) text = shown.contains("type") ? shown.at("type").dump() : std::string("another form");
                forms += (forms.empty() ? "" : "; ") + text;
            }
            if (schema.contains("description")) forms = schema.at("description").get<std::string>();
            throw SpecError(at, Name(value) + " is not an accepted form. Use " + forms + ".");
        }
        inline void Value(const Json& value, const Json& schema, const std::string& at, const Json& root)
        {
            if (schema.contains("$ref")) { Value(value, Resolve(schema, root), at, root); return; }
            if (schema.contains("anyOf")) { Forms(value, schema, at, root); return; }
            if (schema.contains("const") && value != schema.at("const")) throw SpecError(at, "must be " + schema.at("const").dump() + ", not " + Name(value) + ".");
            if (schema.contains("type"))
            {
                const auto& type = schema.at("type");
                bool ok = false;
                if (type.is_array()) { for (const auto& t : type) ok = ok || IsType(value, t.get<std::string>()); }
                else ok = IsType(value, type.get<std::string>());
                if (!ok)
                {
                    const auto wanted = type.is_array() ? type.dump() : type.get<std::string>();
                    const bool vowel = wanted == "integer" || wanted == "array" || wanted == "object";
                    throw SpecError(at, "expected " + std::string(vowel ? "an " : "a ") + wanted + ", not " + Name(value) + ".");
                }
            }
            if (schema.contains("enum"))
            {
                bool found = false;
                for (const auto& e : schema.at("enum")) found = found || e == value;
                if (!found)
                {
                    std::string choices;
                    for (const auto& e : schema.at("enum")) choices += (choices.empty() ? "" : ", ") + e.get<std::string>();
                    throw SpecError(at, Name(value) + " is not one of " + choices + ".");
                }
            }
            if (value.is_object())
            {
                if (schema.contains("required"))
                    for (const auto& key : schema.at("required"))
                        if (!value.contains(key.get<std::string>())) throw SpecError(at, "needs \"" + key.get<std::string>() + "\".");
                const auto properties = schema.value("properties", Json::object());
                for (auto it = value.begin(); it != value.end(); ++it)
                {
                    const auto where = Child(at, it.key());
                    if (schema.contains("propertyNames")) Value(Json(it.key()), schema.at("propertyNames"), where, root);
                    if (properties.contains(it.key())) Value(it.value(), properties.at(it.key()), where, root);
                    else if (schema.contains("additionalProperties"))
                    {
                        const auto& extra = schema.at("additionalProperties");
                        if (extra.is_boolean() && !extra.get<bool>())
                        {
                            std::string known;
                            for (auto p = properties.begin(); p != properties.end(); ++p) known += (known.empty() ? "" : ", ") + p.key();
                            throw SpecError(where, "unknown field \"" + it.key() + "\"." + (known.empty() ? std::string() : " Fields here: " + known + "."));
                        }
                        if (extra.is_object()) Value(it.value(), extra, where, root);
                    }
                }
            }
            if (value.is_array())
            {
                if (schema.contains("minItems") && value.size() < schema.at("minItems").get<size_t>())
                    throw SpecError(at, "needs at least " + schema.at("minItems").dump() + " item(s), not " + std::to_string(value.size()) + ".");
                if (schema.contains("maxItems") && value.size() > schema.at("maxItems").get<size_t>())
                    throw SpecError(at, "takes at most " + schema.at("maxItems").dump() + " item(s), not " + std::to_string(value.size()) + ".");
                if (schema.value("uniqueItems", false))
                    for (size_t i = 0; i < value.size(); ++i)
                        for (size_t j = i + 1; j < value.size(); ++j)
                            if (value[i] == value[j]) throw SpecError(Child(at, j), value[j].dump() + " is listed twice.");
                if (schema.contains("items"))
                    for (size_t i = 0; i < value.size(); ++i) Value(value[i], schema.at("items"), Child(at, i), root);
            }
            if (value.is_string() && schema.contains("pattern") && !Matches(value.get<std::string>(), schema.at("pattern").get<std::string>()))
                throw SpecError(at, Name(value) + (schema.contains("description") ? " does not fit: " + schema.at("description").get<std::string>() : " has the wrong form."));
            if (value.is_number())
            {
                const double n = value.get<double>();
                if (schema.contains("minimum") && n < schema.at("minimum").get<double>()) throw SpecError(at, value.dump() + " is below the minimum " + schema.at("minimum").dump() + ".");
                if (schema.contains("maximum") && n > schema.at("maximum").get<double>()) throw SpecError(at, value.dump() + " is above the maximum " + schema.at("maximum").dump() + ".");
            }
        }
    }

    // Every "id" in a spec with its pointer: items' own ids and the names
    // existing.refs gives to live actors. Raw bags (props, documents, extra
    // operations, release, provenance) are not looked into.
    inline void CollectIds(const Json& value, const std::string& at, std::vector<std::pair<std::string, std::string>>& out)
    {
        if (value.is_object())
        {
            for (auto it = value.begin(); it != value.end(); ++it)
            {
                const auto& key = it.key();
                if (key == "props" || key == "properties" || key == "document" || key == "release" || key == "provenance" || key == "extra") continue;
                if (key == "id" && it.value().is_string()) out.push_back({it.value().get<std::string>(), Child(at, key)});
                else if (key == "refs" && at == "/existing" && it.value().is_object())
                    for (auto ref = it.value().begin(); ref != it.value().end(); ++ref) out.push_back({ref.key(), Child(Child(at, key), ref.key())});
                else CollectIds(it.value(), Child(at, key), out);
            }
        }
        else if (value.is_array())
            for (size_t i = 0; i < value.size(); ++i) CollectIds(value[i], Child(at, i), out);
    }
    // The file stem a map name suggests: its first four letters and D (AquaD).
    inline std::string DefaultFile(const std::string& name)
    {
        std::string letters;
        for (unsigned char c : name)
            if (std::isalpha(c) && letters.size() < 4) letters += static_cast<char>(letters.empty() ? std::toupper(c) : std::tolower(c));
        return (letters.empty() ? std::string("Map") : letters) + "D";
    }
    // Checks a spec's shape and returns it normalised: the format's defaults
    // filled in for map, units, profile and defaults. Throws SpecError at the
    // first problem, with the JSON pointer of the part at fault.
    inline Json Parse(const Json& spec)
    {
        if (!spec.is_object()) throw SpecError("", "A level spec is a JSON object with \"format\": \"scct.level-spec\" and \"version\": 1.");
        if (!spec.contains("format") || spec.at("format") != kFormat) throw SpecError("/format", "must be \"scct.level-spec\".");
        if (!spec.contains("version") || spec.at("version") != kVersion) throw SpecError("/version", "must be 1; this server reads scct.level-spec version 1.");
        Check::Value(spec, Schema(), "", Schema());
        std::vector<std::pair<std::string, std::string>> ids;
        CollectIds(spec, "", ids);
        std::map<std::string, std::string> seen;
        for (const auto& [id, pointer] : ids)
        {
            const auto [at, fresh] = seen.emplace(Workflow::Fold(id), pointer);
            if (!fresh) throw SpecError(pointer, "the id \"" + id + "\" is already used at " + at->second + "; ids are unique ignoring case, because each becomes an actor's Tag.");
        }
        Json out = spec;
        auto& map = out["map"];
        if (!map.is_object()) map = Json::object();
        if (!map.contains("name")) map["name"] = "UNTITLED";
        if (!map.contains("file")) map["file"] = DefaultFile(map.at("name").get<std::string>());
        auto& units = out["units"];
        if (!units.is_object()) units = Json::object();
        if (!units.contains("unit")) units["unit"] = "uu";
        if (!units.contains("uuPerMetre")) units["uuPerMetre"] = 100;
        if (!out.contains("profile")) out["profile"] = "draft";
        auto& defaults = out["defaults"];
        if (!defaults.is_object()) defaults = Json::object();
        if (!defaults.contains("construction")) defaults["construction"] = "Carve";
        if (!defaults.contains("thickness")) defaults["thickness"] = 16;
        if (!defaults.contains("lights")) defaults["lights"] = "rooms";
        if (!defaults.contains("startsPerTeam")) defaults["startsPerTeam"] = 4;
        return out;
    }
    // Unreal units per spec unit.
    inline double Scale(const Json& spec)
    {
        const auto units = spec.value("units", Json::object());
        const auto unit = units.value("unit", std::string("uu"));
        const double perMetre = units.value("uuPerMetre", 100.0);
        if (unit == "m") return perMetre;
        if (unit == "ft") return perMetre * 0.3048;
        return 1;
    }

    // A fragment laid over a spec: objects merge key by key, arrays of items
    // with ids merge by id (the fragment's fields win, new ids are appended),
    // any other value is replaced. An item {"id": x, "$remove": true} takes x
    // out, which is how an intent tool deletes one thing.
    inline bool Keyed(const Json& list)
    {
        if (!list.is_array() || list.empty()) return false;
        for (const auto& item : list) if (!item.is_object() || !item.contains("id") || !item.at("id").is_string()) return false;
        return true;
    }
    inline Json Merge(const Json& base, const Json& fragment)
    {
        if (!base.is_object() || !fragment.is_object()) return fragment;
        Json out = base;
        for (auto it = fragment.begin(); it != fragment.end(); ++it)
        {
            const auto& key = it.key();
            const auto& value = it.value();
            const bool bag = key == "props" || key == "properties" || key == "document";
            if (!out.contains(key))
            {
                // Taken whole, but through Merge, so removals nested in it go too.
                if (value.is_object() && !bag) { out[key] = Merge(Json::object(), value); continue; }
                if (!Keyed(value)) { out[key] = value; continue; }
                Json added = Json::array();
                for (const auto& item : value) if (!item.value("$remove", false)) added.push_back(item);
                out[key] = added;
            }
            else if (out.at(key).is_object() && value.is_object() && !bag) out[key] = Merge(out.at(key), value);
            else if (Keyed(value) && (out.at(key).is_array() && (out.at(key).empty() || Keyed(out.at(key)))))
            {
                Json merged = out.at(key);
                for (const auto& item : value)
                {
                    const auto id = Workflow::Fold(item.at("id").get<std::string>());
                    auto found = std::find_if(merged.begin(), merged.end(), [&](const Json& m) { return Workflow::Fold(m.at("id").get<std::string>()) == id; });
                    const bool remove = item.value("$remove", false);
                    if (found == merged.end()) { if (!remove) merged.push_back(item); }
                    else if (remove) merged.erase(found);
                    else
                    {
                        // The base keeps its spelling of the id, which other items name.
                        const auto spelling = found->at("id");
                        *found = Merge(*found, item);
                        (*found)["id"] = spelling;
                    }
                }
                out[key] = merged;
            }
            else out[key] = value;
        }
        // Strip the markers of items the fragment adds that had nothing to remove.
        std::function<void(Json&)> strip = [&](Json& v)
        {
            if (v.is_object()) { v.erase("$remove"); for (auto& child : v) strip(child); }
            else if (v.is_array()) for (auto& child : v) strip(child);
        };
        strip(out);
        return out;
    }
    // FNV-1a over the normalised spec's text: the sidecar's record of what was
    // built.
    inline std::string Hash(const Json& spec)
    {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : spec.dump()) { h ^= c; h *= 1099511628211ull; }
        static const char* digits = "0123456789abcdef";
        std::string out(16, '0');
        for (int i = 15; i >= 0; --i) { out[static_cast<size_t>(i)] = digits[h & 15]; h >>= 4; }
        return out;
    }
}
