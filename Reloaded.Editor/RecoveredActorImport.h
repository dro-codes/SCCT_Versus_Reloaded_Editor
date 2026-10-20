#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Pure text preparation. No editor state or package data is touched here.
namespace RecoveredActorImport
{
    struct ExternalizedAsset
    {
        std::string className;
        std::string originalPath;
        std::string externalPath;
    };

    struct PreparedMap
    {
        std::string actorsT3d;
        // Native full exports carry ledges/pipes in a top-level GE block.
        std::string mapSectionsT3d;
        std::string levelInfoActor;
        std::string levelInfoName;
        std::size_t actorCount = 0;
        std::size_t removedBrushCount = 0;
        std::size_t removedRuntimePropertyCount = 0;
        std::size_t skippedXboxActorCount = 0;
        std::size_t keptXboxActorCount = 0;
        std::size_t correctedPcActorPlatformCount = 0;
        std::size_t clearedXboxActorReferenceCount = 0;
        std::size_t clearedDeletedActorReferenceCount = 0;
        std::vector<std::string> unsupportedReferences;
        // For the recovery report: actors left out, and kept actors that lost
        // a reference to an excluded or deleted actor.
        std::vector<std::string> skippedXboxActorNames;
        // keepXboxOnlyActors: the Xbox-only actors kept as common content.
        std::vector<std::string> keptXboxActorNames;
        std::vector<std::string> clearedReferenceActorNames;
        std::vector<ExternalizedAsset> externalizedAssets;
        // Native procedural strip doors and patches rebuild their level-owned simulation
        // from actor settings. Callers must also verify their native topology.
        std::vector<std::string> regeneratedSoftBodies;
    };

    // Targets PC: native PC runtime actor membership overrides stale platform
    // labels. Confirmed PC actors labelled Xbox-only use the common default; without
    // that evidence Xbox-only actors and references to them are excluded.
    // Also removes exact structural Brush actors and transient actor fields.
    // Nested objects/volume brushes and authored actor relationships survive.
    // Inline particle bindings use native map-root names, with deterministic
    // disambiguation. Whitespace in typed asset paths uses native import quoting.
    // Optional dependency package maps missing known asset types to an
    // external copy of the original logical package. Missing actors still fail.
    // A native caller may supply actor paths proven deleted and absent from all
    // level actor arrays; only exact typed references to those paths are cleared.
    // keepXboxOnlyActors keeps every other Xbox-only actor too, as common
    // content (its root platform label removed, so the PC game loads it), and
    // references to it stay; its LevelInfo must still not be Xbox-only.
    bool Prepare(std::string_view exportedMap, PreparedMap& prepared,
                 std::string& error, std::string_view externalAssetPackage = {},
                 const std::vector<std::string>& confirmedDeletedActorPaths = {},
                 const std::vector<std::string>& confirmedPcActorPaths = {},
                 bool keepXboxOnlyActors = false);

    // freshMap must be the complete MAP EXPORT of a fresh normal MAP NEW.
    // Keeps its builder brush and a canonical LevelInfo0, applies recovered level
    // settings, and combines recovered actors with reconstructed geometry.
    // Generated actor/model names are disambiguated without renaming authored objects.
    // The result is a complete map for MAP IMPORT, not MAP IMPORTADD.
    bool ComposeSourceMap(const PreparedMap& prepared,
                          std::string_view freshMap,
                          std::string_view geometryMap,
                          std::string& completeMap, std::string& error);

    // Verify a native export after source import/build/ordinary reload. The
    // engine's command return value alone does not prove actors were restored.
    bool VerifySourceMap(const PreparedMap& prepared,
                         std::string_view actualMap, std::string& error);
}
