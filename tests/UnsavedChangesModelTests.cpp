#include "../Reloaded.Editor/UnsavedChangesModel.h"
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
using namespace UnsavedChanges;
int checks = 0;
void Check(bool value, const char* message, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) throw std::runtime_error(std::string(message) + " (line " + std::to_string(where.line()) + ")");
}

const std::string kMoon = "C:\\Game\\Packages\\MapsEd\\MoonD.sdc";
const std::string kCopy = "C:\\Game\\Packages\\MapsEd\\MoonD_v2.sdc";

void Files()
{
    Check(SameFile(kMoon, "c:/game/packages/mapsed/MOOND.SDC"), "any case, either slash");
    Check(!SameFile(kMoon, "C:\\Game\\Packages\\MapsEd\\MoonE.sdc"), "another map");
    Check(!SameFile(kMoon, "C:\\Game\\Packages\\Maps\\MoonD.sdc"), "the runtime copy is not the map");
    Check(!SameFile(kMoon, kMoon + ".bak") && !SameFile(kMoon + ".bak", kMoon), "a longer name");
    Check(!SameFile("", "") && !SameFile(kMoon, "") && !SameFile("", kMoon), "an untitled map has no file");

    Check(Written(100, 200) && Written(200, 100), "the write time moved, either way");
    Check(Written(std::nullopt, 100), "a new file");
    Check(!Written(100, 100), "the engine gave up: the file is as it was");
    Check(!Written(std::nullopt, std::nullopt) && !Written(100, std::nullopt), "no file after the save");
}

void Opening()
{
    Tracker tracker;
    Check(!Unsaved(tracker, {}, 0, ""), "the editor starts clean");
    Check(Unsaved(tracker, {}, 3, ""), "edits to the start-up map");

    // A map opened by stock File > Open is clean at the revision its load
    // left, even when the edits after it happen before anyone looks.
    const Opened moon{1, 4};
    Check(Unsaved(tracker, moon, 6, kMoon), "two edits straight after the load");
    Check(!Unsaved(tracker, moon, 4, kMoon), "the load's own revision is clean");
    Tracker late;
    Check(Unsaved(late, moon, 6, kMoon), "noticed late, the edits since the load still count");

    // A load whose generation the tracker has already seen does not reset it.
    Tracker same;
    Check(Unsaved(same, moon, 9, kMoon), "edited");
    Check(Unsaved(same, moon, 9, kMoon), "and still edited when asked again");

    // Each opening starts over, edits to the previous map dropped with it.
    Check(!Unsaved(same, {2, 12}, 12, kCopy), "the next map opened is clean");
    Check(Unsaved(same, {2, 12}, 13, kCopy), "until it is edited");

    // Open Recent loads the map's settings and sets its Save target after
    // the load: still the map as its file has it.
    Tracker recent;
    Check(Unsaved(recent, {5, 50}, 51, kMoon), "the settings' command counted as a change");
    Settled(recent, {5, 50}, 51);
    Check(!Unsaved(recent, {5, 50}, 51, kMoon) && Unsaved(recent, {5, 50}, 52, kMoon), "settled, then edited");
    Tracker text;
    Settled(text, {6, 60, true}, 60);
    Check(Unsaved(text, {6, 60, true}, 60, ""), "an import is not settled into a file");

    // MAP NEW has nothing to lose; MAP IMPORT builds a map no file holds.
    Tracker fresh;
    Check(!Unsaved(fresh, {3, 20}, 20, ""), "a new, empty map");
    Tracker imported;
    Check(Unsaved(imported, {3, 20, true}, 20, ""), "an imported map is unsaved before anything is done to it");
    Check(Unsaved(imported, {3, 20, true}, 25, ""), "and after");
}

