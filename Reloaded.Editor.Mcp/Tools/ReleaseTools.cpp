// The release tool: a finished map checked and packaged the way the wiki's
// Map Packaging, Sound Browser and Map Settings pages require. The files are
// read here, in the game folder beside the exe (System's parent), so the
// check also works with no editor running; the editor builds the .uas, the
// optimised release copy and the interface textures (uas.build,
// optimise.report / optimise.release, interface.texture).
#include "Common.h"
#include "../ImageCodec.h"
#include "../../Reloaded.Editor/LevelSnapshotModel.h"
#include "../../Reloaded.Editor/McpSafetyModel.h"
#include <set>

namespace Mcp
{
namespace
{
    using namespace Tools;
    namespace S = Tools::Schema;
    namespace fs = std::filesystem;

    constexpr uintmax_t kMaxMapFile = 512u * 1024 * 1024, kMaxInterfaceFile = 64u * 1024 * 1024;

    fs::path Utf8Path(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }
    // The game folder: the exe lives in System.
    fs::path GameRoot(const ToolContext& c)
    {
        auto system = c.systemDir;
        if (!system.has_filename()) system = system.parent_path();
        return system.parent_path();
    }
    // A map's file stem from a name or a path ("AquaD", "AquaD.sdc", "C:\...\AquaD.sdc").
    std::string Stem(std::string text)
    {
        const auto slash = text.find_last_of("\\/");
        if (slash != std::string::npos) text = text.substr(slash + 1);
        if (text.size() > 4 && Lower(text.substr(text.size() - 4)) == ".sdc") text.resize(text.size() - 4);
        return text;
    }
    std::string Letters(const std::string& name)
    {
        std::string out;
        for (unsigned char ch : name) if (ch < 0x80 && std::isalpha(ch)) out += static_cast<char>(ch);
        return out;
    }
    // A sound package name as Build UAS takes it.
    bool PlainName(const std::string& name)
    {
        if (name.empty() || name.size() > 63) return false;
        for (unsigned char ch : name) if (ch >= 0x80 || !(std::isalnum(ch) || ch == '_' || ch == '-')) return false;
        return true;
    }
    // A package name the bridge writes: letters, digits and underscores.
    bool AssetName(const std::string& name)
    {
        return !name.empty() && name.size() <= 63 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos;
    }
    std::string Megabytes(uintmax_t bytes)
    {
        std::ostringstream text;
        text.setf(std::ios::fixed);
        text.precision(bytes < 10u * 1024 * 1024 ? 2 : 1);
        if (bytes < 1024u * 1024) text << static_cast<double>(bytes) / 1024.0 << " KB";
        else text << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
        return text.str();
    }
    std::string Archive(const std::string& folder, const std::string& name) { return "Packages\\" + folder + "\\" + name; }

    // ------------------------------------------------------------ the folder
    // Every package file in the game's Packages folders by folded base name:
    // the engine knows a package by that name alone, whatever its extension,
    // so Textures\X.utx and StaticMeshes\X.usx are one package to it.
    struct PackageFile
    {
        std::string name, base, folder, extension;
        fs::path path;
        uintmax_t size = 0;
        fs::file_time_type time{};
    };
    struct Survey
    {
        fs::path root, packages;
        std::string stem;
        std::map<std::string, std::vector<PackageFile>> files;
        std::set<std::string> code; // System\*.u: the game's code, which every player has
        const PackageFile* Find(const std::string& base, const std::string& folder, const std::string& extension) const
        {
            const auto found = files.find(Lower(base));
            if (found == files.end()) return nullptr;
            for (const auto& f : found->second)
                if (Lower(f.folder) == Lower(folder) && f.extension == Lower(extension)) return &f;
            return nullptr;
        }
        bool Owned(const std::string& base) const { return Bridge::MapOwnedPackage(base, stem); }
        fs::path In(const std::string& folder, const std::string& name) const { return packages / folder / Utf8Path(name); }
    };
    Survey Look(const ToolContext& c, const std::string& stem)
    {
        Survey s;
        s.root = GameRoot(c);
        s.packages = s.root / "Packages";
        s.stem = stem;
        std::error_code error;
        if (!fs::is_directory(s.packages, error))
            throw std::runtime_error("There is no Packages folder at " + PathText(s.packages) + ". Reloaded.Editor.Mcp.exe must stay in the game's System folder, beside the editor.");
        static const std::vector<std::pair<const char*, std::vector<const char*>>> kinds = {
            {"Textures", {".utx", ".utc"}}, {"StaticMeshes", {".usx"}}, {"Sounds", {".uax", ".uas"}}, {"Animations", {".ukx"}}, {"Maps", {".sdc"}}, {"MapsEd", {".sdc"}}};
        for (const auto& [folder, extensions] : kinds)
            for (fs::directory_iterator it(s.packages / folder, error), end; !error && it != end; it.increment(error))
            {
                std::error_code fileError;
                if (!it->is_regular_file(fileError)) continue;
                const auto extension = Lower(PathText(it->path().extension()));
                if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) continue;
                PackageFile f;
                f.name = PathText(it->path().filename());
                f.base = PathText(it->path().stem());
                f.folder = folder;
                f.extension = extension;
                f.path = it->path();
                f.size = fs::file_size(f.path, fileError);
                f.time = fs::last_write_time(f.path, fileError);
                s.files[Lower(f.base)].push_back(std::move(f));
            }
        for (fs::directory_iterator it(s.root / "System", error), end; !error && it != end; it.increment(error))
            if (Lower(PathText(it->path().extension())) == ".u") s.code.insert(Lower(PathText(it->path().stem())));
        return s;
    }

