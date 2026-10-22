// Media helpers: media_extract_frames, media_read_image, image_convert. The
// server reads pictures and video for the agent and measures them; what they
// show is the agent's to interpret.
#include "Common.h"
#include "../Media.h"

namespace Mcp
{
namespace
{
    using namespace Tools;
    namespace S = Tools::Schema;
    namespace fs = std::filesystem;

    // Base64 image bytes one result carries inline (about 3 MiB of PNG).
    constexpr size_t kInlineBudget = 4u * 1024 * 1024;
    const char* kNoInterpretation =
        "The server does not interpret images or video: it only extracts and measures them. Look at the pictures yourself, describe what they show, "
        "and write what you see into a scct.level-spec (provenance.frames can list the files you used).";

    std::string IniValue(ToolContext& c, const std::string& key)
    {
        const auto section = IniSection(c.systemDir / "Reloaded_Editor.ini", "Mcp");
        const auto found = section.find(key);
        return found == section.end() ? std::string() : found->second;
    }
    std::vector<fs::path> Roots(ToolContext& c) { return Media::WriteRoots(c.workDir, IniValue(c, "writeroots")); }
    fs::path WorkFolder(ToolContext& c, const std::string& kind)
    {
        if (c.workDir.empty()) throw std::runtime_error("The server has no work folder; set [Mcp] WorkDir in System\\Reloaded_Editor.ini.");
        return c.workDir / kind / Media::Stamp();
    }
    // A file the agent names: a full path that exists.
    fs::path InputFile(const Json& a, const char* key)
    {
        const auto text = RequireString(a, key);
        const auto path = Media::Utf8Path(text);
        if (path.is_relative()) throw std::runtime_error(std::string(key) + " must be a full path (such as C:\\plans\\ship.png), not \"" + text + "\".");
        std::error_code error;
        if (!fs::is_regular_file(path, error)) throw std::runtime_error(std::string(key) + ": " + text + " does not exist or is not a file.");
        return path;
    }
    // A folder or file the agent names for output: full, and inside the work
    // folder or [Mcp] WriteRoots.
    fs::path OutputPath(ToolContext& c, const Json& a, const char* key)
    {
        const auto text = RequireString(a, key);
        const auto path = Media::Utf8Path(text);
        if (path.is_relative()) throw std::runtime_error(std::string(key) + " must be a full path, not \"" + text + "\".");
        Media::CheckWritable(path, Roots(c));
        return path;
    }
    std::string Header(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        std::string bytes(64, '\0');
        in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<size_t>(std::max<std::streamsize>(0, in.gcount())));
        return bytes;
    }
    std::string FormatName(const fs::path& file)
    {
        const auto known = Image::Format(Header(file));
        if (!known.empty()) return known;
        auto ext = Lower(Media::Utf8(file.extension()));
        if (!ext.empty() && ext[0] == '.') ext.erase(ext.begin());
        return ext.empty() ? std::string("unknown") : ext;
    }
    Json SizeJson(int width, int height) { return {{"width", width}, {"height", height}}; }

