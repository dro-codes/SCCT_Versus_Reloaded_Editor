// Media.h's platform parts: WIC and Media Foundation on Windows, ImageCodec.h
// and an ffmpeg child process everywhere.
#include "Media.h"
#include <exception>
#include <thread>
#include <tuple>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
#ifdef _MSC_VER
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#endif
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace Mcp::Media
{
namespace
{
    // What the progress callback throws (the request was cancelled) is the
    // caller's own: carried past the decoders' error handling, so it is never
    // taken for a video Media Foundation cannot read, and ExtractFrames
    // throws it again as it was.
    struct Interrupted { std::exception_ptr cause; };
    void Report(const FrameOptions& o, double done, double total, const std::string& message)
    {
        if (!o.progress) return;
        try { o.progress(done, total, message); }
        catch (...) { throw Interrupted{std::current_exception()}; }
    }
    // The last lines of a log, for an error message.
    std::string Tail(const std::string& log, size_t lines = 4)
    {
        auto end = log.find_last_not_of("\r\n ");
        if (end == std::string::npos) return {};
        size_t start = end + 1, found = 0;
        while (start > 0 && found < lines)
        {
            const auto newline = log.rfind('\n', start - 1);
            if (newline == std::string::npos) { start = 0; break; }
            start = newline;
            ++found;
        }
        auto text = log.substr(start, end + 1 - start);
        for (auto& c : text) if (c == '\r' || c == '\n') c = ' ';
        const auto first = text.find_first_not_of(' ');
        return first == std::string::npos ? std::string() : text.substr(first, 600);
    }
    std::string ReadLog(const fs::path& log)
    {
        try { return ReadFile(log, 4u * 1024 * 1024); }
        catch (const std::exception&) { return {}; }
    }
    void Remove(const fs::path& path)
    {
        std::error_code ignored;
        fs::remove(path, ignored);
    }
    // A finished frame: fitted, written as PNG, recorded.
    Frame Save(const Image::Rgba& picture, double at, size_t number, const FrameOptions& o)
    {
        const auto fitted = Image::FitWithin(picture, o.maxEdge, Image::Filter::Box);
        Frame frame;
        frame.path = o.outDir / FrameName(number, at);
        frame.time = at;
        frame.width = fitted.width;
        frame.height = fitted.height;
        WriteFile(frame.path, PngBytes(fitted));
        return frame;
    }
    std::string NoBackend(const std::string& mediaFoundation)
    {
#ifdef _WIN32
        const std::string first = mediaFoundation.empty() ? "Media Foundation was not used" : "Media Foundation could not read the video (" + mediaFoundation + ")";
#else
        const std::string first = mediaFoundation.empty() ? "Media Foundation exists only on Windows" : mediaFoundation;
#endif
        return first + ", and no ffmpeg was found to read it instead. Install ffmpeg (ffmpeg.org) and set [Mcp] Ffmpeg=<full path to ffmpeg.exe> in "
                       "System\\Reloaded_Editor.ini, put it on PATH, or pass ffmpeg; or take the frames yourself and read them with media_read_image.";
    }

    // ------------------------------------------------------------ ffmpeg
    Extracted FfmpegFrames(const fs::path& ffmpeg, const fs::path& video, const FrameOptions& o)
    {
        Extracted out;
        out.backend = "ffmpeg";
        const auto log = o.outDir / "ffmpeg.log";
        // file: keeps ffmpeg from reading the name as another protocol's URL.
        const auto input = "file:" + Utf8(video);
        // ffmpeg with an input and no output describes the input and exits 1.
        RunProcess(ffmpeg, {"-hide_banner", "-nostdin", "-i", input}, log, 60000);
        const auto probe = ReadLog(log);
        if (probe.find("Video:") == std::string::npos)
        {
            Remove(log);
            throw std::runtime_error("ffmpeg found no video stream in " + Utf8(video) + (probe.empty() ? std::string(".") : ": " + Tail(probe)));
        }
        out.duration = FfmpegDuration(probe);
        std::tie(out.width, out.height) = FfmpegVideoSize(probe);
        const auto times = SampleTimes(o, out.duration, out.notes);
        if (o.mode != "scene")
        {
            for (size_t i = 0; i < times.size(); ++i)
            {
                Report(o, static_cast<double>(i), static_cast<double>(times.size()), "Frame " + std::to_string(i + 1) + " of " + std::to_string(times.size()) + " at " + TimeText(times[i]));
                const auto raw = o.outDir / ("raw-" + std::to_string(i + 1) + ".png");
                // Input seeking (-ss before -i) is exact and decodes only from the
                // keyframe before; scale applies the sample aspect ratio.
                const int code = RunProcess(ffmpeg, {"-hide_banner", "-nostdin", "-loglevel", "error", "-ss", Seconds(times[i]), "-i", input, "-an", "-frames:v", "1",
                                                     "-vf", "scale=iw*sar:ih,setsar=1", "-y", Utf8(raw)}, log, 120000);
                std::error_code error;
                if (!fs::exists(raw, error))
                {
                    if (code != 0) { const auto text = ReadLog(log); Remove(log); throw std::runtime_error("ffmpeg failed reading the frame at " + TimeText(times[i]) + ": " + Tail(text)); }
                    // No frame at or after this time: past the end of the stream.
                    out.notes.push_back("The video ends before " + TimeText(times[i]) + "; frames stop there.");
                    break;
                }
                const auto picture = DecodeImageFile(raw);
                Remove(raw);
                out.frames.push_back(Save(picture, times[i], out.frames.size() + 1, o));
            }
            Remove(log);
            return out;
        }
        // Scene: one run writes every candidate at the frame size (the fps
        // filter takes the frame at start + k / rate), then the histograms pick.
        const double end = times.size() > 1 ? times.back() + (times[1] - times[0]) : o.start + 1;
        const double span = end - o.start, rate = static_cast<double>(times.size()) / span;
        std::string scale = "scale=" + std::to_string(o.maxEdge) + ":" + std::to_string(o.maxEdge) + ":force_original_aspect_ratio=decrease";
        if (out.width > 0 && out.height > 0)
        {
            const double s = std::min(1.0, static_cast<double>(o.maxEdge) / std::max(out.width, out.height));
            scale = "scale=" + std::to_string(std::max(2L, std::lround(out.width * s))) + ":" + std::to_string(std::max(2L, std::lround(out.height * s)));
        }
        char rateText[32];
        std::snprintf(rateText, sizeof rateText, "%.6f", rate);
        Report(o, 0, static_cast<double>(times.size()), "Reading " + std::to_string(times.size()) + " candidate frames with ffmpeg");
        const auto pattern = o.outDir / "candidate-%04d.png";
        const int code = RunProcess(ffmpeg, {"-hide_banner", "-nostdin", "-loglevel", "error", "-ss", Seconds(o.start), "-t", Seconds(span), "-i", input, "-an",
                                             "-vf", std::string("fps=") + rateText + "," + scale + ",setsar=1", "-frames:v", std::to_string(times.size()), "-y", Utf8(pattern)},
                                    log, 600000);
        std::vector<fs::path> candidates;
        std::vector<Histogram> histograms;
        for (size_t i = 0; i < times.size(); ++i)
        {
            char name[32];
            std::snprintf(name, sizeof name, "candidate-%04u.png", static_cast<unsigned>(i + 1));
            const auto file = o.outDir / name;
            std::error_code error;
            if (!fs::exists(file, error)) break;
            candidates.push_back(file);
            histograms.push_back(LumaHistogram(DecodeImageFile(file)));
            Report(o, static_cast<double>(i + 1), static_cast<double>(times.size()), "Measured candidate " + std::to_string(i + 1));
        }
        if (candidates.empty())
        {
            const auto text = ReadLog(log);
            Remove(log);
            throw std::runtime_error("ffmpeg wrote no frames" + (code ? std::string(" (exit code ") + std::to_string(code) + ")" : std::string()) + (text.empty() ? std::string(".") : ": " + Tail(text)));
        }
        Remove(log);
        std::vector<double> change;
        const auto picks = PickScenes(histograms, static_cast<size_t>(o.count), o.threshold, change);
        std::vector<bool> keep(candidates.size(), false);
        for (const auto i : picks) keep[i] = true;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            if (!keep[i]) { Remove(candidates[i]); continue; }
            const auto picture = DecodeImageFile(candidates[i]);
            Remove(candidates[i]);
            auto frame = Save(picture, times[i], out.frames.size() + 1, o);
            frame.change = i ? Round(change[i], 3) : -1;
            out.frames.push_back(frame);
        }
        if (picks.size() < 2) out.notes.push_back("No cut reached the threshold (" + Detail::Number(o.threshold) + "); lower threshold to catch smaller changes.");
        return out;
    }

#ifdef _WIN32
    // ------------------------------------------------------------ COM
    template<class T> class Com
    {
    public:
        Com() = default;
        Com(const Com&) = delete;
        Com& operator=(const Com&) = delete;
        ~Com() { Reset(); }
        T** Put() { Reset(); return &pointer; }
        T* Get() const { return pointer; }
        T* operator->() const { return pointer; }
        explicit operator bool() const { return pointer != nullptr; }
        void Reset() { if (pointer) { pointer->Release(); pointer = nullptr; } }
    private:
        T* pointer = nullptr;
    };
    std::string HexCode(HRESULT hr)
    {
        char text[16];
        std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(hr));
        return text;
    }
    void Check(HRESULT hr, const std::string& what)
    {
        if (FAILED(hr)) throw std::runtime_error(what + " (" + HexCode(hr) + ").");
    }
    // COM on the calling thread (the server's tool worker), for the length of one call.
    class Apartment
    {
    public:
        Apartment()
        {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            // RPC_E_CHANGED_MODE: the thread already has an apartment, which serves too.
            if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) Check(hr, "COM could not start");
            owned = SUCCEEDED(hr);
        }
        Apartment(const Apartment&) = delete;
        Apartment& operator=(const Apartment&) = delete;
        ~Apartment() { if (owned) CoUninitialize(); }
    private:
        bool owned = false;
    };
    // WIC's version 1 factory, which every Windows since Vista has.
    // (CLSID_WICImagingFactory means version 2 when built for Windows 8 SDKs.)
    const GUID kWicFactory1 = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    void Factory(Com<IWICImagingFactory>& factory)
    {
        Check(CoCreateInstance(kWicFactory1, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.Put())), "Windows Imaging Component is not available");
    }

    Image::Rgba WicDecode(const fs::path& path)
    {
        Apartment apartment;
        Com<IWICImagingFactory> factory;
        Factory(factory);
        Com<IWICBitmapDecoder> decoder;
        const HRESULT opened = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.Put());
        if (opened == WINCODEC_ERR_COMPONENTNOTFOUND) throw std::runtime_error("Windows has no image codec for " + Utf8(path.filename()) + "; convert it to PNG first.");
        Check(opened, "Windows could not open the image " + Utf8(path));
        Com<IWICBitmapFrameDecode> frame;
        Check(decoder->GetFrame(0, frame.Put()), "Windows could not read the image " + Utf8(path));
        UINT width = 0, height = 0;
        Check(frame->GetSize(&width, &height), "Windows could not read the image's size");
        if (!width || !height || width > static_cast<UINT>(Image::kMaxEdge) || height > static_cast<UINT>(Image::kMaxEdge) ||
            static_cast<uint64_t>(width) * height > Image::kMaxPixels)
            throw std::runtime_error("The image is " + std::to_string(width) + "x" + std::to_string(height) + ", outside the supported " + std::to_string(Image::kMaxEdge) + " a side.");
        Com<IWICFormatConverter> converter;
        Check(factory->CreateFormatConverter(converter.Put()), "Windows could not convert the image");
        Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom), "Windows could not convert the image to RGBA");
        Image::Rgba image(static_cast<int>(width), static_cast<int>(height));
        Check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.pixels.size()), image.pixels.data()), "Windows could not read the image's pixels");
        for (size_t i = 0; i < image.pixels.size(); i += 4) std::swap(image.pixels[i], image.pixels[i + 2]);
        return image;
    }

    std::string WicPng(const Image::Rgba& image)
    {
        Apartment apartment;
        Com<IWICImagingFactory> factory;
        Factory(factory);
        Com<IStream> stream;
        *stream.Put() = SHCreateMemStream(nullptr, 0);
        if (!stream) throw std::runtime_error("Out of memory writing a PNG.");
        Com<IWICBitmapEncoder> encoder;
        Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put()), "Windows has no PNG encoder");
        Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Windows could not start a PNG");
        Com<IWICBitmapFrameEncode> frame;
        Com<IPropertyBag2> properties;
        Check(encoder->CreateNewFrame(frame.Put(), properties.Put()), "Windows could not start a PNG frame");
        Check(frame->Initialize(properties.Get()), "Windows could not start a PNG frame");
        Check(frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height)), "Windows could not size the PNG");
        const bool alpha = !image.Opaque();
        WICPixelFormatGUID format = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
        Check(frame->SetPixelFormat(&format), "Windows could not set the PNG's pixel format");
        if (!IsEqualGUID(format, alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR)) throw std::runtime_error("The PNG encoder wants another pixel format.");
        const size_t bpp = alpha ? 4 : 3, stride = (static_cast<size_t>(image.width) * bpp + 3) & ~static_cast<size_t>(3);
        std::vector<BYTE> pixels(stride * static_cast<size_t>(image.height));
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x)
            {
                const uint8_t* p = image.At(x, y);
                BYTE* q = &pixels[static_cast<size_t>(y) * stride + static_cast<size_t>(x) * bpp];
                q[0] = p[2]; q[1] = p[1]; q[2] = p[0];
                if (alpha) q[3] = p[3];
            }
        Check(frame->WritePixels(static_cast<UINT>(image.height), static_cast<UINT>(stride), static_cast<UINT>(pixels.size()), pixels.data()), "Windows could not write the PNG's pixels");
        Check(frame->Commit(), "Windows could not finish the PNG");
        Check(encoder->Commit(), "Windows could not finish the PNG");
        STATSTG stat{};
        Check(stream->Stat(&stat, STATFLAG_NONAME), "Windows could not measure the PNG");
        std::string bytes(static_cast<size_t>(stat.cbSize.QuadPart), '\0');
        LARGE_INTEGER zero{};
        Check(stream->Seek(zero, STREAM_SEEK_SET, nullptr), "Windows could not read the PNG back");
        ULONG read = 0;
        Check(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read), "Windows could not read the PNG back");
        bytes.resize(read);
        return bytes;
    }

    // ------------------------------------------------------------ Media Foundation
    // mfplat.dll and mfreadwrite.dll are delay-loaded, so the server starts on
    // Windows N editions and Server, which lack them until the Media Feature
    // Pack is added. A delay-load that fails raises an SEH exception no catch
    // here sees, so both are loaded (and every function used looked up) first:
    // a missing one is an ordinary error, which ExtractFrames answers with
    // ffmpeg. They stay loaded, so the delay-load helper finds them by name.
    void LoadMediaFoundation()
    {
        static const std::string problem = []() -> std::string
        {
            struct Module { const wchar_t* file; const char* name; std::vector<const char*> functions; };
            const Module modules[] = {{L"mfplat.dll", "mfplat.dll", {"MFStartup", "MFShutdown", "MFCreateAttributes", "MFCreateMediaType"}},
                                      {L"mfreadwrite.dll", "mfreadwrite.dll", {"MFCreateSourceReaderFromURL"}}};
            for (const auto& m : modules)
            {
                HMODULE library = LoadLibraryExW(m.file, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                // Windows 7 without KB2533623 has no LOAD_LIBRARY_SEARCH_*: name the system folder instead.
                if (!library && GetLastError() == ERROR_INVALID_PARAMETER)
                {
                    std::wstring system(MAX_PATH, L'\0');
                    const UINT length = GetSystemDirectoryW(system.data(), MAX_PATH);
                    if (length && length < MAX_PATH)
                    {
                        system.resize(length);
                        library = LoadLibraryW((system + L"\\" + m.file).c_str());
                    }
                }
                if (!library)
                    return std::string(m.name) + " is missing, so Windows has no Media Foundation (Windows N and Server editions need the Media Feature Pack)";
                for (const char* function : m.functions)
                    if (!GetProcAddress(library, function)) return std::string(m.name) + " has no " + function + " (this Windows's Media Foundation is too old)";
            }
            return {};
        }();
        if (!problem.empty()) throw std::runtime_error(problem);
    }
    class Foundation
    {
    public:
        Foundation()
        {
            LoadMediaFoundation();
            Check(MFStartup(MF_VERSION, MFSTARTUP_FULL), "Media Foundation could not start (is the Media Feature Pack installed?)");
        }
        Foundation(const Foundation&) = delete;
        Foundation& operator=(const Foundation&) = delete;
        ~Foundation() { MFShutdown(); }
    };
    // One video stream decoded to RGB32 by the source reader's video processor.
    class Reader
    {
    public:
        explicit Reader(const fs::path& video)
        {
            // Advanced video processing (Windows 8 and later) converts any
            // decoder output to RGB32; Windows 7 has the plain processor.
            HRESULT hr = Open(video, MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING);
            if (FAILED(hr)) hr = Open(video, MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING);
            if (hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr == MF_E_UNSUPPORTED_FORMAT)
                throw std::runtime_error("Media Foundation cannot read this kind of file (" + HexCode(hr) + ")");
            if (hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_TOPO_CODEC_NOT_FOUND)
                throw std::runtime_error("Windows has no decoder for this video's codec (" + HexCode(hr) + ")");
            Check(hr, "Media Foundation could not open the video");
            Format();
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value)) && value.vt == VT_UI8)
                duration = static_cast<double>(value.uhVal.QuadPart) / 1e7;
            PropVariantClear(&value);
            PropVariantInit(&value);
            if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS, &value)) && value.vt == VT_UI4)
                seekable = (value.ulVal & MFMEDIASOURCE_CAN_SEEK) != 0;
            PropVariantClear(&value);
        }
        // The first frame at or after seconds (the one showing then); false
        // when the stream ends first.
        bool Read(double seconds, Image::Rgba& picture, double& at)
        {
            const auto target = static_cast<LONGLONG>(seconds * 1e7);
            if (seekable && (target < position || target > position + 20000000))
            {
                PROPVARIANT where;
                PropVariantInit(&where);
                where.vt = VT_I8;
                where.hVal.QuadPart = target;
                const HRESULT hr = reader->SetCurrentPosition(GUID_NULL, where);
                PropVariantClear(&where);
                Check(hr, "Media Foundation could not seek in the video");
                position = target;
            }
            else if (!seekable && target < position) return false;
            for (int samples = 0; samples < 100000; ++samples)
            {
                DWORD stream = 0, flags = 0;
                LONGLONG stamp = 0;
                Com<IMFSample> sample;
                Check(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &stamp, sample.Put()), "Media Foundation could not decode the video");
                if (flags & MF_SOURCE_READERF_ERROR) throw std::runtime_error("Media Foundation stopped decoding the video.");
                if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) Format();
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
                if (!sample) continue;
                position = stamp;
                LONGLONG length = 0;
                sample->GetSampleDuration(&length);
                // The frame on screen at the target: it starts before and ends after.
                if (stamp + std::max<LONGLONG>(length, 1) <= target) continue;
                Convert(sample.Get(), picture);
                at = static_cast<double>(stamp) / 1e7;
                return true;
            }
            throw std::runtime_error("Media Foundation did not reach " + TimeText(seconds) + " in the video.");
        }
        double duration = -1;
        int width = 0, height = 0;
        double aspect = 1; // pixel aspect ratio
    private:
        HRESULT Open(const fs::path& video, const GUID& processing)
        {
            Com<IMFAttributes> attributes;
            HRESULT hr = MFCreateAttributes(attributes.Put(), 2);
            if (FAILED(hr)) return hr;
            attributes->SetUINT32(processing, TRUE);
            hr = MFCreateSourceReaderFromURL(video.c_str(), attributes.Get(), reader.Put());
            if (FAILED(hr)) return hr;
            reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
            if (FAILED(hr)) return hr == MF_E_INVALIDSTREAMNUMBER ? MF_E_INVALIDMEDIATYPE : hr;
            Com<IMFMediaType> type;
            hr = MFCreateMediaType(type.Put());
            if (FAILED(hr)) return hr;
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            return reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get());
        }
        void Format()
        {
            Com<IMFMediaType> type;
            Check(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), type.Put()), "Media Foundation could not describe the video");
            UINT32 w = 0, h = 0;
            Check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h), "Media Foundation could not read the video's frame size");
            if (!w || !h || w > 16384 || h > 16384) throw std::runtime_error("The video's frames are " + std::to_string(w) + "x" + std::to_string(h) + ", which is not supported.");
            width = static_cast<int>(w);
            height = static_cast<int>(h);
            stride = static_cast<INT32>(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, w * 4));
            UINT32 num = 1, den = 1;
            aspect = SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, &num, &den)) && num && den ? static_cast<double>(num) / den : 1.0;
        }
        void Convert(IMFSample* sample, Image::Rgba& picture) const
        {
            Com<IMFMediaBuffer> buffer;
            Check(sample->ConvertToContiguousBuffer(buffer.Put()), "Media Foundation could not read a frame");
            picture = Image::Rgba(width, height);
            auto copy = [&](const BYTE* top, LONG pitch)
            {
                for (int y = 0; y < height; ++y)
                {
                    const BYTE* row = top + static_cast<std::ptrdiff_t>(y) * pitch;
                    uint8_t* out = picture.At(0, y);
                    for (int x = 0; x < width; ++x, row += 4, out += 4) { out[0] = row[2]; out[1] = row[1]; out[2] = row[0]; out[3] = 255; }
                }
            };
            Com<IMF2DBuffer> flat;
            if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(flat.Put()))))
            {
                BYTE* top = nullptr;
                LONG pitch = 0;
                if (SUCCEEDED(flat->Lock2D(&top, &pitch)))
                {
                    // Lock2D gives the top row and a negative pitch for bottom-up frames.
                    copy(top, pitch);
                    flat->Unlock2D();
                    return;
                }
            }
            BYTE* data = nullptr;
            DWORD capacity = 0, length = 0;
            Check(buffer->Lock(&data, &capacity, &length), "Media Foundation could not read a frame");
            const LONG pitch = stride ? stride : width * 4;
            const size_t needed = static_cast<size_t>(std::abs(pitch)) * static_cast<size_t>(height);
            if (length < needed) { buffer->Unlock(); throw std::runtime_error("A decoded frame is smaller than its size says."); }
            copy(pitch < 0 ? data + static_cast<size_t>(-pitch) * static_cast<size_t>(height - 1) : data, pitch);
            buffer->Unlock();
        }
        Com<IMFSourceReader> reader;
        INT32 stride = 0;
        LONGLONG position = -1;
        bool seekable = true;
    };
    // The frame as shown: wider or narrower when its pixels are not square.
    Image::Rgba Square(Image::Rgba picture, double aspect)
    {
        if (std::abs(aspect - 1) < 0.01) return picture;
        const int w = std::max(1, static_cast<int>(std::lround(picture.width * aspect)));
        return Image::Resize(picture, w, picture.height, Image::Filter::Bilinear);
    }
    Extracted MediaFoundationFrames(const fs::path& video, const FrameOptions& o)
    {
        Apartment apartment;
        Foundation foundation;
        Reader reader(video);
        Extracted out;
        out.backend = "mediafoundation";
        out.duration = reader.duration;
        out.width = std::max(1, static_cast<int>(std::lround(reader.width * reader.aspect)));
        out.height = reader.height;
        const auto times = SampleTimes(o, out.duration, out.notes);
        Image::Rgba picture;
        double at = 0;
        if (o.mode != "scene")
        {
            for (size_t i = 0; i < times.size(); ++i)
            {
                Report(o, static_cast<double>(i), static_cast<double>(times.size()), "Frame " + std::to_string(i + 1) + " of " + std::to_string(times.size()) + " at " + TimeText(times[i]));
                if (!reader.Read(times[i], picture, at)) { out.notes.push_back("The video ends before " + TimeText(times[i]) + "; frames stop there."); break; }
                out.frames.push_back(Save(Square(std::move(picture), reader.aspect), at, out.frames.size() + 1, o));
            }
            return out;
        }
        // Scene: measure every candidate small, then read the picks again at size.
        std::vector<Histogram> histograms;
        std::vector<double> stamps;
        for (size_t i = 0; i < times.size(); ++i)
        {
            Report(o, static_cast<double>(i), static_cast<double>(times.size()) * 1.25, "Measuring candidate " + std::to_string(i + 1) + " of " + std::to_string(times.size()));
            if (!reader.Read(times[i], picture, at)) break;
            histograms.push_back(LumaHistogram(Image::FitWithin(picture, 256)));
            stamps.push_back(at);
        }
        if (histograms.empty()) throw std::runtime_error("Media Foundation decoded no frames between " + TimeText(o.start) + " and the end.");
        std::vector<double> change;
        const auto picks = PickScenes(histograms, static_cast<size_t>(o.count), o.threshold, change);
        for (const auto i : picks)
        {
            Report(o, static_cast<double>(times.size()) + static_cast<double>(out.frames.size()), static_cast<double>(times.size()) * 1.25, "Saving scene " + std::to_string(out.frames.size() + 1));
            if (!reader.Read(stamps[i], picture, at)) continue;
            auto frame = Save(Square(std::move(picture), reader.aspect), at, out.frames.size() + 1, o);
            frame.change = i ? Round(change[i], 3) : -1;
            out.frames.push_back(frame);
        }
        if (picks.size() < 2) out.notes.push_back("No cut reached the threshold (" + Detail::Number(o.threshold) + "); lower threshold to catch smaller changes.");
        return out;
    }

    std::wstring Wide(const std::string& utf8)
    {
        if (utf8.empty()) return {};
        const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (size <= 0) throw std::runtime_error("An argument is not valid UTF-8.");
        std::wstring out(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
        return out;
    }
    std::string LastError()
    {
        const DWORD code = GetLastError();
        char text[512] = {};
        const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text, sizeof text, nullptr);
        std::string message(text, n);
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) message.pop_back();
        return message.empty() ? "error " + std::to_string(code) : message;
    }