    // ----------------------------------------------------------- packages
    Snapshot::Bytes ReadBytes(const fs::path& file, uintmax_t limit)
    {
        std::error_code error;
        const auto size = fs::file_size(file, error);
        if (error) throw std::runtime_error("Cannot read " + PathText(file) + ".");
        if (size > limit) throw std::runtime_error(PathText(file.filename()) + " is larger than " + std::to_string(limit >> 20) + " MiB.");
        Snapshot::Bytes bytes(static_cast<size_t>(size));
        std::ifstream in(file, std::ios::binary);
        if (!in || (size && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))))
            throw std::runtime_error("Cannot read " + PathText(file) + ".");
        return bytes;
    }
    // A package as the table reader wants it: a .utc is a zlib container
    // around an ordinary package (LevelSnapshotModel.h).
    Snapshot::Bytes Unpacked(Snapshot::Bytes bytes)
    {
        const auto container = Snapshot::Describe(bytes);
        if (!container.compressed) return bytes;
        const auto first = bytes.begin() + static_cast<std::ptrdiff_t>(container.dataOffset);
        const std::string stream(first, first + static_cast<std::ptrdiff_t>(container.compressedSize));
        const auto inflated = Image::ZlibDecompress(stream, container.uncompressedSize);
        if (inflated.size() != container.uncompressedSize) throw std::runtime_error("The package did not inflate to the size its container records.");
        return Snapshot::Bytes(inflated.begin(), inflated.end());
    }
    // The packages a map takes objects from: its import table's top-level
    // packages, without the game's code and the map's own level.
    struct Uses { bool read = false; std::string problem; std::vector<std::string> packages; };
    Uses MapImports(const Survey& s)
    {
        Uses uses;
        const auto* map = s.Find(s.stem, "MapsEd", ".sdc");
        if (!map) { uses.problem = "there is no Packages\\MapsEd\\" + s.stem + ".sdc"; return uses; }
        try
        {
            const auto bytes = Unpacked(ReadBytes(map->path, kMaxMapFile));
            const auto tables = Snapshot::Parse(bytes);
            std::set<std::string> seen;
            for (const auto& entry : tables.imports)
            {
                const auto folded = Lower(entry.name);
                if (entry.outer != 0 || Lower(entry.className) != "package" || folded == Lower(s.stem) || folded == "mylevel" || s.code.count(folded)) continue;
                if (seen.insert(folded).second) uses.packages.push_back(entry.name);
            }
            uses.read = true;
        }
        catch (const std::exception& e) { uses.problem = e.what(); }
        return uses;
    }

    // The interface package as the game reads it, in mapsettings.get's shape.
    struct Slot { const char* name; const char* key; int size; const char* format; };
    constexpr Slot kSlots[] = {{"Menu", "menu", 256, "DXT5"}, {"Briefing", "briefing", 256, "DXT5"}, {"LoadingATT0", "loadingAtt", 512, "DXT1"}, {"LoadingDEF0", "loadingDef", 512, "DXT1"}};
    std::string FormatName(int format)
    {
        static const char* names[] = {"P8", "RGBA7", "RGB16", "DXT1", "RGB8", "RGBA8", "NODATA", "DXT3", "DXT5", "L8", "G16", "RRRGGGBBB"};
        return format >= 0 && format < 12 ? names[format] : "format " + std::to_string(format);
    }
    Json DescribeInterface(const Survey& s)
    {
        const auto* utc = s.Find(s.stem + "-i", "Textures", ".utc");
        Json result{{"package", s.stem + "-i"}, {"file", PathText(utc ? utc->path : s.In("Textures", s.stem + "-i.utc"))}, {"saved", utc != nullptr}, {"settings", false},
                    {"textures", Json::object()}};
        if (!utc) return result;
        try
        {
            const auto bytes = Unpacked(ReadBytes(utc->path, kMaxInterfaceFile));
            const auto tables = Snapshot::Parse(bytes);
            result["exports"] = tables.exports.size();
            result["settings"] = tables.Find("MapSettings", "SMapSettings") != nullptr;
            for (const auto& slot : kSlots)
                if (const auto* texture = tables.Find(slot.name, "Texture"))
                    try
                    {
                        const auto t = Snapshot::DescribeTexture(bytes, tables, *texture);
                        result["textures"][slot.name] = {{"width", t.width}, {"height", t.height}, {"format", FormatName(t.format)}, {"mips", t.mips},
                                                         {"expected", {{"size", slot.size}, {"format", slot.format}}}};
                    }
                    catch (const std::exception& problem) { result["textures"][slot.name] = {{"unreadable", problem.what()}}; }
        }
        catch (const std::exception& e) { result["unreadable"] = e.what(); }
        return result;
    }

    // The packages Build UAS put into the .uas, from the per-sound log RE+
    // writes beside it ("Package  Sound  Flags ..."); the .uas itself does
    // not name them. current: the log belongs to this .uas, not an older one.
    struct UasLog { bool found = false, current = false; std::vector<std::string> packages; size_t sounds = 0; };
    UasLog ReadUasLog(const Survey& s, const PackageFile* uas)
    {
        UasLog log;
        const auto file = s.In("Sounds", s.stem + ".uas.buildlog.txt");
        std::error_code error;
        if (!fs::is_regular_file(file, error)) return log;
        log.found = true;
        const auto time = fs::last_write_time(file, error);
        log.current = uas && !error && time + std::chrono::minutes(10) >= uas->time;
        std::ifstream in(file, std::ios::binary);
        bool rows = false;
        std::set<std::string> seen;
        for (std::string line; std::getline(in, line);)
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!rows) { rows = line.rfind("-------", 0) == 0; continue; }
            if (line.empty() || line.rfind("Total", 0) == 0) break;
            std::istringstream words(line);
            std::string package, sound;
            if (!(words >> package >> sound)) continue;
            ++log.sounds;
            if (seen.insert(Lower(package)).second) log.packages.push_back(package);
        }
        return log;
    }

    bool SameBytes(const fs::path& a, const fs::path& b)
    {
        std::error_code e1, e2;
        const auto sa = fs::file_size(a, e1), sb = fs::file_size(b, e2);
        if (e1 || e2 || sa != sb) return false;
        std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
        if (!x || !y) return false;
        std::vector<char> p(1u << 20), q(1u << 20);
        for (;;)
        {
            x.read(p.data(), static_cast<std::streamsize>(p.size()));
            y.read(q.data(), static_cast<std::streamsize>(q.size()));
            if (x.gcount() != y.gcount()) return false;
            if (x.gcount() == 0) return true;
            if (std::memcmp(p.data(), q.data(), static_cast<size_t>(x.gcount())) != 0) return false;
        }
    }

    // ------------------------------------------------------------ findings
    struct Findings
    {
        Json rows = Json::array();
        void Add(const char* severity, const char* rule, const char* page, const std::string& message, const Json& fix = Json())
        {
            Json row{{"severity", severity}, {"rule", rule}, {"wikiPage", page}, {"message", message}};
            if (!fix.is_null()) row["fix"] = fix;
            rows.push_back(std::move(row));
        }
        size_t Of(const std::string& severity) const
        {
            size_t n = 0;
            for (const auto& row : rows) n += row.at("severity") == severity;
            return n;
        }
        void Sort()
        {
            auto rank = [](const Json& row) { const auto s = row.at("severity").get<std::string>(); return s == "error" ? 0 : s == "warning" ? 1 : 2; };
            std::vector<Json> list(rows.begin(), rows.end());
            std::stable_sort(list.begin(), list.end(), [&](const Json& x, const Json& y) { return rank(x) < rank(y); });
            rows = Json(list);
        }
    };
    Json Fix(const std::string& tool, const Json& args, const std::string& why = {})
    {
        Json fix{{"tool", tool}, {"args", args}};
        if (!why.empty()) fix["why"] = why;
        return fix;
    }

    // The editor, when one answers: its open map's file stem and state.
    struct EditorView { bool live = false; Json state; std::string stem, problem; };
    EditorView Connect(ToolContext& c)
    {
        EditorView e;
        try
        {
            e.state = Call(c, "editor.state");
            e.live = true;
            if (e.state.value("open", true)) e.stem = Stem(e.state.value("mapFile", std::string()));
        }
        catch (const std::exception& problem) { e.problem = problem.what(); }
        return e;
    }
    // The open map, which the editor-side actions work on.
    std::string OpenMap(ToolContext& c, const Json& a, const char* doing)
    {
        const auto state = Call(c, "editor.state");
        if (!state.value("open", true)) throw std::runtime_error(std::string("No map is open in the editor; open the map to ") + doing + " (map_file action open).");
        const auto stem = Stem(state.value("mapFile", std::string()));
        if (stem.empty())
            throw std::runtime_error(std::string("The open map has no file yet. Save it under its release name first (map_file action save_as, four letters plus D, such as AquaD): ") +
                                     doing + " is named after the map's file.");
        if (Has(a, "map") && Lower(Stem(RequireString(a, "map"))) != Lower(stem))
            throw std::runtime_error("The editor has " + stem + " open, not " + Stem(RequireString(a, "map")) + ": open that map first (map_file action open), as " + doing + " works on the open map.");
        return stem;
    }

    // --------------------------------------------------------------- layout
    // The release archive as the wiki's Map Packaging page lists it, each
    // file with where it is now (the release copy's folder first, when
    // Optimise Map Assets made one).
    Json Layout(const Survey& s, const fs::path& release, const std::string& assetPackage, Json& warnings)
    {
        Json files = Json::array(), missing = Json::array();
        uintmax_t total = 0;
        auto add = [&](const std::string& folder, const std::string& name, const char* role, bool required, const PackageFile* found)
        {
            fs::path source = found ? found->path : s.In(folder, name);
            std::error_code error;
            if (!release.empty())
            {
                const auto copy = release / "Packages" / folder / Utf8Path(name);
                if (fs::is_regular_file(copy, error)) source = copy;
            }
            const bool present = fs::is_regular_file(source, error);
            const auto size = present ? fs::file_size(source, error) : 0;
            total += size;
            files.push_back({{"archive", Archive(folder, name)}, {"source", PathText(source)}, {"present", present}, {"size", size}, {"required", required}, {"role", role}});
            if (!present && required) missing.push_back(Archive(folder, name));
        };
        add("Maps", s.stem + ".sdc", "playable map", true, s.Find(s.stem, "Maps", ".sdc"));
        add("MapsEd", s.stem + ".sdc", "editable map: not needed to play, but a Maps file cannot be turned back into a MapsEd file, so keep and ship it", false, s.Find(s.stem, "MapsEd", ".sdc"));
        add("Textures", s.stem + "-i.utc", "Map Settings, map-selection picture and loading screens", true, s.Find(s.stem + "-i", "Textures", ".utc"));
        add("Sounds", s.stem + ".uas", "the map's .uas: Interface's and the map's streamed sounds", true, s.Find(s.stem, "Sounds", ".uas"));
        for (const auto& [base, list] : s.files)
        {
            if (base == Lower(s.stem) || base == Lower(s.stem + "-i") || !s.Owned(list.front().base)) continue;
            for (const auto& f : list) add(f.folder, f.name, "the map's own package", true, &f);
        }
        if (!release.empty() && !assetPackage.empty() && !s.Find(assetPackage, "StaticMeshes", ".usx"))
            add("StaticMeshes", assetPackage + ".usx", "the release copy's asset package (Optimise Map Assets)", true, nullptr);
        if (s.Find(s.stem + "-i", "Textures", ".utx"))
            warnings.push_back("Packages\\Textures\\" + s.stem + "-i.utx is left out: the game reads only the .utc, so the wiki says to delete the .utx.");
        return {{"files", files}, {"missing", missing}, {"totalSize", total}, {"total", Megabytes(total)}};
    }

    // ---------------------------------------------------------------- check
    ToolResult CheckRelease(const Json& a, ToolContext& c)
    {
        c.progress(0, 5, "Looking at the editor and the game folder");
        const auto editor = Connect(c);
        std::string stem = Has(a, "map") ? Stem(RequireString(a, "map")) : editor.stem;
        if (stem.empty())
        {
            if (editor.live && editor.state.value("open", true))
                return Fail("The open map has no file yet. Save it under its release name (map_file action save_as: four letters plus D, such as AquaD), or name the map to check with map.");
            return Fail("Name the map to check with map (its file name, such as \"AquaD\"), or open it in the editor." +
                        (editor.problem.empty() ? std::string() : " The editor did not answer: " + editor.problem));
        }
        const bool current = editor.live && !editor.stem.empty() && Lower(editor.stem) == Lower(stem);
        const auto s = Look(c, stem);
        Findings f;
        Json notes = Json::array();
        fs::path clean;
        if (Has(a, "base"))
        {
            clean = Utf8Path(RequireString(a, "base"));
            std::error_code error;
            if (!fs::is_directory(clean / "Packages", error)) return Fail("base is a clean game install with a Packages folder; " + PathText(clean) + " has none.");
            if (fs::equivalent(clean, s.root, error)) return Fail("base must be a separate, untouched install, not the game folder this server runs in.");
        }

        // The map's display name, for the file name convention.
        std::string mapName = String(a, "mapName");
        if (mapName.empty() && current)
            try
            {
                const auto sidecar = Call(c, "mcp.sidecar.get").value("sidecar", Json());
                if (sidecar.is_object() && sidecar.contains("spec") && sidecar.at("spec").is_object())
                {
                    const auto map = sidecar.at("spec").value("map", Json::object());
                    if (map.is_object() && map.contains("name") && map.at("name").is_string()) mapName = map.at("name").get<std::string>();
                }
            }
            catch (const std::exception&) {}

        // The file name: ASCII without spaces, and the wiki's 4 + 1 letters.
        const bool ascii = std::all_of(stem.begin(), stem.end(), [](unsigned char ch) { return ch >= 0x21 && ch < 0x80 && ch != '"'; });
        if (!ascii)
            f.Add("error", "release.file-name", "map-packaging", "The map file \"" + stem + ".sdc\" must be an ASCII name without spaces or quotes: the engine reads map names on its command line.",
                  Fix("map_file", mapName.empty() ? Json{{"action", "save_as"}} : Json{{"action", "save_as"}, {"mapName", mapName}}, "Save the map under a plain name (four letters plus D)."));
        else if (!AssetName(stem))
            f.Add("warning", "release.file-name", "map-packaging", "The map file \"" + stem + "\" has characters other than letters, digits and underscores; its packages and loading screens are named after it.");
        const auto letters = Letters(mapName);
        const bool shape = stem.size() == 5 && Letters(stem) == stem;
        if (!shape || (letters.size() >= 4 && Lower(stem.substr(0, 4)) != Lower(letters.substr(0, 4))))
        {
            const auto expected = letters.size() >= 4 ? letters.substr(0, 1) + Lower(letters.substr(1, 3)) + "D" : std::string();
            f.Add("info", "release.naming-convention", "map-packaging",
                  "The map file " + stem + " does not follow the wiki's convention: the first 4 letters of the map name plus a suffix letter, D for the default version (Aquarius -> AquaD" +
                      (expected.empty() ? std::string(")") : "; this map: " + expected + ")") + ". Renaming means map_file save_as and new names for its packages.");
        }
        else if (std::toupper(static_cast<unsigned char>(stem[4])) != 'D')
            notes.push_back("The file's suffix letter is " + stem.substr(4) + "; the wiki uses D for a map's default version and other letters for variants.");
        if (mapName.empty()) notes.push_back("The map's display name is unknown (pass mapName), so only the file name's shape was checked against the convention.");

        // The map's files: MapsEd to edit it, Maps to play it.
        c.progress(1, 5, "Checking the map's files and package names");
        const auto* editable = s.Find(stem, "MapsEd", ".sdc");
        const auto* playable = s.Find(stem, "Maps", ".sdc");
        if (!editable)
            f.Add("error", "release.files", "map-packaging", "There is no Packages\\MapsEd\\" + stem + ".sdc. Save the map there (map_file save): without it the map can never be edited again.",
                  Fix("map_file", {{"action", "save"}}));
        if (!playable)
            f.Add("error", "release.files", "map-packaging", "There is no playable Packages\\Maps\\" + stem + ".sdc, which every save in the editor writes. Save the map (map_file save).",
                  Fix("map_file", {{"action", "save"}}));
        else if (editable && playable->time + std::chrono::minutes(2) < editable->time)
            f.Add("warning", "release.files", "map-packaging", "Packages\\Maps\\" + stem + ".sdc is older than the MapsEd file, so it may not hold the latest changes. Save the map again (map_file save).",
                  Fix("map_file", {{"action", "save"}}));
        if (current && editor.state.value("dirty", false))
            f.Add("warning", "release.files", "map-packaging", "The open map has unsaved changes; the files on disk, which are what you release, do not have them. Save it (map_file save).",
                  Fix("map_file", {{"action", "save"}}));

        // Package names: unique base names, the map's own named after it.
        Json release{{"packages", Json::array()}};
        Json owned = Json::array();
        for (const auto& [base, list] : s.files)
        {
            if (!s.Owned(list.front().base)) continue;
            std::vector<std::string> names;
            for (const auto& file : list) names.push_back(file.folder + "\\" + file.name);
            if (base == Lower(stem))
            {
                for (const auto& file : list)
                    if (!(file.extension == ".sdc" || (file.folder == "Sounds" && file.extension == ".uas")))
                        f.Add("error", "release.package-names", "map-packaging",
                              "Packages\\" + file.folder + "\\" + file.name + " has the map's own base name " + stem +
                                  ", so the engine takes it for the map package. Rename it " + stem + "_TXT, " + stem + "_STM or Amb_" + stem + ".");
                continue;
            }
            if (base == Lower(stem + "-i"))
            {
                for (const auto& file : list)
                    if (file.extension == ".utx")
                        f.Add("warning", "release.files", "map-settings",
                              "Packages\\Textures\\" + file.name + " is a stray copy of the interface package: the game reads only " + stem + "-i.utc, so the wiki says to delete the .utx (and leave it out of the release).");
                    else if (file.extension != ".utc")
                        f.Add("error", "release.package-names", "map-packaging", "Packages\\" + file.folder + "\\" + file.name + " has the interface package's base name " + stem + "-i; rename it.");
                continue;
            }
            if (list.size() > 1)
                f.Add("error", "release.package-names", "map-packaging",
                      Join(names, " and ") + " share the base name " + list.front().base + "; the engine ignores extensions, so they are one package to it. Rename one (" + stem + "_TXT.utx for textures, " +
                          stem + "_STM.usx for static meshes, Amb_" + stem + ".uax for sounds).");
            for (const auto& file : list)
            {
                const auto folded = Lower(file.base);
                const char* wanted = folded.size() > 4 && folded.compare(folded.size() - 4, 4, "_txt") == 0 ? ".utx"
                                     : folded.size() > 4 && folded.compare(folded.size() - 4, 4, "_stm") == 0 ? ".usx"
                                     : folded.rfind("amb_", 0) == 0 ? ".uax" : nullptr;
                if (wanted && file.extension != wanted)
                    f.Add("info", "release.naming-convention", "map-packaging", "Packages\\" + file.folder + "\\" + file.name + " is named like a " +
                          (std::string(wanted) == ".utx" ? "texture" : std::string(wanted) == ".usx" ? "static mesh" : "sound") + " package (" + wanted + ") but is a " + file.extension + " file.");
                owned.push_back({{"name", file.name}, {"folder", file.folder}, {"file", PathText(file.path)}, {"size", file.size}});
                release["packages"].push_back({{"name", file.name}, {"file", PathText(file.path)}, {"stock", false}});
            }
        }

        // What the map takes objects from.
        c.progress(2, 5, "Reading the packages the map uses");
        const auto uses = MapImports(s);
        std::vector<std::string> used = uses.packages;
        Json report;
        if (current)
            try
            {
                Json args = Json::object();
                if (Has(a, "base")) args["base"] = RequireString(a, "base");
                report = Call(c, "optimise.report", args, {}, false, 600000);
                for (const auto& pack : report.value("packs", Json::array()))
                {
                    const auto name = pack.value("name", std::string());
                    if (!name.empty() && std::none_of(used.begin(), used.end(), [&](const std::string& u) { return Lower(u) == Lower(name); })) used.push_back(name);
                }
            }
            catch (const std::exception& e) { notes.push_back(std::string("Optimise Map Assets' report could not run: ") + e.what()); }
        if (!uses.read) notes.push_back("The map's import table could not be read (" + uses.problem + "), so the packages it uses were not all checked.");
        Json dependencies = Json::array(), stock = Json::array();
        for (const auto& name : used)
        {
            const auto folded = Lower(name);
            if (s.code.count(folded) || folded == Lower(stem) || folded == "mylevel") continue;
            const auto found = s.files.find(folded);
            const bool mine = s.Owned(name);
            if (found == s.files.end())
            {
                if (mine)
                    f.Add("error", "release.dependencies", "map-packaging", "The map uses its own package " + name + ", but no file of it is in Packages: save it (package.save, or the browser's Save) or the map cannot load.");
                else
                    f.Add("error", "release.dependencies", "map-packaging", "The map uses " + name + ", but no package of that name is in Packages, so the game cannot load the map. Restore the package, or replace what the map takes from it.");
                continue;
            }
            dependencies.push_back({{"name", name}, {"mapOwned", mine}, {"files", [&] { Json list = Json::array(); for (const auto& file : found->second) list.push_back(file.folder + "\\" + file.name); return list; }()}});
            if (mine) continue;
            stock.push_back(name);
            if (clean.empty()) continue;
            for (const auto& file : found->second)
            {
                const auto original = clean / "Packages" / file.folder / Utf8Path(file.name);
                std::error_code error;
                if (!fs::is_regular_file(original, error))
                    f.Add("warning", "release.dependencies", "map-packaging",
                          "Packages\\" + file.folder + "\\" + file.name + " is not in the base install, so players do not have it. Move what the map uses from it into the release copy (release action optimise with packs [\"" +
                              file.base + "\"]), or ship it if it is your own.",
                          Fix("release", {{"action", "optimise"}, {"packs", Json::array({file.base})}}));
                else if (!SameBytes(file.path, original))
                {
                    f.Add("error", "release.package-names", "map-packaging",
                          "Packages\\" + file.folder + "\\" + file.name + " differs from the base install's copy: a stock package was modified. The wiki: never modify or ship original packages; restore it and "
                          "move your changes into the map's own packages.");
                    release["packages"].push_back({{"name", file.name}, {"file", PathText(file.path)}, {"stock", true}, {"modified", true}});
                }
            }
        }
        if (!stock.empty() && clean.empty())
            notes.push_back("The map uses " + Count(stock.size(), "package") + " that the map does not own (" + Join([&] { std::vector<std::string> v; for (const auto& n : stock) v.push_back(n.get<std::string>()); return v; }()) +
                            "): players must already have them, so they must be stock and unmodified. Pass base (the folder of a clean install) to compare them byte for byte.");
        if (report.is_object())
            for (const auto& pack : report.value("packs", Json::array()))
            {
                const auto name = pack.value("name", std::string());
                if (pack.value("suggested", false) && !s.Owned(name))
                    f.Add("info", "release.optimise", "map-packaging",
                          "The map uses " + Megabytes(pack.value("usedSize", uintmax_t{0})) + " of the " + Megabytes(pack.value("fileSize", uintmax_t{0})) + " in " + name +
                              ": a release copy with only those assets (release action optimise) is smaller to share.",
                          Fix("release", {{"action", "optimise"}, {"packs", Json::array({name})}}));
            }

        // Map Settings and the interface textures, from <Map>-i.utc.
        c.progress(3, 5, "Reading the interface package");
        auto ui = DescribeInterface(s);
        if ((!ui.value("saved", false) || ui.contains("unreadable")) && current)
            try
            {
                const auto live = Call(c, "mapsettings.get");
                if (live.is_object() && live.value("saved", false) && live.contains("textures")) ui = live;
            }
            catch (const std::exception&) {}
        if (!ui.value("saved", false))
            f.Add("error", "release.files", "map-settings",
                  "There is no Packages\\Textures\\" + stem + "-i.utc: the game reads Map Settings, the map-selection picture and the loading screens from it. Fill Map Settings (map_settings) and save the map, "
                  "then import the interface textures (release action interface).",
                  Fix("map_settings", Json::object()));
        else if (ui.contains("unreadable"))
            f.Add("warning", "interface.textures", "map-settings", stem + "-i.utc could not be read (" + ui.value("unreadable", std::string()) + "); check it opens in the Texture Browser.");
        else
        {
            if (!ui.value("settings", false))
                f.Add("warning", "settings.missing", "map-settings",
                      stem + "-i.utc has no SMapSettings: the map's name, briefs and modes are missing in game. Save the map, restart the editor and reopen it, then fill Map Settings (map_settings).",
                      Fix("editor_restart", Json::object(), "Map Settings open only after a new map has been saved, the editor closed and the map reopened."));
            const auto textures = ui.value("textures", Json::object());
            for (const auto& slot : kSlots)
            {
                const auto t = textures.value(slot.name, Json());
                const auto size = std::to_string(slot.size);
                const Json fix = Fix("release", {{"action", "interface"}, {"images", {{slot.key, "path to the image"}}}}, "Converts and imports it as the slot needs.");
                if (!t.is_object())
                    f.Add("warning", "interface.textures", "map-settings", stem + "-i has no " + slot.name + " texture (" + size + "x" + size + " " + slot.format + ").", fix);
                else if (t.contains("unreadable"))
                    f.Add("warning", "interface.textures", "map-settings", std::string(slot.name) + " in " + stem + "-i cannot be read: " + t.value("unreadable", std::string()) + ".", fix);
                else if (t.value("width", 0) != slot.size || t.value("height", 0) != slot.size || t.value("format", std::string()) != slot.format)
                    f.Add("warning", "interface.textures", "map-settings",
                          std::string(slot.name) + " is " + std::to_string(t.value("width", 0)) + "x" + std::to_string(t.value("height", 0)) + " " + t.value("format", std::string()) + "; the game expects " +
                              size + "x" + size + " " + slot.format + (std::string(slot.format) == "DXT5" ? " with alpha" : "") + ".",
                          fix);
            }
        }

        // The .uas: present, and with Interface and the map's ambient sounds.
        c.progress(4, 5, "Checking the .uas");
        const auto* uas = s.Find(stem, "Sounds", ".uas");
        const bool ambient = s.Find("Amb_" + stem, "Sounds", ".uax") != nullptr;
        std::vector<std::string> wanted{"Interface"};
        if (ambient) wanted.push_back("Amb_" + stem);
        for (const auto& name : used)
            if (Lower(name).rfind("amb_", 0) == 0 && std::none_of(wanted.begin(), wanted.end(), [&](const std::string& w) { return Lower(w) == Lower(name); })) wanted.push_back(name);
        Json sounds{{"file", PathText(uas ? uas->path : s.In("Sounds", stem + ".uas"))}, {"exists", uas != nullptr}, {"expected", wanted}};
        if (!uas)
            f.Add("error", "release.uas", "sound-browser",
                  "There is no Packages\\Sounds\\" + stem + ".uas. The wiki: always build one, even without streamed sounds of your own, with Interface (objective sounds, win/lose music) included.",
                  Fix("release", {{"action", "sounds"}}));
        else
        {
            const auto log = ReadUasLog(s, uas);
            sounds["size"] = uas->size;
            if (log.found && log.current)
            {
                sounds["packages"] = log.packages;
                sounds["sounds"] = log.sounds;
                sounds["verified"] = true;
                release["uas"] = {{"packages", log.packages}};
                for (const auto& w : wanted)
                    if (std::none_of(log.packages.begin(), log.packages.end(), [&](const std::string& p) { return Lower(p) == Lower(w); }))
                        f.Add("warning", "release.uas", "sound-browser",
                              stem + ".uas was built without " + w + (Lower(w) == "interface" ? " (objective sounds and win/lose music: menus and HUD go silent)" : ", the map's own ambient sounds") +
                                  ". Build it again (release action sounds).",
                              Fix("release", {{"action", "sounds"}}));
            }
            else
            {
                sounds["verified"] = false;
                f.Add("info", "release.uas", "sound-browser",
                      std::string("What ") + stem + ".uas includes cannot be checked: " +
                          (log.found ? "its build log is older than it" : "it has no build log (" + stem + ".uas.buildlog.txt), so it was not built with RE+") +
                          ". Build it with release action sounds to be sure it has Interface" + (ambient ? " and Amb_" + stem : std::string()) + ".",
                      Fix("release", {{"action", "sounds"}}));
            }
        }
        if (!ambient && std::none_of(used.begin(), used.end(), [](const std::string& n) { return Lower(n).rfind("amb_", 0) == 0; }))
            notes.push_back("The map has no Amb_" + stem + " sound package; that is fine when it uses only stock sounds.");
        for (const auto& name : used)
            if (Lower(name).rfind("amb_", 0) == 0 && Lower(name) != Lower("Amb_" + stem))
                notes.push_back("The map's ambient sounds are in " + name + ", not Amb_" + stem + " (the file name): build the .uas with packages [\"" + name + "\"] and ship " + name + ".uax.");
        if (!editor.live) notes.push_back("No editor answered (" + editor.problem + "): the unsaved-changes check and Optimise Map Assets' report were skipped.");
        else if (!current) notes.push_back("The editor has " + (editor.stem.empty() ? std::string("another or no saved map") : editor.stem) + " open, so only the files were checked.");

        c.progress(5, 5, "Done");
        f.Sort();
        Json warnings = Json::array();
        const auto layout = Layout(s, {}, {}, warnings);
        const auto errors = f.Of("error"), cautions = f.Of("warning"), infos = f.Of("info");
        Outcome o;
        o.summary = "Release check of " + stem + ": " + Count(errors, "error") + ", " + Count(cautions, "warning") + ", " + std::to_string(infos) + " info." +
                    (errors ? " Not ready to share." : cautions ? " Shareable once the warnings are looked at." : " Ready to package.");
        for (const auto& row : f.rows)
            if (row.at("severity") != "info") o.summary += "\n- " + row.at("severity").get<std::string>() + ": " + row.at("message").get<std::string>();
        o.warnings = warnings;
        o.extra = {{"map", stem}, {"ready", errors == 0}, {"findings", f.rows}, {"notes", notes}, {"packages", owned}, {"dependencies", dependencies},
                   {"interface", ui}, {"uas", sounds}, {"layout", layout}, {"release", release}, {"live", current}};
        if (report.is_object())
        {
            Json packs = Json::array();
            for (const auto& pack : report.value("packs", Json::array()))
                packs.push_back({{"name", pack.value("name", std::string())}, {"file", pack.value("file", std::string())}, {"fileSize", pack.value("fileSize", Json(0))},
                                 {"usedSize", pack.value("usedSize", Json(0))}, {"suggested", pack.value("suggested", false)}, {"installed", pack.value("installed", false)},
                                 {"assets", pack.value("assets", Json::array()).size()}});
            o.extra["optimise"] = {{"packs", packs}, {"inside", report.value("inside", Json::array()).size()}};
        }
        std::set<std::string> fixes;
        for (const auto& row : f.rows)
            if (row.at("severity") != "info" && row.contains("fix") && fixes.insert(row.at("fix").dump()).second && o.next.size() < 6)
                o.next.push_back(Next(row.at("fix").at("tool").get<std::string>(), row.at("message").get<std::string>(), row.at("fix").at("args")));
        o.next.push_back(Next("map_validate", "Check the map's content against the wiki's release rules too; release passes on these file facts.", {{"profile", "release"}, {"release", release}}));
        if (!errors) o.next.push_back(Next("release", "List the release archive's files (and make an optimised release copy with packs).", {{"action", "package"}}));
        return Done(c, o, false);
    }

    // --------------------------------------------------------------- sounds
    ToolResult BuildSounds(const Json& a, ToolContext& c, const std::string& label, bool dry)
    {
        const auto stem = OpenMap(c, a, "the .uas");
        const auto s = Look(c, stem);
        const double quality = Number(a, "quality", 5.0);
        if (quality < 0 || quality > 10) return Fail("quality is an Ogg Vorbis quality from 0 to 10; the wiki's default 5.0 is what Ubisoft used.");
        Outcome o;
        if (quality < 5) o.warnings.push_back("The wiki does not recommend a quality below 5.0 (Ubisoft's); the streamed sounds will sound worse.");
        Json packages = Json::array();
        std::vector<std::string> names{"Interface", "Amb_" + stem};
        for (const auto& name : Strings(a, "packages"))
        {
            if (!PlainName(name)) return Fail("Sound package names are letters, digits, underscores and hyphens, without an extension: \"" + name + "\".");
            if (std::any_of(names.begin(), names.end(), [&](const std::string& n) { return Lower(n) == Lower(name); })) continue;
            names.push_back(name);
            packages.push_back(name);
            if (!s.Find(name, "Sounds", ".uax"))
                o.warnings.push_back("There is no Packages\\Sounds\\" + name + ".uax; the editor includes " + name + " only if it is loaded.");
        }
        if (!s.Find("Amb_" + stem, "Sounds", ".uax"))
            o.warnings.push_back("There is no Packages\\Sounds\\Amb_" + stem + ".uax, so the .uas carries Interface" + (packages.empty() ? "" : " and the listed packages") +
                                 " only; that is right when the map has no streamed sounds of its own.");
        const auto file = s.In("Sounds", stem + ".uas");
        c.progress(0, 1, "Building " + stem + ".uas (Ogg Vorbis at quality " + std::to_string(quality).substr(0, 4) + ")");
        const auto result = Call(c, "uas.build", {{"packages", packages}, {"quality", quality}}, label, dry, 1800000);
        if (dry)
        {
            o.summary = "Dry run: would build Packages\\Sounds\\" + stem + ".uas from " + Join(names) + " at quality " + std::to_string(quality).substr(0, 4) + ", replacing the old one.";
            return Done(c, o, false);
        }
        o.changed.push_back(result.value("file", PathText(file)));
        for (const auto& p : result.value("skipped", Json::array())) o.warnings.push_back(p.get<std::string>() + " was not found in memory or in Packages\\Sounds, so it is not in the .uas.");
        for (const auto& p : result.value("withoutStreamSounds", Json::array()))
            o.warnings.push_back(p.get<std::string>() + " has no Stream-flagged sounds, so nothing of it went into the .uas.");
        std::vector<std::string> built;
        for (const auto& p : result.value("packages", Json::array())) if (p.is_string()) built.push_back(p.get<std::string>());
        o.summary = "Built Packages\\Sounds\\" + stem + ".uas" + (built.empty() ? std::string() : " from " + Join(built)) + " (" + std::to_string(result.value("encoded", 0)) +
                    " sounds encoded, " + Megabytes(result.value("size", uintmax_t{0})) + ").";
        if (std::none_of(built.begin(), built.end(), [](const std::string& p) { return Lower(p) == "interface"; }))
            o.warnings.push_back("Interface is not in the built .uas; the wiki requires it (objective sounds, win/lose music). Check Interface.uax is in Packages\\Sounds.");
        o.extra = {{"file", result.value("file", PathText(file))}, {"result", result}};
        o.next.push_back(Next("release", "Check the release again.", {{"action", "check"}}));
        return Done(c, o, false);
    }

    // ------------------------------------------------------------- optimise
    // The packs to take assets from: as given, or the ones Optimise Map
    // Assets suggests (the map uses less than half of the file).
    std::vector<std::string> Packs(const Json& a, ToolContext& c, const Survey& s, Json& report)
    {
        std::vector<std::string> packs;
        for (const auto& name : Strings(a, "packs"))
        {
            if (!PlainName(name)) throw std::runtime_error("packs are package names without an extension, such as \"Stock_Props\": \"" + name + "\".");
            if (std::none_of(packs.begin(), packs.end(), [&](const std::string& p) { return Lower(p) == Lower(name); })) packs.push_back(name);
        }
        if (packs.empty() && Bool(a, "suggested"))
        {
            Json args = Json::object();
            if (Has(a, "base")) args["base"] = RequireString(a, "base");
            report = Call(c, "optimise.report", args, {}, false, 600000);
            for (const auto& pack : report.value("packs", Json::array()))
                if (pack.value("suggested", false) && !s.Owned(pack.value("name", std::string()))) packs.push_back(pack.value("name", std::string()));
        }
        return packs;
    }
    // The release copy's folder as given, checked the way the editor checks
    // it (McpLifecycle.cpp, which refuses it too): a full path inside
    // <Game>\Releases, the work folder or [Mcp] WriteRoots, since with
    // overwrite the copy replaces the map's files there. Checked here first,
    // so the agent hears why before the map is saved. "" when none is given.
    std::string ReleaseFolder(const Json& a, ToolContext& c, const std::string& stem)
    {
        if (!Has(a, "folder")) return {};
        const auto text = RequireString(a, "folder");
        const auto folder = Utf8Path(text);
        const auto standard = GameRoot(c) / "Releases" / Utf8Path(stem);
        if (folder.is_relative()) throw std::runtime_error("folder is the release folder's full path, such as " + PathText(standard) + ": " + text);
        std::vector<fs::path> roots;
        std::error_code error;
        auto add = [&](const fs::path& root)
        {
            if (root.empty() || root.is_relative()) return;
            const auto canonical = fs::weakly_canonical(root, error);
            roots.push_back(error ? root.lexically_normal() : canonical);
        };
        add(GameRoot(c) / "Releases");
        add(c.workDir);
        const auto mcp = IniSection(c.systemDir / "Reloaded_Editor.ini", "Mcp");
        if (const auto found = mcp.find("writeroots"); found != mcp.end())
            for (const auto& root : Safety::SplitRoots(found->second)) add(fs::path(root)); // the INI is ANSI, as path's narrow form
        auto resolved = fs::weakly_canonical(folder, error);
        if (error) resolved = folder.lexically_normal();
        if (!Safety::InsideAny(resolved, roots))
            throw Bridge::Error(Bridge::Code::Refused, "The release copy is written only in the game's Releases folder, the work folder and [Mcp] WriteRoots (" + Safety::RootList(roots) +
                                                           "), not " + PathText(resolved) + ". Leave folder out for " + PathText(standard) +
                                                           ", or ask the user to add the folder to WriteRoots in System\\Reloaded_Editor.ini.",
                                "outside_write_roots");
        return text;
    }
    // optimise.release: the release copy, with the assets the map uses from
    // the packs moved into it. Null when there is nothing to move.
    Json ReleaseCopy(const Json& a, ToolContext& c, const Survey& s, const std::vector<std::string>& packs, const std::string& label, bool dry, Outcome& o, std::string& assetPackage)
    {
        const auto destination = String(a, "destination", "map");
        if (destination != "map" && destination != "package") throw std::runtime_error("destination is map (the assets go inside the map, the default) or package (into assetPackage).");
        Json args{{"packs", packs}, {"destination", destination}, {"overwrite", Bool(a, "overwrite")}};
        if (destination == "package")
        {
            assetPackage = String(a, "assetPackage", s.Find(s.stem + "_STM", "StaticMeshes", ".usx") ? std::string() : s.stem + "_STM");
            if (assetPackage.empty()) throw std::runtime_error(s.stem + "_STM.usx exists already: name a new package for the moved assets with assetPackage, such as " + s.stem + "_Assets.");
            if (!AssetName(assetPackage)) throw std::runtime_error("assetPackage is letters, digits and underscores: \"" + assetPackage + "\".");
            if (!s.Owned(assetPackage) || Lower(assetPackage).rfind(Lower(s.stem) + "_", 0) != 0)
                throw std::runtime_error("assetPackage must be one of the map's own names, " + s.stem + "_<name> (such as " + s.stem + "_STM): stock packages are never written.");
            if (s.files.count(Lower(assetPackage)))
                throw std::runtime_error("A package named " + assetPackage + " exists already (" + s.files.at(Lower(assetPackage)).front().folder + "\\" + s.files.at(Lower(assetPackage)).front().name +
                                         "); packages of one base name are one package to the engine. Choose a new name.");
            for (const auto& p : packs)
                if (Lower(p) == Lower(assetPackage)) throw std::runtime_error("assetPackage cannot be one of the packs.");
            args["assetPackage"] = assetPackage;
        }
        if (const auto folder = ReleaseFolder(a, c, s.stem); !folder.empty()) args["folder"] = folder;
        c.progress(0, 1, "Making the release copy (Optimise Map Assets): " + Join(packs));
        const auto result = Call(c, "optimise.release", args, label, dry, 1800000);
        if (dry) return result;
        o.changed.push_back(result.value("folder", std::string()));
        for (const auto& left : result.value("left", Json::array()))
            o.warnings.push_back("The release copy still takes " + (left.is_string() ? left.get<std::string>() : left.dump()) + " from a pack; players need that pack.");
        for (const auto& need : result.value("needs", Json::array()))
        {
            const auto name = need.value("name", std::string());
            if (!name.empty() && !s.Owned(name) && !s.code.count(Lower(name)) && Lower(name) != Lower(assetPackage))
                o.warnings.push_back("The release copy uses " + name + ", which it does not carry: players must have it (a stock package), or it goes in the archive.");
        }
        return result;
    }
    ToolResult Optimise(const Json& a, ToolContext& c, const std::string& label, bool dry)
    {
        const auto stem = OpenMap(c, a, "the release copy");
        const auto s = Look(c, stem);
        // A folder the release copy may not be written in is refused before
        // Optimise Map Assets reads the map (which can take minutes).
        ReleaseFolder(a, c, s.stem);
        Json report;
        const auto packs = Packs(a, c, s, report);
        Outcome o;
        if (packs.empty())
        {
            if (report.is_null())
            {
                Json args = Json::object();
                if (Has(a, "base")) args["base"] = RequireString(a, "base");
                report = Call(c, "optimise.report", args, {}, false, 600000);
            }
            Json packList = Json::array(), suggested = Json::array();
            for (const auto& pack : report.value("packs", Json::array()))
            {
                Json row{{"name", pack.value("name", std::string())}, {"file", pack.value("file", std::string())}, {"fileSize", pack.value("fileSize", Json(0))},
                         {"usedSize", pack.value("usedSize", Json(0))}, {"suggested", pack.value("suggested", false)}, {"installed", pack.value("installed", false)},
                         {"mapOwned", s.Owned(pack.value("name", std::string()))}};
                Json assets = Json::array();
                for (const auto& asset : pack.value("assets", Json::array())) if (assets.size() < 25) assets.push_back(asset);
                row["assets"] = assets;
                row["assetCount"] = pack.value("assets", Json::array()).size();
                if (row.at("suggested").get<bool>() && !row.at("mapOwned").get<bool>()) suggested.push_back(row.at("name"));
                packList.push_back(std::move(row));
            }
            o.summary = "Optimise Map Assets' report for " + stem + ": the map uses " + Count(packList.size(), "package") + " outside itself" +
                        (suggested.empty() ? std::string(", none of which is worth moving.") : "; worth moving into the release copy: " + Join([&] { std::vector<std::string> v; for (const auto& n : suggested) v.push_back(n.get<std::string>()); return v; }()) + ".");
            o.extra = {{"packs", packList}, {"inside", report.value("inside", Json::array()).size()}, {"text", report.value("text", std::string())}};
            if (!suggested.empty()) o.next.push_back(Next("release", "Make the release copy with only the assets the map uses from these packs.", {{"action", "optimise"}, {"packs", suggested}}));
            return Done(c, o, false);
        }
        std::string assetPackage;
        const auto result = ReleaseCopy(a, c, s, packs, label, dry, o, assetPackage);
        if (dry)
        {
            o.summary = "Dry run: would save " + stem + ", then make its release copy" + (Has(a, "folder") ? " in " + RequireString(a, "folder") : " in Releases\\" + stem) +
                        " with the assets it uses from " + Join(packs) + (assetPackage.empty() ? " moved inside the map." : " moved into " + assetPackage + ".");
            return Done(c, o, false);
        }
        o.summary = "Made the release copy of " + stem + " in " + result.value("folder", std::string()) + ": " + Count(result.value("moved", Json::array()).size(), "asset") + " moved from " +
                    Join(packs) + (assetPackage.empty() ? " into the map" : " into " + assetPackage) + ". The working map and the packs are as they were (the map was saved first).";
        o.extra = {{"folder", result.value("folder", std::string())}, {"result", result}};
        o.next.push_back(Next("release", "List the release archive with the copy's files.", {{"action", "package"}, {"folder", result.value("folder", std::string())}}));
        return Done(c, o, false);
    }

    // -------------------------------------------------------------- package
    ToolResult Package(const Json& a, ToolContext& c, const std::string& label, bool dry)
    {
        const auto editor = Connect(c);
        const std::string stem = Has(a, "map") ? Stem(RequireString(a, "map")) : editor.stem;
        if (stem.empty()) return Fail("Name the map to package with map (its file name, such as \"AquaD\"), or open it in the editor.");
        const auto s = Look(c, stem);
        Json report;
        Outcome o;
        std::vector<std::string> packs;
        fs::path folder = Has(a, "folder") ? Utf8Path(RequireString(a, "folder")) : fs::path();
        std::string assetPackage;
        if (!Strings(a, "packs").empty() || Bool(a, "suggested"))
        {
            if (!editor.live || Lower(editor.stem) != Lower(stem))
                return Fail("Making the release copy needs " + stem + " open in the editor (map_file action open); without packs, package only lists the files.");
            ReleaseFolder(a, c, s.stem);
            packs = Packs(a, c, s, report);
            if (packs.empty()) o.warnings.push_back("Optimise Map Assets suggests no packs, so no release copy was made; the archive takes the game folder's files.");
            else
            {
                const auto result = ReleaseCopy(a, c, s, packs, label, dry, o, assetPackage);
                if (!dry && result.is_object() && result.contains("folder")) folder = Utf8Path(result.at("folder").get<std::string>());
                o.extra["optimise"] = result;
            }
        }
        if (!folder.empty())
        {
            std::error_code error;
            if (!dry && !fs::is_directory(folder / "Packages", error)) return Fail("The release folder " + PathText(folder) + " has no Packages folder; make it with release action optimise and packs.");
        }
        if (editor.live && Lower(editor.stem) == Lower(stem) && editor.state.value("dirty", false))
            o.warnings.push_back("The open map has unsaved changes, which the listed files do not have. Save it (map_file save) before you share it.");
        const auto layout = Layout(s, folder, assetPackage, o.warnings);
        for (const auto& m : layout.at("missing")) o.warnings.push_back(m.get<std::string>() + " is missing; release action check says how to make it.");
        const auto uses = MapImports(s);
        Json dependencies = Json::array();
        for (const auto& name : uses.packages)
            if (!s.Owned(name)) dependencies.push_back(name);
        o.summary = std::string(dry ? "Dry run: " : "") + "The release archive of " + stem + " holds " + Count(layout.at("files").size(), "file") + " (" + layout.at("total").get<std::string>() + ")" +
                    (layout.at("missing").empty() ? "." : ", " + std::to_string(layout.at("missing").size()) + " of them missing.") +
                    " Zip them at their archive paths (relative to the game folder), or use File > Package Map for Sharing in the editor, which writes the same layout.";
        o.extra["map"] = stem;
        o.extra["layout"] = layout;
        o.extra["dependencies"] = dependencies;
        if (!folder.empty()) o.extra["folder"] = PathText(folder);
        if (!uses.read) o.warnings.push_back("The map's import table could not be read (" + uses.problem + "), so the packages it needs from elsewhere are not listed.");
        else if (!dependencies.empty())
            o.warnings.push_back("The map also uses " + Count(dependencies.size(), "package") + " it does not own; players must already have them (stock and unmodified), or move what it uses from them into the release copy (packs).");
        o.next.push_back(Next("release", "Check names, interface textures and the .uas before sharing.", {{"action", "check"}}));
        return Done(c, o, false);
    }

    // ------------------------------------------------------------ interface
    ToolResult InterfaceTextures(const Json& a, ToolContext& c, const std::string& label, bool dry)
    {
        const auto images = Object(a, "images");
        if (images.empty()) return Fail("images names the files: {menu, briefing, loadingAtt, loadingDef} (any of them, full paths).");
        const auto stem = OpenMap(c, a, "the interface package");
        Outcome o;
        Json failures = Json::array();
        size_t done = 0, given = 0;
        for (const auto& slot : kSlots)
        {
            if (!Has(images, slot.key)) continue;
            ++given;
            const auto text = String(images, slot.key);
            const auto file = Utf8Path(text);
            std::error_code error;
            std::string problem;
            const auto extension = Lower(PathText(file.extension()));
            static const std::set<std::string> readable{".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tif", ".tiff", ".tga", ".dds"};
            if (!file.is_absolute()) problem = "give the image's full path";
            else if (!fs::is_regular_file(file, error)) problem = "there is no file there";
            else if (!readable.count(extension)) problem = "the editor reads PNG, JPEG, BMP, GIF, TIFF, TGA or DDS images, not " + (extension.empty() ? std::string("this file") : extension);
            if (!problem.empty())
            {
                failures.push_back({{"slot", slot.name}, {"file", text}, {"error", problem}});
                o.warnings.push_back(std::string(slot.name) + ": " + problem + " (" + text + ").");
                continue;
            }
            c.progress(static_cast<double>(given - 1), 4, std::string("Importing ") + slot.name);
            try
            {
                const auto result = Call(c, "interface.texture", {{"slot", slot.name}, {"file", text}}, label, dry, 300000);
                ++done;
                o.changed.push_back({{"slot", slot.name}, {"file", text}, {"result", result}});
            }
            catch (const std::exception& e)
            {
                failures.push_back({{"slot", slot.name}, {"file", text}, {"error", e.what()}});
                o.warnings.push_back(std::string(slot.name) + ": " + e.what());
            }
        }
        if (!given) return Fail("images takes menu, briefing, loadingAtt and loadingDef.");
        if (!done) return Fail("No interface texture was imported. " + failures.at(0).value("slot", std::string()) + ": " + failures.at(0).value("error", std::string()), {{"failures", failures}});
        o.summary = std::string(dry ? "Dry run: would import " : "Imported ") + Count(done, "interface texture") + " into " + stem + "-i (Menu and Briefing 256x256 DXT5 with alpha, LoadingATT0 and LoadingDEF0 512x512 DXT1).";
        if (!dry)
        {
            const auto s = Look(c, stem);
            o.extra["interface"] = DescribeInterface(s);
            if (s.Find(stem + "-i", "Textures", ".utx"))
                o.warnings.push_back("Packages\\Textures\\" + stem + "-i.utx is a stray copy: the game reads only the .utc, so the wiki says to delete it.");
        }
        if (!failures.empty()) o.extra["failures"] = failures;
        o.next.push_back(Next("release", "Check the release, the interface textures included.", {{"action", "check"}}));
        return Done(c, o, false);
    }

    // The action, named or as one of the shorthand flags map_validate's fixes use.
    std::string ActionOf(const Json& a)
    {
        std::vector<std::string> named;
        if (Has(a, "action")) named.push_back(RequireString(a, "action"));
        for (const char* flag : {"check", "sounds", "optimise", "package", "interface"})
            if (Bool(a, flag) && std::find(named.begin(), named.end(), flag) == named.end()) named.push_back(flag);
        if (named.empty()) throw std::runtime_error("action is check, sounds, optimise, package or interface.");
        if (named.size() > 1) throw std::runtime_error("Give one action at a time, not " + Join(named, " and ") + ".");
        return named.front();
    }
    ToolResult Release(const Json& a, ToolContext& c)
    {
        const auto action = ActionOf(a);
        const auto label = Label(a, "release " + action);
        const bool dry = DryRun(a);
        if (action == "check") return CheckRelease(a, c);
        if (action == "sounds") return BuildSounds(a, c, label, dry);
        if (action == "optimise") return Optimise(a, c, label, dry);
        if (action == "package") return Package(a, c, label, dry);
        if (action == "interface") return InterfaceTextures(a, c, label, dry);
        return Fail("action is check, sounds, optimise, package or interface.");
    }
}