    // ------------------------------------------------- media_extract_frames
    ToolResult ExtractFrames(const Json& a, ToolContext& c)
    {
        const auto video = InputFile(a, "video");
        Media::FrameOptions o;
        o.mode = String(a, "mode", "interval");
        o.intervalS = Number(a, "intervalS", 2);
        o.count = static_cast<int>(Integer(a, "count", o.mode == "interval" ? 64 : 12, 1, 64));
        o.start = Number(a, "start", 0);
        o.end = Has(a, "end") ? Number(a, "end", -1) : -1;
        o.maxEdge = static_cast<int>(Integer(a, "maxEdge", 768, 64, 1280));
        o.threshold = Number(a, "threshold", 0.3);
        o.backend = String(a, "backend", "auto");
        o.outDir = Has(a, "outDir") ? OutputPath(c, a, "outDir") : WorkFolder(c, "frames");
        // Only the user chooses which ffmpeg runs: [Mcp] Ffmpeg, else PATH.
        const auto configured = IniValue(c, "ffmpeg");
        if (!configured.empty()) o.ffmpeg = fs::path(configured); // the INI is ANSI, as path's narrow form
        o.progress = [&c](double done, double total, const std::string& message) { c.progress(done, total, message); };
        const bool sheet = Bool(a, "sheet", false);
        const auto attach = String(a, "attach", sheet ? "sheet" : "frames");
        if (attach != "frames" && attach != "sheet" && attach != "both" && attach != "none") return Fail("attach is frames, sheet, both or none.");
        if ((attach == "sheet" || attach == "both") && !sheet) return Fail("attach " + attach + " needs sheet: true.");

        const auto extracted = Media::ExtractFrames(video, o);
        if (extracted.frames.empty()) return Fail("No frames were read from " + Media::Utf8(video) + " between " + Media::TimeText(o.start) + " and " +
                                                  (o.end < 0 ? std::string("the end") : Media::TimeText(o.end)) + ".", {{"notes", extracted.notes}});
        ToolResult r;
        std::vector<Content> images;
        size_t used = 0;
        auto attachImage = [&](const std::string& caption, const std::string& png)
        {
            auto data = Bridge::Base64(png);
            if (used + data.size() > kInlineBudget) return false;
            used += data.size();
            images.push_back(TextContent(caption));
            images.push_back(ImageContent(std::move(data)));
            return true;
        };
        Json sheetJson;
        if (sheet)
        {
            const auto columns = static_cast<int>(Integer(a, "sheetColumns", 0, 0, 16));
            const auto edge = static_cast<int>(Integer(a, "sheetMaxEdge", 1536, 256, 4096));
            const auto image = Media::ContactSheet(extracted.frames, columns, edge);
            const auto png = Media::PngBytes(image);
            const auto file = o.outDir / "contact-sheet.png";
            Media::WriteFile(file, png);
            sheetJson = {{"file", PathText(file)}, {"width", image.width}, {"height", image.height}, {"inline", false}};
            if (attach == "sheet" || attach == "both")
                sheetJson["inline"] = attachImage("Contact sheet of all " + Count(extracted.frames.size(), "frame") + ", each labelled with its number and time.", png);
        }
        Json frames = Json::array();
        size_t attached = 0;
        for (size_t i = 0; i < extracted.frames.size(); ++i)
        {
            const auto& f = extracted.frames[i];
            Json item{{"number", i + 1}, {"file", PathText(f.path)}, {"time", Media::Round(f.time, 3)}, {"timeText", Media::TimeText(f.time)}, {"width", f.width}, {"height", f.height}, {"inline", false}};
            if (f.change >= 0) item["change"] = f.change;
            if (attach == "frames" || attach == "both")
            {
                const auto png = Media::ReadFile(f.path, 64u * 1024 * 1024);
                if (attachImage("Frame " + std::to_string(i + 1) + " at " + Media::TimeText(f.time) + " (" + PathText(f.path.filename()) + ")", png)) { item["inline"] = true; ++attached; }
            }
            frames.push_back(item);
        }
        Json notes = Json::array();
        for (const auto& n : extracted.notes) notes.push_back(n);
        const bool sheetInline = sheetJson.is_object() && sheetJson.value("inline", false);
        if ((attach == "frames" || attach == "both") && attached < extracted.frames.size())
            notes.push_back("Only " + std::to_string(attached) + " of " + std::to_string(extracted.frames.size()) +
                            " frames are attached (a 4 MiB image budget per result); the rest are files at their paths. sheet: true shows them all in one image, or lower maxEdge.");
        if (sheet && (attach == "sheet" || attach == "both") && !sheetInline) notes.push_back("The contact sheet is too big to attach; lower sheetMaxEdge.");
        const auto from = Media::TimeText(extracted.frames.front().time), to = Media::TimeText(extracted.frames.back().time);
        std::string text = "Extracted " + Count(extracted.frames.size(), "frame") + " (" + o.mode + " mode, " + from + " to " + to + ") from " + Media::Utf8(video.filename()) +
                           (extracted.duration > 0 ? " (" + Media::TimeText(extracted.duration) + " long" : " (length unknown") +
                           (extracted.width > 0 ? ", " + std::to_string(extracted.width) + "x" + std::to_string(extracted.height) + ")" : ")") + " with " +
                           (extracted.backend == "ffmpeg" ? "ffmpeg" : "Media Foundation") + " into " + PathText(o.outDir) + ". ";
        if (attached || sheetInline)
            text += "Attached: " + std::string(sheetInline ? "the contact sheet" : "") + (sheetInline && attached ? " and " : "") +
                    (attached ? Count(attached, "frame") : std::string()) + ". ";
        text += kNoInterpretation;
        text += " media_read_image measures a frame with a labelled grid; plan_render draws your spec to compare with the frames.";
        r.structured = {{"video", PathText(video)}, {"backend", extracted.backend}, {"duration", extracted.duration > 0 ? Json(Media::Round(extracted.duration, 3)) : Json()},
                        {"size", extracted.width > 0 ? SizeJson(extracted.width, extracted.height) : Json()}, {"mode", o.mode}, {"outDir", PathText(o.outDir)},
                        {"frames", frames}, {"sheet", sheetJson}, {"attached", {{"images", attached + (sheetInline ? 1 : 0)}, {"bytes", used}}}, {"notes", notes},
                        {"interpretation", kNoInterpretation},
                        {"next", Json::array({Next("media_read_image", "Measure a frame: a grid in pixels or Unreal units, its colours and how much of it is drawn.", {{"path", frames.at(0).at("file")}}),
                                              Next("plan_render", "Draw the spec you write from the frames, to compare with them.")})}};
        r.content.push_back(TextContent(text + "\n\n" + r.structured.dump(2, ' ', false, Json::error_handler_t::replace)));
        for (auto& image : images) r.content.push_back(std::move(image));
        return r;
    }