#endif
}

// ---------------------------------------------------------------- images
Image::Rgba DecodeImageFile(const fs::path& path)
{
    const auto bytes = ReadFile(path, 512u * 1024 * 1024);
    const auto format = Image::Format(bytes);
#ifdef _WIN32
    // WIC has no TGA codec; ImageCodec.h reads TGA, and anything WIC cannot.
    if (format != "tga")
    {
        try { return WicDecode(path); }
        catch (const std::exception& wic)
        {
            if (format.empty()) throw std::runtime_error(Utf8(path.filename()) + " is not an image this build reads (" + ImageFormats() + "): " + wic.what());
            if (format != "png" && format != "bmp") throw std::runtime_error(Utf8(path.filename()) + ": " + wic.what());
        }
    }
#endif
    if (format.empty()) throw std::runtime_error(Utf8(path.filename()) + " is not an image this build reads (" + ImageFormats() + ").");
    try { return Image::Decode(bytes); }
    catch (const std::exception& e) { throw std::runtime_error(Utf8(path.filename()) + ": " + e.what()); }
}

std::string PngBytes(const Image::Rgba& image)
{
#ifdef _WIN32
    try { return WicPng(image); }
    catch (const std::exception&) {} // ImageCodec.h writes it instead
#endif
    Image::PngOptions options;
    options.level = 6;
    return Image::EncodePng(image, options);
}

