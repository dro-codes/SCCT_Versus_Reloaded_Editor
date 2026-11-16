#include "../Reloaded.Editor/CommandPaletteModel.h"
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
using namespace CommandPalette::Model;
int checks = 0;
void Check(bool value, const char* message, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) throw std::runtime_error(std::string(message) + " (line " + std::to_string(where.line()) + ")");
}

// A menu bar like the editor's, in menu order.
std::vector<Entry> Menus()
{
    return {
        { "File > New", "", 40004 },
        { "File > Open...", "Ctrl+O", 40005 },
        { "File > Save", "Ctrl+L", 40007 },
        { "File > Save As...", "", 40008 },
        { "File > Open Recent > 1  BankD.sdc", "MapsEd", 40970 },
        { "Edit > Undo", "Ctrl+Z", 40019 },
        { "Edit > Undo History...", "", 40990 },
        { "View > Show Actor Class Browser", "", 19004 },
        { "Build > Play Level", "Ctrl+P", 40015 },
        { "Build > Map Check...", "", 41270 },
        { "Build > Render Budget...", "", 41280 },
        { "Build > Rebuild All Maps...", "", 40902 },
        { "RE+ Tools > Map Design...", "", 40948 },
        { "RE+ Tools > SMagicEvent Workbench...", "", 40927 },
        { "RE+ Tools > SCamNetwork Manager...", "", 40932 },
        { "RE+ Tools > Camera Bookmarks > Go to Bookmark 1", "1", 41440 },
        { "RE+ Tools > Save Selection as Assembly...", "", 40923 },
        { "Help > RE+ Shortcuts...", "", 40980 },
    };
}
std::string First(const std::vector<Entry>& entries, const std::string& query, const std::vector<std::string>& recent = {})
{
    const auto order = Rank(entries, query, recent);
    return order.empty() ? std::string() : entries[order[0]].path;
}
bool Lists(const std::vector<Entry>& entries, const std::string& query, const std::string& path)
{
    for (auto i : Rank(entries, query, {}))
        if (entries[i].path == path) return true;
    return false;
}

void MenuText()
{
    auto t = SplitMenuText("&Map Check...");
    Check(t.label == "Map Check..." && t.shortcut.empty(), "the access key is dropped");
    t = SplitMenuText("RE+ Options\tF12");
    Check(t.label == "RE+ Options" && t.shortcut == "F12", "the shortcut after the tab");
    t = SplitMenuText("&Save\tCtrl+L");
    Check(t.label == "Save" && t.shortcut == "Ctrl+L", "both");
    t = SplitMenuText("Lights && &Shadows");
    Check(t.label == "Lights & Shadows", "&& is an ampersand");
    t = SplitMenuText("&1  BankD.sdc\tMapsEd");
    Check(t.label == "1  BankD.sdc" && t.shortcut == "MapsEd", "a recent map entry");
    t = SplitMenuText("  Padded &label \t  Ctrl+Q ");
    Check(t.label == "Padded label" && t.shortcut == "Ctrl+Q", "trimmed");
    t = SplitMenuText("Trailing &");
    Check(t.label == "Trailing", "a lone trailing & is dropped");
    Check(JoinPath("", "File") == "File" && JoinPath("File", "Save") == "File > Save", "paths");
    Check(JoinPath(JoinPath("RE+ Tools", "Camera Bookmarks"), "Go to Bookmark 1") == "RE+ Tools > Camera Bookmarks > Go to Bookmark 1", "nested");
}

void WordMatching()
{
    Check(WordScore("mc", "Build > Map Check...") > 0, "initials");
    Check(WordScore("rndr", "Build > Render Budget...") > 0, "scattered letters");
    Check(WordScore("xyz", "Build > Render Budget...") < 0, "letters not there");
    Check(WordScore("kcehc", "Build > Map Check...") < 0, "letters out of order");
    Check(WordScore("CHECK", "Build > Map Check...") > 0, "case is ignored");
    Check(WordScore("", "anything") == 0, "an empty word matches");
    Check(WordScore("toolong", "short") < 0, "longer than the text");
    // A word start beats the same letters in the middle of a word.
    Check(WordScore("ch", "Build > Map Check...") > WordScore("ch", "Build > Match..."), "word start");
    // Consecutive beats scattered.
    Check(WordScore("map", "Build > Map Check...") > WordScore("map", "Build > Make All Paths"), "consecutive letters");
    // The command's own label beats the menus above it.
    Check(WordScore("build", "Build > Play Level") < WordScore("build", "Lighting > Build Lighting"), "the label counts more");
    // CamelCase boundaries are word starts.
    Check(WordStart("SCamNetwork", 4) && WordStart("SCamNetwork", 0) && !WordStart("SCamNetwork", 1), "camel case");
    Check(WordStart("RE+ Tools", 4) && !WordStart("RE+ Tools", 2), "after punctuation, not at it");
}

