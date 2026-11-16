#pragma once
// Floor plans to scct.level-spec: an ASCII or CSV grid (one character per
// cell), and the plan model the other 2D importers (SVG, Tiled, GeoJSON,
// raster plans) fill in world units before the same builder writes the spec.
// Engine-free and Windows-free.
//
// The grid legend (options.legend {"char": meaning} adds or overrides one):
//   #  solid              .  floor             S  spy start      M  merc start
//   O  terminal objective F  flag              Z  flag drop zone B  bomb target
//   K  disk               D  door              V  vent           C  camera
//   L  light              A  ammo              H  infirmary      N  CamNet camera
//   X  laser
// A space is solid; letters match either case; any other character is floor
// (with a warning). In CSV each cell holds a legend character or a meaning
// name ("floor", "spy", "camnet", ...); an empty cell is solid.
// "--- storey N ---" (or floor/level N) starts storey N (negative below
// ground); rows before the first separator are storey 0.
//
// What the builder makes (options in brackets, with their defaults):
//   - Row 1 is the north edge and columns run west to east; each cell is
//     cell (128) units and the grid is centred on the origin (center: true;
//     origin [x, y] places the north-west corner instead).
//   - Storey N stands at z = N x storeyHeight (384).
//   - The floor cells of a storey are cut into the fewest large rectangles
//     (greedy maximal rectangles). Each is a Room roomHeight (320) tall, or a
//     Corridor when its short side is corridorMax (256) or less and it is
//     longer than wide. V cells become Vents ventHeight (128) tall.
//   - Rectangles that touch are joined by a full-width opening without a
//     zone portal (autoDoorways: true): carved spaces that touch are open to
//     each other anyway, and the joins tell Map Design and the rules how
//     the rooms connect.
//   - A block of D cells between two spaces becomes a doorway through that
//     wall, doorHeight (196) tall, without a portal (portals: false); with
//     doorMesh, a door mover in it too.
//   - Markers become actors in the room they stand in ({in, u, v}): starts
//     facing the objectives, terminals, flags (each Z goes to the nearest
//     flag), bombs, disks (groups 1/2/3 in turn), cameras facing away from
//     the wall beside them, CamNet cameras (the first is bFirstCam), lasers
//     spanning their room, lights (and defaults.lights "none"), ammo and
//     infirmaries. Cameras and lasers share one alarm.
#include "Importers/Common.h"
#include <cstdlib>
#include <functional>
#include <optional>
#include <regex>
#include <utility>

namespace Mcp::Grid
{
    using Json = Workflow::Json;
    using Import::Num;
    using P2 = std::array<double, 2>;
    constexpr double kPi = 3.14159265358979323846;

    // --- The legend --------------------------------------------------------------
    struct Meaning { char code; const char* name; };
    inline const std::vector<Meaning>& Legend()
    {
        static const std::vector<Meaning> legend = {
            {'#', "solid"}, {'.', "floor"}, {'S', "spy"}, {'M', "merc"}, {'O', "terminal"}, {'F', "flag"}, {'Z', "dropzone"}, {'B', "bomb"},
            {'K', "disk"}, {'D', "door"}, {'V', "vent"}, {'C', "camera"}, {'L', "light"}, {'A', "ammo"}, {'H', "infirmary"}, {'N', "camnet"}, {'X', "laser"}};
        return legend;
    }
    // A meaning by name or a common synonym: "floor", "spawn-spy", "terminal".
    inline char CodeOf(const std::string& meaning)
    {
        const auto key = Import::Lower(Import::Trim(meaning));
        if (key.size() == 1)
            for (const auto& m : Legend()) if (std::toupper(static_cast<unsigned char>(key[0])) == m.code || key[0] == m.code) return m.code;
        static const std::map<std::string, char> names{
            {"solid", '#'}, {"wall", '#'}, {"rock", '#'}, {"void", '#'}, {"none", '#'},
            {"floor", '.'}, {"open", '.'}, {"room", '.'}, {"empty", '.'}, {"corridor", '.'}, {"space", '.'},
            {"spy", 'S'}, {"spystart", 'S'}, {"spawnspy", 'S'}, {"spies", 'S'},
            {"merc", 'M'}, {"mercstart", 'M'}, {"spawnmerc", 'M'}, {"mercenary", 'M'}, {"mercs", 'M'},
            {"terminal", 'O'}, {"objective", 'O'}, {"computer", 'O'}, {"flag", 'F'}, {"dropzone", 'Z'}, {"drop", 'Z'},
            {"bomb", 'B'}, {"bombtarget", 'B'}, {"disk", 'K'}, {"datadisk", 'K'}, {"door", 'D'}, {"doorway", 'D'},
            {"vent", 'V'}, {"crawlway", 'V'}, {"camera", 'C'}, {"securitycamera", 'C'}, {"light", 'L'}, {"lamp", 'L'},
            {"ammo", 'A'}, {"infirmary", 'H'}, {"health", 'H'}, {"medkit", 'H'}, {"camnet", 'N'}, {"camnetwork", 'N'}, {"laser", 'X'}};
        std::string squashed;
        for (unsigned char c : key) if (std::isalnum(c)) squashed += static_cast<char>(c);
        const auto found = names.find(squashed);
        return found == names.end() ? '\0' : found->second;
    }
    inline const char* NameOf(char code)
    {
        for (const auto& m : Legend()) if (m.code == code) return m.name;
        return "floor";
    }
    inline bool MarkerCode(char code) { return std::string("SMOFZBKCLAHNX").find(code) != std::string::npos; }

    // --- Options -----------------------------------------------------------------
    struct Options
    {
        double cell = 128, storeyHeight = 384, roomHeight = 320, ventHeight = 128, doorHeight = 196, corridorMax = 256;
        bool center = true, autoDoorways = true, portals = false;
        P2 origin{0, 0};
        std::string doorMesh, material;
        Json legend = Json::object();
    };
    inline Options ReadOptions(const Json& options)
    {
        Options o;
        o.cell = Import::Option(options, "cell", o.cell, 8, 4096);
        o.storeyHeight = Import::Option(options, "storeyHeight", o.storeyHeight, 64, 8192);
        o.roomHeight = Import::Option(options, "roomHeight", o.roomHeight, 32, 8192);
        o.ventHeight = Import::Option(options, "ventHeight", o.ventHeight, 32, 8192);
        o.doorHeight = Import::Option(options, "doorHeight", o.doorHeight, 32, 8192);
        o.corridorMax = Import::Option(options, "corridorMax", o.corridorMax, 0, 65536);
        o.autoDoorways = Import::Option(options, "autoDoorways", o.autoDoorways);
        o.portals = Import::Option(options, "portals", o.portals);
        o.doorMesh = Import::Option(options, "doorMesh", "");
        o.material = Import::Option(options, "material", "");
        if (options.is_object() && options.contains("origin"))
        {
            const auto& origin = options.at("origin");
            if (!origin.is_array() || origin.size() != 2 || !origin[0].is_number() || !origin[1].is_number()) throw std::runtime_error("Option origin is [x, y], the north-west corner of the plan.");
            o.origin = {origin[0].get<double>(), origin[1].get<double>()};
            o.center = false;
        }
        o.center = Import::Option(options, "center", o.center);
        if (options.is_object() && options.contains("legend"))
        {
            if (!options.at("legend").is_object()) throw std::runtime_error("Option legend maps characters to meanings, e.g. {\"~\": \"floor\"}.");
            o.legend = options.at("legend");
        }
        return o;
    }

