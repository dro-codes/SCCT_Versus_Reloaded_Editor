#pragma once
#include "Include/nlohmann/json.hpp"
#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Engine-independent document, identity and placement logic.
namespace Workflow
{
    using Json = nlohmann::json;
    using Vector = std::array<double, 3>;
    using Rotation = std::array<int, 3>; // Unreal pitch/yaw/roll (65536 units/turn).
    struct Pose { Vector position{}; Rotation rotation{}; };
    struct ActorText { std::string name, type, text; };
    struct Reference { std::string type, path; size_t begin{}, end{}; };
    std::string Fold(std::string value);
    // Objects by path, compared without case as a search of the map does. The
    // first of two paths that differ only in case is kept, which is the one a
    // search in list order would find.
    template<class Handle> class PathIndex
    {
    public:
        void Add(const std::string& path, Handle h) { entries.emplace(Fold(path), h); }
        const Handle* Find(const std::string& path) const
        {
            if (path.empty()) return nullptr;
            const auto key = Fold(path);
            if (key == "none") return nullptr;
            const auto at = entries.find(key);
            return at == entries.end() ? nullptr : &at->second;
        }
        size_t size() const { return entries.size(); }
    private:
        std::unordered_map<std::string, Handle> entries;
    };
    // Exclusions identify exact preview rows; never accept replacement changes.
    Json SelectedTagChanges(const Json& preview);
    std::string Id();
    std::string Timestamp();
    Json ReadDocument(const std::filesystem::path& path, const Json& empty);
    void WriteDocument(const std::filesystem::path& path, const Json& document);
    void UpdateEntry(Json& entries, const std::string& id, Json replacement);
    std::vector<ActorText> ParseActors(const std::string& t3d);
    std::string NormalizeExportNames(const std::string& text);
    std::vector<Reference> References(const std::string& text);
    std::string RewriteReferences(const std::string& text,
                                  const std::map<std::string, std::string>& paths);
    std::string Property(const std::string& text, const std::string& key);
    std::string SetProperty(std::string text, const std::string& key, const std::string& value);
    std::string RemoveProperty(const std::string& text, const std::string& key);
    // Actor text without any property whose value is a StaticMeshInstance
    // (a mesh actor's baked lighting in the map it was copied from), at any
    // depth and under any property name. dropped receives the paths named.
    std::string RemoveBakedLighting(const std::string& text, std::vector<std::string>* dropped = nullptr);
    std::string RenameObjects(const std::string& text, const std::string& prefix);
    // Names of the Begin Object / Begin Brush blocks nested in actor text: the
    // inline sub-objects (particle emitters, brush models) native copy exports.
    std::vector<std::string> InlineObjectNames(const std::string& text);
    Json CanonicalizeAssembly(Json definition, const std::map<std::string,std::string>& memberNames);
    Vector TransformPoint(const Vector& point, const Pose& frame, bool inverse = false);
    Rotation TransformRotation(const Rotation& rotation, const Rotation& frame, bool inverse = false);
    std::string VectorText(const Vector& v);
    std::string RotationText(const Rotation& v);
    // Generates a copy; saved definitions are never mutated by placement.
    Json PreparePlacement(const Json& definition, const Pose& frame,
                          const std::string& prefix, const std::string& levelPath,
                          const std::map<std::string, std::string>& bindings);
    // The definition's dependencies that placement must find or load: its own
    // inline sub-objects, which older captures listed, are recreated by paste.
    Json PlacementDependencies(const Json& definition);
}