std::string ImageFormats()
{
#ifdef _WIN32
    return "PNG, JPEG, GIF, TIFF, BMP, ICO and TGA, plus any codec Windows has installed";
#else
    return "PNG, BMP and TGA here; JPEG, GIF and TIFF need the Windows build";
#endif
}

// ---------------------------------------------------------------- processes
fs::path FindFfmpeg(const fs::path& configured)
{
#ifdef _WIN32
    const fs::path name = L"ffmpeg.exe";
    const wchar_t separator = L';';
    std::wstring search(32768, L'\0');
    const DWORD size = GetEnvironmentVariableW(L"PATH", search.data(), static_cast<DWORD>(search.size()));
    search.resize(size < search.size() ? size : 0);
#else
    const fs::path name = "ffmpeg";
    const char separator = ':';
    const char* value = std::getenv("PATH");
    const std::string search = value ? value : "";
#endif
    std::error_code error;
    auto runnable = [&](const fs::path& file)
    {
        if (!fs::is_regular_file(file, error)) return false;
#ifndef _WIN32
        if (access(file.c_str(), X_OK) != 0) return false;
#endif
        return true;
    };
    if (!configured.empty())
    {
        for (const auto& candidate : {configured, configured / name, configured / "bin" / name})
            if (runnable(candidate)) return candidate;
        throw std::runtime_error("The ffmpeg given (" + Utf8(configured) + ") is not an ffmpeg executable or a folder holding one. Fix [Mcp] Ffmpeg in System\\Reloaded_Editor.ini or the ffmpeg argument.");
    }
    size_t start = 0;
    while (start <= search.size())
    {
        auto stop = search.find(separator, start);
        if (stop == search.npos) stop = search.size();
        auto folder = search.substr(start, stop - start);
        start = stop + 1;
        while (!folder.empty() && folder.front() == '"') folder.erase(folder.begin());
        while (!folder.empty() && folder.back() == '"') folder.pop_back();
        if (folder.empty()) continue;
        const auto candidate = fs::path(folder) / name;
        if (runnable(candidate)) return candidate;
    }
    return {};
}

