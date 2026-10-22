#pragma once
// The pure part of the self-updater: release versions and their order, the
// choice of release from the GitHub releases list, the zip directory of the
// release archive and the checks on the files taken out of it, the What's
// New record and what a start keeps of the version an update replaced. No network,
// no zlib and no Windows, so the tests compile it alone.
#include "Include/nlohmann/json.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Updater
{
    using Json = nlohmann::json;
    using Bytes = std::vector<std::uint8_t>;

    // A release version: up to three numbers and an optional pre-release
    // ("1.3.0-beta.12"), ordered as semantic versions order them.
    struct Version
    {
        std::array<unsigned, 3> core{};
        std::vector<std::string> pre;

        static std::optional<Version> Parse(std::string text)
        {
            if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.erase(0, 1);
            if (const auto plus = text.find('+'); plus != std::string::npos) text.resize(plus);
            std::string preText;
            if (const auto dash = text.find('-'); dash != std::string::npos)
            {
                preText = text.substr(dash + 1);
                text.resize(dash);
                if (preText.empty()) return std::nullopt;
            }
            Version v;
            size_t part = 0, i = 0;
            while (true)
            {
                if (part == 3 || i >= text.size() || !IsDigit(text[i])) return std::nullopt;
                unsigned long long value = 0;
                for (; i < text.size() && IsDigit(text[i]); ++i)
                    if ((value = value * 10 + (text[i] - '0')) > 0xFFFFFFFFull) return std::nullopt;
                v.core[part++] = static_cast<unsigned>(value);
                if (i == text.size()) break;
                if (text[i++] != '.') return std::nullopt;
            }
            for (size_t start = 0; !preText.empty();)
            {
                const auto dot = preText.find('.', start);
                std::string id = preText.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
                if (id.empty()) return std::nullopt;
                for (char c : id)
                    if (!IsDigit(c) && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && c != '-') return std::nullopt;
                v.pre.push_back(id);
                if (dot == std::string::npos) break;
                start = dot + 1;
            }
            return v;
        }

        std::string ToString() const
        {
            std::string s = std::to_string(core[0]) + "." + std::to_string(core[1]) + "." + std::to_string(core[2]);
            for (size_t i = 0; i < pre.size(); ++i) s += (i ? "." : "-") + pre[i];
            return s;
        }

        // Negative, zero or positive as a is older than, equal to or newer than b.
        static int Compare(const Version& a, const Version& b)
        {
            for (size_t i = 0; i < 3; ++i)
                if (a.core[i] != b.core[i]) return a.core[i] < b.core[i] ? -1 : 1;
            // A release is newer than any of its pre-releases.
            if (a.pre.empty() || b.pre.empty()) return a.pre.empty() == b.pre.empty() ? 0 : a.pre.empty() ? 1 : -1;
            for (size_t i = 0; i < (std::min)(a.pre.size(), b.pre.size()); ++i)
            {
                const auto& x = a.pre[i];
                const auto& y = b.pre[i];
                const bool xn = Numeric(x), yn = Numeric(y);
                if (xn && yn)
                {
                    // Compare as numbers without overflow: longer is larger once zeros are gone.
                    const auto sx = x.substr((std::min)(x.find_first_not_of('0'), x.size()));
                    const auto sy = y.substr((std::min)(y.find_first_not_of('0'), y.size()));
                    if (sx.size() != sy.size()) return sx.size() < sy.size() ? -1 : 1;
                    if (sx != sy) return sx < sy ? -1 : 1;
                }
                else if (xn != yn) return xn ? -1 : 1;
                else if (x != y) return x < y ? -1 : 1;
            }
            if (a.pre.size() != b.pre.size()) return a.pre.size() < b.pre.size() ? -1 : 1;
            return 0;
        }
        friend bool operator<(const Version& a, const Version& b) { return Compare(a, b) < 0; }
        friend bool operator==(const Version& a, const Version& b) { return Compare(a, b) == 0; }

    private:
        static bool IsDigit(char c) { return c >= '0' && c <= '9'; }
        static bool Numeric(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), IsDigit); }
    };

    struct Release
    {
        Version version;
        std::string tag, name, notes, page;
        std::string assetName, assetUrl, sha256; // sha256: lower-case hex, empty when GitHub gave none
        std::uint64_t assetSize = 0;
        bool prerelease = false;
    };

    inline std::string Lower(std::string s)
    {
        for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }

    // The release archive: one Reloaded_Editor*.zip per release.
    inline bool IsReleaseArchive(const std::string& name)
    {
        const auto n = Lower(name);
        return n.size() > 4 && n.rfind("reloaded_editor", 0) == 0 && n.compare(n.size() - 4, 4, ".zip") == 0;
    }

    // The newest published release in a GitHub /releases list that has an
    // archive to install. Drafts, unparseable tags and releases without
    // exactly one archive are passed over.
    inline std::optional<Release> Newest(const Json& releases, bool includePrereleases)
    {
        if (!releases.is_array()) throw std::runtime_error("GitHub returned an unexpected releases list.");
        std::optional<Release> best;
        for (const auto& r : releases)
        {
            if (!r.is_object() || r.value("draft", false)) continue;
            const bool pre = r.value("prerelease", false);
            if (pre && !includePrereleases) continue;
            const auto tag = r.contains("tag_name") && r["tag_name"].is_string() ? r["tag_name"].get<std::string>() : std::string{};
            const auto version = Version::Parse(tag);
            if (!version || (best && !(best->version < *version))) continue;
            const Json* archive = nullptr;
            int archives = 0;
            if (r.contains("assets") && r["assets"].is_array())
                for (const auto& a : r["assets"])
                    if (a.is_object() && a.contains("name") && a["name"].is_string() && IsReleaseArchive(a["name"].get<std::string>()))
                        ++archives, archive = &a;
            if (archives != 1 || !(*archive).contains("browser_download_url") || !(*archive)["browser_download_url"].is_string()) continue;
            Release out;
            out.version = *version;
            out.tag = tag;
            out.prerelease = pre;
            auto text = [&](const Json& o, const char* key) { return o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string{}; };
            out.name = text(r, "name");
            out.notes = text(r, "body");
            out.page = text(r, "html_url");
            out.assetName = text(*archive, "name");
            out.assetUrl = text(*archive, "browser_download_url");
            if (archive->contains("size") && (*archive)["size"].is_number_integer() && (*archive)["size"].get<std::int64_t>() > 0)
                out.assetSize = (*archive)["size"].get<std::uint64_t>();
            const auto digest = Lower(text(*archive, "digest"));
            if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64
                && digest.find_first_not_of("0123456789abcdef", 7) == std::string::npos)
                out.sha256 = digest.substr(7);
            best = out;
        }
        return best;
    }

    // Release notes as a message box shows them: Markdown headings and
    // emphasis dropped, CRLF line ends, and no more than limit characters.
    inline std::string PlainNotes(const std::string& markdown, size_t limit = 1200)
    {
        std::string out, line;
        auto flush = [&] {
            size_t start = line.find_first_not_of('#');
            if (start != 0 && start != std::string::npos && line[start] == ' ') line.erase(0, start + 1);
            for (size_t p; (p = line.find("**")) != std::string::npos;) line.erase(p, 2);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() && (out.empty() || (out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0))) { line.clear(); return; }
            out += line + "\r\n";
            line.clear();
        };
        for (char c : markdown)
            if (c == '\n') flush(); else line += c;
        flush();
        while (out.size() >= 2 && out.compare(out.size() - 2, 2, "\r\n") == 0) out.resize(out.size() - 2);
        if (out.size() > limit)
        {
            out.resize(limit);
            // Do not cut a UTF-8 sequence in half.
            while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
            if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) out.pop_back();
            out += "...";
        }
        return out;
    }

    // Release notes as blocks a window can lay out: headings, paragraphs,
    // list items and code. GitHub release bodies are Markdown; only what
    // release notes use is understood (headings, - * + and 1. lists, **bold**,
    // `code`, [links](url), ``` fences, HTML comments dropped). Plain text,
    // as What's New records from before Markdown was kept hold, is read one
    // paragraph per line.
    struct NoteSpan
    {
        std::string text;
        bool bold = false, code = false;
        bool operator==(const NoteSpan&) const = default;
    };
    struct NoteBlock
    {
        enum class Kind { Heading, Paragraph, Item, Code } kind = Kind::Paragraph;
        int level = 0;          // heading 1-6; list nesting from 0
        std::string marker;     // a numbered item's "3."; empty for a bullet
        std::vector<NoteSpan> spans;
        bool operator==(const NoteBlock&) const = default;
    };

    inline std::vector<NoteSpan> NoteSpans(const std::string& text)
    {
        std::vector<NoteSpan> spans;
        auto add = [&](std::string s, bool bold, bool code) {
            if (s.empty()) return;
            if (!spans.empty() && spans.back().bold == bold && spans.back().code == code) spans.back().text += s;
            else spans.push_back({std::move(s), bold, code});
        };
        bool bold = false;
        std::string run;
        for (size_t i = 0; i < text.size();)
        {
            if ((text.compare(i, 2, "**") == 0 || text.compare(i, 2, "__") == 0))
            {
                // Only a marker with a partner further on starts bold.
                if (bold || text.find(text.substr(i, 2), i + 2) != std::string::npos)
                {
                    add(run, bold, false), run.clear();
                    bold = !bold;
                    i += 2;
                    continue;
                }
            }
            if (text[i] == '`')
            {
                if (const auto close = text.find('`', i + 1); close != std::string::npos)
                {
                    add(run, bold, false), run.clear();
                    add(text.substr(i + 1, close - i - 1), bold, true);
                    i = close + 1;
                    continue;
                }
            }
            if (text[i] == '[')
            {
                const auto mid = text.find("](", i + 1);
                const auto close = mid == std::string::npos ? mid : text.find(')', mid + 2);
                if (close != std::string::npos && text.find('[', i + 1) > mid)
                {
                    const auto label = text.substr(i + 1, mid - i - 1), url = text.substr(mid + 2, close - mid - 2);
                    run += label == url || label.empty() ? url : label + " (" + url + ")";
                    i = close + 1;
                    continue;
                }
            }
            run += text[i++];
        }
        add(run, bold, false);
        return spans;
    }

    inline std::vector<NoteBlock> ParseNotes(const std::string& source, bool markdown = true)
    {
        using Kind = NoteBlock::Kind;
        std::vector<NoteBlock> blocks;
        std::string paragraph;   // Markdown paragraph lines joined
        std::string code;        // inside a ``` fence
        bool inFence = false, inComment = false, itemOpen = false;
        std::string itemText;
        NoteBlock item;
        auto trimmed = [](const std::string& s) {
            const auto a = s.find_first_not_of(" \t");
            return a == std::string::npos ? std::string{} : s.substr(a, s.find_last_not_of(" \t") - a + 1);
        };
        auto closeItem = [&] {
            if (!itemOpen) return;
            item.spans = NoteSpans(itemText);
            blocks.push_back(item);
            itemOpen = false;
        };
        auto closeParagraph = [&] {
            if (paragraph.empty()) return;
            blocks.push_back({Kind::Paragraph, 0, {}, NoteSpans(paragraph)});
            paragraph.clear();
        };
        auto closeAll = [&] { closeItem(); closeParagraph(); };
        size_t at = 0;
        while (at <= source.size())
        {
            const auto end = source.find('\n', at);
            std::string line = source.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? source.size() + 1 : end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (markdown && trimmed(line).rfind("```", 0) == 0)
            {
                if (inFence)
                {
                    if (!code.empty()) blocks.push_back({Kind::Code, 0, {}, {{code, false, true}}});
                    code.clear();
                }
                else closeAll();
                inFence = !inFence;
                continue;
            }
            if (inFence) { code += (code.empty() ? "" : "\n") + line; continue; }
            // HTML comments, which release templates leave in.
            for (;;)
            {
                if (inComment)
                {
                    const auto close = line.find("-->");
                    if (close == std::string::npos) { line.clear(); break; }
                    line.erase(0, close + 3);
                    inComment = false;
                }
                const auto open = line.find("<!--");
                if (open == std::string::npos) break;
                const auto close = line.find("-->", open + 4);
                if (close == std::string::npos) { line.resize(open); inComment = true; break; }
                line.erase(open, close + 3 - open);
            }
            const auto text = trimmed(line);
            if (text.empty()) { closeAll(); continue; }
            const auto indent = static_cast<int>(line.find_first_not_of(" \t"));
            // # Heading
            const auto hashes = text.find_first_not_of('#');
            if (hashes != 0 && hashes != std::string::npos && hashes <= 6 && text[hashes] == ' ')
            {
                closeAll();
                blocks.push_back({Kind::Heading, static_cast<int>(hashes), {}, NoteSpans(trimmed(text.substr(hashes)))});
                continue;
            }
            // - item, * item, + item, 1. item
            std::string marker;
            size_t body = std::string::npos;
            if (text.size() > 1 && (text[0] == '-' || text[0] == '*' || text[0] == '+') && text[1] == ' ') body = 2;
            else if (const auto digits = text.find_first_not_of("0123456789");
                     digits != 0 && digits != std::string::npos && digits <= 3 && text.compare(digits, 2, ". ") == 0)
                marker = text.substr(0, digits + 1), body = digits + 2;
            if (body != std::string::npos)
            {
                closeAll();
                item = {Kind::Item, (std::min)(indent / 2, 4), marker, {}};
                itemText = trimmed(text.substr(body));
                itemOpen = true;
                continue;
            }
            if (!markdown) { closeAll(); blocks.push_back({Kind::Paragraph, 0, {}, NoteSpans(text)}); continue; }
            // A line under an item continues it; otherwise it joins the paragraph.
            if (itemOpen) { itemText += " " + text; continue; }
            paragraph += (paragraph.empty() ? "" : " ") + text;
        }
        if (inFence && !code.empty()) blocks.push_back({Kind::Code, 0, {}, {{code, false, true}}});
        closeAll();
        return blocks;
    }

    // UTF-8 text as RTF: the specials escaped, everything past ASCII as
    // \uN? (UTF-16, signed), line breaks as \line.
    inline std::string RtfText(const std::string& s)
    {
        std::string out;
        auto unit = [&](unsigned u) { out += "\\u" + std::to_string(static_cast<int>(static_cast<std::int16_t>(u))) + "?"; };
        for (size_t i = 0; i < s.size();)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80)
            {
                if (c == '\\' || c == '{' || c == '}') out += '\\', out += static_cast<char>(c);
                else if (c == '\n') out += "\\line ";
                else if (c == '\t') out += "\\tab ";
                else if (c >= 32) out += static_cast<char>(c);
                ++i;
                continue;
            }
            const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
            unsigned cp = extra == 3 ? c & 7 : extra == 2 ? c & 15 : c & 31;
            bool ok = extra > 0 && i + static_cast<size_t>(extra) < s.size();
            for (int k = 1; ok && k <= extra; ++k)
            {
                const unsigned char d = static_cast<unsigned char>(s[i + k]);
                if ((d & 0xC0) != 0x80) ok = false;
                else cp = cp << 6 | (d & 0x3F);
            }
            if (!ok) { out += '?'; ++i; continue; }
            i += 1 + extra;
            if (cp >= 0x10000) { cp -= 0x10000; unit(0xD800 + (cp >> 10)); unit(0xDC00 + (cp & 0x3FF)); }
            else unit(cp);
        }
        return out;
    }

    // A release notes document for a rich edit control: a bold title, an
    // optional line under it, the notes, and an optional closing line.
    inline std::string NotesRtf(const std::string& title, const std::string& subtitle, const std::vector<NoteBlock>& blocks, const std::string& footer)
    {
        using Kind = NoteBlock::Kind;
        std::string rtf = "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fswiss Segoe UI;}{\\f1\\fmodern Consolas;}}\\f0\\fs18 ";
        auto spans = [&](const std::vector<NoteSpan>& list) {
            std::string out;
            for (const auto& s : list)
            {
                // A group: its formatting ends with it.
                std::string open;
                if (s.bold) open += "\\b ";
                if (s.code) open += "\\f1 ";
                out += "{" + open + RtfText(s.text) + "}";
            }
            return out;
        };
        rtf += "\\pard\\sa60{\\b\\fs30 " + RtfText(title) + "}\\par ";
        if (!subtitle.empty()) rtf += "\\pard\\sa160 " + RtfText(subtitle) + "\\par ";
        for (const auto& b : blocks)
            switch (b.kind)
            {
            case Kind::Heading:
            {
                const int size = b.level <= 1 ? 26 : b.level == 2 ? 22 : 19;
                rtf += "\\pard\\sb200\\sa80{\\b\\fs" + std::to_string(size) + " " + spans(b.spans) + "}\\par ";
                break;
            }
            case Kind::Paragraph: rtf += "\\pard\\sb80\\sa60 " + spans(b.spans) + "\\par "; break;
            case Kind::Item:
            {
                const int left = 360 + b.level * 360;
                rtf += "\\pard\\fi-240\\li" + std::to_string(left) + "\\tx" + std::to_string(left) + "\\sa50 "
                       + (b.marker.empty() ? std::string(b.level % 2 ? "\\u9702?" : "\\bullet") : RtfText(b.marker)) + "\\tab " + spans(b.spans) + "\\par ";
                break;
            }
            case Kind::Code: rtf += "\\pard\\li360\\sa100{\\f1 " + RtfText(b.spans.empty() ? std::string{} : b.spans[0].text) + "}\\par "; break;
            }
        if (!footer.empty()) rtf += "\\pard\\sb160 " + RtfText(footer) + "\\par ";
        return rtf + "}";
    }

    // "2.3 MB", for the download size GitHub gives the release archive.
    inline std::string SizeText(std::uint64_t bytes)
    {
        if (!bytes) return "size unknown";
        if (bytes < 1024) return std::to_string(bytes) + " bytes";
        const double kb = bytes / 1024.0;
        char text[32];
        if (kb < 1024) std::snprintf(text, sizeof(text), "%.0f KB", kb);
        else std::snprintf(text, sizeof(text), "%.1f MB", kb / 1024.0);
        return text;
    }

    // Remind me later: the offer comes back at the next start, or not before
    // a chosen delay has passed. Stored in [Updates] as RemindVersion (the
    // tag) and RemindAfter (seconds since 1970, UTC; 0 for the next start).
    enum class Snooze { NextStart, OneDay, ThreeDays, OneWeek };
    inline const char* SnoozeText(Snooze s)
    {
        switch (s)
        {
        case Snooze::OneDay: return "Tomorrow";
        case Snooze::ThreeDays: return "In 3 days";
        case Snooze::OneWeek: return "In a week";
        default: return "At the next start";
        }
    }
    inline std::int64_t RemindAfter(std::int64_t now, Snooze s)
    {
        constexpr std::int64_t day = 24 * 60 * 60;
        switch (s)
        {
        case Snooze::OneDay: return now + day;
        case Snooze::ThreeDays: return now + 3 * day;
        case Snooze::OneWeek: return now + 7 * day;
        default: return 0;
        }
    }

    // Whether the start-up check offers this release. Help > Check for
    // Updates always does; a skip or a reminder holds only for the tag it
    // was made for, so a newer release is offered straight away.
    struct OfferSettings
    {
        std::string skipped, remindVersion;
        std::int64_t remindAfter = 0;
    };
    inline bool OfferAtStartup(const std::string& tag, const OfferSettings& settings, std::int64_t now)
    {
        if (!tag.empty() && settings.skipped == tag) return false;
        if (!tag.empty() && settings.remindVersion == tag && now < settings.remindAfter) return false;
        return true;
    }

    // The line under the offer's title.
    inline std::string OfferSummary(const Release& release, const std::string& running)
    {
        std::string text = "You have RE+ " + running + ". Download: " + SizeText(release.assetSize);
        if (!release.assetName.empty()) text += " (" + release.assetName + ")";
        text += ". The update takes effect when you restart the editor.";
        return text;
    }

    // Text cut to at most limit bytes without splitting a UTF-8 sequence.
    inline std::string ClipUtf8(std::string text, size_t limit)
    {
        if (text.size() <= limit) return text;
        text.resize(limit);
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
        if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xC0) text.pop_back();
        return text;
    }

    // The title and Markdown notes of one release object, as GitHub's
    // /releases/tags/<tag> answers it. The title falls back to the tag.
    inline std::pair<std::string, std::string> TitleAndNotes(const Json& release)
    {
        if (!release.is_object()) throw std::runtime_error("GitHub returned an unexpected release.");
        auto text = [&](const char* key) { return release.contains(key) && release[key].is_string() ? release[key].get<std::string>() : std::string{}; };
        auto title = text("name");
        if (title.empty()) title = text("tag_name");
        return {title, text("body")};
    }

    // What's new: the notes of the release the updater installed, kept
    // beside the editor until the first start that runs that version shows
    // them. Stored as a few "Key=value" lines, a blank line, then the notes.
    struct WhatsNew
    {
        std::string version, title, notes; // notes: plain text, CRLF line ends, or Markdown
        bool shown = false;
        bool markdown = false; // records written before 2.2.0 kept plain text

        static constexpr size_t kNotesLimit = 30000; // an EDIT control holds this comfortably

        std::string Format() const
        {
            auto oneLine = [](const std::string& s) {
                std::string out;
                for (size_t i = 0; i < s.size(); ++i)
                    if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
                    else out += s[i] == '\r' || s[i] == '\n' ? ' ' : s[i];
                return out;
            };
            return "RE+ What's New\r\nVersion=" + oneLine(version) + "\r\nTitle=" + oneLine(title)
                   + "\r\nShown=" + (shown ? "1" : "0") + (markdown ? "\r\nMarkdown=1" : "") + "\r\n\r\n" + notes;
        }

        static std::optional<WhatsNew> Parse(const std::string& text)
        {
            WhatsNew out;
            size_t at = 0;
            bool header = false;
            while (at < text.size())
            {
                const auto end = text.find('\n', at);
                std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
                at = end == std::string::npos ? text.size() : end + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) break; // the notes follow
                if (!header) { if (line != "RE+ What's New") return std::nullopt; header = true; continue; }
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                const auto key = line.substr(0, eq), value = line.substr(eq + 1);
                if (key == "Version") out.version = value;
                else if (key == "Title") out.title = value;
                else if (key == "Shown") out.shown = value == "1";
                else if (key == "Markdown") out.markdown = value == "1";
            }
            if (!header || !Version::Parse(out.version)) return std::nullopt;
            out.notes = text.substr((std::min)(at, text.size()));
            return out;
        }

        // Whether these are the notes of the version running now.
        bool For(const std::string& running) const
        {
            const auto a = Version::Parse(version), b = Version::Parse(running);
            return a && b && *a == *b;
        }
        // Shown once, at the first start of the version they describe.
        bool ShowAtStart(const std::string& running) const { return !shown && For(running); }
    };

    // The version in a DLL's VERSIONINFO ProductVersion. RE+ writes
    // "RE+ 2.0.0"; the 1.x builds wrote "2.0.0.1" and the like, which does
    // not say which release it was, so anything else is unknown.
    inline std::optional<Version> ProductVersion(const std::string& text)
    {
        const std::string prefix = "RE+ ";
        if (text.rfind(prefix, 0) != 0) return std::nullopt;
        return Version::Parse(text.substr(prefix.size()));
    }

    // The tag to record as skipped after rolling back from running to
    // previous, so the start-up check does not offer the version just left
    // straight away; empty to leave the setting alone (rolling forward).
    inline std::string SkipAfterRollBack(const std::string& running, const std::optional<Version>& previous)
    {
        const auto current = Version::Parse(running);
        if (!current) return {};
        if (previous && !(*previous < *current)) return {};
        return "v" + current->ToString();
    }

    // What a start does with the files the last update or roll back left.
    // Installing renames the running DLL (and a replaced launcher or MCP
    // server) to .old; the next start keeps it as the previous version, which
    // Roll Back puts back. A previous launcher or MCP server that does not go
    // with the new previous DLL (the update kept it) is removed so the set
    // stays matched: the MCP server and the DLL must speak the same pipe
    // protocol.
    enum class CleanUpAction
    {
        KeepOldDll,          // Reloaded.Editor.dll.old -> Reloaded.Editor.previous.dll
        KeepOldLauncher,     // Reloaded_Editor.exe.old -> Reloaded_Editor.previous.exe
        RemovePreviousLauncher,
        RemoveOldLauncher,
        RemoveStagedDll,     // .update files an interrupted install left
        RemoveStagedLauncher,
        KeepOldMcp,          // Reloaded.Editor.Mcp.exe.old -> Reloaded.Editor.Mcp.previous.exe
        RemovePreviousMcp,
        RemoveOldMcp,
        RemoveStagedMcp,
    };
    struct Leftovers
    {
        bool oldDll = false, oldLauncher = false, previousLauncher = false, stagedDll = false, stagedLauncher = false;
        bool oldMcp = false, previousMcp = false, stagedMcp = false;
    };
    struct CleanUpStep
    {
        CleanUpAction action;
        bool needsDll; // skipped when keeping the old DLL failed, so the set stays together
    };
    inline std::vector<CleanUpStep> CleanUpPlan(const Leftovers& found)
    {
        std::vector<CleanUpStep> steps;
        if (found.oldDll)
        {
            steps.push_back({CleanUpAction::KeepOldDll, false});
            if (found.oldLauncher) steps.push_back({CleanUpAction::KeepOldLauncher, true});
            else if (found.previousLauncher) steps.push_back({CleanUpAction::RemovePreviousLauncher, true});
            if (found.oldMcp) steps.push_back({CleanUpAction::KeepOldMcp, true});
            else if (found.previousMcp) steps.push_back({CleanUpAction::RemovePreviousMcp, true});
        }
        else
        {
            if (found.oldLauncher) steps.push_back({CleanUpAction::RemoveOldLauncher, false});
            if (found.oldMcp) steps.push_back({CleanUpAction::RemoveOldMcp, false});
        }
        if (found.stagedDll) steps.push_back({CleanUpAction::RemoveStagedDll, false});
        if (found.stagedLauncher) steps.push_back({CleanUpAction::RemoveStagedLauncher, false});
        if (found.stagedMcp) steps.push_back({CleanUpAction::RemoveStagedMcp, false});
        return steps;
    }

    // Started by the MCP server (editor_launch passes -reMcp): the start-up
    // update offer would hold up its calls, so the check waits for a start
    // by hand. Any argument equal to -reMcp, ignoring case.
    inline bool StartedForMcp(const std::vector<std::string>& arguments)
    {
        return std::any_of(arguments.begin(), arguments.end(), [](const std::string& a) { return Lower(a) == "-remcp"; });
    }

    inline std::uint32_t Crc32(const std::uint8_t* data, size_t size)
    {
        static const auto table = [] {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t i = 0; i < 256; ++i)
            {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        std::uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    struct ZipEntry
    {
        std::string name; // as stored, '/' separated
        std::uint16_t method = 0; // 0 stored, 8 deflate
        std::uint32_t crc = 0, compressedSize = 0, size = 0;
        size_t dataOffset = 0; // the compressed bytes in the archive
    };

    // The entries of a zip archive, read from its central directory.
    // Multi-disk, ZIP64 and encrypted archives are refused.
    inline std::vector<ZipEntry> ReadZip(const Bytes& zip)
    {
        auto fail = [](const char* why) -> void { throw std::runtime_error(std::string("The update archive is not a usable zip file: ") + why); };
        auto u16 = [&](size_t at) { if (at + 2 > zip.size()) fail("it is truncated."); return static_cast<std::uint16_t>(zip[at] | zip[at + 1] << 8); };
        auto u32 = [&](size_t at) { if (at + 4 > zip.size()) fail("it is truncated."); return static_cast<std::uint32_t>(zip[at] | zip[at + 1] << 8 | zip[at + 2] << 16 | static_cast<std::uint32_t>(zip[at + 3]) << 24); };
        if (zip.size() < 22) fail("it is too small.");
        size_t end = std::string::npos;
        for (size_t at = zip.size() - 22;; --at)
        {
            if (u32(at) == 0x06054b50 && at + 22 + u16(at + 20) == zip.size()) { end = at; break; }
            if (at == 0 || zip.size() - at > 22 + 0xFFFF) break;
        }
        if (end == std::string::npos) fail("its directory is missing.");
        if (u16(end + 4) != 0 || u16(end + 6) != 0) fail("it spans several disks.");
        const unsigned count = u16(end + 10);
        if (count != u16(end + 8)) fail("it spans several disks.");
        const std::uint32_t directorySize = u32(end + 12), directoryOffset = u32(end + 16);
        if (directoryOffset == 0xFFFFFFFFu || count == 0xFFFF) fail("it is a ZIP64 archive.");
        if (static_cast<std::uint64_t>(directoryOffset) + directorySize > end) fail("its directory is out of range.");
        std::vector<ZipEntry> entries;
        size_t at = directoryOffset;
        for (unsigned i = 0; i < count; ++i)
        {
            if (u32(at) != 0x02014b50) fail("a directory record is damaged.");
            ZipEntry e;
            const std::uint16_t flags = u16(at + 8);
            e.method = u16(at + 10);
            e.crc = u32(at + 16);
            e.compressedSize = u32(at + 20);
            e.size = u32(at + 24);
            const std::uint16_t nameLength = u16(at + 28), extraLength = u16(at + 30), commentLength = u16(at + 32);
            const std::uint32_t local = u32(at + 42);
            if (at + 46 + nameLength > end) fail("a file name is out of range.");
            e.name.assign(reinterpret_cast<const char*>(&zip[at + 46]), nameLength);
            at += 46 + nameLength + extraLength + commentLength;
            if (flags & 1) fail("it is encrypted.");
            if (e.compressedSize == 0xFFFFFFFFu || e.size == 0xFFFFFFFFu || local == 0xFFFFFFFFu) fail("it is a ZIP64 archive.");
            if (u32(local) != 0x04034b50) fail("a file record is damaged.");
            e.dataOffset = static_cast<size_t>(local) + 30 + u16(local + 26) + u16(local + 28);
            if (static_cast<std::uint64_t>(e.dataOffset) + e.compressedSize > directoryOffset) fail("a file is out of range.");
            entries.push_back(std::move(e));
        }
        return entries;
    }

    // The one entry whose file name (ignoring any folder) is name, compared
    // without case; nullptr when there is none. Two such entries are refused.
    inline const ZipEntry* FindEntry(const std::vector<ZipEntry>& entries, const std::string& name)
    {
        const ZipEntry* found = nullptr;
        for (const auto& e : entries)
        {
            const auto slash = e.name.find_last_of("/\\");
            if (Lower(slash == std::string::npos ? e.name : e.name.substr(slash + 1)) != Lower(name)) continue;
            if (found) throw std::runtime_error("The update archive contains " + name + " more than once.");
            found = &e;
        }
        return found;
    }

    // A 32-bit Windows image, and whether it is a DLL.
    enum class Image { None, Exe, Dll };
    inline Image ImageKind(const Bytes& file)
    {
        auto u16 = [&](size_t at) { return static_cast<unsigned>(file[at] | file[at + 1] << 8); };
        if (file.size() < 0x40 || file[0] != 'M' || file[1] != 'Z') return Image::None;
        const size_t pe = file[0x3C] | file[0x3D] << 8 | file[0x3E] << 16 | static_cast<size_t>(file[0x3F]) << 24;
        if (pe > file.size() || file.size() - pe < 24 || file[pe] != 'P' || file[pe + 1] != 'E' || file[pe + 2] || file[pe + 3]) return Image::None;
        if (u16(pe + 4) != 0x14C) return Image::None; // IMAGE_FILE_MACHINE_I386
        return u16(pe + 22) & 0x2000 ? Image::Dll : Image::Exe; // IMAGE_FILE_DLL
    }
}
