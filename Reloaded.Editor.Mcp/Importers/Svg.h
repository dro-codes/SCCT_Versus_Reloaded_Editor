#pragma once
// SVG floor plans (a subset) to scct.level-spec. Engine-free; the XML is read
// by Xml.h.
//
// Shapes: rect, polygon, polyline, line, path (M L H V Z, curves by their end
// points), circle, ellipse, use and text, inside g/a groups with transform
// (matrix, translate, scale, rotate, skewX, skewY). Hidden elements
// (display:none, visibility:hidden) are skipped.
// What a shape is: its data-kind, else a class name, else its
// inkscape:label or id ("room-3", "spy_1"), else what its group says; a
// closed shape without one is a room. The kinds are PlanShapes.h's. Other
// attributes: data-name (or a <title>), data-yaw (degrees, east 0, north
// 90), data-h (height above the floor), data-z (floor offset), data-height,
// data-mesh, data-class, data-sound, data-radius, data-brightness, data-h0
// and data-h1 (a line's end heights), data-portal, and data-prop-<Name> for
// raw actor properties. data-* lengths are Unreal units. A text inside a
// room names it.
// Layers are storeys: top-level groups with inkscape:groupmode="layer", a
// data-storey, or a label or id such as "storey 1" / "floor-2"; data-z sets
// a storey's height. Everything else is storey 0.
// Scale (Unreal units per SVG unit): options.scale, else the root's
// data-scale, else the root's physical width over its viewBox (at
// uuPerMetre, 100), else 8. SVG's y runs down, so it is flipped (north up),
// and the plan is centred on the origin (center: false keeps it). The
// storey, room and door options are GridPlanModel's.
#include "PlanShapes.h"
#include "Xml.h"
#include <cstring>