void Tiers()
{
    Check(MatchPath("map ch", "Build > Map Check...").tier == 2, "word starts");
    Check(MatchPath("eck", "Build > Map Check...").tier == 1, "inside a word");
    Check(MatchPath("rndr", "Build > Render Budget...").tier == 0, "scattered");
    Check(!MatchPath("map zzz", "Build > Map Check...").matched, "every word must match");
    Check(MatchPath("", "Build > Map Check...").matched, "nothing typed matches");
    Check(MatchPath("build > map", "Build > Map Check...").tier == 2, "the > of a typed path is a separator");
    // The best placement is found, not just the first: "check" is a word start later on.
    Check(MatchPath("check", "Build > Cache > Map Check...").tier == 2, "a later word start");
}

void Ranking()
{
    const auto menus = Menus();
    Check(First(menus, "map check") == "Build > Map Check...", "map check");
    Check(First(menus, "mapcheck") == "Build > Map Check...", "run together");
    Check(First(menus, "render") == "Build > Render Budget...", "render");
    Check(First(menus, "rndr bdgt") == "Build > Render Budget...", "scattered letters");
    Check(First(menus, "save") == "File > Save", "the shortest of the saves first");
    Check(First(menus, "save as") == "File > Save As...", "save as");
    Check(First(menus, "magic") == "RE+ Tools > SMagicEvent Workbench...", "inside a camel-case name");
    Check(First(menus, "cam net") == "RE+ Tools > SCamNetwork Manager...", "camel case words");
    Check(First(menus, "undo hist") == "Edit > Undo History...", "undo history");
    Check(First(menus, "bookmark 1") == "RE+ Tools > Camera Bookmarks > Go to Bookmark 1", "a submenu's command");
    Check(First(menus, "BankD") == "File > Open Recent > 1  BankD.sdc", "a recent map");
    Check(First(menus, "qqq").empty() && Rank(menus, "qqq", {}).empty(), "nothing matches");
    Check(Lists(menus, "build", "Build > Play Level") && Lists(menus, "build", "Build > Rebuild All Maps..."), "a menu name lists its commands");
    Check(!Lists(menus, "help shortcuts", "Build > Map Check..."), "only matches are listed");
    // A word start in the label beats letters scattered through the path.
    Check(First(menus, "play") == "Build > Play Level", "play");
    // Nothing typed: every command, in menu order.
    const auto all = Rank(menus, "  ", {});
    Check(all.size() == menus.size(), "everything listed");
    for (size_t i = 0; i < all.size(); ++i) Check(all[i] == i, "menu order");
}

void Recent()
{
    const auto menus = Menus();
    std::vector<std::string> recent;
    recent = NoteRun(recent, "Build > Map Check...");
    recent = NoteRun(recent, "Help > RE+ Shortcuts...");
    Check(recent.size() == 2 && recent[0] == "Help > RE+ Shortcuts...", "the last run first");
    recent = NoteRun(recent, "Build > Map Check...");
    Check(recent.size() == 2 && recent[0] == "Build > Map Check..." && recent[1] == "Help > RE+ Shortcuts...", "run again moves to the front, once");
    Check(NoteRun(recent, "").size() == 2, "an empty path is not noted");
    // Nothing typed: the recent ones first, most recent first, then the rest in menu order.
    const auto order = Rank(menus, "", recent);
    Check(menus[order[0]].path == "Build > Map Check..." && menus[order[1]].path == "Help > RE+ Shortcuts...", "recent first");
    Check(menus[order[2]].path == "File > New" && order.size() == menus.size(), "then the rest");
    // Typed: among equally good matches, the recent one first.
    Check(First(menus, "save") == "File > Save", "before running");
    recent = NoteRun(recent, "RE+ Tools > Save Selection as Assembly...");
    Check(First(menus, "save", recent) == "RE+ Tools > Save Selection as Assembly...", "recent first among word-start matches");
    // But a recent command matched only by scattered letters does not beat a real match.
    const std::vector<Entry> two{ { "Build > Render Budget...", "", 1 }, { "Edit > Big Untidy Dialog Get Text", "", 2 } };
    Check(MatchPath("budget", two[1].path).tier == 0, "only scattered letters");
    Check(First(two, "budget", { two[1].path }) == "Build > Render Budget...", "tiers before recency");
    recent = NoteRun({}, "Build > Render Budget...");
    Check(First(menus, "map ch", recent) == "Build > Map Check...", "a recent command that does not match is not listed first");
    Check(RecentRank(recent, "Build > Render Budget...") == 0 && RecentRank(recent, "nope") == -1, "rank");
    // At most kRecentMax kept.
    std::vector<std::string> many;
    for (int i = 0; i < 30; ++i) many = NoteRun(many, "Command " + std::to_string(i));
    Check(many.size() == kRecentMax && many[0] == "Command 29", "the list is capped");
}

void Rows()
{
    Entry entry{ "File > Save", "Ctrl+L", 40007 };
    Check(RowText(entry) == "File > Save\tCtrl+L", "with a shortcut");
    entry.shortcut.clear();
    Check(RowText(entry) == "File > Save", "without");
    Check(LabelStart("File > Save") == 7 && LabelStart("Save") == 0, "the label's start");
}

int main()
{
    try
    {
        MenuText();
        WordMatching();
        Tiers();
        Ranking();
        Recent();
        Rows();
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
    std::cout << "CommandPaletteModelTests: " << checks << " checks passed\n";
    return 0;
}
