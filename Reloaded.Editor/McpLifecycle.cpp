#include "pch.h"
#undef min
#undef max
#include "McpOps.h"
#include "McpBridge.h"
#include "McpBridgeModel.h"
#include "McpSafetyModel.h"
#include "McpEditor.h"
#include "WorkflowEditor.h"
#include "WorkflowTools.h"
#include "EditorExtras.h"
#include "RebuildAllMaps.h"
#include "LevelSnapshot.h"
#include "MapCheck.h"
#include "MapCheckModel.h"
#include "LightingBudget.h"
#include "MinimapWindow.h"
#include "SoundBrowser.h"
#include "LightmapFix.h"
#include "MapAuthoringModel.h"
#include "SaveBackupModel.h"
#include "CrashRecoveryModel.h"
#include "UnsavedChangesModel.h"
#include "logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// The ops of McpOps.h that open, save and build maps, take pictures of the
// viewports, run the checks and write the map's packages: what the File and
// Build menus, Level Snapshot, Map Check, Lighting Budget, Generate Minimap,
// the Static Mesh Browser's import and the Sound Browser's Build UAS do,
// without their dialogs, and what Map Design keeps of the map. Every handler
// runs on the editor's UI thread with the whole request, {"op", ...args}.
namespace McpOps
{
namespace
{
    namespace Editor = Workflow::Editor;
    namespace Bridge = Mcp::Bridge;
    namespace Safety = Mcp::Safety;
    namespace fs = std::filesystem;

    [[noreturn]] void Invalid(const std::string& message) { throw Bridge::Error(Bridge::Code::Invalid, message); }
    [[noreturn]] void Refuse(const std::string& message, const std::string& reason, const Json& details = Json())
    {
        throw Bridge::Error(Bridge::Code::Refused, message, reason, details);
    }
    // What the editor cannot do over the bridge yet: the steps the user takes
    // instead, and what the agent asked for, so it can hand both on.
    [[noreturn]] void Unsupported(const std::string& message, const Json& steps, const Json& wanted = Json())
    {
        Json details{{"steps", steps}};
        if (!wanted.is_null()) details["wanted"] = wanted;
        std::string text = message + " Do it in the editor:";
        int n = 0;
        for (const auto& step : steps) text += " " + std::to_string(++n) + ". " + step.get<std::string>();
        Refuse(text, "unsupported", details);
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }
    bool Same(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }

    // Engine text is ANSI; JSON must be UTF-8 or the reply cannot be written.
    bool ValidUtf8(const std::string& text)
    {
        for (size_t i = 0; i < text.size();)
        {
            const auto c = static_cast<unsigned char>(text[i]);
            const size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            if (!length || (length > 1 && c < 0xC2) || i + length > text.size()) return false;
            for (size_t k = 1; k < length; ++k)
                if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
            i += length;
        }
        return true;
    }
    std::string Utf8(const std::string& text)
    {
        if (ValidUtf8(text)) return text;
        const int wide = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (wide <= 0) return MapCheck::Utf8(text);
        std::wstring buffer(static_cast<size_t>(wide), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), buffer.data(), wide);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) return MapCheck::Utf8(text);
        std::string out(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, out.data(), bytes, nullptr, nullptr);
        return out;
    }
    void Clean(Json& value)
    {
        if (value.is_string())
        {
            const auto& text = value.get_ref<const std::string&>();
            if (!ValidUtf8(text)) value = Utf8(text);
        }
        else if (value.is_array()) for (auto& item : value) Clean(item);
        else if (value.is_object())
        {
            bool keys = true;
            for (auto it = value.begin(); it != value.end(); ++it) { Clean(it.value()); if (!ValidUtf8(it.key())) keys = false; }
            if (!keys)
            {
                Json fixed = Json::object();
                for (auto it = value.begin(); it != value.end(); ++it) fixed[Utf8(it.key())] = std::move(it.value());
                value = std::move(fixed);
            }
        }
    }
    // Paths come from the engine and std::filesystem in the ANSI code page.
    std::string Text(const fs::path& path)
    {
        try { return Utf8(path.string()); }
        catch (const std::exception&) { return Workflow::Authoring::Utf8Text(path); }
    }

    // Every handler's result and message reach the pipe as UTF-8; a dry run
    // of an op that writes files or starts processes changes nothing.
    Handler Wrap(const char* op, Handler run, bool mutates)
    {
        return [op, run = std::move(run), mutates](const Json& request) -> Json
        {
            if (mutates && Current().dryRun)
                return {{"dryRun", true}, {"op", op}, {"note", "Runs on apply; nothing was changed."}};
            try
            {
                Json result = run(request);
                Clean(result);
                return result;
            }
            catch (const Bridge::Error& e) { throw Bridge::Error(e.code, Utf8(e.what()), e.reason, e.details); }
            catch (const nlohmann::json::exception& e) { throw Bridge::Error(Bridge::Code::Invalid, std::string(op) + ": " + Utf8(e.what())); }
            catch (const std::exception& e) { throw std::runtime_error(Utf8(e.what())); }
        };
    }
    // Reads, and ops that only say what the user must do instead, run in a
    // dry run too; the rest answer a dry run without touching anything.
    void Read(const char* op, Handler run) { Add(op, Wrap(op, std::move(run), false)); }
    void Write(const char* op, Handler run) { Add(op, Wrap(op, std::move(run), true)); }

    // editor.state as dll-core answers it, or the little this file can say.
    Json State()
    {
        if (const auto* state = Find("editor.state")) return (*state)({{"op", "editor.state"}});
        Json fallback{{"open", true}};
        try { fallback["mapFile"] = Editor::MapFile(); fallback["generation"] = Editor::MapGeneration(); fallback["dirty"] = EditorExtras::HasUnsavedChanges(); }
        catch (const std::exception&) {}
        return fallback;
    }
    // The workflow library carries a map's entry (Map Design pieces, views,
    // the MCP sidecar) to its new name when it next looks at the map; one of
    // its reads makes that happen now, before another map can be opened.
    void SyncLibrary()
    {
        if (const auto* read = Find("mcp.sidecar.get"))
            try { (*read)({{"op", "mcp.sidecar.get"}}); }
            catch (const std::exception& e) { Logger::log(std::string("MCP: the workflow library did not follow the save: ") + e.what()); }
    }

    fs::path Root() { return Editor::Directory().parent_path().parent_path(); }
    fs::path MapsEd() { return Root() / "Packages" / "MapsEd"; }
    std::string MapStem()
    {
        const auto file = Editor::MapFile();
        return file.empty() ? std::string() : fs::path(file).stem().string();
    }
    // The open map's file name, which its packages are named after.
    std::string NamedMap(const char* what)
    {
        const auto stem = MapStem();
        if (stem.empty())
            Refuse(std::string("Save the map first (map.save with a name such as \"NEWD.sdc\"): ") + what + " is named after the map's file.", "unsaved_map");
        return stem;
    }
    bool Unsaved()
    {
        try { return EditorExtras::HasUnsavedChanges(); }
        catch (const std::exception&) { return false; }
    }
    void RefuseUnsaved(const Json& q, const char* doing)
    {
        if (!q.value("discard", false) && Unsaved())
            Refuse(std::string("The open map has unsaved changes. Save it with map.save, or pass \"discard\": true to ") + doing + " and lose them.", "unsaved_changes");
    }

    // A map file named by a request (UTF-8): a bare name or one relative to
    // the game's folder ("Packages\\MapsEd\\NEWD.sdc") or a full path.
    fs::path MapPath(const std::string& text)
    {
        if (text.empty() || text.size() > 400) Invalid("Name the map file, such as \"NEWD.sdc\" (in Packages\\MapsEd) or a full path.");
        auto path = Workflow::Authoring::Utf8Path(text);
        if (path.is_relative())
        {
            const auto first = path.begin() != path.end() ? Lower(Text(*path.begin())) : std::string();
            path = (first == "packages" ? Root() : MapsEd()) / path;
        }
        return path.lexically_normal();
    }
    // The file as the editor's console is handed it: in full when the engine
    // can read that, else relative to System (the editor's current folder).
    // Checked as UTF-8: the ANSI form of a name the code page cannot hold
    // reads as question marks, which look like plain ASCII.
    std::string EnginePath(const fs::path& full)
    {
        const auto text = Workflow::Authoring::Utf8Text(full);
        if (Bridge::EngineSafePath(text)) return text;
        std::error_code error;
        const auto here = fs::current_path(error);
        if (!error)
        {
            const auto relative = full.lexically_relative(here);
            const auto shorter = Workflow::Authoring::Utf8Text(relative);
            if (!relative.empty() && Bridge::EngineSafePath(shorter) && (here / relative).lexically_normal() == full.lexically_normal()) return shorter;
        }
        Invalid("The editor cannot open " + Text(full) + " by name: its console reads plain ASCII paths without quotes. Use a file in a folder with such a path.");
    }
    // Where map.save may write: a .sdc in Packages\MapsEd, named in ASCII
    // letters, digits and underscores (the map's packages take the name).
    void CheckSaveTarget(const fs::path& file)
    {
        std::error_code error;
        const auto folder = MapsEd();
        if (!fs::is_directory(folder, error)) throw std::runtime_error("The game has no Packages\\MapsEd folder (" + Text(folder) + ").");
        if (!fs::equivalent(file.parent_path(), folder, error))
            Invalid("Maps are saved in Packages\\MapsEd, where the editor and Rebuild All Maps look for them, not " + Text(file.parent_path()) + ".");
        const auto name = Workflow::Authoring::Utf8Text(file.filename());
        const auto stem = Workflow::Authoring::Utf8Text(file.stem());
        if (Lower(Workflow::Authoring::Utf8Text(file.extension())) != ".sdc") Invalid("A map file ends in .sdc: " + name);
        if (stem.empty() || stem.size() > 60 || stem.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
            Invalid("A map's file name is ASCII letters, digits and underscores, with no spaces (its packages and loading screens are named after it; the wiki's convention is four letters and D, such as AquaD): " + name);
        if (Sessions::IsAutosaveName(name) || SaveBackups::IsPlayLevelName(name))
            Invalid(name + " is one of the editor's own files (Auto0-9 autosaves, Autoplay for Play Level); choose another name.");
    }

    // A package, group or asset name as the editor's console reads it.
    void Token(const std::string& value, const std::string& what)
    {
        if (value.empty() || value.size() > 63 || value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
            Invalid(what + " is 1-63 ASCII letters, digits and underscores: \"" + value + "\".");
    }
    // The two kinds of package the bridge imports into (McpSafetyModel.h).
    using Safety::PackageKind;
    constexpr const PackageKind& kTextures = Safety::kTextures;
    constexpr const PackageKind& kMeshes = Safety::kMeshes;

    // Packages\<folder>\<file> as text when it exists, for the package rule.
    std::string ExistingPackageFile(const std::string& folder, const std::string& file)
    {
        std::error_code error;
        const auto path = Root() / "Packages" / folder / file;
        return fs::exists(path, error) ? Text(path) : std::string();
    }
    // A package the bridge may write: the map's own, <Map>_<anything>
    // (Safety::PackageRefusal says why the others are not).
    void CheckPackage(const std::string& package, const std::string& stem, const PackageKind& kind)
    {
        if (const auto refusal = Safety::PackageRefusal(package, stem, kind, ExistingPackageFile)) throw Bridge::Error(refusal.code, refusal.message, refusal.reason);
    }
    // The loaded asset of a kind at exactly this path, or null.
    Json AssetAt(const std::string& path, const char* kind)
    {
        const auto found = Editor::SearchAssets(kind, path, 50);
        const auto& assets = found.is_object() && found.contains("assets") ? found.at("assets") : found;
        if (assets.is_array())
            for (const auto& asset : assets)
                if (asset.is_object() && Same(asset.value("path", std::string()), path)) return asset;
        return Json();
    }
    // Whether the package holds assets of a kind in memory (its file was
    // loaded or something was imported into it).
    bool PackageLoaded(const std::string& package, const char* kind = "material")
    {
        auto found = Editor::SearchAssets(kind, package + ".", 50);
        const auto& assets = found.is_object() && found.contains("assets") ? found.at("assets") : found;
        const auto prefix = Lower(package + ".");
        if (assets.is_array())
            for (const auto& asset : assets)
            {
                const auto path = asset.is_object() ? asset.value("path", std::string()) : asset.is_string() ? asset.get<std::string>() : std::string();
                if (Lower(path).rfind(prefix, 0) == 0) return true;
            }
        return false;
    }
    // Which file package.save writes: the one the package already has, else
    // .usx for a package of static meshes and .utx for one of textures.
    const PackageKind& SaveKind(const std::string& package)
    {
        std::error_code error;
        if (fs::exists(Root() / "Packages" / kTextures.folder / (package + kTextures.extension), error)) return kTextures;
        if (fs::exists(Root() / "Packages" / kMeshes.folder / (package + kMeshes.extension), error)) return kMeshes;
        return PackageLoaded(package, kMeshes.assets) ? kMeshes : kTextures;
    }

    // Damage warnings a map load raises go into the reply, not a message box.
    std::vector<std::string>* collected = nullptr;
    void __cdecl Collect(const char* message)
    {
        try { if (collected && message && *message) collected->push_back(message); }
        catch (...) {}
    }
    struct Collecting
    {
        std::vector<std::string> lines;
        LightmapFix::DamageSink previous;
        Collecting() { collected = &lines; previous = LightmapFix::SetDamageSink(&Collect); }
        ~Collecting() { LightmapFix::SetDamageSink(previous); collected = nullptr; }
        Collecting(const Collecting&) = delete;
        Collecting& operator=(const Collecting&) = delete;
    };

    // ---------------------------------------------------------- map files --

    Json NewMap(const Json& q)
    {
        RefuseUnsaved(q, "start a new map");
        const auto previous = Editor::MapOpened().generation;
        if (!Editor::Exec("MAP NEW")) throw std::runtime_error("The editor refused MAP NEW; the open map is unchanged.");
        if (Editor::MapOpened().generation == previous) throw std::runtime_error("The editor did not start a new map; the open map is unchanged.");
        Editor::SetMapFile("");
        Editor::Redraw();
        return State();
    }

    Json OpenMap(const Json& q)
    {
        const auto file = MapPath(q.at("path").get<std::string>());
        std::error_code error;
        if (!fs::is_regular_file(file, error)) Invalid("There is no map file at " + Text(file) + ".");
        if (Lower(Workflow::Authoring::Utf8Text(file.extension())) != ".sdc") Invalid("The editor opens .sdc map files: " + Text(file.filename()));
        RefuseUnsaved(q, "open another map");
        const auto engine = EnginePath(file);
        std::vector<std::string> warnings;
        {
            Collecting damage;
            EditorExtras::OpenMap(engine, false);
            warnings = damage.lines;
        }
        // The Save target in full, as File > Open leaves it.
        if (engine != Workflow::Authoring::Utf8Text(file)) Editor::SetMapFile(file.string());
        auto state = State();
        if (!warnings.empty()) state["warnings"] = warnings;
        return state;
    }

    Json SaveMap(const Json& q)
    {
        const auto current = Editor::MapFile();
        fs::path target;
        if (q.contains("path") && !q.at("path").is_null()) target = MapPath(q.at("path").get<std::string>());
        else if (current.empty()) Invalid("The map has no file yet: give map.save a path, such as \"NEWD.sdc\" (saved in Packages\\MapsEd).");
        else target = fs::path(current);
        std::error_code error;
        target = fs::weakly_canonical(target, error);
        if (error) target = target.lexically_normal();
        CheckSaveTarget(target);
        const bool sameFile = !current.empty() && UnsavedChanges::SameFile(target.string(), current);
        if (!sameFile && fs::exists(target, error) && !q.value("overwrite", false))
            Refuse(Text(target) + " is another map's file. Pass \"overwrite\": true to save over it (the file it replaces is kept in System\\ReloadedEditor\\Backups).", "exists");
        RebuildAllMaps::SaveMapTo(EnginePath(target));
        if (!sameFile) Editor::SetMapFile(target.string());
        SyncLibrary();
        Json result{{"file", Text(target)}, {"saved", true}};
        if (!sameFile) result["previous"] = current.empty() ? Json() : Json(Utf8(current));
        return result;
    }

    Json BuildAll(const Json&)
    {
        const auto started = std::chrono::steady_clock::now();
        auto log = RebuildAllMaps::BuildCurrentMap();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        Json lines = Json::array();
        for (auto& line : log) lines.push_back(Utf8(line));
        return {{"ok", true}, {"log", lines}, {"seconds", std::round(seconds * 10) / 10}};
    }

    // ---------------------------------------------------------- pictures --

    Json Screenshot(const Json& q)
    {
        const auto kind = q.value("viewport", std::string("perspective"));
        const int maxEdge = q.value("maxEdge", 1024);
        if (maxEdge < 16 || maxEdge > 2048) Invalid("maxEdge is 16 to 2048 pixels.");
        if (const HWND frame = Editor::MainWindow(); frame && IsIconic(frame)) Refuse("The editor is minimised; a screenshot needs its window restored.", "minimised");
        const auto viewport = LevelSnapshot::FindViewport(kind);

        // A camera given in the request: the viewport's camera moves there for
        // the shot and goes back afterwards, as Working Views restore it.
        Json restore;
        if (q.contains("camera") && !q.at("camera").is_null())
        {
            const auto& camera = q.at("camera");
            auto view = Editor::CaptureView();
            Json entry;
            for (const auto& c : view.at("cameras")) if (c.value("slot", -1) == viewport.slot) entry = c;
            if (entry.is_null()) throw std::runtime_error("The " + kind + " viewport's camera could not be read.");
            restore = {{"cameras", Json::array({entry})}, {"actors", Json::array()}};
            auto moved = entry;
            if (camera.contains("location"))
            {
                const auto at = camera.at("location").get<Workflow::Vector>();
                for (double v : at) if (!std::isfinite(v) || std::abs(v) > 1000000) Invalid("camera.location is [x, y, z] inside the world.");
                moved["position"] = at;
            }
            if (camera.contains("rotation")) moved["rotation"] = camera.at("rotation").get<Workflow::Rotation>();
            Editor::RestoreView({{"cameras", Json::array({moved})}, {"actors", Json::array()}});
        }
        Editor::CameraState framed;
        auto putBack = [&]
        {
            try { Editor::RestoreCamera(framed); } catch (const std::exception&) {}
            if (!restore.is_null()) try { Editor::RestoreView(restore); } catch (const std::exception&) {}
        };
        LevelSnapshot::Picture picture;
        try
        {
            if (q.contains("frame") && q.at("frame").is_array() && !q.at("frame").empty())
            {
                RECT r{};
                GetClientRect(viewport.window, &r);
                framed = Editor::FrameCamera(viewport.camera, q.at("frame"), r.bottom > 0 ? static_cast<double>(r.right) / r.bottom : 4.0 / 3);
            }
            picture = LevelSnapshot::ViewportImage(viewport, maxEdge);
        }
        catch (...) { putBack(); throw; }
        putBack();
        return {{"png", Bridge::Base64(picture.png)}, {"mimeType", "image/png"}, {"width", picture.width}, {"height", picture.height}, {"viewport", kind}};
    }

    // ------------------------------------------------- packages and assets --

    Json ImportTexture(const Json& q)
    {
        const auto stem = NamedMap("a texture package");
        const auto package = q.at("package").get<std::string>();
        CheckPackage(package, stem, kTextures);
        const auto fileText = q.at("file").get<std::string>();
        const auto file = Workflow::Authoring::Utf8Path(fileText);
        if (file.is_relative()) Invalid("Give the image's full path: " + fileText);
        if (!Bridge::EngineSafePath(fileText)) Invalid("The editor's importer reads plain ASCII paths without quotes: copy the image to such a path first (" + fileText + ").");
        Json op{{"op", "texture"}, {"package", package}, {"name", q.at("name")}, {"file", fileText}};
        for (const char* key : {"group", "format", "mips", "masked", "alphaTexture", "lodSet"})
            if (q.contains(key) && !q.at(key).is_null()) op[key] = q.at(key);
        Json operations = Json::array();
        // A package with a file that is not in memory is read first, so the
        // save after the import keeps what the file holds.
        std::error_code error;
        const auto existing = Root() / "Packages" / "Textures" / (package + ".utx");
        const bool load = fs::exists(existing, error) && !PackageLoaded(package);
        if (load) operations.push_back({{"op", "load"}, {"package", package}});
        operations.push_back(op);
        const Json document{{"format", "scct.map-changes"}, {"version", 2}, {"map", "*"}, {"description", "MCP asset.texture"}, {"operations", operations}};
        const auto applied = Editor::ApplyMapAuthoring(document);
        return {{"texture", Workflow::Authoring::TexturePath(op)}, {"package", package}, {"loaded", load}, {"done", applied.value("assets", Json::array())},
                {"note", "Imported in memory: save the package with package.save to keep it."}};
    }

    Json SavePackage(const Json& q)
    {
        const auto stem = NamedMap("a package");
        const auto package = q.at("package").get<std::string>();
        Token(package, "A package name");
        const auto& kind = SaveKind(package);
        CheckPackage(package, stem, kind);
        const auto folder = Root() / "Packages" / kind.folder;
        const auto file = folder / (package + kind.extension);
        std::error_code error;
        fs::create_directories(folder, error);
        Json result{{"package", package}, {"file", Text(file)}, {"kind", &kind == &kMeshes ? "staticmesh" : "texture"}};
        if (!PackageLoaded(package, kind.assets))
            Refuse(package + " is not in memory, so there is nothing to save. Import into it with asset.texture or asset.mesh first.", "not_loaded");
        if (fs::exists(file, error))
        {
            if (!q.value("overwrite", false)) Refuse(Text(file) + " exists. Pass \"overwrite\": true to replace it (the old file is backed up first).", "exists");
            const auto backups = Editor::Directory() / "map-json-backups";
            fs::create_directories(backups, error);
            const auto backup = backups / (package + "." + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + kind.extension);
            if (!fs::copy_file(file, backup, fs::copy_options::overwrite_existing, error)) throw std::runtime_error("Cannot back up " + Text(file) + "; it was not replaced.");
            result["backup"] = Text(backup);
        }
        const auto started = fs::file_time_type::clock::now() - std::chrono::seconds(2);
        if (!Editor::Exec("OBJ SAVEPACKAGE PACKAGE=\"" + package + "\" FILE=\"" + EnginePath(file) + "\"") || !fs::exists(file, error) || fs::last_write_time(file, error) < started)
            throw std::runtime_error("The editor did not save " + package + " to " + Text(file) + ".");
        result["size"] = fs::file_size(file, error);
        return result;
    }

    // What an ASCII Scene Export holds: the objects drawn, and the MCD ones
    // (MCDCX_ convex hulls and the like) the importer makes the mesh's
    // collision of instead. Without any, every triangle collides.
    struct AseObjects { size_t visible = 0, collision = 0; };
    AseObjects ReadAse(const fs::path& file)
    {
        std::error_code error;
        const auto size = fs::file_size(file, error);
        if (error) Invalid("Cannot read " + Text(file) + ".");
        if (size > 256u * 1024 * 1024) Invalid(Text(file.filename()) + " is over 256 MiB; split the model before importing it.");
        std::ifstream in(file, std::ios::binary);
        if (!in) Invalid("Cannot read " + Text(file) + ".");
        AseObjects objects;
        bool header = false, pending = false;
        size_t lines = 0;
        for (std::string line; std::getline(in, line); ++lines)
        {
            if (!header)
            {
                if (line.find("*3DSMAX_ASCIIEXPORT") != std::string::npos) header = true;
                else if (lines > 8) break;
                continue;
            }
            if (line.find("*GEOMOBJECT") != std::string::npos) { pending = true; continue; }
            if (!pending) continue;
            const auto at = line.find("*NODE_NAME");
            if (at == std::string::npos) continue;
            const auto open = line.find('"', at), close = open == std::string::npos ? open : line.find('"', open + 1);
            const auto name = close == std::string::npos ? std::string() : line.substr(open + 1, close - open - 1);
            ++(Lower(name).rfind("mcd", 0) == 0 ? objects.collision : objects.visible);
            pending = false;
        }
        if (!header) Invalid(Text(file.filename()) + " is not an ASCII Scene Export (no *3DSMAX_ASCIIEXPORT header). Export the model as ASE, or give assets_import an .obj to convert.");
        return objects;
    }

    // The Static Mesh Browser's Import, through the console command the
    // browser and RE+'s conveyor belts use. A mesh is never replaced: actors
    // that use it would change shape with no Undo.
    Json ImportMesh(const Json& q)
    {
        for (const char* key : {"file", "package", "name"})
            if (!q.contains(key) || !q.at(key).is_string())
                Invalid(std::string("asset.mesh takes file (the .ase's full path), package (<Map>_STM) and name; ") + key + " is missing or not text.");
        if (q.contains("group") && !q.at("group").is_null() && !q.at("group").is_string()) Invalid("asset.mesh: group is text.");
        const auto stem = NamedMap("a static mesh package");
        const auto package = q.at("package").get<std::string>();
        const auto name = q.at("name").get<std::string>();
        const auto group = q.contains("group") && !q.at("group").is_null() ? q.at("group").get<std::string>() : std::string();
        Token(package, "A package name");
        Token(name, "A static mesh name");
        if (!group.empty()) Token(group, "A group name");
        CheckPackage(package, stem, kMeshes);
        const auto fileText = q.at("file").get<std::string>();
        const auto file = Workflow::Authoring::Utf8Path(fileText);
        if (file.is_relative()) Invalid("Give the .ase file's full path: " + fileText);
        if (!Bridge::EngineSafePath(fileText)) Invalid("The editor's importer reads plain ASCII paths without quotes: copy the file to such a path first (" + fileText + ").");
        if (Lower(Workflow::Authoring::Utf8Text(file.extension())) != ".ase")
            Invalid("The editor imports static meshes from ASCII Scene Export (.ase) files, not " + Text(file.filename()) + "; assets_import converts .obj models.");
        std::error_code error;
        if (!fs::is_regular_file(file, error)) Invalid("There is no file at " + fileText + ".");
        const auto objects = ReadAse(file);
        if (!objects.visible)
            Invalid(Text(file.filename()) + " has no geometry to draw" + (objects.collision ? " (only MCD collision objects)." : "."));

        // collision: true (or "hulls") needs the file's own collision
        // objects; false (or "triangles") accepts per-triangle collision.
        std::string collision = "auto";
        if (q.contains("collision") && !q.at("collision").is_null())
        {
            const auto& c = q.at("collision");
            if (c.is_boolean()) collision = c.get<bool>() ? "hulls" : "triangles";
            else if (c.is_string() && (c == "auto" || c == "hulls" || c == "triangles")) collision = c.get<std::string>();
            else Invalid("collision is \"auto\", \"hulls\" (the file must carry MCDCX_ collision objects) or \"triangles\" (every triangle collides).");
        }
        Json warnings = Json::array();
        if (!objects.collision)
        {
            const std::string why = Text(file.filename()) + " has no MCDCX_ collision objects, so every triangle of " + name +
                                    " collides, which the wiki (static-meshes) says is worse for performance. Add convex hulls named MCDCX_" + name + "_1, MCDCX_" + name +
                                    "_2 ... in the modelling tool";
            if (collision == "hulls") Invalid(why + ", then import again.");
            if (collision == "auto") warnings.push_back(why + ", or pass collision \"triangles\" to accept it.");
        }

        // A package with a file that nothing of is in memory is read first,
        // so the mesh joins what the file holds and package.save keeps it.
        const auto existing = Root() / "Packages" / kMeshes.folder / (package + kMeshes.extension);
        bool load = false;
        if (fs::exists(existing, error) && !PackageLoaded(package, kMeshes.assets) && !PackageLoaded(package, kTextures.assets))
        {
            std::string output;
            Editor::Exec("OBJ LOAD FILE=\"" + EnginePath(existing) + "\"", output);
            if (!PackageLoaded(package, kMeshes.assets) && !PackageLoaded(package, kTextures.assets))
                throw std::runtime_error("The editor did not load " + Text(existing) + ", so a mesh imported into " + package + " would replace what the file holds. Open the package in the Static Mesh Browser first.");
            load = true;
        }
        const auto path = package + (group.empty() ? std::string() : "." + group) + "." + name;
        for (const char* kind : {kMeshes.assets, kTextures.assets, "sound"})
            if (!AssetAt(path, kind).is_null())
                Refuse(path + " already exists. Import under a new name: asset.mesh never replaces an asset, as the actors using it would change with no Undo.", "exists", {{"path", path}});

        std::string command = "STATICMESH IMPORT FILE=\"" + fileText + "\" NAME=\"" + name + "\" PACKAGE=\"" + package + "\"";
        if (!group.empty()) command += " GROUP=\"" + group + "\"";
        std::string output;
        const bool accepted = Editor::Exec(command, output);
        const auto mesh = AssetAt(path, kMeshes.assets);
        if (mesh.is_null())
        {
            auto said = output.substr(0, std::min<size_t>(output.size(), 600));
            while (!said.empty() && (said.back() == '\r' || said.back() == '\n' || said.back() == ' ')) said.pop_back();
            throw std::runtime_error("The editor's static mesh importer did not create " + path + " from " + fileText + (accepted ? "" : " (it refused the command)") +
                                     (said.empty() ? std::string(".") : ": " + said));
        }
        return {{"mesh", path}, {"package", package}, {"bounds", mesh.value("bounds", Json())}, {"loaded", load},
                {"collision", {{"objects", objects.collision}, {"mode", objects.collision ? "hulls" : "triangles"}}}, {"objects", objects.visible}, {"warnings", warnings},
                {"note", "Imported in memory: save the package with package.save to keep it (Packages\\StaticMeshes\\" + package + ".usx)."}};
    }

    // What Map Design keeps of the open map in the workflow library: its
    // pieces (design.block / design.blockbatch records) and elements.
    Json DesignPieces(const Json&)
    {
        const auto entry = WorkflowTools::LibraryEntry();
        const auto design = entry.is_object() && entry.contains("design") && entry.at("design").is_object() ? entry.at("design") : Json::object();
        auto list = [&](const char* key) { return design.contains(key) && design.at(key).is_array() ? design.at(key) : Json::array(); };
        Json result{{"pieces", list("pieces")}, {"elements", list("elements")}, {"map", Editor::AuthoringMap()}};
        result["count"] = result["pieces"].size();
        return result;
    }

    Json InterfaceTexture(const Json& q)
    {
        NamedMap("the interface package");
        const auto file = Workflow::Authoring::Utf8Path(q.at("file").get<std::string>());
        return LevelSnapshot::ApplyInterfaceTexture(q.at("slot").get<std::string>(), file);
    }

    Json BuildUas(const Json& q)
    {
        const auto stem = NamedMap("the .uas");
        // Interface and the map's ambient package go in every map's .uas.
        std::vector<std::string> packages{"Interface", "Amb_" + stem};
        if (q.contains("packages"))
            for (const auto& package : q.at("packages"))
            {
                const auto name = package.get<std::string>();
                if (std::none_of(packages.begin(), packages.end(), [&](const std::string& p) { return Same(p, name); })) packages.push_back(name);
            }
        const double quality = q.value("quality", 5.0);
        if (!std::isfinite(quality) || quality < 0 || quality > 10) Invalid("quality is an OGG quality from 0 to 10 (5 is the shipped default).");
        return SoundBrowser::BuildUas(stem, packages, static_cast<float>(quality));
    }

    // --------------------------------------- older ops a bridge call reaches --

    // authoring.apply / authoring.preview, as WorkflowTools' chain runs them
    // (McpOps answers first). Over the pipe a document's texture and save
    // operations write only the map's own packages: its save replaces
    // Packages\Textures\<package>.utx, a stock package's included. A load
    // only reads, so it stays allowed. The preview refuses what the apply
    // would, so a dry run or the approval window says so first.
    Json Authoring(const Json& q, bool apply)
    {
        auto document = q.at("document");
        Workflow::Authoring::Rebase(document, Workflow::Authoring::Utf8Path(q.value("baseDirectory", std::string{})));
        if (Current().call)
            if (const auto refusal = Safety::DocumentRefusal(document, MapStem(), ExistingPackageFile))
                throw Bridge::Error(refusal.code, Utf8(refusal.message), refusal.reason);
        return apply ? Editor::ApplyMapAuthoring(document) : Editor::PreviewMapAuthoring(document);
    }

    // optimise.release over the pipe: the release copy goes only into
    // <Game>\Releases, the work folder or [Mcp] WriteRoots (with overwrite it
    // deletes and rewrites the map's files there), and its asset package is
    // one of the map's own. Optimise Map Assets' window still takes any folder.
    Json Release(const Json& q)
    {
        if (!Current().call) return Editor::OptimiseRelease(q);
        const auto stem = NamedMap("the release copy");
        auto options = q;
        fs::path folder = Editor::ReleaseFolder(stem);
        const auto given = q.contains("folder") && !q.at("folder").is_null() ? q.at("folder").get<std::string>() : std::string();
        if (!given.empty())
        {
            folder = Workflow::Authoring::Utf8Path(given);
            if (folder.is_relative()) Invalid("Give the release folder's full path, such as " + Text(Editor::ReleaseFolder(stem)) + ": " + given);
        }
        std::error_code error;
        auto resolved = fs::weakly_canonical(folder, error);
        if (error) resolved = folder.lexically_normal();
        auto roots = McpBridge::WriteRoots();
        const auto releases = fs::weakly_canonical(Root() / "Releases", error);
        roots.insert(roots.begin(), error ? (Root() / "Releases").lexically_normal() : releases);
        if (!Safety::InsideAny(resolved, roots))
            Refuse("The release copy is written only in the game's Releases folder, the work folder and [Mcp] WriteRoots (" + Safety::RootList(roots) + "), not " + Text(resolved) +
                       ". Leave folder out for " + Text(Editor::ReleaseFolder(stem)) + ", or ask the user to add the folder to WriteRoots in System\\Reloaded_Editor.ini.",
                   "outside_write_roots");
        if (!given.empty())
        {
            // Optimise Map Assets reads the folder in the ANSI code page, as its window gives it.
            std::string ansi;
            try { ansi = resolved.string(); }
            catch (const std::exception&) { ansi.clear(); }
            if (ansi.empty() || fs::path(ansi) != resolved) Invalid("The editor cannot write to " + Text(resolved) + " by name: choose a folder whose path the system's code page can spell.");
            options["folder"] = ansi;
        }
        else options.erase("folder");
        if (q.value("destination", std::string("map")) != "map") CheckPackage(q.value("assetPackage", std::string()), stem, kMeshes);
        return Editor::OptimiseRelease(options);
    }

    // ------------------------------------------------- what is not reachable --

    Json MapSettingsSteps()
    {
        return Json::array({
            "If the map is new, save it (map.save), then exit the editor and open the map again: Map Settings stay unavailable on a new map until then.",
            "Right-click empty space in a viewport (easiest in Wireframe) and choose Map Settings.",
            "Set the values: MapName in capitals; | starts a new line; Modes[i] and ModesStrings[i] index-aligned (GameModeId GM_Multi for Story, GM_DiskHunt, GM_DeathMatch); RespawnTime 5-15, SpiesLives and SentriesLives 0-10, TimeLimit 0-20.",
            "Save the map: the editor writes the settings into Packages\\Textures\\<Map>-i.utc."});
    }
}

void RegisterLifecycle()
{
    // Map files and builds.
    Write("map.new", NewMap);
    Write("map.open", OpenMap);
    Write("map.save", SaveMap);
    Write("build.all", BuildAll);
    Write("editor.quit", [](const Json& q) -> Json
    {
        RefuseUnsaved(q, "quit");
        const HWND frame = Editor::MainWindow();
        if (!frame) throw std::runtime_error("The editor's main window is not available.");
        PostMessage(frame, WM_CLOSE, 0, 0);
        return true;
    });
    Write("play.level", [](const Json&) -> Json
    {
        if (!McpBridge::Current().allowPlay)
            Refuse("Play Level over MCP is off. The user can allow it with [Mcp] AllowPlay=1 in Reloaded_Editor.ini.", "play_off");
        Editor::PlayLevel();
        return true;
    });

    // Pictures and checks.
    Read("view.screenshot", Screenshot);
    Read("mapcheck.run", [](const Json&) -> Json { return MapCheck::ToJson(MapCheck::Measure()); });
    Read("lighting.budget", [](const Json&) -> Json { return LightingBudget::Measure(); });

    Read("design.pieces", DesignPieces);

    // Packages and assets: the map's own only.
    Write("asset.texture", ImportTexture);
    Write("asset.mesh", ImportMesh);
    Write("package.save", SavePackage);
    Write("interface.texture", InterfaceTexture);
    Write("minimap.apply", [](const Json& q) -> Json { return MinimapWindow::Apply(q); });
    Write("uas.build", BuildUas);
    Add("authoring.apply", [](const Json& q) -> Json { return Authoring(q, true); });
    Add("authoring.preview", [](const Json& q) -> Json { return Authoring(q, false); });
    Write("optimise.release", Release);

    // Reflection on objects outside the level is not reachable from here:
    // the editor's property readers only resolve actors in the open level.
    Read("object.inspect", [](const Json& q) -> Json
    {
        Unsupported("object.inspect cannot read objects outside the level yet (" + q.value("path", std::string("no path")) + "); actors are read with actors.query or settings.inspect.",
                    Json::array({"Find the object in its browser (Texture, Static Mesh or Sound Browser).", "Right-click it and choose Properties to read its values."}));
    });
    Read("object.set", [](const Json& q) -> Json
    {
        Unsupported("object.set cannot write objects outside the level yet; actors are edited with authoring.apply.",
                    Json::array({"Find the object in its browser and open its Properties.", "Set the values listed under wanted.",
                                 "Save its package with package.save (only the map's own packages, never a stock one)."}),
                    {{"path", q.value("path", std::string())}, {"properties", q.value("properties", Json::object())}});
    });
    Read("mapsettings.get", [](const Json&) -> Json
    {
        auto result = LevelSnapshot::DescribeInterfacePackage();
        result["values"] = nullptr;
        result["editable"] = false;
        result["unavailable"] = "The SMapSettings values (MapName, briefs, legends, Modes, ModesStrings) cannot be read over the bridge yet; the editor shows them in Map Settings.";
        result["steps"] = MapSettingsSteps();
        return result;
    });
    Read("mapsettings.set", [](const Json& q) -> Json
    {
        Unsupported("mapsettings.set cannot write SMapSettings yet: the map's settings object is not reachable from the bridge, and setting it by class would change every map's.",
                    MapSettingsSteps(), q.value("changes", Json::object()));
    });
}
}
