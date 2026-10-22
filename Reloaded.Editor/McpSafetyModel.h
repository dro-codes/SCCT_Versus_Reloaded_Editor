#pragma once
// What the MCP bridge refuses before a call writes anything, and the button
// its dialog watchdog presses on a question it does not know. Engine-free and
// Windows-free: McpOps.cpp, McpLifecycle.cpp and McpBridge.cpp apply these,
// McpOpCoverageTests tests them.
#include "McpBridgeModel.h"
#include "SaveBackupModel.h"
#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Mcp::Safety
{
    using Json = Workflow::Json;

    // A refusal as a bridge error: code, sentence and reason. No code, none.
    struct Refusal
    {
        std::string code, message, reason;
        explicit operator bool() const { return !code.empty(); }
    };
    inline Refusal Refuse(std::string message, std::string reason) { return {Bridge::Code::Refused, std::move(message), std::move(reason)}; }
    inline Refusal Invalid(std::string message) { return {Bridge::Code::Invalid, std::move(message), {}}; }
    inline std::string Fold(std::string text)
    {
        for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return text;
    }
    inline std::string Upper(std::string text)
    {
        for (auto& c : text) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        return text;
    }

    // ------------------------------------------------------------ packages --

    // The two kinds of package the bridge imports into, the file each saves
    // to and the name the wiki's Map Packaging page gives it.
    struct PackageKind { const char* assets; const char* folder; const char* extension; const char* suffix; const char* what; };
    inline constexpr PackageKind kTextures{"material", "Textures", ".utx", "_TXT", "Textures"};
    inline constexpr PackageKind kMeshes{"staticmesh", "StaticMeshes", ".usx", "_STM", "Static meshes"};
    // A package, group or asset name as the editor's console reads it.
    inline bool Token(const std::string& value)
    {
        return !value.empty() && value.size() <= 63 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos;
    }
    // The text of Packages\<folder>\<file> when that file exists, else "".
    using Existing = std::function<std::string(const std::string& folder, const std::string& file)>;
    // A package the bridge may write: the map's own, named <Map>_<anything>
    // as the wiki's Map Packaging page has it (<Map>_TXT, <Map>_STM). <Map>
    // itself would clash with the map's package, <Map>-i is written by
    // interface.texture, and Amb_<Map> holds the map's sounds. No file of
    // another kind may have its base name.
    inline Refusal PackageRefusal(const std::string& package, const std::string& stem, const PackageKind& kind, const Existing& existing)
    {
        if (Fold(package) == Fold(stem + "-i")) return Refuse("The interface package is written by interface.texture, through the map save.", "interface_package");
        if (!Token(package)) return Invalid("A package name is 1-63 ASCII letters, digits and underscores: \"" + package + "\".");
        if (!Bridge::MapOwnedPackage(package, stem))
            return Refuse("Only the map's own packages are written over the bridge, never a stock one: name it " + stem + kind.suffix + " (or " + stem + "_<anything>).", "not_map_owned");
        if (Fold(package).rfind(Fold(stem) + "_", 0) != 0)
            return Refuse(std::string(kind.what) + " go in " + stem + kind.suffix + " (or " + stem + "_<anything>): a package named " + package +
                              " would clash with the map's own or its sounds, as packages of one base name are one package to the engine.", "package_name");
        static const std::pair<const char*, const char*> files[] = {
            {"Textures", ".utx"}, {"Textures", ".utc"}, {"StaticMeshes", ".usx"}, {"Sounds", ".uax"}, {"Animations", ".ukx"}, {"Maps", ".sdc"}, {"MapsEd", ".sdc"}};
        for (const auto& [folder, extension] : files)
        {
            if (Fold(folder) == Fold(kind.folder) && Fold(extension) == Fold(kind.extension)) continue;
            const auto other = existing ? existing(folder, package + extension) : std::string();
            if (!other.empty())
                return Refuse(other + " already has the base name " + package + "; packages of one base name are one package to the engine. Choose another name.", "package_name");
        }
        return {};
    }
    // What a Map JSON document writes besides the map: the packages its
    // texture operations import into and its save operations save (to
    // Packages\Textures\<package>.utx). A load only reads a package.
    struct PackageWrite { std::string op, package; };
    inline std::vector<PackageWrite> PackageWrites(const Json& document)
    {
        std::vector<PackageWrite> writes;
        if (!document.is_object() || !document.contains("operations") || !document.at("operations").is_array()) return writes;
        auto text = [](const Json& o, const char* key) { return o.contains(key) && o.at(key).is_string() ? o.at(key).get<std::string>() : std::string(); };
        for (const auto& operation : document.at("operations"))
        {
            if (!operation.is_object()) continue;
            const auto op = text(operation, "op");
            if (op == "texture" || op == "save") writes.push_back({op, text(operation, "package")});
        }
        return writes;
    }
    // The first texture or save of a document that names a package the
    // bridge may not write (stem is the open map's file name, empty when it
    // has none yet). A load stays allowed: it only reads.
    inline Refusal DocumentRefusal(const Json& document, const std::string& stem, const Existing& existing)
    {
        for (const auto& write : PackageWrites(document))
        {
            // A texture imported into the map itself is saved with it.
            if (write.op == "texture" && Fold(write.package) == "mylevel") continue;
            if (stem.empty())
                return Refuse("Save the map first (map.save with a name such as \"NEWD.sdc\"): a Map JSON " + write.op +
                                  " operation writes the map's own package, which is named after the map's file.", "unsaved_map");
            auto refusal = PackageRefusal(write.package, stem, kTextures, existing);
            if (refusal)
            {
                refusal.message = "The Map JSON " + write.op + " of " + write.package + " is refused: " + refusal.message;
                return refusal;
            }
        }
        return {};
    }
    // objimport.import (File > Import OBJ Mesh): its meshes and the .mtl's
    // textures go into one package, MyLevel (the map itself, the default) or
    // one of the map's own, as asset.mesh's do.
    inline Refusal ObjImportRefusal(const Json& args, const std::string& stem, const Existing& existing)
    {
        const bool given = args.is_object() && args.contains("package");
        if (given && !args.at("package").is_string()) return Invalid("objimport.import: package is text (MyLevel, or the map's own <Map>_STM).");
        const auto package = given ? args.at("package").get<std::string>() : std::string("MyLevel");
        // Saved with the map, so allowed before its first save too.
        if (Fold(package) == "mylevel") return {};
        if (stem.empty())
            return Refuse("Save the map first (map.save with a name such as \"NEWD.sdc\"): an OBJ import into " + package +
                              " writes the map's own package, which is named after the map's file. MyLevel takes it now.", "unsaved_map");
        auto refusal = PackageRefusal(package, stem, kMeshes, existing);
        if (refusal) refusal.message = "The OBJ import into " + package + " is refused: " + refusal.message;
        return refusal;
    }

    // ------------------------------------------------------------- folders --

    // A path inside a folder (or the folder itself), compared as Windows
    // does: either slash, any case. Both should be canonical already.
    inline bool Inside(const std::filesystem::path& path, const std::filesystem::path& root)
    {
        using String = std::filesystem::path::string_type;
        auto key = [](const std::filesystem::path& p)
        {
            String text = p.lexically_normal().native();
            for (auto& c : text)
            {
                if (c == '/') c = '\\';
                else if (c >= 'A' && c <= 'Z') c = static_cast<String::value_type>(c - 'A' + 'a');
            }
            while (text.size() > 1 && text.back() == '\\') text.pop_back();
            return text;
        };
        const auto p = key(path), r = key(root);
        if (r.empty() || p.size() < r.size() || p.compare(0, r.size(), r) != 0) return false;
        return p.size() == r.size() || p[r.size()] == '\\' || r.back() == '\\';
    }
    inline bool InsideAny(const std::filesystem::path& path, const std::vector<std::filesystem::path>& roots)
    {
        for (const auto& root : roots) if (Inside(path, root)) return true;
        return false;
    }
    // [Mcp] WriteRoots: folders separated by ';', each trimmed of spaces and
    // quotes, as Reloaded.Editor.Mcp.exe reads it (Media.h).
    inline std::vector<std::string> SplitRoots(const std::string& list)
    {
        std::vector<std::string> roots;
        size_t start = 0;
        while (start <= list.size())
        {
            const auto end = (std::min)(list.find(';', start), list.size());
            const auto item = list.substr(start, end - start);
            const auto first = item.find_first_not_of(" \t\"");
            if (first != std::string::npos) roots.push_back(item.substr(first, item.find_last_not_of(" \t\"") - first + 1));
            start = end + 1;
        }
        return roots;
    }
    inline std::string RootList(const std::vector<std::filesystem::path>& roots)
    {
        std::string list;
        for (const auto& root : roots)
        {
            const auto text = root.u8string();
            list += (list.empty() ? "" : "; ") + std::string(text.begin(), text.end());
        }
        return list.empty() ? std::string("none set") : list;
    }

    // -------------------------------------------------------- raw commands --

    // Whether a command is phrase ("MAP NEW") as the engine's ParseCommand
    // reads it, word by word (SaveBackupModel.h): any case, the blanks
    // around each word skipped, and a word followed by anything but a letter
    // or digit, so QUIT; is QUIT, EXEC"x" is EXEC and SAVEMAPPROP;MAP=X is
    // SAVEMAPPROP.
    inline bool CommandIs(const std::string& command, const std::string& phrase)
    {
        size_t at = 0;
        for (size_t start = 0; start < phrase.size();)
        {
            const auto end = (std::min)(phrase.find(' ', start), phrase.size());
            if (!SaveBackups::Word(command, at, phrase.substr(start, end - start).c_str())) return false;
            start = end + 1;
        }
        return true;
    }
    // What the engine's Parse(command, "KEY=", ...) can read: it finds the
    // key anywhere, in any case (PROFILE= holds FILE=), and takes a quoted
    // value or one up to a space or comma. Every occurrence, not only the
    // first the engine would read.
    inline std::vector<std::string> ParameterValues(const std::string& command, const std::string& key)
    {
        std::vector<std::string> values;
        const auto upper = Upper(command), wanted = Upper(key);
        for (size_t at = upper.find(wanted); at != std::string::npos; at = upper.find(wanted, at + 1))
        {
            size_t start = at + wanted.size(), end = std::string::npos;
            if (start < command.size() && command[start] == '"') end = command.find('"', ++start);
            else end = command.find_first_of(" \t,", start);
            values.push_back(command.substr(start, end == std::string::npos ? std::string::npos : end - start));
        }
        return values;
    }
    // Console commands exec never runs: they end the editor (or crash it:
    // DEBUG), run a file of commands (EXEC), replace, save, import or export
    // the map, read or write packages and files, undo or redo blindly, or
    // reach the sound driver. map.*, package.save, asset.* and undo.* do
    // those with their checks.
    inline const std::vector<const char*>& DeniedCommands()
    {
        static const std::vector<const char*> denied{
            "QUIT", "EXIT", "RELAUNCH", "DEBUG", "EXEC", "MAP NEW", "MAP LOAD", "MAP SAVE", "MAP IMPORT", "MAP EXPORT",
            "OBJ SAVEPACKAGE", "OBJ EXPORT", "OBJ IMPORT", "OBJ LOAD", "SAVEMAPPROP", "LOADMAPPROP", "TEXTURE IMPORT", "TEXTURE EXPORT",
            "STATICMESH IMPORT", "STATICMESH EXPORT", "MESH IMPORT", "BRUSH IMPORT", "BRUSH EXPORT", "DUMPINT", "TRANSACTION", "AUDIO"};
        return denied;
    }
    struct RawContext
    {
        std::string mapStem;                       // the open map's file name; empty before its first save
        std::filesystem::path base;                // where the console reads relative paths from (System)
        std::vector<std::filesystem::path> roots;  // the work folder and [Mcp] WriteRoots
    };
    inline Refusal RawCommandRefusal(const std::string& command, const RawContext& context)
    {
        if (command.empty() || command.size() > 4096) return Invalid("exec takes one editor command of up to 4096 characters.");
        for (unsigned char c : command)
            if (c < 0x20 && c != '\t') return Invalid("exec runs one command, on one line.");
        if (command.find('|') != std::string::npos) return Invalid("exec runs one command, without |.");
        for (const std::string denied : DeniedCommands())
            if (CommandIs(command, denied))
                return Refuse("exec does not run " + denied + " commands. Use the map, package and undo tools, which check before they act.", "denied_command");
        // Any other command that names a file: only in the folders the
        // server may write in.
        for (const auto& value : ParameterValues(command, "FILE="))
        {
            auto path = std::filesystem::path(std::u8string(value.begin(), value.end()));
            if (path.is_relative()) path = context.base / path;
            if (value.empty() || !InsideAny(path.lexically_normal(), context.roots))
                return Refuse("exec names files only inside the work folder and [Mcp] WriteRoots (" + RootList(context.roots) + "), not FILE=" + value +
                                  ". The map, package and import tools write the game's own folders.", "outside_write_roots");
        }
        // And packages only of the map's own (or the map itself, MyLevel);
        // a package named the map's file would clash with the map.
        for (const auto& value : ParameterValues(command, "PACKAGE="))
            if (Fold(value) != "mylevel" && !(Bridge::MapOwnedPackage(value, context.mapStem) && Fold(value) != Fold(context.mapStem)))
            {
                const auto map = context.mapStem.empty() ? std::string("<Map>") : context.mapStem;
                return Refuse("exec names only the map's own packages (" + map + "_<name>, " + map + "-i, Amb_" + map + " or MyLevel), never a stock one: PACKAGE=" + value +
                                  (context.mapStem.empty() ? std::string(". The map has no file yet; save it first.") : std::string(".")), "not_map_owned");
            }
        return {};
    }

    // ------------------------------------------------------ dialog watchdog --

    // The standard dialog command ids (winuser.h IDOK..IDCONTINUE).
    namespace Id
    {
        constexpr int Ok = 1, Cancel = 2, Abort = 3, Retry = 4, Ignore = 5, Yes = 6, No = 7, Close = 8, Help = 9, TryAgain = 10, Continue = 11;
    }
    struct Button { int id = 0; std::wstring label; }; // the label as shown: "&Nein"
    // "&Cancel " -> "cancel".
    inline std::wstring PlainLabel(std::wstring label)
    {
        std::wstring plain;
        for (auto c : label)
            if (c != L'&') plain += (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        while (!plain.empty() && plain.back() == L' ') plain.pop_back();
        while (!plain.empty() && plain.front() == L' ') plain.erase(plain.begin());
        return plain;
    }
    // The most cautious button of a dialog nobody answers: by control id
    // first, as Windows translates a message box's labels ("&Nein") but not
    // its ids, in the order Cancel, No, Close, Abort; then the English
    // labels of a dialog with ids of its own; OK only when it is the only
    // button. -1 when none is safe to press: the dialog is closed instead.
    inline int CautiousButton(const std::vector<Button>& buttons)
    {
        for (int id : {Id::Cancel, Id::No, Id::Close, Id::Abort})
            for (size_t i = 0; i < buttons.size(); ++i)
                if (buttons[i].id == id) return static_cast<int>(i);
        for (const wchar_t* word : {L"cancel", L"no", L"close", L"abort"})
            for (size_t i = 0; i < buttons.size(); ++i)
                if (PlainLabel(buttons[i].label) == word) return static_cast<int>(i);
        if (buttons.size() == 1 && (buttons[0].id == Id::Ok || PlainLabel(buttons[0].label) == L"ok")) return 0;
        return -1;
    }
}
