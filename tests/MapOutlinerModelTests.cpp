#include "../Reloaded.Editor/MapOutlinerModel.h"
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
using namespace Outliner;
int checks = 0;
void Check(bool value, const char* message, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) throw std::runtime_error(std::string(message) + " (line " + std::to_string(where.line()) + ")");
}

Row Make(const char* name, const char* cls, const char* group = "None", const char* tag = "None", const char* event = "None")
{
    Row r;
    r.name = name;
    r.path = std::string("MyLevel.") + name;
    r.cls = cls;
    r.group = group;
    r.tag = tag;
    r.event = event;
    return r;
}
std::vector<Row> Sample()
{
    std::vector<Row> rows;
    rows.push_back(Make("LevelInfo0", "Engine.LevelInfo"));
    rows.back().authorable = false;
    rows.push_back(Make("StaticMeshActor10", "Engine.StaticMeshActor", "Lobby,Props"));
    rows.push_back(Make("StaticMeshActor2", "Engine.StaticMeshActor", "props"));
    rows.push_back(Make("Light5", "Engine.Light", "Lobby", "LobbyLights"));
    rows.push_back(Make("SAlarm0", "SBase.SAlarm", "None", "Alarm1", "DoorOpen"));
    rows.push_back(Make("Brush3", "Engine.Brush"));
    rows[1].location[0] = 100.4f; rows[1].location[1] = -20.6f; rows[1].location[2] = 64;
    rows[3].hidden = true;
    rows[4].locked = true;
    return rows;
}
std::vector<std::string> Texts(const std::vector<Row>& rows)
{
    std::vector<std::string> texts;
    for (const auto& r : rows) texts.push_back(SearchText(r));
    return texts;
}
std::vector<std::string> Names(const std::vector<Row>& rows, const View& view)
{
    std::vector<std::string> names;
    for (const auto& e : view.entries) names.push_back(e.Header() ? "#" + view.groups[e.group].label : rows[e.row].name);
    return names;
}

void Cells()
{
    auto rows = Sample();
    Check(Cell(rows[1], Column::Class) == "StaticMeshActor", "the class shows its short name");
    Check(Cell(rows[1], Column::Location) == "100, -21, 64", "location in whole units, rounded");
    Check(Cell(rows[0], Column::Tag).empty() && Cell(rows[4], Column::Tag) == "Alarm1", "None shows blank");
    Check(Cell(rows[3], Column::Hidden) == "Hidden" && Cell(rows[4], Column::Locked) == "Locked" && Cell(rows[1], Column::Hidden).empty(), "flags as words");
    Check(ShortClass("NoPackage") == "NoPackage", "a class with no package");
    Check(std::string(ColumnName(Column::Event)) == "Event" && std::string(ColumnName(Column::Count)).empty(), "column names");
    float odd[3] = {std::nanf(""), 1e30f * 0, -0.4f};
    Check(LocationText(odd) == "0, 0, 0", "a broken location reads 0, not garbage");
}

void Groups()
{
    auto names = GroupNames("Lobby, props ,LOBBY,None,,  ");
    Check(names.size() == 2 && names[0] == "Lobby" && names[1] == "props", "groups split, trimmed, once each, None left out");
    Check(GroupNames("None").empty() && GroupNames("").empty(), "no groups");
}

void Search()
{
    auto rows = Sample();
    auto texts = Texts(rows);
    Options o;
    o.words = SearchWords("  static   LOBBY ");
    Check(o.words.size() == 2 && o.words[0] == "static" && o.words[1] == "lobby", "words split and lower case");
    auto view = Build(rows, texts, o);
    Check(view.matching == 1 && view.entries.size() == 1 && rows[view.entries[0].row].name == "StaticMeshActor10", "every word must match (several words narrow it)");
    o.words = SearchWords("alarm1");
    view = Build(rows, texts, o);
    Check(view.matching == 1 && rows[view.entries[0].row].name == "SAlarm0", "Tag column searched");
    o.words = SearchWords("dooropen");
    Check(Build(rows, texts, o).matching == 1, "Event column searched");
    o.words = SearchWords("sbase");
    Check(Build(rows, texts, o).matching == 1, "the full class name is searched");
    o.words = SearchWords("hidden");
    view = Build(rows, texts, o);
    Check(view.matching == 1 && rows[view.entries[0].row].name == "Light5", "Hidden is searchable as a word");
    o.words = SearchWords("locked");
    Check(Build(rows, texts, o).matching == 1, "Locked too");
    o.words = SearchWords("-21");
    Check(Build(rows, texts, o).matching == 1, "location searched");
    o.words = SearchWords("lightlobby");
    Check(Build(rows, texts, o).matching == 0, "a word never spans two columns");
    o.words = SearchWords("none");
    Check(Build(rows, texts, o).matching == 0, "None is not a value to find");
    o.words = {};
    Check(Build(rows, {}, o).matching == rows.size(), "no words: everything, and texts are made when missing");
    o.words = SearchWords("lobby");
    Check(Build(rows, {}, o).matching == 2, "missing texts are made on the fly");
}