void RegisterReleaseTools(Registry& registry)
{
    Tool t;
    t.name = "release";
    t.title = "Check and package the map for release";
    t.description =
        "Takes a finished map to a shareable release by the wiki's rules (scct://wiki/map-packaging, sound-browser, map-settings). The map must be saved under "
        "its release name first (map_file save_as). Every package it writes is the map's own; files are not undoable. Details: "
        "scct://docs/tool-reference#release.\n"
        "- check (read-only): the file name, the MapsEd and Maps copies, package base names, <Map>-i.utc with SMapSettings and the four interface textures, the "
        ".uas with Interface and Amb_<Map>, the packages the map imports (base, a clean install, compares stock ones byte for byte) and Optimise Map Assets' "
        "suggestions. Returns findings, ready, the archive layout and a release block for map_validate.\n"
        "- sounds: builds Packages\\Sounds\\<Map>.uas from Interface, Amb_<Map> and packages (quality 5.0).\n"
        "- optimise: Optimise Map Assets' report; with packs (or suggested) it saves the map and makes the release copy in <Game>\\Releases\\<Map>, "
        "the assets the map uses from those packs moved inside it (destination map) or into assetPackage (destination package). A folder of your own "
        "must be inside <Game>\\Releases, the work folder or [Mcp] WriteRoots: any other is refused (outside_write_roots) before anything is saved.\n"
        "- package: the same with packs, then the archive's file list.\n"
        "- interface: images {menu, briefing, loadingAtt, loadingDef} into <Map>-i.";
    Json quality = S::Number("sounds: Ogg Vorbis quality. Default 5.0, as Ubisoft shipped.");
    quality["minimum"] = 0;
    quality["maximum"] = 10;
    t.inputSchema = S::Mutating(S::Input(
        {{"action", S::Enum({"check", "sounds", "optimise", "package", "interface"}, "What to do.")},
         {"check", S::Boolean("Same as that action (the form map_validate's fixes use); so are sounds, optimise, package and interface.")},
         {"sounds", {{"type", "boolean"}}},
         {"optimise", {{"type", "boolean"}}},
         {"package", {{"type", "boolean"}}},
         {"interface", {{"type", "boolean"}}},
         {"map", S::String("check, package: the map's file name (\"AquaD\"). Default: the open map, which the other actions need.")},
         {"mapName", S::String("check: the map's display name, for the file name convention. Default: the level spec's.")},
         {"base", S::String("check, optimise: the folder of a clean game install, to compare stock packages with.")},
         {"packages", S::Array({{"type", "string"}}, "sounds: more sound packages; Interface and Amb_<Map> are always in.", 32)},
         {"quality", quality},
         {"packs", S::Array({{"type", "string"}}, "optimise, package: packages (no extension) whose assets the map uses that go into the release copy.", 64)},
         {"suggested", S::Boolean("optimise, package: take the packs Optimise Map Assets suggests.")},
         {"destination", S::Enum({"map", "package"}, "optimise, package: the moved assets go inside the map (default) or into assetPackage.")},
         {"assetPackage", S::String("destination package: <Map>_<name>. Default <Map>_STM.")},
         {"folder", S::String("optimise, package: the release folder, a full path inside <Game>\\Releases, the work folder or [Mcp] WriteRoots. Default <Game>\\Releases\\<Map>.")},
         {"overwrite", S::Boolean("optimise, package: replace an earlier release copy's files.")},
         {"images", S::Object({{"menu", {{"type", "string"}}}, {"briefing", {{"type", "string"}}}, {"loadingAtt", {{"type", "string"}}}, {"loadingDef", {{"type", "string"}}}},
                              {}, "interface: image files (full paths): menu and briefing 256x256 DXT5, loadingAtt and loadingDef 512x512 DXT1.")}}));
    t.outputSchema = S::MutationOutput();
    t.destructive = true;
    t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return Release(a, c); }); };
    registry.Add(std::move(t));
}
}
