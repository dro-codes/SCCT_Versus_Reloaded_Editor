#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

// Map Outliner (View > Map Outliner): every actor of the open map as rows,
// filtered by a search over all columns and grouped by class, by the map's
// Group field or not at all. Engine-independent: the window turns the
// editor's actors into Rows and shows the Entries this builds in a virtual
// list, one entry per header or actor line.
namespace Outliner
{
struct Row
{
    std::uintptr_t handle = 0; // The actor, for the editor side; never read here.
    std::string path, name, cls, group, tag, event;
    float location[3]{};
    bool hidden = false, locked = false, selected = false;
    bool authorable = true; // False for the level info and the builder brush.
};

enum class GroupBy : int { None = 0, Class = 1, Group = 2 };
enum class Column : int { Name = 0, Class, Group, Tag, Event, Location, Hidden, Locked, Count };

inline const char* ColumnName(Column c)
{
    static const char* names[] = {"Name", "Class", "Group", "Tag", "Event", "Location", "Hidden", "Locked"};
    const int i = static_cast<int>(c);
    return i >= 0 && i < static_cast<int>(Column::Count) ? names[i] : "";
}

inline std::string Lower(std::string text)
{
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// "Engine.StaticMeshActor" -> "StaticMeshActor".
inline std::string ShortClass(const std::string& cls)
{
    const auto dot = cls.find_last_of('.');
    return dot == std::string::npos ? cls : cls.substr(dot + 1);
}

// The stock editor writes None for an empty name property.
inline bool Blank(const std::string& value) { return value.empty() || Lower(value) == "none"; }
inline std::string Shown(const std::string& value) { return Blank(value) ? std::string() : value; }

// Whole units, as the editor's own location boxes show them.
inline std::string LocationText(const float (&v)[3])
{
    char text[96];
    auto whole = [](float f) { return static_cast<long long>(std::llround(std::isfinite(f) ? f : 0.0f)); };
    std::snprintf(text, sizeof(text), "%lld, %lld, %lld", whole(v[0]), whole(v[1]), whole(v[2]));
    return text;
}

// The cell text of a row, as the list shows it.
inline std::string Cell(const Row& row, Column c)
{
    switch (c)
    {
    case Column::Name: return row.name;
    case Column::Class: return ShortClass(row.cls);
    case Column::Group: return Shown(row.group);
    case Column::Tag: return Shown(row.tag);
    case Column::Event: return Shown(row.event);
    case Column::Location: return LocationText(row.location);
    case Column::Hidden: return row.hidden ? "Hidden" : "";
    case Column::Locked: return row.locked ? "Locked" : "";
    default: return {};
    }
}

// The group names of a Group field: comma-separated, blanks and None left
// out, each name once whatever its case (as Map Design writes them).
inline std::vector<std::string> GroupNames(const std::string& groups)
{
    std::vector<std::string> names;
    std::set<std::string> seen;
    size_t at = 0;
    while (at <= groups.size())
    {
        size_t comma = groups.find(',', at);
        if (comma == std::string::npos) comma = groups.size();
        std::string name = groups.substr(at, comma - at);
        const auto begin = name.find_first_not_of(" \t"), end = name.find_last_not_of(" \t");
        name = begin == std::string::npos ? std::string() : name.substr(begin, end - begin + 1);
        if (!Blank(name) && seen.insert(Lower(name)).second) names.push_back(name);
        at = comma + 1;
    }
    return names;
}

// Search words: split on spaces, case ignored. Every word must appear in
// some column of a row (several words narrow the list).
inline std::vector<std::string> SearchWords(const std::string& text)
{
    std::vector<std::string> words;
    std::string word;
    for (char c : text + " ")
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (!word.empty()) words.push_back(Lower(word)); word.clear(); }
        else word += c;
    }
    return words;
}

// What a row is searched by: every column, lower case, with a separator no
// typed word holds, so a word never matches across two columns. The class is
// searched by its full name, which holds the short one, and Hidden / Locked
// by those words.
inline std::string SearchText(const Row& row)
{
    std::string text;
    for (int c = 0; c < static_cast<int>(Column::Count); ++c)
    {
        const auto column = static_cast<Column>(c);
        text += Lower(column == Column::Class ? row.cls : Cell(row, column));
        text += '\x01';
    }
    return text;
}
inline bool Matches(const std::string& searchText, const std::vector<std::string>& words)
{
    for (const auto& word : words) if (searchText.find(word) == std::string::npos) return false;
    return true;
}