    // ----------------------------------------------------- media_read_image
    Media::GridSpec GridFrom(const Json& g)
    {
        Media::GridSpec grid;
        grid.uuPerPx = Number(g, "uuPerPx", 0);
        if (grid.uuPerPx < 0) throw std::runtime_error("grid.uuPerPx must be more than 0.");
        if (Has(g, "spacingPx")) grid.spacing = Number(g, "spacingPx", 0);
        else if (Has(g, "cellUu"))
        {
            if (!(grid.uuPerPx > 0)) throw std::runtime_error("grid.cellUu needs grid.uuPerPx (Unreal units per source pixel) to find the spacing in pixels.");
            grid.spacing = Number(g, "cellUu", 0) / grid.uuPerPx;
        }
        else throw std::runtime_error("grid needs spacingPx (source pixels between lines), or cellUu with uuPerPx.");
        if (!(grid.spacing >= 1)) throw std::runtime_error("The grid spacing must be at least 1 source pixel.");
        if (Has(g, "origin"))
        {
            const auto& o = g.at("origin");
            if (!o.is_array() || o.size() != 2 || !o[0].is_number() || !o[1].is_number()) throw std::runtime_error("grid.origin is [x, y] in source pixels.");
            grid.origin = {o[0].get<double>(), o[1].get<double>()};
        }
        grid.labels = Bool(g, "labels", true);
        return grid;
    }
    ToolResult ReadImage(const Json& a, ToolContext&)
    {
        const auto file = InputFile(a, "path");
        const auto source = Media::DecodeImageFile(file);
        Image::Rgba region = source;
        std::array<double, 2> offset{0, 0};
        Json regionJson = {{"x", 0}, {"y", 0}, {"width", source.width}, {"height", source.height}};
        if (Has(a, "crop"))
        {
            const auto crop = Object(a, "crop");
            const auto x = static_cast<int>(Integer(crop, "x", 0, 0, Image::kMaxEdge)), y = static_cast<int>(Integer(crop, "y", 0, 0, Image::kMaxEdge));
            const auto w = static_cast<int>(Integer(crop, "width", source.width - x, 1, Image::kMaxEdge)), h = static_cast<int>(Integer(crop, "height", source.height - y, 1, Image::kMaxEdge));
            region = Image::Crop(source, x, y, w, h);
            offset = {static_cast<double>(x), static_cast<double>(y)};
            regionJson = {{"x", x}, {"y", y}, {"width", w}, {"height", h}};
        }
        const int maxEdge = static_cast<int>(Integer(a, "maxEdge", 1024, 64, 2048));
        const bool enlarge = Bool(a, "enlarge", false);
        const bool hasGrid = Has(a, "grid");
        const auto grid = hasGrid ? GridFrom(Object(a, "grid")) : Media::GridSpec{};
        // The picture at an edge, with the grid drawn after scaling so its lines stay sharp.
        Json gridJson;
        auto render = [&](int edge)
        {
            Image::Rgba shown;
            const int longer = std::max(region.width, region.height);
            if (enlarge && longer * 2 <= edge)
            {
                const int k = std::min(8, edge / longer);
                shown = Image::Resize(region, region.width * k, region.height * k, Image::Filter::Nearest);
            }
            else shown = Image::FitWithin(region, edge, Image::Filter::Box);
            const double scale = static_cast<double>(shown.width) / region.width;
            if (hasGrid) gridJson = Media::DrawGrid(shown, scale, offset, grid);
            return shown;
        };
        int edge = maxEdge;
        auto shown = render(edge);
        auto png = Media::PngBytes(shown);
        Json notes = Json::array();
        while (Bridge::Base64(png).size() > kInlineBudget && edge > 128)
        {
            edge = edge * 3 / 4;
            shown = render(edge);
            png = Media::PngBytes(shown);
        }
        if (edge != maxEdge) notes.push_back("Shown at " + std::to_string(edge) + " pixels on the longer side so the image fits the 4 MiB attachment budget; crop for detail.");
        const double scale = static_cast<double>(shown.width) / region.width;
        Json result{{"file", PathText(file)}, {"format", FormatName(file)}, {"source", SizeJson(source.width, source.height)}, {"region", regionJson},
                    {"shown", {{"width", shown.width}, {"height", shown.height}, {"scale", Media::Round(scale, 6)}, {"pixelsPerShownPixel", Media::Round(1 / scale, 6)}}}};
        if (hasGrid) result["grid"] = gridJson;
        // Palette and coverage come from the source pixels, not the scaled copy.
        const auto paletteSize = static_cast<size_t>(Integer(a, "paletteSize", 12, 1, 32));
        const auto swatches = Media::Palette(region, std::max<size_t>(paletteSize, 1));
        if (Bool(a, "palette", true))
        {
            Json palette = Json::array();
            for (const auto& s : swatches) palette.push_back({{"hex", Media::Hex(s.rgb)}, {"rgb", s.rgb}, {"share", Media::Round(s.share)}});
            result["palette"] = palette;
        }
        const auto tolerance = static_cast<int>(Integer(a, "tolerance", 48, 0, 255));
        Media::Rgb background{255, 255, 255};
        if (Has(a, "background")) background = Media::ParseHex(String(a, "background"));
        else if (!swatches.empty()) background = swatches.front().rgb;
        result["occupancy"] = Media::Occupancy(region, background, tolerance, offset);
        if (hasGrid)
        {
            const auto cells = Media::CellOccupancy(region, background, tolerance, offset, grid);
            if (!cells.is_null()) result["cells"] = cells;
        }
        result["notes"] = notes;
        result["interpretation"] = kNoInterpretation;
        const auto& occupancy = result.at("occupancy");
        std::string text = PathText(file.filename()) + ": " + std::to_string(source.width) + "x" + std::to_string(source.height) + " " + result.at("format").get<std::string>() +
                           (Has(a, "crop") ? ", region " + std::to_string(region.width) + "x" + std::to_string(region.height) + " at " + Media::Detail::Number(offset[0]) + "," + Media::Detail::Number(offset[1]) : std::string()) +
                           ", shown at " + std::to_string(shown.width) + "x" + std::to_string(shown.height) + " (" + Media::Detail::Number(Media::Round(scale, 3)) + " shown pixels per source pixel). ";
        if (hasGrid)
            text += "Grid every " + Media::Detail::Number(Media::Round(grid.spacing, 2)) + " source pixels" +
                    (grid.uuPerPx > 0 ? " = " + Media::Detail::Number(Media::Round(grid.spacing * grid.uuPerPx, 2)) + " uu" : std::string()) + " from origin " +
                    Media::Detail::Number(grid.origin[0]) + "," + Media::Detail::Number(grid.origin[1]) + "; labels count " + (grid.uuPerPx > 0 ? "Unreal units" : "pixels") +
                    " from the origin, x rightwards and y downwards as in the image (the cyan line is 0). ";
        text += "Background " + occupancy.at("background").get<std::string>() + "; " + Media::Detail::Number(Media::Round(occupancy.at("ink").get<double>() * 100, 1)) + "% of it is drawn. ";
        text += kNoInterpretation;
        ToolResult r;
        r.structured = result;
        r.content.push_back(TextContent(text + "\n\n" + result.dump(2, ' ', false, Json::error_handler_t::replace)));
        r.content.push_back(ImageContent(Bridge::Base64(png)));
        return r;
    }

