// Files into levels and assets: import_to_spec (any supported file, or the
// open map, as a scct.level-spec) and assets_import (textures and static
// meshes into the map's own packages).
#include "Common.h"
#include "../Media.h"
#include "../Importers/Import.h"
#include "../PlanRender.h"
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation" // MapAuthoringModel.h's dense one-liners
#endif
#include "../../Reloaded.Editor/MapAuthoringModel.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#if !defined(RE_MCP_STANDALONE) && __has_include("../LevelSpecCompiler.h")
#include "../LevelSpecCompiler.h"
#define RE_MCP_SPEC 1
#endif

namespace Mcp
{
namespace
{
    using namespace Tools;
    namespace S = Tools::Schema;
    namespace fs = std::filesystem;

    // A spec bigger than this (as JSON) is saved to a file but not returned inline.
    constexpr size_t kInlineSpec = 1024u * 1024;

    fs::path WorkFolder(ToolContext& c, const std::string& kind)
    {
        if (c.workDir.empty()) throw std::runtime_error("The server has no work folder; set [Mcp] WorkDir in System\\Reloaded_Editor.ini.");
        return c.workDir / kind / Media::Stamp();
    }
    // The open map's file stem (AquaD), which its packages are named after;
    // empty when the map has no file yet.
    std::string MapStem(const Json& state) { return Import::Stem(state.value("mapFile", std::string())); }