// Names compare as people read them: case ignored and runs of digits by
// value, so StaticMeshActor2 comes before StaticMeshActor10.
inline int NaturalCompare(const std::string& a, const std::string& b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size())
    {
        const bool da = a[i] >= '0' && a[i] <= '9', db = b[j] >= '0' && b[j] <= '9';
        if (da && db)
        {
            size_t ei = i, ej = j;
            while (ei < a.size() && a[ei] >= '0' && a[ei] <= '9') ++ei;
            while (ej < b.size() && b[ej] >= '0' && b[ej] <= '9') ++ej;
            size_t si = i, sj = j;
            while (si + 1 < ei && a[si] == '0') ++si;
            while (sj + 1 < ej && b[sj] == '0') ++sj;
            if (ei - si != ej - sj) return ei - si < ej - sj ? -1 : 1;
            for (; si < ei; ++si, ++sj) if (a[si] != b[sj]) return a[si] < b[sj] ? -1 : 1;
            i = ei; j = ej;
            continue;
        }
        char ca = a[i], cb = b[j];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb) ? -1 : 1;
        ++i; ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

inline int CompareRows(const Row& a, const Row& b, Column by)
{
    int result = 0;
    switch (by)
    {
    case Column::Location:
        for (int axis = 0; axis < 3 && !result; ++axis)
            if (a.location[axis] != b.location[axis]) result = a.location[axis] < b.location[axis] ? -1 : 1;
        break;
    case Column::Hidden: result = a.hidden == b.hidden ? 0 : a.hidden ? -1 : 1; break;
    case Column::Locked: result = a.locked == b.locked ? 0 : a.locked ? -1 : 1; break;
    default:
        // Blank values (no Tag, no group) after the others.
        {
            const auto x = Cell(a, by), y = Cell(b, by);
            if (x.empty() != y.empty()) result = x.empty() ? 1 : -1;
            else result = NaturalCompare(x, y);
        }
    }
    return result ? result : NaturalCompare(a.name, b.name);
}

struct Group
{
    std::string key;   // lower case
    std::string label; // as shown
    std::vector<int> rows; // matching rows, in order
    bool collapsed = false;
};

// One line of the list: a group header (row -1) or an actor.
struct Entry
{
    int row = -1;
    int group = -1; // index into View::groups, -1 when not grouped
    bool Header() const { return row < 0; }
};

struct View
{
    std::vector<Group> groups;
    std::vector<Entry> entries;
    size_t matching = 0; // rows that match the search (once each)
};

constexpr char kNoGroup[] = "(no group)";

struct Options
{
    GroupBy groupBy = GroupBy::None;
    Column sortBy = Column::Name;
    bool descending = false;
    std::vector<std::string> words;
    std::set<std::string> collapsed; // lower-case group keys
};

// The list: the rows matching every search word, sorted, under one header
// per group. By Class, each class is a group; by Group, each name in an
// actor's Group field is one, so an actor in two groups is listed in both,
// and actors in none are listed last under (no group). Groups are in name
// order; a group with no matching row has no header; a collapsed group keeps
// its header and count but lists no rows. searchTexts are SearchText(row),
// made once per read of the map rather than once per key press.
inline View Build(const std::vector<Row>& rows, const std::vector<std::string>& searchTexts, const Options& options)
{
    View view;
    std::vector<int> order;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        const bool match = options.words.empty()
            || Matches(i < static_cast<int>(searchTexts.size()) ? searchTexts[i] : SearchText(rows[i]), options.words);
        if (match) order.push_back(i);
    }
    view.matching = order.size();
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const int c = CompareRows(rows[a], rows[b], options.sortBy);
        return options.descending ? c > 0 : c < 0;
    });
    if (options.groupBy == GroupBy::None)
    {
        view.entries.reserve(order.size());
        for (int i : order) view.entries.push_back({i, -1});
        return view;
    }
    std::map<std::string, int> byKey;
    auto groupFor = [&](const std::string& label) {
        const auto key = Lower(label);
        auto found = byKey.find(key);
        if (found != byKey.end()) return found->second;
        Group g;
        g.key = key;
        g.label = label;
        g.collapsed = options.collapsed.count(key) != 0;
        view.groups.push_back(std::move(g));
        return byKey[key] = static_cast<int>(view.groups.size()) - 1;
    };
    for (int i : order)
    {
        const auto& row = rows[i];
        if (options.groupBy == GroupBy::Class) view.groups[groupFor(ShortClass(row.cls))].rows.push_back(i);
        else
        {
            const auto names = GroupNames(row.group);
            if (names.empty()) view.groups[groupFor(kNoGroup)].rows.push_back(i);
            for (const auto& name : names) view.groups[groupFor(name)].rows.push_back(i);
        }
    }
    std::vector<int> groupOrder(view.groups.size());
    for (size_t i = 0; i < groupOrder.size(); ++i) groupOrder[i] = static_cast<int>(i);
    std::sort(groupOrder.begin(), groupOrder.end(), [&](int a, int b) {
        const bool na = view.groups[a].key == kNoGroup, nb = view.groups[b].key == kNoGroup;
        if (na != nb) return nb;
        return NaturalCompare(view.groups[a].label, view.groups[b].label) < 0;
    });
    std::vector<Group> sorted;
    sorted.reserve(view.groups.size());
    for (int g : groupOrder) sorted.push_back(std::move(view.groups[g]));
    view.groups = std::move(sorted);
    for (int g = 0; g < static_cast<int>(view.groups.size()); ++g)
    {
        view.entries.push_back({-1, g});
        if (!view.groups[g].collapsed)
            for (int r : view.groups[g].rows) view.entries.push_back({r, g});
    }
    return view;
}