    // -------------------------------------------------------- image_convert
    struct Slot { const char* name; int size; bool alpha; };
    const Slot* FindSlot(const std::string& name)
    {
        static const Slot slots[] = {{"Menu", 256, true}, {"Briefing", 256, true}, {"LoadingATT0", 512, false}, {"LoadingDEF0", 512, false}};
        for (const auto& s : slots) if (Lower(s.name) == Lower(name)) return &s;
        return nullptr;
    }
    int Side(const Json& v, const char* what)
    {
        if (!v.is_number_integer()) throw std::runtime_error(std::string(what) + " must be a whole number of pixels.");
        const auto n = v.get<long long>();
        if (n < 1 || n > 8192 || !Media::PowerOfTwo(static_cast<int>(n)))
            throw std::runtime_error(std::string(what) + " must be a power of two up to 8192 (1, 2, 4 ... 256, 512, 1024, 2048, 4096, 8192), not " + std::to_string(n) + ".");
        return static_cast<int>(n);
    }
    ToolResult ConvertImage(const Json& a, ToolContext& c)
    {
        const auto file = InputFile(a, "file");
        const auto source = Media::DecodeImageFile(file);
        int width = 0, height = 0;
        bool alphaDefault = true;
        std::string slotName;
        if (Has(a, "slot"))
        {
            const auto* slot = FindSlot(String(a, "slot"));
            if (!slot) return Fail("slot is Menu, Briefing, LoadingATT0 or LoadingDEF0.");
            width = height = slot->size;
            alphaDefault = slot->alpha;
            slotName = slot->name;
        }
        if (Has(a, "size"))
        {
            const auto& size = a.at("size");
            if (size.is_array())
            {
                if (size.size() != 2) return Fail("size is one power of two (a square) or [width, height].");
                width = Side(size[0], "size[0]");
                height = Side(size[1], "size[1]");
            }
            else width = height = Side(size, "size");
            if (!slotName.empty() && (width != FindSlot(slotName)->size || height != width))
                return Fail("The " + slotName + " interface texture is " + std::to_string(FindSlot(slotName)->size) + " square; leave size out or match it.");
        }
        Json warnings = Json::array();
        if (!width)
        {
            width = std::min(8192, Image::NearestPowerOfTwo(source.width));
            height = std::min(8192, Image::NearestPowerOfTwo(source.height));
        }
        const auto format = Lower(String(a, "format", "tga"));
        if (format != "tga" && format != "bmp" && format != "png") return Fail("format is tga (the editor's texture importer reads it, with alpha), bmp or png.");
        bool alpha = Bool(a, "alpha", alphaDefault);
        const auto fit = Lower(String(a, "fit", "pad"));
        const auto background = Media::ParseHex(String(a, "background", "#000000"));
        if (format == "bmp" && alpha && !source.Opaque())
        {
            warnings.push_back("BMP keeps no alpha for the editor's importer, so transparency was flattened onto the background; use tga to keep it.");
            alpha = false;
        }
        fs::path out;
        if (Has(a, "out"))
        {
            out = OutputPath(c, a, "out");
            auto ext = Lower(Media::Utf8(out.extension()));
            if (ext.empty()) out += "." + format;
            else if (ext != "." + format) return Fail("out ends in " + ext + " but format is " + format + "; make them agree.");
        }
        else out = WorkFolder(c, "converted") / (Media::SafeName(Media::Utf8(file.stem()), "image") + "_" + std::to_string(width) + "x" + std::to_string(height) + "." + format);
        std::error_code error;
        if (fs::exists(out, error) && !Bool(a, "overwrite", false)) return Fail(PathText(out) + " exists. Pass overwrite: true to replace it, or choose another out.");
        const auto image = Media::Fit(source, width, height, fit, alpha, background);
        std::string bytes;
        if (format == "tga") bytes = Image::EncodeTga(image, false);
        else if (format == "bmp") bytes = Image::EncodeBmp(image);
        else bytes = Media::PngBytes(image);
        Media::WriteFile(out, bytes);
        const double grow = std::max(static_cast<double>(width) / source.width, static_cast<double>(height) / source.height);
        if (grow > 2.01) warnings.push_back("The picture was enlarged " + Media::Detail::Number(Media::Round(grow, 1)) + " times, so it will look soft.");
        if (fit == "stretch" && std::abs(static_cast<double>(width) / height - static_cast<double>(source.width) / source.height) > 0.02 * static_cast<double>(width) / height)
            warnings.push_back("stretch changed the picture's proportions; pad or crop keep them.");
        if (width > 4096 || height > 4096) warnings.push_back("Over 4096 is risky: the 32-bit editor and game keep a system-memory copy of every texture.");
        const bool keptAlpha = alpha && !image.Opaque();
        Json next = Json::array();
        if (!slotName.empty()) next.push_back(Next("map_settings", "Use it as the map's " + slotName + " interface texture (interface in the level spec); the editor imports it into <Map>-i when the map is saved."));
        else if (format != "png") next.push_back(Next("assets_import", "Import it as a texture into the map's own <Map>_TXT package.", {{"items", Json::array({{{"kind", "texture"}, {"file", PathText(out)}}})}}));
        Json result{{"file", PathText(out)}, {"format", format}, {"width", width}, {"height", height}, {"alpha", keptAlpha}, {"fit", fit}, {"bytes", bytes.size()},
                    {"source", {{"file", PathText(file)}, {"width", source.width}, {"height", source.height}}}, {"warnings", warnings}, {"next", next}};
        if (!slotName.empty()) result["slot"] = slotName;
        ToolResult r;
        r.structured = result;
        const std::string text = "Wrote " + PathText(out) + ": " + std::to_string(width) + "x" + std::to_string(height) + " " + format + (keptAlpha ? " with alpha" : "") + " (" + fit +
                                 " from " + std::to_string(source.width) + "x" + std::to_string(source.height) + "). A preview is attached.";
        r.content.push_back(TextContent(text + "\n\n" + result.dump(2, ' ', false, Json::error_handler_t::replace)));
        r.content.push_back(ImageContent(Bridge::Base64(Media::PngBytes(Image::FitWithin(image, 256)))));
        return r;
    }
}

