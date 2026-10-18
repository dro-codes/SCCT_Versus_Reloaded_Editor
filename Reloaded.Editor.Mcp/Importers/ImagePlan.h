#pragma once
// A raster floor plan (PNG, BMP or TGA, decoded by ImageCodec.h) to
// scct.level-spec, through the grid of GridPlanModel.h. Engine-free; JPEG
// and others need converting first (image_convert, or any editor).
//
// The image is cut into cells of pixelsPerCell pixels (default: the longer
// side over 96, so a plan comes out about 96 cells across), each
// options.cell (128) units, or uuPerPixel x pixelsPerCell when uuPerPixel is
// given. Each pixel is read by the nearest colour of the legend within
// options.tolerance (60), else by its lightness (dark is solid, light is
// floor); transparent pixels are solid. The default legend, which
// options.colors {"#rrggbb": meaning} extends or replaces entries of:
//   #000000 solid      #ffffff floor     #0000ff spy        #ff0000 merc
//   #00ff00 terminal   #ff8000 flag      #ffc000 dropzone   #800080 bomb
//   #c0c000 disk       #ff00ff door      #00ffff vent       #800000 camera
//   #ffff00 light      #008000 ammo      #00ff80 infirmary  #8000ff camnet
//   #ff0080 laser
// A cell is the most common of solid, floor, door and vent in it, or a
// marker when 15% or more of it is that marker's colour. Touching marker
// cells of one kind are one marker (draw one dot per start), at their middle.
// The image is one storey (options.storey, 0). report.overlay is a PNG
// (base64) of how the cells were read, in the legend's colours.
#include "../../Reloaded.Editor/McpBridgeModel.h"
#include "../GridPlanModel.h"
#include "../ImageCodec.h"