    // --- Parsing a grid ------------------------------------------------------------
    struct Layer { int number = 0; std::vector<std::string> rows; };
    inline std::string StoreyId(int number) { return number >= 0 ? "Storey" + std::to_string(number) : "StoreyB" + std::to_string(-number); }
    // Characters as legend codes; the storeys in the order the text gives them.
    inline std::vector<Layer> ParseLayers(const std::string& text, const Options& o, Json& report)
    {
        static const std::regex separator(R"(^\s*-{2,}\s*(storey|story|floor|level)\s*(-?\d+)\s*-{2,}\s*$)", std::regex::icase);
        std::map<char, char> custom;
        for (auto it = o.legend.begin(); it != o.legend.end(); ++it)
        {
            if (it.key().size() != 1 || !it.value().is_string()) throw std::runtime_error("Each legend entry maps one character to a meaning name, e.g. {\"~\": \"floor\"}.");
            const char code = CodeOf(it.value().get<std::string>());
            if (!code) throw std::runtime_error("Legend meaning \"" + it.value().get<std::string>() + "\" is unknown; use solid, floor, spy, merc, terminal, flag, dropzone, bomb, disk, door, vent, camera, light, ammo, infirmary, camnet or laser.");
            custom[it.key()[0]] = code;
        }
        const auto lines = Import::Lines(text);
        // CSV when the grid lines carry a delimiter.
        bool csv = false;
        for (const auto& line : lines)
            if (!std::regex_match(line, separator) && !Import::StartsWith(Import::Trim(line), "//") && (line.find(',') != std::string::npos || line.find(';') != std::string::npos || line.find('\t') != std::string::npos)) { csv = true; break; }
        const char delimiter = csv ? Import::SniffDelimiter(text) : '\0';
        std::set<std::string> unknown;
        auto code = [&](const std::string& cell) -> char
        {
            if (cell.size() == 1 && custom.count(cell[0])) return custom.at(cell[0]);
            if (csv)
            {
                const auto t = Import::Trim(cell);
                if (t.empty()) return '#';
                if (t.size() == 1 && custom.count(t[0])) return custom.at(t[0]);
                const char c = CodeOf(t);
                if (c) return c;
                unknown.insert(t);
                return '.';
            }
            const char ch = cell[0];
            if (ch == ' ' || ch == '\t') return '#';
            for (const auto& m : Legend()) if (ch == m.code || std::toupper(static_cast<unsigned char>(ch)) == m.code) return m.code;
            unknown.insert(std::string(1, ch));
            return '.';
        };
        std::vector<Layer> layers;
        Layer current;
        bool started = false;
        std::set<int> numbers;
        auto finish = [&]
        {
            // Blank lines at the end are layout; a row of walls is not.
            while (!current.rows.empty() && current.rows.back().empty()) current.rows.pop_back();
            if (started || !current.rows.empty())
            {
                if (!numbers.insert(current.number).second) throw std::runtime_error("Storey " + std::to_string(current.number) + " appears twice in the grid.");
                layers.push_back(current);
            }
        };
        for (const auto& line : lines)
        {
            std::smatch m;
            if (std::regex_match(line, m, separator))
            {
                finish();
                current = Layer();
                current.number = std::stoi(m[2].str());
                started = true;
                continue;
            }
            if (Import::StartsWith(Import::Trim(line), "//")) continue;
            if (Import::Trim(line).empty() || (csv && line.find_first_not_of(" \t,;") == std::string::npos))
            {
                if (!current.rows.empty()) current.rows.push_back(std::string());
                continue;
            }
            std::string row;
            if (csv)
            {
                const auto parsed = Import::ParseCsv(line, delimiter);
                if (!parsed.empty()) for (const auto& cell : parsed.front()) row += code(cell);
            }
            else for (char ch : line) row += code(std::string(1, ch));
            current.rows.push_back(row);
        }
        finish();
        auto cellless = [](const Layer& l) { return std::all_of(l.rows.begin(), l.rows.end(), [](const std::string& r) { return r.find_first_not_of('#') == std::string::npos; }); };
        if (layers.empty() || std::all_of(layers.begin(), layers.end(), cellless))
            throw std::runtime_error("The grid has no cells. Draw the plan with # for solid and . for floor, one character per cell.");
        for (const auto& l : layers)
        {
            if (l.rows.size() > 1024) throw std::runtime_error("A grid storey has " + std::to_string(l.rows.size()) + " rows; 1024 is the most.");
            for (const auto& r : l.rows) if (r.size() > 1024) throw std::runtime_error("A grid row has " + std::to_string(r.size()) + " cells; 1024 is the most.");
        }
        if (layers.size() > 64) throw std::runtime_error("The grid has more than 64 storeys.");
        if (!unknown.empty())
        {
            std::string list;
            for (const auto& u : unknown) list += (list.empty() ? "" : ", ") + ("'" + u + "'");
            Import::Warn(report, "Unknown grid cells " + list + " were read as floor; map them with options.legend.");
        }
        return layers;
    }

    // --- Rectangles ------------------------------------------------------------------
    struct Rect { int c0 = 0, r0 = 0, c1 = 0, r1 = 0; int Area() const { return (c1 - c0) * (r1 - r0); } }; // cells [c0, c1) x [r0, r1)
    // Row runs merged downwards while they keep the same span: linear time,
    // for plans too large for the greedy search.
    inline std::vector<Rect> Scanline(const std::vector<std::vector<char>>& mask)
    {
        std::vector<Rect> out;
        std::map<std::pair<int, int>, size_t> open;
        for (size_t r = 0; r < mask.size(); ++r)
        {
            std::map<std::pair<int, int>, size_t> next;
            const int cols = static_cast<int>(mask[r].size());
            for (int c = 0; c < cols;)
            {
                if (!mask[r][static_cast<size_t>(c)]) { ++c; continue; }
                int end = c;
                while (end < cols && mask[r][static_cast<size_t>(end)]) ++end;
                const auto span = std::make_pair(c, end);
                const auto found = open.find(span);
                if (found != open.end()) { out[found->second].r1 = static_cast<int>(r) + 1; next[span] = found->second; }
                else { out.push_back({c, static_cast<int>(r), end, static_cast<int>(r) + 1}); next[span] = out.size() - 1; }
                c = end;
            }
            open = std::move(next);
        }
        return out;
    }
    // Greedy maximal rectangles: the largest rectangle of the cells left
    // (the northernmost, then westernmost, of equals) until none are left.
    // Past kLargePlan open cells the scanline merge is used instead.
    constexpr size_t kLargePlan = 65536;
    inline std::vector<Rect> Decompose(std::vector<std::vector<char>> mask)
    {
        size_t cells = 0;
        for (const auto& row : mask) cells += static_cast<size_t>(std::count(row.begin(), row.end(), 1));
        if (cells > kLargePlan) return Scanline(mask);
        std::vector<Rect> out;
        const int rows = static_cast<int>(mask.size()), cols = rows ? static_cast<int>(mask[0].size()) : 0;
        std::vector<int> heights(static_cast<size_t>(cols));
        for (;;)
        {
            Rect best;
            int bestArea = 0;
            std::fill(heights.begin(), heights.end(), 0);
            for (int r = 0; r < rows; ++r)
            {
                for (int c = 0; c < cols; ++c) heights[static_cast<size_t>(c)] = mask[static_cast<size_t>(r)][static_cast<size_t>(c)] ? heights[static_cast<size_t>(c)] + 1 : 0;
                // Largest rectangle in the histogram ending at this row.
                std::vector<int> stack;
                for (int c = 0; c <= cols; ++c)
                {
                    const int h = c < cols ? heights[static_cast<size_t>(c)] : 0;
                    while (!stack.empty() && heights[static_cast<size_t>(stack.back())] >= h)
                    {
                        const int top = stack.back();
                        stack.pop_back();
                        const int height = heights[static_cast<size_t>(top)];
                        if (!height) continue;
                        const int left = stack.empty() ? 0 : stack.back() + 1;
                        const Rect candidate{left, r - height + 1, c, r + 1};
                        const int area = candidate.Area();
                        if (area > bestArea || (area == bestArea && (candidate.r0 < best.r0 || (candidate.r0 == best.r0 && candidate.c0 < best.c0))))
                        {
                            best = candidate;
                            bestArea = area;
                        }
                    }
                    stack.push_back(c);
                }
            }
            if (!bestArea) break;
            for (int r = best.r0; r < best.r1; ++r)
                for (int c = best.c0; c < best.c1; ++c) mask[static_cast<size_t>(r)][static_cast<size_t>(c)] = 0;
            out.push_back(best);
        }
        return out;
    }