void RegisterMediaTools(Registry& registry)
{
    {
        Tool t;
        t.name = "media_extract_frames";
        t.title = "Extract frames from a video";
        t.description =
            "Pulls still frames out of a video (MP4, MOV, AVI, WMV, MKV: whatever Media Foundation or ffmpeg decodes) as PNG files and attaches them as "
            "images to look at: the first step of building a level from a walkthrough, trailer or reference clip.\n"
            "- mode interval (default) takes a frame every intervalS from start; count spreads count frames evenly from start to end; scene keeps the "
            "first frame and each cut (4 x count frames are sampled, and kept when their brightness histogram changes by threshold or more).\n"
            "- sheet also writes a contact sheet of every frame, numbered and timed; attach picks what comes back as images (at most about 4 MiB; every "
            "frame is also a file, frames[].file). Files go to <work folder>\\frames\\<stamp> unless outDir (inside the work folder or [Mcp] WriteRoots).\n"
            "- Decoding: Media Foundation on Windows, else ffmpeg from [Mcp] Ffmpeg or PATH (backend forces one); with neither, the error says how to "
            "get ffmpeg.\n"
            "The server does not interpret the frames: you decide what they show and write a scct.level-spec. Measure frames with media_read_image, "
            "calibrate against known sizes (a player about 180 uu tall, a door about 96 x 128 uu, about 100 uu to the metre) and compare with "
            "plan_render; the create_level_from_media prompt walks through it.";
        t.inputSchema = S::Input(
            {{"video", S::String("Full path of the video file.")},
             {"mode", S::Enum({"interval", "count", "scene"}, "How frames are chosen. Default interval.")},
             {"intervalS", {{"type", "number"}, {"minimum", 0.04}, {"maximum", 3600}, {"description", "interval mode: seconds between frames. Default 2."}}},
             {"count", S::Integer("count mode: how many frames (default 12); interval mode: the most (default 64); scene mode: the most scenes (default 12).", 1, 64)},
             {"start", {{"type", "number"}, {"minimum", 0}, {"description", "Seconds from the start of the video to begin at. Default 0."}}},
             {"end", {{"type", "number"}, {"minimum", 0}, {"description", "Seconds to stop at. Default: the end of the video."}}},
             {"maxEdge", S::Integer("Longer side of each frame in pixels, 64-1280. Default 768.", 64, 1280)},
             {"threshold", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}, {"description", "scene mode: how different (0-1) a frame must be from the one before to count as a new scene. Default 0.3; lower catches smaller changes."}}},
             {"sheet", S::Boolean("Also write a contact sheet of every frame. Default false.")},
             {"sheetColumns", S::Integer("Contact sheet columns, 1-16 (0 or absent: about square).", 0, 16)},
             {"sheetMaxEdge", S::Integer("Contact sheet's longer side in pixels, 256-4096. Default 1536.", 256, 4096)},
             {"attach", S::Enum({"frames", "sheet", "both", "none"}, "What to return as images. Default frames, or sheet when sheet is true.")},
             {"outDir", S::String("Full path of the folder for the PNGs, inside the work folder or [Mcp] WriteRoots. Default <work folder>\\frames\\<stamp>.")},
             {"backend", S::Enum({"auto", "mediafoundation", "ffmpeg"}, "The decoder. Default auto: Media Foundation on Windows, then ffmpeg.")}},
            {"video"});
        t.readOnly = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return ExtractFrames(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "media_read_image";
        t.title = "Read and measure an image";
        t.description =
            "Opens an image (" + Media::ImageFormats() + ") and returns it to look at, with measurements: for floor plans, maps, screenshots, concept "
            "art and video frames.\n"
            "- crop reads one part (source pixels); maxEdge caps the longer side shown; enlarge blows small pictures up by whole pixels.\n"
            "- grid draws a labelled measurement grid: spacingPx, or cellUu with uuPerPx (Unreal units per source pixel); origin (cyan) is 0; labels "
            "count pixels, or uu with uuPerPx, x rightwards and y downwards as in the image (up the image is Unreal +Y, so world Y = -label); every "
            "fifth line is brighter. cells.cells then maps how much of each cell is drawn, one character per cell, to transcribe for import_to_spec's "
            "grid format or a spec.\n"
            "- palette: the most common colours with their shares, for a raster plan's legend (import_to_spec format image, options.colors).\n"
            "- occupancy: the background (the most common colour, or background) and the share drawn (more than tolerance from it), transparent, dark "
            "and light, with the drawn part's bounding box.\n"
            "Calibrate with something of known size (a door about 96 uu wide, a player about 180 uu tall, a grid square of known metres at 100 uu "
            "each): uuPerPx = its size in uu / its size in pixels. The server only measures; you interpret what the picture shows.";
        t.inputSchema = S::Input(
            {{"path", S::String("Full path of the image file.")},
             {"maxEdge", S::Integer("Longer side of the returned image in pixels, 64-2048. Default 1024.", 64, 2048)},
             {"enlarge", S::Boolean("Enlarge a small picture by whole pixels (nearest neighbour) to near maxEdge. Default false.")},
             {"crop", S::Object({{"x", S::Integer("Left edge in source pixels.", 0, Image::kMaxEdge)}, {"y", S::Integer("Top edge in source pixels.", 0, Image::kMaxEdge)},
                                 {"width", S::Integer("Width in source pixels (default: to the right edge).", 1, Image::kMaxEdge)},
                                 {"height", S::Integer("Height in source pixels (default: to the bottom).", 1, Image::kMaxEdge)}},
                                {}, "The part of the image to read, in source pixels.")},
             {"grid", S::Object({{"spacingPx", {{"type", "number"}, {"minimum", 1}, {"description", "Source pixels between grid lines."}}},
                                 {"cellUu", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Unreal units between grid lines (needs uuPerPx)."}}},
                                 {"uuPerPx", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Unreal units per source pixel: labels become Unreal units."}}},
                                 {"origin", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}, {"description", "[x, y] source pixel where the grid's 0 is. Default [0, 0]."}}},
                                 {"labels", S::Boolean("Label the lines. Default true.")}},
                                {}, "A measurement grid to draw: spacingPx, or cellUu with uuPerPx.")},
             {"palette", S::Boolean("Return the most common colours. Default true.")},
             {"paletteSize", S::Integer("How many colours, 1-32. Default 12.", 1, 32)},
             {"background", S::String("Background colour #rrggbb for occupancy. Default: the most common colour.")},
             {"tolerance", S::Integer("How far (0-255 in any channel) a pixel may differ from the background and still count as background. Default 48.", 0, 255)}},
            {"path"});
        t.readOnly = true;
        t.idempotent = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return ReadImage(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "image_convert";
        t.title = "Make a texture-ready image";
        t.description =
            "Converts an image (PNG, JPEG, GIF, TIFF, BMP, TGA) into a texture the editor imports: power-of-two sides, as TGA (default; keeps alpha), "
            "BMP or PNG. Use it before assets_import (textures must be .tga, .bmp, .pcx or .dds with power-of-two sides) and for interface pictures.\n"
            "- size: a power of two up to 8192 (a square) or [width, height]; default each side rounded to the nearest power of two. 4096 is the "
            "practical most for the 32-bit editor.\n"
            "- slot: Menu or Briefing (256 square, alpha) or LoadingATT0 / LoadingDEF0 (512 square, no alpha), the map's interface textures (wiki: Map "
            "Settings); sets size and alpha.\n"
            "- fit: pad (default; the rest is background, or transparent with alpha), crop (fills, cutting the overflow evenly) or stretch (distorts).\n"
            "- out: the file to write, inside the work folder or [Mcp] WriteRoots (default <work folder>\\converted\\<stamp>\\<name>_<w>x<h>.<format>); "
            "an existing file is replaced only with overwrite.\n"
            "Returns the file, its size and a small preview image.";
        t.inputSchema = S::Input(
            {{"file", S::String("Full path of the source image.")},
             {"out", S::String("Full path to write (inside the work folder or [Mcp] WriteRoots). Default: the work folder.")},
             {"size", {{"anyOf", Json::array({{{"type", "integer"}, {"minimum", 1}, {"maximum", 8192}},
                                              {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8192}}}, {"minItems", 2}, {"maxItems", 2}}})},
                       {"description", "A power of two (square), or [width, height] powers of two, up to 8192."}}},
             {"slot", S::Enum({"Menu", "Briefing", "LoadingATT0", "LoadingDEF0"}, "An interface texture slot: sets size (256 or 512) and alpha.")},
             {"format", S::Enum({"tga", "bmp", "png"}, "File format. Default tga.")},
             {"alpha", S::Boolean("Keep transparency (tga, png). Default true (false for the loading screens).")},
             {"fit", S::Enum({"pad", "crop", "stretch"}, "How the picture fills the new size. Default pad.")},
             {"background", S::String("#rrggbb behind padding and flattened alpha. Default #000000.")},
             {"overwrite", S::Boolean("Replace out if it exists. Default false.")}},
            {"file"});
        t.idempotent = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return ConvertImage(a, c); }); };
        registry.Add(std::move(t));
    }
}
}
