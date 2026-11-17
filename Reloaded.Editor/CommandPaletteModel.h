#pragma once
// Help > Command Palette (Ctrl+Shift+P): a search box over every command of
// the frame's menu bar. This header is the pure part: turning a menu item's
// text into a label and its shortcut, the fuzzy match of what is typed against
// "Menu > Submenu > Command", the order the matches are listed in, and the list
// of recently run commands. No Windows, no engine.
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace CommandPalette::Model
{
    // How many recently run commands are remembered.
    constexpr size_t kRecentMax = 20;
    constexpr const char* kSeparator = " > ";

    // One command found in the menus.
    struct Entry
    {
        std::string path;     // "Build > Map Check..."
        std::string shortcut; // "Ctrl+L", or empty
        unsigned id = 0;      // the WM_COMMAND id
        bool enabled = true;
        bool checked = false;
    };

    // A menu item's text as the menu shows it: "&Map Check...\tCtrl+M" is the
    // label "Map Check..." and the shortcut "Ctrl+M". A single & marks the
    // access key and is dropped; && is a real ampersand.
    struct Text
    {
        std::string label, shortcut;
    };
    inline std::string Trim(const std::string& text)
    {
        size_t from = 0, to = text.size();
        while (from < to && std::isspace(static_cast<unsigned char>(text[from]))) ++from;
        while (to > from && std::isspace(static_cast<unsigned char>(text[to - 1]))) --to;
        return text.substr(from, to - from);
    }
    inline Text SplitMenuText(const std::string& raw)
    {
        Text result;
        const size_t tab = raw.find('\t');
        const std::string label = raw.substr(0, tab);
        for (size_t i = 0; i < label.size(); ++i)
        {
            if (label[i] == '&')
            {
                if (i + 1 < label.size() && label[i + 1] == '&') { result.label += '&'; ++i; }
                continue;
            }
            result.label += label[i];
        }
        result.label = Trim(result.label);
        if (tab != std::string::npos) result.shortcut = Trim(raw.substr(tab + 1));
        return result;
    }
    inline std::string JoinPath(const std::string& parent, const std::string& label)
    {
        return parent.empty() ? label : parent + kSeparator + label;
    }

    inline char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    inline bool IsWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
    // A word starts at the beginning, after anything that is not a letter or
    // digit, and at a capital after a small letter (SMagicEvent, CamNetwork).
    inline bool WordStart(const std::string& text, size_t i)
    {
        if (i == 0) return true;
        const char previous = text[i - 1], c = text[i];
        if (!IsWordChar(previous)) return IsWordChar(c);
        return std::isupper(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(previous));
    }
    // Where the last segment ("Map Check..." in "Build > Map Check...") starts.
    inline size_t LabelStart(const std::string& path)
    {
        const size_t at = path.rfind(kSeparator);
        return at == std::string::npos ? 0 : at + std::char_traits<char>::length(kSeparator);
    }
    inline std::vector<std::string> Words(const std::string& query)
    {
        std::vector<std::string> words;
        std::string word;
        for (char c : query)
        {
            if (std::isspace(static_cast<unsigned char>(c)) || c == '>')
            {
                if (!word.empty()) words.push_back(word);
                word.clear();
            }
            else word += Lower(c);
        }
        if (!word.empty()) words.push_back(word);
        return words;
    }

    // How well one typed word matches the text as a subsequence, ignoring
    // case: every letter in order, the best of all the ways to place them.
    // Each matched letter scores; more for a letter that starts a word, for
    // one right after the previous match, and for one in the command's own
    // label rather than in the menus above it. -1 when the letters are not
    // all there in order.
    inline int WordScore(const std::string& typed, const std::string& text)
    {
        std::string word(typed);
        for (auto& c : word) c = Lower(c);
        const size_t n = word.size(), m = text.size();
        if (!n) return 0;
        if (n > m) return -1;
        const size_t label = LabelStart(text);
        constexpr int kNone = -1000000;
        // best[i][j]: best score with word[0..i] placed and word[i] on text[j].
        std::vector<std::vector<int>> best(n, std::vector<int>(m, kNone));
        auto letter = [&](size_t j) {
            int score = 1;
            if (WordStart(text, j)) score += 8;
            if (j >= label) score += 2;
            return score;
        };
        for (size_t j = 0; j < m; ++j)
            if (Lower(text[j]) == word[0]) best[0][j] = letter(j) + (j == label ? 4 : 0);
        for (size_t i = 1; i < n; ++i)
        {
            int runningBest = kNone; // best of best[i-1][0..j-2]
            for (size_t j = i; j < m; ++j)
            {
                if (j >= 2) runningBest = std::max(runningBest, best[i - 1][j - 2]);
                if (Lower(text[j]) != word[i]) continue;
                int from = kNone;
                if (best[i - 1][j - 1] != kNone) from = best[i - 1][j - 1] + 9; // consecutive: more than a word start
                if (runningBest != kNone) from = std::max(from, runningBest);
                if (from != kNone) best[i][j] = from + letter(j);
            }
        }
        int result = kNone;
        for (size_t j = 0; j < m; ++j) result = std::max(result, best[n - 1][j]);
        return result == kNone ? -1 : result;
    }

    // How a command matches the typed words, all of which must be in it.
    // tier 2: every word is the start of a word of the path ("map ch" for
    // Map Check); tier 1: every word is somewhere in it as it stands; tier 0:
    // only as scattered letters ("rndr" for Render). score orders within a tier.
    struct Match
    {
        bool matched = false;
        int tier = 0;
        int score = 0;
    };
    inline std::string Folded(const std::string& text)
    {
        std::string folded(text);
        for (auto& c : folded) c = Lower(c);
        return folded;
    }
    inline Match MatchPath(const std::string& query, const std::string& path)
    {
        Match match;
        const auto words = Words(query);
        const auto folded = Folded(path);
        match.tier = 2;
        for (const auto& word : words)
        {
            const int score = WordScore(word, path);
            if (score < 0) return {};
            match.score += score;
            int tier = 0;
            for (size_t at = folded.find(word); at != std::string::npos; at = folded.find(word, at + 1))
            {
                tier = std::max(tier, 1);
                if (WordStart(path, at)) { tier = 2; break; }
            }
            match.tier = std::min(match.tier, tier);
        }
        match.matched = true;
        // Shorter paths first among equals: "Save" before "Save Selection as Assembly...".
        match.score = match.score * 4 - static_cast<int>(path.size() / 8);
        return match;
    }

    // Where a path is in the recent list (0 = run last), or -1.
    inline int RecentRank(const std::vector<std::string>& recent, const std::string& path)
    {
        for (size_t i = 0; i < recent.size(); ++i)
            if (recent[i] == path) return static_cast<int>(i);
        return -1;
    }

    // The entries to list for what is typed, as indexes into `entries`, best
    // first. Nothing typed: the recently run commands (most recent first),
    // then every other one in menu order. Something typed: only the matches,
    // the better tier first; within a tier the recently run ones first, then
    // by score, then in menu order.
    inline std::vector<size_t> Rank(const std::vector<Entry>& entries, const std::string& query, const std::vector<std::string>& recent)
    {
        struct Row
        {
            size_t index;
            int tier, recency, score;
        };
        std::vector<Row> rows;
        const bool typed = !Words(query).empty();
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const int rank = RecentRank(recent, entries[i].path);
            const int recency = rank < 0 ? 1000000 : rank;
            if (!typed) { rows.push_back({ i, 0, recency, 0 }); continue; }
            const auto match = MatchPath(query, entries[i].path);
            if (match.matched) rows.push_back({ i, match.tier, recency, match.score });
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.tier != b.tier) return a.tier > b.tier;
            if (a.recency != b.recency) return a.recency < b.recency;
            return a.score > b.score;
        });
        std::vector<size_t> order;
        order.reserve(rows.size());
        for (const auto& row : rows) order.push_back(row.index);
        return order;
    }

    // The recent list after running `path`: it moves to the front.
    inline std::vector<std::string> NoteRun(std::vector<std::string> recent, const std::string& path)
    {
        if (path.empty()) return recent;
        recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
        recent.insert(recent.begin(), path);
        if (recent.size() > kRecentMax) recent.resize(kRecentMax);
        return recent;
    }

    // The list row's text: the path, then the shortcut after a tab.
    inline std::string RowText(const Entry& entry)
    {
        return entry.shortcut.empty() ? entry.path : entry.path + "\t" + entry.shortcut;
    }
}