    // --- Polygons ----------------------------------------------------------------------
    inline double SignedArea(const std::vector<P2>& poly)
    {
        double a = 0;
        for (size_t i = 0; i < poly.size(); ++i)
        {
            const auto& p = poly[i];
            const auto& q = poly[(i + 1) % poly.size()];
            a += p[0] * q[1] - q[0] * p[1];
        }
        return a / 2;
    }
    inline double Cross(const P2& o, const P2& a, const P2& b) { return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]); }
    // Counter-clockwise, without repeated or collinear points.
    inline std::vector<P2> Clean(std::vector<P2> poly)
    {
        std::vector<P2> out;
        for (const auto& p : poly)
            if (out.empty() || std::hypot(p[0] - out.back()[0], p[1] - out.back()[1]) > 0.01) out.push_back(p);
        while (out.size() > 1 && std::hypot(out.front()[0] - out.back()[0], out.front()[1] - out.back()[1]) <= 0.01) out.pop_back();
        for (bool changed = true; changed && out.size() >= 3;)
        {
            changed = false;
            for (size_t i = 0; i < out.size() && out.size() >= 3; ++i)
            {
                const auto& a = out[(i + out.size() - 1) % out.size()];
                const auto& b = out[i];
                const auto& c = out[(i + 1) % out.size()];
                const double len = std::hypot(b[0] - a[0], b[1] - a[1]) * std::hypot(c[0] - b[0], c[1] - b[1]);
                if (len < 1e-9 || std::abs(Cross(a, b, c)) < 1e-6 * len) { out.erase(out.begin() + static_cast<std::ptrdiff_t>(i)); changed = true; break; }
            }
        }
        if (out.size() >= 3 && SignedArea(out) < 0) std::reverse(out.begin(), out.end());
        return out;
    }
    inline bool Convex(const std::vector<P2>& poly)
    {
        for (size_t i = 0; i < poly.size(); ++i)
            if (Cross(poly[i], poly[(i + 1) % poly.size()], poly[(i + 2) % poly.size()]) < -1e-9) return false;
        return true;
    }
    inline bool Inside(const std::vector<P2>& poly, double x, double y)
    {
        bool in = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
            if (((poly[i][1] > y) != (poly[j][1] > y)) && x < (poly[j][0] - poly[i][0]) * (y - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0]) in = !in;
        return in;
    }
    // Whether two edges that share no corner cross.
    inline bool SelfIntersects(const std::vector<P2>& poly)
    {
        const size_t n = poly.size();
        auto crosses = [](const P2& a, const P2& b, const P2& c, const P2& d)
        {
            const double d1 = Cross(c, d, a), d2 = Cross(c, d, b), d3 = Cross(a, b, c), d4 = Cross(a, b, d);
            return ((d1 > 1e-9 && d2 < -1e-9) || (d1 < -1e-9 && d2 > 1e-9)) && ((d3 > 1e-9 && d4 < -1e-9) || (d3 < -1e-9 && d4 > 1e-9));
        };
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 2; j < n; ++j)
            {
                if (i == 0 && j == n - 1) continue;
                if (crosses(poly[i], poly[(i + 1) % n], poly[j], poly[(j + 1) % n])) return true;
            }
        return false;
    }
    // A simple polygon as convex pieces: ear clipping, then neighbouring
    // pieces merged while the result stays convex (Hertel-Mehlhorn).
    inline std::vector<std::vector<P2>> MergedTriangles(const std::vector<P2>& poly)
    {
        std::vector<std::vector<size_t>> parts;
        std::vector<size_t> remaining(poly.size());
        for (size_t i = 0; i < remaining.size(); ++i) remaining[i] = i;
        size_t guard = 0;
        while (remaining.size() > 3 && guard++ < poly.size() * poly.size())
        {
            bool clipped = false;
            for (size_t i = 0; i < remaining.size(); ++i)
            {
                const size_t a = remaining[(i + remaining.size() - 1) % remaining.size()], b = remaining[i], c = remaining[(i + 1) % remaining.size()];
                if (Cross(poly[a], poly[b], poly[c]) <= 1e-9) continue;
                bool ear = true;
                for (size_t k : remaining)
                {
                    if (k == a || k == b || k == c) continue;
                    if (Cross(poly[a], poly[b], poly[k]) >= 0 && Cross(poly[b], poly[c], poly[k]) >= 0 && Cross(poly[c], poly[a], poly[k]) >= 0) { ear = false; break; }
                }
                if (!ear) continue;
                parts.push_back({a, b, c});
                remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
                clipped = true;
                break;
            }
            if (!clipped) throw std::runtime_error("A polygon crosses itself, so it cannot be split into convex brushes; redraw it as a simple outline.");
        }
        if (remaining.size() == 3) parts.push_back(remaining);
        auto convex = [&](const std::vector<size_t>& part)
        {
            for (size_t i = 0; i < part.size(); ++i)
                if (Cross(poly[part[i]], poly[part[(i + 1) % part.size()]], poly[part[(i + 2) % part.size()]]) < -1e-9) return false;
            return true;
        };
        for (bool merged = true; merged;)
        {
            merged = false;
            for (size_t i = 0; i < parts.size() && !merged; ++i)
                for (size_t j = i + 1; j < parts.size() && !merged; ++j)
                {
                    const auto& A = parts[i];
                    const auto& B = parts[j];
                    for (size_t ai = 0; ai < A.size() && !merged; ++ai)
                    {
                        const size_t u = A[ai], v = A[(ai + 1) % A.size()];
                        const auto bj = std::find(B.begin(), B.end(), v);
                        if (bj == B.end() || B[static_cast<size_t>(bj - B.begin() + 1) % B.size()] != u) continue;
                        // A runs ...u v...; B runs ...v u...: join them across u-v.
                        std::vector<size_t> joined;
                        for (size_t k = 0; k < A.size(); ++k) joined.push_back(A[(ai + 1 + k) % A.size()]);
                        const size_t start = static_cast<size_t>(bj - B.begin());
                        for (size_t k = 2; k < B.size(); ++k) joined.push_back(B[(start + k) % B.size()]);
                        if (!convex(joined)) continue;
                        parts[i] = joined;
                        parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(j));
                        merged = true;
                    }
                }
        }
        std::vector<std::vector<P2>> out;
        for (const auto& part : parts)
        {
            std::vector<P2> p;
            for (size_t k : part) p.push_back(poly[k]);
            p = Clean(p);
            if (p.size() >= 3) out.push_back(p);
        }
        return out;
    }
    // A simple polygon as convex pieces: the fewest the merge finds from any
    // starting corner (plans' polygons are small, so trying each is cheap).
    inline std::vector<std::vector<P2>> ConvexParts(const std::vector<P2>& polygon)
    {
        const auto poly = Clean(polygon);
        if (poly.size() < 3) return {};
        if (SelfIntersects(poly)) throw std::runtime_error("A polygon crosses itself, so it cannot be split into convex brushes; redraw it as a simple outline.");
        if (Convex(poly)) return {poly};
        std::vector<std::vector<P2>> best;
        const size_t tries = std::min<size_t>(poly.size(), 64);
        for (size_t r = 0; r < tries; ++r)
        {
            std::vector<P2> rotated(poly.begin() + static_cast<std::ptrdiff_t>(r), poly.end());
            rotated.insert(rotated.end(), poly.begin(), poly.begin() + static_cast<std::ptrdiff_t>(r));
            auto parts = MergedTriangles(rotated);
            if (best.empty() || parts.size() < best.size()) best = std::move(parts);
            if (best.size() <= 2) break;
        }
        return best;
    }
    // An axis-aligned rectangle given as a polygon: its corners, or nothing.
    inline std::optional<std::array<double, 4>> AxisRectangle(const std::vector<P2>& polygon)
    {
        const auto p = Clean(polygon);
        if (p.size() != 4) return std::nullopt;
        double x0 = p[0][0], x1 = x0, y0 = p[0][1], y1 = y0;
        for (const auto& q : p) { x0 = std::min(x0, q[0]); x1 = std::max(x1, q[0]); y0 = std::min(y0, q[1]); y1 = std::max(y1, q[1]); }
        for (const auto& q : p)
            if ((std::abs(q[0] - x0) > 0.01 && std::abs(q[0] - x1) > 0.01) || (std::abs(q[1] - y0) > 0.01 && std::abs(q[1] - y1) > 0.01)) return std::nullopt;
        return std::array<double, 4>{x0, y0, x1, y1};
    }

    // --- The plan ------------------------------------------------------------------------
    // Everything in Unreal units, X east, Y north; z is a storey's floor.
    struct Space { std::string id, kind, name, storey, material; double x0 = 0, y0 = 0, x1 = 0, y1 = 0, z = 0, height = 320; bool explicitKind = false; bool turned = false; };
    // An extruded convex polygon: a carved space (CSG_Subtract) or a solid
    // pillar (CSG_Add) that is not a rectangle.
    struct Prism { std::string id, csg = "CSG_Subtract", storey, name, material; std::vector<P2> points; double z = 0, height = 320; };
    struct Marker
    {
        std::string kind, id, name, storey, cls, mesh, sound;
        double x = 0, y = 0;
        std::optional<double> z, h, yaw, radius, brightness;
        Json props = Json::object();
    };
    struct Segment { std::string kind, id, storey; double x0 = 0, y0 = 0, x1 = 0, y1 = 0; std::optional<double> h0, h1; };
    // A doorway: where it is and how wide; ResolveDoors finds the wall.
    struct Door { std::string id, storey, name; double x = 0, y = 0, width = 0, depth = 0; bool alongX = true; std::optional<bool> portal; std::string host, side, other; double offset = 0; double z = 0; };
    struct StoreyInfo { std::string id; int number = 0; double z = 0; std::string name; };
    struct Plan
    {
        std::vector<StoreyInfo> storeys;
        std::vector<Space> spaces;
        std::vector<Prism> prisms;
        std::vector<Marker> markers;
        std::vector<Segment> segments;
        std::vector<Door> doors;
        Import::Ids ids;
        std::string format = "grid";
        const StoreyInfo* Storey(const std::string& id) const
        {
            for (const auto& s : storeys) if (Import::Lower(s.id) == Import::Lower(id)) return &s;
            return nullptr;
        }
        double StoreyZ(const std::string& id) const { const auto* s = Storey(id); return s ? s->z : 0.0; }
        // The space a point is in on a storey: the smallest that holds it.
        const Space* SpaceAt(double x, double y, const std::string& storey) const
        {
            const Space* best = nullptr;
            for (const auto& s : spaces)
            {
                if (Import::Lower(s.storey) != Import::Lower(storey)) continue;
                if (s.kind != "Room" && s.kind != "Corridor" && s.kind != "Vent" && s.kind != "Crawlway") continue;
                if (x < s.x0 - 0.5 || x > s.x1 + 0.5 || y < s.y0 - 0.5 || y > s.y1 + 0.5) continue;
                if (!best || (s.x1 - s.x0) * (s.y1 - s.y0) < (best->x1 - best->x0) * (best->y1 - best->y0)) best = &s;
            }
            return best;
        }
    };
    // A rectangle's kind and orientation: a Corridor when it is narrow and
    // long; corridors and vents run along their longer side (local Y).
    inline void Shape(Space& s, const Options& o)
    {
        const double dx = s.x1 - s.x0, dy = s.y1 - s.y0;
        if (!s.explicitKind && s.kind == "Room" && std::min(dx, dy) <= o.corridorMax && std::abs(dx - dy) > 0.5) s.kind = "Corridor";
        s.turned = (s.kind == "Corridor" || s.kind == "Vent" || s.kind == "Crawlway" || s.kind == "Stairs" || s.kind == "Ramp") && dx > dy + 0.5;
    }

    // A doorway's host wall: the space on each side of it, the nearer within
    // 512 units. The host is the north (or west) one when both are found.
    inline bool ResolveDoor(Door& d, const Plan& plan, Json& report)
    {
        const double reach = 512;
        const Space* sides[2] = {nullptr, nullptr};
        double faces[2] = {0, 0};
        for (int s = 0; s < 2; ++s)
        {
            const double sign = s == 0 ? 1 : -1;
            for (double t = d.depth / 2 + 8; t <= d.depth / 2 + reach && !sides[s]; t += 16)
            {
                const double px = d.alongX ? d.x : d.x + sign * t, py = d.alongX ? d.y + sign * t : d.y;
                if (const auto* space = plan.SpaceAt(px, py, d.storey))
                {
                    sides[s] = space;
                    faces[s] = d.alongX ? (s == 0 ? space->y0 : space->y1) : (s == 0 ? space->x0 : space->x1);
                }
            }
        }
        // Index 0 is north (alongX) or east; prefer north or west as host.
        const Space* host = d.alongX ? (sides[0] ? sides[0] : sides[1]) : (sides[1] ? sides[1] : sides[0]);
        if (!host)
        {
            Import::Warn(report, "Doorway " + d.id + " is not beside any room within 512 units, so it was left out.");
            return false;
        }
        const Space* other = host == sides[0] ? sides[1] : sides[0];
        if (d.alongX) d.side = host == sides[0] ? "south" : "north";
        else d.side = host == sides[1] ? "east" : "west";
        // The opening spans the wall between the two spaces.
        if (other) d.depth = std::max(16.0, std::abs(faces[0] - faces[1]));
        else d.depth = std::max(16.0, d.depth);
        const double lo = std::max(d.alongX ? host->x0 : host->y0, (d.alongX ? d.x : d.y) - d.width / 2), hi = std::min(d.alongX ? host->x1 : host->y1, (d.alongX ? d.x : d.y) + d.width / 2);
        if (hi - lo < 32)
        {
            Import::Warn(report, "Doorway " + d.id + " barely meets the wall of " + host->id + ", so it was left out.");
            return false;
        }
        d.width = hi - lo;
        d.offset = (lo + hi) / 2 - (d.alongX ? (host->x0 + host->x1) / 2 : (host->y0 + host->y1) / 2);
        d.host = host->id;
        d.other = other ? other->id : std::string();
        d.z = host->z;
        return true;
    }

    // --- A grid as a plan ------------------------------------------------------------------
    inline Plan FromLayers(const std::vector<Layer>& layers, const Options& o, Json& report)
    {
        Plan plan;
        size_t width = 0, height = 0;
        for (const auto& l : layers) { height = std::max(height, l.rows.size()); for (const auto& r : l.rows) width = std::max(width, r.size()); }
        const P2 corner = o.center ? P2{-static_cast<double>(width) * o.cell / 2, static_cast<double>(height) * o.cell / 2} : o.origin;
        auto cx = [&](double c) { return corner[0] + c * o.cell; };
        auto cy = [&](double r) { return corner[1] - r * o.cell; };
        std::vector<Layer> sorted = layers;
        std::sort(sorted.begin(), sorted.end(), [](const Layer& a, const Layer& b) { return a.number < b.number; });
        long long cells = 0, floors = 0;
        for (const auto& layer : sorted)
        {
            StoreyInfo storey{StoreyId(layer.number), layer.number, layer.number * o.storeyHeight, {}};
            plan.ids.Reserve(storey.id);
            plan.storeys.push_back(storey);
            std::vector<std::string> g(height, std::string(width, '#'));
            for (size_t r = 0; r < layer.rows.size(); ++r) g[r].replace(0, layer.rows[r].size(), layer.rows[r]);
            auto at = [&](long long r, long long c) -> char { return r < 0 || c < 0 || r >= static_cast<long long>(height) || c >= static_cast<long long>(width) ? '#' : g[static_cast<size_t>(r)][static_cast<size_t>(c)]; };
            auto open = [&](long long r, long long c) { const char ch = at(r, c); return ch != '#' && ch != 'D'; };
            // Door blocks: a rectangle of D cells with open cells on two
            // opposite sides. Anything else is read as floor.
            struct Block { int r0, r1, c0, c1; bool alongX; };
            std::vector<Block> blocks;
            std::vector<std::vector<char>> seen(height, std::vector<char>(width, 0));
            for (size_t r = 0; r < height; ++r)
                for (size_t c = 0; c < width; ++c)
                {
                    if (g[r][c] != 'D' || seen[r][c]) continue;
                    std::vector<std::pair<size_t, size_t>> component{{r, c}};
                    seen[r][c] = 1;
                    for (size_t i = 0; i < component.size(); ++i)
                    {
                        const auto [rr, cc] = component[i];
                        const std::pair<long long, long long> steps[4] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                        for (const auto& [dr, dc] : steps)
                        {
                            const long long nr = static_cast<long long>(rr) + dr, nc = static_cast<long long>(cc) + dc;
                            if (at(nr, nc) == 'D' && !seen[static_cast<size_t>(nr)][static_cast<size_t>(nc)]) { seen[static_cast<size_t>(nr)][static_cast<size_t>(nc)] = 1; component.push_back({static_cast<size_t>(nr), static_cast<size_t>(nc)}); }
                        }
                    }
                    int r0 = static_cast<int>(r), r1 = r0, c0 = static_cast<int>(c), c1 = c0;
                    for (const auto& [rr, cc] : component) { r0 = std::min(r0, static_cast<int>(rr)); r1 = std::max(r1, static_cast<int>(rr)); c0 = std::min(c0, static_cast<int>(cc)); c1 = std::max(c1, static_cast<int>(cc)); }
                    const bool full = component.size() == static_cast<size_t>((r1 - r0 + 1) * (c1 - c0 + 1));
                    bool north = false, south = false, west = false, east = false;
                    for (int k = c0; k <= c1; ++k) { north = north || open(r0 - 1, k); south = south || open(r1 + 1, k); }
                    for (int k = r0; k <= r1; ++k) { west = west || open(k, c0 - 1); east = east || open(k, c1 + 1); }
                    const bool ns = north && south, ew = west && east;
                    if (full && ns != ew) { blocks.push_back({r0, r1, c0, c1, ns}); continue; }
                    for (const auto& [rr, cc] : component) g[rr][cc] = '.';
                    Import::Warn(report, "Door cells at row " + std::to_string(r0 + 1) + ", column " + std::to_string(c0 + 1) + " (storey " + std::to_string(layer.number) +
                                             ") do not sit in a wall between two open areas, so they were read as floor.");
                }
            // Spaces: rooms and corridors from the open cells, vents from V.
            std::vector<std::vector<char>> roomMask(height, std::vector<char>(width, 0)), ventMask = roomMask;
            size_t layerFloors = 0;
            for (size_t r = 0; r < height; ++r)
                for (size_t c = 0; c < width; ++c)
                {
                    const char ch = g[r][c];
                    if (ch == '#') continue;
                    ++cells;
                    if (ch == 'D') continue;
                    ++floors;
                    ++layerFloors;
                    (ch == 'V' ? ventMask : roomMask)[r][c] = 1;
                }
            if (layerFloors > kLargePlan)
                Import::Warn(report, "A storey has over " + std::to_string(kLargePlan) + " floor cells, so it was cut into rows rather than the largest rectangles; a larger cell (or pixelsPerCell) gives fewer, larger rooms.");
            for (int pass = 0; pass < 2; ++pass)
                for (const auto& rect : Decompose(pass == 0 ? roomMask : ventMask))
                {
                    Space s;
                    s.kind = pass == 0 ? "Room" : "Vent";
                    s.storey = storey.id;
                    s.x0 = cx(rect.c0); s.x1 = cx(rect.c1); s.y1 = cy(rect.r0); s.y0 = cy(rect.r1);
                    s.z = storey.z;
                    s.height = pass == 0 ? o.roomHeight : o.ventHeight;
                    s.material = o.material;
                    Shape(s, o);
                    s.id = plan.ids.Next(s.kind);
                    plan.spaces.push_back(s);
                }
            for (const auto& b : blocks)
            {
                Door d;
                d.id = plan.ids.Next("Door");
                d.storey = storey.id;
                d.alongX = b.alongX;
                d.x = (cx(b.c0) + cx(b.c1 + 1)) / 2;
                d.y = (cy(b.r0) + cy(b.r1 + 1)) / 2;
                d.width = (b.alongX ? b.c1 - b.c0 + 1 : b.r1 - b.r0 + 1) * o.cell;
                d.depth = (b.alongX ? b.r1 - b.r0 + 1 : b.c1 - b.c0 + 1) * o.cell;
                plan.doors.push_back(d);
            }
            // Markers, and which way cameras look: away from the wall beside them.
            for (size_t r = 0; r < height; ++r)
                for (size_t c = 0; c < width; ++c)
                {
                    const char ch = g[r][c];
                    if (!MarkerCode(ch)) continue;
                    Marker m;
                    m.kind = NameOf(ch);
                    m.storey = storey.id;
                    m.x = cx(static_cast<double>(c) + 0.5);
                    m.y = cy(static_cast<double>(r) + 0.5);
                    if (ch == 'C' || ch == 'N')
                    {
                        double dx = 0, dy = 0;
                        const long long rr = static_cast<long long>(r), cc = static_cast<long long>(c);
                        if (!open(rr - 1, cc)) dy -= 1;
                        if (!open(rr + 1, cc)) dy += 1;
                        if (!open(rr, cc - 1)) dx += 1;
                        if (!open(rr, cc + 1)) dx -= 1;
                        if (std::abs(dx) + std::abs(dy) > 0) m.yaw = Import::Degrees(std::atan2(dy, dx));
                    }
                    plan.markers.push_back(m);
                }
        }
        if (plan.spaces.size() > 2000) Import::Warn(report, std::to_string(plan.spaces.size()) + " spaces is a great many for one level; use a larger cell size.");
        Import::Count(report, "cells", cells);
        Import::Count(report, "floorCells", floors);
        Import::Assume(report, "Each grid cell is " + Import::Num(o.cell).dump() + " units square; row 1 is the north edge and columns run west to east" +
                                   (o.center ? std::string(", centred on the origin.") : ", from the north-west corner at the origin option."));
        return plan;
    }

    // --- The plan as a spec -----------------------------------------------------------------
    namespace Detail
    {
        // Where markers stand above the floor when no room holds them,
        // matching the compiler's usual heights.
        inline double UsualHeight(const std::string& kind, double roomHeight)
        {
            if (kind == "spy" || kind == "merc" || kind == "disk" || kind == "laser" || kind == "motion" || kind == "presence") return 48;
            if (kind == "terminal") return 78;
            if (kind == "flag" || kind == "bomb" || kind == "dropzone" || kind == "alarm") return 64;
            if (kind == "camera" || kind == "camnet") return std::min(200.0, roomHeight - 24);
            if (kind == "light") return roomHeight - 24;
            return 0;
        }
        inline const std::set<std::string>& IngredientKinds()
        {
            static const std::set<std::string> kinds{"ammo", "infirmary", "glass", "extinguisher", "projector", "sprinkler", "puddle", "balltrap"};
            return kinds;
        }
        inline const std::set<std::string>& GeKinds()
        {
            static const std::set<std::string> kinds{"ledge", "hoh", "pipe", "ladder", "zipline", "fence", "pole"};
            return kinds;
        }
        // A marker or line kind from what plans call it ("spawn-spy", "Terminal", "zip line").
        inline std::string KindOf(const std::string& text)
        {
            std::string key;
            for (unsigned char c : Import::Lower(text)) if (std::isalnum(c)) key += static_cast<char>(c);
            static const std::map<std::string, std::string> names{
                {"spy", "spy"}, {"spystart", "spy"}, {"spawnspy", "spy"}, {"spies", "spy"}, {"merc", "merc"}, {"mercstart", "merc"}, {"spawnmerc", "merc"}, {"mercenary", "merc"},
                {"terminal", "terminal"}, {"objective", "terminal"}, {"computer", "terminal"}, {"flag", "flag"}, {"dropzone", "dropzone"}, {"flagdropzone", "dropzone"},
                {"bomb", "bomb"}, {"bombtarget", "bomb"}, {"disk", "disk"}, {"datadisk", "disk"}, {"camera", "camera"}, {"securitycamera", "camera"},
                {"camnet", "camnet"}, {"camnetwork", "camnet"}, {"laser", "laser"}, {"motion", "motion"}, {"motionsensor", "motion"}, {"presence", "presence"},
                {"mine", "mine"}, {"alarm", "alarm"}, {"light", "light"}, {"lamp", "light"}, {"ammo", "ammo"}, {"infirmary", "infirmary"}, {"health", "infirmary"},
                {"glass", "glass"}, {"extinguisher", "extinguisher"}, {"projector", "projector"}, {"sprinkler", "sprinkler"}, {"puddle", "puddle"}, {"balltrap", "balltrap"},
                {"sound", "sound"}, {"mesh", "mesh"}, {"prop", "mesh"}, {"staticmesh", "mesh"}, {"actor", "actor"}, {"door", "door"}, {"doorway", "door"},
                {"ledge", "ledge"}, {"ledgegrab", "ledge"}, {"hoh", "hoh"}, {"handoverhand", "hoh"}, {"pipe", "pipe"}, {"ladder", "ladder"},
                {"zipline", "zipline"}, {"zip", "zipline"}, {"fence", "fence"}, {"pole", "pole"},
                {"room", "Room"}, {"corridor", "Corridor"}, {"hall", "Room"}, {"vent", "Vent"}, {"crawlway", "Crawlway"}, {"platform", "Platform"},
                {"stairs", "Stairs"}, {"ramp", "Ramp"}, {"floor", "Room"}, {"solid", "solid"}, {"wall", "solid"}, {"pillar", "solid"}, {"column", "solid"}, {"obstacle", "solid"}};
            auto found = names.find(key);
            // Groups and layers name kinds in the plural: rooms, lasers.
            if (found == names.end() && key.size() > 3 && key.back() == 's') found = names.find(key.substr(0, key.size() - 1));
            return found == names.end() ? std::string() : found->second;
        }
        // Default heights of a geometric event above its floor.
        inline std::pair<double, double> GeHeights(const std::string& kind)
        {
            if (kind == "ledge") return {208, 208};
            if (kind == "hoh") return {216, 216};
            if (kind == "zipline") return {256, 256};
            if (kind == "fence") return {96, 96};
            return {0, 256}; // pole, pipe, ladder: bottom and top
        }
    }
    // The plan as a scct.level-spec. options are the importer's (name, path,
    // doorMesh, ...); format names the source in provenance.
    inline Json ToSpec(Plan& plan, const Options& o, const Json& options, Json& report)
    {
        using namespace Detail;
        Import::Report(report);
        Json spec = Import::NewSpec(options, plan.format);
        auto& defaults = Import::Section(spec, {"defaults"});
        defaults["construction"] = "Carve";
        if (!o.material.empty()) defaults["material"] = o.material;
        // Storeys.
        std::sort(plan.storeys.begin(), plan.storeys.end(), [](const StoreyInfo& a, const StoreyInfo& b) { return a.z < b.z; });
        for (const auto& s : plan.storeys)
        {
            Json item{{"id", s.id}, {"z", Num(s.z)}};
            if (!s.name.empty()) item["name"] = s.name;
            Import::List(spec, {"storeys"}).push_back(item);
        }
        // Pieces.
        for (auto& s : plan.spaces)
        {
            Shape(s, o);
            const double dx = s.x1 - s.x0, dy = s.y1 - s.y0;
            Json piece{{"id", s.id}, {"kind", s.kind}, {"size", s.turned ? Json::array({Num(dy), Num(dx), Num(s.height)}) : Json::array({Num(dx), Num(dy), Num(s.height)})},
                       {"at", Import::Point((s.x0 + s.x1) / 2, (s.y0 + s.y1) / 2)}};
            if (!s.storey.empty()) piece["storey"] = s.storey;
            else if (s.z != 0) piece["at"].push_back(Num(s.z));
            if (s.turned) piece["yaw"] = 90;
            if (!s.name.empty()) piece["name"] = s.name;
            if (!s.material.empty() && s.material != o.material) piece["material"] = s.material;
            Import::List(spec, {"geometry", "pieces"}).push_back(piece);
            Import::Count(report, Import::Lower(s.kind) == "room" ? "rooms" : Import::Lower(s.kind) == "corridor" ? "corridors" : Import::Lower(s.kind) == "vent" ? "vents" : "pieces");
        }
        // Prisms: carved spaces first, then the solids inside them.
        std::stable_sort(plan.prisms.begin(), plan.prisms.end(), [](const Prism& a, const Prism& b) { return a.csg == "CSG_Subtract" && b.csg != "CSG_Subtract"; });
        for (const auto& prism : plan.prisms)
        {
            auto points = Clean(prism.points);
            if (points.size() < 3) continue;
            double ax = 0, ay = 0;
            for (const auto& p : points) { ax += p[0]; ay += p[1]; }
            ax = std::round(ax / static_cast<double>(points.size())); ay = std::round(ay / static_cast<double>(points.size()));
            const double z0 = prism.z, h = prism.height;
            auto local = [&](const P2& p, double z) { return Import::Point(p[0] - ax, p[1] - ay, z); };
            Json polygons = Json::array();
            auto cap = [&](bool top)
            {
                // At most 16 vertices a polygon: fans of the convex outline.
                for (size_t first = 1; first + 1 < points.size();)
                {
                    const size_t last = std::min(points.size() - 1, first + 14);
                    std::vector<size_t> fan{0};
                    for (size_t k = first; k <= last; ++k) fan.push_back(k);
                    Json vertices = Json::array();
                    if (top) for (size_t k : fan) vertices.push_back(local(points[k], h));
                    else for (auto it = fan.rbegin(); it != fan.rend(); ++it) vertices.push_back(local(points[*it], 0));
                    Json polygon{{"vertices", vertices}};
                    if (!prism.material.empty()) polygon["texture"] = prism.material;
                    polygons.push_back(polygon);
                    first = last;
                }
            };
            cap(true);
            cap(false);
            for (size_t i = 0; i < points.size(); ++i)
            {
                const auto& a = points[i];
                const auto& b = points[(i + 1) % points.size()];
                Json polygon{{"vertices", Json::array({local(a, 0), local(b, 0), local(b, h), local(a, h)})}};
                if (!prism.material.empty()) polygon["texture"] = prism.material;
                polygons.push_back(polygon);
            }
            Json brush{{"id", prism.id}, {"csg", prism.csg}, {"at", Import::Point(ax, ay, z0)}, {"polygons", polygons}};
            Import::List(spec, {"geometry", "brushes"}).push_back(brush);
            Import::Count(report, prism.csg == "CSG_Subtract" ? "polygonSpaces" : "polygonSolids");
        }
        // Doorways in walls, then the joins between touching spaces.
        for (auto& d : plan.doors)
        {
            if (d.host.empty() && !ResolveDoor(d, plan, report)) continue;
            const Space* host = nullptr;
            const Space* other = nullptr;
            for (const auto& s : plan.spaces) { if (s.id == d.host) host = &s; if (s.id == d.other) other = &s; }
            double h = o.doorHeight;
            if (host) h = std::min(h, host->height);
            if (other) h = std::min(h, other->height);
            Json doorway{{"id", d.id}, {"host", d.host}, {"side", d.side}, {"offset", Num(d.offset)}, {"size", Json::array({Num(d.width), Num(h)})}, {"depth", Num(d.depth)},
                         {"portal", d.portal.value_or(o.portals)}};
            if (!d.name.empty()) doorway["name"] = d.name;
            Import::List(spec, {"geometry", "doorways"}).push_back(doorway);
            Import::Count(report, "doorways");
            if (!o.doorMesh.empty())
            {
                Json door{{"id", plan.ids.Make(d.id + "_Mover", "Door")}, {"mesh", o.doorMesh}, {"at", {{"doorway", d.id}}}};
                Import::List(spec, {"mechanisms", "doors"}).push_back(door);
                Import::Count(report, "doorMovers");
            }
        }
        if (o.autoDoorways)
        {
            for (size_t i = 0; i < plan.spaces.size(); ++i)
                for (size_t j = i + 1; j < plan.spaces.size(); ++j)
                {
                    const auto& a = plan.spaces[i];
                    const auto& b = plan.spaces[j];
                    if (Import::Lower(a.storey) != Import::Lower(b.storey) || std::abs(a.z - b.z) > 1) continue;
                    auto space = [](const Space& s) { return s.kind == "Room" || s.kind == "Corridor" || s.kind == "Vent" || s.kind == "Crawlway"; };
                    if (!space(a) || !space(b)) continue;
                    double overlap = 0;
                    if (std::abs(a.x1 - b.x0) < 0.5 || std::abs(b.x1 - a.x0) < 0.5) overlap = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
                    else if (std::abs(a.y1 - b.y0) < 0.5 || std::abs(b.y1 - a.y0) < 0.5) overlap = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
                    if (overlap < 32) continue;
                    // The taller space hosts, so the opening fits its wall.
                    const bool aHosts = a.height >= b.height;
                    Json join{{"id", plan.ids.Next("Join")}, {"between", Json::array({aHosts ? a.id : b.id, aHosts ? b.id : a.id})},
                              {"size", Json::array({Num(overlap), Num(std::min(a.height, b.height))})}, {"portal", false}};
                    Import::List(spec, {"geometry", "doorways"}).push_back(join);
                    Import::Count(report, "joins");
                }
            // A rectangle against a carved polygon: an opening in the
            // rectangle's wall where a polygon edge runs along it.
            for (const auto& a : plan.spaces)
            {
                if (a.kind != "Room" && a.kind != "Corridor" && a.kind != "Vent" && a.kind != "Crawlway") continue;
                for (const auto& prism : plan.prisms)
                {
                    if (prism.csg != "CSG_Subtract" || Import::Lower(prism.storey) != Import::Lower(a.storey) || std::abs(prism.z - a.z) > 1) continue;
                    struct Side { const char* name; bool vertical; double at, lo, hi, centre; };
                    const Side sides[4] = {{"east", true, a.x1, a.y0, a.y1, (a.y0 + a.y1) / 2}, {"west", true, a.x0, a.y0, a.y1, (a.y0 + a.y1) / 2},
                                           {"north", false, a.y1, a.x0, a.x1, (a.x0 + a.x1) / 2}, {"south", false, a.y0, a.x0, a.x1, (a.x0 + a.x1) / 2}};
                    const Side* bestSide = nullptr;
                    double bestLo = 0, bestHi = 0;
                    for (const auto& side : sides)
                        for (size_t i = 0; i < prism.points.size(); ++i)
                        {
                            const auto& p = prism.points[i];
                            const auto& q = prism.points[(i + 1) % prism.points.size()];
                            const int across = side.vertical ? 0 : 1, along = 1 - across;
                            if (std::abs(p[static_cast<size_t>(across)] - side.at) > 0.5 || std::abs(q[static_cast<size_t>(across)] - side.at) > 0.5) continue;
                            const double lo = std::max(side.lo, std::min(p[static_cast<size_t>(along)], q[static_cast<size_t>(along)]));
                            const double hi = std::min(side.hi, std::max(p[static_cast<size_t>(along)], q[static_cast<size_t>(along)]));
                            if (hi - lo >= 32 && (!bestSide || hi - lo > bestHi - bestLo)) { bestSide = &side; bestLo = lo; bestHi = hi; }
                        }
                    if (!bestSide) continue;
                    Json join{{"id", plan.ids.Next("Join")}, {"host", a.id}, {"side", bestSide->name}, {"offset", Num((bestLo + bestHi) / 2 - bestSide->centre)},
                              {"size", Json::array({Num(bestHi - bestLo), Num(std::min(a.height, prism.height))})}, {"depth", 16}, {"portal", false}};
                    Import::List(spec, {"geometry", "doorways"}).push_back(join);
                    Import::Count(report, "joins");
                }
            }
            if (report.at("counts").value("joins", 0))
                Import::Assume(report, "Spaces that touch are joined by full-width openings without zone portals (autoDoorways false leaves them out).");
        }

        // Where a marker stands: in its room ({in, u, v}) or at a height.
        auto position = [&](const Marker& m, const std::string& kind) -> Json
        {
            if (!m.z)
                if (const auto* s = plan.SpaceAt(m.x, m.y, m.storey))
                {
                    const double dx = s->x1 - s->x0, dy = s->y1 - s->y0, cx = (s->x0 + s->x1) / 2, cy = (s->y0 + s->y1) / 2;
                    double u = s->turned ? (m.y - cy) / dy : (m.x - cx) / dx, v = s->turned ? -(m.x - cx) / dx : (m.y - cy) / dy;
                    u = std::clamp(std::round(u * 10000) / 10000, -0.5, 0.5);
                    v = std::clamp(std::round(v * 10000) / 10000, -0.5, 0.5);
                    Json at{{"in", s->id}, {"u", u == 0 ? Json(0) : Json(u)}, {"v", v == 0 ? Json(0) : Json(v)}};
                    if (m.h) at["h"] = "floor+" + Import::Num(*m.h).dump();
                    return at;
                }
            double z = m.z ? *m.z : plan.StoreyZ(m.storey) + (m.h ? *m.h : UsualHeight(kind, o.roomHeight));
            if (!m.z) Import::Warn(report, "Some " + kind + " markers stand outside every rectangular room, so they were placed at an absolute height.");
            return Import::Point(m.x, m.y, z);
        };
        // Starts face the objectives, or the middle of the level.
        double tx = 0, ty = 0;
        int targets = 0;
        for (const auto& m : plan.markers)
            if (m.kind == "terminal" || m.kind == "flag" || m.kind == "bomb") { tx += m.x; ty += m.y; ++targets; }
        if (!targets)
        {
            for (const auto& s : plan.spaces) { tx += (s.x0 + s.x1) / 2; ty += (s.y0 + s.y1) / 2; ++targets; }
            for (const auto& p : plan.prisms) for (const auto& q : p.points) { tx += q[0] / static_cast<double>(p.points.size()); ty += q[1] / static_cast<double>(p.points.size()); ++targets; }
        }
        if (targets) { tx /= targets; ty /= targets; }
        auto idFor = [&](const Marker& m, const std::string& prefix) { return m.id.empty() ? plan.ids.Next(prefix) : plan.ids.Make(m.id, prefix); };

        // Flags first, so drop zones can join the nearest one.
        std::vector<const Marker*> flags, zones;
        for (const auto& m : plan.markers) { if (m.kind == "flag") flags.push_back(&m); if (m.kind == "dropzone") zones.push_back(&m); }
        std::map<const Marker*, std::vector<const Marker*>> zonesOf;
        for (const auto* z : zones)
        {
            const Marker* best = nullptr;
            double bestD = 1e18;
            for (const auto* f : flags)
            {
                const double d = std::hypot(f->x - z->x, f->y - z->y) + std::abs(plan.StoreyZ(f->storey) - plan.StoreyZ(z->storey)) * 4;
                if (d < bestD) { bestD = d; best = f; }
            }
            if (!best) { Import::Warn(report, "Flag drop zones without any flag were left out."); continue; }
            if (zonesOf[best].size() >= 5) { Import::Warn(report, "A flag has more than 5 drop zones; the extra ones were left out."); continue; }
            zonesOf[best].push_back(z);
        }
        std::string alarmId;
        bool needAlarm = false;
        for (const auto& m : plan.markers) needAlarm = needAlarm || m.kind == "camera" || m.kind == "laser" || m.kind == "motion" || m.kind == "presence" || m.kind == "mine";
        for (const auto& s : plan.segments) needAlarm = needAlarm || s.kind == "laser";
        for (const auto& m : plan.markers)
            if (m.kind == "alarm" && alarmId.empty())
            {
                alarmId = idFor(m, "Alarm");
                Json alarm{{"id", alarmId}, {"at", position(m, "alarm")}};
                if (!m.name.empty()) alarm["name"] = m.name;
                Import::List(spec, {"security", "alarms"}).push_back(alarm);
            }
        if (needAlarm && alarmId.empty())
        {
            alarmId = plan.ids.Next("Alarm");
            Import::List(spec, {"security", "alarms"}).push_back({{"id", alarmId}});
            Import::Assume(report, "Cameras and lasers all report to one alarm, " + alarmId + ".");
        }
        int disks = 0, camnets = 0;
        std::map<std::string, int> perKind;
        bool lights = false;
        std::set<std::string> modes;
        for (const auto& m : plan.markers)
        {
            const auto& k = m.kind;
            if (k == "spy" || k == "merc")
            {
                Json start{{"id", idFor(m, k == "spy" ? "SpyStart" : "MercStart")}, {"at", position(m, k)}};
                double yaw = m.yaw ? *m.yaw : (std::hypot(tx - m.x, ty - m.y) > 1 ? Import::Degrees(std::atan2(ty - m.y, tx - m.x)) : 0.0);
                start["yaw"] = Num(yaw);
                Import::List(spec, {"spawns", k == "spy" ? "spy" : "merc"}).push_back(start);
                Import::Count(report, k == "spy" ? "spyStarts" : "mercStarts");
            }
            else if (k == "terminal" || k == "flag" || k == "bomb")
            {
                const char* label = k == "terminal" ? "Terminal" : k == "flag" ? "Flag" : "Bomb";
                Json trigger{{"id", idFor(m, label)}, {"kind", k}, {"at", position(m, k)}};
                if (m.yaw) trigger["yaw"] = Num(*m.yaw);
                if (k == "flag")
                {
                    Json dropZones = Json::array();
                    for (const auto* z : zonesOf[&m]) dropZones.push_back({{"id", idFor(*z, "DropZone")}, {"at", position(*z, "dropzone")}});
                    if (!dropZones.empty()) trigger["dropZones"] = dropZones;
                    else Import::Warn(report, "Flag " + trigger.at("id").get<std::string>() + " has no drop zone; add Z cells (1 to 5 per flag) near where it is carried.");
                }
                Json objective{{"id", plan.ids.Next("Objective")}, {"name", m.name.empty() ? std::string(label) + " " + std::to_string(++perKind[k]) : m.name}, {"triggers", Json::array({trigger})}};
                Import::List(spec, {"gameplay", "story", "objectives"}).push_back(objective);
                Import::Count(report, "objectives");
                modes.insert("story");
            }
            else if (k == "disk")
            {
                Import::List(spec, {"gameplay", "diskhunt", "disks"}).push_back({{"id", idFor(m, "Disk")}, {"at", position(m, k)}, {"group", disks % 3 + 1}});
                ++disks;
                Import::Count(report, "disks");
                modes.insert("diskhunt");
            }
            else if (k == "camera" || k == "laser" || k == "motion" || k == "presence" || k == "mine")
            {
                Json device{{"id", idFor(m, k == "camera" ? "Camera" : k == "laser" ? "Laser" : k == "motion" ? "Motion" : k == "presence" ? "Presence" : "Mine")}, {"kind", k}, {"at", position(m, k)}};
                if (m.yaw) device["facing"] = Num(*m.yaw);
                if (!alarmId.empty()) device["alarm"] = alarmId;
                if (!m.props.empty()) device["props"] = m.props;
                Import::List(spec, {"security", "devices"}).push_back(device);
                Import::Count(report, k == "camera" ? "cameras" : k == "laser" ? "lasers" : "sensors");
            }
            else if (k == "camnet")
            {
                Json camera{{"id", idFor(m, "CamNet")}, {"at", position(m, k)}, {"first", camnets == 0}};
                if (m.yaw) camera["yaw"] = Num(*m.yaw);
                if (!m.name.empty()) camera["camName"] = m.name;
                Import::List(spec, {"security", "camnet"}).push_back(camera);
                ++camnets;
                Import::Count(report, "camnet");
            }
            else if (k == "light")
            {
                Json light{{"id", idFor(m, "Light")}, {"preset", "ceiling"}, {"at", position(m, k)}};
                if (m.radius) light["radius"] = Num(*m.radius);
                if (m.brightness) light["brightness"] = Num(std::clamp(*m.brightness, 0.0, 255.0));
                if (!m.props.empty()) light["props"] = m.props;
                Import::List(spec, {"environment", "lights"}).push_back(light);
                lights = true;
                Import::Count(report, "lights");
            }
            else if (IngredientKinds().count(k))
            {
                Json ingredient{{"id", idFor(m, k == "ammo" ? "Ammo" : k == "infirmary" ? "Infirmary" : "Ingredient")}, {"kind", k}, {"at", position(m, k)}};
                if (m.yaw) ingredient["yaw"] = Num(*m.yaw);
                Import::List(spec, {"environment", "ingredients"}).push_back(ingredient);
                Import::Count(report, "ingredients");
            }
            else if (k == "sound")
            {
                if (m.sound.empty()) { Import::Warn(report, "Sound markers need a sound asset (Sound'Package.Group.Name'); those without one were left out."); continue; }
                Json sound{{"id", idFor(m, "Sound")}, {"sound", m.sound}, {"at", position(m, k)}};
                if (m.radius) sound["radius"] = Num(*m.radius);
                Import::List(spec, {"environment", "sounds"}).push_back(sound);
                Import::Count(report, "sounds");
            }
            else if (k == "mesh" || (k == "actor" && m.cls.empty() && !m.mesh.empty()))
            {
                if (m.mesh.empty()) { Import::Warn(report, "Mesh markers need a mesh (StaticMesh'Package.Group.Name'); those without one were left out."); continue; }
                Json mesh{{"id", idFor(m, "Mesh")}, {"mesh", m.mesh}, {"at", position(m, k)}};
                if (m.yaw) mesh["yaw"] = Num(*m.yaw);
                if (!m.props.empty()) mesh["props"] = m.props;
                Import::List(spec, {"geometry", "meshes"}).push_back(mesh);
                Import::Count(report, "meshes");
            }
            else if (k == "actor")
            {
                if (m.cls.empty()) { Import::Warn(report, "Actor markers need a class; those without one were left out."); continue; }
                Json actor{{"id", idFor(m, "Actor")}, {"class", m.cls}, {"at", position(m, k)}};
                if (m.yaw) actor["yaw"] = Num(*m.yaw);
                if (!m.mesh.empty()) actor["mesh"] = m.mesh;
                if (!m.props.empty()) actor["properties"] = m.props;
                Import::List(spec, {"environment", "props"}).push_back(actor);
                Import::Count(report, "actors");
            }
            else if (k != "dropzone" && k != "alarm")
                Import::Warn(report, "Markers of kind \"" + k + "\" have no level-spec meaning here and were left out.");
        }
        if (lights)
        {
            defaults["lights"] = "none";
            Import::Assume(report, "The plan places its own lights, so rooms get no automatic ceiling lights (defaults.lights \"none\").");
        }
        // Lines: lasers across, and geometric events.
        int ges = 0;
        for (const auto& s : plan.segments)
        {
            const double length = std::hypot(s.x1 - s.x0, s.y1 - s.y0);
            const double floorZ = plan.StoreyZ(s.storey);
            if (s.kind == "laser")
            {
                Marker start;
                start.x = s.x0; start.y = s.y0; start.storey = s.storey; start.h = s.h0;
                Json device{{"id", s.id.empty() ? plan.ids.Next("Laser") : plan.ids.Make(s.id, "Laser")}, {"kind", "laser"}, {"at", position(start, "laser")},
                            {"facing", Num(Import::Degrees(std::atan2(s.y1 - s.y0, s.x1 - s.x0)))}, {"length", Num(length)}};
                if (!alarmId.empty()) device["alarm"] = alarmId;
                Import::List(spec, {"security", "devices"}).push_back(device);
                Import::Count(report, "lasers");
                continue;
            }
            if (!GeKinds().count(s.kind)) { Import::Warn(report, "Lines of kind \"" + s.kind + "\" have no level-spec meaning and were left out."); continue; }
            const auto heights = GeHeights(s.kind);
            const double h0 = s.h0.value_or(heights.first), h1 = s.h1.value_or(heights.second);
            Json points = Json::array();
            if (s.kind == "pipe" || s.kind == "ladder" || s.kind == "pole")
            {
                // A vertical element at the line's first point; a pipe or
                // ladder is climbed from the side the line points to.
                points.push_back(Import::Point(s.x0, s.y0, floorZ + h0));
                points.push_back(Import::Point(s.x0, s.y0, floorZ + h1));
                if (s.kind != "pole")
                {
                    const double ux = length > 0 ? (s.x1 - s.x0) / length : 1, uy = length > 0 ? (s.y1 - s.y0) / length : 0;
                    points.push_back(Import::Point(s.x0 + ux * 32, s.y0 + uy * 32, floorZ + h0));
                }
            }
            else
            {
                points.push_back(Import::Point(s.x0, s.y0, floorZ + h0));
                points.push_back(Import::Point(s.x1, s.y1, floorZ + h1));
            }
            Json ge{{"kind", s.kind}, {"points", points}};
            if (!s.id.empty()) ge["id"] = plan.ids.Make(s.id, "GE");
            Import::List(spec, {"ges"}).push_back(ge);
            ++ges;
            Import::Count(report, "ges");
        }
        if (ges) Import::Assume(report, "Geometric events stand at default heights above their floor (ledge 208, hand-over-hand 216, zip line 256, fence 96, poles, pipes and ladders 0 to 256) unless the plan gives them.");
        if (!modes.empty())
        {
            Json list = Json::array();
            for (const char* mode : {"story", "diskhunt", "deathmatch"}) if (modes.count(mode)) list.push_back(mode);
            Import::Section(spec, {"gameplay"})["modes"] = list;
        }
        spec["provenance"]["notes"] = report.at("assumptions");
        Import::Prune(spec);
        return spec;
    }

    // An ASCII or CSV grid as a scct.level-spec (see the header).
    inline Json ImportText(const std::string& text, const Json& options, Json& report)
    {
        Import::Report(report);
        const auto o = ReadOptions(options);
        const auto layers = ParseLayers(text, o, report);
        auto plan = FromLayers(layers, o, report);
        plan.format = "grid";
        Import::Count(report, "storeys", static_cast<long long>(plan.storeys.size()));
        Import::Assume(report, "Storey N stands at z = N x " + Import::Num(o.storeyHeight).dump() + "; rooms are " + Import::Num(o.roomHeight).dump() + " units tall and vents " +
                                   Import::Num(o.ventHeight).dump() + "; a rectangle " + Import::Num(o.corridorMax).dump() + " units wide or less is a corridor.");
        if (!plan.doors.empty())
            Import::Assume(report, "D cells are doorways " + Import::Num(o.doorHeight).dump() + " units tall" + (o.portals ? " with" : " without") + " zone portals" +
                                       (o.doorMesh.empty() ? "; pass doorMesh for door movers." : ", each with a door mover."));
        return ToSpec(plan, o, options, report);
    }
}