    // -------------------------------------------------------- import_to_spec
    std::string SpecName(const Json& spec)
    {
        const auto map = spec.value("map", Json::object());
        return map.is_object() ? map.value("name", std::string("IMPORTED")) : std::string("IMPORTED");
    }
    ToolResult ImportToSpec(const Json& a, ToolContext& c)
    {
        const auto source = RequireString(a, "source");
        const auto format = String(a, "format", "auto");
        auto options = Object(a, "options");
        const auto maxEdge = static_cast<int>(Integer(a, "maxEdge", 1024, 64, 2048));
        Json report = Json::object(), notes = Json::array();
        std::string bytes, pathText, kind, sourceText;
        if (Lower(source) == "live")
        {
            const auto wanted = Import::ImportDetail::Canonical(format);
            if (wanted != "auto" && wanted != "map-authoring") return Fail("source \"live\" reads the open map's map-authoring export; leave format auto (or map-authoring).");
            c.progress(0, 0, "Exporting the open map (authoring.export)");
            const auto exported = Call(c, "authoring.export", Json::object(), {}, false, 600000);
            if (exported.is_object() && exported.contains("blob") && exported.at("blob").is_string()) bytes = c.bridge.ReadBlob(exported.at("blob").get<std::string>());
            else if (exported.is_object() && exported.value("format", std::string()) == "scct.map-authoring") bytes = exported.dump();
            else return Fail("authoring.export returned neither a blob nor a map-authoring document; is the DLL from the same RE+ release?");
            kind = "map-authoring";
            sourceText = "the open map";
        }
        else
        {
            const auto path = Media::Utf8Path(source);
            if (path.is_relative()) return Fail("source is a full file path (such as C:\\plans\\ship.svg), or \"live\" for the open map; not \"" + source + "\".");
            bytes = Media::ReadFile(path, 256u * 1024 * 1024);
            pathText = source;
            sourceText = PathText(path.filename());
            const auto wanted = Import::ImportDetail::Canonical(format);
            kind = wanted == "auto" ? Import::Detect(pathText, bytes) : wanted;
            if (kind == "image")
            {
                // The raster importer reads PNG, BMP and TGA (not interlaced);
                // anything else Windows can open is decoded here and handed on as PNG.
                const auto imageFormat = Image::Format(bytes);
                const bool interlaced = imageFormat == "png" && bytes.size() > 28 && bytes[28] != 0;
                if ((imageFormat != "png" && imageFormat != "bmp" && imageFormat != "tga") || interlaced)
                {
                    const auto decoded = Media::DecodeImageFile(path);
                    Image::PngOptions fast;
                    fast.level = 1;
                    bytes = Image::EncodePng(decoded, fast);
                    notes.push_back("The " + (imageFormat.empty() ? std::string("image") : imageFormat) + " was decoded with " + (interlaced ? "the system codec (it is interlaced)" : "the system codec") + " and read as PNG.");
                }
            }
            if ((kind == "obj" || kind == "ase") && !options.contains("mapFile") && c.bridge.Connected())
            {
                // Suggested package names follow the open map's file.
                try
                {
                    const auto stem = MapStem(Call(c, "editor.state"));
                    if (!stem.empty()) options["mapFile"] = stem;
                }
                catch (const std::exception&) {}
            }
        }
        c.progress(1, 3, "Importing " + sourceText + " as " + kind);
        auto spec = Import::Import(kind, pathText, bytes, options, report);
        bytes.clear();
        bytes.shrink_to_fit();
        for (const auto& n : notes) report["assumptions"].push_back(n);
        std::string overlay;
        if (report.contains("overlay") && report.at("overlay").is_string())
        {
            overlay = report.at("overlay").get<std::string>();
            report["overlay"] = "Attached as an image: how each cell of the picture was read, in the legend's colours.";
        }
        // The compiled spec drawn from above, with the compiler's findings.
        std::string plan;
        Json diagnostics = Json::array(), summary;
        size_t errors = 0, warnings = 0;
#ifdef RE_MCP_SPEC
        if (Bool(a, "plan", true))
        {
            c.progress(2, 3, "Compiling and drawing the spec");
            try
            {
                Spec::CompileOptions compile;
                compile.build = false;
                compile.screenshot = false;
                const auto compiled = Spec::Compile(spec, compile);
                for (const auto& d : compiled.diagnostics)
                {
                    errors += d.severity == "error";
                    warnings += d.severity == "warning";
                    if (diagnostics.size() < 60) diagnostics.push_back({{"severity", d.severity}, {"rule", d.rule}, {"pointer", d.pointer}, {"message", d.message}, {"wikiPage", d.wikiPage}});
                }
                summary = compiled.summary;
                auto scene = compiled.render;
                Render::AddBrushes(scene, spec);
                plan = Render::PlanPng(scene, {{"maxEdge", maxEdge}, {"title", SpecName(spec) + " (imported from " + kind + ")"}});
            }
            catch (const Spec::SpecError& e)
            {
                ++errors;
                diagnostics.push_back({{"severity", "error"}, {"rule", "spec.shape"}, {"pointer", e.pointer}, {"message", e.what()}});
            }
            catch (const std::exception& e) { report["warnings"].push_back(std::string("The plan could not be drawn: ") + e.what()); }
        }
#else
        (void)maxEdge;
        report["warnings"].push_back("This build has no level-spec compiler, so no plan is drawn and the spec is not compiled; check it with level_spec_validate.");
#endif
        // The spec is also a file, for big imports and for editing by hand.
        const auto file = WorkFolder(c, "imports") / (Media::SafeName(pathText.empty() ? std::string("live") : Media::Utf8(Media::Utf8Path(pathText).stem()), "import") + ".level.json");
        const auto text = spec.dump(1, ' ', false, Json::error_handler_t::replace);
        Media::WriteFile(file, text);
        const auto counts = report.value("counts", Json::object());
        std::string countText;
        for (auto it = counts.begin(); it != counts.end(); ++it)
            if (it.value().is_number()) countText += (countText.empty() ? "" : ", ") + it.value().dump() + " " + it.key();
        Json next = Json::array({Next("plan_render", "Draw the spec (with any options, such as storey) and compare it with the source.", {{"spec", "<the spec>"}}),
                                 Next("level_spec_validate", "Check it in the release profile before building.", {{"spec", "<the spec>"}}),
                                 Next("level_build", "Preview the build (mode preview), then apply it.", {{"spec", "<the spec>"}, {"mode", "preview"}})});
        if (report.contains("assets") && report.at("assets").is_array() && !report.at("assets").empty())
        {
            Json items = Json::array();
            for (const auto& asset : report.at("assets"))
            {
                Json item{{"kind", "staticmesh"}, {"file", asset.value("file", pathText)}};
                if (asset.contains("suggestedPackage")) item["package"] = asset.at("suggestedPackage");
                if (asset.contains("suggestedName")) item["name"] = asset.at("suggestedName");
                items.push_back(item);
            }
            next = Json::array({Next("assets_import", "Import the mesh into the map's <Map>_STM package; then place it with geometry.meshes (options.mesh here does it for you).", {{"items", items}})});
        }
        Json result{{"format", kind}, {"source", pathText.empty() ? Json("live") : Json(pathText)}, {"report", report}, {"specFile", PathText(file)}, {"specBytes", text.size()},
                    {"diagnostics", diagnostics}, {"compile", {{"errors", errors}, {"warnings", warnings}}}, {"next", next}};
        if (!summary.is_null()) result["summary"] = summary;
        const bool inlineSpec = text.size() <= kInlineSpec;
        if (inlineSpec) result["spec"] = spec;
        std::string message = "Imported " + sourceText + " as " + kind + (countText.empty() ? std::string() : " (" + countText + ")") + " into the scct.level-spec \"" + SpecName(spec) + "\"";
        message += inlineSpec ? ", returned as spec and saved to " + PathText(file) + "." : ", saved to " + PathText(file) + " (" + std::to_string(text.size() / 1024) +
                                                                                              " KB is too big to return; narrow the import with options, such as mode refs for map-authoring).";
#ifdef RE_MCP_SPEC
        if (Bool(a, "plan", true)) message += " Compiled: " + Count(errors, "error") + ", " + Count(warnings, "warning") + (diagnostics.empty() ? "." : " (diagnostics).");
#endif
        const auto warned = report.value("warnings", Json::array()).size(), assumed = report.value("assumptions", Json::array()).size();
        if (warned || assumed) message += " The importer made " + Count(assumed, "assumption") + " and " + Count(warned, "warning") + " (report): read them before building.";
        if (!overlay.empty()) message += " Attached: how the picture was read.";
        if (!plan.empty()) message += " Attached: a top-down plan of the spec (north up).";
        message += " An import is a starting point: check it against the source, then fix and extend the spec yourself.";
        ToolResult r;
        r.structured = result;
        // Clients before MCP 2025-06-18 see only the text, so it carries everything.
        r.content.push_back(TextContent(message + "\n\n" + result.dump(1, ' ', false, Json::error_handler_t::replace)));
        if (!overlay.empty()) r.content.push_back(ImageContent(overlay));
        if (!plan.empty()) r.content.push_back(ImageContent(Bridge::Base64(plan)));
        return r;
    }