// The header's text: "- StaticMeshActor (1,234)", + when collapsed.
inline std::string Thousands(size_t n)
{
    std::string digits = std::to_string(n), out;
    for (size_t i = 0; i < digits.size(); ++i)
    {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}
inline std::string HeaderText(const Group& g)
{
    return std::string(g.collapsed ? "[+] " : "[-] ") + g.label + "  (" + Thousands(g.rows.size()) + ")";
}

// The rows chosen by selected list entries, each once, in list order. A
// selected header stands for every matching row of its group, collapsed or
// not.
inline std::vector<int> ChosenRows(const View& view, const std::vector<int>& selectedEntries)
{
    std::vector<int> rows;
    std::set<int> seen;
    for (int e : selectedEntries)
    {
        if (e < 0 || e >= static_cast<int>(view.entries.size())) continue;
        const auto& entry = view.entries[e];
        if (entry.Header())
        {
            for (int r : view.groups[entry.group].rows) if (seen.insert(r).second) rows.push_back(r);
        }
        else if (seen.insert(entry.row).second) rows.push_back(entry.row);
    }
    return rows;
}

// Follow Selection: the actor lines whose actor is selected in the editor.
inline std::vector<int> SelectedEntries(const View& view, const std::vector<Row>& rows)
{
    std::vector<int> entries;
    for (int e = 0; e < static_cast<int>(view.entries.size()); ++e)
    {
        const auto& entry = view.entries[e];
        if (!entry.Header() && entry.row < static_cast<int>(rows.size()) && rows[entry.row].selected) entries.push_back(e);
    }
    return entries;
}

// What a click selects in the editor: the chosen rows that are on show.
// Hidden actors are never selected, as with RE+: Select, so the next Delete
// or move cannot change what cannot be seen.
struct Choice { std::vector<int> rows; size_t hidden = 0; };
inline Choice Selectable(const std::vector<Row>& rows, const std::vector<int>& chosen)
{
    Choice choice;
    for (int r : chosen)
    {
        if (r < 0 || r >= static_cast<int>(rows.size())) continue;
        if (rows[r].hidden) ++choice.hidden;
        else choice.rows.push_back(r);
    }
    return choice;
}

// What Delete removes: the chosen rows other than the level info and the
// builder brush, which the map cannot do without. Hidden rows are deleted
// too: they were picked by name in the list, not in a viewport.
struct DeletePlan { std::vector<int> rows; size_t kept = 0; };
inline DeletePlan PlanDelete(const std::vector<Row>& rows, const std::vector<int>& chosen)
{
    DeletePlan plan;
    for (int r : chosen)
    {
        if (r < 0 || r >= static_cast<int>(rows.size())) continue;
        if (rows[r].authorable) plan.rows.push_back(r);
        else ++plan.kept;
    }
    return plan;
}
inline std::string DeleteQuestion(const std::vector<Row>& rows, const DeletePlan& plan)
{
    std::string text;
    if (plan.rows.size() == 1) text = "Delete " + rows[plan.rows[0]].name + "?";
    else
    {
        text = "Delete these " + Thousands(plan.rows.size()) + " actors?";
        for (size_t i = 0; i < plan.rows.size() && i < 8; ++i) text += "\n    " + rows[plan.rows[i]].name;
        if (plan.rows.size() > 8) text += "\n    ... and " + Thousands(plan.rows.size() - 8) + " more";
    }
    if (plan.kept) text += "\n\nThe level info and the builder brush stay.";
    return text + "\n\nOne Undo brings them back.";
}

// A header's group toggles open or shut; the collapsed keys are returned for
// the next Build.
inline std::set<std::string> Toggle(const View& view, int entry, std::set<std::string> collapsed)
{
    if (entry < 0 || entry >= static_cast<int>(view.entries.size()) || !view.entries[entry].Header()) return collapsed;
    const auto& key = view.groups[view.entries[entry].group].key;
    if (!collapsed.erase(key)) collapsed.insert(key);
    return collapsed;
}

// The list's footer.
inline std::string Summary(const std::vector<Row>& rows, const View& view, size_t selected)
{
    size_t hidden = 0;
    for (const auto& r : rows) hidden += r.hidden;
    std::string text = Thousands(rows.size()) + " actors";
    if (view.matching != rows.size()) text += ", " + Thousands(view.matching) + " match the search";
    if (!view.groups.empty()) text += ", in " + Thousands(view.groups.size()) + " group(s)";
    text += "; " + Thousands(selected) + " selected, " + Thousands(hidden) + " hidden.";
    return text;
}
}