void Sorting()
{
    auto rows = Sample();
    Options o;
    auto view = Build(rows, Texts(rows), o);
    auto names = Names(rows, view);
    Check(names == std::vector<std::string>{"Brush3", "LevelInfo0", "Light5", "SAlarm0", "StaticMeshActor2", "StaticMeshActor10"}, "by name, digits by value");
    o.descending = true;
    names = Names(rows, Build(rows, Texts(rows), o));
    Check(names.front() == "StaticMeshActor10" && names.back() == "Brush3", "descending");
    o.descending = false;
    o.sortBy = Column::Tag;
    names = Names(rows, Build(rows, Texts(rows), o));
    Check(names[0] == "SAlarm0" && names[1] == "Light5" && names[2] == "Brush3", "by Tag, blanks last, then by name");
    o.sortBy = Column::Hidden;
    Check(Names(rows, Build(rows, Texts(rows), o))[0] == "Light5", "hidden first");
    o.sortBy = Column::Location;
    Check(Names(rows, Build(rows, Texts(rows), o)).back() == "StaticMeshActor10", "by location, x first");
    Check(NaturalCompare("a007", "a7") == 0 && NaturalCompare("A2", "a10") < 0 && NaturalCompare("ab", "a") > 0 && NaturalCompare("x", "x") == 0,
          "natural order ignores case and leading zeros");
}

void Grouping()
{
    auto rows = Sample();
    auto texts = Texts(rows);
    Options o;
    o.groupBy = GroupBy::Class;
    auto view = Build(rows, texts, o);
    auto names = Names(rows, view);
    Check(view.groups.size() == 5, "one group per class");
    Check(names == std::vector<std::string>{"#Brush", "Brush3", "#LevelInfo", "LevelInfo0", "#Light", "Light5", "#SAlarm", "SAlarm0",
                                            "#StaticMeshActor", "StaticMeshActor2", "StaticMeshActor10"},
          "classes in name order, rows sorted under them");
    Check(HeaderText(view.groups[4]) == "[-] StaticMeshActor  (2)", "header text with count");

    o.groupBy = GroupBy::Group;
    view = Build(rows, texts, o);
    names = Names(rows, view);
    Check(names == std::vector<std::string>{"#Lobby", "Light5", "StaticMeshActor10", "#props", "StaticMeshActor2", "StaticMeshActor10",
                                            "#(no group)", "Brush3", "LevelInfo0", "SAlarm0"},
          "an actor in two groups is in both; groups match whatever their case (named as first seen); ungrouped last");
    Check(view.matching == rows.size(), "matching counts each actor once");

    // Collapsing keeps the header and its count.
    o.collapsed = Toggle(view, 0, {});
    Check(o.collapsed.count("lobby") == 1, "toggle collapses");
    view = Build(rows, texts, o);
    Check(Names(rows, view)[0] == "#Lobby" && Names(rows, view)[1] == "#props" && view.groups[0].collapsed && HeaderText(view.groups[0]) == "[+] Lobby  (2)",
          "a collapsed group lists no rows");
    o.collapsed = Toggle(view, 0, o.collapsed);
    Check(o.collapsed.empty(), "toggle again opens it");
    view = Build(rows, texts, o);
    Check(Toggle(view, 1, {"x"}) == std::set<std::string>{"x"}, "toggling an actor line changes nothing");
    Check(Toggle(view, 99, {}).empty(), "toggling past the end changes nothing");

    // A search leaves out groups with nothing matching.
    o.words = SearchWords("props");
    view = Build(rows, texts, o);
    Check(view.groups.size() == 2 && view.groups[0].label == "Lobby" && view.groups[0].rows.size() == 1, "only groups with matches; counts are matches");
}

