#pragma once
// Linked prefabs: an Actor Assembly placed with Place Linked stays linked to
// its library entry. Each placed copy (an instance) is
//  - a Group on its actors, Prefab_<assembly>_<8 hex of the instance id>: the
//    Group field is saved in the map, so the link survives save and reopen
//    (editor-hidden flags would not), and
//  - a record in the map's "instances" list in the RE+ library, with
//    "linked": true, the group, the pose it was placed at and the local pose of
//    each member in the assembly ("locals", by name without the placement
//    prefix), which is how the instance's current pose is found again after
//    the user moves it.
// Update Prefab from This Instance captures one instance as the assembly's
// content and rebuilds every other instance of it where it now stands. This
// header is the pure part: names, groups, records and the pose model. No
// Windows, no engine.
#include "WorkflowModel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Workflow::Prefab
{
    // The comma list a Group field holds, trimmed, without None or repeats.
    inline std::vector<std::string> SplitGroups(const std::string& groups)
    {
        std::vector<std::string> result; std::istringstream text(groups); std::string name;
        while (std::getline(text, name, ','))
        {
            const auto begin = name.find_first_not_of(" \t\""), end = name.find_last_not_of(" \t\"");
            if (begin == std::string::npos) continue;
            name = name.substr(begin, end - begin + 1);
            if (Fold(name) == "none") continue;
            if (std::none_of(result.begin(), result.end(), [&](const std::string& other) { return Fold(other) == Fold(name); })) result.push_back(name);
        }
        return result;
    }
    inline std::string JoinGroups(const std::vector<std::string>& names)
    {
        if (names.empty()) return "None";
        std::string result; for (const auto& name : names) result += (result.empty() ? "" : ",") + name; return result;
    }
    // Prefab_<up to 20 letters, digits or underscores of the name>_<8 hex>: short
    // enough to share the 63-character Group name with a few of the user's own.
    inline std::string GroupName(const std::string& assemblyName, const std::string& instanceId)
    {
        if (instanceId.size() < 8 || !std::all_of(instanceId.begin(), instanceId.begin() + 8, [](unsigned char c) { return std::isxdigit(c) != 0; }))
            throw std::runtime_error("Invalid prefab instance id.");
        std::string stem;
        for (unsigned char c : assemblyName)
        {
            if (stem.size() >= 20) break;
            if (std::isalnum(c)) stem += static_cast<char>(c);
            else if (!stem.empty() && stem.back() != '_') stem += '_';
        }
        while (!stem.empty() && stem.back() == '_') stem.pop_back();
        if (stem.empty()) stem = "Assembly";
        return "Prefab_" + stem + "_" + Fold(instanceId.substr(0, 8));
    }
    inline bool IsInstanceGroup(const std::string& name)
    {
        static const std::regex pattern("Prefab_[A-Za-z0-9_]*_[0-9a-fA-F]{8}", std::regex::icase);
        return std::regex_match(name, pattern);
    }
    inline std::vector<std::string> InstanceGroups(const std::string& groups)
    {
        std::vector<std::string> result;
        for (const auto& name : SplitGroups(groups)) if (IsInstanceGroup(name)) result.push_back(name);
        return result;
    }
    inline std::string WithoutInstanceGroups(const std::string& groups)
    {
        auto names = SplitGroups(groups);
        names.erase(std::remove_if(names.begin(), names.end(), IsInstanceGroup), names.end());
        return JoinGroups(names);
    }
    inline std::string WithGroup(const std::string& groups, const std::string& name)
    {
        auto names = SplitGroups(groups);
        if (std::none_of(names.begin(), names.end(), [&](const std::string& other) { return Fold(other) == Fold(name); })) names.push_back(name);
        return JoinGroups(names);
    }
    inline std::string WithoutGroup(const std::string& groups, const std::string& name)
    {
        auto names = SplitGroups(groups);
        names.erase(std::remove_if(names.begin(), names.end(), [&](const std::string& other) { return Fold(other) == Fold(name); }), names.end());
        return JoinGroups(names);
    }
    // An assembly's actor text keeps the user's own groups and none of an
    // instance's: a rebuilt or newly placed copy gets its own.
    inline Json WithoutInstanceGroupsInText(Json definition)
    {
        for (auto& actor : definition.at("actors"))
        {
            auto text = actor.at("text").get<std::string>();
            const auto value = Property(text, "Group");
            if (value.empty()) continue;
            const auto kept = WithoutInstanceGroups(value);
            if (Fold(kept) == "none") text = RemoveProperty(text, "Group");
            else if (kept != value) text = SetProperty(text, "Group", "\"" + kept + "\"");
            actor["text"] = text;
        }
        return definition;
    }
    // A member's name in the assembly: its placed name without the RE_<12 hex>_
    // prefixes placement adds.
    inline std::string CanonicalName(std::string name)
    {
        static const std::regex prefix("^RE_[0-9a-fA-F]{12}_");
        while (std::regex_search(name, prefix)) name.erase(0, 16);
        return name;
    }

    // ---- poses ----------------------------------------------------------------------------------
    // A member placed at `local` in the assembly by an instance at `frame`.
    inline Pose Placed(const Pose& frame, const Pose& local)
    {
        return { TransformPoint(local.position, frame), TransformRotation(local.rotation, frame.rotation) };
    }
    // The instance frame that puts a member whose assembly pose is `local` at `world`.
    inline Pose FrameFromMember(const Pose& local, const Pose& world)
    {
        const Rotation inverse = TransformRotation({ 0, 0, 0 }, local.rotation, true);
        Pose frame; frame.rotation = TransformRotation(inverse, world.rotation);
        const auto offset = TransformPoint(local.position, { {}, frame.rotation });
        for (int i = 0; i < 3; ++i) frame.position[i] = world.position[i] - offset[i];
        return frame;
    }
    // Within half a unit, and turning two 100-unit arms to within half a unit
    // of each other (about a third of a degree), so equal rotators spelt
    // differently (pitch at a pole) still compare equal.
    inline bool SamePose(const Pose& a, const Pose& b, double tolerance = 0.5)
    {
        auto within = [&](const Vector& x, const Vector& y) { double d = 0; for (int i = 0; i < 3; ++i) d += (x[i] - y[i]) * (x[i] - y[i]); return std::sqrt(d) <= tolerance; };
        if (!within(a.position, b.position)) return false;
        for (const Vector arm : { Vector{ 100, 0, 0 }, Vector{ 0, 100, 0 } })
            if (!within(TransformPoint(arm, { {}, a.rotation }), TransformPoint(arm, { {}, b.rotation }))) return false;
        return true;
    }
    inline Pose RecordPose(const Json& record)
    {
        return { record.at("position").get<Vector>(), record.at("rotation").get<Rotation>() };
    }
    // The members' assembly poses, by folded name without the placement prefix.
    inline Json Locals(const Json& definition)
    {
        Json locals = Json::object();
        for (const auto& actor : definition.at("actors"))
            locals[Fold(CanonicalName(actor.at("name").get<std::string>()))] = { { "position", actor.at("position") }, { "rotation", actor.at("rotation") } };
        return locals;
    }
    struct MemberPose { std::string name; Pose world; };
    // Where an instance stands now. Its recorded frame while any member it
    // knows is still where that frame put it (the others may have been edited);
    // otherwise the instance was moved as a whole, and the frame comes from the
    // first member it knows. A record without local poses keeps its frame.
    inline Pose InstanceFrame(const Json& record, const std::vector<MemberPose>& members)
    {
        const Pose recorded = RecordPose(record);
        if (!record.contains("locals") || !record.at("locals").is_object()) return recorded;
        const auto& locals = record.at("locals");
        std::optional<Pose> first;
        for (const auto& member : members)
        {
            const auto key = Fold(CanonicalName(member.name));
            if (!locals.contains(key)) continue;
            const Pose local{ locals.at(key).at("position").get<Vector>(), locals.at(key).at("rotation").get<Rotation>() };
            if (SamePose(Placed(recorded, local), member.world)) return recorded;
            if (!first) first = FrameFromMember(local, member.world);
        }
        return first ? *first : recorded;
    }

    // ---- records --------------------------------------------------------------------------------
    inline bool Linked(const Json& record)
    {
        return record.is_object() && record.value("linked", false) && record.contains("group") && record.at("group").is_string();
    }
    inline std::string AssemblyOf(const Json& record) { return record.value("assembly", std::string{}); }
    // A record for a copy placed by PlaceAssembly ({id, assembly, members,
    // names, tags, position, rotation}) in `group`, from `definition`.
    inline Json NewRecord(Json placed, const std::string& group, const Json& definition)
    {
        placed["linked"] = true; placed["group"] = group; placed["locals"] = Locals(definition);
        return placed;
    }
    // The same record after the instance was rebuilt from `definition`: its
    // id, assembly and group stay; members, names, tags and pose are the new copy's.
    inline Json Rebuilt(Json record, const Json& placed, const Json& definition)
    {
        for (const char* key : { "members", "names", "tags", "position", "rotation" }) if (placed.contains(key)) record[key] = placed.at(key);
        record["locals"] = Locals(definition);
        return record;
    }
    inline std::vector<Json> LinkedOf(const Json& instances, const std::string& assembly)
    {
        std::vector<Json> result;
        if (instances.is_array()) for (const auto& record : instances) if (Linked(record) && AssemblyOf(record) == assembly) result.push_back(record);
        return result;
    }
    // A record by group, compared without case as the editor compares names.
    inline const Json* ByGroup(const Json& instances, const std::string& group)
    {
        if (instances.is_array()) for (const auto& record : instances) if (Linked(record) && Fold(record.at("group").get<std::string>()) == Fold(group)) return &record;
        return nullptr;
    }
    // The linked records the given Group fields name, each once, in order.
    inline std::vector<Json> RecordsNamed(const Json& instances, const std::vector<std::string>& groupFields)
    {
        std::vector<Json> result;
        for (const auto& field : groupFields)
            for (const auto& group : InstanceGroups(field))
                if (const auto* record = ByGroup(instances, group))
                    if (std::none_of(result.begin(), result.end(), [&](const Json& r) { return r.at("id") == record->at("id"); })) result.push_back(*record);
        return result;
    }
    inline Json WithoutRecord(const Json& instances, const std::string& id)
    {
        Json result = Json::array();
        for (const auto& record : instances) if (record.value("id", std::string{}) != id) result.push_back(record);
        return result;
    }
    inline void Replace(Json& instances, const Json& record)
    {
        for (auto& r : instances) if (r.value("id", std::string{}) == record.at("id").get<std::string>()) { r = record; return; }
        throw std::runtime_error("That linked instance is no longer recorded.");
    }
    // Brushes need a geometry build before their new shape shows.
    inline bool HasBrushes(const Json& definition)
    {
        for (const auto& actor : definition.at("actors"))
        {
            const auto type = Fold(actor.value("class", std::string{}));
            if (type == "engine.brush" || type.find("volume") != std::string::npos || type.find("mover") != std::string::npos) return true;
            if (Fold(actor.value("text", std::string{})).find("begin brush") != std::string::npos) return true;
        }
        return false;
    }
    // "3 linked instances" for the Actor Assemblies list ("" for none).
    inline std::string CountText(size_t count)
    {
        if (!count) return {};
        return std::to_string(count) + (count == 1 ? " linked instance" : " linked instances");
    }
}