namespace Mcp::Import
{
    namespace SvgDetail
    {
        // x' = a x + c y + e, y' = b x + d y + f.
        struct Affine
        {
            double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
            Affine operator*(const Affine& m) const { return {a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d, b * m.c + d * m.d, a * m.e + c * m.f + e, b * m.e + d * m.f + f}; }
            Grid::P2 Apply(double x, double y) const { return {a * x + c * y + e, b * x + d * y + f}; }
        };
        inline Affine Transform(const std::string& text)
        {
            Affine m;
            size_t at = 0;
            while (at < text.size())
            {
                const auto open = text.find('(', at);
                if (open == std::string::npos) break;
                const auto close = text.find(')', open);
                if (close == std::string::npos) throw std::runtime_error("An SVG transform is missing a \")\": " + text);
                auto name = Trim(text.substr(at, open - at));
                while (!name.empty() && (name[0] == ',' || std::isspace(static_cast<unsigned char>(name[0])))) name.erase(0, 1);
                const auto v = Numbers(text.substr(open + 1, close - open - 1));
                constexpr double rad = 3.14159265358979323846 / 180;
                Affine t;
                if (name == "matrix" && v.size() == 6) t = {v[0], v[1], v[2], v[3], v[4], v[5]};
                else if (name == "translate" && !v.empty()) t = {1, 0, 0, 1, v[0], v.size() > 1 ? v[1] : 0};
                else if (name == "scale" && !v.empty()) t = {v[0], 0, 0, v.size() > 1 ? v[1] : v[0], 0, 0};
                else if (name == "rotate" && !v.empty())
                {
                    const double c = std::cos(v[0] * rad), s = std::sin(v[0] * rad);
                    t = {c, s, -s, c, 0, 0};
                    if (v.size() >= 3) t = Affine{1, 0, 0, 1, v[1], v[2]} * t * Affine{1, 0, 0, 1, -v[1], -v[2]};
                }
                else if (name == "skewX" && !v.empty()) t = {1, 0, std::tan(v[0] * rad), 1, 0, 0};
                else if (name == "skewY" && !v.empty()) t = {1, std::tan(v[0] * rad), 0, 1, 0, 0};
                else throw std::runtime_error("Unknown SVG transform \"" + name + "\".");
                m = m * t;
                at = close + 1;
            }
            return m;
        }
        // Path data: each subpath as points, and whether it is closed. Curves
        // keep their end points only.
        inline std::vector<std::pair<std::vector<Grid::P2>, bool>> PathPoints(const std::string& d, Json& report)
        {
            std::vector<std::pair<std::vector<Grid::P2>, bool>> out;
            std::vector<Grid::P2> current;
            double x = 0, y = 0, sx = 0, sy = 0;
            char command = 0;
            size_t at = 0;
            bool curved = false;
            auto number = [&]() -> double
            {
                while (at < d.size() && (std::isspace(static_cast<unsigned char>(d[at])) || d[at] == ',')) ++at;
                const size_t start = at;
                if (at < d.size() && (d[at] == '-' || d[at] == '+')) ++at;
                bool dot = false, digits = false;
                while (at < d.size() && (std::isdigit(static_cast<unsigned char>(d[at])) || (d[at] == '.' && !dot)))
                {
                    if (d[at] == '.') dot = true; else digits = true;
                    ++at;
                }
                if (digits && at < d.size() && (d[at] == 'e' || d[at] == 'E'))
                {
                    size_t e = at + 1;
                    if (e < d.size() && (d[e] == '-' || d[e] == '+')) ++e;
                    if (e < d.size() && std::isdigit(static_cast<unsigned char>(d[e]))) { at = e; while (at < d.size() && std::isdigit(static_cast<unsigned char>(d[at]))) ++at; }
                }
                if (!digits) throw std::runtime_error("SVG path data has a bad number near \"" + d.substr(start, 12) + "\".");
                return std::stod(d.substr(start, at - start));
            };
            auto more = [&]
            {
                while (at < d.size() && (std::isspace(static_cast<unsigned char>(d[at])) || d[at] == ',')) ++at;
                return at < d.size() && (std::isdigit(static_cast<unsigned char>(d[at])) || d[at] == '-' || d[at] == '+' || d[at] == '.');
            };
            auto finish = [&](bool closed) { if (!current.empty()) out.push_back({current, closed}); current.clear(); };
            while (at < d.size())
            {
                while (at < d.size() && (std::isspace(static_cast<unsigned char>(d[at])) || d[at] == ',')) ++at;
                if (at >= d.size()) break;
                if (std::isalpha(static_cast<unsigned char>(d[at]))) command = d[at++];
                else if (!command) throw std::runtime_error("SVG path data must start with a command.");
                const bool rel = std::islower(static_cast<unsigned char>(command)) != 0;
                switch (std::toupper(static_cast<unsigned char>(command)))
                {
                case 'M':
                {
                    finish(false);
                    const double nx = number(), ny = number();
                    x = rel ? x + nx : nx; y = rel ? y + ny : ny; sx = x; sy = y;
                    current.push_back({x, y});
                    command = rel ? 'l' : 'L';
                    break;
                }
                case 'L': { const double nx = number(), ny = number(); x = rel ? x + nx : nx; y = rel ? y + ny : ny; current.push_back({x, y}); break; }
                case 'H': { const double nx = number(); x = rel ? x + nx : nx; current.push_back({x, y}); break; }
                case 'V': { const double ny = number(); y = rel ? y + ny : ny; current.push_back({x, y}); break; }
                case 'Z': x = sx; y = sy; finish(true); if (more()) throw std::runtime_error("SVG path data has numbers after Z."); break;
                case 'C': case 'S': case 'Q': case 'T': case 'A':
                {
                    const int count = std::toupper(static_cast<unsigned char>(command)) == 'C' ? 6 : std::toupper(static_cast<unsigned char>(command)) == 'A' ? 7 :
                                      std::toupper(static_cast<unsigned char>(command)) == 'T' ? 2 : 4;
                    double v[7] = {0, 0, 0, 0, 0, 0, 0};
                    for (int i = 0; i < count; ++i) v[i] = number();
                    x = rel ? x + v[count - 2] : v[count - 2]; y = rel ? y + v[count - 1] : v[count - 1];
                    current.push_back({x, y});
                    curved = true;
                    break;
                }
                default: throw std::runtime_error(std::string("Unknown SVG path command '") + command + "'.");
                }
                if (std::toupper(static_cast<unsigned char>(command)) == 'Z') command = 0;
            }
            finish(false);
            if (curved) Warn(report, "Curves in SVG paths were straightened (only their end points are kept).");
            return out;
        }
        inline double Length(const std::string& text, double fallback)
        {
            double n = 0;
            const auto t = Trim(text);
            size_t end = 0;
            try { n = std::stod(t, &end); } catch (const std::exception&) { return fallback; }
            return std::isfinite(n) ? n : fallback;
        }
        inline bool Hidden(const Xml::Node& n)
        {
            if (Lower(n.Get("display")) == "none" || Lower(n.Get("visibility")) == "hidden") return true;
            auto style = Lower(n.Get("style"));
            style.erase(std::remove_if(style.begin(), style.end(), [](unsigned char c) { return std::isspace(c); }), style.end());
            return style.find("display:none") != std::string::npos || style.find("visibility:hidden") != std::string::npos;
        }
        using Shapes::KindWord;
        using Shapes::StoreyNumber;
    }
    inline Json ImportSvg(const std::string& text, const Json& options, Json& report)
    {
        using namespace SvgDetail;
        Report(report);
        const auto o = Grid::ReadOptions(options);
        const auto root = Xml::Parse(text);
        if (root.Local() != "svg") throw std::runtime_error("The XML's root is <" + root.name + ">, not <svg>.");
        // Unreal units per SVG unit.
        double scale = Option(options, "scale", 0.0, 0, 100000);
        const double uuPerMetre = Option(options, "uuPerMetre", 100.0, 1, 10000);
        std::string scaleFrom = "the scale option";
        if (scale <= 0 && root.Has("data-scale")) { scale = Length(root.Get("data-scale"), 0); scaleFrom = "the SVG's data-scale"; }
        if (scale <= 0 && root.Has("viewBox") && root.Has("width"))
        {
            const auto box = Numbers(root.Get("viewBox"));
            const auto width = Trim(root.Get("width"));
            static const std::vector<std::pair<std::string, double>> units{{"mm", 0.001}, {"cm", 0.01}, {"m", 1}, {"in", 0.0254}, {"ft", 0.3048}, {"pt", 0.0254 / 72}};
            for (const auto& [unit, metres] : units)
                if (box.size() == 4 && box[2] > 0 && EndsWith(width, unit) && (unit != "m" || !EndsWith(width, "mm")))
                {
                    const double n = Length(width.substr(0, width.size() - unit.size()), 0);
                    if (n > 0) { scale = n * metres / box[2] * uuPerMetre; scaleFrom = "the SVG's width (" + width + ") at " + Num(uuPerMetre).dump() + " units a metre"; }
                    break;
                }
        }
        if (scale <= 0) { scale = 8; scaleFrom = "the default (no scale option, data-scale or physical width)"; }
        Assume(report, "One SVG unit is " + Num(scale).dump() + " Unreal units, from " + scaleFrom + "; y is flipped so the top of the drawing is north.");

        Grid::Plan plan;
        plan.format = "svg";
        // Layers: top-level groups that are storeys.
        struct LayerInfo { const Xml::Node* node; int number; std::string name; std::optional<double> z; };
        std::vector<LayerInfo> layers;
        int index = 0;
        for (const auto& child : root.children)
        {
            if (child.Local() != "g") continue;
            const auto label = child.Get("inkscape:label", child.Get("data-name", child.Get("id")));
            const bool layer = child.Get("inkscape:groupmode") == "layer" || child.Has("data-storey") || StoreyNumber(label).has_value();
            if (!layer) continue;
            std::optional<int> number;
            if (child.Has("data-storey")) number = static_cast<int>(std::lround(Length(child.Get("data-storey"), index)));
            else number = StoreyNumber(label);
            LayerInfo info{&child, number.value_or(index), label, std::nullopt};
            if (child.Has("data-z")) info.z = Length(child.Get("data-z"), 0);
            layers.push_back(info);
            ++index;
        }
        std::map<const Xml::Node*, std::string> storeyOf;
        std::set<int> numbers;
        for (const auto& l : layers)
        {
            if (!numbers.insert(l.number).second) throw std::runtime_error("Two SVG layers are storey " + std::to_string(l.number) + "; give each layer its own data-storey.");
            Grid::StoreyInfo s{Grid::StoreyId(l.number), l.number, l.z ? *l.z : l.number * o.storeyHeight, l.name};
            plan.ids.Reserve(s.id);
            plan.storeys.push_back(s);
            storeyOf[l.node] = s.id;
        }
        const std::string ground = Grid::StoreyId(0);
        bool usedGround = false;
        struct Label { double x, y; std::string storey, text; };
        std::vector<Label> labels;
        long long shapes = 0;
        std::function<void(const Xml::Node&, const Affine&, const std::string&, const std::string&)> visit =
            [&](const Xml::Node& n, const Affine& parent, const std::string& storey, const std::string& inherited)
        {
            if (Hidden(n)) return;
            const auto tag = n.Local();
            static const std::set<std::string> skipped{"defs", "symbol", "clipPath", "mask", "pattern", "marker", "style", "script", "metadata", "title", "desc", "linearGradient", "radialGradient", "filter"};
            if (skipped.count(tag)) return;
            const Affine m = n.Has("transform") ? parent * Transform(n.Get("transform")) : parent;
            // Attributes and kind.
            auto get = [&](const char* key) -> std::string
            {
                const std::string k = key;
                if (k == "id") return n.Get("id");
                if (k == "name")
                {
                    if (n.Has("data-name")) return n.Get("data-name");
                    for (const auto& c : n.children) if (c.Local() == "title") return Trim(c.AllText());
                    return {};
                }
                if (k == "kind") return n.Get("data-kind");
                return n.Get("data-" + k);
            };
            auto attributes = Shapes::ReadAttributes(get);
            if (attributes.kind.empty())
            {
                std::string cls = n.Get("class");
                for (size_t start = 0; start < cls.size() && attributes.kind.empty();)
                {
                    const auto end = cls.find(' ', start);
                    attributes.kind = Grid::Detail::KindOf(cls.substr(start, end == std::string::npos ? std::string::npos : end - start));
                    start = end == std::string::npos ? cls.size() : end + 1;
                }
                if (attributes.kind.empty() && n.Has("inkscape:label")) attributes.kind = KindWord(n.Get("inkscape:label"));
                if (attributes.kind.empty() && n.Has("id")) attributes.kind = KindWord(n.Get("id"));
                attributes.explicitKind = !attributes.kind.empty();
                if (attributes.kind.empty()) attributes.kind = inherited;
            }
            for (const auto& [key, value] : n.attributes)
                if (StartsWith(key, "data-prop-") && key.size() > 10) attributes.props[key.substr(10)] = value;
            // data-* heights are Unreal units already; nothing to scale.
            auto world = [&](double x, double y) { const auto p = m.Apply(x, y); return Grid::P2{p[0] * scale, -p[1] * scale}; };
            const std::string& here = storey;
            if (here == ground) usedGround = true;
            auto num = [&](const char* key) { return Length(n.Get(key), 0); };
            if (tag == "g" || tag == "a" || tag == "svg" || tag == "switch")
            {
                // A group's kind is its children's default.
                const auto groupKind = attributes.kind;
                for (const auto& c : n.children)
                {
                    const auto found = storeyOf.find(&c);
                    visit(c, m, found == storeyOf.end() ? storey : found->second, groupKind);
                }
                return;
            }
            ++shapes;
            if (tag == "rect")
            {
                // A rect sized in percent is a page background, not a room.
                if (EndsWith(Trim(n.Get("width")), "%") || EndsWith(Trim(n.Get("height")), "%")) { Warn(report, "Rectangles sized in percent (page backgrounds) were skipped."); return; }
                const double x = num("x"), y = num("y"), w = num("width"), h = num("height");
                if (w <= 0 || h <= 0) return;
                Shapes::AddClosed(plan, o, {world(x, y), world(x + w, y), world(x + w, y + h), world(x, y + h)}, attributes, here, report);
            }
            else if (tag == "polygon" || tag == "polyline")
            {
                const auto v = Numbers(n.Get("points"));
                std::vector<Grid::P2> points;
                for (size_t i = 0; i + 1 < v.size(); i += 2) points.push_back(world(v[i], v[i + 1]));
                if (tag == "polygon" || (attributes.kind.empty() ? false : Shapes::PieceKind(attributes.kind) || attributes.kind == "solid")) Shapes::AddClosed(plan, o, points, attributes, here, report);
                else Shapes::AddOpen(plan, o, points, attributes, here, report);
            }
            else if (tag == "path")
            {
                for (const auto& [points, closed] : PathPoints(n.Get("d"), report))
                {
                    std::vector<Grid::P2> w;
                    for (const auto& p : points) w.push_back(world(p[0], p[1]));
                    if (closed) Shapes::AddClosed(plan, o, w, attributes, here, report);
                    else Shapes::AddOpen(plan, o, w, attributes, here, report);
                }
            }
            else if (tag == "line") Shapes::AddOpen(plan, o, {world(num("x1"), num("y1")), world(num("x2"), num("y2"))}, attributes, here, report);
            else if (tag == "circle" || tag == "ellipse")
            {
                if (!attributes.radius && tag == "circle" && attributes.kind == "light") attributes.radius = num("r") * scale;
                Shapes::AddPoint(plan, world(num("cx"), num("cy")), attributes, here, report);
            }
            else if (tag == "use" || tag == "image")
            {
                if (attributes.kind.empty())
                {
                    auto href = n.Get("href", n.Get("xlink:href"));
                    if (!href.empty() && href[0] == '#') attributes.kind = KindWord(href.substr(1));
                }
                const double w = num("width"), h = num("height");
                if (attributes.kind.empty() && tag == "image") return;
                Shapes::AddPoint(plan, world(num("x") + w / 2, num("y") + h / 2), attributes, here, report);
            }
            else if (tag == "text")
            {
                double x = num("x"), y = num("y");
                if (!n.Has("x")) for (const auto& c : n.children) if (c.Local() == "tspan" && c.Has("x")) { x = Length(c.Get("x"), 0); y = Length(c.Get("y"), 0); break; }
                const auto p = world(x, y);
                if (Shapes::MarkerKind(attributes.kind) && attributes.explicitKind) Shapes::AddPoint(plan, p, attributes, here, report);
                else labels.push_back({p[0], p[1], here, n.AllText()});
            }
            else --shapes;
        };
        visit(root, Affine(), ground, "");
        if (usedGround || layers.empty())
            if (!plan.Storey(ground))
            {
                plan.storeys.push_back({ground, 0, 0, ""});
                plan.ids.Reserve(ground);
            }
        for (const auto& l : labels) Shapes::NameSpaces(plan, l.x, l.y, l.storey, l.text);
        if (o.center && Shapes::Center(plan)) Assume(report, "The drawing is centred on the origin (center false keeps the SVG's own origin).");
        Count(report, "shapes", shapes);
        Count(report, "storeys", static_cast<long long>(plan.storeys.size()));
        if (plan.spaces.empty() && plan.prisms.empty()) Warn(report, "The SVG has no rooms; draw them as rectangles or polygons (closed shapes are rooms unless a data-kind says otherwise).");
        return Grid::ToSpec(plan, o, options, report);
    }
}