    // --------------------------------------------------------- assets_import
    struct Prepared
    {
        size_t item = 0;
        std::string kind, op, package, group, name, file; // file: what the editor reads
        Json args = Json::object(), source = Json::object();
        std::string pending;                               // bytes to write to file first
        Json warnings = Json::array();
        std::string Path() const { return package + (group.empty() ? "" : "." + group) + "." + name; }
        // Its file in the work folder, unique per asset.
        std::string FileName(const std::string& extension) const { return Media::SafeName(package + "_" + (group.empty() ? "" : group + "_") + name, "asset") + extension; }
    };
    std::string Extension(const fs::path& file) { return Lower(Media::Utf8(file.extension())); }
    std::string ReadHeader(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        std::string bytes(256, '\0');
        in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<size_t>(std::max<std::streamsize>(0, in.gcount())));
        return bytes;
    }
    void Name(const std::string& value, const char* what)
    {
        try { Workflow::Authoring::AssetName(value, what); }
        catch (const std::exception& e) { throw std::runtime_error(std::string(e.what()) + "."); }
    }
    Prepared Prepare(const Json& item, size_t index, const std::string& stem, const fs::path& folder, std::map<std::string, std::string>& packageKinds)
    {
        if (!item.is_object()) throw std::runtime_error("Each item is an object {kind, file, ...}.");
        Prepared p;
        p.item = index;
        p.kind = Lower(RequireString(item, "kind"));
        if (p.kind == "mesh" || p.kind == "static mesh") p.kind = "staticmesh";
        if (p.kind != "texture" && p.kind != "staticmesh") throw std::runtime_error("kind is texture or staticmesh.");
        const bool texture = p.kind == "texture";
        const auto fileText = RequireString(item, "file");
        const auto file = Media::Utf8Path(fileText);
        if (file.is_relative()) throw std::runtime_error("file must be a full path, not \"" + fileText + "\".");
        std::error_code error;
        if (!fs::is_regular_file(file, error)) throw std::runtime_error(fileText + " does not exist or is not a file.");
        p.source = {{"file", fileText}};
        static const std::vector<std::string> meshOnly{"scale", "up"}, textureOnly{"format", "mips", "masked", "alpha", "lodSet"};
        for (const auto& key : texture ? meshOnly : textureOnly)
            if (Has(item, key.c_str())) throw std::runtime_error(key + " is for " + (texture ? "staticmesh" : "texture") + " items.");
        p.name = String(item, "name", Import::Ids::Clean(Media::Utf8(file.stem())));
        if (p.name.empty()) p.name = texture ? "Texture" : "Mesh";
        Name(p.name, texture ? "Texture" : "Static mesh");
        p.group = String(item, "group");
        if (!p.group.empty()) Name(p.group, "Group");
        p.package = String(item, "package", stem + (texture ? "_TXT" : "_STM"));
        Name(p.package, "Package");
        if (!Bridge::MapOwnedPackage(p.package, stem) || Lower(p.package).rfind(Lower(stem) + "_", 0) != 0)
            throw std::runtime_error("Assets go in the map's own packages, never a stock one: " + stem + (texture ? "_TXT" : "_STM") + " (or " + stem +
                                     "_<anything>), not " + p.package + " (the bare map name, " + stem + "-i and Amb_" + stem + " belong to the map, its interface and its sounds).");
        auto& seen = packageKinds[Lower(p.package)];
        if (!seen.empty() && seen != p.kind) throw std::runtime_error("Keep textures and meshes in separate packages (" + stem + "_TXT and " + stem + "_STM): the editor saves a package as one file of one kind.");
        seen = p.kind;
        const auto ext = Extension(file);
        std::string readable = fileText;
        if (texture)
        {
            p.op = "asset.texture";
            const bool alpha = Bool(item, "alpha", false), masked = Bool(item, "masked", false);
            auto format = String(item, "format", alpha ? "DXT5" : "DXT1");
            for (auto& ch : format) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (format != "DXT1" && format != "DXT3" && format != "DXT5" && format != "RGBA8") throw std::runtime_error("format is DXT1 (default), DXT3, DXT5 (alpha) or RGBA8 (uncompressed).");
            if (ext == ".bmp" || ext == ".tga" || ext == ".pcx" || ext == ".dds")
            {
                try
                {
                    const auto size = Workflow::Authoring::ImageSize(ReadHeader(file), ext);
                    p.source["width"] = size.first;
                    p.source["height"] = size.second;
                }
                catch (const std::exception& e) { throw std::runtime_error(std::string(e.what()) + ". image_convert {file, size} makes a power-of-two TGA (fit pad, crop or stretch)."); }
                if (ext == ".dds")
                {
                    if (Has(item, "format")) p.warnings.push_back("A .dds keeps its own DXT format and mips; format was ignored.");
                    format.clear();
                }
            }
            else
            {
                // PNG, JPEG and the rest become a TGA the editor's importer reads.
                const auto image = Media::DecodeImageFile(file);
                if (!Media::PowerOfTwo(image.width) || !Media::PowerOfTwo(image.height) || image.width > 8192 || image.height > 8192)
                    throw std::runtime_error(Media::Utf8(file.filename()) + " is " + std::to_string(image.width) + "x" + std::to_string(image.height) +
                                             "; texture sides must be powers of two up to 8192. image_convert {file, size} makes one (fit pad, crop or stretch).");
                p.pending = Image::EncodeTga(alpha || masked ? image : Media::Fit(image, image.width, image.height, "stretch", false, {0, 0, 0}), false);
                const auto converted = folder / p.FileName(".tga");
                readable = Media::Utf8(converted);
                p.source["convertedTo"] = readable;
                p.source["width"] = image.width;
                p.source["height"] = image.height;
            }
            p.args = {{"package", p.package}, {"name", p.name}, {"mips", Bool(item, "mips", true)}, {"masked", masked}, {"alphaTexture", alpha}};
            if (!format.empty()) p.args["format"] = format;
            if (Has(item, "lodSet")) p.args["lodSet"] = Integer(item, "lodSet", 0, 0, 15);
        }
        else
        {
            p.op = "asset.mesh";
            if (ext == ".obj")
            {
                const auto text = Media::ReadFile(file, 64u * 1024 * 1024);
                Json convert{{"name", p.name}};
                if (Has(item, "scale")) convert["scale"] = Number(item, "scale", 1);
                if (Has(item, "up")) convert["up"] = String(item, "up");
                // The material library beside the OBJ names each material's texture.
                std::istringstream lines(text);
                for (std::string line; std::getline(lines, line);)
                {
                    if (line.rfind("mtllib", 0) != 0) continue;
                    auto name = line.substr(6);
                    const auto first = name.find_first_not_of(" \t");
                    if (first == std::string::npos) break;
                    name = name.substr(first);
                    while (!name.empty() && (name.back() == '\r' || name.back() == ' ' || name.back() == '\t')) name.pop_back();
                    const auto mtl = file.parent_path() / Media::Utf8Path(name);
                    std::error_code missing;
                    if (fs::is_regular_file(mtl, missing)) convert["mtl"] = Media::ReadFile(mtl, 4u * 1024 * 1024);
                    else p.warnings.push_back("The OBJ's material library " + name + " is not beside it; materials keep their names only.");
                    break;
                }
                auto ase = Media::ObjToAse(text, convert);
                for (const auto& w : ase.info.value("warnings", Json::array())) p.warnings.push_back(w);
                ase.info.erase("warnings");
                p.pending = std::move(ase.text);
                const auto converted = folder / p.FileName(".ase");
                readable = Media::Utf8(converted);
                p.source["convertedTo"] = readable;
                p.source["mesh"] = ase.info;
            }
            else if (ext != ".ase")
                throw std::runtime_error("Static meshes import from .ase files, or .obj (converted to ASE here). Export " + (ext.empty() ? std::string("the model") : ext) + " to OBJ or ASE first.");
            p.args = {{"package", p.package}, {"name", p.name}};
        }
        // The editor's importer reads plain ASCII paths without quotes.
        if (p.pending.empty() && !Bridge::EngineSafePath(readable))
        {
            p.pending = Media::ReadFile(file, 512u * 1024 * 1024);
            readable = Media::Utf8(folder / p.FileName(ext));
            p.source["copiedTo"] = readable;
        }
        if (!Bridge::EngineSafePath(readable))
            throw std::runtime_error("The work folder's path (" + readable + ") has characters the editor's importer cannot read; ask the user to set [Mcp] WorkDir to a plain ASCII path.");
        p.file = readable;
        p.args["file"] = readable;
        if (!p.group.empty()) p.args["group"] = p.group;
        return p;
    }
    ToolResult AssetsImport(const Json& a, ToolContext& c)
    {
        const auto items = List(a, "items");
        if (items.empty()) return Fail("items lists the textures and meshes to import: [{kind: texture|staticmesh, file, ...}].");
        if (items.size() > 64) return Fail("Import at most 64 assets per call.");
        const bool dryRun = DryRun(a), save = Bool(a, "save", true), overwrite = Bool(a, "overwrite", false);
        const auto label = Label(a, "assets_import");
        const auto state = Call(c, "editor.state");
        if (!state.value("open", true)) return Fail("No map is open. Open or create one with map_file and save it under its name: its packages are named after its file.");
        const auto stem = MapStem(state);
        if (stem.empty()) return Fail("Save the map first (map_file save, such as NEWD.sdc): its texture and mesh packages are named after the map's file (<Map>_TXT, <Map>_STM).");
        const auto folder = WorkFolder(c, "imports");
        std::vector<Prepared> plan;
        std::map<std::string, std::string> packageKinds;
        Json problems = Json::array();
        for (size_t i = 0; i < items.size(); ++i)
        {
            c.progress(static_cast<double>(i), static_cast<double>(items.size()) * 2, "Checking item " + std::to_string(i + 1));
            try { plan.push_back(Prepare(items[i], i, stem, folder, packageKinds)); }
            catch (const std::exception& e) { problems.push_back("items[" + std::to_string(i) + "]: " + e.what()); }
        }
        std::set<std::string> assets;
        for (const auto& p : plan)
            if (!assets.insert(Lower(p.Path())).second) problems.push_back("items[" + std::to_string(p.item) + "]: " + p.Path() + " is listed twice; give one of them another name or group.");
        if (!problems.empty())
            return Fail("Nothing was imported: " + Count(problems.size(), "item") + " cannot be imported as given. " + problems.at(0).get<std::string>(), {{"problems", problems}});
        auto describe = [](const Prepared& p)
        {
            Json row{{"item", p.item}, {"op", p.op}, {"asset", p.Path()}, {"args", p.args}, {"source", p.source}};
            if (!p.warnings.empty()) row["warnings"] = p.warnings;
            return row;
        };
        Outcome o;
        std::vector<std::string> packages;
        for (const auto& p : plan)
        {
            if (std::find(packages.begin(), packages.end(), p.package) == packages.end()) packages.push_back(p.package);
            for (const auto& w : p.warnings) o.warnings.push_back("items[" + std::to_string(p.item) + "]: " + w.get<std::string>());
        }
        size_t textures = 0;
        for (const auto& p : plan) textures += p.kind == "texture";
        const auto what = (textures ? Count(textures, "texture") : std::string()) + (textures && textures < plan.size() ? " and " : "") +
                          (plan.size() > textures ? Count(plan.size() - textures, "static mesh", "static meshes") : std::string());
        if (dryRun)
        {
            Json rows = Json::array();
            for (const auto& p : plan) rows.push_back(describe(p));
            o.summary = "Would import " + what + " into " + Join(packages) + (save ? " and save them" : "") + "; nothing was changed or written.";
            o.extra["plan"] = rows;
            o.next.push_back(Next("assets_import", "Run it for real (without dryRun)."));
            return Done(c, o, false);
        }
        for (const auto& p : plan)
            if (!p.pending.empty()) Media::WriteFile(Media::Utf8Path(p.file), p.pending);
        std::map<std::string, size_t> imported;
        size_t meshes = 0;
        bool stopped = false;
        Json failures = Json::array();
        for (size_t i = 0; i < plan.size(); ++i)
        {
            const auto& p = plan[i];
            c.progress(static_cast<double>(items.size() + i), static_cast<double>(items.size()) * 2, "Importing " + p.Path());
            try
            {
                const auto result = Call(c, p.op, p.args, label, false, 300000);
                Json created = describe(p);
                created.erase("args");
                const auto reported = [&](const char* key) { return result.is_object() && result.contains(key) && result.at(key).is_string() ? result.at(key).get<std::string>() : p.Path(); };
                if (p.kind == "texture") created["texture"] = reported("texture");
                else
                {
                    auto mesh = reported("mesh");
                    if (mesh.find('\'') == std::string::npos) mesh = "StaticMesh'" + mesh + "'";
                    created["mesh"] = mesh;
                    ++meshes;
                }
                created["result"] = result;
                o.created.push_back(created);
                ++imported[p.package];
            }
            catch (const Bridge::Error& e)
            {
                const auto hint = Hint(e);
                failures.push_back({{"item", p.item}, {"asset", p.Path()}, {"code", e.code}, {"error", e.what()}});
                o.warnings.push_back("items[" + std::to_string(p.item) + "] " + p.Path() + " was not imported: " + e.what() + (hint.empty() ? "" : " " + hint));
                namespace C = Bridge::Code;
                if (e.code == C::NotConnected || e.code == C::Disabled || e.code == C::ProtocolMismatch || e.code == C::Denied) { stopped = true; break; }
            }
            catch (const std::exception& e)
            {
                failures.push_back({{"item", p.item}, {"asset", p.Path()}, {"error", e.what()}});
                o.warnings.push_back("items[" + std::to_string(p.item) + "] " + p.Path() + " was not imported: " + e.what());
            }
        }
        if (o.created.empty())
        {
            Json details{{"failures", failures}};
            return Fail("Nothing was imported: " + (failures.empty() ? std::string("no item ran.") : failures.at(0).value("error", std::string("see failures."))), details);
        }
        std::vector<std::string> saved;
        for (const auto& package : packages)
        {
            if (!imported.count(package)) continue;
            Json change{{"package", package}, {"imported", imported[package]}, {"saved", false}};
            if (save && !stopped)
            {
                try
                {
                    const auto result = Call(c, "package.save", {{"package", package}, {"overwrite", overwrite}}, label, false, 300000);
                    change["saved"] = true;
                    change["result"] = result;
                    saved.push_back(package);
                }
                catch (const Bridge::Error& e)
                {
                    o.warnings.push_back("package.save " + package + ": " + e.what() +
                                         (e.reason == "exists" ? " Pass overwrite: true to replace the file (the old one is backed up first)." : " Until it is saved the import lives only in this editor session."));
                }
                catch (const std::exception& e) { o.warnings.push_back("package.save " + package + ": " + e.what()); }
            }
            else if (!save) o.warnings.push_back(package + " is only in memory: run again with save true (or save it in the editor's browser) before closing the editor.");
            o.changed.push_back(change);
        }
        o.summary = "Imported " + std::to_string(o.created.size()) + " of " + std::to_string(plan.size()) + " (" + what + ") into " + Join(packages) +
                    (saved.empty() ? std::string(save ? "; no package was saved (warnings)." : "; not saved.") : "; saved " + Join(saved) + ".");
        if (meshes) o.next.push_back(Next("level_build", "Place the meshes: geometry.meshes [{id, mesh: \"StaticMesh'<Package>.<Name>'\", at, yaw}] (the created[].mesh values), or environment objects for damageable props."));
        if (o.created.size() > meshes) o.next.push_back(Next("level_build", "Use the textures: a piece's or polygon's material/texture \"<Package>.<Group>.<Name>\" (the created[].texture values), or geometry.surfaces after a build."));
        o.next.push_back(Next("map_file", "Save the map, which refers to these packages; they ship with it (release)."));
        o.extra["failures"] = failures;
        return Done(c, o, true);
    }

