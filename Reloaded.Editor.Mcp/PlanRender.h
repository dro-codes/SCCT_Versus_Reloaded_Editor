#pragma once
// Top-down plans as PNG, without the editor: plan_render draws a level spec
// (LevelSpecCompiler's Compiled.render) or the live map (SceneFromLive) so an
// agent can compare a layout with its source before building anything.
// Engine-free and Windows-free; PNG through ImageCodec.h.
//
// The render scene. Unreal units, X east, Y north, Z up; every list optional.
//   {bounds:{min:[x,y,z], max:[x,y,z]},       the area drawn (default: everything)
//    pieces:[{id, kind, name, storey, box:{min, max}, yaw (degrees),
//             outline:[[x,y]...] (optional footprint; box otherwise),
//             space (bool), zone}],
//      kind: Room|Corridor|Vent|Crawlway|Doorway|Platform|Stairs|Stairs L|
//            Stairs U|Spiral|Ramp (spec pieces), or for live brushes
//            add|subtract|volume|portal|mover. Spec pieces are drawn lowest
//            first; live brushes in their CSG order, so a subtractive hull
//            and the additive solids inside it come out as built.
//    doorways:[{id, at:[x,y,z], width, yaw (degrees: the opening runs along
//               the doorway's local X), portal (bool)}],
//    actors:[{id, cls, kind, at:[x,y,z] (or position), yaw (degrees), label,
//             fov (degrees, cameras), range (cameras), radius (lights)}],
//      kind: spawn-spy|spawn-merc|spawn|objective|flag|dropzone|bomb|disk|
//            camera|camnet|laser|motion|presence|mine|alarm|light|door|lift|
//            button|sound|volume|ingredient|ge|mesh|other ("_" works for "-",
//            and a missing or vague kind is worked out from cls),
//    lines:[{from:[x,y,z], to:[x,y,z], kind (laser|camnet|alarm|event|lock|
//            motion|lift|ge-<ledge|hoh|pipe|ladder|zipline|fence|pole>)}],
//    labels:[{at:[x,y,z], text, kind (zone|room)}]}
//
// Options (all optional):
//   maxEdge (64-4096, default 1024): the longer side of the image in pixels
//   storey: a storey id (or its index, lowest 0) to draw alone; the other
//           storeys show as faint outlines
//   labels (true), grid (true), legend (true), actorLabels (false),
//   cones (true: camera views), title (text), bounds ({min, max}: override)
#include "../Reloaded.Editor/WorkflowModel.h"
#include "ImageCodec.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Mcp::Render
{
    using Json = Workflow::Json;
    struct Colour { uint8_t r = 0, g = 0, b = 0, a = 255; };
    constexpr double kPi = 3.14159265358979323846;

    // 5x8 glyphs for ASCII 32-126, one byte per column, bit 0 the top row.
    namespace Font
    {
        constexpr int kWidth = 5, kHeight = 8, kAdvance = 6;
        inline const uint8_t* Glyph(char c)
        {
            static const uint8_t table[95 * 5] = {
                0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, 0x00,0x07,0x00,0x07,0x00, 0x14,0x7F,0x14,0x7F,0x14,
                0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62, 0x36,0x49,0x56,0x20,0x50, 0x00,0x08,0x07,0x03,0x00,
                0x00,0x1C,0x22,0x41,0x00, 0x00,0x41,0x22,0x1C,0x00, 0x2A,0x1C,0x7F,0x1C,0x2A, 0x08,0x08,0x3E,0x08,0x08,
                0x00,0x80,0x70,0x30,0x00, 0x08,0x08,0x08,0x08,0x08, 0x00,0x00,0x60,0x60,0x00, 0x20,0x10,0x08,0x04,0x02,
                0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00, 0x72,0x49,0x49,0x49,0x46, 0x21,0x41,0x49,0x4D,0x33,
                0x18,0x14,0x12,0x7F,0x10, 0x27,0x45,0x45,0x45,0x39, 0x3C,0x4A,0x49,0x49,0x31, 0x41,0x21,0x11,0x09,0x07,
                0x36,0x49,0x49,0x49,0x36, 0x46,0x49,0x49,0x29,0x1E, 0x00,0x00,0x14,0x00,0x00, 0x00,0x40,0x34,0x00,0x00,
                0x00,0x08,0x14,0x22,0x41, 0x14,0x14,0x14,0x14,0x14, 0x00,0x41,0x22,0x14,0x08, 0x02,0x01,0x59,0x09,0x06,
                0x3E,0x41,0x5D,0x59,0x4E, 0x7C,0x12,0x11,0x12,0x7C, 0x7F,0x49,0x49,0x49,0x36, 0x3E,0x41,0x41,0x41,0x22,
                0x7F,0x41,0x41,0x41,0x3E, 0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x09,0x01, 0x3E,0x41,0x41,0x51,0x73,
                0x7F,0x08,0x08,0x08,0x7F, 0x00,0x41,0x7F,0x41,0x00, 0x20,0x40,0x41,0x3F,0x01, 0x7F,0x08,0x14,0x22,0x41,
                0x7F,0x40,0x40,0x40,0x40, 0x7F,0x02,0x1C,0x02,0x7F, 0x7F,0x04,0x08,0x10,0x7F, 0x3E,0x41,0x41,0x41,0x3E,
                0x7F,0x09,0x09,0x09,0x06, 0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46, 0x26,0x49,0x49,0x49,0x32,
                0x03,0x01,0x7F,0x01,0x03, 0x3F,0x40,0x40,0x40,0x3F, 0x1F,0x20,0x40,0x20,0x1F, 0x3F,0x40,0x38,0x40,0x3F,
                0x63,0x14,0x08,0x14,0x63, 0x03,0x04,0x78,0x04,0x03, 0x61,0x59,0x49,0x4D,0x43, 0x00,0x7F,0x41,0x41,0x41,
                0x02,0x04,0x08,0x10,0x20, 0x00,0x41,0x41,0x41,0x7F, 0x04,0x02,0x01,0x02,0x04, 0x40,0x40,0x40,0x40,0x40,
                0x00,0x03,0x07,0x08,0x00, 0x20,0x54,0x54,0x78,0x40, 0x7F,0x28,0x44,0x44,0x38, 0x38,0x44,0x44,0x44,0x28,
                0x38,0x44,0x44,0x28,0x7F, 0x38,0x54,0x54,0x54,0x18, 0x00,0x08,0x7E,0x09,0x02, 0x18,0xA4,0xA4,0x9C,0x78,
                0x7F,0x08,0x04,0x04,0x78, 0x00,0x44,0x7D,0x40,0x00, 0x20,0x40,0x40,0x3D,0x00, 0x7F,0x10,0x28,0x44,0x00,
                0x00,0x41,0x7F,0x40,0x00, 0x7C,0x04,0x78,0x04,0x78, 0x7C,0x08,0x04,0x04,0x78, 0x38,0x44,0x44,0x44,0x38,
                0xFC,0x18,0x24,0x24,0x18, 0x18,0x24,0x24,0x18,0xFC, 0x7C,0x08,0x04,0x04,0x08, 0x48,0x54,0x54,0x54,0x24,
                0x04,0x04,0x3F,0x44,0x24, 0x3C,0x40,0x40,0x20,0x7C, 0x1C,0x20,0x40,0x20,0x1C, 0x3C,0x40,0x30,0x40,0x3C,
                0x44,0x28,0x10,0x28,0x44, 0x4C,0x90,0x90,0x90,0x7C, 0x44,0x64,0x54,0x4C,0x44, 0x00,0x08,0x36,0x41,0x00,
                0x00,0x00,0x77,0x00,0x00, 0x00,0x41,0x36,0x08,0x00, 0x02,0x01,0x02,0x04,0x02};
            const unsigned char u = static_cast<unsigned char>(c);
            return &table[(u >= 32 && u <= 126 ? u - 32 : '?' - 32) * 5];
        }
    }

    // A raster with the few primitives a plan needs. Coordinates are pixels
    // (fractions allowed); every primitive blends by its colour's alpha.
    class Canvas
    {
    public:
        Image::Rgba image;
        Canvas(int width, int height, Colour background) : image(width, height, {background.r, background.g, background.b, 255}) {}
        int Width() const { return image.width; }
        int Height() const { return image.height; }
        void Plot(int x, int y, Colour c)
        {
            if (x < 0 || y < 0 || x >= image.width || y >= image.height || c.a == 0) return;
            uint8_t* p = image.At(x, y);
            if (c.a == 255) { p[0] = c.r; p[1] = c.g; p[2] = c.b; return; }
            const int a = c.a, keep = 255 - a;
            p[0] = static_cast<uint8_t>((c.r * a + p[0] * keep + 127) / 255);
            p[1] = static_cast<uint8_t>((c.g * a + p[1] * keep + 127) / 255);
            p[2] = static_cast<uint8_t>((c.b * a + p[2] * keep + 127) / 255);
        }
        // Pixels whose centres fall inside [x0, x1) x [y0, y1).
        void FillRect(double x0, double y0, double x1, double y1, Colour c)
        {
            const int ix0 = std::max(0, static_cast<int>(std::ceil(std::min(x0, x1) - 0.5))), ix1 = std::min(image.width, static_cast<int>(std::ceil(std::max(x0, x1) - 0.5)));
            const int iy0 = std::max(0, static_cast<int>(std::ceil(std::min(y0, y1) - 0.5))), iy1 = std::min(image.height, static_cast<int>(std::ceil(std::max(y0, y1) - 0.5)));
            for (int y = iy0; y < iy1; ++y)
                for (int x = ix0; x < ix1; ++x) Plot(x, y, c);
        }
        // Even-odd scanline fill at pixel centres; each pixel is painted once.
        void FillPolygon(const std::vector<std::pair<double, double>>& points, Colour c)
        {
            if (points.size() < 3) return;
            double top = points[0].second, bottom = top;
            for (const auto& p : points) { top = std::min(top, p.second); bottom = std::max(bottom, p.second); }
            const int y0 = std::max(0, static_cast<int>(std::floor(top))), y1 = std::min(image.height - 1, static_cast<int>(std::ceil(bottom)));
            std::vector<double> crossings;
            for (int y = y0; y <= y1; ++y)
            {
                const double cy = y + 0.5;
                crossings.clear();
                for (size_t i = 0; i < points.size(); ++i)
                {
                    const auto& a = points[i];
                    const auto& b = points[(i + 1) % points.size()];
                    if ((a.second <= cy && b.second > cy) || (b.second <= cy && a.second > cy))
                        crossings.push_back(a.first + (cy - a.second) * (b.first - a.first) / (b.second - a.second));
                }
                std::sort(crossings.begin(), crossings.end());
                for (size_t i = 0; i + 1 < crossings.size(); i += 2)
                {
                    const int x0 = std::max(0, static_cast<int>(std::ceil(crossings[i] - 0.5))), x1 = std::min(image.width, static_cast<int>(std::ceil(crossings[i + 1] - 0.5)));
                    for (int x = x0; x < x1; ++x) Plot(x, y, c);
                }
            }
        }
        // A line width pixels wide; with dash > 0 it is drawn in dash-long
        // pieces with gap-long holes.
        void Line(double x0, double y0, double x1, double y1, Colour c, double width = 1, double dash = 0, double gap = 0)
        {
            const double dx = x1 - x0, dy = y1 - y0, length = std::hypot(dx, dy);
            if (!std::isfinite(length)) return;
            if (dash <= 0 || length <= dash) { Segment(x0, y0, x1, y1, c, width); return; }
            const double step = dash + std::max(gap, 1.0);
            for (double t = 0; t < length; t += step)
            {
                const double t1 = std::min(length, t + dash);
                Segment(x0 + dx * t / length, y0 + dy * t / length, x0 + dx * t1 / length, y0 + dy * t1 / length, c, width);
            }
        }
        void Outline(const std::vector<std::pair<double, double>>& points, Colour c, double width = 1, double dash = 0, double gap = 0)
        {
            for (size_t i = 0; i < points.size(); ++i)
            {
                const auto& a = points[i];
                const auto& b = points[(i + 1) % points.size()];
                Line(a.first, a.second, b.first, b.second, c, width, dash, gap);
            }
        }
        void FillCircle(double cx, double cy, double r, Colour c)
        {
            const int x0 = std::max(0, static_cast<int>(std::floor(cx - r))), x1 = std::min(image.width - 1, static_cast<int>(std::ceil(cx + r)));
            const int y0 = std::max(0, static_cast<int>(std::floor(cy - r))), y1 = std::min(image.height - 1, static_cast<int>(std::ceil(cy + r)));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if ((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy) <= r * r) Plot(x, y, c);
        }
        void Ring(double cx, double cy, double r, Colour c, double width = 1)
        {
            const double outer = r + width / 2, inner = std::max(0.0, r - width / 2);
            const int x0 = std::max(0, static_cast<int>(std::floor(cx - outer))), x1 = std::min(image.width - 1, static_cast<int>(std::ceil(cx + outer)));
            const int y0 = std::max(0, static_cast<int>(std::floor(cy - outer))), y1 = std::min(image.height - 1, static_cast<int>(std::ceil(cy + outer)));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                    if (d <= outer && d >= inner) Plot(x, y, c);
                }
        }
        static int TextWidth(const std::string& text, int scale) { return text.empty() ? 0 : (static_cast<int>(text.size()) * Font::kAdvance - 1) * scale; }
        static int TextHeight(int scale) { return Font::kHeight * scale; }
        // Text with its top-left corner at (x, y); a halo keeps it readable on
        // any fill.
        void Text(int x, int y, const std::string& text, Colour c, int scale = 1, std::optional<Colour> halo = std::nullopt)
        {
            if (halo)
                for (int oy = -1; oy <= 1; ++oy)
                    for (int ox = -1; ox <= 1; ++ox)
                        if (ox || oy) Glyphs(x + ox * scale, y + oy * scale, text, *halo, scale);
            Glyphs(x, y, text, c, scale);
        }
    private:
        void Segment(double x0, double y0, double x1, double y1, Colour c, double width)
        {
            const double dx = x1 - x0, dy = y1 - y0, length = std::hypot(dx, dy);
            if (width <= 1.25 || length < 1e-9)
            {
                const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy)))));
                int lastX = INT32_MIN, lastY = INT32_MIN;
                for (int i = 0; i <= steps; ++i)
                {
                    const int x = static_cast<int>(std::floor(x0 + dx * i / steps)), y = static_cast<int>(std::floor(y0 + dy * i / steps));
                    if (x == lastX && y == lastY) continue;
                    if (width > 1.25) FillCircle(x + 0.5, y + 0.5, width / 2, c);
                    else Plot(x, y, c);
                    lastX = x; lastY = y;
                }
                return;
            }
            const double nx = -dy / length * width / 2, ny = dx / length * width / 2;
            FillPolygon({{x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}}, c);
        }
        void Glyphs(int x, int y, const std::string& text, Colour c, int scale)
        {
            for (size_t i = 0; i < text.size(); ++i)
            {
                const uint8_t* glyph = Font::Glyph(text[i]);
                const int gx = x + static_cast<int>(i) * Font::kAdvance * scale;
                for (int col = 0; col < Font::kWidth; ++col)
                    for (int row = 0; row < Font::kHeight; ++row)
                        if ((glyph[col] >> row) & 1)
                            for (int sy = 0; sy < scale; ++sy)
                                for (int sx = 0; sx < scale; ++sx) Plot(gx + col * scale + sx, y + row * scale + sy, c);
            }
        }
    };

    namespace Detail
    {
        using Vec3 = std::array<double, 3>;
        inline std::string Lower(std::string s)
        {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }
        inline bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
        // [x, y, z], [x, y] or {X, Y, Z} / {x, y, z}.
        inline std::optional<Vec3> Point(const Json& v)
        {
            Vec3 p{0, 0, 0};
            if (v.is_array() && v.size() >= 2)
            {
                for (size_t k = 0; k < 3 && k < v.size(); ++k)
                {
                    if (!v[k].is_number()) return std::nullopt;
                    p[k] = v[k].get<double>();
                }
            }
            else if (v.is_object())
            {
                const char* upper[3] = {"X", "Y", "Z"};
                const char* lower[3] = {"x", "y", "z"};
                bool any = false;
                for (int k = 0; k < 3; ++k)
                {
                    const Json* item = v.contains(upper[k]) ? &v.at(upper[k]) : v.contains(lower[k]) ? &v.at(lower[k]) : nullptr;
                    if (!item) continue;
                    if (item->is_number()) p[static_cast<size_t>(k)] = item->get<double>();
                    else if (item->is_string()) { try { p[static_cast<size_t>(k)] = std::stod(item->get<std::string>()); } catch (const std::exception&) { return std::nullopt; } }
                    else return std::nullopt;
                    any = true;
                }
                if (!any) return std::nullopt;
            }
            else return std::nullopt;
            for (double n : p) if (!std::isfinite(n) || std::abs(n) > 1e7) return std::nullopt;
            return p;
        }
        inline double Number(const Json& item, const char* key, double fallback)
        {
            if (!item.is_object() || !item.contains(key)) return fallback;
            const auto& v = item.at(key);
            if (v.is_number()) { const double n = v.get<double>(); return std::isfinite(n) ? n : fallback; }
            if (v.is_string()) { try { return std::stod(v.get<std::string>()); } catch (const std::exception&) {} }
            return fallback;
        }
        inline std::string Text(const Json& item, const char* key)
        {
            if (!item.is_object() || !item.contains(key)) return {};
            const auto& v = item.at(key);
            if (v.is_string()) return v.get<std::string>();
            if (v.is_number()) return v.dump();
            return {};
        }
        // What an actor class is on a plan, from its name.
        inline std::string ClassKind(const std::string& cls, const std::string& team = {})
        {
            const auto c = Lower(cls);
            if (Has(c, "playerstart")) return team == "0" ? "spawn-spy" : team == "1" ? "spawn-merc" : "spawn";
            if (Has(c, "computerobjectivetrigger") || Has(c, "terminal")) return "objective";
            if (Has(c, "flagdropzone") || Has(c, "dropzone")) return "dropzone";
            if (Has(c, "sflag")) return "flag";
            if (Has(c, "bombtarget")) return "bomb";
            if (Has(c, "sdisk")) return "disk";
            if (Has(c, "camnetwork")) return "camnet";
            if (Has(c, "scamera") || Has(c, ".camera")) return "camera";
            if (Has(c, "laser")) return "laser";
            if (Has(c, "volumetricsensor") || Has(c, "motion")) return "motion";
            if (Has(c, "presence")) return "presence";
            if (Has(c, "smine") || Has(c, "mineprox")) return "mine";
            if (Has(c, "alarmzone") || Has(c, "zoneinfo") || Has(c, "volume")) return "volume";
            if (Has(c, "salarm")) return "alarm";
            if (Has(c, "light")) return "light";
            if (Has(c, "slift")) return "lift";
            if (Has(c, "mover")) return "door";
            if (Has(c, "interrupteur") || Has(c, "button")) return "button";
            if (Has(c, "sound")) return "sound";
            if (Has(c, "ammo") || Has(c, "infirmary") || Has(c, "extinguisher") || Has(c, "sprinkler") || Has(c, "projector") || Has(c, "puddle") || Has(c, "balltrap") || Has(c, "glass")) return "ingredient";
            if (Has(c, "getrigger")) return "ge";
            if (Has(c, "staticmeshactor")) return "mesh";
            if (Has(c, "objective")) return "objective";
            return "other";
        }
        inline std::string ActorKind(const Json& actor)
        {
            auto kind = Lower(Text(actor, "kind"));
            std::replace(kind.begin(), kind.end(), '_', '-');
            static const std::map<std::string, std::string> synonyms{
                {"spy", "spawn-spy"}, {"merc", "spawn-merc"}, {"start", "spawn"}, {"terminal", "objective"}, {"mover", "door"},
                {"cam", "camera"}, {"zone", "volume"}, {"ammo", "ingredient"}, {"infirmary", "ingredient"}, {"drop-zone", "dropzone"}};
            if (const auto found = synonyms.find(kind); found != synonyms.end()) kind = found->second;
            static const std::set<std::string> known{"spawn-spy", "spawn-merc", "spawn", "objective", "flag", "dropzone", "bomb", "disk", "camera", "camnet", "laser",
                                                     "motion", "presence", "mine", "alarm", "light", "door", "lift", "button", "sound", "volume", "ingredient", "ge", "mesh"};
            if (known.count(kind) && kind != "spawn") return kind;
            const auto cls = Text(actor, "cls").empty() ? Text(actor, "class") : Text(actor, "cls");
            std::string team = Text(actor, "team");
            if (team.empty() && actor.contains("values") && actor.at("values").is_object()) team = Text(actor.at("values"), "TeamNumber");
            if (team == "spy") team = "0";
            if (team == "merc") team = "1";
            const auto byClass = ClassKind(cls, team);
            if (kind == "spawn" && byClass == "other") return "spawn";
            return byClass;
        }
        struct Style { Colour fill, edge; const char* shape; const char* legend; };
        inline const Style& ActorStyle(const std::string& kind)
        {
            static const std::map<std::string, Style> styles{
                {"spawn-spy", {{52, 152, 219}, {16, 60, 110}, "circle", "Spy start"}},
                {"spawn-merc", {{231, 76, 60}, {110, 25, 20}, "circle", "Merc start"}},
                {"spawn", {{180, 180, 180}, {70, 70, 70}, "circle", "Player start"}},
                {"objective", {{255, 196, 0}, {120, 80, 0}, "square", "Terminal / objective"}},
                {"flag", {{255, 120, 20}, {120, 50, 0}, "triangle", "Flag"}},
                {"dropzone", {{255, 120, 20}, {120, 50, 0}, "ring", "Flag drop zone"}},
                {"bomb", {{214, 40, 140}, {90, 10, 60}, "diamond", "Bomb target"}},
                {"disk", {{255, 225, 60}, {130, 100, 0}, "disk", "Disk"}},
                {"camera", {{235, 70, 70}, {100, 20, 20}, "square", "Security camera"}},
                {"camnet", {{150, 90, 230}, {60, 30, 110}, "square", "CamNet camera"}},
                {"laser", {{255, 40, 40}, {110, 0, 0}, "diamond", "Laser"}},
                {"motion", {{60, 190, 180}, {20, 90, 85}, "diamond", "Motion sensor"}},
                {"presence", {{60, 190, 180}, {20, 90, 85}, "square", "Presence trigger"}},
                {"mine", {{120, 120, 40}, {50, 50, 10}, "cross", "Mine"}},
                {"alarm", {{255, 150, 40}, {120, 60, 0}, "triangle", "Alarm"}},
                {"light", {{255, 236, 120}, {140, 120, 20}, "star", "Light"}},
                {"door", {{160, 100, 50}, {80, 45, 15}, "square", "Door"}},
                {"lift", {{120, 140, 160}, {40, 50, 60}, "square", "Lift"}},
                {"button", {{100, 130, 200}, {30, 50, 100}, "square", "Button / panel"}},
                {"sound", {{90, 200, 230}, {20, 90, 110}, "circle", "Sound"}},
                {"volume", {{80, 170, 210}, {30, 80, 110}, "ring", "Volume / zone"}},
                {"ingredient", {{70, 200, 90}, {20, 90, 30}, "cross", "Ammo / infirmary"}},
                {"ge", {{190, 90, 220}, {80, 30, 100}, "circle", "Geometric event"}},
                {"mesh", {{150, 150, 150}, {80, 80, 80}, "square", "Static mesh"}},
                {"other", {{170, 170, 170}, {80, 80, 80}, "circle", "Other actor"}}};
            const auto found = styles.find(kind);
            return found == styles.end() ? styles.at("other") : found->second;
        }
        struct PieceStyle { Colour fill, edge; bool stripes; };
        inline PieceStyle PieceLook(const std::string& kind)
        {
            const auto k = Lower(kind);
            if (k == "room" || k == "subtract") return {{236, 230, 214}, {110, 100, 85}, false};
            if (k == "corridor") return {{222, 228, 236}, {100, 110, 125}, false};
            if (k == "vent") return {{206, 232, 206}, {70, 120, 70}, false};
            if (k == "crawlway") return {{194, 222, 194}, {70, 120, 70}, false};
            if (k == "doorway") return {{250, 242, 196}, {150, 120, 60}, false};
            if (k == "platform") return {{206, 190, 164}, {120, 100, 70}, false};
            if (k == "stairs" || k == "stairs l" || k == "stairs u" || k == "spiral") return {{226, 208, 170}, {130, 105, 60}, true};
            if (k == "ramp") return {{226, 214, 186}, {130, 110, 70}, true};
            if (k == "add") return {{82, 86, 94}, {40, 42, 46}, false};
            if (k == "volume") return {{80, 170, 210, 40}, {60, 150, 200}, false};
            if (k == "mover") return {{230, 150, 60, 170}, {170, 95, 30}, false};
            if (k == "portal") return {{0, 0, 0, 0}, {210, 60, 210}, false};
            return {{205, 205, 205}, {120, 120, 120}, false};
        }
        inline bool CsgKind(const std::string& kind) { const auto k = Lower(kind); return k == "add" || k == "subtract"; }
        inline std::vector<std::pair<double, double>> Hull(std::vector<std::pair<double, double>> points)
        {
            std::sort(points.begin(), points.end());
            points.erase(std::unique(points.begin(), points.end(), [](const auto& a, const auto& b) { return std::abs(a.first - b.first) < 1e-6 && std::abs(a.second - b.second) < 1e-6; }), points.end());
            if (points.size() < 3) return points;
            auto cross = [](const auto& o, const auto& a, const auto& b) { return (a.first - o.first) * (b.second - o.second) - (a.second - o.second) * (b.first - o.first); };
            std::vector<std::pair<double, double>> hull(points.size() * 2);
            size_t k = 0;
            for (size_t i = 0; i < points.size(); ++i)
            {
                while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
                hull[k++] = points[i];
            }
            for (size_t i = points.size() - 1, t = k + 1; i > 0; --i)
            {
                while (k >= t && cross(hull[k - 2], hull[k - 1], points[i - 1]) <= 0) --k;
                hull[k++] = points[i - 1];
            }
            hull.resize(k - 1);
            return hull;
        }
        inline double Area(const std::vector<std::pair<double, double>>& polygon)
        {
            double a = 0;
            for (size_t i = 0; i < polygon.size(); ++i)
            {
                const auto& p = polygon[i];
                const auto& q = polygon[(i + 1) % polygon.size()];
                a += p.first * q.second - q.first * p.second;
            }
            return std::abs(a) / 2;
        }
        // A distance that reads well on a scale bar or grid.
        inline double Nice(double atLeast)
        {
            for (double n : {8.0, 16.0, 32.0, 64.0, 128.0, 256.0, 512.0, 1024.0, 2048.0, 4096.0, 8192.0, 16384.0, 32768.0, 65536.0, 131072.0})
                if (n >= atLeast) return n;
            return 262144;
        }
        inline std::string Units(double uu)
        {
            const auto whole = static_cast<long long>(std::llround(uu));
            return std::to_string(whole) + " uu";
        }
    }

    // A spec geometry.brushes item ({id, csg, at, polygons}) as a render
    // piece: its footprint (the hull of its vertices) and bounds.
    inline Json BrushPiece(const Json& brush)
    {
        using namespace Detail;
        Vec3 at{0, 0, 0};
        if (brush.contains("at")) if (const auto p = Point(brush.at("at"))) at = *p;
        std::vector<std::pair<double, double>> points;
        Vec3 lo{1e18, 1e18, 1e18}, hi{-1e18, -1e18, -1e18};
        for (const auto& polygon : brush.value("polygons", Json::array()))
            for (const auto& v : polygon.value("vertices", Json::array()))
                if (const auto p = Point(v))
                {
                    const Vec3 w{(*p)[0] + at[0], (*p)[1] + at[1], (*p)[2] + at[2]};
                    for (size_t k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], w[k]); hi[k] = std::max(hi[k], w[k]); }
                    points.push_back({w[0], w[1]});
                }
        if (points.empty()) return Json();
        const auto csg = brush.value("csg", std::string("CSG_Add"));
        const auto cls = Lower(brush.value("class", std::string("Engine.Brush")));
        const std::string kind = Has(cls, "volume") ? "volume" : Has(cls, "mover") ? "mover" : csg == "CSG_Subtract" ? "subtract" : "add";
        Json outline = Json::array();
        for (const auto& p : Hull(points)) outline.push_back({p.first, p.second});
        return {{"id", brush.value("id", std::string())}, {"kind", kind}, {"box", {{"min", lo}, {"max", hi}}}, {"yaw", 0}, {"outline", outline}};
    }

    // A spec's geometry.brushes added to a render scene as pieces (the
    // compiler's Compiled.render holds the Map Design pieces only). Lengths
    // follow the spec's units (uu, or m and ft at uuPerMetre).
    inline void AddBrushes(Json& scene, const Json& spec)
    {
        if (!scene.is_object() || !spec.is_object() || !spec.contains("geometry") || !spec.at("geometry").is_object() || !spec.at("geometry").contains("brushes")) return;
        const auto units = spec.value("units", Json::object());
        const auto unit = units.is_object() ? units.value("unit", std::string("uu")) : std::string("uu");
        const double perMetre = units.is_object() ? units.value("uuPerMetre", 100.0) : 100.0;
        const double scale = unit == "m" ? perMetre : unit == "ft" ? perMetre * 0.3048 : 1.0;
        if (!scene.contains("pieces") || !scene.at("pieces").is_array()) scene["pieces"] = Json::array();
        for (const auto& brush : spec.at("geometry").at("brushes"))
        {
            Json scaled = brush;
            if (scale != 1.0)
            {
                auto times = [&](Json& v) { if (v.is_array()) for (auto& n : v) if (n.is_number()) n = n.get<double>() * scale; };
                if (scaled.contains("at")) times(scaled["at"]);
                if (scaled.contains("polygons")) for (auto& p : scaled["polygons"]) if (p.contains("vertices")) for (auto& v : p["vertices"]) times(v);
            }
            auto piece = BrushPiece(scaled);
            if (!piece.is_null()) scene["pieces"].push_back(piece);
        }
        // Bounds the compiler worked out without the brushes would cut them off.
        if (scene.contains("bounds")) scene.erase("bounds");
    }
    // The live map as a render scene: design.scene's items (brushes with
    // their edges, actors with positions, lights' radius, cameras' fov) and
    // actors.query's actors (TeamNumber and other values, Tag and Event),
    // matched by path. Event links become event lines; a ZoneInfo's
    // LocationName becomes a zone label.
    inline Json SceneFromLive(const Json& designScene, const Json& actorsQuery = Json())
    {
        using namespace Detail;
        const Json* items = &designScene;
        if (designScene.is_object())
            for (const char* key : {"scene", "items", "actors"})
                if (designScene.contains(key) && designScene.at(key).is_array()) { items = &designScene.at(key); break; }
        Json empty = Json::array();
        if (!items->is_array()) items = &empty;
        const Json* queried = &actorsQuery;
        if (actorsQuery.is_object() && actorsQuery.contains("actors")) queried = &actorsQuery.at("actors");
        if (!queried->is_array()) queried = &empty;
        std::map<std::string, const Json*> byPath;
        for (const auto& a : *queried) if (a.is_object()) byPath[Lower(Text(a, "path"))] = &a;
        Json scene{{"pieces", Json::array()}, {"doorways", Json::array()}, {"actors", Json::array()}, {"lines", Json::array()}, {"labels", Json::array()}, {"live", true}};
        struct Placed { std::string id, tag, event, kind; Vec3 at; };
        std::vector<Placed> placed;
        std::set<std::string> seen;
        auto addActor = [&](const Json& item, const Json* query)
        {
            const auto path = Text(item, "path");
            const auto at = Point(item.contains("position") ? item.at("position") : query && query->contains("position") ? query->at("position") : Json());
            if (!at) return;
            Json actor{{"id", path}, {"cls", Text(item, "class")}, {"at", *at}};
            const Json values = query && query->contains("values") && query->at("values").is_object() ? query->at("values") : Json::object();
            Json probe{{"cls", Text(item, "class")}, {"values", values}};
            actor["kind"] = ActorKind(probe);
            double yaw = 0;
            if (const auto& rot = item.contains("rotation") ? item.at("rotation") : query && query->contains("rotation") ? query->at("rotation") : Json(); rot.is_array() && rot.size() == 3 && rot[1].is_number())
                yaw = rot[1].get<double>() * 360.0 / 65536.0;
            actor["yaw"] = yaw;
            if (item.contains("light") && item.at("light").is_object()) actor["radius"] = Number(item.at("light"), "radius", 0);
            if (item.contains("camera") && item.at("camera").is_object()) actor["fov"] = Number(item.at("camera"), "fov", 60);
            const auto tag = query ? Text(*query, "tag") : std::string(), event = query ? Text(*query, "event") : std::string();
            const auto name = path.substr(path.find_last_of('.') == std::string::npos ? 0 : path.find_last_of('.') + 1);
            actor["label"] = !tag.empty() && Lower(tag) != "none" && Lower(tag) != Lower(name) ? tag : name;
            scene["actors"].push_back(actor);
            placed.push_back({path, tag, event, actor.at("kind").get<std::string>(), *at});
            if (Lower(Text(item, "class")).find("zoneinfo") != std::string::npos)
            {
                auto location = Text(values, "LocationName");
                if (location.size() >= 2 && location.front() == '"' && location.back() == '"') location = location.substr(1, location.size() - 2);
                if (!location.empty()) scene["labels"].push_back({{"at", *at}, {"text", location}, {"kind", "zone"}});
            }
        };
        for (const auto& item : *items)
        {
            if (!item.is_object()) continue;
            const auto path = Text(item, "path");
            seen.insert(Lower(path));
            const auto found = byPath.find(Lower(path));
            const Json* query = found == byPath.end() ? nullptr : found->second;
            std::vector<std::pair<double, double>> points;
            Vec3 lo{1e18, 1e18, 1e18}, hi{-1e18, -1e18, -1e18};
            if (item.contains("edges") && item.at("edges").is_array())
                for (const auto& edge : item.at("edges"))
                    if (edge.is_array())
                        for (const auto& v : edge)
                            if (const auto p = Point(v))
                            {
                                for (size_t k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], (*p)[k]); hi[k] = std::max(hi[k], (*p)[k]); }
                                points.push_back({(*p)[0], (*p)[1]});
                            }
            if (!points.empty())
            {
                const int csg = item.value("csg", 0);
                const std::string kind = item.value("portal", false) ? "portal" : item.value("volume", false) ? "volume" : item.value("mover", false) ? "mover" : csg == 2 ? "subtract" : "add";
                Json piece{{"id", path}, {"kind", kind}, {"cls", Text(item, "class")}, {"box", {{"min", lo}, {"max", hi}}}, {"yaw", 0}};
                const auto hull = Hull(points);
                if (hull.size() >= 3 && Area(hull) > 1)
                {
                    Json outline = Json::array();
                    for (const auto& p : hull) outline.push_back({p.first, p.second});
                    piece["outline"] = outline;
                }
                scene["pieces"].push_back(piece);
                continue;
            }
            if (item.value("brush", false)) continue;
            addActor(item, query);
        }
        for (const auto& a : *queried)
            if (a.is_object() && !seen.count(Lower(Text(a, "path"))))
            {
                Json item{{"path", Text(a, "path")}, {"class", Text(a, "class")}};
                if (a.contains("position")) item["position"] = a.at("position");
                if (a.contains("rotation")) item["rotation"] = a.at("rotation");
                addActor(item, &a);
            }
        // Event wiring: an actor's Event names the Tag of what it triggers.
        std::map<std::string, std::vector<size_t>> byTag;
        for (size_t i = 0; i < placed.size(); ++i)
            if (!placed[i].tag.empty() && Lower(placed[i].tag) != "none") byTag[Lower(placed[i].tag)].push_back(i);
        size_t lines = 0;
        for (const auto& from : placed)
        {
            if (from.event.empty() || Lower(from.event) == "none") continue;
            const auto found = byTag.find(Lower(from.event));
            if (found == byTag.end()) continue;
            for (size_t index : found->second)
            {
                if (++lines > 4000) break;
                const auto& to = placed[index];
                scene["lines"].push_back({{"from", from.at}, {"to", to.at}, {"kind", to.kind == "alarm" ? "alarm" : "event"}, {"ids", {from.id, to.id}}});
            }
        }
        return scene;
    }

    namespace Detail
    {
        struct Piece { std::string id, kind, name, storey; Vec3 lo{}, hi{}; double yaw = 0; std::vector<std::pair<double, double>> outline; size_t order = 0; };
        struct Actor { std::string id, kind, label; Vec3 at{}; double yaw = 0, fov = 0, range = 0, radius = 0; bool hasYaw = false; };
        struct Line { Vec3 from{}, to{}; std::string kind; };
        struct Label { Vec3 at{}; std::string text, kind; };
        struct Doorway { Vec3 at{}; double width = 96, yaw = 0; bool portal = false; };
        struct View
        {
            double minX = 0, maxY = 0, scale = 1;
            int margin = 16;
            double X(double x) const { return margin + (x - minX) * scale; }
            double Y(double y) const { return margin + (maxY - y) * scale; }
        };
        inline std::vector<std::pair<double, double>> Footprint(const Piece& p, const View& view)
        {
            std::vector<std::pair<double, double>> out;
            if (p.outline.size() >= 3) { for (const auto& q : p.outline) out.push_back({view.X(q.first), view.Y(q.second)}); return out; }
            out = {{view.X(p.lo[0]), view.Y(p.lo[1])}, {view.X(p.hi[0]), view.Y(p.lo[1])}, {view.X(p.hi[0]), view.Y(p.hi[1])}, {view.X(p.lo[0]), view.Y(p.hi[1])}};
            return out;
        }
        // Treads across a flight: lines across the piece's local Y (its
        // direction of travel), clipped to the box.
        inline void Stripes(Canvas& canvas, const Piece& p, const View& view, Colour c)
        {
            const double yaw = p.yaw * kPi / 180;
            const bool alongX = std::abs(std::sin(yaw)) > 0.7;
            const double spacing = std::max(32.0, 6 / view.scale);
            if (alongX)
                for (double x = p.lo[0] + spacing / 2; x < p.hi[0]; x += spacing) canvas.Line(view.X(x), view.Y(p.lo[1]), view.X(x), view.Y(p.hi[1]), c);
            else
                for (double y = p.lo[1] + spacing / 2; y < p.hi[1]; y += spacing) canvas.Line(view.X(p.lo[0]), view.Y(y), view.X(p.hi[0]), view.Y(y), c);
        }
        inline void Icon(Canvas& canvas, const std::string& shape, double x, double y, double r, const Style& style)
        {
            const Colour fill = style.fill, edge = style.edge;
            if (shape == "square") { canvas.FillRect(x - r - 1, y - r - 1, x + r + 1, y + r + 1, edge); canvas.FillRect(x - r, y - r, x + r, y + r, fill); }
            else if (shape == "diamond")
            {
                canvas.FillPolygon({{x, y - r - 1.5}, {x + r + 1.5, y}, {x, y + r + 1.5}, {x - r - 1.5, y}}, edge);
                canvas.FillPolygon({{x, y - r}, {x + r, y}, {x, y + r}, {x - r, y}}, fill);
            }
            else if (shape == "triangle")
            {
                canvas.FillPolygon({{x, y - r - 2}, {x + r + 1.5, y + r + 1}, {x - r - 1.5, y + r + 1}}, edge);
                canvas.FillPolygon({{x, y - r}, {x + r, y + r}, {x - r, y + r}}, fill);
            }
            else if (shape == "ring") { canvas.Ring(x, y, r, edge, 3); canvas.Ring(x, y, r, fill, 1.5); }
            else if (shape == "cross")
            {
                canvas.FillRect(x - r - 1, y - r / 3 - 1, x + r + 1, y + r / 3 + 1, edge); canvas.FillRect(x - r / 3 - 1, y - r - 1, x + r / 3 + 1, y + r + 1, edge);
                canvas.FillRect(x - r, y - r / 3, x + r, y + r / 3, fill); canvas.FillRect(x - r / 3, y - r, x + r / 3, y + r, fill);
            }
            else if (shape == "star")
            {
                std::vector<std::pair<double, double>> outer, inner;
                for (int i = 0; i < 10; ++i)
                {
                    const double a = -kPi / 2 + i * kPi / 5, rr = (i % 2 ? 0.45 : 1.0);
                    outer.push_back({x + std::cos(a) * (r + 1.5) * rr, y + std::sin(a) * (r + 1.5) * rr});
                    inner.push_back({x + std::cos(a) * r * rr, y + std::sin(a) * r * rr});
                }
                canvas.FillPolygon(outer, edge);
                canvas.FillPolygon(inner, fill);
            }
            else if (shape == "disk") { canvas.FillCircle(x, y, r * 0.8 + 1, edge); canvas.FillCircle(x, y, r * 0.8, fill); canvas.FillCircle(x, y, std::max(1.0, r * 0.25), edge); }
            else { canvas.FillCircle(x, y, r + 1, edge); canvas.FillCircle(x, y, r, fill); }
        }
        struct Box { int x0, y0, x1, y1; };
        inline bool Overlap(const Box& a, const Box& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
    }

    // The plan as an image (see the header for the scene and options).
    inline Image::Rgba PlanImage(const Json& scene, const Json& options = Json::object())
    {
        using namespace Detail;
        const Json opts = options.is_object() ? options : Json::object();
        const Json sc = scene.is_object() ? scene : Json::object();
        auto flag = [&](const char* key, bool fallback) { return opts.contains(key) && opts.at(key).is_boolean() ? opts.at(key).get<bool>() : fallback; };
        const int maxEdge = static_cast<int>(std::clamp(Number(opts, "maxEdge", 1024), 64.0, 4096.0));
        const bool labels = flag("labels", true), grid = flag("grid", true), legend = flag("legend", true), actorLabels = flag("actorLabels", false), cones = flag("cones", true);

        // Read the scene.
        std::vector<Piece> pieces;
        for (const auto& item : sc.value("pieces", Json::array()))
        {
            if (!item.is_object() || !item.contains("box") || !item.at("box").is_object()) continue;
            const auto lo = Point(item.at("box").value("min", Json())), hi = Point(item.at("box").value("max", Json()));
            if (!lo || !hi) continue;
            Piece p;
            p.id = Text(item, "id"); p.kind = Text(item, "kind"); p.name = Text(item, "name"); p.storey = Text(item, "storey");
            for (size_t k = 0; k < 3; ++k) { p.lo[k] = std::min((*lo)[k], (*hi)[k]); p.hi[k] = std::max((*lo)[k], (*hi)[k]); }
            p.yaw = Number(item, "yaw", 0);
            if (item.contains("outline") && item.at("outline").is_array())
                for (const auto& q : item.at("outline")) if (const auto v = Point(q)) p.outline.push_back({(*v)[0], (*v)[1]});
            p.order = pieces.size();
            pieces.push_back(std::move(p));
        }
        std::vector<Actor> actors;
        for (const auto& item : sc.value("actors", Json::array()))
        {
            if (!item.is_object()) continue;
            const auto at = Point(item.contains("at") ? item.at("at") : item.value("position", Json()));
            if (!at) continue;
            Actor a;
            a.id = Text(item, "id"); a.kind = ActorKind(item); a.label = Text(item, "label"); a.at = *at;
            a.hasYaw = item.contains("yaw") && item.at("yaw").is_number();
            a.yaw = Number(item, "yaw", 0); a.fov = Number(item, "fov", 0); a.range = Number(item, "range", 0); a.radius = Number(item, "radius", 0);
            if (a.label.empty()) a.label = a.id;
            actors.push_back(std::move(a));
        }
        std::vector<Line> lines;
        for (const auto& item : sc.value("lines", Json::array()))
        {
            if (!item.is_object()) continue;
            const auto from = Point(item.value("from", Json())), to = Point(item.value("to", Json()));
            if (from && to) lines.push_back({*from, *to, Lower(Text(item, "kind"))});
        }
        std::vector<Label> texts;
        for (const auto& item : sc.value("labels", Json::array()))
        {
            if (!item.is_object()) continue;
            const auto at = Point(item.value("at", Json()));
            if (at && !Text(item, "text").empty()) texts.push_back({*at, Text(item, "text"), Lower(Text(item, "kind"))});
        }
        std::vector<Doorway> doors;
        for (const auto& item : sc.value("doorways", Json::array()))
        {
            if (!item.is_object()) continue;
            const auto at = Point(item.value("at", Json()));
            if (at) doors.push_back({*at, Number(item, "width", 96), Number(item, "yaw", 0), item.value("portal", false)});
        }

        // The storey drawn: its pieces, and the things within their heights.
        std::string storeyName;
        bool filtered = false;
        double bandLo = -1e18, bandHi = 1e18;
        std::vector<char> keep(pieces.size(), 1);
        if (opts.contains("storey") && !opts.at("storey").is_null() && !(opts.at("storey").is_string() && Lower(opts.at("storey").get<std::string>()) == "all"))
        {
            std::vector<std::pair<double, std::string>> storeys;
            std::map<std::string, double> lowest;
            for (const auto& p : pieces)
                if (!p.storey.empty()) { const auto key = Lower(p.storey); lowest[key] = lowest.count(key) ? std::min(lowest[key], p.lo[2]) : p.lo[2]; }
            for (const auto& [key, z] : lowest) storeys.push_back({z, key});
            std::sort(storeys.begin(), storeys.end());
            std::string wanted;
            if (opts.at("storey").is_number_integer())
            {
                const auto index = opts.at("storey").get<long long>();
                if (index < 0 || static_cast<size_t>(index) >= storeys.size()) throw std::runtime_error("The plan has " + std::to_string(storeys.size()) + " storeys; storey " + std::to_string(index) + " does not exist.");
                wanted = storeys[static_cast<size_t>(index)].second;
            }
            else if (opts.at("storey").is_string()) wanted = Lower(opts.at("storey").get<std::string>());
            else throw std::runtime_error("storey is a storey id or index.");
            if (!lowest.count(wanted))
            {
                std::string known;
                for (const auto& s : storeys) known += (known.empty() ? "" : ", ") + s.second;
                throw std::runtime_error("No piece is on storey \"" + wanted + "\"" + (known.empty() ? std::string(" (the pieces name no storeys)") : "; storeys: " + known) + ".");
            }
            filtered = true;
            bandLo = 1e18; bandHi = -1e18;
            for (size_t i = 0; i < pieces.size(); ++i)
            {
                keep[i] = Lower(pieces[i].storey) == wanted;
                if (keep[i]) { bandLo = std::min(bandLo, pieces[i].lo[2]); bandHi = std::max(bandHi, pieces[i].hi[2]); }
            }
            bandLo -= 32; bandHi += 32;
            for (const auto& p : pieces) if (Lower(p.storey) == wanted) { storeyName = p.storey; break; }
        }
        auto inBand = [&](const Vec3& v) { return v[2] >= bandLo && v[2] <= bandHi; };

        // The area drawn.
        Vec3 lo{1e18, 1e18, 1e18}, hi{-1e18, -1e18, -1e18};
        auto grow = [&](const Vec3& v) { for (size_t k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], v[k]); hi[k] = std::max(hi[k], v[k]); } };
        const auto* given = opts.contains("bounds") ? &opts.at("bounds") : sc.contains("bounds") ? &sc.at("bounds") : nullptr;
        if (given && given->is_object() && Point(given->value("min", Json())) && Point(given->value("max", Json())))
        {
            grow(*Point(given->at("min")));
            grow(*Point(given->at("max")));
        }
        else
        {
            for (const auto& p : pieces) { grow(p.lo); grow(p.hi); }
            for (const auto& a : actors) grow(a.at);
            for (const auto& l : lines) { grow(l.from); grow(l.to); }
            for (const auto& d : doors) grow(d.at);
        }
        const bool empty = lo[0] > hi[0];
        if (empty) { lo = {-512, -512, 0}; hi = {512, 512, 0}; }
        // A little room round the edge, and never thinner than 256 units.
        for (size_t k = 0; k < 2; ++k)
        {
            const double pad = std::max(64.0, (hi[k] - lo[k]) * 0.03);
            lo[k] -= pad; hi[k] += pad;
            if (hi[k] - lo[k] < 256) { const double mid = (lo[k] + hi[k]) / 2; lo[k] = mid - 128; hi[k] = mid + 128; }
        }
        // The legend gets a strip of its own, below a wide plan or beside a
        // tall one, and the plan shrinks to leave room for it.
        const int fontScale = maxEdge >= 1536 ? 2 : 1;
        const int pad = 4 * fontScale, lineHeight = (Font::kHeight + 4) * fontScale;
        std::vector<std::string> legendKinds;
        if (legend && !empty)
        {
            std::set<std::string> kindsDrawn;
            for (const auto& a : actors) if (!filtered || inBand(a.at)) kindsDrawn.insert(a.kind);
            static const char* orderOfKinds[] = {"spawn-spy", "spawn-merc", "spawn", "objective", "flag", "dropzone", "bomb", "disk", "camnet", "camera", "laser", "motion", "presence",
                                                 "mine", "alarm", "light", "door", "lift", "button", "ingredient", "ge", "sound", "volume", "mesh", "other"};
            for (const char* k : orderOfKinds) if (kindsDrawn.count(k)) legendKinds.push_back(k);
        }
        int legendColumn = 0;
        for (const auto& k : legendKinds) legendColumn = std::max(legendColumn, Canvas::TextWidth(ActorStyle(k).legend, fontScale) + lineHeight + 3 * pad);
        const bool legendBelow = (hi[0] - lo[0]) >= (hi[1] - lo[1]);
        View view;
        view.margin = std::max(8, maxEdge / 64);
        view.minX = lo[0];
        view.maxY = hi[1];
        int stripW = 0, stripH = 0, planW = 0, planH = 0, columns = 1;
        for (int pass = 0; pass < 4; ++pass)
        {
            if (!legendKinds.empty() && !legendBelow) stripW = legendColumn + pad;
            const double availW = maxEdge - 2.0 * view.margin - stripW, availH = maxEdge - 2.0 * view.margin - stripH;
            view.scale = std::max(1e-6, std::min(availW / (hi[0] - lo[0]), availH / (hi[1] - lo[1])));
            planW = std::max(64, std::min(maxEdge - stripW, static_cast<int>(std::ceil((hi[0] - lo[0]) * view.scale)) + 2 * view.margin));
            planH = std::max(64, std::min(maxEdge - stripH, static_cast<int>(std::ceil((hi[1] - lo[1]) * view.scale)) + 2 * view.margin));
            if (legendKinds.empty() || !legendBelow) break;
            columns = std::max(1, (planW - 2 * view.margin) / std::max(1, legendColumn));
            const int rows = (static_cast<int>(legendKinds.size()) + columns - 1) / columns;
            const int wanted = rows * lineHeight + 2 * pad;
            if (wanted == stripH) break;
            stripH = wanted;
        }
        const int width = planW + stripW, height = planH + stripH;
        const Colour background{44, 48, 54};
        Canvas canvas(width, height, background);
        const Colour ink{30, 30, 34}, paper{250, 250, 246}, white{245, 245, 245};
        if (empty)
        {
            const std::string text = "Nothing to draw";
            canvas.Text((width - Canvas::TextWidth(text, fontScale)) / 2, height / 2 - 4 * fontScale, text, white, fontScale);
            return canvas.image;
        }

        // Pieces: spec pieces lowest first, live CSG brushes in build order.
        std::vector<size_t> order(pieces.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        auto layer = [&](const Piece& p)
        {
            const auto k = Lower(p.kind);
            if (k == "volume" || k == "mover") return 2;
            if (k == "portal") return 3;
            if (k == "doorway") return 1;
            return 0;
        };
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b)
        {
            const auto& A = pieces[a];
            const auto& B = pieces[b];
            if (layer(A) != layer(B)) return layer(A) < layer(B);
            if (CsgKind(A.kind) || CsgKind(B.kind)) return false;
            return A.lo[2] < B.lo[2];
        });
        if (filtered)
            for (size_t i : order)
                if (!keep[i]) canvas.Outline(Footprint(pieces[i], view), Colour{200, 200, 200, 70}, 1);
        for (size_t i : order)
        {
            if (!keep[i]) continue;
            const auto& p = pieces[i];
            const auto look = PieceLook(p.kind);
            const auto shape = Footprint(p, view);
            if (Lower(p.kind) == "portal")
            {
                // A portal is a sheet: draw it along its longer side.
                const bool alongX = (p.hi[0] - p.lo[0]) >= (p.hi[1] - p.lo[1]);
                const double cx = (p.lo[0] + p.hi[0]) / 2, cy = (p.lo[1] + p.hi[1]) / 2;
                if (alongX) canvas.Line(view.X(p.lo[0]), view.Y(cy), view.X(p.hi[0]), view.Y(cy), look.edge, 2, 4, 3);
                else canvas.Line(view.X(cx), view.Y(p.lo[1]), view.X(cx), view.Y(p.hi[1]), look.edge, 2, 4, 3);
                continue;
            }
            canvas.FillPolygon(shape, look.fill);
            if (look.stripes) Stripes(canvas, p, view, Colour{look.edge.r, look.edge.g, look.edge.b, 110});
        }
        for (size_t i : order)
        {
            if (!keep[i]) continue;
            const auto& p = pieces[i];
            const auto k = Lower(p.kind);
            if (k == "portal") continue;
            const auto look = PieceLook(p.kind);
            if (k == "volume") canvas.Outline(Footprint(p, view), look.edge, 1, 5, 3);
            else if (k != "doorway") canvas.Outline(Footprint(p, view), look.edge, 1);
        }
        if (grid)
        {
            const double step = Nice(16 / view.scale);
            const Colour minor{128, 128, 128, 34}, major{128, 128, 128, 70};
            for (double x = std::ceil(lo[0] / step) * step; x <= hi[0]; x += step)
            {
                const bool strong = std::fmod(std::abs(x), step * 4) < 1e-6;
                canvas.Line(std::floor(view.X(x)), 0, std::floor(view.X(x)), height, strong ? major : minor);
            }
            for (double y = std::ceil(lo[1] / step) * step; y <= hi[1]; y += step)
            {
                const bool strong = std::fmod(std::abs(y), step * 4) < 1e-6;
                canvas.Line(0, std::floor(view.Y(y)), width, std::floor(view.Y(y)), strong ? major : minor);
            }
        }
        // Doorways: the opening across the wall, and a portal's sheet.
        for (const auto& d : doors)
        {
            if (filtered && !inBand(d.at)) continue;
            const double yaw = d.yaw * kPi / 180, half = d.width / 2;
            const double ux = std::cos(yaw), uy = std::sin(yaw);
            const double x0 = view.X(d.at[0] - ux * half), y0 = view.Y(d.at[1] - uy * half), x1 = view.X(d.at[0] + ux * half), y1 = view.Y(d.at[1] + uy * half);
            canvas.Line(x0, y0, x1, y1, Colour{150, 100, 40}, std::max(2.0, std::min(4.0, 12 * view.scale)));
            if (d.portal) canvas.Line(x0, y0, x1, y1, Colour{210, 60, 210}, 1, 3, 2);
        }
        // Lines: lasers, CamNet loops, wiring, geometric events.
        for (const auto& l : lines)
        {
            if (filtered && !inBand(l.from) && !inBand(l.to)) continue;
            const double x0 = view.X(l.from[0]), y0 = view.Y(l.from[1]), x1 = view.X(l.to[0]), y1 = view.Y(l.to[1]);
            if (l.kind == "laser") { canvas.Line(x0, y0, x1, y1, Colour{255, 50, 50, 230}, 2); continue; }
            if (l.kind == "camnet") { canvas.Line(x0, y0, x1, y1, Colour{160, 100, 240, 200}, 1.5, 6, 4); continue; }
            if (l.kind == "alarm") { canvas.Line(x0, y0, x1, y1, Colour{255, 150, 40, 170}, 1, 2, 3); continue; }
            if (l.kind == "lock") { canvas.Line(x0, y0, x1, y1, Colour{180, 30, 30, 200}, 1, 5, 3); continue; }
            if (l.kind == "motion") { canvas.Outline({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, Colour{60, 190, 180, 200}, 1, 4, 3); continue; }
            if (l.kind == "lift") { canvas.Ring(x0, y0, 5, Colour{120, 140, 160}, 2); continue; }
            if (l.kind.rfind("ge-", 0) == 0) { canvas.Line(x0, y0, x1, y1, Colour{190, 90, 220, 230}, 2.5); canvas.FillCircle(x0, y0, 2.5, Colour{190, 90, 220}); continue; }
            canvas.Line(x0, y0, x1, y1, Colour{120, 120, 120, 150}, 1, 2, 3);
        }
        // Actors, with camera views and light reach under them.
        const double r = std::clamp(36 * view.scale, 3.0, 9.0) * (fontScale > 1 ? 1.4 : 1.0);
        for (const auto& a : actors)
        {
            if (filtered && !inBand(a.at)) continue;
            const double x = view.X(a.at[0]), y = view.Y(a.at[1]);
            if (a.kind == "light" && a.radius > 0)
            {
                canvas.FillCircle(x, y, a.radius * view.scale, Colour{255, 236, 120, 28});
                canvas.Ring(x, y, a.radius * view.scale, Colour{255, 236, 120, 80}, 1);
            }
            if (cones && (a.kind == "camera" || a.kind == "camnet"))
            {
                const double fov = (a.fov > 0 ? a.fov : a.kind == "camera" ? 60 : 75) * kPi / 180, range = (a.range > 0 ? a.range : 640) * view.scale;
                const double yaw = a.yaw * kPi / 180;
                std::vector<std::pair<double, double>> cone{{x, y}};
                for (int i = 0; i <= 12; ++i)
                {
                    const double t = yaw - fov / 2 + fov * i / 12;
                    cone.push_back({x + std::cos(t) * range, y - std::sin(t) * range});
                }
                const auto& st = ActorStyle(a.kind);
                canvas.FillPolygon(cone, Colour{st.fill.r, st.fill.g, st.fill.b, 45});
                canvas.Outline(cone, Colour{st.fill.r, st.fill.g, st.fill.b, 110}, 1);
            }
        }
        for (const auto& a : actors)
        {
            if (filtered && !inBand(a.at)) continue;
            const double x = view.X(a.at[0]), y = view.Y(a.at[1]);
            const auto& style = ActorStyle(a.kind);
            const double size = a.kind == "other" || a.kind == "mesh" || a.kind == "sound" ? r * 0.6 : r;
            // Starts and cameras point the way they face.
            if (a.hasYaw && (a.kind.rfind("spawn", 0) == 0 || a.kind == "camera" || a.kind == "camnet"))
            {
                const double yaw = a.yaw * kPi / 180;
                canvas.Line(x, y, x + std::cos(yaw) * size * 2.2, y - std::sin(yaw) * size * 2.2, style.edge, 2);
            }
            Icon(canvas, style.shape, x, y, size, style);
        }

        // Labels: zones, rooms, then actors, skipping any that would overlap.
        std::vector<Box> taken;
        auto place = [&](int x, int y, const std::string& text, Colour colour, int scale, std::optional<Colour> halo) -> bool
        {
            const Box box{x - 2, y - 2, x + Canvas::TextWidth(text, scale) + 2, y + Canvas::TextHeight(scale) + 2};
            if (box.x0 < 0 || box.y0 < 0 || box.x1 > planW || box.y1 > planH) return false;
            for (const auto& t : taken) if (Overlap(t, box)) return false;
            taken.push_back(box);
            canvas.Text(x, y, text, colour, scale, halo);
            return true;
        };
        auto shorten = [](std::string text, size_t max) { if (text.size() > max) text = text.substr(0, max - 2) + ".."; return text; };
        if (labels)
        {
            std::vector<const Label*> ordered;
            for (const auto& l : texts) if (l.kind == "zone") ordered.push_back(&l);
            for (const auto& l : texts) if (l.kind != "zone") ordered.push_back(&l);
            for (const auto* l : ordered)
            {
                if (filtered && !inBand(l->at)) continue;
                const bool zone = l->kind == "zone";
                const int scale = zone && maxEdge >= 768 ? fontScale + 1 : fontScale;
                const auto text = shorten(l->text, zone ? 28 : 24);
                const int tx = static_cast<int>(view.X(l->at[0])) - Canvas::TextWidth(text, scale) / 2, ty = static_cast<int>(view.Y(l->at[1])) - Canvas::TextHeight(scale) / 2;
                place(tx, ty + (zone ? 0 : Canvas::TextHeight(scale) + 2), text, zone ? Colour{40, 50, 140} : ink, scale, paper);
            }
        }
        if (actorLabels)
            for (const auto& a : actors)
            {
                if (filtered && !inBand(a.at)) continue;
                place(static_cast<int>(view.X(a.at[0]) + r + 3), static_cast<int>(view.Y(a.at[1])) - Canvas::TextHeight(fontScale) / 2, shorten(a.label, 20), ink, fontScale, paper);
            }

        // Furniture: title, north arrow, scale bar, legend.
        std::string title = opts.contains("title") && opts.at("title").is_string() ? opts.at("title").get<std::string>() : std::string();
        if (!storeyName.empty()) title += (title.empty() ? "" : " - ") + std::string("storey ") + storeyName;
        if (!title.empty())
        {
            title = shorten(title, 60);
            canvas.FillRect(view.margin / 2.0, view.margin / 2.0, view.margin / 2.0 + Canvas::TextWidth(title, fontScale) + 2 * pad, view.margin / 2.0 + Canvas::TextHeight(fontScale) + 2 * pad, Colour{0, 0, 0, 150});
            canvas.Text(view.margin / 2 + pad, view.margin / 2 + pad, title, white, fontScale);
        }
        {
            const double nx = planW - view.margin - 10.0 * fontScale, ny = view.margin + 4.0 * fontScale;
            canvas.FillPolygon({{nx, ny}, {nx + 6 * fontScale, ny + 14 * fontScale}, {nx, ny + 10 * fontScale}, {nx - 6 * fontScale, ny + 14 * fontScale}}, white);
            canvas.Text(static_cast<int>(nx) - 2 * fontScale, static_cast<int>(ny + 16 * fontScale), "N", white, fontScale);
        }
        {
            const double length = Nice(std::min(planW * 0.25, 120.0 * fontScale) / view.scale / 2);
            const double pixels = length * view.scale;
            const double x0 = view.margin, y0 = planH - view.margin - 6.0 * fontScale;
            const auto text = Units(length);
            canvas.FillRect(x0 - pad, y0 - Canvas::TextHeight(fontScale) - 2 * pad, x0 + std::max(pixels, static_cast<double>(Canvas::TextWidth(text, fontScale))) + pad, y0 + 6 * fontScale + pad, Colour{0, 0, 0, 150});
            canvas.FillRect(x0, y0, x0 + pixels, y0 + 4 * fontScale, white);
            canvas.FillRect(x0 + pixels / 2, y0 + 1, x0 + pixels, y0 + 4 * fontScale - 1, Colour{60, 60, 60});
            canvas.Text(static_cast<int>(x0), static_cast<int>(y0) - Canvas::TextHeight(fontScale) - pad / 2, text, white, fontScale);
        }
        if (!legendKinds.empty())
        {
            const int x0 = legendBelow ? 0 : planW, y0 = legendBelow ? planH : 0;
            canvas.FillRect(x0, y0, width, height, Colour{28, 30, 34});
            for (size_t i = 0; i < legendKinds.size(); ++i)
            {
                const auto& style = ActorStyle(legendKinds[i]);
                const int column = legendBelow ? static_cast<int>(i) % columns : 0, row = legendBelow ? static_cast<int>(i) / columns : static_cast<int>(i);
                const int x = x0 + (legendBelow ? view.margin : pad) + column * legendColumn, cy = y0 + (legendBelow ? pad : view.margin) + row * lineHeight + lineHeight / 2;
                Icon(canvas, style.shape, x + lineHeight / 2.0 - 2, cy, std::max(3.0, lineHeight / 3.0), style);
                canvas.Text(x + lineHeight, cy - Canvas::TextHeight(fontScale) / 2, style.legend, white, fontScale);
            }
        }
        return canvas.image;
    }
    // The plan as PNG bytes.
    inline std::string PlanPng(const Json& scene, const Json& options = Json::object())
    {
        return Image::EncodePng(PlanImage(scene, options));
    }
}