void Saving()
{
    const Opened moon{1, 4};
    Tracker tracker;
    Check(Unsaved(tracker, moon, 7, kMoon), "edited");

    // File > Save, Ctrl+L, the engine's own request to save: the open map's file.
    Check(Saved(tracker, moon, 7, "c:/game/packages/mapsed/moond.sdc", kMoon), "a save of the open map's file");
    Check(!Unsaved(tracker, moon, 7, kMoon), "is clean");
    Check(Unsaved(tracker, moon, 8, kMoon), "until the next edit");

    // Saves that do not save the open map leave it as it was.
    Check(!Saved(tracker, moon, 8, "C:\\Game\\Packages\\Maps\\Autoplay.sdc", kMoon), "Play Level's runtime copy");
    Check(!Saved(tracker, moon, 8, "D:\\Release\\MoonD\\Packages\\Maps\\MoonD.sdc", kMoon), "a release copy");
    Check(Unsaved(tracker, moon, 8, kMoon), "so the edit is still unsaved");

    // Save As writes the new file first and names the map after it once the
    // save is done: clean from then on, under that name.
    Tracker saveAs;
    Check(!Saved(saveAs, moon, 8, kCopy, kMoon), "Save As writes before the map has the new name");
    Check(Unsaved(saveAs, moon, 8, kMoon), "still unsaved while the map keeps its old name");
    Check(!Unsaved(saveAs, moon, 8, kCopy), "clean once it is named after the file written");
    Check(Unsaved(saveAs, moon, 9, kCopy), "and unsaved at the next edit");
    Tracker saveAsEdited;
    Saved(saveAsEdited, moon, 8, kCopy, kMoon);
    Check(Unsaved(saveAsEdited, moon, 9, kCopy), "an edit after the Save As counts");
    Check(Saved(saveAsEdited, moon, 9, kCopy, kCopy) && !Unsaved(saveAsEdited, moon, 9, kCopy), "and File > Save under the new name saves it");
    Tracker other;
    Saved(other, moon, 8, kCopy, kMoon);
    Check(Unsaved(other, {2, 8}, 9, kCopy), "the next map opened drops what was waiting");
    // Play Level (or a release copy) saves another file after a Save As, before
    // anything asked whether the map is unsaved: the Save As still counts.
    Tracker played;
    Saved(played, moon, 8, kCopy, kMoon);
    Check(!Saved(played, moon, 8, "C:\\Game\\Packages\\Maps\\Autoplay.sdc", kCopy), "Play Level after a Save As");
    Check(!Unsaved(played, moon, 8, kCopy), "the Save As still made the map clean");
    Tracker playedEdited;
    Saved(playedEdited, moon, 8, kCopy, kMoon);
    Saved(playedEdited, moon, 9, "C:\\Game\\Packages\\Maps\\Autoplay.sdc", kCopy);
    Check(Unsaved(playedEdited, moon, 9, kCopy), "an edit between the Save As and Play Level still counts");
    Tracker twice;
    Saved(twice, moon, 8, kCopy, kMoon);
    Saved(twice, moon, 9, "C:\\Game\\Packages\\MapsEd\\MoonD_v3.sdc", kCopy);
    Check(!Unsaved(twice, moon, 9, "C:\\Game\\Packages\\MapsEd\\MoonD_v3.sdc"), "a second Save As straight after the first");
    Tracker latest;
    Saved(latest, moon, 8, kCopy, kMoon);
    Saved(latest, moon, 10, "D:\\Release\\MoonD\\Packages\\Maps\\MoonD.sdc", kMoon);
    Check(Unsaved(latest, moon, 10, kCopy), "only the last save waits for a name");

    // A cancelled Save As or a save the engine could not write is never
    // reported (Written): nothing is recorded, and the edit stays unsaved.
    Check(Unsaved(tracker, moon, 9, kMoon), "an edit, then a cancelled Save As");

    // An untitled map saved nowhere.
    Tracker untitled;
    Check(!Saved(untitled, {2, 30}, 31, "", ""), "no file written, no file of its own");
    Check(Unsaved(untitled, {2, 30}, 31, ""), "leaves the untitled map's edit unsaved");
    Check(!untitled.pending, "and nothing waiting for a name");

    // An imported map is clean once saved, and stays clean only until edited.
    Tracker imported;
    const Opened text{3, 40, true};
    Check(Saved(imported, text, 40, kCopy, kCopy), "an import saved");
    Check(!Unsaved(imported, text, 40, kCopy) && Unsaved(imported, text, 41, kCopy), "clean, then edited");

    // A save recorded for one opening does not carry over to the next.
    Tracker next;
    Check(Saved(next, moon, 10, kMoon, kMoon), "saved");
    Check(!Unsaved(next, {4, 10}, 10, kMoon) && next.generation == 4, "the next map opened at the same revision counts from its own load");
}

int main()
{
    try
    {
        Files();
        Opening();
        Saving();
        std::cout << "UnsavedChangesModelTests: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "UnsavedChangesModelTests failed: " << error.what() << '\n';
        return 1;
    }
}