namespace Mcp::Import
{
    namespace ImagePlanDetail
    {
        struct Swatch { std::array<int, 3> rgb; char code; };
        inline std::array<int, 3> Hex(const std::string& text)
        {
            auto t = Trim(text);
            if (!t.empty() && t[0] == '#') t = t.substr(1);
            if (t.size() == 3) t = std::string{t[0], t[0], t[1], t[1], t[2], t[2]};
            if (t.size() != 6 || t.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) throw std::runtime_error("Colour \"" + text + "\" is not #rrggbb.");
            return {std::stoi(t.substr(0, 2), nullptr, 16), std::stoi(t.substr(2, 2), nullptr, 16), std::stoi(t.substr(4, 2), nullptr, 16)};
        }
        inline std::vector<Swatch> DefaultLegend()
        {
            const std::vector<std::pair<const char*, char>> base{
                {"#000000", '#'}, {"#ffffff", '.'}, {"#0000ff", 'S'}, {"#ff0000", 'M'}, {"#00ff00", 'O'}, {"#ff8000", 'F'}, {"#ffc000", 'Z'}, {"#800080", 'B'},
                {"#c0c000", 'K'}, {"#ff00ff", 'D'}, {"#00ffff", 'V'}, {"#800000", 'C'}, {"#ffff00", 'L'}, {"#008000", 'A'}, {"#00ff80", 'H'}, {"#8000ff", 'N'}, {"#ff0080", 'X'}};
            std::vector<Swatch> out;
            for (const auto& [hex, code] : base) out.push_back({Hex(hex), code});
            return out;
        }
        inline std::array<uint8_t, 4> ColourOf(char code, const std::vector<Swatch>& legend)
        {
            for (const auto& s : legend) if (s.code == code) return {static_cast<uint8_t>(s.rgb[0]), static_cast<uint8_t>(s.rgb[1]), static_cast<uint8_t>(s.rgb[2]), 255};
            return {128, 128, 128, 255};
        }
    }
    inline Json ImportImagePlan(const std::string& bytes, const Json& options, Json& report)
    {
        using namespace ImagePlanDetail;
        Report(report);
        auto o = Grid::ReadOptions(options);
        const auto image = Image::Decode(bytes);
        const int longer = std::max(image.width, image.height);
        const int ppc = static_cast<int>(Option(options, "pixelsPerCell", std::max(1.0, std::ceil(longer / 96.0)), 1, 4096));
        if (options.is_object() && options.contains("uuPerPixel")) o.cell = Option(options, "uuPerPixel", 1.0, 0.01, 10000) * ppc;
        const double tolerance = Option(options, "tolerance", 60.0, 0, 442);
        const int storey = static_cast<int>(Option(options, "storey", 0.0, -64, 64));
        auto legend = DefaultLegend();
        if (options.is_object() && options.contains("colors"))
        {
            if (!options.at("colors").is_object()) throw std::runtime_error("Option colors maps \"#rrggbb\" to meanings, e.g. {\"#808080\": \"floor\"}.");
            for (auto it = options.at("colors").begin(); it != options.at("colors").end(); ++it)
            {
                const auto rgb = Hex(it.key());
                const char code = it.value().is_string() ? Grid::CodeOf(it.value().get<std::string>()) : '\0';
                if (!code) throw std::runtime_error("Option colors: \"" + it.value().dump() + "\" is not a legend meaning.");
                legend.erase(std::remove_if(legend.begin(), legend.end(), [&](const Swatch& s) { return s.rgb == rgb; }), legend.end());
                legend.insert(legend.begin(), {rgb, code});
            }
        }
        const int cols = (image.width + ppc - 1) / ppc, rows = (image.height + ppc - 1) / ppc;
        if (cols > 1024 || rows > 1024) throw std::runtime_error("The image makes " + std::to_string(cols) + "x" + std::to_string(rows) + " cells; raise pixelsPerCell so it is 1024 or fewer a side.");
        // Each pixel's meaning, then each cell's.
        std::unordered_map<uint32_t, char> cache;
        auto meaning = [&](const uint8_t* p) -> char
        {
            if (p[3] < 128) return '#';
            const uint32_t key = (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
            if (const auto found = cache.find(key); found != cache.end()) return found->second;
            double best = 1e18;
            char code = 0;
            for (const auto& s : legend)
            {
                const double d = std::sqrt(std::pow(p[0] - s.rgb[0], 2) + std::pow(p[1] - s.rgb[1], 2) + std::pow(p[2] - s.rgb[2], 2));
                if (d < best) { best = d; code = s.code; }
            }
            if (best > tolerance) code = (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) < 128 ? '#' : '.';
            cache[key] = code;
            return code;
        };
        std::vector<std::string> grid(static_cast<size_t>(rows), std::string(static_cast<size_t>(cols), '#'));
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c)
            {
                std::map<char, int> counts;
                int total = 0;
                for (int y = r * ppc; y < std::min(image.height, (r + 1) * ppc); ++y)
                    for (int x = c * ppc; x < std::min(image.width, (c + 1) * ppc); ++x) { counts[meaning(image.At(x, y))]++; ++total; }
                char area = '#', marker = 0;
                int areaCount = -1, markerCount = 0;
                for (const auto& [code, n] : counts)
                {
                    if (code == '#' || code == '.' || code == 'D' || code == 'V') { if (n > areaCount) { areaCount = n; area = code; } }
                    else if (n > markerCount) { markerCount = n; marker = code; }
                }
                grid[static_cast<size_t>(r)][static_cast<size_t>(c)] = marker && markerCount * 100 >= total * 15 ? marker : area;
            }
        // One marker per blob, at its middle cell.
        std::vector<std::vector<char>> seen(static_cast<size_t>(rows), std::vector<char>(static_cast<size_t>(cols), 0));
        long long merged = 0;
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c)
            {
                const char code = grid[static_cast<size_t>(r)][static_cast<size_t>(c)];
                if (!Grid::MarkerCode(code) || seen[static_cast<size_t>(r)][static_cast<size_t>(c)]) continue;
                std::vector<std::pair<int, int>> blob{{r, c}};
                seen[static_cast<size_t>(r)][static_cast<size_t>(c)] = 1;
                for (size_t i = 0; i < blob.size(); ++i)
                    for (const auto& [dr, dc] : std::vector<std::pair<int, int>>{{-1, 0}, {1, 0}, {0, -1}, {0, 1}})
                    {
                        const int nr = blob[i].first + dr, nc = blob[i].second + dc;
                        if (nr < 0 || nc < 0 || nr >= rows || nc >= cols || seen[static_cast<size_t>(nr)][static_cast<size_t>(nc)] || grid[static_cast<size_t>(nr)][static_cast<size_t>(nc)] != code) continue;
                        seen[static_cast<size_t>(nr)][static_cast<size_t>(nc)] = 1;
                        blob.push_back({nr, nc});
                    }
                if (blob.size() == 1) continue;
                double mr = 0, mc = 0;
                for (const auto& [br, bc] : blob) { mr += br; mc += bc; }
                mr /= static_cast<double>(blob.size()); mc /= static_cast<double>(blob.size());
                auto keep = blob.front();
                double bestD = 1e18;
                for (const auto& cell : blob)
                {
                    const double d = std::hypot(cell.first - mr, cell.second - mc);
                    if (d < bestD) { bestD = d; keep = cell; }
                }
                for (const auto& cell : blob) if (cell != keep) grid[static_cast<size_t>(cell.first)][static_cast<size_t>(cell.second)] = '.';
                merged += static_cast<long long>(blob.size()) - 1;
            }
        if (merged) Assume(report, "Touching marker cells of one kind were read as one marker each (draw one dot per start, camera or light).");
        // How the cells were read, for the agent to compare with the image.
        if (Option(options, "overlay", true))
        {
            const int k = std::clamp(512 / std::max(cols, rows), 1, 8);
            Image::Rgba overlay(cols * k, rows * k);
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                {
                    const auto colour = ColourOf(grid[static_cast<size_t>(r)][static_cast<size_t>(c)], legend);
                    for (int y = 0; y < k; ++y) for (int x = 0; x < k; ++x) std::memcpy(overlay.At(c * k + x, r * k + y), colour.data(), 4);
                }
            const auto png = Image::EncodePng(overlay);
            report["overlay"] = Bridge::Base64(png);
            report["overlaySize"] = Json::array({overlay.width, overlay.height});
        }
        Assume(report, "The image is read as " + std::to_string(cols) + "x" + std::to_string(rows) + " cells of " + std::to_string(ppc) + " pixels, each " + Num(o.cell).dump() + " units.");
        Count(report, "pixels", static_cast<long long>(image.width) * image.height);
        const std::vector<Grid::Layer> layers{{storey, grid}};
        auto plan = Grid::FromLayers(layers, o, report);
        plan.format = "image";
        Count(report, "storeys", 1);
        if (plan.spaces.empty()) Warn(report, "No floor was found in the image: floor is light (or white) and walls dark; set colors for other palettes.");
        return Grid::ToSpec(plan, o, options, report);
    }
}
