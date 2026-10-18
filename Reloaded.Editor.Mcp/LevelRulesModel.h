#pragma once
// The wiki's rules for a playable Versus map, checked the same way on a
// compiled level spec (offline) and on the map open in the editor (live):
// spawns, Story / Disk Hunt / Deathmatch, objectives, CamNet, security,
// zones, lights and other actors, Map Settings and interface textures,
// release packages, geometric event measurements, plus the Map Design check
// (MapDesignPanel.inl), Map Check rows and the lighting budget folded in.
// Every finding names its rule, the wiki page it comes from (scct://wiki/<page>)
// and, where there is one, the MCP tool call that fixes it. map_validate
// (Tools/ValidateTools.cpp) gathers a live scene; scct://rules is
// CatalogMarkdown(). Engine-free. Tests: tests/LevelRulesModelTests.cpp.
//
// The scene (SceneFromJson; Compiled.scene of LevelSpecCompiler.h):
//   {actors: [{path | id, class, name?, tag?, event?, position: [x,y,z] (or
//              values.Location), values: {Property: value}}],
//    pieces: [{spec: {kind, width, length, height, ...}, position, rotation}
//             (a Map Design piece or design.blockbatch item) | {id, kind,
//             size: [x,y,z], at: [x,y(,z)], yaw (degrees)}],
//    ges: [{kind | command, points, bars?, barSpacing?, surface?}],
//    level: {LevelInfo values} | {deathPawnHeight, motionTracking, events},
//    mapSettings | settings: SMapSettings values (MapName, Modes,
//             ModesStrings, ...) or the spec's settings section, or
//             mapsettings.get's {saved, settings, textures, values},
//    modes: ["story", "diskhunt", "deathmatch"] (the modes the map must play),
//    mapName, mapFile (or map: {name, file}), geometryBuilt, live,
//    release: {packages: [name | {name, file, stock}], uas: {packages},
//              interface: {menu, briefing, loadingAtt, loadingDef},
//              findings: the release tool's check findings [{severity, rule,
//              message, fix}], of which release.files, release.dependencies
//              and release.optimise are folded in (only it reads the files)},
//    playerCounts: [{objective, min, max}], targets: [{mission, sizes[8]}],
//    documents: [scct.map-changes], schema: authoring.schema's result,
//    mapCheck: mapcheck.run's result, lighting: lighting.budget's result}
// Values are written as Map JSON and InspectActor write them: every leaf a
// string, strings JSON-quoted, enums bare, bools True/False, references
// Class'MyLevel.Name', Class'Name', {"$ref": "id"} or a bare name, structs
// as objects and arrays as arrays. A value named "$floor" (the height of the
// floor under the actor) or "$blocked" (true when geometry touches it) is
// what map_validate traced in the live map; a GE's "$floors" lists
// [{at, floor}] for the points Ge::Probes names.
#include "../Reloaded.Editor/WorkflowModel.h"
// The editor's own models put two statements on one line after an if, which
// g++'s -Wall reads as misleading indentation; MSVC has no such warning.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "../Reloaded.Editor/CameraNetworkModel.h"
#include "../Reloaded.Editor/MapAuthoringModel.h"
#include "../Reloaded.Editor/MapDesignModel.h"
#include "../Reloaded.Editor/McpBridgeModel.h"
#include "../Reloaded.Editor/ObjectivePlayersModel.h"
#include "../Reloaded.Editor/SecurityModel.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "GeometricEventModel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Mcp::Rules
{
    using Json = Workflow::Json;
    struct Actor { std::string path, cls, name, tag, event; Workflow::Vector position{}; Json values = Json::object(); };
    struct Scene
    {
        std::vector<Actor> actors;
        Json pieces = Json::array();
        Json ges = Json::array();
        Json level = Json::object();
        Json mapSettings; // null if unknown
        std::set<std::string> modes; // requested modes: story/diskhunt/deathmatch
        std::string mapName, mapFile;
        bool geometryBuilt = false;
        bool live = false;
        Json release = Json::object();
        // Beyond the contract: what map_validate fetched besides the actors.
        Json mapCheck;                  // mapcheck.run's result; null when not run
        Json lighting;                  // lighting.budget's result; null when not run
        Json documents = Json::array(); // scct.map-changes documents to check with Authoring::Validate
        Json schema;                    // authoring.schema's result, for values a document writes
    };
    struct Finding { std::string severity, rule, wikiPage, message; std::vector<std::string> actors; Json fix = Json(); };
    // draft and release are each rule's severity in that profile: error,
    // warning or info, or "reported" where the check it folds in (Map Check,
    // the Map Design check) gives each row its own.
    struct RuleInfo { std::string id, wikiPage, draft, release, description; };

    // The wiki's pages (scct://wiki/<slug>); every rule cites one.
    inline const std::vector<std::string>& WikiPages()
    {
        static const std::vector<std::string> pages = {
            "home", "spawn-points", "game-modes", "map-settings", "level-properties", "vision-mode-properties", "geometric-events", "events", "alarms", "movers",
            "elevators", "lighting", "material-classes", "static-meshes", "sound-actors", "sound-browser", "gameplay-objects", "buttons-and-hacking-panels",
            "level-ingredients", "zones-and-volumes", "softbody-actors", "animated-meshes", "fish", "map-tutorials", "map-packaging"};
        return pages;
    }

    inline const std::vector<RuleInfo>& Catalog()
    {
        static const std::vector<RuleInfo> rules = {
            // Spawns.
            {"spawns.team-missing", "spawn-points", "error", "error", "Each team needs a PlayerStart: TeamNumber 0 is the Spies, 1 the Mercenaries."},
            {"spawns.too-few", "spawn-points", "warning", "error", "At least 3 starts per team, so 2v2 and 3v1 matches find a start for every player (RE+ places 4)."},
            {"spawns.in-solid", "spawn-points", "warning", "warning", "A start inside solid geometry: outside the open space the blockout carves."},
            {"spawns.spy-near-objective", "spawn-points", "warning", "warning", "A spy start within 512 units of an objective: the spies reach it before the mercs can respond."},
            {"spawns.teams-close", "spawn-points", "warning", "warning", "A spy start and a merc start within 768 units of each other: the teams meet immediately."},
            {"spawns.objective-far", "spawn-points", "warning", "warning", "An objective more than 4096 units from every merc start: hard to defend."},
            {"layout.unreachable", "spawn-points", "warning", "error", "An objective the blockout gives the spies no way to reach from their starts (rooms joined by doorways, corridors, stairs, lifts and climbing GEs)."},
            // Story and the other modes.
            {"story.no-mission", "game-modes", "error", "error", "No SMission: the map has no game mode to play."},
            {"story.no-multi", "game-modes", "error", "error", "Story is asked for but no SMission has GameMode GM_Multi."},
            {"story.no-objective", "game-modes", "warning", "warning", "No SObjective: there is nothing for the spies to do in Story."},
            {"story.unresolved-reference", "game-modes", "error", "error", "An SMission's Objectives or Triggers, or an SObjective's Triggers, names an actor that is not in the map."},
            {"story.trigger-kind", "game-modes", "warning", "warning", "A list names the wrong kind of actor: Objectives hold SObjectives (or sector SMissions), Triggers hold SComputerObjectiveTrigger, SFlag, SBombTargetObjectiveTrigger or SDisk."},
            {"story.objective-orphan", "game-modes", "error", "error", "An SObjective in no SMission's Objectives list never counts."},
            {"story.objective-no-trigger", "game-modes", "warning", "warning", "An SObjective with no Triggers: the spies cannot complete it."},
            {"story.mission-empty", "game-modes", "error", "error", "A Story or sector SMission whose Objectives list is empty: nothing ends the round."},
            {"story.minimum-objectives", "game-modes", "error", "error", "MinimumObjectives is more than the mission's objectives can give, so the spies can never win."},
            {"story.sector-chain", "game-modes", "error", "error", "Sector maps: the GM_Multi mission lists the sector SMissions with bChained True; each sector is GM_Undefined with bChained False."},
            {"story.visibility-zone", "game-modes", "error", "error", "An SObjective's TriggersERVisibilityZone must name a ZoneInfo."},
            {"modes.missing", "game-modes", "warning", "warning", "Story (GM_Multi), Disk Hunt (GM_DiskHunt) or Deathmatch (GM_DeathMatch) has no SMission, so a player who picks that mode gets nothing."},
            {"objectives.flag-dropzones", "game-modes", "error", "error", "An SFlag's DropZone list names 1 to 5 SFlagDropZone actors."},
            {"objectives.dropzone-unused", "game-modes", "warning", "warning", "An SFlagDropZone no SFlag lists."},
            {"objectives.terminal-placement", "game-modes", "warning", "warning", "An SComputerObjectiveTrigger sits about 78 units above the floor and clear of collision, or players cannot capture it."},
            {"objectives.terminal-capture-time", "game-modes", "warning", "warning", "CaptureTime does nothing on an SComputerObjectiveTrigger; set DominationCaptureTime."},
            {"diskhunt.disk-count", "game-modes", "error", "error", "Disk Hunt needs SDisks listed in its SMission's Triggers, at least ScoreLimit of them, and Handicap and NumDisksMax within what is placed."},
            {"diskhunt.disk-group", "game-modes", "error", "error", "SDisk Group is 1, 2 or 3."},
            {"diskhunt.disk-clearance", "game-modes", "warning", "warning", "An SDisk touching the floor or other geometry cannot be picked up."},
            {"diskhunt.disk-unlisted", "game-modes", "warning", "warning", "An SDisk the Disk Hunt SMission's Triggers do not list never spawns."},
            {"deathmatch.security", "game-modes", "warning", "warning", "Deathmatch has no objectives: its SMission's AtResetEvent usually untriggers the security, which stays live otherwise."},
            {"objectiveplayers.problems", "game-modes", "error", "error", "Objective player counts leave a mission with nothing to play for some lobby size."},
            {"objectiveplayers.targets", "game-modes", "warning", "warning", "Objectives to win by match size ask for more than that match can give."},
            // CamNet and level ingredients.
            {"camnet.missing", "level-ingredients", "error", "error", "No SCamNetwork: mercs who use the Camera Network gadget are softlocked."},
            {"camnet.first-cam", "level-ingredients", "error", "error", "Exactly one SCamNetwork has bFirstCam True."},
            {"camnet.loop", "level-ingredients", "error", "error", "The SCamNetwork cameras form one closed loop through NextCam and PrevCam."},
            {"ingredients.collision", "level-ingredients", "warning", "warning", "SCamNetwork and SInfirmary have no collision by default: set bBlockActors and bBlockPlayers True."},
            {"ingredients.interaction", "level-ingredients", "info", "info", "Interaction prompts as the Enhanced maps set them: SAmmoStock 60 high / 120 radius, SInfirmary 120 / 60, SInterrupteur 120 / 60."},
            {"ingredients.sprinkler-hidden", "level-ingredients", "warning", "warning", "An SSprinkler shows its sprite in game unless bHidden is True."},
            // Security.
            {"security.dangling-event", "alarms", "error", "error", "A detector's Event names a Tag nothing in the map has."},
            {"security.sensor-no-volume", "alarms", "error", "error", "An SVolumetricSensor whose Volumes list is empty never fires."},
            {"security.detector-unwired", "alarms", "warning", "warning", "A detector whose Event is None triggers nothing: point it at an SAlarm's Tag."},
            {"security.alarm-unfed", "alarms", "warning", "warning", "An SAlarm no detector's Event reaches."},
            {"security.alarm-no-output", "alarms", "warning", "warning", "An SAlarm with no Events, MoversToLock or ObjectiveTriggersToLock does nothing when it goes off."},
            {"security.bomb-locked", "alarms", "error", "error", "An SBombTargetObjectiveTrigger cannot be locked by an alarm."},
            {"security.lock-unresolved", "alarms", "error", "error", "An SAlarm's MoversToLock or ObjectiveTriggersToLock names an actor that is not in the map."},
            {"security.message-priority", "alarms", "warning", "warning", "SAlarm MessagePriority is 0 to 5."},
            {"security.wiring", "alarms", "info", "info", "Each alarm with the detectors that feed it and what it triggers (Security::Wiring)."},
            // Zones.
            {"zones.alarm-zone", "zones-and-volumes", "warning", "warning", "SAlarmZoneInfo was never used by Ubisoft and misbehaves; use a Volume with an SVolumetricSensor."},
            {"zones.location-name", "zones-and-volumes", "info", "info", "A ZoneInfo without LocationName shows no location on the HUD."},
            {"zones.leak", "zones-and-volumes", "warning", "warning", "Map Check found a zone leak: a portal does not seal its zone."},
            // Other actors.
            {"lighting.static-effect", "lighting", "error", "error", "Lights use LightEffect LE_None, LE_Spotlight or LE_Sunlight (LE_ESpotShadow for Echelon lights); the others no longer work."},
            {"lighting.echelon-normal", "lighting", "info", "info", "Echelon lights (LE_ESpotShadow) usually set EchelonNormalInfluence 0.9."},
            {"objects.pickable", "gameplay-objects", "error", "error", "bPickable is unfinished and never shipped: leave it False."},
            {"objects.chaff", "gameplay-objects", "info", "info", "Keep bCanBeChaffed off; the game turns it on where STriggerLights need it."},
            {"softbody.duplicated", "softbody-actors", "error", "error", "A duplicated SoftBody actor shares its ESoftBody with the original: create each one new."},
            {"movers.static-mesh", "movers", "warning", "warning", "Movers use a static mesh for their look."},
            {"fish.pilot", "fish", "warning", "warning", "A fish's Pilot is an SFish or SFish_Big with bPilote True."},
            {"tutorials.hidden", "map-tutorials", "warning", "warning", "Tutorial actors (SZoneTips) set bHidden True, or their sprite shows in game."},
            {"events.dangling", "events", "warning", "warning", "An Event (or AtResetEvent, alarm Events, level event) names a Tag nothing in the map has."},
            // Level properties.
            {"level.death-height", "level-properties", "error", "error", "Nothing players need sits below fDeathPawnHeight, where they die instantly."},
            {"level.send-at-time", "level-properties", "warning", "warning", "A level event's SendAtTime of 0 or more never fires; negative values count back from the end of the match."},
            {"level.motion-tracking", "level-properties", "info", "info", "Motion Tracking Vision: most maps use MTVOffset 52 / MTVSlope 85, Enhanced maps 8 / 8."},
            // Packages and documents.
            {"release.package-names", "map-packaging", "error", "error", "Release packages have unique base names and never overwrite a stock package."},
            {"authoring.validate", "home", "error", "error", "A scct.map-changes document the editor's Import Map from JSON would refuse."},
            {"authoring.unwritable", "home", "error", "error", "A value Map JSON cannot write, or one the property's choices do not include (authoring.schema)."},
            // Map Settings and interface.
            {"settings.missing", "map-settings", "info", "warning", "No SMapSettings yet: save the map, exit and reopen it, then fill Map Settings."},
            {"settings.ranges", "map-settings", "warning", "warning", "Map Settings Modes: RespawnTime 5-15, SpiesLives and SentriesLives 0-10, TimeLimit 0-20."},
            {"settings.map-name-caps", "map-settings", "warning", "warning", "MapName is in capitals, the Ubisoft standard."},
            {"settings.mode-parity", "map-settings", "warning", "warning", "Modes[i] and ModesStrings[i] describe the same mode, once each."},
            {"settings.modes-missing", "map-settings", "warning", "warning", "Every mode the map has an SMission for has a Map Settings Modes entry."},
            {"interface.textures", "map-settings", "info", "warning", "<Map>-i holds Menu and Briefing (256x256 DXT5) and LoadingATT0 and LoadingDEF0 (512x512 DXT1)."},
            // Release.
            {"release.uas", "sound-browser", "warning", "warning", "The map's .uas includes Interface (and the map's own Amb_ sounds)."},
            {"release.file-name", "map-packaging", "warning", "warning", "The map file is an ASCII .sdc whose name has no spaces."},
            {"release.naming-convention", "map-packaging", "info", "info", "Map file: first 4 letters of the map name plus a suffix letter (D for the default version); packages <Map>_TXT, <Map>_STM, Amb_<Map>."},
            // Found by the release tool's file check (release action check) and
            // folded in from its findings, at the severity it gave each row.
            {"release.files", "map-packaging", "reported", "reported", "The files a release ships are on disk and current: Packages\\MapsEd and Packages\\Maps <Map>.sdc saved since the last change, and <Map>-i.utc without a stray <Map>-i.utx."},
            {"release.dependencies", "map-packaging", "reported", "reported", "Every package the map imports is in Packages, and a stock one is unchanged from a clean install, which is what players have."},
            {"release.optimise", "map-packaging", "info", "info", "A stock package the map uses little of can be cut down to a release copy with only those assets (release action optimise)."},
            // Geometric events.
            {"ge.points", "geometric-events", "error", "error", "A GE is LG, HOH, PIPE, LADDER, ZL, FENCE or POLE with 2 points (3 for a pipe or ladder)."},
            {"ge.ledge-height", "geometric-events", "warning", "warning", "Ledges are grabbed 208-320 units up standing, up to 448 with a running jump, or at 512 with a co-op boost."},
            {"ge.ledge-reach", "geometric-events", "info", "info", "A ledge only a running jump (320-448) or a co-op boost (512, with 144 units of platform) reaches."},
            {"ge.obstacle", "geometric-events", "warning", "warning", "A ledge under 115 units is jumped, not climbed."},
            {"ge.pull-up", "geometric-events", "warning", "warning", "64 units from a ledge to the wall behind it to pull up and stand."},
            {"ge.ledge-length", "geometric-events", "warning", "warning", "A ledge is at least 32 units long."},
            {"ge.fence-height", "geometric-events", "warning", "warning", "The fence jump needs a fence 88 to 128 units high."},
            {"ge.fence-recommended", "geometric-events", "info", "info", "96 units is the recommended fence height."},
            {"ge.fence-climbable", "geometric-events", "info", "info", "A fence is climbable only with bClimbable on its texture (Echelon category), as well as its GE."},
            {"ge.hoh-clearance", "geometric-events", "warning", "warning", "216 units of clearance below a hand-over-hand bar."},
            {"ge.ladder-bars", "geometric-events", "warning", "warning", "A ladder has an even number of bars, at least 8 (RE+ counts 32 units a bar unless the GE gives bars or barSpacing)."},
            {"ge.zipline-hang", "geometric-events", "warning", "warning", "A zip line starts at most 384 units above the floor."},
            {"ge.zipline-clearance", "geometric-events", "warning", "warning", "198 units of clearance below a zip line (161 with the spy tucked)."},
            {"ge.zipline-slope", "geometric-events", "warning", "warning", "A zip line slopes: its ends are at different heights."},
            {"ge.point-order", "geometric-events", "warning", "warning", "A pipe or ladder starts at its lower end, with the side point at that height."},
            // Folded-in checks.
            {"design.layout", "home", "warning", "error", "Map Design check: a room no doorway, corridor or vent reaches, a doorway cutting into nothing, a piece that cannot be measured (Design::DesignIssues)."},
            {"design.traversal", "home", "warning", "warning", "Map Design check: steps, ramps, platforms and openings against the movement limits (Design::TraversalWarnings), and spaces that merge."},
            {"design.stale-geometry", "home", "warning", "warning", "Brushes changed since the last geometry build (or none was built): build before testing."},
            {"mapcheck.row", "home", "reported", "reported", "A row of the editor's Map Check (RE+ Tools > Map Check), at its own severity."},
            {"lighting.budget", "lighting", "warning", "warning", "Lights over the lighting budget's build warning: too many lights reach one BSP leaf or overlap."},
            {"lighting.guideline", "lighting", "info", "info", "Lights over the lighting budget's guideline (every shipped map is; the build warning is what matters)."},
            // Overview.
            {"info.summary", "home", "info", "info", "What the map holds: starts per team, missions, objectives, cameras, security, lights, GEs and pieces."},
            {"info.travel", "spawn-points", "info", "info", "How far each side's nearest start is from each objective, as the crow flies, with the time at 300 units a second."}};
        return rules;
    }
    inline const RuleInfo* FindRule(const std::string& id)
    {
        for (const auto& rule : Catalog()) if (rule.id == id) return &rule;
        return nullptr;
    }
    // What map_validate's include list switches a rule with: wiki, design,
    // security, mapcheck, lighting or release.
    inline std::string Group(const std::string& rule)
    {
        auto starts = [&](const char* prefix) { return rule.rfind(prefix, 0) == 0; };
        if (starts("design.") || starts("layout.")) return "design";
        if (starts("security.")) return "security";
        if (starts("mapcheck.") || rule == "zones.leak") return "mapcheck";
        if (rule == "lighting.budget" || rule == "lighting.guideline") return "lighting";
        if (starts("release.") || starts("settings.") || starts("interface.")) return "release";
        return "wiki";
    }

    // Property names actors.query must fetch for Check. SoftBody and
    // SBody are guesses at the native ESoftBody reference, which the wiki
    // does not name; a missing field is left out of the reply.
    inline std::vector<std::string> RuleFields()
    {
        return {"TeamNumber", "GameMode", "Objectives", "Triggers", "bChained", "MinimumObjectives", "AtResetEvent", "ObjectiveName",
                "TriggersERVisibilityZone", "DropZone", "Group", "CaptureTime", "DominationCaptureTime", "ResetTime",
                "NextCam", "PrevCam", "bFirstCam", "CamName", "bBlockActors", "bBlockPlayers",
                "Events", "MessagePriority", "MoversToLock", "ObjectiveTriggersToLock", "Duration", "Volumes", "DetectedClass",
                "LightEffect", "EchelonNormalInfluence", "bPickable", "bCanBeChaffed", "LocationName", "bDetectIntruders",
                "fInteractionHeight", "fInteractionRadius", "bHidden", "Pilot", "bPilote", "StaticMesh",
                "Subject", "MinPlayers", "MaxPlayers", "WinMission", "WinTarget", "SoftBody", "SBody"};
    }

    namespace Detail
    {
        inline std::string Lower(std::string text)
        {
            for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }
        inline std::string Last(const std::string& path)
        {
            const auto dot = path.find_last_of('.');
            return dot == std::string::npos ? path : path.substr(dot + 1);
        }
        inline std::string ClassKey(const std::string& cls) { return Lower(Last(cls)); }
        inline bool EndsWith(const std::string& text, const std::string& tail) { return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0; }
        inline bool IsNone(const std::string& text) { return text.empty() || Lower(text) == "none"; }
        inline std::string Round(double value, int decimals = 0)
        {
            std::ostringstream text;
            text.setf(std::ios::fixed);
            text.precision(decimals);
            text << value;
            return text.str();
        }
        // A leaf as plain text: a JSON-quoted string unquoted, bools as
        // True/False, numbers as written.
        inline std::string Text(const Json& v)
        {
            if (v.is_string())
            {
                const auto s = v.get<std::string>();
                if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
                {
                    const auto parsed = Json::parse(s, nullptr, false);
                    return parsed.is_string() ? parsed.get<std::string>() : s.substr(1, s.size() - 2);
                }
                return s;
            }
            if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
            if (v.is_number_integer()) return std::to_string(v.get<long long>());
            if (v.is_number()) { std::ostringstream t; t << v.get<double>(); return t.str(); }
            if (v.is_null()) return {};
            return v.dump();
        }
        inline bool Truth(const Json* v)
        {
            if (!v) return false;
            const auto t = Lower(Text(*v));
            return t == "true" || t == "1" || t == "yes";
        }
        inline std::optional<double> Number(const Json* v)
        {
            if (!v || v->is_null()) return std::nullopt;
            if (v->is_number()) return v->get<double>();
            if (v->is_boolean()) return v->get<bool>() ? 1.0 : 0.0;
            const auto t = Text(*v);
            if (t.empty()) return std::nullopt;
            char* end = nullptr;
            const double n = std::strtod(t.c_str(), &end);
            if (end == t.c_str() || !std::isfinite(n)) return std::nullopt;
            return n;
        }
        // A property by name, ignoring case as the engine does.
        inline const Json* Field(const Json& values, const std::string& name)
        {
            if (!values.is_object()) return nullptr;
            const auto exact = values.find(name);
            if (exact != values.end()) return &*exact;
            const auto key = Lower(name);
            for (auto it = values.begin(); it != values.end(); ++it) if (Lower(it.key()) == key) return &it.value();
            return nullptr;
        }
        inline const Json* Field(const Actor& a, const std::string& name) { return Field(a.values, name); }
        inline std::string TextOf(const Actor& a, const std::string& name)
        {
            const auto* v = Field(a, name);
            return v ? Text(*v) : std::string();
        }
        inline std::optional<double> NumberOf(const Actor& a, const std::string& name) { return Number(Field(a, name)); }
        inline Json Get(const Json& j, const char* key)
        {
            return j.is_object() && j.contains(key) ? j.at(key) : Json();
        }
        inline std::string GetString(const Json& j, std::initializer_list<const char*> keys)
        {
            for (const char* key : keys)
                if (j.is_object() && j.contains(key) && j.at(key).is_string()) return j.at(key).get<std::string>();
            return {};
        }

        // A reference in a value: Class'Path', a bare name, or {"$ref": id}.
        struct Ref { std::string text, cls, path, id; };
        inline void CollectRefs(const Json& v, std::vector<Ref>& out, bool bare, int depth = 0)
        {
            if (depth > 8) return;
            if (v.is_array()) { for (const auto& item : v) CollectRefs(item, out, bare, depth + 1); return; }
            if (v.is_object())
            {
                if (v.contains("$ref") && v.at("$ref").is_string()) { out.push_back({"$ref:" + v.at("$ref").get<std::string>(), "", "", v.at("$ref").get<std::string>()}); return; }
                // Inside a struct only typed references count: its other leaves are enums and numbers.
                for (const auto& item : v) CollectRefs(item, out, false, depth + 1);
                return;
            }
            if (!v.is_string()) return;
            auto s = v.get<std::string>();
            const auto first = s.find_first_not_of(" \t"), lastChar = s.find_last_not_of(" \t");
            if (first == std::string::npos) return;
            s = s.substr(first, lastChar - first + 1);
            if (IsNone(s) || s.front() == '"') return;
            const auto quote = s.find('\'');
            if (quote != std::string::npos && quote > 0 && s.back() == '\'' && s.size() > quote + 1)
            {
                auto path = s.substr(quote + 1, s.size() - quote - 2);
                if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
                if (!IsNone(path)) out.push_back({s, s.substr(0, quote), path, ""});
                return;
            }
            if (bare && s.find_first_of(" '\"(),=") == std::string::npos) out.push_back({s, "", s, ""});
        }
        // The event names a value sends: fields called Event, Events or
        // AtResetEvent anywhere inside it (an SMagicEvent's Groups hold them
        // in EventGroup entries).
        inline void SentEvents(const Json& v, std::vector<std::string>& out, bool named = false, int depth = 0)
        {
            if (depth > 8) return;
            if (v.is_object())
            {
                for (auto it = v.begin(); it != v.end(); ++it)
                {
                    const auto key = Lower(it.key());
                    SentEvents(it.value(), out, key == "event" || key == "events" || key == "atresetevent", depth + 1);
                }
            }
            else if (v.is_array()) { for (const auto& item : v) SentEvents(item, out, named, depth + 1); }
            else if (named && v.is_string()) { const auto t = Text(v); if (!IsNone(t)) out.push_back(t); }
        }
        inline std::vector<Ref> Refs(const Json* v)
        {
            std::vector<Ref> out;
            if (v) CollectRefs(*v, out, true);
            return out;
        }
        inline std::string Join(const std::vector<std::string>& items, const std::string& separator = ", ")
        {
            std::string out;
            for (const auto& item : items) out += (out.empty() ? "" : separator) + item;
            return out;
        }
        inline std::string Shown(const Ref& r) { return r.id.empty() ? (r.cls.empty() ? r.path : r.text) : r.id; }

        // The classes the rules know, by the last part of the class path.
        enum class Kind { Other, Start, Mission, Objective, Terminal, Bomb, Flag, DropZone, Trigger, Disk, CamNet, Alarm, Detector, Zone, AlarmZone,
                          SoftBody, PlayerRules, Lift, Mover, Fish, Tutorial, Infirmary, Ammo, Switch, Sprinkler };
        inline Kind KindOf(const std::string& cls)
        {
            const auto k = ClassKey(cls);
            if (EndsWith(k, "playerstart")) return Kind::Start;
            if (k == "smission") return Kind::Mission;
            if (k == "sobjective") return Kind::Objective;
            if (k == "scomputerobjectivetrigger") return Kind::Terminal;
            if (k == "sbombtargetobjectivetrigger") return Kind::Bomb;
            if (k == "sflag") return Kind::Flag;
            if (k == "sflagdropzone") return Kind::DropZone;
            if (k == "sobjectivetrigger") return Kind::Trigger;
            if (k == "sdisk") return Kind::Disk;
            if (k == "scamnetwork") return Kind::CamNet;
            if (k == "salarm") return Kind::Alarm;
            for (const auto& device : Workflow::Security::Devices())
                if (Workflow::Security::Detector(device.kind) && k == ClassKey(device.type)) return Kind::Detector;
            if (k == "salarmzoneinfo") return Kind::AlarmZone;
            if (EndsWith(k, "zoneinfo")) return Kind::Zone;
            if (k.rfind("esb", 0) == 0 || k.find("softbody") != std::string::npos) return Kind::SoftBody;
            if (k == Lower(ObjectivePlayers::ClassName)) return Kind::PlayerRules;
            for (const char* legacy : ObjectivePlayers::LegacyClassNames) if (k == Lower(legacy)) return Kind::PlayerRules;
            if (k == "slift") return Kind::Lift;
            if (k == "mover" || k == "sliftdoor") return Kind::Mover;
            if (k == "sfish" || k == "sfish_big") return Kind::Fish;
            if (k.find("zonetips") != std::string::npos || k.find("tutorial") != std::string::npos) return Kind::Tutorial;
            if (k == "sinfirmary") return Kind::Infirmary;
            if (k == "sammostock") return Kind::Ammo;
            if (k == "sinterrupteur") return Kind::Switch;
            if (k == "ssprinkler") return Kind::Sprinkler;
            return Kind::Other;
        }
        inline bool ObjectiveTrigger(Kind k) { return k == Kind::Terminal || k == Kind::Bomb || k == Kind::Flag || k == Kind::Trigger; }
        inline std::string DeviceKind(const std::string& cls)
        {
            const auto k = ClassKey(cls);
            for (const auto& device : Workflow::Security::Devices()) if (k == ClassKey(device.type)) return device.kind;
            return {};
        }
        inline std::string ModeKey(std::string text)
        {
            text = Lower(text);
            std::string key;
            for (unsigned char c : text) if (std::isalnum(c)) key += static_cast<char>(c);
            if (key == "story" || key == "gmmulti" || key == "multi") return "story";
            if (key.find("disk") != std::string::npos) return "diskhunt";
            if (key.find("death") != std::string::npos || key == "dm") return "deathmatch";
            if (key == "gmundefined" || key == "undefined" || key.empty()) return "undefined";
            return key;
        }
        inline const char* ModeName(const std::string& key)
        {
            return key == "story" ? "Story (GM_Multi)" : key == "diskhunt" ? "Disk Hunt (GM_DiskHunt)" : key == "deathmatch" ? "Deathmatch (GM_DeathMatch)" : "the sector";
        }
        inline Json Fix(const std::string& tool, Json args, const std::string& note = {})
        {
            Json fix{{"tool", tool}, {"args", std::move(args)}};
            if (!note.empty()) fix["note"] = note;
            return fix;
        }
        inline Workflow::Vector PointOf(const Json& v, bool& ok)
        {
            ok = false;
            Workflow::Vector out{};
            try { out = Ge::Point(v, "position"); ok = true; }
            catch (const std::exception&) {}
            return out;
        }

        // A piece as Map Design keeps it: {spec, position, rotation}.
        inline Json Piece(const Json& p)
        {
            if (!p.is_object()) return Json();
            try
            {
                if (p.contains("spec") && p.at("spec").is_object())
                {
                    Json spec = p.at("spec");
                    if (!spec.contains("kind") || !spec.contains("width") || !spec.contains("length") || !spec.contains("height")) return Json();
                    bool ok = false;
                    const auto position = PointOf(Get(p, "position"), ok);
                    if (!ok) return Json();
                    Workflow::Rotation rotation{};
                    if (p.contains("rotation") && p.at("rotation").is_array() && p.at("rotation").size() == 3)
                        for (size_t i = 0; i < 3; ++i) rotation[i] = static_cast<int>(std::lround(p.at("rotation")[i].get<double>()));
                    Json out{{"spec", spec}, {"position", position}, {"rotation", rotation}};
                    if (p.contains("generated") && p.at("generated").is_boolean()) out["generated"] = p.at("generated");
                    return out;
                }
                if (p.contains("kind") && p.contains("size") && p.at("size").is_array() && p.at("size").size() == 3)
                {
                    const auto& size = p.at("size");
                    Json spec{{"kind", p.at("kind")}, {"width", size[0].get<double>()}, {"length", size[1].get<double>()}, {"height", size[2].get<double>()}};
                    for (const char* key : {"name", "construction", "thickness", "steps", "ceiling"}) if (p.contains(key)) spec[key] = p.at(key);
                    if (!spec.contains("name") && p.contains("id")) spec["name"] = p.at("id");
                    if (!spec.contains("construction")) spec["construction"] = "Carve";
                    Workflow::Vector position{};
                    const auto at = Get(p, "at");
                    if (!at.is_array() || at.size() < 2) return Json();
                    position[0] = at[0].get<double>();
                    position[1] = at[1].get<double>();
                    position[2] = at.size() > 2 && at[2].is_number() ? at[2].get<double>() : 0;
                    const double yaw = p.contains("yaw") && p.at("yaw").is_number() ? p.at("yaw").get<double>() : 0;
                    const Workflow::Rotation rotation{0, static_cast<int>(std::lround(yaw * 65536 / 360)) % 65536, 0};
                    return Json{{"spec", spec}, {"position", position}, {"rotation", rotation}};
                }
            }
            catch (const std::exception&) {}
            return Json();
        }
        inline Json Pieces(const Json& list)
        {
            Json out = Json::array();
            if (!list.is_array()) return out;
            for (const auto& item : list)
            {
                if (item.is_array()) { for (const auto& inner : item) if (auto p = Piece(inner); !p.is_null()) out.push_back(p); }
                else if (auto p = Piece(item); !p.is_null()) out.push_back(p);
            }
            return out;
        }

        // Map Settings as the engine names them, or as the spec's settings section does.
        struct SettingsMode { std::string key, raw; std::optional<double> respawn, timeLimit, spiesLives, sentriesLives, scoreLimit, handicap, numDisksMax; };
        struct Settings
        {
            bool given = false, known = false; // given: some settings at all; known: their values
            std::optional<bool> exists;         // SMapSettings saved in <Map>-i (live)
            std::string mapName;
            std::vector<SettingsMode> modes;
            std::optional<size_t> strings;
            Json textures, images; // mapsettings.get's textures; the spec's interface section
        };
        inline Settings ReadSettings(const Json& given)
        {
            Settings s;
            if (!given.is_object()) return s;
            s.given = true;
            if (given.contains("settings") && given.at("settings").is_boolean()) s.exists = given.at("settings").get<bool>();
            if (given.contains("textures") && given.at("textures").is_object()) s.textures = given.at("textures");
            if (given.contains("interface") && given.at("interface").is_object()) s.images = given.at("interface");
            Json values = given.contains("values") ? given.at("values") : given;
            if (!values.is_object()) return s;
            if (const auto* name = Field(values, "MapName")) { s.mapName = Text(*name); s.known = true; }
            const Json* modes = Field(values, "Modes");
            if (modes && modes->is_array())
            {
                s.known = true;
                for (const auto& m : *modes)
                {
                    SettingsMode mode;
                    const Json* id = Field(m, "GameModeId");
                    if (!id) id = Field(m, "mode");
                    mode.raw = id ? Text(*id) : std::string();
                    mode.key = ModeKey(mode.raw);
                    auto number = [&](std::initializer_list<const char*> names) -> std::optional<double>
                    {
                        for (const char* n : names) if (const auto* v = Field(m, n)) return Number(v);
                        return std::nullopt;
                    };
                    mode.respawn = number({"RespawnTime", "respawn"});
                    mode.timeLimit = number({"TimeLimit", "timeLimit"});
                    mode.spiesLives = number({"SpiesLives", "spiesLives"});
                    mode.sentriesLives = number({"SentriesLives", "sentriesLives", "mercsLives"});
                    mode.scoreLimit = number({"ScoreLimit", "scoreLimit"});
                    mode.handicap = number({"Handicap", "handicap"});
                    mode.numDisksMax = number({"NumDisksMax", "numDisksMax"});
                    s.modes.push_back(mode);
                }
            }
            if (const auto* strings = Field(values, "ModesStrings"); strings && strings->is_array()) s.strings = strings->size();
            else if (modes && modes->is_array())
            {
                // The spec keeps each mode's strings inside it.
                size_t inside = 0;
                for (const auto& m : *modes) if (Field(m, "strings")) ++inside;
                if (inside) s.strings = inside;
            }
            return s;
        }
    }

    // ------------------------------------------------------------ scenes
    namespace Detail
    {
        inline Actor ActorFromJson(const Json& a)
        {
            Actor out;
            out.path = GetString(a, {"path", "id"});
            out.cls = GetString(a, {"class", "cls", "type"});
            out.name = GetString(a, {"name"});
            out.tag = GetString(a, {"tag"});
            out.event = GetString(a, {"event"});
            if (a.contains("values") && a.at("values").is_object()) out.values = a.at("values");
            else if (a.contains("properties") && a.at("properties").is_object()) out.values = a.at("properties");
            bool ok = false;
            for (const char* key : {"position", "location", "at"})
                if (!ok && a.contains(key)) out.position = PointOf(a.at(key), ok);
            if (!ok) if (const auto* location = Field(out.values, "Location")) out.position = PointOf(*location, ok);
            if (out.tag.empty()) if (const auto* tag = Field(out.values, "Tag")) out.tag = Text(*tag);
            if (out.event.empty()) if (const auto* event = Field(out.values, "Event")) out.event = Text(*event);
            if (out.name.empty()) out.name = Last(out.path);
            return out;
        }
        inline void ReadModes(const Json& modes, std::set<std::string>& into)
        {
            if (modes.is_string()) { into.insert(ModeKey(modes.get<std::string>())); return; }
            if (!modes.is_array()) return;
            for (const auto& m : modes)
                if (m.is_string()) into.insert(ModeKey(m.get<std::string>()));
                else if (m.is_object()) { const auto key = GetString(m, {"mode", "GameModeId"}); if (!key.empty()) into.insert(ModeKey(key)); }
        }
        inline Json GeList(const Json& ges)
        {
            if (ges.is_object() && ges.contains("elements")) return GeList(ges.at("elements"));
            return ges.is_array() ? ges : Json::array();
        }
        // Objective player counts from a spec ({objective, min, max} and
        // {mission, sizes}) as the SMatchSizeRules actor the editor places.
        inline void PlayerCounts(const Json& counts, const Json& targets, std::vector<Actor>& actors)
        {
            if ((!counts.is_array() || counts.empty()) && (!targets.is_array() || targets.empty())) return;
            Actor rules;
            rules.path = rules.name = ObjectivePlayers::ClassName;
            rules.cls = ObjectivePlayers::ClassName;
            Json subject = Json::array(), low = Json::array(), high = Json::array(), win = Json::array(), target = Json::array();
            auto ref = [](const Json& v) { return v.is_string() ? Json{{"$ref", v.get<std::string>()}} : v; };
            if (counts.is_array())
                for (const auto& c : counts)
                {
                    if (!c.is_object()) continue;
                    subject.push_back(ref(Get(c, c.contains("objective") ? "objective" : "subject")));
                    const auto* lowest = Field(c, "min");
                    const auto* highest = Field(c, "max");
                    low.push_back(std::to_string(static_cast<int>(Number(lowest).value_or(0))));
                    high.push_back(std::to_string(static_cast<int>(Number(highest).value_or(0))));
                }
            if (targets.is_array())
                for (const auto& t : targets)
                {
                    if (!t.is_object()) continue;
                    win.push_back(ref(Get(t, "mission")));
                    const auto sizes = Get(t, "sizes");
                    for (int i = 0; i < ObjectivePlayers::TeamSizes; ++i)
                    {
                        const auto at = static_cast<size_t>(i);
                        target.push_back(std::to_string(sizes.is_array() && at < sizes.size() ? static_cast<int>(Number(&sizes[at]).value_or(0)) : 0));
                    }
                }
            rules.values = {{"Subject", subject}, {"MinPlayers", low}, {"MaxPlayers", high}, {"WinMission", win}, {"WinTarget", target}};
            actors.push_back(std::move(rules));
        }
        inline void Common(Scene& s, const Json& j)
        {
            if (j.contains("pieces")) s.pieces = Pieces(j.at("pieces"));
            if (j.contains("ges")) s.ges = GeList(j.at("ges"));
            if (j.contains("level") && j.at("level").is_object())
                s.level = j.at("level").contains("values") && j.at("level").at("values").is_object() ? j.at("level").at("values") : j.at("level");
            for (const char* key : {"mapSettings", "settings"})
                if (s.mapSettings.is_null() && j.contains(key) && j.at(key).is_object()) s.mapSettings = j.at(key);
            if (j.contains("modes")) ReadModes(j.at("modes"), s.modes);
            s.mapName = GetString(j, {"mapName"});
            s.mapFile = GetString(j, {"mapFile"});
            if (j.contains("map") && j.at("map").is_object())
            {
                if (s.mapName.empty()) s.mapName = GetString(j.at("map"), {"name"});
                if (s.mapFile.empty()) s.mapFile = GetString(j.at("map"), {"file"});
            }
            if (j.contains("geometryBuilt") && j.at("geometryBuilt").is_boolean()) s.geometryBuilt = j.at("geometryBuilt").get<bool>();
            if (j.contains("release") && j.at("release").is_object()) s.release = j.at("release");
            if (j.contains("mapCheck")) s.mapCheck = j.at("mapCheck");
            if (j.contains("lighting")) s.lighting = j.at("lighting");
            if (j.contains("documents") && j.at("documents").is_array()) s.documents = j.at("documents");
            if (j.contains("schema")) s.schema = j.at("schema");
            PlayerCounts(Get(j, "playerCounts"), Get(j, "targets"), s.actors);
        }
    }
    // The Compiled.scene shape (see the top of this file).
    inline Scene SceneFromJson(const Json& scene)
    {
        if (!scene.is_object()) throw std::runtime_error("A rules scene is a JSON object with actors, pieces, ges, level, settings and modes.");
        Scene s;
        for (const auto& a : Detail::Get(scene, "actors")) if (a.is_object()) s.actors.push_back(Detail::ActorFromJson(a));
        Detail::Common(s, scene);
        if (scene.contains("live") && scene.at("live").is_boolean()) s.live = scene.at("live").get<bool>();
        return s;
    }
    // The live map: actors.query's result (one page, or every page's actors
    // in one list) and extra {level (settings.inspect), mapSettings
    // (mapsettings.get), ges (ge.list), pieces, spawns (design.spawns, whose
    // teams fill TeamNumber where reflection has none), modes, mapName,
    // mapFile, geometryBuilt, release, mapCheck, lighting, schema}.
    inline Scene SceneFromQuery(const Json& actorsQueryResult, const Json& extra)
    {
        Scene s;
        s.live = true;
        const Json list = actorsQueryResult.is_array() ? actorsQueryResult : Detail::Get(actorsQueryResult, "actors");
        for (const auto& a : list) if (a.is_object()) s.actors.push_back(Detail::ActorFromJson(a));
        if (extra.is_object())
        {
            Detail::Common(s, extra);
            for (const auto& spawn : Detail::GeList(Detail::Get(extra, "spawns")))
            {
                if (!spawn.is_object() || !spawn.contains("team")) continue;
                const auto path = Detail::Lower(Detail::GetString(spawn, {"path"}));
                for (auto& a : s.actors)
                    if (Detail::Lower(a.path) == path && !Detail::Field(a, "TeamNumber"))
                        a.values["TeamNumber"] = spawn.at("team").is_string() ? spawn.at("team").get<std::string>() : spawn.at("team").dump();
            }
        }
        return s;
    }

    // ------------------------------------------------------------ checks
    namespace Detail
    {
        inline double Distance(const Workflow::Vector& a, const Workflow::Vector& b) { return Ge::Distance(a, b); }

        // The floor under a point from the Map Design pieces: the highest room
        // or corridor floor, or platform top, below it.
        inline std::optional<double> FloorBelow(const Json& pieces, const Workflow::Vector& at)
        {
            std::optional<double> best;
            for (const auto& piece : pieces)
            {
                try
                {
                    const auto& spec = piece.at("spec");
                    const auto kind = spec.at("kind").get<std::string>();
                    const bool space = kind == "Room" || kind == "Corridor" || kind == "Vent" || kind == "Crawlway";
                    if ((!space && kind != "Platform") || (space && Workflow::Design::SpansFloors(spec.value("name", std::string())))) continue;
                    const Workflow::Pose pose{piece.at("position").get<Workflow::Vector>(), piece.at("rotation").get<Workflow::Rotation>()};
                    const auto extent = Workflow::Design::PieceExtent(spec, pose);
                    if (at[0] < extent.lo[0] - 1 || at[0] > extent.hi[0] + 1 || at[1] < extent.lo[1] - 1 || at[1] > extent.hi[1] + 1) continue;
                    double surface = 0;
                    if (space)
                    {
                        surface = pose.position[2];
                        if (at[2] < surface - 1 || at[2] > surface + spec.at("height").get<double>() + 1) continue;
                    }
                    else
                    {
                        surface = extent.hi[2];
                        if (surface > at[2] + 1) continue;
                    }
                    if (!best || surface > *best) best = surface;
                }
                catch (const std::exception&) { /* A piece that cannot be measured holds no floor. */ }
            }
            return best;
        }
        struct Placed { int index = -1; std::string kind, name; Workflow::Design::Extent extent; double thickness = 16; };
        inline std::vector<Placed> Place(const Json& pieces)
        {
            std::vector<Placed> out;
            for (size_t i = 0; i < pieces.size(); ++i)
                try
                {
                    const auto& spec = pieces[i].at("spec");
                    const Workflow::Pose pose{pieces[i].at("position").get<Workflow::Vector>(), pieces[i].at("rotation").get<Workflow::Rotation>()};
                    out.push_back({static_cast<int>(i), spec.at("kind").get<std::string>(), spec.value("name", std::string("Piece " + std::to_string(i + 1))),
                                   Workflow::Design::PieceExtent(spec, pose), spec.value("thickness", 16.0)});
                }
                catch (const std::exception&) {}
            return out;
        }
        inline bool Space(const std::string& kind) { return kind == "Room" || kind == "Corridor" || kind == "Vent" || kind == "Crawlway" || kind == "Doorway"; }
        inline bool Connector(const std::string& kind) { return Workflow::Design::StairKind(kind) || kind == "Ramp" || kind == "Platform"; }
        inline bool Inside(const Workflow::Design::Extent& e, const Workflow::Vector& p, double margin)
        {
            for (int k = 0; k < 3; ++k) if (p[k] < e.lo[k] - margin || p[k] > e.hi[k] + margin) return false;
            return true;
        }
        // Whether a point is in solid geometry the blockout makes: near a
        // carved space but outside it, or inside a platform or stair.
        // Unknown (false) away from every piece.
        inline bool InSolid(const std::vector<Placed>& placed, const std::vector<Workflow::Design::WorldSolid>& carved, const Workflow::Vector& p)
        {
            bool walled = false;
            for (const auto& piece : placed)
            {
                if (Space(piece.kind) && Inside(piece.extent, p, piece.thickness + 48)) walled = true;
                if (Connector(piece.kind) && Inside(piece.extent, p, -1)) return true;
            }
            return walled && !Workflow::Design::InsideCarved(carved, p);
        }

        struct Index
        {
            const Scene& scene;
            std::map<std::string, std::vector<size_t>> byPath, byLast, byName, byTag;
            explicit Index(const Scene& s) : scene(s)
            {
                for (size_t i = 0; i < s.actors.size(); ++i)
                {
                    const auto& a = s.actors[i];
                    byPath[Lower(a.path)].push_back(i);
                    byLast[Lower(Last(a.path))].push_back(i);
                    if (!a.name.empty()) byName[Lower(a.name)].push_back(i);
                    if (!IsNone(a.tag)) byTag[Lower(a.tag)].push_back(i);
                }
            }
            const Actor* First(const std::map<std::string, std::vector<size_t>>& map, const std::string& key) const
            {
                const auto found = map.find(key);
                return found == map.end() || found->second.empty() ? nullptr : &scene.actors[found->second.front()];
            }
            const Actor* Resolve(const Ref& r) const
            {
                if (!r.id.empty())
                {
                    const auto key = Lower(r.id);
                    if (const auto* hit = First(byPath, key)) return hit;
                    if (const auto* hit = First(byLast, key)) return hit;
                    return First(byTag, key);
                }
                if (const auto* hit = First(byPath, Lower(r.path))) return hit;
                if (const auto* hit = First(byLast, Lower(Last(r.path)))) return hit;
                return First(byName, Lower(Last(r.path)));
            }
            // Whether a reference that resolves to nothing is wrong: always
            // in the live map; offline only for the spec's own ids, since a
            // Class'MyLevel.Name' may be an actor already in the map.
            bool Definite(const Ref& r) const { return scene.live || !r.id.empty(); }
            bool Tagged(const std::string& tag) const { return !IsNone(tag) && byTag.count(Lower(tag)); }
        };

        struct Checker
        {
            const Scene& scene;
            const std::string profile;
            const Index index;
            std::vector<Finding> out;
            std::vector<const Actor*> starts, spies, mercs, missions, objectives, triggers, terminals, bombs, flags, dropZones, disks, cams, alarms, detectors, zones,
                alarmZones, softBodies, playerRules, lifts;
            std::set<std::string> requested;
            Json pieces;
            std::vector<Placed> placed;
            std::vector<Workflow::Design::WorldSolid> carved;
            Settings settings;

            Checker(const Scene& s, const std::string& p) : scene(s), profile(p), index(s)
            {
                for (const auto& a : s.actors)
                {
                    switch (KindOf(a.cls))
                    {
                    case Kind::Start: starts.push_back(&a); (TeamOf(a) == "1" ? mercs : spies).push_back(&a); break;
                    case Kind::Mission: missions.push_back(&a); break;
                    case Kind::Objective: objectives.push_back(&a); break;
                    case Kind::Terminal: triggers.push_back(&a); terminals.push_back(&a); break;
                    case Kind::Bomb: triggers.push_back(&a); bombs.push_back(&a); break;
                    case Kind::Flag: triggers.push_back(&a); flags.push_back(&a); break;
                    case Kind::Trigger: triggers.push_back(&a); break;
                    case Kind::DropZone: dropZones.push_back(&a); break;
                    case Kind::Disk: disks.push_back(&a); break;
                    case Kind::CamNet: cams.push_back(&a); break;
                    case Kind::Alarm: alarms.push_back(&a); break;
                    case Kind::Detector: detectors.push_back(&a); break;
                    case Kind::Zone: zones.push_back(&a); break;
                    case Kind::AlarmZone: alarmZones.push_back(&a); break;
                    case Kind::SoftBody: softBodies.push_back(&a); break;
                    case Kind::PlayerRules: playerRules.push_back(&a); break;
                    case Kind::Lift: lifts.push_back(&a); break;
                    default: break;
                    }
                }
                requested = s.modes;
                requested.erase("undefined");
                if (requested.empty()) requested.insert("story");
                pieces = Pieces(s.pieces);
                placed = Place(pieces);
                try { carved = Workflow::Design::CarvedSolids(pieces); }
                catch (const std::exception&) { carved.clear(); }
                settings = ReadSettings(s.mapSettings);
            }
            static std::string TeamOf(const Actor& a)
            {
                const auto* team = Field(a, "TeamNumber");
                if (!team) return "0"; // The engine's default: spies.
                const auto t = Text(*team);
                return t.empty() ? "0" : t;
            }
            static std::string Name(const Actor& a) { return a.name.empty() ? Last(a.path) : a.name; }
            static std::vector<std::string> Paths(const std::vector<const Actor*>& list, size_t most = 50)
            {
                std::vector<std::string> paths;
                for (const auto* a : list) { if (paths.size() >= most) break; paths.push_back(a->path); }
                return paths;
            }
            std::string Mode(const Actor& mission) const
            {
                const auto* mode = Field(mission, "GameMode");
                return ModeKey(mode ? Text(*mode) : std::string("GM_Undefined"));
            }
            std::vector<const Actor*> MissionsOf(const std::string& key) const
            {
                std::vector<const Actor*> found;
                for (const auto* m : missions) if (Mode(*m) == key) found.push_back(m);
                return found;
            }
            void Add(const std::string& rule, std::string message, std::vector<std::string> actors = {}, Json fix = Json(), const std::string& reported = {})
            {
                const auto* info = FindRule(rule);
                Finding f;
                f.rule = rule;
                f.wikiPage = info ? info->wikiPage : "home";
                f.severity = info ? (profile == "release" ? info->release : info->draft) : "warning";
                if (f.severity == "reported") f.severity = reported.empty() ? "warning" : reported;
                f.message = std::move(message);
                f.actors = std::move(actors);
                f.fix = std::move(fix);
                out.push_back(std::move(f));
            }
            // A property fix: actors_edit in the live map, the spec offline.
            Json SetFix(const Actor& a, const Json& properties) const
            {
                if (scene.live)
                    return Fix("actors_edit", {{"edits", Json::array({{{"action", "set"}, {"actor", a.path}, {"properties", properties}}})}});
                return Fix("level_build", {{"sync", "merge"}}, "In the level spec, give " + a.path + " these properties: " + properties.dump() + ".");
            }
            std::optional<double> FloorOf(const Actor& a) const
            {
                if (const auto floor = NumberOf(a, "$floor")) return floor;
                if (!pieces.empty()) return FloorBelow(pieces, a.position);
                return std::nullopt;
            }
            // Live: what map_validate traced. Offline: the blockout's solids.
            bool Blocked(const Actor& a, const std::vector<Workflow::Vector>& around) const
            {
                if (const auto* blocked = Field(a, "$blocked")) return Truth(blocked);
                if (pieces.empty() || carved.empty()) return false;
                for (const auto& offset : around)
                {
                    const Workflow::Vector p{a.position[0] + offset[0], a.position[1] + offset[1], a.position[2] + offset[2]};
                    if (InSolid(placed, carved, p)) return true;
                }
                return false;
            }

            // Whether anything but the actor itself (and, unless asked, any
            // detector) sends this Tag as an event.
            bool Sent(const std::string& tag, const Actor& except, bool detectorsToo = false) const
            {
                if (IsNone(tag)) return false;
                const auto key = Lower(tag);
                for (const auto& a : scene.actors)
                {
                    if (&a == &except || (!detectorsToo && KindOf(a.cls) == Kind::Detector)) continue;
                    if (Lower(a.event) == key) return true;
                    std::vector<std::string> events;
                    SentEvents(a.values, events);
                    for (const auto& e : events) if (Lower(e) == key) return true;
                }
                return false;
            }

            // ----------------------------------------------------- spawns
            void Spawns()
            {
                if (spies.empty()) Add("spawns.team-missing", "No spy start: add PlayerStart actors with TeamNumber 0.", {}, Fix("gameplay", {{"spawns", {{"spy", {{"count", 4}}}}}}));
                if (mercs.empty()) Add("spawns.team-missing", "No merc start: add PlayerStart actors with TeamNumber 1.", {}, Fix("gameplay", {{"spawns", {{"merc", {{"count", 4}}}}}}));
                for (const auto& [team, list] : {std::pair<const char*, const std::vector<const Actor*>*>{"spy", &spies}, {"merc", &mercs}})
                    if (!list->empty() && list->size() < 3)
                        Add("spawns.too-few", "Only " + std::to_string(list->size()) + " " + team + " start" + (list->size() == 1 ? "" : "s") +
                            ": place at least 3 per team so 2v2 and 3v1 matches find a start for every player (4 is the RE+ default).",
                            Paths(*list), Fix("gameplay", {{"spawns", {{team, {{"count", 4}}}}}}));
                for (const auto* start : starts)
                {
                    const std::vector<Workflow::Vector> around{{0, 0, 0}, {0, 0, 40}};
                    if (!Field(*start, "$blocked") && (pieces.empty() || carved.empty())) continue;
                    if (Blocked(*start, around))
                        Add("spawns.in-solid", Name(*start) + " is inside solid geometry, so a player spawning there is stuck.", {start->path},
                            Fix("actors_edit", {{"edits", Json::array({{{"action", "drop"}, {"actor", start->path}}})}}, "Move it into the room it belongs to."));
                }
                std::vector<const Actor*> targets = triggers;
                for (const auto* s : spies)
                    for (const auto* t : targets)
                        if (const double d = Distance(s->position, t->position); d < 512)
                            Add("spawns.spy-near-objective", "Spy start " + Name(*s) + " is within " + Round(d) + " units of " + Name(*t) + ": the objective is reachable before the mercs can respond.",
                                {s->path, t->path}, Fix("gameplay", {{"spawns", {{"spy", {{"in", "a room at least 512 units from every objective"}}}}}}));
                for (const auto* s : spies)
                    for (const auto* m : mercs)
                        if (const double d = Distance(s->position, m->position); d < 768)
                            Add("spawns.teams-close", "Spy start " + Name(*s) + " and merc start " + Name(*m) + " are only " + Round(d) + " units apart: the teams meet immediately.",
                                {s->path, m->path}, Fix("gameplay", {{"spawns", {{"merc", {{"in", "a room at least 768 units from the spy starts"}}}}}}));
                if (!mercs.empty())
                    for (const auto* t : targets)
                    {
                        double nearest = 1e18;
                        for (const auto* m : mercs) nearest = std::min(nearest, Distance(m->position, t->position));
                        if (nearest > 4096)
                            Add("spawns.objective-far", Name(*t) + " is " + Round(nearest) + " units from the nearest merc start: hard to defend.", {t->path},
                                Fix("gameplay", {{"spawns", {{"merc", {{"in", "a room within 4096 units of " + Name(*t)}}}}}}));
                    }
                if (!targets.empty() && (!spies.empty() || !mercs.empty()))
                {
                    const double speed = 300; // Design::DefaultMovement spySpeed and mercSpeed.
                    for (const auto* t : targets)
                    {
                        double spy = 1e18, merc = 1e18;
                        for (const auto* s : spies) spy = std::min(spy, Distance(s->position, t->position));
                        for (const auto* m : mercs) merc = std::min(merc, Distance(m->position, t->position));
                        std::string text = Name(*t) + ": ";
                        if (spy < 1e18) text += "spies " + Round(spy) + " units (" + Round(spy / speed, 1) + " s)";
                        if (spy < 1e18 && merc < 1e18) text += ", ";
                        if (merc < 1e18) text += "mercs " + Round(merc) + " units (" + Round(merc / speed, 1) + " s)";
                        Add("info.travel", text + " from the nearest start, as the crow flies.", {t->path});
                    }
                }
            }

            // ------------------------------------------------------ story
            void Unresolved(const Actor& owner, const std::string& property, const Ref& r)
            {
                if (!index.Definite(r)) return;
                Add("story.unresolved-reference", Name(owner) + "'s " + property + " names " + Shown(r) + ", which is not in the map: the game ignores it. Use the exact object name (Ctrl+B copies it).",
                    {owner.path}, Fix("map_query", {{"what", "missions"}}, "Read the missions and objectives, then set " + property + " to actors that exist."));
            }
            std::vector<const Actor*> ResolveList(const Actor& owner, const std::string& property)
            {
                std::vector<const Actor*> found;
                for (const auto& r : Refs(Field(owner, property)))
                {
                    if (const auto* target = index.Resolve(r)) found.push_back(target);
                    else Unresolved(owner, property, r);
                }
                return found;
            }
            void Story()
            {
                if (missions.empty())
                    Add("story.no-mission", "No SMission: the map has no game mode to play.", {},
                        Fix("gameplay", {{"modes", Json::array({"story", "diskhunt", "deathmatch"})}, {"story", {{"mission", {{"description", "..."}}}}}}));
                const auto multi = MissionsOf("story");
                if (requested.count("story") && multi.empty() && !missions.empty())
                    Add("story.no-multi", "Story is asked for, but no SMission has GameMode GM_Multi.", Paths(missions),
                        Fix("gameplay", {{"story", {{"mission", {{"gameMode", "GM_Multi"}}}}}}, "Set GameMode GM_Multi on the Story mission."));
                if (requested.count("story") && objectives.empty())
                    Add("story.no-objective", "No SObjective: there is nothing for the spies to do in Story.", {},
                        Fix("gameplay", {{"story", {{"objectives", Json::array({{{"id", "Objective1"}, {"triggers", Json::array({{{"kind", "terminal"}}})}}})}}}}));
                std::set<const Actor*> held;
                std::map<const Actor*, std::vector<const Actor*>> children;
                for (const auto* m : missions)
                {
                    const auto mode = Mode(*m);
                    const auto listed = ResolveList(*m, "Objectives");
                    children[m] = listed;
                    for (const auto* child : listed)
                    {
                        held.insert(child);
                        const auto kind = KindOf(child->cls);
                        if (kind != Kind::Objective && kind != Kind::Mission)
                            Add("story.trigger-kind", Name(*m) + "'s Objectives lists " + Name(*child) + " (" + Last(child->cls) + "): Objectives hold SObjectives, or sector SMissions.", {m->path, child->path});
                    }
                    if ((mode == "story" || mode == "undefined") && listed.empty() && Refs(Field(*m, "Objectives")).empty())
                        Add("story.mission-empty", Name(*m) + " (" + ModeName(mode) + ") lists no objectives: nothing ends the round.", {m->path},
                            Fix("gameplay", {{"story", {{"objectives", "list this mission's SObjectives"}}}}));
                    for (const auto* t : ResolveList(*m, "Triggers"))
                    {
                        const auto kind = KindOf(t->cls);
                        if (!ObjectiveTrigger(kind) && kind != Kind::Disk)
                            Add("story.trigger-kind", Name(*m) + "'s Triggers lists " + Name(*t) + " (" + Last(t->cls) + "), which is not an objective trigger or SDisk.", {m->path, t->path});
                    }
                }
                for (const auto* m : missions)
                {
                    std::vector<const Actor*> sectors, direct;
                    for (const auto* child : children[m]) (KindOf(child->cls) == Kind::Mission ? sectors : direct).push_back(child);
                    const auto minimum = NumberOf(*m, "MinimumObjectives").value_or(0);
                    double reachable = static_cast<double>(direct.size());
                    for (const auto* sector : sectors)
                    {
                        double members = 0;
                        for (const auto* c : children[sector]) members += KindOf(c->cls) == Kind::Objective ? 1 : 0;
                        const double threshold = NumberOf(*sector, "MinimumObjectives").value_or(0);
                        reachable += threshold > 0 && threshold < members ? threshold : members;
                    }
                    if (minimum > reachable && (!direct.empty() || !sectors.empty()))
                        Add("story.minimum-objectives", Name(*m) + " needs " + Round(minimum) + " objectives to win but its objectives can give only " + Round(reachable) + ", so the spies can never win.",
                            {m->path}, SetFix(*m, {{"MinimumObjectives", Round(reachable)}}));
                    if (sectors.empty()) continue;
                    if (Mode(*m) == "story")
                    {
                        if (!Truth(Field(*m, "bChained")))
                            Add("story.sector-chain", Name(*m) + " holds " + std::to_string(sectors.size()) + " sector missions but bChained is False, so every sector is live from the start.",
                                {m->path}, SetFix(*m, {{"bChained", "True"}}));
                        for (const auto* sector : sectors)
                        {
                            if (Mode(*sector) != "undefined")
                                Add("story.sector-chain", "Sector mission " + Name(*sector) + " has GameMode " + TextOf(*sector, "GameMode") + "; a sector inside the Story mission is GM_Undefined.",
                                    {sector->path}, SetFix(*sector, {{"GameMode", "GM_Undefined"}}));
                            if (Truth(Field(*sector, "bChained")))
                                Add("story.sector-chain", "Sector mission " + Name(*sector) + " is chained; keep bChained False on sectors so their objectives are available together.",
                                    {sector->path}, SetFix(*sector, {{"bChained", "False"}}));
                        }
                        for (const auto* d : direct)
                            Add("story.trigger-kind", Name(*d) + " hangs straight off " + Name(*m) + " beside its sectors, so it becomes a step of its own in the chain. Put it in a sector.", {m->path, d->path});
                    }
                    else if (Mode(*m) != "undefined")
                        Add("story.sector-chain", Name(*m) + " (" + TextOf(*m, "GameMode") + ") lists other missions; only the Story mission (GM_Multi) holds sectors.", {m->path});
                }
                for (const auto* m : missions)
                    if (Mode(*m) == "story" && held.count(m))
                        Add("story.sector-chain", Name(*m) + " (GM_Multi) is listed inside another mission; the Story mission is the top of the chain.", {m->path});
                for (const auto* o : objectives)
                {
                    if (!held.count(o))
                        Add("story.objective-orphan", Name(*o) + " is in no SMission's Objectives list, so it never counts.", {o->path},
                            multi.empty() ? Json() : Fix("map_query", {{"what", "missions"}}, "Add " + Name(*o) + " to " + Name(*multi.front()) + "'s Objectives (or to its sector's)."));
                    const auto listed = ResolveList(*o, "Triggers");
                    if (listed.empty() && Refs(Field(*o, "Triggers")).empty())
                        Add("story.objective-no-trigger", Name(*o) + " has no Triggers (terminal, flag or bomb target): the spies cannot complete it.", {o->path},
                            Fix("gameplay", {{"story", {{"objectives", Json::array({{{"id", Name(*o)}, {"triggers", Json::array({{{"kind", "terminal"}}})}}})}}}}));
                    for (const auto* t : listed)
                        if (!ObjectiveTrigger(KindOf(t->cls)))
                            Add("story.trigger-kind", Name(*o) + "'s Triggers lists " + Name(*t) + " (" + Last(t->cls) + "), which is not an SComputerObjectiveTrigger, SFlag or SBombTargetObjectiveTrigger.", {o->path, t->path});
                    for (const auto& r : Refs(Field(*o, "TriggersERVisibilityZone")))
                    {
                        const auto* zone = index.Resolve(r);
                        if (!zone && !index.Definite(r)) continue;
                        if (!zone || (KindOf(zone->cls) != Kind::Zone && KindOf(zone->cls) != Kind::AlarmZone))
                            Add("story.visibility-zone", Name(*o) + "'s TriggersERVisibilityZone names " + (zone ? Name(*zone) + " (" + Last(zone->cls) + ")" : Shown(r) + ", which is not in the map") + "; it must be a ZoneInfo.",
                                {o->path}, SetFix(*o, {{"TriggersERVisibilityZone", "None"}}));
                    }
                }
            }
            void Modes()
            {
                if (missions.empty()) return; // story.no-mission says it.
                for (const char* mode : {"story", "diskhunt", "deathmatch"})
                    if (MissionsOf(mode).empty())
                    {
                        const std::string key = mode;
                        Add("modes.missing", std::string("No ") + ModeName(key) + " SMission" + (requested.count(key) ? " although the map asks for it" : "") +
                                ": a player who picks that mode on this map gets no mission. Maps should support Story, Disk Hunt and Deathmatch.",
                            {}, Fix("gameplay", {{"modes", Json::array({key})}}));
                    }
            }

            // ------------------------------------------------- objectives
            void Objectives()
            {
                std::set<const Actor*> used;
                for (const auto* f : flags)
                {
                    size_t valid = 0;
                    std::vector<std::string> wrong;
                    for (const auto& r : Refs(Field(*f, "DropZone")))
                    {
                        const auto* zone = index.Resolve(r);
                        if (zone && KindOf(zone->cls) == Kind::DropZone) { ++valid; used.insert(zone); }
                        else if (zone) wrong.push_back(Name(*zone) + " (" + Last(zone->cls) + ")");
                        else if (index.Definite(r)) wrong.push_back(Shown(r) + " (not in the map)");
                        else ++valid;
                    }
                    if (!wrong.empty())
                        Add("objectives.flag-dropzones", Name(*f) + "'s DropZone lists " + Join(wrong) + "; it holds SFlagDropZone actors only.", {f->path},
                            Fix("gameplay", {{"story", {{"triggers", Json::array({{{"id", Name(*f)}, {"kind", "flag"}, {"dropZones", Json::array({{{"at", "..."}}})}}})}}}}));
                    if (valid < 1 || valid > 5)
                        Add("objectives.flag-dropzones", Name(*f) + " has " + std::to_string(valid) + " drop zones; an SFlag needs 1 to 5 SFlagDropZone actors in DropZone, or the spies have nowhere to bring the disk.", {f->path},
                            Fix("gameplay", {{"story", {{"triggers", Json::array({{{"id", Name(*f)}, {"kind", "flag"}, {"dropZones", Json::array({{{"at", "..."}}})}}})}}}}));
                }
                for (const auto* z : dropZones)
                    if (!used.count(z))
                        Add("objectives.dropzone-unused", Name(*z) + " is an SFlagDropZone no SFlag lists in DropZone.", {z->path});
                for (const auto* t : terminals)
                {
                    if (const auto floor = FloorOf(*t))
                    {
                        const double h = t->position[2] - *floor;
                        if (std::abs(h - 78) > 8)
                        {
                            Add("objectives.terminal-placement", Name(*t) + " is " + Round(h) + " units above the floor; a terminal sits about 78 units up so players can capture it.", {t->path},
                                scene.live ? Fix("actors_edit", {{"edits", Json::array({{{"action", "move"}, {"actor", t->path}, {"delta", Json::array({0, 0, *floor + 78 - t->position[2]})}}})}})
                                           : Fix("level_build", {{"sync", "merge"}}, "Place " + t->path + " at floor+78."));
                        }
                    }
                    if (Blocked(*t, {{0, 0, 0}, {8, 0, 0}, {-8, 0, 0}, {0, 8, 0}, {0, -8, 0}}))
                        Add("objectives.terminal-placement", Name(*t) + " intersects collision geometry, so players cannot capture it. Move it clear of walls and props.", {t->path});
                    const auto capture = NumberOf(*t, "CaptureTime"), domination = NumberOf(*t, "DominationCaptureTime");
                    if (capture && *capture > 0 && (!scene.live || !domination || *domination <= 0))
                        Add("objectives.terminal-capture-time", Name(*t) + " sets CaptureTime, which terminals ignore; set DominationCaptureTime (the time the spies need to capture it).", {t->path},
                            SetFix(*t, {{"DominationCaptureTime", Round(*capture, 1)}}));
                }
            }
            void DiskHunt()
            {
                const auto hunts = MissionsOf("diskhunt");
                std::set<const Actor*> listed;
                for (const auto* m : hunts)
                {
                    size_t count = 0;
                    for (const auto& r : Refs(Field(*m, "Triggers")))
                        if (const auto* d = index.Resolve(r); d && KindOf(d->cls) == Kind::Disk) { listed.insert(d); ++count; }
                    if (!count)
                        Add("diskhunt.disk-count", Name(*m) + " (Disk Hunt) lists no SDisk in Triggers, so there is nothing to collect.", {m->path},
                            Fix("gameplay", {{"diskhunt", {{"disks", {{"count", 12}}}}}}));
                }
                for (const auto* d : disks)
                {
                    if (const auto group = NumberOf(*d, "Group"); group && (*group < 1 || *group > 3))
                        Add("diskhunt.disk-group", Name(*d) + " has Group " + Round(*group) + "; the engine draws disks from Groups 1, 2 and 3 in turn.", {d->path}, SetFix(*d, {{"Group", "1"}}));
                    if (!hunts.empty() && !listed.count(d))
                        Add("diskhunt.disk-unlisted", Name(*d) + " is not in the Disk Hunt SMission's Triggers, so it never spawns.", {d->path});
                    bool close = false;
                    if (const auto floor = FloorOf(*d); floor && d->position[2] - *floor < 8) close = true;
                    if (close || Blocked(*d, {{0, 0, 0}, {16, 0, 0}, {-16, 0, 0}, {0, 16, 0}, {0, -16, 0}, {0, 0, -6}}))
                        Add("diskhunt.disk-clearance", Name(*d) + " touches the floor or other geometry; players cannot pick up a disk that intersects collision. Give it free space all round.", {d->path});
                }
                if (hunts.empty() && !requested.count("diskhunt")) return;
                const size_t count = hunts.empty() ? disks.size() : listed.size();
                if (disks.empty())
                {
                    Add("diskhunt.disk-count", "Disk Hunt needs SDisk actors and the map has none.", {}, Fix("gameplay", {{"diskhunt", {{"disks", {{"count", 12}}}}}}));
                    return;
                }
                for (const auto& mode : settings.modes)
                {
                    if (mode.key != "diskhunt") continue;
                    const auto n = static_cast<double>(count);
                    if (mode.scoreLimit && *mode.scoreLimit > n)
                        Add("diskhunt.disk-count", "Map Settings asks for " + Round(*mode.scoreLimit) + " disks to win (ScoreLimit) but only " + std::to_string(count) + " are placed.", Paths(disks),
                            Fix("gameplay", {{"diskhunt", {{"disks", {{"count", static_cast<int>(*mode.scoreLimit) + 4}}}}}}));
                    if (mode.handicap && *mode.handicap > n)
                        Add("diskhunt.disk-count", "Map Settings activates " + Round(*mode.handicap) + " disks (Handicap) but only " + std::to_string(count) + " are placed.", Paths(disks),
                            Fix("map_settings", {{"modes", Json::array({{{"mode", "diskhunt"}, {"handicap", count}}})}}));
                    if (mode.handicap && mode.scoreLimit && *mode.handicap < *mode.scoreLimit)
                        Add("diskhunt.disk-count", "Map Settings activates " + Round(*mode.handicap) + " disks (Handicap) but asks for " + Round(*mode.scoreLimit) + " to win (ScoreLimit).", {},
                            Fix("map_settings", {{"modes", Json::array({{{"mode", "diskhunt"}, {"handicap", static_cast<int>(*mode.scoreLimit)}}})}}));
                    if (mode.numDisksMax && *mode.numDisksMax > n)
                        Add("diskhunt.disk-count", "Map Settings NumDisksMax is " + Round(*mode.numDisksMax) + " but only " + std::to_string(count) + " disks are placed; set it to the number of SDisk actors.", {},
                            Fix("map_settings", {{"modes", Json::array({{{"mode", "diskhunt"}, {"numDisksMax", count}}})}}));
                }
            }
            void Deathmatch()
            {
                const bool security = !alarms.empty() || !detectors.empty();
                for (const auto* m : missions)
                {
                    const auto reset = TextOf(*m, "AtResetEvent");
                    if (!IsNone(reset) && !index.Tagged(reset))
                        Add("events.dangling", Name(*m) + "'s AtResetEvent is " + reset + " but nothing in the map has that Tag.", {m->path});
                    if (Mode(*m) == "deathmatch" && security && IsNone(reset))
                        Add("deathmatch.security", Name(*m) + " (Deathmatch) has no AtResetEvent, so the map's alarms and detectors stay live; point AtResetEvent at an SMagicEvent that untriggers them.",
                            {m->path}, Fix("gameplay", {{"deathmatch", {{"disableSecurity", true}}}}));
                }
            }
            void PlayerCounts()
            {
                if (playerRules.empty()) return;
                ObjectivePlayers::Missions table;
                std::map<std::string, std::string> names;
                for (const auto* m : missions)
                {
                    ObjectivePlayers::Mission mission;
                    for (const auto& r : Refs(Field(*m, "Objectives")))
                        if (const auto* child = index.Resolve(r)) mission.objectives.push_back(child->path);
                    mission.minimum = static_cast<int>(NumberOf(*m, "MinimumObjectives").value_or(0));
                    table[ObjectivePlayers::Fold(m->path)] = mission;
                    names[ObjectivePlayers::Fold(m->path)] = Name(*m);
                }
                ObjectivePlayers::Rules rules;
                ObjectivePlayers::Targets targets;
                for (const auto* holder : playerRules)
                {
                    const auto* subjects = Field(*holder, "Subject");
                    const auto* low = Field(*holder, "MinPlayers");
                    const auto* high = Field(*holder, "MaxPlayers");
                    if (subjects && subjects->is_array())
                        for (size_t i = 0; i < subjects->size(); ++i)
                        {
                            std::vector<Ref> refs;
                            CollectRefs((*subjects)[i], refs, true);
                            if (refs.empty()) continue;
                            const auto* target = index.Resolve(refs.front());
                            if (!target) continue;
                            ObjectivePlayers::Rule rule;
                            rule.minimum = low && low->is_array() && i < low->size() ? static_cast<int>(Number(&(*low)[i]).value_or(0)) : 0;
                            rule.maximum = high && high->is_array() && i < high->size() ? static_cast<int>(Number(&(*high)[i]).value_or(0)) : 0;
                            if (!ObjectivePlayers::Empty(rule)) rules[ObjectivePlayers::Fold(target->path)] = rule;
                        }
                    const auto* win = Field(*holder, "WinMission");
                    const auto* wins = Field(*holder, "WinTarget");
                    if (win && win->is_array())
                        for (size_t i = 0; i < win->size(); ++i)
                        {
                            std::vector<Ref> refs;
                            CollectRefs((*win)[i], refs, true);
                            if (refs.empty()) continue;
                            const auto* mission = index.Resolve(refs.front());
                            if (!mission) continue;
                            std::array<int, ObjectivePlayers::TeamSizes> sizes{};
                            for (int k = 0; k < ObjectivePlayers::TeamSizes; ++k)
                            {
                                const size_t at = i * static_cast<size_t>(ObjectivePlayers::TeamSizes) + static_cast<size_t>(k);
                                sizes[static_cast<size_t>(k)] = wins && wins->is_array() && at < wins->size() ? static_cast<int>(Number(&(*wins)[at]).value_or(0)) : 0;
                            }
                            if (!ObjectivePlayers::EmptyTargets(sizes)) targets[ObjectivePlayers::Fold(mission->path)] = sizes;
                        }
                }
                try
                {
                    for (const auto& text : ObjectivePlayers::Problems(table, rules, names))
                        Add("objectiveplayers.problems", text, Paths(playerRules), Fix("gameplay", {{"playerCounts", "widen the player ranges so every lobby size keeps an objective"}}));
                    for (const auto& text : ObjectivePlayers::TargetWarnings(table, rules, targets, names))
                        Add("objectiveplayers.targets", text, Paths(playerRules));
                }
                catch (const std::exception& e)
                {
                    Add("objectiveplayers.problems", std::string("The objective player counts cannot be evaluated: ") + e.what(), Paths(playerRules));
                }
            }

            // ----------------------------------------------------- CamNet
            void CamNet()
            {
                if (cams.empty())
                {
                    Add("camnet.missing", "No SCamNetwork: mercs who use the Camera Network gadget are softlocked. Place at least one (three in a loop is usual).", {},
                        Fix("security", {{"camnet", Json::array({{{"id", "Cam1"}, {"first", true}}, {{"id", "Cam2"}}, {{"id", "Cam3"}}})}}));
                    return;
                }
                std::vector<const Actor*> first;
                for (const auto* c : cams) if (Truth(Field(*c, "bFirstCam"))) first.push_back(c);
                if (first.size() != 1)
                    Add("camnet.first-cam", std::to_string(first.size()) + " SCamNetwork cameras have bFirstCam True; exactly one should, the camera a merc sees first.",
                        Paths(first.empty() ? cams : first), SetFix(first.empty() ? *cams.front() : *first.front(), {{"bFirstCam", "True"}}));
                // The links, by camera.
                std::map<const Actor*, size_t> slot;
                for (size_t i = 0; i < cams.size(); ++i) slot[cams[i]] = i;
                auto link = [&](const Actor& c, const char* field) -> long long
                {
                    const auto refs = Refs(Field(c, field));
                    if (refs.empty()) return -1;
                    const auto* target = index.Resolve(refs.front());
                    const auto found = target ? slot.find(target) : slot.end();
                    return found == slot.end() ? -1 : static_cast<long long>(found->second);
                };
                std::vector<long long> next(cams.size()), prev(cams.size());
                for (size_t i = 0; i < cams.size(); ++i) { next[i] = link(*cams[i], "NextCam"); prev[i] = link(*cams[i], "PrevCam"); }
                std::vector<std::string> problems;
                for (size_t i = 0; i < cams.size() && problems.size() < 6; ++i)
                {
                    if (next[i] < 0) problems.push_back(Name(*cams[i]) + " has no NextCam camera");
                    else if (prev[static_cast<size_t>(next[i])] != static_cast<long long>(i)) problems.push_back(Name(*cams[i]) + "'s NextCam " + Name(*cams[static_cast<size_t>(next[i])]) + " does not point back with PrevCam");
                    if (prev[i] < 0) problems.push_back(Name(*cams[i]) + " has no PrevCam camera");
                }
                size_t visited = 0;
                bool closed = false;
                {
                    std::set<long long> seen;
                    long long at = 0;
                    while (at >= 0 && seen.insert(at).second) at = next[static_cast<size_t>(at)];
                    visited = seen.size();
                    closed = at == 0;
                }
                // Separate networks: the components Cameras::Networks finds.
                size_t networks = 1;
                Json table = Json::array();
                for (const auto* c : cams)
                {
                    auto value = [&](const char* field)
                    {
                        const auto refs = Refs(Field(*c, field));
                        const auto* target = refs.empty() ? nullptr : index.Resolve(refs.front());
                        return target && slot.count(target) ? Json(target->cls + "'" + target->path + "'") : Json("None");
                    };
                    table.push_back({{"actor", {{"path", c->path}, {"class", c->cls.empty() ? "SBase.SCamNetwork" : c->cls}}},
                                     {"values", {{"NextCam", value("NextCam")}, {"PrevCam", value("PrevCam")}, {"bFirstCam", Truth(Field(*c, "bFirstCam")) ? "True" : "False"}, {"CamName", TextOf(*c, "CamName")}}}});
                }
                Json order = Json::array();
                try
                {
                    const auto found = Workflow::Cameras::Networks(table);
                    networks = found.size();
                    for (const auto& network : found) for (const auto& camera : network.cameras) order.push_back(camera);
                }
                catch (const std::exception&) { order = table; }
                if (!problems.empty() || !closed || visited != cams.size() || networks != 1)
                {
                    std::string text = "The SCamNetwork cameras do not form one closed loop";
                    if (networks > 1) text += " (" + std::to_string(networks) + " separate networks, so mercs never reach the others' cameras)";
                    text += ": ";
                    for (size_t i = 0; i < problems.size(); ++i) text += (i ? "; " : "") + problems[i];
                    if (problems.empty()) text += "following NextCam from " + Name(*cams.front()) + " visits " + std::to_string(visited) + " of " + std::to_string(cams.size()) + " cameras";
                    text += ". Link them in a loop (Cam1 > Cam2 > Cam3 > Cam1, PrevCam the reverse).";
                    Json fix;
                    try
                    {
                        const auto changes = Workflow::Cameras::Wiring(order, true);
                        Json edits = Json::array();
                        for (size_t i = 0; i < order.size() && i < changes.size(); ++i)
                            edits.push_back({{"action", "set"}, {"actor", order[i].at("actor").at("path")}, {"properties", changes[i]}});
                        fix = scene.live ? Fix("actors_edit", {{"edits", edits}}) : Fix("security", {{"camnet", "list the cameras in loop order"}}, "level_build links camnet entries in a loop.");
                    }
                    catch (const std::exception&) {}
                    Add("camnet.loop", text, Paths(cams), fix);
                }
                std::vector<const Actor*> loose;
                for (const auto* c : cams)
                    if (!Truth(Field(*c, "bBlockActors")) || !Truth(Field(*c, "bBlockPlayers")))
                        if (!scene.live || (Field(*c, "bBlockActors") && Field(*c, "bBlockPlayers"))) loose.push_back(c);
                if (!loose.empty())
                    Add("ingredients.collision", std::to_string(loose.size()) + " SCamNetwork camera" + (loose.size() == 1 ? " has" : "s have") + " no collision; set bBlockActors and bBlockPlayers True.",
                        Paths(loose), SetFix(*loose.front(), {{"bBlockActors", "True"}, {"bBlockPlayers", "True"}}));
            }

            // --------------------------------------------------- security
            void Security()
            {
                Json actors = Json::array();
                for (const auto& a : scene.actors)
                {
                    Json item{{"path", a.path}, {"tag", a.tag}, {"event", a.event}};
                    const auto kind = KindOf(a.cls);
                    if (kind == Kind::Alarm || kind == Kind::Detector)
                    {
                        item["kind"] = kind == Kind::Alarm ? "Alarm" : DeviceKind(a.cls);
                        Json events = Json::array();
                        if (const auto* list = Field(a, "Events"))
                            for (const auto& e : list->is_array() ? *list : Json::array({*list}))
                                if (const auto t = Text(e); !IsNone(t)) events.push_back(t);
                        // Locks count as what an alarm does.
                        if (kind == Kind::Alarm && events.empty() && (!Refs(Field(a, "MoversToLock")).empty() || !Refs(Field(a, "ObjectiveTriggersToLock")).empty())) events.push_back("(locks)");
                        item["events"] = events;
                        Json volumes = Json::array();
                        for (const auto& r : Refs(Field(a, "Volumes"))) volumes.push_back(Shown(r));
                        item["volumes"] = volumes;
                    }
                    actors.push_back(item);
                }
                std::map<std::string, const Actor*> byPath;
                for (const auto& a : scene.actors) byPath[a.path] = &a;
                for (const auto& issue : Workflow::Security::Issues(actors))
                {
                    // An alarm another actor triggers by its Tag (a sector's
                    // Zone<N>_Announce from its SMagicEvent, say) is fed, and one
                    // that only shows a message needs no outputs.
                    const auto owner = byPath.find(issue.path);
                    if (owner != byPath.end() && KindOf(owner->second->cls) == Kind::Alarm)
                    {
                        const auto& alarm = *owner->second;
                        bool detected = false;
                        for (const auto* d : detectors) detected = detected || (!IsNone(alarm.tag) && Lower(d->event) == Lower(alarm.tag));
                        const bool announcement = !detected && Sent(alarm.tag, alarm);
                        const auto priority = NumberOf(alarm, "MessagePriority");
                        if (issue.text.find("has no detector wired") != std::string::npos && announcement) continue;
                        if (issue.text.find("triggers nothing when it goes off") != std::string::npos && (announcement || (priority && *priority <= 1))) continue;
                    }
                    std::string rule = issue.severity == "error" ? "security.dangling-event" : "security.detector-unwired";
                    if (issue.text.find("covers no volume") != std::string::npos) rule = "security.sensor-no-volume";
                    else if (issue.text.find("nothing has that Tag") != std::string::npos) rule = "security.dangling-event";
                    else if (issue.text.find("has no detector wired") != std::string::npos) rule = "security.alarm-unfed";
                    else if (issue.text.find("triggers nothing when it goes off") != std::string::npos) rule = "security.alarm-no-output";
                    else if (issue.text.find("give it an alarm") != std::string::npos) rule = "security.detector-unwired";
                    Json fix;
                    if (rule == "security.sensor-no-volume") fix = Fix("security", {{"devices", Json::array({{{"kind", "motion"}, {"box", "the area to watch"}}})}}, "A motion sensor needs a Volume in its Volumes list.");
                    else if (rule == "security.alarm-unfed" || rule == "security.detector-unwired") fix = Fix("security", {{"devices", "set each detector's alarm"}}, "Detectors send Event; the SAlarm's Tag must match it.");
                    else if (rule == "security.alarm-no-output") fix = Fix("security", {{"alarms", Json::array({{{"outputs", "doors, lights or tags to trigger"}, {"lockObjectives", "objectives to lock"}}})}});
                    Add(rule, issue.text, {issue.path}, fix);
                }
                for (const auto& row : Workflow::Security::Wiring(actors))
                {
                    const auto path = row.at("alarm").get<std::string>();
                    std::string text = Last(path) + ": " + std::to_string(row.at("detectors").size()) + " detector(s) feed it";
                    if (!row.at("outputs").empty()) text += ", it triggers " + std::to_string(row.at("outputs").size()) + " actor(s)";
                    Add("security.wiring", text + ".", {path});
                }
                for (const auto* alarm : alarms)
                {
                    if (const auto priority = NumberOf(*alarm, "MessagePriority"); priority && (*priority < 0 || *priority > 5))
                        Add("security.message-priority", Name(*alarm) + " has MessagePriority " + Round(*priority) + "; use 0-1 for messages, 2-4 for alarms that lock nothing, 5 for a full alarm.", {alarm->path},
                            SetFix(*alarm, {{"MessagePriority", "5"}}));
                    for (const auto& r : Refs(Field(*alarm, "ObjectiveTriggersToLock")))
                    {
                        const auto* target = index.Resolve(r);
                        if (target && KindOf(target->cls) == Kind::Bomb)
                            Add("security.bomb-locked", Name(*alarm) + " locks " + Name(*target) + ", an SBombTargetObjectiveTrigger, which alarms cannot lock. Remove it from ObjectiveTriggersToLock.", {alarm->path, target->path},
                                Fix("security", {{"alarms", Json::array({{{"id", Name(*alarm)}, {"lockObjectives", "only terminals and flags"}}})}}));
                        else if (!target && index.Definite(r))
                            Add("security.lock-unresolved", Name(*alarm) + "'s ObjectiveTriggersToLock names " + Shown(r) + ", which is not in the map.", {alarm->path});
                    }
                    for (const auto& r : Refs(Field(*alarm, "MoversToLock")))
                        if (!index.Resolve(r) && index.Definite(r))
                            Add("security.lock-unresolved", Name(*alarm) + "'s MoversToLock names " + Shown(r) + ", which is not in the map.", {alarm->path});
                    if (const auto* list = Field(*alarm, "Events"))
                        for (const auto& e : list->is_array() ? *list : Json::array({*list}))
                            if (const auto t = Text(e); !IsNone(t) && !index.Tagged(t))
                                Add("events.dangling", Name(*alarm) + "'s Events lists " + t + " but nothing in the map has that Tag.", {alarm->path});
                }
            }
            void Zones()
            {
                for (const auto* z : alarmZones)
                    Add("zones.alarm-zone", Name(*z) + " is an SAlarmZoneInfo: Ubisoft never used it, it detects the camera rather than the spy and fails while a merc is inside. Use a Volume with an SVolumetricSensor.",
                        {z->path}, Fix("security", {{"devices", Json::array({{{"kind", "motion"}, {"box", "the zone's area"}}})}}));
                std::vector<const Actor*> unnamed;
                for (const auto* z : zones)
                    if (ClassKey(z->cls) != "skyzoneinfo" && IsNone(TextOf(*z, "LocationName")) && (!scene.live || Field(*z, "LocationName"))) unnamed.push_back(z);
                if (!unnamed.empty())
                    Add("zones.location-name", std::to_string(unnamed.size()) + " ZoneInfo actor" + (unnamed.size() == 1 ? " has" : "s have") + " no LocationName, so the HUD shows no location there.",
                        Paths(unnamed), Fix("environment", {{"zones", "give each zone a locationName"}}));
            }

            // ----------------------------------------------------- actors
            void Actors()
            {
                std::map<std::string, std::vector<const Actor*>> bodies;
                for (const auto& a : scene.actors)
                {
                    const auto kind = KindOf(a.cls);
                    if (const auto* effect = Field(a, "LightEffect"))
                    {
                        const auto e = Lower(Text(*effect));
                        if (!e.empty() && e != "le_none" && e != "le_spotlight" && e != "le_sunlight" && e.rfind("le_e", 0) != 0)
                            Add("lighting.static-effect", Name(a) + " uses LightEffect " + Text(*effect) + "; only LE_None, LE_Spotlight and LE_Sunlight work for static lights (LE_ESpotShadow for Echelon lights).",
                                {a.path}, SetFix(a, {{"LightEffect", "LE_None"}}));
                        if (e == "le_espotshadow")
                            if (const auto influence = NumberOf(a, "EchelonNormalInfluence"); influence && std::abs(*influence - .9) > .05)
                                Add("lighting.echelon-normal", Name(a) + " is an Echelon light with EchelonNormalInfluence " + Round(*influence, 2) + "; 0.9 is the usual setting.", {a.path});
                    }
                    if (Truth(Field(a, "bPickable")))
                        Add("objects.pickable", Name(a) + " has bPickable True: picking objects up is unfinished in the game and never shipped.", {a.path}, SetFix(a, {{"bPickable", "False"}}));
                    if (Truth(Field(a, "bCanBeChaffed")))
                        Add("objects.chaff", Name(a) + " has bCanBeChaffed True; keep it off, as the game turns chaffing on where STriggerLights need it.", {a.path}, SetFix(a, {{"bCanBeChaffed", "False"}}));
                    if (kind == Kind::SoftBody)
                    {
                        std::vector<Ref> refs;
                        for (auto it = a.values.begin(); it != a.values.end(); ++it) CollectRefs(it.value(), refs, false);
                        for (const auto& r : refs)
                            if (Lower(r.cls).find("softbody") != std::string::npos && Lower(r.path) != Lower(a.path)) bodies[Lower(r.path)].push_back(&a);
                    }
                    if (kind == Kind::Mover)
                    {
                        const auto* mesh = Field(a, "StaticMesh");
                        if ((mesh && IsNone(Text(*mesh))) || (!mesh && !scene.live))
                            Add("movers.static-mesh", Name(a) + " has no StaticMesh; movers use a static mesh for their look.", {a.path},
                                Fix("mechanisms", {{"doors", Json::array({{{"id", Name(a)}, {"mesh", "StaticMesh'Package.Group.Name'"}}})}}));
                    }
                    if (kind == Kind::Fish)
                        for (const auto& r : Refs(Field(a, "Pilot")))
                        {
                            const auto* pilot = index.Resolve(r);
                            if (!pilot && !index.Definite(r)) continue;
                            if (!pilot || KindOf(pilot->cls) != Kind::Fish || !Truth(Field(*pilot, "bPilote")))
                                Add("fish.pilot", Name(a) + "'s Pilot " + (pilot ? Name(*pilot) : Shown(r)) + " is not an SFish or SFish_Big with bPilote True.", {a.path});
                        }
                    auto hiddenRule = [&](const char* rule, const std::string& what)
                    {
                        const auto* hidden = Field(a, "bHidden");
                        if ((hidden && !Truth(hidden)) || (!hidden && !scene.live))
                            Add(rule, Name(a) + " (" + what + ") is not hidden, so its sprite shows in game: set bHidden True.", {a.path}, SetFix(a, {{"bHidden", "True"}}));
                    };
                    if (kind == Kind::Tutorial) hiddenRule("tutorials.hidden", Last(a.cls));
                    if (kind == Kind::Sprinkler) hiddenRule("ingredients.sprinkler-hidden", "SSprinkler");
                    if (kind == Kind::Infirmary && (!Truth(Field(a, "bBlockActors")) || !Truth(Field(a, "bBlockPlayers"))) && (!scene.live || (Field(a, "bBlockActors") && Field(a, "bBlockPlayers"))))
                        Add("ingredients.collision", Name(a) + " (SInfirmary) has no collision; set bBlockActors and bBlockPlayers True.", {a.path}, SetFix(a, {{"bBlockActors", "True"}, {"bBlockPlayers", "True"}}));
                    double height = 0, radius = 0;
                    if (kind == Kind::Ammo) { height = 60; radius = 120; }
                    if (kind == Kind::Infirmary || kind == Kind::Switch) { height = 120; radius = 60; }
                    if (height > 0)
                    {
                        const auto h = NumberOf(a, "fInteractionHeight"), r = NumberOf(a, "fInteractionRadius");
                        if ((h && std::abs(*h - height) > .5) || (r && std::abs(*r - radius) > .5))
                            Add("ingredients.interaction", Name(a) + " (" + Last(a.cls) + ") uses an interaction height of " + (h ? Round(*h) : std::string("?")) + " and radius " + (r ? Round(*r) : std::string("?")) +
                                "; the Enhanced maps use " + Round(height) + " and " + Round(radius) + ".", {a.path}, SetFix(a, {{"fInteractionHeight", Round(height, 1)}, {"fInteractionRadius", Round(radius, 1)}}));
                    }
                    // Detectors are the security rules' (security.dangling-event).
                    if (kind != Kind::Detector && !IsNone(a.event) && !index.Tagged(a.event))
                        Add("events.dangling", Name(a) + " sends Event " + a.event + " but nothing in the map has that Tag.", {a.path});
                    if (ClassKey(a.cls) == "smagicevent")
                        if (const auto* groups = Field(a, "Groups"))
                        {
                            std::vector<std::string> events, missing;
                            SentEvents(*groups, events);
                            for (const auto& e : events) if (!index.Tagged(e) && std::find(missing.begin(), missing.end(), e) == missing.end()) missing.push_back(e);
                            if (!missing.empty())
                                Add("events.dangling", Name(a) + " (SMagicEvent) sends " + Join(missing) + " but nothing in the map has " + (missing.size() == 1 ? "that Tag." : "those Tags."), {a.path});
                        }
                }
                for (const auto& [body, owners] : bodies)
                    if (owners.size() > 1)
                        Add("softbody.duplicated", std::to_string(owners.size()) + " SoftBody actors share one ESoftBody (" + body + "): they were duplicated. Delete the copies and create each one new.",
                            Paths(owners), Fix("environment", {{"softbody", "create each SoftBody new instead of copying"}}));
            }

            // ------------------------------------------------------ level
            void Level()
            {
                const auto& v = scene.level;
                if (!v.is_object() || v.empty()) return;
                auto number = [&](std::initializer_list<const char*> names) -> std::optional<double>
                {
                    for (const char* n : names) if (const auto* f = Field(v, n)) return Number(f);
                    return std::nullopt;
                };
                if (const auto death = number({"fDeathPawnHeight", "deathPawnHeight"}); death && *death != 0)
                {
                    std::vector<const Actor*> below;
                    for (const auto* a : starts) if (a->position[2] < *death) below.push_back(a);
                    for (const auto* a : triggers) if (a->position[2] < *death) below.push_back(a);
                    for (const auto* a : disks) if (a->position[2] < *death) below.push_back(a);
                    if (!below.empty())
                        Add("level.death-height", std::to_string(below.size()) + " start(s) or objective(s) sit below fDeathPawnHeight " + Round(*death) + ", where players die instantly.", Paths(below),
                            Fix("map_settings", {{"level", {{"deathPawnHeight", 0}}}}, "Lower fDeathPawnHeight below everything players use, or 0 to turn it off."));
                }
                // Level events: {Event, SendAtTime} structs, parallel arrays, or the spec's {event, sendAtTime}.
                std::vector<std::pair<std::string, double>> events;
                const Json* list = Field(v, "Events");
                if (!list) list = Field(v, "events");
                const Json* times = Field(v, "SendAtTime");
                if (list && list->is_array())
                    for (size_t i = 0; i < list->size(); ++i)
                    {
                        const auto& item = (*list)[i];
                        std::string name;
                        std::optional<double> at;
                        if (item.is_object())
                        {
                            const Json* n = Field(item, "Event");
                            if (!n) n = Field(item, "event");
                            if (n) name = Text(*n);
                            const Json* t = Field(item, "SendAtTime");
                            if (!t) t = Field(item, "sendAtTime");
                            at = Number(t);
                        }
                        else
                        {
                            name = Text(item);
                            if (times && times->is_array() && i < times->size()) at = Number(&(*times)[i]);
                            else if (times && !times->is_array()) at = Number(times);
                        }
                        if (!IsNone(name) && at) events.push_back({name, *at});
                    }
                for (const auto& [name, at] : events)
                {
                    if (at >= 0)
                        Add("level.send-at-time", "Level event " + name + " has SendAtTime " + Round(at, 1) + ", which never fires: SendAtTime counts back from the end of the match (-180 is 3 minutes left).", {},
                            Fix("map_settings", {{"level", {{"events", Json::array({{{"event", name}, {"sendAtTime", -180}}})}}}}));
                    if (!index.Tagged(name)) Add("events.dangling", "Level event " + name + " names a Tag nothing in the map has.", {});
                }
                if (const auto* tweak = Field(v, "MotionTrackingTweak"); tweak && tweak->is_object())
                {
                    const auto offset = Number(Field(*tweak, "MTVOffset")), slope = Number(Field(*tweak, "MTVSlope"));
                    if (offset && slope && !((*offset == 52 && *slope == 85) || (*offset == 8 && *slope == 8)))
                        Add("level.motion-tracking", "MotionTrackingTweak is MTVOffset " + Round(*offset) + " / MTVSlope " + Round(*slope) + "; most maps use 52 / 85, Enhanced maps 8 / 8.", {},
                            Fix("map_settings", {{"level", {{"motionTracking", "standard"}}}}));
                }
            }

            // -------------------------------------------------- documents
            void Documents()
            {
                for (size_t i = 0; i < scene.documents.size(); ++i)
                {
                    const auto& doc = scene.documents[i];
                    try
                    {
                        Workflow::Authoring::Validate(doc, doc.is_object() && doc.contains("map") && doc.at("map").is_string() ? doc.at("map").get<std::string>() : std::string("*"));
                    }
                    catch (const std::exception& e)
                    {
                        Add("authoring.validate", "Change document " + std::to_string(i + 1) + " would be refused by Import Map from JSON: " + e.what(), {},
                            Fix("map_changes_apply", {{"dryRun", true}}, "Fix the document, then preview it with dryRun."));
                    }
                }
                if (scene.live || !scene.schema.is_object()) return;
                const Json classes = scene.schema.contains("classes") ? scene.schema.at("classes") : scene.schema;
                if (!classes.is_object()) return;
                for (const auto& a : scene.actors)
                {
                    const Json* entry = Field(classes, a.cls);
                    if (!entry) for (auto it = classes.begin(); it != classes.end() && !entry; ++it) if (ClassKey(it.key()) == ClassKey(a.cls)) entry = &it.value();
                    if (!entry || !entry->is_object() || !entry->contains("schema")) continue;
                    const auto& schema = entry->at("schema");
                    for (auto it = a.values.begin(); it != a.values.end(); ++it)
                    {
                        if (!it.key().empty() && it.key()[0] == '$') continue;
                        const auto* field = Field(schema, it.key());
                        if (!field || !field->is_object()) continue;
                        if (field->contains("writable") && field->at("writable") == false && !(field->contains("create") && field->at("create") == true))
                            Add("authoring.unwritable", a.path + " sets " + it.key() + ", which Map JSON cannot write: " + field->value("reason", std::string("the property is not writable")) + ".", {a.path});
                        else if (field->contains("choices") && field->at("choices").is_array() && it.value().is_string())
                        {
                            const auto value = Lower(it.value().get<std::string>());
                            bool listed = false;
                            for (const auto& choice : field->at("choices")) if (choice.is_string() && Lower(choice.get<std::string>()) == value) listed = true;
                            if (!listed)
                                Add("authoring.unwritable", a.path + " sets " + it.key() + " to " + it.value().get<std::string>() + ", which is not one of its choices (" + field->at("choices").dump() + ").", {a.path});
                        }
                    }
                }
            }

            // ------------------------------------------- settings, release
            void SettingsRules()
            {
                const auto& s = settings;
                if (!s.given) return;
                if (s.exists && !*s.exists)
                    Add("settings.missing", "The map has no SMapSettings in <Map>-i yet: save the map, exit the editor and reopen it, then fill Map Settings (MapName, briefs, Modes).", {},
                        Fix("editor_restart", Json::object(), "Then map_settings."));
                if (s.known)
                {
                    if (!s.mapName.empty() && std::any_of(s.mapName.begin(), s.mapName.end(), [](unsigned char c) { return std::islower(c) != 0; }))
                    {
                        std::string upper = s.mapName;
                        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        Add("settings.map-name-caps", "MapName \"" + s.mapName + "\" is not in capitals; the Ubisoft standard is \"" + upper + "\".", {}, Fix("map_settings", {{"mapName", upper}}));
                    }
                    std::set<std::string> seen;
                    for (size_t i = 0; i < s.modes.size(); ++i)
                    {
                        const auto& m = s.modes[i];
                        std::vector<std::string> wrong;
                        if (m.respawn && (*m.respawn < 5 || *m.respawn > 15)) wrong.push_back("RespawnTime " + Round(*m.respawn) + " (5-15)");
                        if (m.spiesLives && (*m.spiesLives < 0 || *m.spiesLives > 10)) wrong.push_back("SpiesLives " + Round(*m.spiesLives) + " (0-10)");
                        if (m.sentriesLives && (*m.sentriesLives < 0 || *m.sentriesLives > 10)) wrong.push_back("SentriesLives " + Round(*m.sentriesLives) + " (0-10)");
                        if (m.timeLimit && (*m.timeLimit < 0 || *m.timeLimit > 20)) wrong.push_back("TimeLimit " + Round(*m.timeLimit) + " (0-20 minutes)");
                        if (!wrong.empty())
                        {
                            Add("settings.ranges", "Map Settings Modes[" + std::to_string(i) + "] (" + m.raw + ") is outside the supported ranges: " + Join(wrong) + ".", {},
                                Fix("map_settings", {{"modes", Json::array({{{"mode", m.key}, {"respawn", 10}, {"timeLimit", 10}}})}}));
                        }
                        if (!seen.insert(m.key).second)
                            Add("settings.mode-parity", "Map Settings Modes lists " + m.raw + " twice; each mode has one entry.", {}, Fix("map_settings", {{"modes", "one entry per mode"}}));
                    }
                    if (s.strings && *s.strings != s.modes.size())
                        Add("settings.mode-parity", "Map Settings has " + std::to_string(s.modes.size()) + " Modes but " + std::to_string(*s.strings) + " ModesStrings; ModesStrings[i] holds the text of Modes[i].", {},
                            Fix("map_settings", {{"modes", "give every mode its strings"}}));
                    for (const char* mode : {"story", "diskhunt", "deathmatch"})
                        if (!MissionsOf(mode).empty() && std::none_of(s.modes.begin(), s.modes.end(), [&](const SettingsMode& m) { return m.key == mode; }))
                            Add("settings.modes-missing", std::string("The map has a ") + ModeName(mode) + " SMission but Map Settings has no Modes entry for it.", {},
                                Fix("map_settings", {{"modes", Json::array({{{"mode", mode}}})}}));
                }
                struct Slot { const char* name; const char* spec; int size; const char* format; };
                static const Slot slots[] = {{"Menu", "menu", 256, "DXT5"}, {"Briefing", "briefing", 256, "DXT5"}, {"LoadingATT0", "loadingAtt", 512, "DXT1"}, {"LoadingDEF0", "loadingDef", 512, "DXT1"}};
                if (s.textures.is_object())
                    for (const auto& slot : slots)
                    {
                        const auto* t = Field(s.textures, slot.name);
                        if (!t)
                        {
                            if (s.exists.value_or(true))
                                Add("interface.textures", std::string("<Map>-i has no ") + slot.name + " texture (" + std::to_string(slot.size) + "x" + std::to_string(slot.size) + " " + slot.format + ").", {},
                                    Fix("map_settings", {{"interface", {{slot.spec, "path to the image"}}}}));
                            continue;
                        }
                        if (!t->is_object() || t->contains("unreadable"))
                        {
                            Add("interface.textures", std::string(slot.name) + " in <Map>-i cannot be read" + (t->is_object() ? ": " + Text(t->at("unreadable")) : std::string()) + ".", {});
                            continue;
                        }
                        const auto expected = Get(*t, "expected");
                        const int size = static_cast<int>(Number(expected.is_object() ? Field(expected, "size") : nullptr).value_or(slot.size));
                        const std::string format = expected.is_object() && Field(expected, "format") ? Text(*Field(expected, "format")) : std::string(slot.format);
                        const int width = static_cast<int>(Number(Field(*t, "width")).value_or(0)), height = static_cast<int>(Number(Field(*t, "height")).value_or(0));
                        const std::string have = Field(*t, "format") ? Text(*Field(*t, "format")) : std::string();
                        if (width != size || height != size || Lower(have) != Lower(format))
                            Add("interface.textures", std::string(slot.name) + " is " + std::to_string(width) + "x" + std::to_string(height) + " " + have + "; the game expects " + std::to_string(size) + "x" +
                                std::to_string(size) + " " + format + ".", {}, Fix("map_settings", {{"interface", {{slot.spec, "path to the image"}}}}, "interface.texture converts and imports it."));
                    }
                Json images = s.images;
                if (!images.is_object() && scene.release.is_object() && scene.release.contains("interface")) images = scene.release.at("interface");
                if (images.is_object() && !s.textures.is_object())
                    for (const auto& slot : slots)
                        if (!Field(images, slot.spec) && !Field(images, slot.name))
                            Add("interface.textures", std::string("The spec gives no ") + slot.name + " image (interface." + slot.spec + ", " + std::to_string(slot.size) + "x" + std::to_string(slot.size) + " " + slot.format + ").", {},
                                Fix("map_settings", {{"interface", {{slot.spec, "path to the image"}}}}));
            }
            void Release()
            {
                const std::string stem = [&]
                {
                    auto file = scene.mapFile;
                    const auto slash = file.find_last_of("\\/");
                    if (slash != std::string::npos) file = file.substr(slash + 1);
                    return file;
                }();
                const auto dot = stem.find_last_of('.');
                const std::string base = dot == std::string::npos ? stem : stem.substr(0, dot), extension = dot == std::string::npos ? std::string() : Lower(stem.substr(dot));
                if (!stem.empty())
                {
                    const bool ascii = std::all_of(stem.begin(), stem.end(), [](unsigned char c) { return c >= 0x20 && c < 0x80; });
                    if (!ascii || stem.find(' ') != std::string::npos || (!extension.empty() && extension != ".sdc"))
                        Add("release.file-name", "The map file \"" + stem + "\" must be an ASCII .sdc whose name has no spaces (the engine parses it on a command line).", {},
                            Fix("map_file", {{"action", "save_as"}, {"path", "Packages\\MapsEd\\" + (scene.mapName.size() >= 4 ? scene.mapName.substr(0, 4) : std::string("Map")) + "D.sdc"}}));
                    bool convention = base.size() == 5 && std::all_of(base.begin(), base.end(), [](unsigned char c) { return std::isalpha(c) != 0; });
                    if (convention && !scene.mapName.empty())
                    {
                        std::string letters;
                        for (unsigned char c : scene.mapName) if (std::isalpha(c)) letters += static_cast<char>(c);
                        convention = letters.size() >= 4 && Lower(base.substr(0, 4)) == Lower(letters.substr(0, 4));
                    }
                    if (!convention)
                        Add("release.naming-convention", "The map file " + base + " does not follow the convention: the first 4 letters of the map name and a suffix letter, D for the default version (AquaD for Aquarius).", {});
                }
                const auto& r = scene.release;
                if (!r.is_object()) return;
                std::map<std::string, std::string> names;
                std::vector<std::string> packageNames;
                for (const auto& p : Get(r, "packages"))
                {
                    std::string name = p.is_string() ? p.get<std::string>() : GetString(p, {"name", "file", "path"});
                    if (name.empty()) continue;
                    const auto slash = name.find_last_of("\\/");
                    if (slash != std::string::npos) name = name.substr(slash + 1);
                    const auto period = name.find_last_of('.');
                    const auto packageBase = period == std::string::npos ? name : name.substr(0, period);
                    const auto packageExt = period == std::string::npos ? std::string() : Lower(name.substr(period));
                    packageNames.push_back(packageBase);
                    const auto key = Lower(packageBase);
                    if (names.count(key))
                        Add("release.package-names", "Two release packages share the base name " + packageBase + " (" + names[key] + " and " + name + "); the engine treats them as one package.", {},
                            Fix("release", {{"check", true}}, "Rename one: <Map>_TXT, <Map>_STM, Amb_<Map>."));
                    names[key] = name;
                    if (!base.empty() && key == Lower(base) && !packageExt.empty() && packageExt != ".sdc")
                        Add("release.package-names", name + " has the map's own base name " + base + ", so it clashes with the map package.", {}, Fix("release", {{"check", true}}, "Name it " + base + "_TXT or " + base + "_STM."));
                    if (p.is_object() && (p.value("stock", false) || p.value("modified", false)))
                        Add("release.package-names", name + " is a stock package; a release must never modify or ship one (it breaks other players' game).", {}, Fix("release", {{"check", true}}));
                    else if (!base.empty() && !Bridge::MapOwnedPackage(packageBase, base))
                        Add("release.naming-convention", "Release package " + name + " is not named after the map (" + base + "_TXT, " + base + "_STM, Amb_" + base + ").", {});
                }
                const auto uas = Get(r, "uas");
                if (!uas.is_null())
                {
                    std::vector<std::string> inside;
                    for (const auto& p : uas.is_object() ? Get(uas, "packages") : uas) if (p.is_string()) inside.push_back(Lower(p.get<std::string>()));
                    if (std::find(inside.begin(), inside.end(), "interface") == inside.end())
                        Add("release.uas", "The map's .uas does not include Interface; menus and HUD sounds go silent on the map.", {}, Fix("release", {{"sounds", true}}, "Build the .uas with Interface and the map's Amb_ package."));
                    for (const auto& p : packageNames)
                        if (Lower(p).rfind("amb_", 0) == 0 && std::find(inside.begin(), inside.end(), Lower(p)) == inside.end())
                            Add("release.uas", "The map's .uas does not include " + p + ", the map's own ambient sounds.", {}, Fix("release", {{"sounds", true}}));
                }
                // What only the release tool's file check sees, from its findings;
                // the rules above check the rest from the facts themselves.
                static const std::set<std::string> fileRules{"release.files", "release.dependencies", "release.optimise"};
                for (const auto& row : Get(r, "findings"))
                {
                    const auto rule = GetString(row, {"rule"});
                    const auto message = GetString(row, {"message"});
                    if (!fileRules.count(rule) || message.empty()) continue;
                    const auto severity = GetString(row, {"severity"});
                    Add(rule, message, {}, Get(row, "fix"), severity == "error" || severity == "warning" || severity == "info" ? severity : std::string());
                }
            }

            // --------------------------------------------- geometric events
            void Ges()
            {
                bool fences = false;
                const auto list = GeList(scene.ges);
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const auto& ge = list[i];
                    Ge::Element e;
                    try { e = Ge::FromJson(ge); }
                    catch (const std::exception& problem)
                    {
                        Add("ge.points", "GE " + std::to_string(i + 1) + ": " + problem.what(), {}, Fix("geometric_events", {{"ges", Json::array({{{"kind", "ledge"}, {"points", "[[x,y,z],[x,y,z]]"}}})}}));
                        continue;
                    }
                    fences = fences || std::string(e.kind->command) == "FENCE";
                    // Floors: what map_validate traced, else the blockout.
                    std::vector<std::pair<Workflow::Vector, std::optional<double>>> traced;
                    if (ge.is_object() && ge.contains("$floors") && ge.at("$floors").is_array())
                        for (const auto& f : ge.at("$floors"))
                        {
                            bool ok = false;
                            const auto at = PointOf(Get(f, "at"), ok);
                            if (!ok) continue;
                            const auto value = Get(f, "floor");
                            traced.push_back({at, value.is_number() ? std::optional<double>(value.get<double>()) : std::nullopt});
                        }
                    const Json& blockout = pieces;
                    Ge::FloorAt floor = [&traced, &blockout](const Workflow::Vector& at) -> std::optional<double>
                    {
                        for (const auto& [point, value] : traced) if (Distance(point, at) < .5) return value;
                        if (!blockout.empty()) return FloorBelow(blockout, at);
                        return std::nullopt;
                    };
                    if (traced.empty() && pieces.empty()) floor = {};
                    // How deep the platform behind a ledge is, from the blockout.
                    if (std::string(e.kind->command) == "LG" && e.surface < 0 && e.points.size() == 2)
                        for (const auto& piece : placed)
                        {
                            if (piece.kind != "Platform" || std::abs(piece.extent.hi[2] - std::max(e.points[0][2], e.points[1][2])) > 4) continue;
                            for (const auto& probe : Ge::Probes(e))
                                if (Inside(piece.extent, {probe[0], probe[1], piece.extent.hi[2] - 1}, 1))
                                {
                                    const bool alongX = std::abs(e.points[1][0] - e.points[0][0]) >= std::abs(e.points[1][1] - e.points[0][1]);
                                    e.surface = alongX ? piece.extent.hi[1] - piece.extent.lo[1] : piece.extent.hi[0] - piece.extent.lo[0];
                                }
                        }
                    const auto& p0 = e.points.empty() ? Workflow::Vector{} : e.points[0];
                    const std::string label = "GE " + std::to_string(i + 1) + " (" + e.kind->name + " at " + Round(p0[0]) + ", " + Round(p0[1]) + ", " + Round(p0[2]) + ")";
                    for (const auto& m : Ge::Measure(e, floor))
                    {
                        Json args{{"kind", e.kind->spec}};
                        if (m.suggestion.is_object() && m.suggestion.contains("points")) args["points"] = m.suggestion.at("points");
                        else args["points"] = Ge::ToJson(e).at("points");
                        if (m.suggestion.is_object() && m.suggestion.contains("bars")) args["bars"] = m.suggestion.at("bars");
                        Add(m.rule, label + ": " + m.message, {}, Fix("geometric_events", {{"ges", Json::array({args})}}));
                    }
                }
                if (fences)
                    Add("ge.fence-climbable", "Fence GEs work only on a texture with bClimbable 1 (texture Properties > Echelon); set it on the fence's texture and re-save its package.", {});
            }

            // ------------------------------------------------ Map Design
            void Design()
            {
                if (!pieces.empty())
                {
                    const auto& group = Groups();
                    for (const auto& issue : Workflow::Design::DesignIssues(pieces, Json::object()))
                    {
                        // A room only stairs, a lift or a climb reach (a storey
                        // above) is joined all the same.
                        if (issue.severity == "error" && issue.text.find("has no doorway") != std::string::npos)
                        {
                            const auto at = std::find_if(placed.begin(), placed.end(), [&](const Placed& p) { return p.index == issue.piece; });
                            if (at != placed.end())
                            {
                                const auto i = static_cast<size_t>(at - placed.begin());
                                bool joined = false;
                                for (size_t j = 0; j < placed.size() && !joined; ++j) joined = j != i && group[j] == group[i];
                                if (joined) continue;
                            }
                        }
                        // A platform above step height that stairs, a ramp or a
                        // climbing GE reaches is reached.
                        if (issue.severity == "warning" && issue.text.find("step height") != std::string::npos && PlatformReached(issue.piece)) continue;
                        // What the compiler adds on purpose (zone portals, joints)
                        // cuts into what it joins; it marks those generated. A
                        // stairwell or lift shaft carves through the floors it
                        // joins, whoever made it, and needs no doorway.
                        const auto* piece = PlacedPiece(issue.piece);
                        if (piece && Passes(issue.piece) && (piece->kind == "Doorway" || issue.text.find("carve into each other") != std::string::npos || issue.text.find("has no doorway") != std::string::npos))
                            continue;
                        if (piece && issue.text.rfind(piece->name + " and ", 0) == 0 && issue.text.find(" carve into each other") != std::string::npos)
                        {
                            const auto other = issue.text.substr(piece->name.size() + 5, issue.text.find(" carve into each other") - piece->name.size() - 5);
                            bool passing = false;
                            for (const auto& p : placed)
                                if (p.index > issue.piece && p.name == other && Workflow::Design::Overlaps(piece->extent, p.extent, -2) && Passes(p.index)) passing = true;
                            if (passing) continue;
                        }
                        // A corridor, vent or crawlway joins what it overlaps:
                        // that merge is the junction, not a mistake. Two rooms
                        // carving into each other still is.
                        if (piece && issue.text.find(" carve into each other") != std::string::npos && issue.text.rfind(piece->name + " and ", 0) == 0)
                        {
                            const auto other = issue.text.substr(piece->name.size() + 5, issue.text.find(" carve into each other") - piece->name.size() - 5);
                            auto open = [](const std::string& kind) { return kind == "Corridor" || Workflow::Design::Crouching(kind); };
                            bool junction = open(piece->kind);
                            for (const auto& p : placed) if (p.name == other && open(p.kind)) junction = true;
                            if (junction) continue;
                        }
                        // A doorway into a vent or crawlway is passed crouched.
                        if (piece && piece->kind == "Doorway" && issue.text.find("units high, below") != std::string::npos && piece->extent.hi[2] - piece->extent.lo[2] >= 125 - .5)
                        {
                            bool vent = false;
                            for (const auto& other : placed)
                                if (Workflow::Design::Crouching(other.kind) && Workflow::Design::Overlaps(piece->extent, other.extent, other.thickness + 1)) vent = true;
                            if (vent) continue;
                        }
                        Add(issue.severity == "error" ? "design.layout" : "design.traversal", issue.text, {},
                            issue.severity == "error" ? Fix("blockout", {{"doorways", "join every room to the layout"}}, "Rooms need a doorway, corridor or vent; doorways must cut into a room.") : Json());
                    }
                    Unreachable();
                }
                if (scene.live && !scene.geometryBuilt)
                    Add("design.stale-geometry", "Brushes changed since the last geometry build (or the map was never built); build before testing or tracing.", {}, Fix("map_build", {{"what", "geometry"}}));
            }
            const Placed* PlacedPiece(int piece) const
            {
                const auto at = std::find_if(placed.begin(), placed.end(), [&](const Placed& p) { return p.index == piece; });
                return at == placed.end() ? nullptr : &*at;
            }
            // A piece that carves into what it joins on purpose: one the
            // compiler generated, or a stairwell, floor opening or shaft.
            bool Passes(int piece) const
            {
                if (piece < 0 || static_cast<size_t>(piece) >= pieces.size()) return false;
                const auto& p = pieces[static_cast<size_t>(piece)];
                if (p.value("generated", false)) return true;
                const auto& spec = p.at("spec");
                return spec.value("kind", std::string()) == "Room" && Workflow::Design::SpansFloors(spec.value("name", std::string()));
            }
            bool PlatformReached(int piece) const
            {
                const auto* at = PlacedPiece(piece);
                if (!at || at->kind != "Platform") return false;
                for (const auto& other : placed)
                    if (other.index != piece && (Workflow::Design::StairKind(other.kind) || other.kind == "Ramp") && Workflow::Design::Overlaps(other.extent, at->extent, 2)) return true;
                for (const auto& ge : GeList(scene.ges))
                    try
                    {
                        for (const auto& p : Ge::FromJson(ge).points) if (Inside(at->extent, p, 32)) return true;
                    }
                    catch (const std::exception&) {}
                return false;
            }
            // Spaces the spies can walk between, from the pieces: carved spaces
            // joined where they meet, stairs, ramps and platforms joining what
            // they touch, climbing GEs and lifts joining the spaces at their
            // ends. groups[i] names placed[i]'s group.
            std::vector<size_t> pieceGroups;
            bool grouped = false;
            std::vector<size_t> SpacesAt(const Workflow::Vector& p, double margin) const
            {
                std::vector<size_t> found;
                for (size_t i = 0; i < placed.size(); ++i) if (Space(placed[i].kind) && Inside(placed[i].extent, p, margin)) found.push_back(i);
                return found;
            }
            const std::vector<size_t>& Groups()
            {
                if (grouped) return pieceGroups;
                grouped = true;
                const size_t n = placed.size();
                std::vector<size_t> parent(n);
                for (size_t i = 0; i < n; ++i) parent[i] = i;
                auto root = [&](size_t i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; };
                auto join = [&](size_t a, size_t b) { parent[root(a)] = root(b); };
                auto open = [](const std::string& kind) { return kind == "Corridor" || kind == "Vent" || kind == "Crawlway"; };
                for (size_t i = 0; i < n; ++i)
                    for (size_t j = i + 1; j < n; ++j)
                    {
                        const auto& a = placed[i];
                        const auto& b = placed[j];
                        // A doorway meets the room faces it cuts between; a corridor
                        // or vent cuts through the wall at its ends; two rooms join
                        // only where they carve into each other.
                        double margin = 2;
                        if (Space(a.kind) && Space(b.kind) && a.kind != "Doorway" && b.kind != "Doorway") margin = open(a.kind) || open(b.kind) ? 1 : -2;
                        if (Workflow::Design::Overlaps(a.extent, b.extent, margin)) join(i, j);
                    }
                for (const auto& ge : GeList(scene.ges))
                {
                    try
                    {
                        const auto e = Ge::FromJson(ge);
                        std::vector<size_t> ends;
                        for (const auto& p : e.points) for (auto i : SpacesAt(p, 64)) ends.push_back(i);
                        for (size_t k = 1; k < ends.size(); ++k) join(ends[0], ends[k]);
                    }
                    catch (const std::exception&) {}
                }
                for (const auto* lift : lifts)
                {
                    std::vector<size_t> column;
                    for (size_t i = 0; i < n; ++i)
                        if (Space(placed[i].kind) && Inside({{placed[i].extent.lo[0], placed[i].extent.lo[1], -1e9}, {placed[i].extent.hi[0], placed[i].extent.hi[1], 1e9}}, lift->position, 64)) column.push_back(i);
                    for (size_t k = 1; k < column.size(); ++k) join(column[0], column[k]);
                }
                pieceGroups.resize(n);
                for (size_t i = 0; i < n; ++i) pieceGroups[i] = root(i);
                return pieceGroups;
            }
            void Unreachable()
            {
                if (placed.empty() || spies.empty()) return;
                const auto& group = Groups();
                std::set<size_t> reached;
                for (const auto* s : spies)
                {
                    const auto found = SpacesAt(s->position, 16);
                    if (found.empty()) return; // A start outside the blockout: the pieces do not tell.
                    for (auto i : found) reached.insert(group[i]);
                }
                std::vector<const Actor*> goals = triggers;
                goals.insert(goals.end(), disks.begin(), disks.end());
                for (const auto* goal : goals)
                {
                    const auto found = SpacesAt(goal->position, 16);
                    if (found.empty()) continue;
                    if (std::none_of(found.begin(), found.end(), [&](size_t i) { return reached.count(group[i]) > 0; }))
                        Add("layout.unreachable", Name(*goal) + " is in " + placed[found.front()].name + ", which no doorway, corridor, stair, lift or climbing GE joins to a spy start.", {goal->path},
                            Fix("blockout", {{"doorways", Json::array({{{"between", Json::array({placed[found.front()].name, "a room the spies reach"})}}})}}));
                }
            }
            void MapCheck()
            {
                const auto rows = Get(scene.mapCheck, "rows");
                if (!rows.is_array()) return;
                for (const auto& row : rows)
                {
                    if (!row.is_object()) continue;
                    const auto group = GetString(row, {"group"}), check = GetString(row, {"check"}), message = GetString(row, {"message"});
                    std::vector<std::string> paths;
                    for (const auto& p : Get(row, "actors")) if (p.is_string() && paths.size() < 50) paths.push_back(p.get<std::string>());
                    const auto text = "Map Check (" + group + (check.empty() ? "" : " / " + check) + "): " + message;
                    const auto lower = Lower(group + " " + check + " " + message);
                    if (lower.find("leak") != std::string::npos)
                        Add("zones.leak", text, paths, Fix("map_query", {{"what", "design"}}, "Close the gap the leak path runs through, or add a zone portal sheet in the doorway."));
                    else
                    {
                        auto severity = GetString(row, {"severity"});
                        if (severity != "error" && severity != "warning") severity = "info";
                        Add("mapcheck.row", text, paths, Json(), severity);
                    }
                }
            }
            void Lighting()
            {
                const auto& l = scene.lighting;
                if (!l.is_object()) return;
                std::vector<std::string> paths;
                for (const auto& h : Get(l, "hotspots"))
                    for (const auto& p : Get(h, "lights")) if (p.is_string() && paths.size() < 30) paths.push_back(p.get<std::string>());
                const auto build = Get(l, "buildWarning");
                const auto summary = GetString(l, {"summary"});
                if (build.is_object() && build.value("over", false))
                    Add("lighting.budget", "The lighting budget is over its build warning" + (summary.empty() ? std::string(".") : ": " + summary), paths,
                        Fix("map_query", {{"what", "lights"}}, "Make lights static, lower their radius or turn some off (RE+ Tools > Lighting Budget)."));
                else if (l.value("over", false))
                    Add("lighting.guideline", "The lighting budget is over its guideline, as every shipped map is" + (summary.empty() ? std::string(".") : ": " + summary), paths);
            }
            void Summary()
            {
                std::string text = std::to_string(spies.size()) + " spy start(s), " + std::to_string(mercs.size()) + " merc start(s), " + std::to_string(missions.size()) + " mission(s), " +
                                   std::to_string(objectives.size()) + " objective(s), " + std::to_string(triggers.size()) + " objective trigger(s), " + std::to_string(disks.size()) + " disk(s), " +
                                   std::to_string(cams.size()) + " CamNet camera(s), " + std::to_string(alarms.size()) + " alarm(s), " + std::to_string(detectors.size()) + " detector(s), " +
                                   std::to_string(GeList(scene.ges).size()) + " GE(s), " + std::to_string(pieces.size()) + " Map Design piece(s); " + std::to_string(scene.actors.size()) + " actors checked";
                std::string modes;
                for (const auto& m : requested) modes += (modes.empty() ? "" : ", ") + m;
                Add("info.summary", text + (scene.live ? " in the open map" : " in the spec") + "; modes asked for: " + modes + ".", {});
            }
            void Run()
            {
                Spawns();
                Story();
                Modes();
                Objectives();
                DiskHunt();
                Deathmatch();
                PlayerCounts();
                CamNet();
                Security();
                Zones();
                Actors();
                Level();
                Documents();
                SettingsRules();
                Release();
                Ges();
                Design();
                MapCheck();
                Lighting();
                Summary();
            }
        };
    }

    // Every finding for the scene, errors first. profile is draft (a map in
    // progress) or release (ready to share); the catalogue gives each rule's
    // severity in both.
    inline std::vector<Finding> Check(const Scene& scene, const std::string& profile)
    {
        if (profile != "draft" && profile != "release") throw std::runtime_error("profile is draft or release.");
        Detail::Checker checker(scene, profile);
        checker.Run();
        auto rank = [](const std::string& severity) { return severity == "error" ? 0 : severity == "warning" ? 1 : 2; };
        std::stable_sort(checker.out.begin(), checker.out.end(), [&](const Finding& a, const Finding& b) { return rank(a.severity) < rank(b.severity); });
        return std::move(checker.out);
    }
    inline Json ToJson(const Finding& f)
    {
        Json out{{"severity", f.severity}, {"rule", f.rule}, {"wikiPage", f.wikiPage}, {"message", f.message}, {"actors", f.actors}};
        if (!f.fix.is_null()) out["fix"] = f.fix;
        return out;
    }
    inline Json Summary(const std::vector<Finding>& findings)
    {
        size_t errors = 0, warnings = 0, info = 0;
        for (const auto& f : findings) (f.severity == "error" ? errors : f.severity == "warning" ? warnings : info) += 1;
        return {{"errors", errors}, {"warnings", warnings}, {"info", info}};
    }

    // scct://rules: the catalogue as the agent reads it.
    inline std::string CatalogMarkdown()
    {
        struct Area { const char* prefix; const char* title; };
        static const Area areas[] = {{"spawns.", "Spawns"}, {"layout.", "Layout"}, {"story.", "Story"}, {"modes.", "Game modes"}, {"objectives.", "Objectives"},
                                     {"diskhunt.", "Disk Hunt"}, {"deathmatch.", "Deathmatch"}, {"objectiveplayers.", "Objective player counts"}, {"camnet.", "Camera network (CamNet)"},
                                     {"ingredients.", "Level ingredients"}, {"security.", "Security"}, {"zones.", "Zones and volumes"}, {"lighting.", "Lighting"},
                                     {"objects.", "Gameplay objects"}, {"softbody.", "SoftBody actors"}, {"movers.", "Movers"}, {"fish.", "Fish"}, {"tutorials.", "Map tutorials"},
                                     {"events.", "Events"}, {"level.", "Level properties"}, {"authoring.", "Change documents"}, {"settings.", "Map Settings"},
                                     {"interface.", "Interface textures"}, {"release.", "Release"}, {"ge.", "Geometric events"}, {"design.", "Map Design check"},
                                     {"mapcheck.", "Map Check"}, {"info.", "Overview"}};
        std::string md =
            "# Validation rules\n\n"
            "map_validate checks the open map (or a level spec, offline) against these rules. Each finding has a severity, the rule id below, "
            "the wiki page it comes from (read it as scct://wiki/<page>), the actors it concerns and, where one exists, a fix: the MCP tool and "
            "arguments that repair it.\n\n"
            "Profiles: **draft** for a map in progress, **release** before sharing it. Severities per profile are listed as draft / release; "
            "\"reported\" keeps the severity of the check the rule folds in. map_validate's include list switches groups: wiki (most rules), "
            "design (Map Design pieces), security, mapcheck, lighting, release (Map Settings, interface textures, packages).\n\n"
            "Distances are in Unreal units (about 1 cm each); Z is up.\n";
        std::set<std::string> shown;
        for (const auto& area : areas)
        {
            std::string rows;
            for (const auto& rule : Catalog())
                if (rule.id.rfind(area.prefix, 0) == 0 && shown.insert(rule.id).second)
                    rows += "| `" + rule.id + "` | " + rule.draft + " / " + rule.release + " | " + rule.wikiPage + " | " + rule.description + " |\n";
            if (!rows.empty()) md += std::string("\n## ") + area.title + "\n\n| Rule | Draft / release | Wiki page | What it checks |\n|---|---|---|---|\n" + rows;
        }
        std::string rest;
        for (const auto& rule : Catalog())
            if (!shown.count(rule.id)) rest += "| `" + rule.id + "` | " + rule.draft + " / " + rule.release + " | " + rule.wikiPage + " | " + rule.description + " |\n";
        if (!rest.empty()) md += "\n## Other\n\n| Rule | Draft / release | Wiki page | What it checks |\n|---|---|---|---|\n" + rest;
        md += "\n## Geometric event measurements\n\n"
              "Ledge grab: 208 minimum, 320 standing, 448 with a running jump, 512 co-op (144 units of platform). Climbable obstacle at least 115; "
              "64 from ledge to wall to pull up; ledges at least 32 long. Fence 88-128 high (96 recommended) with a bClimbable texture. "
              "Hand-over-hand: 216 clearance. Ladder: an even number of bars, at least 8 (RE+ counts one bar per 32 units unless the GE says otherwise). "
              "Zip line: radius 2, starts at most 384 above the floor, 198 clearance below (161 tucked). Pipes and ladders start at the lower end, "
              "their side point at that height.\n";
        return md;
    }
}