    std::string FormatHelp()
    {
        std::string text;
        for (const auto& f : Import::Formats()) text += "- " + std::string(f.name) + " (" + f.extensions + "): " + f.description + "\n";
        return text;
    }
    Json FormatNames()
    {
        Json names = Json::array({"auto"});
        for (const auto& f : Import::Formats()) names.push_back(f.name);
        return names;
    }
}

void RegisterImportTools(Registry& registry)
{
    {
        Tool t;
        t.name = "import_to_spec";
        t.title = "Import a file as a level spec";
        t.description =
            "Turns a file (or the open map) into a scct.level-spec you can check, edit and build: floor plans, grids, other games' maps, tables and "
            "exports become rooms, corridors, doorways, spawns, objectives, cameras, lights and props. Deterministic: the same file gives the same spec. "
            "source is a full file path, or \"live\" for the map open in the editor (its map-authoring export: with options.mode \"refs\", the default, "
            "every actor becomes an existing.refs entry so a spec can extend and wire up what is there; \"recreate\" rebuilds it as a spec to port it).\n"
            "format is auto (by extension and contents) or one of:\n" +
            FormatHelp() +
            "options go to the importer. Common: name (the map's name, CAPS; default from the file). grid: cell (128 uu per cell), storeyHeight (384), "
            "roomHeight (320), corridorMax (256), autoDoorways (true), legend {char: meaning}, center/origin. image: pixelsPerCell, uuPerPixel, colors "
            "{\"#rrggbb\": meaning} (media_read_image's palette shows a plan's colours), tolerance (60), storey, overlay (true). svg, tiled, geojson: scale "
            "(uu per unit), plus the grid options. vmf, quake: scale (2.5; 3.2 for Quake), mirrorY (true), offset, center, textures {name: "
            "\"Package.Group.Name\"}, material, entities (true). t3d: scale, offset. obj, ase: up (y or z), scale, mesh and at to place an imported mesh. "
            "map-authoring: mode (refs | recreate), values.\n"
            "Returns spec (also saved under the work folder: specFile), report {format, warnings, assumptions, counts, assets, ...}, and the compiler's "
            "diagnostics; attaches a top-down plan of the compiled spec (maxEdge, default 1024; plan false skips it) and, for raster plans, an overlay of "
            "how each cell was read. JPEG, GIF and TIFF plans are decoded with Windows' codecs first. An import is a starting point, not a finished "
            "level: compare the plan with the source, read the assumptions, fix scale, then level_spec_validate, plan_render and level_build (preview "
            "first). Meshes (obj, ase) come back as assets for assets_import.";
        t.inputSchema = S::Input({{"source", S::String("Full path of the file to import, or \"live\" for the map open in the editor.")},
                                  {"format", {{"type", "string"}, {"enum", FormatNames()}, {"description", "The file's format. Default auto."}}},
                                  {"options", S::Free("Importer options (see the description): name, scale, cell, colors, mode, ...")},
                                  {"plan", S::Boolean("Compile the spec and attach a top-down plan. Default true.")},
                                  {"maxEdge", S::Integer("The plan's longer side in pixels, 64-2048. Default 1024.", 64, 2048)}},
                                 {"source"});
        t.readOnly = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return ImportToSpec(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "assets_import";
        t.title = "Import textures and static meshes";
        t.description =
            "Imports image files as textures and model files as static meshes into the open map's own packages, then saves the packages: the step "
            "before a spec or map can use custom materials and props. The map must be saved under its file name first (map_file save), because its "
            "packages are named after it: textures in <Map>_TXT (Packages\\Textures\\<Map>_TXT.utx) and meshes in <Map>_STM, as the wiki's Map Packaging "
            "page asks; stock packages are never written.\n"
            "items (1-64): {kind: texture | staticmesh, file (full path), package (default <Map>_TXT or <Map>_STM; must be <Map>_<anything>), group "
            "(optional), name (default the file's name)}.\n"
            "- texture: .tga, .bmp, .pcx or .dds with power-of-two sides up to 8192 import as they are; PNG, JPEG, GIF and TIFF with power-of-two "
            "sides are converted to TGA first (image_convert resizes others). format DXT1 (default), DXT3, DXT5 (default with alpha) or RGBA8; mips "
            "(default true); alpha (the importer's ALPHATEXTURE: keep transparency); masked (1-bit cut-outs); lodSet 0-15.\n"
            "- staticmesh: .ase imports as it is; .obj is converted here to ASE (one object, faces triangulated, UVs and usemtl materials kept, map_Kd "
            "from the .mtl as each material's bitmap) with up y (default) or z and scale (OBJ files in metres need 100). Other formats: export to OBJ "
            "first.\n"
            "save (default true) saves each package that received an asset; replacing an existing package file needs overwrite: true (it is backed up "
            "first). Files the editor cannot read by path (non-ASCII) are copied to the work folder. dryRun checks every item and converts nothing. "
            "Imports cannot be undone with editor_undo (they live in packages, not the map). Returns created[] with each texture's "
            "\"Package.Group.Name\" and each mesh's \"StaticMesh'Package.Name'\" for the spec (geometry.meshes, materials).";
        Json item = S::Object(
            {{"kind", S::Enum({"texture", "staticmesh"}, "What the file becomes.")},
             {"file", S::String("Full path of the image or model file.")},
             {"package", S::String("Package: <Map>_TXT (textures) or <Map>_STM (meshes) by default; must start with the map's file name and _.")},
             {"group", S::String("Optional group inside the package (letters, digits, _).")},
             {"name", S::String("Asset name (letters, digits, _). Default: the file's name.")},
             {"format", S::Enum({"DXT1", "DXT3", "DXT5", "RGBA8"}, "texture: compression. Default DXT1, or DXT5 with alpha.")},
             {"mips", S::Boolean("texture: build the mip chain. Default true.")},
             {"masked", S::Boolean("texture: masked (1-bit transparency). Default false.")},
             {"alpha", S::Boolean("texture: keep the alpha channel (ALPHATEXTURE). Default false.")},
             {"lodSet", S::Integer("texture: LOD set 0-15.", 0, 15)},
             {"scale", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "staticmesh from OBJ: multiply sizes (100 for metres). Default 1."}}},
             {"up", S::Enum({"y", "z"}, "staticmesh from OBJ: the file's up axis. Default y.")}},
            {"kind", "file"}, "One texture or static mesh.");
        t.inputSchema = S::Mutating(S::Input({{"items", S::Array(item, "The assets to import (1-64).", 64)},
                                              {"save", S::Boolean("Save each package that received an asset. Default true.")},
                                              {"overwrite", S::Boolean("Let package saves replace an existing package file (backed up first). Default false.")}},
                                             {"items"}));
        t.outputSchema = S::MutationOutput();
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return AssetsImport(a, c); }); };
        registry.Add(std::move(t));
    }
}
}