void Choosing()
{
    auto rows = Sample();
    Options o;
    o.groupBy = GroupBy::Group;
    o.collapsed = {"props"};
    auto view = Build(rows, Texts(rows), o);
    // Entries: #Lobby, Light5, SMA10, #Props(collapsed), #(no group), Brush3, LevelInfo0, SAlarm0
    auto chosen = ChosenRows(view, {3, 2, 1, -4, 100});
    Check(chosen == std::vector<int>{2, 1, 3}, "a header stands for its rows even collapsed; each row once; bad entries ignored");
    auto choice = Selectable(rows, chosen);
    Check(choice.rows == std::vector<int>{2, 1} && choice.hidden == 1, "visible rows selectable, the hidden Light5 counted");
    choice = Selectable(rows, {3, 1, 50});
    Check(choice.rows == std::vector<int>{1} && choice.hidden == 1, "hidden rows are never selected");

    rows[1].selected = true;
    rows[4].selected = true;
    auto entries = SelectedEntries(view, rows);
    Check(entries == std::vector<int>{2, 7}, "follow selection marks actor lines only");

    auto plan = PlanDelete(rows, {0, 1, 3});
    Check(plan.rows == std::vector<int>{1, 3} && plan.kept == 1, "the level info stays; hidden rows go");
    auto question = DeleteQuestion(rows, plan);
    Check(question.find("Delete these 2 actors?") == 0 && question.find("StaticMeshActor10") != std::string::npos
              && question.find("level info") != std::string::npos && question.find("Undo") != std::string::npos,
          "the question names them and says what stays");
    plan = PlanDelete(rows, {4});
    Check(DeleteQuestion(rows, plan).find("Delete SAlarm0?") == 0 && DeleteQuestion(rows, plan).find("level info") == std::string::npos, "one actor by name");
    std::vector<Row> many(12, Make("Mesh", "Engine.StaticMeshActor"));
    std::vector<int> all;
    for (int i = 0; i < 12; ++i) all.push_back(i);
    Check(DeleteQuestion(many, PlanDelete(many, all)).find("... and 4 more") != std::string::npos, "a long list is cut short");
}

void Footer()
{
    auto rows = Sample();
    Options o;
    o.groupBy = GroupBy::Class;
    o.words = SearchWords("static");
    auto view = Build(rows, Texts(rows), o);
    Check(Summary(rows, view, 3) == "6 actors, 2 match the search, in 1 group(s); 3 selected, 1 hidden.", "summary");
    Check(Thousands(1234567) == "1,234,567" && Thousands(0) == "0" && Thousands(999) == "999", "thousands");
}

void Large()
{
    // A big map: the build stays a sort, not a scan per row.
    std::vector<Row> rows;
    for (int i = 0; i < 20000; ++i)
        rows.push_back(Make(("StaticMeshActor" + std::to_string(i)).c_str(), i % 3 ? "Engine.StaticMeshActor" : "Engine.Light", i % 7 ? "None" : "Hall"));
    auto texts = Texts(rows);
    Options o;
    o.groupBy = GroupBy::Class;
    auto view = Build(rows, texts, o);
    Check(view.entries.size() == rows.size() + 2 && view.groups.size() == 2, "every row once under two classes");
    Check(rows[view.entries[1].row].name == "StaticMeshActor0" && rows[view.entries[2].row].name == "StaticMeshActor3", "natural order in a group");
    o.words = SearchWords("hall 1999");
    view = Build(rows, texts, o);
    for (const auto& e : view.entries)
        if (!e.Header()) Check(rows[e.row].name.find("1999") != std::string::npos && rows[e.row].group == "Hall", "only rows with both words");
}

int main()
{
    try
    {
        Cells();
        Groups();
        Search();
        Sorting();
        Grouping();
        Choosing();
        Footer();
        Large();
    }
    catch (const std::exception& e)
    {
        std::cerr << "MapOutlinerModelTests FAILED: " << e.what() << "\n";
        return 1;
    }
    std::cout << "MapOutlinerModelTests passed " << checks << " checks\n";
    return 0;
}
