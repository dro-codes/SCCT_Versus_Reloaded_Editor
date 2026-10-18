#include "pch.h"
#undef min
#undef max
#include "McpOps.h"
#include "McpBridge.h"
#include "McpBridgeModel.h"
#include "McpEditor.h"
#include "WorkflowEditor.h"
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
#include <stdexcept>
#include <string>
#include <vector>

// The ops of McpOps.h that open, save and build maps, take pictures of the
// viewports, run the checks and write the map's packages: what the File and
// Build menus, Level Snapshot, Map Check, Lighting Budget, Generate Minimap
// and the Sound Browser's Build UAS do, without their dialogs. Every handler
// runs on the editor's UI thread with the whole request, {"op", ...args}.
namespace McpOps
{
namespace
{
    namespace Editor = Workflow::Editor;
    namespace Bridge = Mcp::Bridge;
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

    // A texture package the bridge may write: the map's own, named
    // <Map>_<anything> as the wiki's Map Packaging page has it (<Map>_TXT).
    // <Map> itself would clash with the map's package, <Map>-i is written by
    // interface.texture, and Amb_<Map> holds the map's sounds.
    void CheckTexturePackage(const std::string& package, const std::string& stem)
    {
        if (Same(package, stem + "-i")) Refuse("The interface package is written by interface.texture, through the map save.", "interface_package");
        Workflow::Authoring::AssetName(package, "Package");
        if (!Bridge::MapOwnedPackage(package, stem))
            Refuse("Only the map's own packages are written over the bridge, never a stock one: name it " + stem + "_TXT (or " + stem + "_<anything>).", "not_map_owned");
        if (Lower(package).rfind(Lower(stem) + "_", 0) != 0)
            Refuse("Textures go in " + stem + "_TXT (or " + stem + "_<anything>): a package named " + package + " would clash with the map's own or its sounds, as packages of one base name are one package to the engine.", "package_name");
        for (const auto& [folder, extension] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"StaticMeshes", ".usx"}, {"Sounds", ".uax"}, {"Animations", ".ukx"}, {"Maps", ".sdc"}, {"MapsEd", ".sdc"}})
        {
            std::error_code error;
            const auto other = Root() / "Packages" / folder / (package + extension);
            if (fs::exists(other, error))
                Refuse(Text(other) + " already has the base name " + package + "; packages of one base name are one package to the engine. Choose another name.", "package_name");
        }
    }
    // Whether anything of the package is in memory (its file was loaded or
    // something was imported into it).
    bool PackageLoaded(const std::string& package)
    {
        auto found = Editor::SearchAssets("material", package + ".", 50);
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
        CheckTexturePackage(package, stem);
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
        const auto stem = NamedMap("a texture package");
        const auto package = q.at("package").get<std::string>();
        CheckTexturePackage(package, stem);
        const auto folder = Root() / "Packages" / "Textures";
        const auto file = folder / (package + ".utx");
        std::error_code error;
        fs::create_directories(folder, error);
        Json result{{"package", package}, {"file", Text(file)}};
        if (!PackageLoaded(package)) Refuse(package + " is not in memory, so there is nothing to save. Import a texture into it with asset.texture first.", "not_loaded");
        if (fs::exists(file, error))
        {
            if (!q.value("overwrite", false)) Refuse(Text(file) + " exists. Pass \"overwrite\": true to replace it (the old file is backed up first).", "exists");
            const auto backups = Editor::Directory() / "map-json-backups";
            fs::create_directories(backups, error);
            const auto backup = backups / (package + "." + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".utx");
            if (!fs::copy_file(file, backup, fs::copy_options::overwrite_existing, error)) throw std::runtime_error("Cannot back up " + Text(file) + "; it was not replaced.");
            result["backup"] = Text(backup);
        }
        const auto started = fs::file_time_type::clock::now() - std::chrono::seconds(2);
        if (!Editor::Exec("OBJ SAVEPACKAGE PACKAGE=\"" + package + "\" FILE=\"" + EnginePath(file) + "\"") || !fs::exists(file, error) || fs::last_write_time(file, error) < started)
            throw std::runtime_error("The editor did not save " + package + " to " + Text(file) + ".");
        result["size"] = fs::file_size(file, error);
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

    // Packages and assets: the map's own only.
    Write("asset.texture", ImportTexture);
    Write("package.save", SavePackage);
    Write("interface.texture", InterfaceTexture);
    Write("minimap.apply", [](const Json& q) -> Json { return MinimapWindow::Apply(q); });
    Write("uas.build", BuildUas);

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
