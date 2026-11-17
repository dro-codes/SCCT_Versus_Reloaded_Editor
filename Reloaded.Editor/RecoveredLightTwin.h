#pragma once
// Finding the editor copy a compiled map was cooked from, by its lightmaps
// (the OpenSCCT Editor's TwinLibrary), and taking Light actors out of a map's
// text export.
//
// One stock Save writes the editor copy and the compiled copy from the same
// bake, so their lightmap atlases are byte-identical: the atlases fingerprint
// the bake, under any file name, in any install. An atlas is written as
// FStaticLightMapTexture (editor 0x110D0A80):
//
//     i32 0, compact n, n bytes      the texels (B,G,R,A), a lazy array
//     i32 0, compact 0               a second, empty lazy array
//     u8  5                          TEXF_RGBA8
//     i32 width, i32 height          n = width x height x 4
//
// which is distinctive enough to find in a package's bytes without decoding
// the model around it.
//
// No Windows or engine calls, so the tests compile it alone.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace RecoveredLightTwin
{
    struct Atlas
    {
        std::size_t offset = 0; // of the texels
        int width = 0, height = 0;
        std::string hash;
    };

    // Unreal's compact index. False when the bytes run out.
    inline bool ReadCompact(const unsigned char* data, std::size_t size, std::size_t& at, std::int64_t& value)
    {
        if (at >= size) return false;
        const unsigned char first = data[at++];
        std::int64_t result = first & 0x3F;
        if (first & 0x40)
        {
            int shift = 6;
            for (int index = 0; index < 4; ++index)
            {
                if (at >= size) return false;
                const unsigned char next = data[at++];
                result |= static_cast<std::int64_t>(next & 0x7F) << shift;
                shift += 7;
                if (!(next & 0x80)) break;
            }
        }
        value = (first & 0x80) ? -result : result;
        return true;
    }

    // Two 64-bit FNV-1a sums from different starts, as hex.
    inline std::string HashBytes(const unsigned char* data, std::size_t size)
    {
        std::uint64_t a = 14695981039346656037ull, b = 0x9E3779B97F4A7C15ull;
        for (std::size_t index = 0; index < size; ++index)
        {
            a = (a ^ data[index]) * 1099511628211ull;
            b = (b ^ data[index]) * 1099511628211ull;
            b ^= b >> 29;
        }
        char text[40];
        std::snprintf(text, sizeof(text), "%016llx%016llx", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
        return text;
    }

    // Every lightmap atlas in a package's (decompressed) bytes.
    inline std::vector<Atlas> FindAtlases(const unsigned char* data, std::size_t size)
    {
        std::vector<Atlas> found;
        if (size < 32) return found;
        for (std::size_t at = 0; at + 24 < size; ++at)
        {
            // A texel count is a multiple of 64 of at least 1,024: its
            // compact index starts with the continuation bit alone.
            if (data[at + 4] != 0x40 || data[at] | data[at + 1] | data[at + 2] | data[at + 3]) continue;
            std::size_t cursor = at + 4;
            std::int64_t count = 0;
            if (!ReadCompact(data, size, cursor, count) || count < 16 * 16 * 4 || count > 2048 * 2048 * 4) continue;
            const std::size_t texels = cursor, end = texels + static_cast<std::size_t>(count);
            if (end + 14 > size) continue;
            static const unsigned char trailer[6] = {0, 0, 0, 0, 0, 5};
            if (std::memcmp(data + end, trailer, 6) != 0) continue;
            std::int32_t width = 0, height = 0;
            std::memcpy(&width, data + end + 6, 4);
            std::memcpy(&height, data + end + 10, 4);
            if (width < 16 || height < 16 || width > 2048 || height > 2048
                || static_cast<std::int64_t>(width) * height * 4 != count) continue;
            found.push_back({texels, width, height, HashBytes(data + texels, static_cast<std::size_t>(count))});
            at = end + 13;
        }
        return found;
    }

    // A bake's atlases as sorted hashes.
    using Fingerprint = std::vector<std::string>;

    inline Fingerprint FingerprintOf(const std::vector<Atlas>& atlases)
    {
        Fingerprint print;
        for (const Atlas& atlas : atlases) print.push_back(atlas.hash);
        std::sort(print.begin(), print.end());
        return print;
    }

    inline bool SameBake(const Fingerprint& a, const Fingerprint& b) { return !a.empty() && a == b; }

    // How many atlases two bakes share (a copy edited after the cook keeps
    // the atlases of rooms it did not relight).
    inline std::size_t Shared(const Fingerprint& a, const Fingerprint& b)
    {
        std::vector<std::string> ua = a, ub = b, both;
        ua.erase(std::unique(ua.begin(), ua.end()), ua.end());
        ub.erase(std::unique(ub.begin(), ub.end()), ub.end());
        std::set_intersection(ua.begin(), ua.end(), ub.begin(), ub.end(), std::back_inserter(both));
        return both.size();
    }

    inline std::string Lower(std::string text)
    {
        for (char& character : text) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        return text;
    }

    // An editor copy the search knows.
    struct Entry
    {
        std::string path;
        std::uint64_t size = 0;
        std::int64_t modified = 0;
        std::string package;    // the file's name without its extension
        Fingerprint fingerprint;
    };

    // The cache: one line a file, tab separated (path, size, modified,
    // package, hashes joined by commas), so only new or changed files are
    // read again.
    inline std::string WriteCache(const std::vector<Entry>& entries)
    {
        std::string text = "RecoveredLightTwins 1\n";
        for (const Entry& entry : entries)
        {
            text += entry.path + "\t" + std::to_string(entry.size) + "\t" + std::to_string(entry.modified) + "\t" + entry.package + "\t";
            for (std::size_t index = 0; index < entry.fingerprint.size(); ++index)
                text += (index ? "," : "") + entry.fingerprint[index];
            text += "\n";
        }
        return text;
    }

    inline std::vector<Entry> ReadCache(const std::string& text)
    {
        std::vector<Entry> entries;
        std::istringstream lines(text);
        std::string line;
        if (!std::getline(lines, line) || line != "RecoveredLightTwins 1") return entries;
        while (std::getline(lines, line))
        {
            std::vector<std::string> fields;
            std::size_t start = 0;
            for (;;)
            {
                const std::size_t tab = line.find('\t', start);
                fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                if (tab == std::string::npos) break;
                start = tab + 1;
            }
            if (fields.size() != 5) continue;
            Entry entry;
            entry.path = fields[0];
            entry.size = std::strtoull(fields[1].c_str(), nullptr, 10);
            entry.modified = std::strtoll(fields[2].c_str(), nullptr, 10);
            entry.package = fields[3];
            std::istringstream hashes(fields[4]);
            std::string hash;
            while (std::getline(hashes, hash, ','))
                if (!hash.empty()) entry.fingerprint.push_back(hash);
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    // The copies whose atlases are the compiled map's exactly: those of the
    // same package name first, then the newest.
    inline std::vector<Entry> FindExact(const std::vector<Entry>& entries, const Fingerprint& cooked, const std::string& package)
    {
        std::vector<Entry> found;
        for (const Entry& entry : entries)
            if (SameBake(entry.fingerprint, cooked)) found.push_back(entry);
        const std::string wanted = Lower(package);
        std::stable_sort(found.begin(), found.end(), [&](const Entry& a, const Entry& b)
        {
            const bool an = Lower(a.package) == wanted, bn = Lower(b.package) == wanted;
            return an != bn ? an : a.modified > b.modified;
        });
        return found;
    }

    // Copies that are not the compiled map's bake but may be its source
    // edited since: the same package name, or any atlas in common; most
    // atlases in common first.
    inline std::vector<Entry> FindStale(const std::vector<Entry>& entries, const Fingerprint& cooked, const std::string& package)
    {
        std::vector<Entry> found;
        const std::string wanted = Lower(package);
        for (const Entry& entry : entries)
            if (!entry.fingerprint.empty() && !SameBake(entry.fingerprint, cooked)
                && (Lower(entry.package) == wanted || Shared(entry.fingerprint, cooked) > 0)) found.push_back(entry);
        std::stable_sort(found.begin(), found.end(), [&](const Entry& a, const Entry& b)
        {
            const std::size_t as = Shared(a.fingerprint, cooked), bs = Shared(b.fingerprint, cooked);
            if (as != bs) return as > bs;
            const bool an = Lower(a.package) == wanted, bn = Lower(b.package) == wanted;
            return an != bn ? an : a.modified > b.modified;
        });
        return found;
    }

    // ---- Light actors in a map's text export --------------------------------------------------------------------

    struct ActorBlock
    {
        std::string className, name, text; // text: "Begin Actor ... End Actor\n"
    };

    inline std::string Trimmed(const std::string& line)
    {
        std::size_t first = 0, last = line.size();
        while (first < last && std::isspace(static_cast<unsigned char>(line[first]))) ++first;
        while (last > first && std::isspace(static_cast<unsigned char>(line[last - 1]))) --last;
        return line.substr(first, last - first);
    }

    // The value after "Key=" on a "Begin Actor" line.
    inline std::string HeaderValue(const std::string& line, const std::string& key)
    {
        const std::string lower = Lower(line), wanted = " " + Lower(key) + "=";
        const std::size_t at = lower.find(wanted);
        if (at == std::string::npos) return "";
        std::size_t start = at + wanted.size(), end = start;
        while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) ++end;
        return line.substr(start, end - start);
    }

    // The top-level actors of a map export.
    inline std::vector<ActorBlock> Actors(const std::string& map)
    {
        std::vector<ActorBlock> actors;
        std::istringstream lines(map);
        std::string line;
        ActorBlock current;
        int depth = 0;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string lower = Lower(Trimmed(line));
            const bool begins = lower.rfind("begin actor", 0) == 0, ends = lower == "end actor";
            if (begins && depth++ == 0)
            {
                current = {};
                current.className = HeaderValue(line, "Class");
                current.name = HeaderValue(line, "Name");
            }
            if (depth > 0) current.text += line + "\n";
            if (ends && depth > 0 && --depth == 0) actors.push_back(std::move(current));
        }
        return actors;
    }

    // True when the actor's own text sets the property (a default is not written).
    inline bool SetsProperty(const ActorBlock& actor, const std::string& property)
    {
        std::istringstream lines(actor.text);
        std::string line;
        const std::string wanted = Lower(property) + "=";
        int depth = 0;
        while (std::getline(lines, line))
        {
            const std::string lower = Lower(Trimmed(line));
            if (lower.rfind("begin ", 0) == 0) { ++depth; continue; }
            if (lower.rfind("end ", 0) == 0) { --depth; continue; }
            if (depth == 1 && lower.rfind(wanted, 0) == 0) return true;
        }
        return false;
    }

    // Replaces a name wherever it stands as a whole word (case ignored).
    inline std::string ReplaceName(const std::string& text, const std::string& from, const std::string& to)
    {
        if (from.empty() || Lower(from) == Lower(to)) return text;
        const std::string lower = Lower(text), wanted = Lower(from);
        auto word = [](char character) { return std::isalnum(static_cast<unsigned char>(character)) || character == '_'; };
        std::string result;
        std::size_t at = 0;
        for (;;)
        {
            const std::size_t found = lower.find(wanted, at);
            if (found == std::string::npos) break;
            const std::size_t end = found + wanted.size();
            result += text.substr(at, found - at);
            const bool whole = (found == 0 || !word(text[found - 1])) && (end >= text.size() || !word(text[end]));
            result += whole ? to : text.substr(found, wanted.size());
            at = end;
        }
        return result + text.substr(at);
    }

    // A name not yet used (case ignored), from `wanted` with a number added.
    inline std::string UniqueName(const std::string& wanted, std::vector<std::string>& usedLower)
    {
        std::string name = wanted;
        for (int index = 1; std::find(usedLower.begin(), usedLower.end(), Lower(name)) != usedLower.end(); ++index)
            name = wanted + "_" + std::to_string(index);
        usedLower.push_back(Lower(name));
        return name;
    }
}
