#pragma once
#include <cstdint>
#include <optional>
#include <string>

// Whether the open map has changes its file does not, for the warning before
// Open Recent, Find Usages in All Maps or Open Map Backup puts another map in
// its place. The editor counts every edit (Workflow::Editor::ContentRevision)
// and every map it opens (MapGeneration). The open map is as its file has it
// at two moments only: just after a MAP LOAD (or MAP NEW, which has nothing to
// lose) opened it, and just after a save wrote the open map's own file. A
// Save As that was cancelled, a save the engine could not write, an autosave
// and a copy written anywhere else change nothing.
namespace UnsavedChanges
{
    // The last map the editor opened: the generation and content revision
    // its opening left, and whether it was imported from text, which no
    // file holds.
    struct Opened
    {
        unsigned generation = 0;
        unsigned revision = 0;
        bool imported = false;
    };

    struct Tracker
    {
        // The opening the clean revision belongs to.
        unsigned generation = 0;
        // The revision at which the open map was last as its file has it;
        // none for an imported map not saved since.
        std::optional<unsigned> clean = 0u;
        // A save of a file the open map was not named after yet: Save As
        // writes the file before the map takes the new name.
        struct Pending { std::string file; unsigned revision = 0; };
        std::optional<Pending> pending;
    };

    inline char FoldPathChar(char c)
    {
        if (c == '/') return '\\';
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    // The same file as Windows names files: in any case, with either slash.
    // Both paths are full ones; an empty one is no file.
    inline bool SameFile(const std::string& a, const std::string& b)
    {
        if (a.empty() || a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (FoldPathChar(a[i]) != FoldPathChar(b[i])) return false;
        return true;
    }

    // Whether a save wrote its file, from the file's last-write time before
    // and after it (none while there is no file). The engine reports a save
    // it gave up on (a read-only map, "try again?" answered No) as done.
    inline bool Written(std::optional<std::uint64_t> before, std::optional<std::uint64_t> after)
    {
        return after && (!before || *after != *before);
    }

    // A map opened since the tracker last looked starts clean at the
    // revision its opening left, however long it took anyone to notice.
    inline void Follow(Tracker& tracker, const Opened& opened)
    {
        if (opened.generation == tracker.generation) return;
        tracker.generation = opened.generation;
        tracker.clean = opened.imported ? std::nullopt : std::optional<unsigned>(opened.revision);
        tracker.pending.reset();
    }

    // What the editor does to finish opening a map it loaded (its settings,
    // its Save target) is part of the opening: the map is still as its file
    // has it.
    inline void Settled(Tracker& tracker, const Opened& opened, unsigned revision)
    {
        Follow(tracker, opened);
        if (!opened.imported) tracker.clean = revision;
        tracker.pending.reset();
    }

    // A save waiting for a name counts once the open map is named after the
    // file it wrote: the map was as that file has it at the save's revision.
    inline void Named(Tracker& tracker, const std::string& mapFile)
    {
        if (tracker.pending && SameFile(tracker.pending->file, mapFile))
        {
            tracker.clean = tracker.pending->revision;
            tracker.pending.reset();
        }
    }

    // A save wrote `saved` while the open map's own file (the one File > Save
    // writes) was `mapFile` and its content revision `revision`. True when
    // that made the open map clean; a save of another file waits to see
    // whether the map takes that name. A Save As named the map after its file
    // once it was done, so a save since then (Play Level's runtime copy, a
    // release copy) first settles the one that was waiting.
    inline bool Saved(Tracker& tracker, const Opened& opened, unsigned revision, const std::string& saved, const std::string& mapFile)
    {
        Follow(tracker, opened);
        Named(tracker, mapFile);
        if (!SameFile(saved, mapFile))
        {
            if (!saved.empty()) tracker.pending = Tracker::Pending{saved, revision};
            return false;
        }
        tracker.clean = revision;
        tracker.pending.reset();
        return true;
    }

    inline bool Unsaved(Tracker& tracker, const Opened& opened, unsigned revision, const std::string& mapFile)
    {
        Follow(tracker, opened);
        Named(tracker, mapFile);
        return !tracker.clean || *tracker.clean != revision;
    }
}
