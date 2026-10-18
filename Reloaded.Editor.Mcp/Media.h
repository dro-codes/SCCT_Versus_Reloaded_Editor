#pragma once
// Media for the exe's tools (media_extract_frames, media_read_image,
// image_convert, import_to_spec, assets_import): image files of any common
// format, frames pulled out of videos, contact sheets, measurement grids,
// palettes, texture-ready sizes, and OBJ meshes rewritten as ASE for the
// editor's static mesh importer.
//
// The functions declared here and defined in Media.cpp use WIC and Media
// Foundation on Windows and fall back to ImageCodec.h and an ffmpeg child
// process anywhere. Everything inline is pure and portable, so the tests run
// it on Linux. The server reads media for the agent; it never interprets it.
#include "ImageCodec.h"
#include "PlanRender.h"
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Mcp::Media
{
    using Json = Workflow::Json;

    // --- Files ---------------------------------------------------------------
    // JSON text is UTF-8; path's narrow form is the ANSI code page on Windows.
    inline std::filesystem::path Utf8Path(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }
    inline std::string Utf8(const std::filesystem::path& path)
    {
        const auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }
    inline std::string Lower(std::string text)
    {
        for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }
    inline std::string ReadFile(const std::filesystem::path& path, size_t limit = 512u * 1024 * 1024)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) throw std::runtime_error(Utf8(path) + " does not exist or is not a file.");
        const auto size = std::filesystem::file_size(path, error);
        if (error) throw std::runtime_error("Cannot read the size of " + Utf8(path) + ".");
        if (size > limit) throw std::runtime_error(Utf8(path) + " is larger than " + std::to_string(limit / (1024 * 1024)) + " MB.");
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open " + Utf8(path) + ".");
        std::string bytes(static_cast<size_t>(size), '\0');
        if (size && !in.read(bytes.data(), static_cast<std::streamsize>(size))) throw std::runtime_error("Cannot read " + Utf8(path) + ".");
        return bytes;
    }
    // Written beside itself first, then moved over, so a reader never sees half a file.
    inline void WriteFile(const std::filesystem::path& path, const std::string& bytes)
    {
        std::error_code error;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
        auto temporary = path;
        temporary += ".part";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("Cannot write " + Utf8(path) + ".");
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!out) throw std::runtime_error("Cannot write " + Utf8(path) + " (is the disk full?).");
        }
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            std::filesystem::remove(path, error);
            std::filesystem::rename(temporary, path, error);
            if (error) throw std::runtime_error("Cannot replace " + Utf8(path) + ": " + error.message());
        }
    }
    inline std::filesystem::path Absolute(const std::filesystem::path& path)
    {
        std::error_code error;
        auto full = std::filesystem::weakly_canonical(std::filesystem::absolute(path, error), error);
        if (error) full = std::filesystem::absolute(path, error);
        return full.lexically_normal();
    }
    // Whether path lies inside root (or is root), both made absolute.
    inline bool Inside(const std::filesystem::path& path, const std::filesystem::path& root)
    {
        auto key = [](const std::filesystem::path& p)
        {
            auto text = Utf8(Absolute(p));
#ifdef _WIN32
            text = Lower(text);
            for (auto& c : text) if (c == '/') c = '\\';
#endif
            while (text.size() > 1 && (text.back() == '/' || text.back() == '\\')) text.pop_back();
            return text;
        };
        const auto p = key(path), r = key(root);
        if (r.empty() || p.size() < r.size() || p.compare(0, r.size(), r) != 0) return false;
        return p.size() == r.size() || p[r.size()] == '/' || p[r.size()] == '\\' || r.back() == '/' || r.back() == '\\';
    }
    // The folders tools may write into: the work folder and [Mcp] WriteRoots
    // (folders separated by ';').
    inline std::vector<std::filesystem::path> WriteRoots(const std::filesystem::path& workDir, const std::string& writeRoots)
    {
        std::vector<std::filesystem::path> roots;
        if (!workDir.empty()) roots.push_back(workDir);
        std::string item;
        std::istringstream list(writeRoots);
        while (std::getline(list, item, ';'))
        {
            const auto first = item.find_first_not_of(" \t\"");
            if (first == std::string::npos) continue;
            item = item.substr(first, item.find_last_not_of(" \t\"") - first + 1);
            if (!item.empty()) roots.push_back(std::filesystem::path(item)); // the INI is ANSI, as path's narrow form
        }
        return roots;
    }
    inline void CheckWritable(const std::filesystem::path& path, const std::vector<std::filesystem::path>& roots)
    {
        for (const auto& root : roots) if (Inside(path, root)) return;
        std::string list;
        for (const auto& root : roots) list += (list.empty() ? "" : "; ") + Utf8(root);
        throw std::runtime_error("The server writes only inside its work folder and [Mcp] WriteRoots (" + (list.empty() ? std::string("none set") : list) + "), not " + Utf8(path) +
                                 ". Leave the output path out to write in the work folder, or ask the user to add the folder to WriteRoots in System\\Reloaded_Editor.ini.");
    }
    // A folder name for this moment, unique within the run: 20261018-142233-001 (UTC).
    inline std::string Stamp()
    {
        using namespace std::chrono;
        static std::mutex lock;
        static unsigned counter = 0;
        unsigned n = 0;
        {
            std::lock_guard<std::mutex> guard(lock);
            n = ++counter;
        }
        const auto now = system_clock::now();
        const auto today = floor<days>(now);
        const year_month_day date{today};
        const hh_mm_ss<seconds> hms{floor<seconds>(now - today)};
        char text[48];
        std::snprintf(text, sizeof text, "%04d%02u%02u-%02d%02d%02d-%03u", static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                      static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count()), static_cast<int>(hms.seconds().count()), n % 1000);
        return text;
    }
    // A file name made of letters, digits, '_', '-' and '.', for names the engine reads.
    inline std::string SafeName(const std::string& wanted, const std::string& fallback = "file")
    {
        std::string out;
        for (unsigned char c : wanted) out += (std::isalnum(c) || c == '_' || c == '-' || c == '.') && c < 0x80 ? static_cast<char>(c) : '_';
        while (!out.empty() && (out.front() == '.' || out.front() == '_')) out.erase(out.begin());
        if (out.size() > 60) out.resize(60);
        return out.empty() ? fallback : out;
    }

    // --- Images (Media.cpp) --------------------------------------------------
    // An image file as RGBA. Windows: WIC (PNG, JPEG, GIF's first frame, TIFF,
    // BMP, ICO and any codec installed), with ImageCodec.h for TGA and as the
    // fallback; elsewhere ImageCodec.h (PNG, BMP, TGA). Errors name the file.
    Image::Rgba DecodeImageFile(const std::filesystem::path& path);
    // PNG bytes: WIC's encoder on Windows (zlib-grade compression), ImageCodec.h elsewhere.
    std::string PngBytes(const Image::Rgba& image);
    // What DecodeImageFile reads in this build, for messages.
    std::string ImageFormats();

    // --- Video frames --------------------------------------------------------
    struct Frame
    {
        std::filesystem::path path;
        double time = 0;    // seconds from the start of the video
        int width = 0, height = 0;
        double change = -1; // scene mode: how different from the frame before (0-1)
    };
    struct FrameOptions
    {
        std::string mode = "interval"; // interval | count | scene
        double intervalS = 2;          // interval: seconds between frames
        int count = 12;                // count: how many, evenly spread; interval and scene: the most (1-64)
        double start = 0, end = -1;    // seconds; end < 0 is the end of the video
        int maxEdge = 768;             // the longer side of each frame, 16-1280 (never enlarged)
        double threshold = 0.3;        // scene: the luma histogram change (0-1) that counts as a cut
        std::filesystem::path outDir;  // created when missing
        std::filesystem::path ffmpeg;  // the ffmpeg executable or its folder; empty: PATH
        std::string backend = "auto";  // auto | mediafoundation | ffmpeg
        std::function<void(double done, double total, const std::string& message)> progress;
    };
    struct Extracted
    {
        std::vector<Frame> frames;
        std::string backend;          // mediafoundation | ffmpeg
        double duration = -1;         // seconds, -1 when unknown
        int width = 0, height = 0;    // the video's own frame size (display aspect)
        std::vector<std::string> notes;
    };
    // Frames of a video as PNG files in outDir, each at most maxEdge on its
    // longer side. Windows: Media Foundation (IMFSourceReader, RGB32, seeking
    // per frame); when it cannot read the file, or elsewhere: ffmpeg run with an
    // argument vector. Scene mode samples 4 x count frames evenly, measures the
    // luma histogram change between neighbours and keeps the first frame plus
    // the biggest changes over threshold.
    Extracted ExtractFrames(const std::filesystem::path& video, const FrameOptions& options);
    // The frames tiled in columns (0: about square), each labelled with its
    // number and time, scaled so the sheet's longer side is at most maxEdge.
    Image::Rgba ContactSheet(const std::vector<Frame>& frames, int columns, int maxEdge);
    // The ffmpeg configured (a file, or a folder holding it), else the first
    // on PATH; empty when there is none. A configured path that does not
    // exist is an error, so a typo in the INI shows.
    std::filesystem::path FindFfmpeg(const std::filesystem::path& configured);
    // A program run with an argument vector, never through a shell: stdin is
    // empty, stdout and stderr go to log. Returns the exit code; throws when it
    // cannot start, or when it outlives timeoutMs (it is killed first).
    int RunProcess(const std::filesystem::path& program, const std::vector<std::string>& arguments, const std::filesystem::path& log, unsigned timeoutMs);

    // --- Pure helpers for frames ---------------------------------------------
    inline std::string TimeText(double seconds)
    {
        if (seconds < 0) return "?";
        const auto tenths = static_cast<long long>(std::llround(seconds * 10));
        char text[32];
        std::snprintf(text, sizeof text, "%lld:%02lld.%lld", tenths / 600, (tenths / 10) % 60, tenths % 10);
        return text;
    }
    // Seconds as ffmpeg reads them: plain decimal.
    inline std::string Seconds(double seconds)
    {
        char text[32];
        std::snprintf(text, sizeof text, "%.3f", seconds < 0 ? 0.0 : seconds);
        return text;
    }
    inline void CheckFrameOptions(const FrameOptions& o)
    {
        if (o.mode != "interval" && o.mode != "count" && o.mode != "scene") throw std::runtime_error("mode is interval, count or scene, not \"" + o.mode + "\".");
        if (o.count < 1 || o.count > 64) throw std::runtime_error("count must be from 1 to 64 (frames are big; narrow start and end for detail).");
        if (!(o.intervalS >= 0.04 && o.intervalS <= 3600)) throw std::runtime_error("intervalS must be from 0.04 to 3600 seconds.");
        if (!(o.start >= 0)) throw std::runtime_error("start must be 0 or more seconds.");
        if (o.end >= 0 && o.end <= o.start) throw std::runtime_error("end must be after start.");
        if (o.maxEdge < 16 || o.maxEdge > 1280) throw std::runtime_error("maxEdge must be from 16 to 1280 pixels.");
        if (!(o.threshold > 0 && o.threshold <= 1)) throw std::runtime_error("threshold must be above 0 and at most 1.");
        if (o.outDir.empty()) throw std::runtime_error("Frames need an output folder.");
        const auto b = Lower(o.backend);
        if (b != "auto" && b != "mediafoundation" && b != "ffmpeg") throw std::runtime_error("backend is auto, mediafoundation or ffmpeg.");
    }
    // The times to read. duration < 0 means unknown: interval mode then reads
    // until the video ends; count and scene need an end.
    inline std::vector<double> SampleTimes(const FrameOptions& o, double duration, std::vector<std::string>& notes)
    {
        double end = o.end;
        if (duration > 0)
        {
            if (o.start >= duration) throw std::runtime_error("start (" + TimeText(o.start) + ") is at or past the end of the video (" + TimeText(duration) + ").");
            if (end > duration) notes.push_back("end is past the end of the video (" + TimeText(duration) + "), so frames stop there.");
            if (end < 0 || end > duration) end = duration;
        }
        else if (end < 0 && o.mode != "interval")
            throw std::runtime_error("The video's length is unknown, so " + o.mode + " mode needs end (in seconds).");
        std::vector<double> times;
        const size_t count = static_cast<size_t>(o.count);
        if (o.mode == "interval")
        {
            for (size_t i = 0; times.size() < count; ++i)
            {
                const double t = o.start + static_cast<double>(i) * o.intervalS;
                if (end >= 0 && t >= end - 1e-6) break;
                times.push_back(t);
            }
            const double next = o.start + static_cast<double>(times.size()) * o.intervalS;
            if (times.size() == count && (end < 0 || next < end - 1e-6))
                notes.push_back("Stopped at " + std::to_string(count) + " frames (" + TimeText(times.back()) + "); raise intervalS, or start later, for the rest.");
            return times;
        }
        const double span = end - o.start;
        if (o.mode == "count")
        {
            // The middle of each of count equal slices: never the black first
            // frame or the end of the stream.
            for (size_t i = 0; i < count; ++i) times.push_back(o.start + (static_cast<double>(i) + 0.5) * span / static_cast<double>(count));
            return times;
        }
        // Scene: candidates at the start of equal slices, no closer than 1/30 s.
        size_t candidates = std::clamp<size_t>(count * 4, 16, 192);
        candidates = std::max<size_t>(2, std::min(candidates, static_cast<size_t>(span * 30)));
        for (size_t i = 0; i < candidates; ++i) times.push_back(o.start + static_cast<double>(i) * span / static_cast<double>(candidates));
        return times;
    }
    using Histogram = std::array<double, 64>;
    // The distribution of brightness (Rec. 709 luma, 64 bins), from at most
    // about 65,000 sampled pixels.
    inline Histogram LumaHistogram(const Image::Rgba& image)
    {
        Histogram h{};
        if (image.Empty()) return h;
        const size_t pixels = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
        const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(pixels) / 65536.0)));
        double total = 0;
        for (int y = 0; y < image.height; y += step)
            for (int x = 0; x < image.width; x += step)
            {
                const uint8_t* p = image.At(x, y);
                const int luma = (54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8;
                h[static_cast<size_t>(std::clamp(luma, 0, 255) >> 2)] += 1;
                total += 1;
            }
        for (auto& v : h) v /= total;
        return h;
    }
    // Half the L1 distance of two histograms: 0 the same, 1 nothing shared.
    inline double HistogramChange(const Histogram& a, const Histogram& b)
    {
        double sum = 0;
        for (size_t i = 0; i < a.size(); ++i) sum += std::abs(a[i] - b[i]);
        return std::min(1.0, sum / 2);
    }
    // The frames to keep: the first, then the biggest changes of at least
    // threshold (up to count frames in all), never the frame right beside an
    // earlier cut, so one slow transition is not taken twice. change[i] is
    // frame i's change from frame i - 1.
    inline std::vector<size_t> PickScenes(const std::vector<Histogram>& histograms, size_t count, double threshold, std::vector<double>& change)
    {
        const size_t n = histograms.size();
        change.assign(n, 0.0);
        std::vector<size_t> picks;
        if (!n || !count) return picks;
        for (size_t i = 1; i < n; ++i) change[i] = HistogramChange(histograms[i - 1], histograms[i]);
        std::vector<size_t> order;
        for (size_t i = 1; i < n; ++i) if (change[i] >= threshold) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return change[x] > change[y]; });
        std::vector<bool> cut(n, false);
        picks.push_back(0);
        for (const auto i : order)
        {
            if (picks.size() >= count) break;
            if (cut[i - 1] || (i + 1 < n && cut[i + 1])) continue;
            cut[i] = true;
            picks.push_back(i);
        }
        std::sort(picks.begin(), picks.end());
        return picks;
    }
    // ffmpeg's report of an input: "Duration: 00:01:02.50" -> 62.5; -1 when absent or N/A.
    namespace Detail
    {
        // A whole number at text[at], its end in at; false when there is none.
        inline bool Whole(const std::string& text, size_t& at, long& value)
        {
            if (at >= text.size() || !std::isdigit(static_cast<unsigned char>(text[at]))) return false;
            char* end = nullptr;
            value = std::strtol(text.c_str() + at, &end, 10);
            at = static_cast<size_t>(end - text.c_str());
            return true;
        }
    }
    // ffmpeg's report of an input: "Duration: 00:01:02.50" -> 62.5; -1 when absent or N/A.
    inline double FfmpegDuration(const std::string& log)
    {
        const auto found = log.find("Duration: ");
        if (found == std::string::npos) return -1;
        size_t at = found + 10;
        long h = 0, m = 0;
        if (!Detail::Whole(log, at, h) || at >= log.size() || log[at++] != ':' || !Detail::Whole(log, at, m) || at >= log.size() || log[at++] != ':') return -1;
        if (at >= log.size() || !std::isdigit(static_cast<unsigned char>(log[at]))) return -1;
        const double s = std::strtod(log.c_str() + at, nullptr);
        return static_cast<double>(h) * 3600.0 + static_cast<double>(m) * 60.0 + s;
    }
    // The first video stream's size in display proportions ("1920x1080 [SAR 1:1 DAR 16:9]"); {0, 0} when absent.
    inline std::pair<int, int> FfmpegVideoSize(const std::string& log)
    {
        const auto line = log.find("Video:");
        if (line == std::string::npos) return {0, 0};
        const auto stop = log.find('\n', line);
        const auto text = log.substr(line, stop == std::string::npos ? std::string::npos : stop - line);
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (!std::isdigit(static_cast<unsigned char>(text[i])) || (i && std::isalnum(static_cast<unsigned char>(text[i - 1])))) continue;
            size_t at = i;
            long w = 0, h = 0;
            if (!Detail::Whole(text, at, w) || at >= text.size() || text[at] != 'x') continue;
            ++at;
            if (!Detail::Whole(text, at, h) || (at < text.size() && std::isalnum(static_cast<unsigned char>(text[at])))) continue;
            if (w < 2 || h < 2 || w > 16384 || h > 16384) continue;
            long sarW = 1, sarH = 1;
            if (text.compare(at, 6, " [SAR ") == 0)
            {
                size_t s = at + 6;
                if (Detail::Whole(text, s, sarW) && s < text.size() && text[s] == ':' && Detail::Whole(text, ++s, sarH) && sarW > 0 && sarH > 0 && sarW != sarH)
                    w = std::max<long>(1, std::lround(static_cast<double>(w) * static_cast<double>(sarW) / static_cast<double>(sarH)));
            }
            return {static_cast<int>(w), static_cast<int>(h)};
        }
        return {0, 0};
    }
    // One argument quoted for a Windows command line, the way
    // CommandLineToArgvW and the C runtime read it back.
    inline std::string QuoteArgument(const std::string& argument)
    {
        if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string::npos) return argument;
        std::string out = "\"";
        size_t slashes = 0;
        for (char c : argument)
        {
            if (c == '\\') { ++slashes; continue; }
            if (c == '"') out.append(slashes * 2 + 1, '\\');
            else out.append(slashes, '\\');
            slashes = 0;
            out += c;
        }
        out.append(slashes * 2, '\\');
        return out + "\"";
    }
    // frame_003_12.500s.png
    inline std::string FrameName(size_t number, double seconds)
    {
        char text[64];
        std::snprintf(text, sizeof text, "frame_%03u_%.3fs.png", static_cast<unsigned>(number), seconds < 0 ? 0.0 : seconds);
        return text;
    }

    // --- Image analysis (media_read_image) -----------------------------------
    using Rgb = std::array<int, 3>;
    inline std::string Hex(const Rgb& c)
    {
        char text[8];
        std::snprintf(text, sizeof text, "#%02x%02x%02x", std::clamp(c[0], 0, 255), std::clamp(c[1], 0, 255), std::clamp(c[2], 0, 255));
        return text;
    }
    inline Rgb ParseHex(const std::string& text)
    {
        auto t = text;
        if (!t.empty() && t[0] == '#') t = t.substr(1);
        if (t.size() == 3) t = std::string{t[0], t[0], t[1], t[1], t[2], t[2]};
        if (t.size() != 6 || t.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) throw std::runtime_error("Colour \"" + text + "\" is not #rrggbb.");
        return {std::stoi(t.substr(0, 2), nullptr, 16), std::stoi(t.substr(2, 2), nullptr, 16), std::stoi(t.substr(4, 2), nullptr, 16)};
    }
    inline int Distance(const Rgb& a, const uint8_t* p) { return std::max({std::abs(a[0] - p[0]), std::abs(a[1] - p[1]), std::abs(a[2] - p[2])}); }
    inline int Distance(const Rgb& a, const Rgb& b) { return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])}); }
    // Sampling step so at most about 4 million pixels are read.
    inline int SampleStep(const Image::Rgba& image)
    {
        const double pixels = static_cast<double>(image.width) * static_cast<double>(image.height);
        return std::max(1, static_cast<int>(std::ceil(std::sqrt(pixels / 4.0e6))));
    }
    struct Swatch { Rgb rgb{}; double share = 0; size_t pixels = 0; };
    // The most common colours of the opaque pixels, most common first: colours
    // are binned at 5 bits a channel, then bins within 24 levels of a bigger
    // one are merged into it. share is of all opaque pixels.
    inline std::vector<Swatch> Palette(const Image::Rgba& image, size_t count)
    {
        struct Bin { double r = 0, g = 0, b = 0; size_t n = 0; };
        std::unordered_map<uint32_t, Bin> bins;
        const int step = SampleStep(image);
        size_t opaque = 0;
        for (int y = 0; y < image.height; y += step)
            for (int x = 0; x < image.width; x += step)
            {
                const uint8_t* p = image.At(x, y);
                if (p[3] < 128) continue;
                ++opaque;
                auto& bin = bins[(static_cast<uint32_t>(p[0] >> 3) << 10) | (static_cast<uint32_t>(p[1] >> 3) << 5) | static_cast<uint32_t>(p[2] >> 3)];
                bin.r += p[0]; bin.g += p[1]; bin.b += p[2]; ++bin.n;
            }
        std::vector<Bin> sorted;
        sorted.reserve(bins.size());
        for (const auto& entry : bins) sorted.push_back(entry.second);
        std::sort(sorted.begin(), sorted.end(), [](const Bin& a, const Bin& b) { return a.n != b.n ? a.n > b.n : a.r + a.g + a.b < b.r + b.g + b.b; });
        std::vector<Swatch> out;
        std::vector<Bin> merged;
        for (const auto& bin : sorted)
        {
            const Rgb colour{static_cast<int>(std::lround(bin.r / static_cast<double>(bin.n))), static_cast<int>(std::lround(bin.g / static_cast<double>(bin.n))), static_cast<int>(std::lround(bin.b / static_cast<double>(bin.n)))};
            bool joined = false;
            for (auto& m : merged)
            {
                const Rgb mc{static_cast<int>(std::lround(m.r / static_cast<double>(m.n))), static_cast<int>(std::lround(m.g / static_cast<double>(m.n))), static_cast<int>(std::lround(m.b / static_cast<double>(m.n)))};
                if (Distance(mc, colour) <= 24) { m.r += bin.r; m.g += bin.g; m.b += bin.b; m.n += bin.n; joined = true; break; }
            }
            if (!joined) merged.push_back(bin);
            if (merged.size() > count * 4 + 64) break; // the rest are too rare to list
        }
        std::stable_sort(merged.begin(), merged.end(), [](const Bin& a, const Bin& b) { return a.n > b.n; });
        const double scale = static_cast<double>(step) * step;
        for (const auto& m : merged)
        {
            if (out.size() >= count) break;
            Swatch s;
            s.rgb = {static_cast<int>(std::lround(m.r / static_cast<double>(m.n))), static_cast<int>(std::lround(m.g / static_cast<double>(m.n))), static_cast<int>(std::lround(m.b / static_cast<double>(m.n)))};
            s.share = opaque ? static_cast<double>(m.n) / static_cast<double>(opaque) : 0;
            s.pixels = static_cast<size_t>(static_cast<double>(m.n) * scale);
            out.push_back(s);
        }
        return out;
    }
    inline double Round(double v, double places = 4)
    {
        const double k = std::pow(10.0, places);
        return std::round(v * k) / k;
    }
    // A measurement grid: lines every spacing source pixels from origin (in
    // the source image's pixels, before any crop). With uuPerPx the labels are
    // Unreal units, else pixels; both count from the origin, x rightwards and
    // y downwards as in the image.
    struct GridSpec
    {
        double spacing = 0;
        double uuPerPx = 0;
        std::array<double, 2> origin{0, 0};
        bool labels = true;
    };
    namespace Detail
    {
        inline std::string Number(double v)
        {
            if (std::abs(v - std::round(v)) < 1e-6) return std::to_string(static_cast<long long>(std::llround(v)));
            char text[32];
            std::snprintf(text, sizeof text, "%.1f", v);
            return text;
        }
        // The grid line numbers k (lines at origin + k * spacing) whose lines
        // fall in [from, to) source pixels.
        inline std::pair<long long, long long> Lines(double origin, double spacing, double from, double to)
        {
            return {static_cast<long long>(std::ceil((from - origin) / spacing - 1e-9)), static_cast<long long>(std::floor((to - origin) / spacing - 1e-9))};
        }
        inline long long Mod(long long a, long long b) { return ((a % b) + b) % b; }
    }
    // Draws the grid on shown, which is the source region starting at offset
    // (source pixels) scaled by scale (shown pixels per source pixel). Returns
    // what was drawn: spacing, unit, the value per line, which lines were drawn
    // and labelled, and the visible line range.
    inline Json DrawGrid(Image::Rgba& shown, double scale, std::array<double, 2> offset, const GridSpec& grid)
    {
        using namespace Detail;
        if (!(grid.spacing > 0) || !std::isfinite(grid.spacing)) throw std::runtime_error("The grid spacing must be more than 0 pixels.");
        if (!(scale > 0)) throw std::runtime_error("The image scale must be more than 0.");
        const double shownSpacing = grid.spacing * scale;
        const double viewW = shown.width / scale, viewH = shown.height / scale;
        const auto xs = Lines(grid.origin[0], grid.spacing, offset[0], offset[0] + viewW + 1e-9);
        const auto ys = Lines(grid.origin[1], grid.spacing, offset[1], offset[1] + viewH + 1e-9);
        const long long xCount = xs.second - xs.first + 1, yCount = ys.second - ys.first + 1;
        if (xCount > 4096 || yCount > 4096) throw std::runtime_error("The grid would have more than 4096 lines across; raise the spacing, or crop.");
        // Every m-th line when lines would be closer than 4 pixels.
        long long every = 1;
        while (shownSpacing * static_cast<double>(every) < 4) every *= 2;
        const bool uu = grid.uuPerPx > 0;
        const double unitPerLine = uu ? grid.spacing * grid.uuPerPx : grid.spacing;
        // Labels need room for the widest one.
        long long widest = 1;
        for (long long k : {xs.first, xs.second, ys.first, ys.second}) widest = std::max<long long>(widest, static_cast<long long>(Number(static_cast<double>(k) * unitPerLine).size()));
        const double labelRoom = static_cast<double>(widest) * Render::Font::kAdvance + 8;
        long long labelEvery = every;
        while (shownSpacing * static_cast<double>(labelEvery) < labelRoom) labelEvery += every;
        Render::Canvas canvas(1, 1, {0, 0, 0, 255});
        canvas.image = std::move(shown);
        const Render::Colour minor{255, 0, 255, 110}, major{255, 0, 255, 200}, zero{0, 210, 255, 230}, ink{255, 255, 255, 255}, halo{0, 0, 0, 255};
        auto colourOf = [&](long long k) { return k == 0 ? zero : Mod(k, 5 * every) == 0 ? major : minor; };
        for (long long k = xs.first; k <= xs.second; ++k)
        {
            if (Mod(k, every)) continue;
            const double x = (grid.origin[0] + static_cast<double>(k) * grid.spacing - offset[0]) * scale;
            canvas.FillRect(x, 0, x + 1, canvas.Height(), colourOf(k)); // the pixel holding x
        }
        for (long long k = ys.first; k <= ys.second; ++k)
        {
            if (Mod(k, every)) continue;
            const double y = (grid.origin[1] + static_cast<double>(k) * grid.spacing - offset[1]) * scale;
            canvas.FillRect(0, y, canvas.Width(), y + 1, colourOf(k));
        }
        if (grid.labels)
        {
            for (long long k = xs.first; k <= xs.second; ++k)
            {
                if (Mod(k, labelEvery)) continue;
                const auto text = Number(static_cast<double>(k) * unitPerLine);
                const int x = static_cast<int>(std::lround((grid.origin[0] + static_cast<double>(k) * grid.spacing - offset[0]) * scale)) + 2;
                canvas.Text(std::min(x, canvas.Width() - Render::Canvas::TextWidth(text, 1) - 1), 2, text, ink, 1, halo);
            }
            for (long long k = ys.first; k <= ys.second; ++k)
            {
                if (Mod(k, labelEvery)) continue;
                const auto text = Number(static_cast<double>(k) * unitPerLine);
                const int y = static_cast<int>(std::lround((grid.origin[1] + static_cast<double>(k) * grid.spacing - offset[1]) * scale)) + 2;
                canvas.Text(2, std::min(y, canvas.Height() - Render::Font::kHeight - 1), text, ink, 1, halo);
            }
        }
        shown = std::move(canvas.image);
        return {{"spacingPx", Round(grid.spacing)}, {"shownSpacingPx", Round(shownSpacing, 2)}, {"unit", uu ? "uu" : "px"}, {"perLine", Round(unitPerLine)},
                {"uuPerPx", uu ? Json(Round(grid.uuPerPx, 6)) : Json()}, {"origin", {Round(grid.origin[0], 2), Round(grid.origin[1], 2)}},
                {"drawnEvery", every}, {"labelledEvery", grid.labels ? Json(labelEvery) : Json()},
                {"columns", {{"first", xs.first}, {"last", xs.second}}}, {"rows", {{"first", ys.first}, {"last", ys.second}}}};
    }
    // Ink is any opaque pixel more than tolerance from the background in some
    // channel. Returns the shares of ink, transparent, dark (luma < 64) and
    // light (luma > 192) pixels and the ink's bounding box in source pixels.
    inline Json Occupancy(const Image::Rgba& image, const Rgb& background, int tolerance, std::array<double, 2> offset)
    {
        const int step = SampleStep(image);
        size_t total = 0, ink = 0, transparent = 0, dark = 0, light = 0;
        int x0 = image.width, y0 = image.height, x1 = -1, y1 = -1;
        for (int y = 0; y < image.height; y += step)
            for (int x = 0; x < image.width; x += step)
            {
                const uint8_t* p = image.At(x, y);
                ++total;
                if (p[3] < 128) { ++transparent; continue; }
                const int luma = (54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8;
                dark += luma < 64;
                light += luma > 192;
                if (Distance(background, p) <= tolerance) continue;
                ++ink;
                x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
        const double n = total ? static_cast<double>(total) : 1.0;
        Json out{{"background", Hex(background)}, {"tolerance", tolerance}, {"ink", Round(static_cast<double>(ink) / n)}, {"transparent", Round(static_cast<double>(transparent) / n)},
                 {"dark", Round(static_cast<double>(dark) / n)}, {"light", Round(static_cast<double>(light) / n)}, {"sampledEvery", step}};
        if (x1 >= 0)
            out["inkBox"] = {{"x", Round(offset[0] + x0, 1)}, {"y", Round(offset[1] + y0, 1)}, {"width", std::min(image.width - x0, x1 - x0 + step)}, {"height", std::min(image.height - y0, y1 - y0 + step)}};
        else out["inkBox"] = Json();
        return out;
    }
    // How much ink each grid cell holds, as rows of characters top to bottom:
    // '.' under 5%, '1'-'9' tenths, '#' 95% or more. Cells are the squares
    // between grid lines (edge cells partly outside the image count what is in).
    inline Json CellOccupancy(const Image::Rgba& image, const Rgb& background, int tolerance, std::array<double, 2> offset, const GridSpec& grid, size_t maxCells = 160)
    {
        using namespace Detail;
        // Cell k spans [origin + k * spacing, origin + (k + 1) * spacing).
        auto cellsOver = [&](double origin, double from, double length) -> std::pair<long long, long long>
        {
            return {static_cast<long long>(std::floor((from - origin) / grid.spacing + 1e-9)), static_cast<long long>(std::ceil((from + length - origin) / grid.spacing - 1e-9)) - 1};
        };
        const auto cx = cellsOver(grid.origin[0], offset[0], image.width);
        const auto cy = cellsOver(grid.origin[1], offset[1], image.height);
        const long long columns = cx.second - cx.first + 1, rows = cy.second - cy.first + 1;
        if (columns <= 0 || rows <= 0) return Json();
        if (columns > static_cast<long long>(maxCells) || rows > static_cast<long long>(maxCells))
            return {{"skipped", "The grid has " + std::to_string(columns) + " x " + std::to_string(rows) + " cells; cell occupancy is given up to " + std::to_string(maxCells) + " a side (raise the spacing or crop)."}};
        std::vector<size_t> inkCount(static_cast<size_t>(columns * rows), 0), totalCount(static_cast<size_t>(columns * rows), 0);
        const int step = SampleStep(image);
        for (int y = 0; y < image.height; y += step)
        {
            const long long row = static_cast<long long>(std::floor((offset[1] + y + 0.5 - grid.origin[1]) / grid.spacing)) - cy.first;
            if (row < 0 || row >= rows) continue;
            for (int x = 0; x < image.width; x += step)
            {
                const long long column = static_cast<long long>(std::floor((offset[0] + x + 0.5 - grid.origin[0]) / grid.spacing)) - cx.first;
                if (column < 0 || column >= columns) continue;
                const size_t at = static_cast<size_t>(row * columns + column);
                ++totalCount[at];
                const uint8_t* p = image.At(x, y);
                if (p[3] >= 128 && Distance(background, p) > tolerance) ++inkCount[at];
            }
        }
        Json lines = Json::array();
        for (long long r = 0; r < rows; ++r)
        {
            std::string line;
            for (long long c = 0; c < columns; ++c)
            {
                const size_t at = static_cast<size_t>(r * columns + c);
                if (!totalCount[at]) { line += ' '; continue; }
                const double f = static_cast<double>(inkCount[at]) / static_cast<double>(totalCount[at]);
                line += f < 0.05 ? '.' : f >= 0.95 ? '#' : static_cast<char>('0' + std::clamp(static_cast<int>(std::lround(f * 10)), 1, 9));
            }
            lines.push_back(line);
        }
        return {{"columns", columns}, {"rows", rows}, {"firstColumn", cx.first}, {"firstRow", cy.first},
                {"legend", "One character per grid cell, rows top to bottom: '.' under 5% ink, 1-9 tenths of the cell, '#' 95% or more; ink is anything not the background colour."},
                {"cells", lines}};
    }

    // --- Texture-ready images (image_convert, assets_import) -----------------
    inline bool PowerOfTwo(int n) { return n > 0 && (n & (n - 1)) == 0; }
    // The image made exactly width x height: stretch (distorts), pad (fits
    // inside, the rest background or transparent), crop (fills, the overflow
    // cut off evenly). Without alpha every pixel is made opaque over background.
    inline Image::Rgba Fit(const Image::Rgba& source, int width, int height, const std::string& fit, bool alpha, const Rgb& background)
    {
        if (source.Empty()) throw std::runtime_error("The image is empty.");
        if (fit != "pad" && fit != "crop" && fit != "stretch") throw std::runtime_error("fit is pad, crop or stretch.");
        auto resize = [&](int w, int h)
        {
            const bool up = w > source.width && h > source.height;
            return Image::Resize(source, w, h, up ? Image::Filter::Bilinear : Image::Filter::Box);
        };
        const std::array<uint8_t, 4> fill{static_cast<uint8_t>(background[0]), static_cast<uint8_t>(background[1]), static_cast<uint8_t>(background[2]), static_cast<uint8_t>(alpha ? 0 : 255)};
        Image::Rgba out;
        if (fit == "stretch") out = resize(width, height);
        else
        {
            const double sx = static_cast<double>(width) / source.width, sy = static_cast<double>(height) / source.height;
            const double s = fit == "pad" ? std::min(sx, sy) : std::max(sx, sy);
            const int w = std::max(1, static_cast<int>(std::lround(source.width * s))), h = std::max(1, static_cast<int>(std::lround(source.height * s)));
            const auto scaled = resize(w, h);
            out = Image::Rgba(width, height, fill);
            const int ox = (width - w) / 2, oy = (height - h) / 2;
            for (int y = 0; y < height; ++y)
            {
                const int from = y - oy;
                if (from < 0 || from >= h) continue;
                const int x0 = std::max(0, ox), x1 = std::min(width, ox + w);
                if (x1 > x0) std::memcpy(out.At(x0, y), scaled.At(x0 - ox, from), static_cast<size_t>(x1 - x0) * 4);
            }
        }
        if (!alpha)
            for (size_t i = 0; i < out.pixels.size(); i += 4)
            {
                const int a = out.pixels[i + 3];
                for (size_t c = 0; c < 3; ++c) out.pixels[i + c] = static_cast<uint8_t>((out.pixels[i + c] * a + background[c] * (255 - a) + 127) / 255);
                out.pixels[i + 3] = 255;
            }
        return out;
    }

    // --- OBJ to ASE (assets_import) ------------------------------------------
    // A Wavefront OBJ as a 3ds Max ASE file, which the editor's static mesh
    // importer reads: one object, faces fan-triangulated, texture coordinates
    // kept (a single 0,0 when the file has none), one material per usemtl (a
    // Multi/Sub-Object material when there are several, each face's MTLID its
    // material), OBJ smoothing groups kept, map_Kd of options.mtl as each
    // material's diffuse bitmap.
    // Axes as Importers/Obj.h reads them: options.up "y" (default; Unreal
    // gets (x, z, y)) or "z" (Unreal (x, -y, z)); ASE is Z-up and the editor
    // flips its Y, so the ASE holds (x, -z, y) or (x, y, z), times
    // options.scale (default 1). options.name names the object.
    // info: vertices, uvs, triangles, materials, groups, skippedFaces, and the
    // mesh's bounds and size in Unreal units.
    struct Ase { std::string text; Json info; };
    inline Ase ObjToAse(const std::string& obj, const Json& options = Json::object())
    {
        using Vec = std::array<double, 3>;
        const auto get = [&](const char* key, const Json& fallback) { return options.is_object() && options.contains(key) && !options.at(key).is_null() ? options.at(key) : fallback; };
        const auto up = Lower(get("up", "y").is_string() ? get("up", "y").get<std::string>() : std::string("?"));
        if (up != "y" && up != "z") throw std::runtime_error("up is y (most OBJ files) or z.");
        const auto scaleValue = get("scale", 1.0);
        if (!scaleValue.is_number() || !(scaleValue.get<double>() > 1e-6) || scaleValue.get<double>() > 1e6) throw std::runtime_error("scale must be a number above 0 (such as 100 for an OBJ in metres).");
        const double scale = scaleValue.get<double>();
        auto name = get("name", "Mesh").is_string() ? get("name", "Mesh").get<std::string>() : std::string("Mesh");
        for (auto& c : name) if (c == '"' || static_cast<unsigned char>(c) < 0x20) c = '_';
        // map_Kd per material, from the .mtl text when given.
        std::map<std::string, std::string> bitmaps;
        if (options.is_object() && options.contains("mtl") && options.at("mtl").is_string())
        {
            std::istringstream mtl(options.at("mtl").get<std::string>());
            std::string line, current;
            while (std::getline(mtl, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto first = line.find_first_not_of(" \t");
                if (first == std::string::npos) continue;
                line = line.substr(first);
                const auto space = line.find_first_of(" \t");
                const auto key = line.substr(0, space);
                const auto value = space == std::string::npos ? std::string::npos : line.find_first_not_of(" \t", space);
                auto rest = value == std::string::npos ? std::string() : line.substr(value);
                while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.pop_back();
                if (key == "newmtl") current = rest;
                else if (key == "map_Kd" && !current.empty())
                {
                    // Options such as -s 1 1 1 come before the file name, which is last.
                    const auto last = rest.find_last_of(" \t");
                    bitmaps[current] = last == std::string::npos ? rest : rest.substr(last + 1);
                }
            }
        }
        std::vector<Vec> positions;
        std::vector<std::array<double, 2>> uvs;
        struct Corner { size_t v = 0; long long t = -1; };
        struct Triangle { Corner c[3]; size_t material = 0; int smoothing = 0; };
        std::vector<Triangle> triangles;
        std::vector<std::string> materials, groups;
        size_t material = 0, skipped = 0;
        int smoothing = 0;
        bool anyMaterial = false;
        auto materialIndex = [&](const std::string& m)
        {
            const auto found = std::find(materials.begin(), materials.end(), m);
            if (found != materials.end()) return static_cast<size_t>(found - materials.begin());
            materials.push_back(m);
            return materials.size() - 1;
        };
        auto number = [](const std::string& text, double& out)
        {
            if (text.empty()) return false;
            char* end = nullptr;
            out = std::strtod(text.c_str(), &end);
            return end && *end == '\0' && std::isfinite(out);
        };
        std::istringstream in(obj);
        std::string raw;
        size_t lineNumber = 0;
        while (std::getline(in, raw))
        {
            ++lineNumber;
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            std::istringstream words(raw);
            std::string key;
            if (!(words >> key) || key[0] == '#') continue;
            std::vector<std::string> args;
            for (std::string w; words >> w;) args.push_back(w);
            if (key == "v")
            {
                double x = 0, y = 0, z = 0;
                if (args.size() < 3 || !number(args[0], x) || !number(args[1], y) || !number(args[2], z))
                    throw std::runtime_error("OBJ line " + std::to_string(lineNumber) + ": a vertex needs x, y and z.");
                positions.push_back({x, y, z});
            }
            else if (key == "vt")
            {
                double u = 0, v = 0;
                if (args.empty() || !number(args[0], u) || (args.size() > 1 && !number(args[1], v)))
                    throw std::runtime_error("OBJ line " + std::to_string(lineNumber) + ": a texture coordinate needs u and v.");
                uvs.push_back({u, v});
            }
            else if (key == "f")
            {
                std::vector<Corner> corners;
                bool ok = args.size() >= 3;
                for (const auto& a : args)
                {
                    const auto slash = a.find('/');
                    double vi = 0;
                    if (!number(a.substr(0, slash), vi) || vi == 0 || vi != std::floor(vi)) { ok = false; break; }
                    const double resolved = vi < 0 ? static_cast<double>(positions.size()) + vi + 1 : vi;
                    if (resolved < 1 || resolved > static_cast<double>(positions.size())) { ok = false; break; }
                    Corner corner;
                    corner.v = static_cast<size_t>(resolved) - 1;
                    if (slash != std::string::npos)
                    {
                        const auto second = a.find('/', slash + 1);
                        const auto t = a.substr(slash + 1, second == std::string::npos ? std::string::npos : second - slash - 1);
                        double ti = 0;
                        if (!t.empty())
                        {
                            if (!number(t, ti) || ti == 0 || ti != std::floor(ti)) { ok = false; break; }
                            const double tr = ti < 0 ? static_cast<double>(uvs.size()) + ti + 1 : ti;
                            if (tr < 1 || tr > static_cast<double>(uvs.size())) { ok = false; break; }
                            corner.t = static_cast<long long>(tr) - 1;
                        }
                    }
                    corners.push_back(corner);
                }
                if (!ok) { ++skipped; continue; }
                if (!anyMaterial) { material = materialIndex("Default"); anyMaterial = true; }
                for (size_t i = 1; i + 1 < corners.size(); ++i) triangles.push_back({{corners[0], corners[i], corners[i + 1]}, material, smoothing});
            }
            else if (key == "usemtl" && !args.empty()) { material = materialIndex(args[0]); anyMaterial = true; }
            else if (key == "s") smoothing = args.empty() || args[0] == "off" ? 0 : std::clamp(std::atoi(args[0].c_str()), 0, 32);
            else if ((key == "o" || key == "g") && !args.empty() && std::find(groups.begin(), groups.end(), args[0]) == groups.end()) groups.push_back(args[0]);
        }
        if (triangles.empty()) throw std::runtime_error("The OBJ has no faces the importer could use" + std::string(skipped ? " (" + std::to_string(skipped) + " faces name vertices it does not have)" : "") + ".");
        auto toAse = [&](const Vec& p) { return up == "y" ? Vec{p[0] * scale, -p[2] * scale, p[1] * scale} : Vec{p[0] * scale, p[1] * scale, p[2] * scale}; };
        std::ostringstream o;
        o << std::fixed << std::setprecision(4);
        auto vec = [&](const Vec& v) { o << v[0] << '\t' << v[1] << '\t' << v[2]; };
        o << "*3DSMAX_ASCIIEXPORT\t200\n*COMMENT \"Written by Reloaded.Editor.Mcp from an OBJ\"\n";
        o << "*SCENE {\n\t*SCENE_FILENAME \"" << name << ".obj\"\n\t*SCENE_FIRSTFRAME 0\n\t*SCENE_LASTFRAME 100\n\t*SCENE_FRAMESPEED 30\n\t*SCENE_TICKSPERFRAME 160\n"
          << "\t*SCENE_BACKGROUND_STATIC 0.0000\t0.0000\t0.0000\n\t*SCENE_AMBIENT_STATIC 0.0000\t0.0000\t0.0000\n}\n";
        auto writeMaterial = [&](const std::string& indent, const std::string& tag, size_t index, const std::string& materialName)
        {
            o << indent << tag << ' ' << index << " {\n";
            o << indent << "\t*MATERIAL_NAME \"" << materialName << "\"\n" << indent << "\t*MATERIAL_CLASS \"Standard\"\n";
            o << indent << "\t*MATERIAL_AMBIENT 0.5882\t0.5882\t0.5882\n" << indent << "\t*MATERIAL_DIFFUSE 0.5882\t0.5882\t0.5882\n" << indent << "\t*MATERIAL_SPECULAR 0.9000\t0.9000\t0.9000\n";
            o << indent << "\t*MATERIAL_SHINE 0.1000\n" << indent << "\t*MATERIAL_SHINESTRENGTH 0.0000\n" << indent << "\t*MATERIAL_TRANSPARENCY 0.0000\n" << indent << "\t*MATERIAL_WIRESIZE 1.0000\n";
            o << indent << "\t*MATERIAL_SHADING Blinn\n" << indent << "\t*MATERIAL_XP_FALLOFF 0.0000\n" << indent << "\t*MATERIAL_SELFILLUM 0.0000\n" << indent << "\t*MATERIAL_FALLOFF In\n" << indent << "\t*MATERIAL_XP_TYPE Filter\n";
            const auto bitmap = bitmaps.find(materialName);
            if (bitmap != bitmaps.end())
                o << indent << "\t*MAP_DIFFUSE {\n" << indent << "\t\t*MAP_NAME \"" << materialName << "\"\n" << indent << "\t\t*MAP_CLASS \"Bitmap\"\n" << indent << "\t\t*MAP_SUBNO 1\n"
                  << indent << "\t\t*MAP_AMOUNT 1.0000\n" << indent << "\t\t*BITMAP \"" << bitmap->second << "\"\n" << indent << "\t\t*MAP_TYPE Screen\n" << indent << "\t\t*UVW_U_OFFSET 0.0000\n"
                  << indent << "\t\t*UVW_V_OFFSET 0.0000\n" << indent << "\t\t*UVW_U_TILING 1.0000\n" << indent << "\t\t*UVW_V_TILING 1.0000\n" << indent << "\t\t*UVW_ANGLE 0.0000\n"
                  << indent << "\t\t*UVW_BLUR 1.0000\n" << indent << "\t\t*UVW_BLUR_OFFSET 0.0000\n" << indent << "\t\t*UVW_NOUSE_AMT 1.0000\n" << indent << "\t\t*UVW_NOISE_SIZE 1.0000\n"
                  << indent << "\t\t*UVW_NOISE_LEVEL 1\n" << indent << "\t\t*UVW_NOISE_PHASE 0.0000\n" << indent << "\t\t*BITMAP_FILTER Pyramidal\n" << indent << "\t}\n";
            o << indent << "}\n";
        };
        o << "*MATERIAL_LIST {\n\t*MATERIAL_COUNT 1\n";
        if (materials.size() <= 1) writeMaterial("\t", "*MATERIAL", 0, materials.empty() ? std::string("Default") : materials[0]);
        else
        {
            o << "\t*MATERIAL 0 {\n\t\t*MATERIAL_NAME \"" << name << "_Multi\"\n\t\t*MATERIAL_CLASS \"Multi/Sub-Object\"\n";
            o << "\t\t*MATERIAL_AMBIENT 0.5882\t0.5882\t0.5882\n\t\t*MATERIAL_DIFFUSE 0.5882\t0.5882\t0.5882\n\t\t*MATERIAL_SPECULAR 0.9000\t0.9000\t0.9000\n";
            o << "\t\t*MATERIAL_SHINE 0.1000\n\t\t*MATERIAL_SHINESTRENGTH 0.0000\n\t\t*MATERIAL_TRANSPARENCY 0.0000\n\t\t*MATERIAL_WIRESIZE 1.0000\n";
            o << "\t\t*NUMSUBMTLS " << materials.size() << "\n";
            for (size_t i = 0; i < materials.size(); ++i) writeMaterial("\t\t", "*SUBMATERIAL", i, materials[i]);
            o << "\t}\n";
        }
        o << "}\n";
        o << "*GEOMOBJECT {\n\t*NODE_NAME \"" << name << "\"\n\t*NODE_TM {\n\t\t*NODE_NAME \"" << name << "\"\n";
        o << "\t\t*INHERIT_POS 0 0 0\n\t\t*INHERIT_ROT 0 0 0\n\t\t*INHERIT_SCL 0 0 0\n";
        o << "\t\t*TM_ROW0 1.0000\t0.0000\t0.0000\n\t\t*TM_ROW1 0.0000\t1.0000\t0.0000\n\t\t*TM_ROW2 0.0000\t0.0000\t1.0000\n\t\t*TM_ROW3 0.0000\t0.0000\t0.0000\n";
        o << "\t\t*TM_POS 0.0000\t0.0000\t0.0000\n\t\t*TM_ROTAXIS 0.0000\t0.0000\t0.0000\n\t\t*TM_ROTANGLE 0.0000\n\t\t*TM_SCALE 1.0000\t1.0000\t1.0000\n";
        o << "\t\t*TM_SCALEAXIS 0.0000\t0.0000\t0.0000\n\t\t*TM_SCALEAXISANG 0.0000\n\t}\n";
        o << "\t*MESH {\n\t\t*TIMEVALUE 0\n\t\t*MESH_NUMVERTEX " << positions.size() << "\n\t\t*MESH_NUMFACES " << triangles.size() << "\n\t\t*MESH_VERTEX_LIST {\n";
        Vec lo{}, hi{};
        for (size_t i = 0; i < positions.size(); ++i)
        {
            const auto p = toAse(positions[i]);
            const Vec unreal{p[0], -p[1], p[2]};
            for (size_t k = 0; k < 3; ++k)
            {
                lo[k] = i ? std::min(lo[k], unreal[k]) : unreal[k];
                hi[k] = i ? std::max(hi[k], unreal[k]) : unreal[k];
            }
            o << "\t\t\t*MESH_VERTEX " << std::setw(5) << i << '\t';
            vec(p);
            o << '\n';
        }
        o << "\t\t}\n\t\t*MESH_FACE_LIST {\n";
        for (size_t i = 0; i < triangles.size(); ++i)
        {
            const auto& t = triangles[i];
            o << "\t\t\t*MESH_FACE " << std::setw(5) << i << ":    A: " << std::setw(5) << t.c[0].v << " B: " << std::setw(5) << t.c[1].v << " C: " << std::setw(5) << t.c[2].v
              << " AB:    1 BC:    1 CA:    1\t *MESH_SMOOTHING " << t.smoothing << " \t*MESH_MTLID " << t.material << '\n';
        }
        o << "\t\t}\n";
        const bool haveUvs = !uvs.empty();
        o << "\t\t*MESH_NUMTVERTEX " << (haveUvs ? uvs.size() : 1) << "\n\t\t*MESH_TVERTLIST {\n";
        if (haveUvs)
            for (size_t i = 0; i < uvs.size(); ++i) o << "\t\t\t*MESH_TVERT " << i << '\t' << uvs[i][0] << '\t' << uvs[i][1] << "\t0.0000\n";
        else o << "\t\t\t*MESH_TVERT 0\t0.0000\t0.0000\t0.0000\n";
        o << "\t\t}\n\t\t*MESH_NUMTVFACES " << triangles.size() << "\n\t\t*MESH_TFACELIST {\n";
        size_t missingUvs = 0;
        for (size_t i = 0; i < triangles.size(); ++i)
        {
            o << "\t\t\t*MESH_TFACE " << i;
            for (const auto& corner : triangles[i].c)
            {
                if (corner.t < 0) ++missingUvs;
                o << '\t' << (corner.t < 0 ? 0 : corner.t);
            }
            o << '\n';
        }
        o << "\t\t}\n\t}\n\t*PROP_MOTIONBLUR 0\n\t*PROP_CASTSHADOW 1\n\t*PROP_RECVSHADOW 1\n\t*MATERIAL_REF 0\n}\n";
        Json warnings = Json::array();
        if (skipped) warnings.push_back(std::to_string(skipped) + (skipped == 1 ? " face names vertices or texture coordinates the file does not have (or has fewer than 3 corners) and was left out."
                                                                          : " faces name vertices or texture coordinates the file does not have (or have fewer than 3 corners) and were left out."));
        if (!haveUvs) warnings.push_back("The OBJ has no texture coordinates; every corner maps to 0,0, so a texture shows as one colour until the mesh is unwrapped.");
        else if (missingUvs) warnings.push_back(std::to_string(missingUvs) + (missingUvs == 1 ? " face corner has" : " face corners have") + " no texture coordinate and maps to the first one.");
        const Vec size{hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
        const double longest = std::max({size[0], size[1], size[2]});
        if (longest < 8) warnings.push_back("The mesh is only " + Detail::Number(longest) + " units across in the editor (a player stands 180); if the OBJ is in metres, pass scale 100.");
        if (longest > 65536) warnings.push_back("The mesh is over 65,536 units across; check the OBJ's units and scale.");
        if (positions.size() > 65535 || triangles.size() > 21845) warnings.push_back("The mesh is large for the engine (65,535 vertices a section); split it if the import fails.");
        auto round3 = [](const Vec& v) { return Json::array({Round(v[0], 3), Round(v[1], 3), Round(v[2], 3)}); };
        Json info{{"vertices", positions.size()}, {"uvs", uvs.size()}, {"triangles", triangles.size()}, {"materials", materials}, {"groups", groups}, {"skippedFaces", skipped},
                  {"up", up}, {"scale", scale}, {"unrealBounds", {{"min", round3(lo)}, {"max", round3(hi)}}}, {"size", round3(size)}, {"warnings", warnings}};
        if (!bitmaps.empty())
        {
            Json maps = Json::object();
            for (const auto& m : materials) if (bitmaps.count(m)) maps[m] = bitmaps[m];
            info["bitmaps"] = maps;
        }
        return {o.str(), info};
    }
}