int RunProcess(const fs::path& program, const std::vector<std::string>& arguments, const fs::path& log, unsigned timeoutMs)
{
#ifdef _WIN32
    std::wstring line = L"\"" + program.wstring() + L"\"";
    for (const auto& argument : arguments) line += L" " + Wide(QuoteArgument(argument));
    SECURITY_ATTRIBUTES inherit{static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), nullptr, TRUE};
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE)
    {
        const auto why = LastError();
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        throw std::runtime_error("Cannot prepare " + Utf8(program.filename()) + "'s output (" + Utf8(log) + "): " + why + ".");
    }
    // Only these two handles go to the child: never the server's own stdin
    // and stdout, which carry the MCP conversation.
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> storage(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    HANDLE handles[2] = {input, output};
    const bool listed = InitializeProcThreadAttributeList(attributes, 1, 0, &size) &&
                        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles, nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = static_cast<DWORD>(listed ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW));
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = output;
    startup.lpAttributeList = listed ? attributes : nullptr;
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> command(line.begin(), line.end());
    command.push_back(L'\0');
    const BOOL started = CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, nullptr,
                                        &startup.StartupInfo, &process);
    const auto why = started ? std::string() : LastError();
    if (listed) DeleteProcThreadAttributeList(attributes);
    CloseHandle(input);
    CloseHandle(output);
    if (!started) throw std::runtime_error("Cannot start " + Utf8(program) + ": " + why + ".");
    CloseHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, timeoutMs) == WAIT_TIMEOUT)
    {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
        throw std::runtime_error(Utf8(program.filename()) + " took longer than " + std::to_string(timeoutMs / 1000) + " s and was stopped; narrow start and end.");
    }
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    std::vector<std::string> words{program.string()};
    words.insert(words.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& word : words) argv.push_back(word.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    pid_t pid = 0;
    const int failed = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (failed) throw std::runtime_error("Cannot start " + program.string() + ": " + std::strerror(failed) + ".");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;)
    {
        int status = 0;
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        if (done < 0 && errno != EINTR) throw std::runtime_error("Lost track of " + program.filename().string() + ".");
        if (std::chrono::steady_clock::now() > deadline)
        {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            throw std::runtime_error(program.filename().string() + " took longer than " + std::to_string(timeoutMs / 1000) + " s and was stopped; narrow start and end.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
}

// ---------------------------------------------------------------- frames
namespace
{
    Extracted Frames(const fs::path& video, const FrameOptions& options)
    {
        CheckFrameOptions(options);
        std::error_code error;
        if (!fs::is_regular_file(video, error)) throw std::runtime_error("The video " + Utf8(video) + " does not exist or is not a file.");
        fs::create_directories(options.outDir, error);
        if (!fs::is_directory(options.outDir, error)) throw std::runtime_error("Cannot create the frame folder " + Utf8(options.outDir) + ".");
        const auto backend = Lower(options.backend);
        std::string mediaFoundation;
#ifdef _WIN32
        if (backend != "ffmpeg")
        {
            try { return MediaFoundationFrames(video, options); }
            catch (const std::exception& e)
            {
                if (backend == "mediafoundation") throw;
                mediaFoundation = e.what();
            }
        }
#else
        if (backend == "mediafoundation") throw std::runtime_error("Media Foundation exists only on Windows; use backend auto or ffmpeg here.");
#endif
        const auto ffmpeg = FindFfmpeg(options.ffmpeg);
        if (ffmpeg.empty()) throw std::runtime_error(NoBackend(mediaFoundation));
        auto out = FfmpegFrames(ffmpeg, video, options);
        if (!mediaFoundation.empty()) out.notes.insert(out.notes.begin(), "Media Foundation could not read the video (" + mediaFoundation + "), so ffmpeg did.");
        return out;
    }
}
Extracted ExtractFrames(const fs::path& video, const FrameOptions& options)
{
    try { return Frames(video, options); }
    catch (const Interrupted& stop) { std::rethrow_exception(stop.cause); }
}

Image::Rgba ContactSheet(const std::vector<Frame>& frames, int columns, int maxEdge)
{
    if (frames.empty()) throw std::runtime_error("A contact sheet needs frames.");
    if (maxEdge < 64 || maxEdge > 4096) throw std::runtime_error("The contact sheet's maxEdge must be from 64 to 4096.");
    const int n = static_cast<int>(frames.size());
    if (columns <= 0) columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    columns = std::clamp(columns, 1, n);
    const int rows = (n + columns - 1) / columns;
    int frameW = 1, frameH = 1;
    for (const auto& f : frames) { frameW = std::max(frameW, f.width); frameH = std::max(frameH, f.height); }
    // Cells sized so the whole sheet fits maxEdge, never enlarged.
    const int gap = 4;
    const double scale = std::min(1.0, std::min(static_cast<double>(maxEdge - gap * (columns + 1)) / (static_cast<double>(columns) * frameW),
                                                 static_cast<double>(maxEdge - gap * (rows + 1)) / (static_cast<double>(rows) * frameH)));
    if (scale <= 0) throw std::runtime_error("Too many frames for a contact sheet of maxEdge " + std::to_string(maxEdge) + ".");
    const int cellW = std::max(8, static_cast<int>(frameW * scale)), cellH = std::max(8, static_cast<int>(frameH * scale));
    Render::Canvas canvas(columns * cellW + gap * (columns + 1), rows * cellH + gap * (rows + 1), {24, 24, 24, 255});
    for (int i = 0; i < n; ++i)
    {
        const int x0 = gap + (i % columns) * (cellW + gap), y0 = gap + (i / columns) * (cellH + gap);
        auto picture = DecodeImageFile(frames[static_cast<size_t>(i)].path);
        const double s = std::min(static_cast<double>(cellW) / picture.width, static_cast<double>(cellH) / picture.height);
        picture = Image::Resize(picture, std::max(1, static_cast<int>(picture.width * s)), std::max(1, static_cast<int>(picture.height * s)), Image::Filter::Box);
        const int ox = x0 + (cellW - picture.width) / 2, oy = y0 + (cellH - picture.height) / 2;
        for (int y = 0; y < picture.height; ++y) std::memcpy(canvas.image.At(ox, oy + y), picture.At(0, y), static_cast<size_t>(picture.width) * 4);
        const auto label = std::to_string(i + 1) + "  " + TimeText(frames[static_cast<size_t>(i)].time);
        const int textScale = cellW >= 320 ? 2 : 1;
        canvas.FillRect(x0, y0 + cellH - Render::Canvas::TextHeight(textScale) - 6, x0 + Render::Canvas::TextWidth(label, textScale) + 8, y0 + cellH, {0, 0, 0, 170});
        canvas.Text(x0 + 4, y0 + cellH - Render::Canvas::TextHeight(textScale) - 3, label, {255, 255, 255, 255}, textScale);
    }
    return canvas.image;
}
}
