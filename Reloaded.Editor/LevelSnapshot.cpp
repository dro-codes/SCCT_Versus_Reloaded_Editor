#include "pch.h"
#undef min
#undef max
#include "LevelSnapshot.h"
#include "EditorConfigBits.h"
#include "LevelSnapshotModel.h"
#include "MinimapModel.h"
#include "EntryThumbnailModel.h"
#include "MapAuthoringModel.h"
#include "WorkflowEditor.h"
#include "MeasureTool.h"
#include "logger.h"
#include <windows.h>
#include <commdlg.h>
#include <algorithm>
using std::max;
using std::min;
#include <objidl.h> // IStream and PROPID for gdiplus.h, which the lean Windows header leaves out.
#include <gdiplus.h>
#include <zlib.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")

namespace Editor = Workflow::Editor;
namespace fs = std::filesystem;
using Snapshot::Bytes;

namespace
{
    // The editor's viewport list, as the grid-size shortcut and the view
    // capture read it.
    constexpr uintptr_t kViewportConfigs = 0x1165e8d4;
    constexpr uintptr_t kViewportCount = 0x1165e8d8;
    constexpr uintptr_t kUnrealEd = 0x117a59b0;        // GUnrealEd, whose config bits the viewports' overlays read

    HWND frameWindow = nullptr;
    HWND previewWindow = nullptr;
    HBITMAP previewBitmap = nullptr;
    std::string previewText;
    ULONG_PTR gdiplusToken = 0;

    template <class T> T Read(uintptr_t at) { return *reinterpret_cast<T*>(at); }

    void EnsureGdiplus()
    {
        if (gdiplusToken) return;
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr) != Gdiplus::Ok) throw std::runtime_error("GDI+ is not available.");
    }

    struct Image { Bytes bgr; }; // kSize x kSize, top-down BGR.

    // The largest visible level viewport's window and camera: a perspective
    // one, or the top view (REN_OrthXY; 14 and 15 are the front and side views).
    LevelSnapshot::Perspective LevelViewport(bool top)
    {
        const auto configs = Read<uintptr_t>(kViewportConfigs);
        const int count = Read<int>(kViewportCount);
        if (!configs || count <= 0 || count > 64) throw std::runtime_error("No level viewports are open.");
        LevelSnapshot::Perspective best;
        long bestArea = 0;
        for (int i = 0; i < count; ++i)
        {
            const auto frame = Read<uintptr_t>(configs + i * 0x28 + 0x24);
            const auto view = frame ? Read<uintptr_t>(frame + 0x3c) : 0;
            const auto camera = view ? Read<uintptr_t>(view + 0x30) : 0;
            if (!camera) continue;
            const int mode = Read<int>(camera + 0x4fc);
            if (top ? mode != 13 : (mode == 13 || mode == 14 || mode == 15)) continue;
            const auto window = Read<uintptr_t>(view + 0x1b4);
            const HWND hwnd = window ? Read<HWND>(window + 4) : nullptr;
            if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) continue;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const long area = static_cast<long>(rc.right - rc.left) * (rc.bottom - rc.top);
            if (area > bestArea) { best = { hwnd, camera, view, i }; bestArea = area; }
        }
        if (!best.window)
            throw std::runtime_error(top ? "No top viewport is open. Set one of the editor's viewports to Top." : "No perspective viewport is open to take the snapshot from.");
        return best;
    }
    LevelSnapshot::Perspective PerspectiveViewport() { return LevelViewport(false); }

    // The whole picture squeezed to the snapshot's square, as the stock
    // snapshots are; the game stretches it back into its own box.
    Image Resample(Gdiplus::Bitmap& source)
    {
        if (source.GetLastStatus() != Gdiplus::Ok || !source.GetWidth() || !source.GetHeight()) throw std::runtime_error("The image could not be read.");
        Gdiplus::Bitmap target(Snapshot::kSize, Snapshot::kSize, PixelFormat24bppRGB);
        Gdiplus::Graphics graphics(&target);
        graphics.Clear(Gdiplus::Color(255, 0, 0, 0));
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.DrawImage(&source, Gdiplus::Rect(0, 0, Snapshot::kSize, Snapshot::kSize), 0, 0, static_cast<INT>(source.GetWidth()), static_cast<INT>(source.GetHeight()), Gdiplus::UnitPixel);
        Gdiplus::BitmapData data{};
        Gdiplus::Rect rect(0, 0, Snapshot::kSize, Snapshot::kSize);
        if (target.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat24bppRGB, &data) != Gdiplus::Ok) throw std::runtime_error("The snapshot could not be read back.");
        Image image{ Bytes(static_cast<size_t>(Snapshot::kSize) * Snapshot::kSize * 3) };
        for (int y = 0; y < Snapshot::kSize; ++y)
            memcpy(&image.bgr[static_cast<size_t>(y) * Snapshot::kSize * 3], static_cast<const std::uint8_t*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride, static_cast<size_t>(Snapshot::kSize) * 3);
        target.UnlockBits(&data);
        return image;
    }

    // A screen BitBlt from this DPI-unaware editor reads physical pixels
    // while its window coordinates are logical, which shifted and cropped the
    // shot on a scaled monitor. Per-monitor awareness for the thread makes
    // both physical. Loaded by name: SetThreadDpiAwarenessContext is Windows 10+.
    class PhysicalPixels
    {
    public:
        PhysicalPixels()
        {
            if (const HMODULE user = GetModuleHandleA("user32.dll"))
                set = reinterpret_cast<Set>(GetProcAddress(user, "SetThreadDpiAwarenessContext"));
            if (set) previous = set(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        }
        ~PhysicalPixels() { if (set && previous) set(previous); }
        PhysicalPixels(const PhysicalPixels&) = delete;
        PhysicalPixels& operator=(const PhysicalPixels&) = delete;
    private:
        using Set = HANDLE(WINAPI*)(HANDLE);
        Set set = nullptr;
        HANDLE previous = nullptr;
    };

    // The viewport's client area, freshly drawn. The window renders its own
    // picture first (PrintWindow with full content, which includes Direct3D
    // output), so windows over it, this editor's or any other program's, are
    // never in the shot; the screen is only the fallback. The caller owns
    // the returned bitmap.
    // UWindowsViewport::Repaint (vtable +0xc8, as the emitter preview calls
    // it): draws and presents now, so a camera moved a moment ago is in the
    // picture. A window repaint only marks the viewport for the main loop.
    bool RepaintNow(uintptr_t viewport)
    {
        if (!viewport) return false;
        __try
        {
            const auto table = *reinterpret_cast<uintptr_t*>(viewport);
            reinterpret_cast<void(__thiscall*)(uintptr_t, int)>(*reinterpret_cast<uintptr_t*>(table + 0xc8))(viewport, 1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // The axis indicator the viewports draw in a corner (UseAxisIndicator,
    // EditorConfigBits.h), turned off while a picture is drawn: it is no part
    // of the map, and it showed in the map-selection picture.
    class NoAxisIndicator
    {
        unsigned char* flags = nullptr;
        uint8_t bit = 0x4;
    public:
        NoAxisIndicator()
        {
            uint32_t at = 0x21C;
            EditorConfigBits::Locate("UseAxisIndicator", at, bit);
            const uintptr_t editor = *reinterpret_cast<const uintptr_t*>(kUnrealEd);
            if (!editor || !bit || !(*reinterpret_cast<unsigned char*>(editor + at) & bit)) return;
            flags = reinterpret_cast<unsigned char*>(editor + at);
            *flags = static_cast<unsigned char>(*flags & ~bit);
        }
        ~NoAxisIndicator() { if (flags) *flags = static_cast<unsigned char>(*flags | bit); } // back on the next paint
        NoAxisIndicator(const NoAxisIndicator&) = delete;
        NoAxisIndicator& operator=(const NoAxisIndicator&) = delete;
    };

    HBITMAP GrabViewport(HWND hwnd, uintptr_t viewport, int& width, int& height)
    {
        // RE+'s own overlays are no part of the map either. Made first, so it
        // goes last: its redraw brings back the axis indicator as well.
        const MeasureTool::OverlayPause noOverlays;
        const NoAxisIndicator noAxis;
        Editor::Redraw();
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        if (viewport && !RepaintNow(viewport)) Logger::log("Viewport capture: the synchronous repaint faulted; capturing the last drawn frame.");
        Sleep(60);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        width = rc.right - rc.left;
        height = rc.bottom - rc.top;
        if (width < 16 || height < 16) throw std::runtime_error("The perspective viewport is too small to capture.");
        const HDC screen = GetDC(nullptr);
        const HDC memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
        HGDIOBJ previous = SelectObject(memory, bitmap);
        constexpr UINT kClientOnly = 1, kRenderFullContent = 2;
        BOOL copied = PrintWindow(hwnd, memory, kClientOnly | kRenderFullContent);
        if (!copied)
        {
            PhysicalPixels physical;
            GetClientRect(hwnd, &rc);
            POINT origin{ 0, 0 };
            ClientToScreen(hwnd, &origin);
            width = rc.right - rc.left;
            height = rc.bottom - rc.top;
            SelectObject(memory, previous);
            DeleteObject(bitmap);
            bitmap = CreateCompatibleBitmap(screen, width, height);
            previous = SelectObject(memory, bitmap);
            copied = bitmap && BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
        }
        SelectObject(memory, previous);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        if (!copied) { if (bitmap) DeleteObject(bitmap); throw std::runtime_error("The viewport could not be captured."); }
        return bitmap;
    }

    Image CaptureViewport()
    {
        EnsureGdiplus();
        const auto perspective = PerspectiveViewport();
        const HWND hwnd = perspective.window;
        // What is on screen is what is captured: the frame in front, freshly drawn.
        if (frameWindow) SetForegroundWindow(frameWindow);
        int width = 0, height = 0;
        const HBITMAP bitmap = GrabViewport(hwnd, perspective.viewport, width, height);
        Image image;
        try
        {
            Gdiplus::Bitmap source(bitmap, nullptr);
            image = Resample(source);
        }
        catch (...) { DeleteObject(bitmap); throw; }
        DeleteObject(bitmap);
        return image;
    }

    bool PngEncoder(CLSID& clsid)
    {
        UINT count = 0, bytes = 0;
        if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || !bytes) return false;
        std::vector<std::uint8_t> buffer(bytes);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
        if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return false;
        for (UINT i = 0; i < count; ++i)
            if (encoders[i].MimeType && wcscmp(encoders[i].MimeType, L"image/png") == 0) { clsid = encoders[i].Clsid; return true; }
        return false;
    }

    // The viewport drawn afresh and scaled to the size fit gives for its
    // client area.
    std::unique_ptr<Gdiplus::Bitmap> Scaled(const LevelSnapshot::Perspective& perspective, const std::function<Thumbnail::Size(int, int)>& fit)
    {
        EnsureGdiplus();
        const HWND viewport = perspective.window;
        if (!viewport || !IsWindow(viewport)) throw std::runtime_error("The viewport is gone.");
        int width = 0, height = 0;
        const HBITMAP bitmap = GrabViewport(viewport, perspective.viewport, width, height);
        Thumbnail::Size size;
        try { size = fit(width, height); }
        catch (...) { DeleteObject(bitmap); throw; }
        auto target = std::make_unique<Gdiplus::Bitmap>(size.width, size.height, PixelFormat24bppRGB);
        bool drawn = false;
        {
            Gdiplus::Bitmap source(bitmap, nullptr);
            if (source.GetLastStatus() == Gdiplus::Ok && target->GetLastStatus() == Gdiplus::Ok)
            {
                Gdiplus::Graphics graphics(target.get());
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                drawn = graphics.DrawImage(&source, Gdiplus::Rect(0, 0, size.width, size.height), 0, 0, width, height, Gdiplus::UnitPixel) == Gdiplus::Ok;
            }
        }
        DeleteObject(bitmap);
        if (!drawn) throw std::runtime_error("The viewport picture could not be scaled.");
        return target;
    }

    void SaveScaled(const LevelSnapshot::Perspective& perspective, const fs::path& file, const std::function<Thumbnail::Size(int, int)>& fit)
    {
        CLSID png{};
        EnsureGdiplus();
        if (!PngEncoder(png)) throw std::runtime_error("GDI+ has no PNG encoder.");
        const auto picture = Scaled(perspective, fit);
        std::error_code error;
        fs::create_directories(file.parent_path(), error);
        fs::path temporary = file;
        temporary += ".tmp";
        const Gdiplus::Status status = picture->Save(temporary.c_str(), &png, nullptr);
        if (status != Gdiplus::Ok || !MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            fs::remove(temporary, error);
            throw std::runtime_error("Cannot write the picture " + file.string());
        }
    }

    // A bitmap as PNG file bytes, encoded in memory.
    std::string PngBytes(Gdiplus::Bitmap& bitmap)
    {
        CLSID png{};
        if (!PngEncoder(png)) throw std::runtime_error("GDI+ has no PNG encoder.");
        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) throw std::runtime_error("No memory for the picture.");
        std::string bytes;
        STATSTG stat{};
        if (bitmap.Save(stream, &png, nullptr) == Gdiplus::Ok && SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME))
            && stat.cbSize.QuadPart > 0 && stat.cbSize.QuadPart < (256ull << 20))
        {
            bytes.resize(static_cast<size_t>(stat.cbSize.QuadPart));
            LARGE_INTEGER start{};
            ULONG read = 0;
            if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr)) || FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) || read != bytes.size())
                bytes.clear();
        }
        stream->Release();
        if (bytes.empty()) throw std::runtime_error("The picture could not be encoded as PNG.");
        return bytes;
    }

    // A cell-sized 32-bit bitmap, opaque, with the window colour behind
    // whatever draw() puts on it.
    HBITMAP Cell(int width, int height, const std::function<void(Gdiplus::Graphics&)>& draw)
    {
        EnsureGdiplus();
        Gdiplus::Bitmap target(width, height, PixelFormat32bppARGB);
        if (target.GetLastStatus() != Gdiplus::Ok) return nullptr;
        {
            Gdiplus::Graphics graphics(&target);
            const COLORREF window = GetSysColor(COLOR_WINDOW);
            graphics.Clear(Gdiplus::Color(255, GetRValue(window), GetGValue(window), GetBValue(window)));
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            draw(graphics);
        }
        HBITMAP result = nullptr;
        if (target.GetHBITMAP(Gdiplus::Color(255, 255, 255, 255), &result) != Gdiplus::Ok) return nullptr;
        return result;
    }

    Image LoadImageFile(const std::string& path)
    {
        EnsureGdiplus();
        std::wstring wide(path.size() + 1, L'\0');
        const int length = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wide.data(), static_cast<int>(wide.size()));
        if (length <= 0) throw std::runtime_error("The image path could not be read.");
        wide.resize(static_cast<size_t>(length) - 1);
        Gdiplus::Bitmap source(wide.c_str());
        if (source.GetLastStatus() != Gdiplus::Ok) throw std::runtime_error("The image could not be opened:\n" + path);
        return Resample(source);
    }

    Bytes ReadFile(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot read " + path.string());
        return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    void WriteFile(const fs::path& path, const Bytes& bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("Cannot write " + path.string());
    }
    // A package as the tables reader wants it: the .utc container inflated.
    Bytes Unpack(const Bytes& file)
    {
        const auto container = Snapshot::Describe(file);
        if (!container.compressed) return file;
        Bytes out(container.uncompressedSize);
        uLongf length = container.uncompressedSize;
        if (uncompress(out.data(), &length, file.data() + container.dataOffset, container.compressedSize) != Z_OK || length != container.uncompressedSize)
            throw std::runtime_error("The .utc package did not inflate.");
        return out;
    }
    std::string Stamp()
    {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
        localtime_s(&local, &now);
        char text[32];
        std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &local);
        return text;
    }
    std::string Quote(const fs::path& path) { return "\"" + path.string() + "\""; }

    struct Written
    {
        fs::path utc, utx, bmp;
        Snapshot::Verified verified;
    };

    // How a picture goes into the interface package: the texture's name and
    // size, the importer's options, and the format to compress it to (none:
    // as the importer made it) that the saved package must then hold.
    struct Import
    {
        std::string name = "Menu", options = "MIPS=0", compress;
        int size = Snapshot::kSize, format = -1;
    };

    // Into the map's interface package: the existing package loaded so nothing
    // else in it is lost, the import of source, the stock save, and a check of
    // what landed on disk. The files it replaced are put back when it fails.
    Written Apply(const fs::path& source, const Import& import)
    {
        const fs::path map = Editor::MapFile();
        const std::string stem = map.stem().string();
        const std::string package = Snapshot::PackageName(stem);
        const fs::path textures = Editor::Directory().parent_path().parent_path() / "Packages" / "Textures";
        const fs::path utc = textures / (package + ".utc"), utx = textures / (package + ".utx");
        std::error_code error;
        fs::create_directories(textures, error);

        Written written;
        written.bmp = source;

        // What is there now: backed up beside the editor's other files, and
        // loaded so the loading screens and map settings ride along.
        size_t previousExports = 0;
        std::vector<std::pair<fs::path, fs::path>> backups;
        const fs::path backupDirectory = Editor::Directory() / "snapshot-backups";
        const std::string stamp = Stamp();
        for (const auto& file : { utc, utx })
        {
            if (!fs::exists(file, error)) continue;
            fs::create_directories(backupDirectory, error);
            const fs::path backup = backupDirectory / (file.stem().string() + "." + stamp + file.extension().string());
            if (!fs::copy_file(file, backup, fs::copy_options::overwrite_existing, error)) throw std::runtime_error("Cannot back up " + file.string() + " to " + backup.string());
            backups.emplace_back(file, backup);
            try { previousExports = max(previousExports, Snapshot::Parse(Unpack(ReadFile(file))).exports.size()); }
            catch (const std::exception& e) { Logger::log("Level snapshot: could not read " + file.string() + ": " + e.what()); }
        }
        // Loaded as the stock map open loads it (LOADMAPPROP): from the file
        // only when it is not in memory yet. OBJ LOAD would read a loaded
        // package back from the file, losing settings changed since.
        Editor::Exec("LOADMAPPROP MAP=\"" + stem + "\"");

        if (!Editor::Exec("TEXTURE IMPORT FILE=" + Quote(source) + " NAME=\"" + import.name + "\" PACKAGE=\"" + package + "\" " + import.options))
            throw std::runtime_error("The editor's texture importer refused the picture for " + import.name + ".");
        if (!import.compress.empty()) Editor::Exec("TEXTURE COMPRESS NAME=" + package + "." + import.name + " FORMAT=" + import.compress);

        const auto started = std::chrono::file_clock::now();
        try
        {
            // The stock map save's route: it carries the map settings object
            // into the package, then saves the .utc; it writes nothing when
            // the map has no settings, and then the package is saved as it
            // is. The stock save asks before writing a package smaller than
            // the file it replaces, which a recompressed picture often is;
            // the file is backed up and the result checked below, so it goes
            // first and the save does not stop for the question.
            auto landed = [&] { return fs::exists(utc, error) && fs::last_write_time(utc, error) >= started; };
            if (!backups.empty()) fs::remove(utc, error);
            bool saved = Editor::Exec("SAVEMAPPROP MAP=\"" + stem + "\"") && landed();
            if (!saved) saved = Editor::Exec("OBJ SAVEPACKAGE PACKAGE=\"" + package + "\" FILE=" + Quote(utc));
            if (!saved || !landed()) throw std::runtime_error("The editor did not write " + utc.string());
            const Bytes packageBytes = Unpack(ReadFile(utc));
            written.verified.menu = Minimap::VerifyTexture(packageBytes, import.name, import.size, previousExports);
            written.verified.exportCount = Snapshot::Parse(packageBytes).exports.size();
            if (import.format >= 0 && written.verified.menu.format != import.format)
                throw std::runtime_error("The " + import.name + " texture was saved in format " + std::to_string(written.verified.menu.format)
                                         + ", not " + std::to_string(import.format) + " (" + (import.compress.empty() ? std::string("the file's own") : import.compress) + ").");
            written.utc = utc;
            // Whichever of the two the game opens first, both now hold the new picture.
            if (fs::exists(utx, error)) { WriteFile(utx, packageBytes); written.utx = utx; }
        }
        catch (const std::exception&)
        {
            for (const auto& [file, backup] : backups) fs::copy_file(backup, file, fs::copy_options::overwrite_existing, error);
            throw;
        }
        Editor::Redraw();
        return written;
    }

    // A small window with the picture as saved and where it went.
    LRESULT CALLBACK PreviewProc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        if (message == WM_CREATE)
        {
            HWND text = CreateWindowExA(0, "STATIC", previewText.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 12, Snapshot::kSize + 24, 420, 70, window, reinterpret_cast<HMENU>(1), GetModuleHandle(nullptr), nullptr);
            SendMessage(text, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            return 0;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT ps{};
            const HDC dc = BeginPaint(window, &ps);
            if (previewBitmap)
            {
                const HDC memory = CreateCompatibleDC(dc);
                const HGDIOBJ previous = SelectObject(memory, previewBitmap);
                BitBlt(dc, 12, 12, Snapshot::kSize, Snapshot::kSize, memory, 0, 0, SRCCOPY);
                SelectObject(memory, previous);
                DeleteDC(memory);
            }
            EndPaint(window, &ps);
            return 0;
        }
        if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
        if (message == WM_NCDESTROY) { previewWindow = nullptr; if (previewBitmap) { DeleteObject(previewBitmap); previewBitmap = nullptr; } }
        return DefWindowProcA(window, message, w, l);
    }
    void ShowPreview(const Image& image, const Written& written)
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = Snapshot::kSize;
        info.bmiHeader.biHeight = -Snapshot::kSize;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap || !bits) return;
        auto* out = static_cast<std::uint8_t*>(bits);
        for (size_t i = 0; i < static_cast<size_t>(Snapshot::kSize) * Snapshot::kSize; ++i)
        {
            out[i * 4] = image.bgr[i * 3]; out[i * 4 + 1] = image.bgr[i * 3 + 1]; out[i * 4 + 2] = image.bgr[i * 3 + 2]; out[i * 4 + 3] = 255;
        }
        if (previewBitmap) DeleteObject(previewBitmap);
        previewBitmap = bitmap;
        previewText = "Saved as the Menu texture of " + written.utc.filename().string() + (written.utx.empty() ? "" : " and " + written.utx.filename().string())
                    + "\nin " + written.utc.parent_path().string() + "\n(" + std::to_string(written.verified.exportCount) + " objects in the package). The game shows it in map selection.";
        const std::string title = "Level Snapshot: " + fs::path(Editor::MapFile()).stem().string();
        if (!previewWindow)
        {
            WNDCLASSA wc{};
            wc.hInstance = GetModuleHandle(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            wc.lpfnWndProc = PreviewProc;
            wc.lpszClassName = "ReloadedLevelSnapshot";
            RegisterClassA(&wc);
            RECT rc{ 0, 0, Snapshot::kSize + 24 + 180, Snapshot::kSize + 24 + 76 };
            AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
            previewWindow = CreateWindowExA(WS_EX_TOOLWINDOW, wc.lpszClassName, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, frameWindow, nullptr, wc.hInstance, nullptr);
            if (!previewWindow) return;
        }
        else
        {
            SetWindowTextA(previewWindow, title.c_str());
            SetWindowTextA(GetDlgItem(previewWindow, 1), previewText.c_str());
            InvalidateRect(previewWindow, nullptr, TRUE);
        }
        ShowWindow(previewWindow, SW_SHOWNORMAL);
        SetForegroundWindow(previewWindow);
    }
    void Finish(const Image& image)
    {
        const std::string stem = fs::path(Editor::MapFile()).stem().string();
        const fs::path bmp = Editor::Directory() / "snapshots" / (stem + "-menu.bmp");
        std::error_code error;
        fs::create_directories(bmp.parent_path(), error);
        WriteFile(bmp, Snapshot::Bmp24(Snapshot::kSize, Snapshot::kSize, image.bgr));
        const Written written = Apply(bmp, Import{});
        Logger::log("Level snapshot: wrote " + written.utc.string());
        ShowPreview(image, written);
    }
}

void LevelSnapshot::Attach(HWND frame) { frameWindow = frame; }

void LevelSnapshot::FromViewport()
{
    Snapshot::PackageName(fs::path(Editor::MapFile()).stem().string()); // Before touching the screen: the map must have a name.
    Finish(CaptureViewport());
}

void LevelSnapshot::FromImageFile()
{
    Snapshot::PackageName(fs::path(Editor::MapFile()).stem().string());
    char file[MAX_PATH] = "";
    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = frameWindow;
    dialog.lpstrFilter = "Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = sizeof file;
    dialog.lpstrTitle = "Image for the level snapshot";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&dialog)) return;
    Finish(LoadImageFile(file));
}

LevelSnapshot::Perspective LevelSnapshot::FindPerspective() { return PerspectiveViewport(); }

LevelSnapshot::Perspective LevelSnapshot::FindViewport(const std::string& kind)
{
    if (kind.empty() || kind == "perspective") return LevelViewport(false);
    if (kind == "top") return LevelViewport(true);
    throw std::runtime_error("The viewport is \"perspective\" or \"top\".");
}

void LevelSnapshot::SaveViewportThumbnail(const Perspective& perspective, const fs::path& file)
{
    SaveScaled(perspective, file, [](int width, int height) { return Thumbnail::Fit(width, height); });
}

void LevelSnapshot::SaveViewportImage(const Perspective& viewport, const fs::path& file, int maxEdge)
{
    SaveScaled(viewport, file, [maxEdge](int width, int height) { return Thumbnail::Within(width, height, maxEdge); });
}

LevelSnapshot::Picture LevelSnapshot::ViewportImage(const Perspective& viewport, int maxEdge)
{
    const auto picture = Scaled(viewport, [maxEdge](int width, int height) { return Thumbnail::Within(width, height, maxEdge); });
    Picture result;
    result.width = static_cast<int>(picture->GetWidth());
    result.height = static_cast<int>(picture->GetHeight());
    result.png = PngBytes(*picture);
    return result;
}

namespace
{
    // The interface package's pictures, as the wiki's Map Settings page lists them.
    struct Slot { const char* name; int size; int format; const char* compress; bool alpha; };
    constexpr Slot kSlots[] = {
        { "Menu", 256, Snapshot::kFormatDxt5, "DXT5", true },
        { "Briefing", 256, Snapshot::kFormatDxt5, "DXT5", true },
        { "LoadingATT0", 512, Snapshot::kFormatDxt1, "DXT1", false },
        { "LoadingDEF0", 512, Snapshot::kFormatDxt1, "DXT1", false },
    };

    // The whole picture squeezed to size x size as top-down RGBA, alpha kept,
    // as the stock pictures are drawn into their square.
    Bytes ResampleRgba(Gdiplus::Bitmap& source, int size)
    {
        if (source.GetLastStatus() != Gdiplus::Ok || !source.GetWidth() || !source.GetHeight()) throw std::runtime_error("The image could not be read.");
        if (source.GetWidth() > 16384 || source.GetHeight() > 16384) throw std::runtime_error("The image is larger than 16384 pixels on a side.");
        Gdiplus::Bitmap target(size, size, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics graphics(&target);
            graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            graphics.DrawImage(&source, Gdiplus::Rect(0, 0, size, size), 0, 0, static_cast<INT>(source.GetWidth()), static_cast<INT>(source.GetHeight()), Gdiplus::UnitPixel);
        }
        Gdiplus::BitmapData data{};
        Gdiplus::Rect rect(0, 0, size, size);
        if (target.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) throw std::runtime_error("The picture could not be read back.");
        Bytes rgba(static_cast<size_t>(size) * size * 4);
        for (int y = 0; y < size; ++y)
        {
            const auto* row = static_cast<const std::uint8_t*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
            for (int x = 0; x < size; ++x)
            {
                auto* out = &rgba[(static_cast<size_t>(y) * size + x) * 4];
                out[0] = row[x * 4 + 2]; out[1] = row[x * 4 + 1]; out[2] = row[x * 4]; out[3] = row[x * 4 + 3];
            }
        }
        target.UnlockBits(&data);
        return rgba;
    }

    // A path for a message from the bridge, which carries UTF-8.
    std::string Shown(const fs::path& path) { return Workflow::Authoring::Utf8Text(path); }

    // The engine's console reads ANSI and stops at quotes. Checked as UTF-8:
    // a name the code page cannot hold turns into question marks in ANSI.
    void CheckEnginePath(const fs::path& path)
    {
        const auto text = Shown(path);
        for (unsigned char c : text)
            if (c < 0x20 || c >= 0x80 || c == '"')
                throw std::runtime_error("The editor's importer cannot open " + text + ": the path must be plain ASCII without quotes. Copy the file somewhere with such a path.");
    }
}

namespace
{
    const char* FormatName(int format)
    {
        static const char* formats[] = { "P8", "RGBA7", "RGB16", "DXT1", "RGB8", "RGBA8", "NODATA", "DXT3", "DXT5", "L8", "G16", "RRRGGGBBB" };
        return format >= 0 && format < 12 ? formats[format] : "unknown";
    }
}

Workflow::Json LevelSnapshot::DescribeInterfacePackage()
{
    const std::string stem = fs::path(Editor::MapFile()).stem().string();
    Workflow::Json result{ { "package", nullptr }, { "saved", false }, { "settings", false }, { "textures", Workflow::Json::object() } };
    if (stem.empty()) return result;
    const std::string package = Snapshot::PackageName(stem);
    const fs::path utc = Editor::Directory().parent_path().parent_path() / "Packages" / "Textures" / (package + ".utc");
    result["package"] = package;
    result["file"] = utc.string();
    std::error_code error;
    if (!fs::is_regular_file(utc, error)) return result;
    result["saved"] = true;
    const Bytes bytes = Unpack(ReadFile(utc));
    const auto tables = Snapshot::Parse(bytes);
    result["exports"] = tables.exports.size();
    result["settings"] = tables.Find("MapSettings", "SMapSettings") != nullptr;
    for (const auto& slot : kSlots)
        if (const auto* e = tables.Find(slot.name, "Texture"))
            try
            {
                const auto t = Snapshot::DescribeTexture(bytes, tables, *e);
                result["textures"][slot.name] = { { "width", t.width }, { "height", t.height }, { "format", FormatName(t.format) }, { "mips", t.mips },
                                                  { "expected", { { "size", slot.size }, { "format", slot.compress } } } };
            }
            catch (const std::exception& problem) { result["textures"][slot.name] = { { "unreadable", problem.what() } }; }
    return result;
}

Workflow::Json LevelSnapshot::ApplyInterfaceTexture(const std::string& slotName, const fs::path& image)
{
    const Slot* slot = nullptr;
    for (const auto& candidate : kSlots) if (Thumbnail::Lower(slotName) == Thumbnail::Lower(candidate.name)) slot = &candidate;
    if (!slot) throw std::runtime_error("The interface texture slot is Menu, Briefing, LoadingATT0 or LoadingDEF0.");
    const std::string stem = fs::path(Editor::MapFile()).stem().string();
    const std::string package = Snapshot::PackageName(stem); // The map must have a name first.
    std::error_code error;
    if (image.is_relative()) throw std::runtime_error("Give the image's full path: " + Shown(image));
    if (!fs::is_regular_file(image, error)) throw std::runtime_error("There is no image file at " + Shown(image));
    const auto extension = Thumbnail::Lower(image.extension().string());
    Import import;
    import.name = slot->name;
    import.size = slot->size;
    import.format = slot->format;
    import.compress = slot->compress;
    import.options = std::string("MIPS=0 MASKED=0 ALPHATEXTURE=") + (slot->alpha ? "1" : "0");
    fs::path source = image;
    if (extension == ".dds" || extension == ".tga")
    {
        // Read as they are: the size must be the slot's already.
        std::ifstream in(image, std::ios::binary);
        std::string header(128, '\0');
        in.read(header.data(), static_cast<std::streamsize>(header.size()));
        header.resize(static_cast<size_t>(in.gcount()));
        const auto size = Workflow::Authoring::ImageSize(header, extension);
        if (size.first != static_cast<uint32_t>(slot->size) || size.second != static_cast<uint32_t>(slot->size))
            throw std::runtime_error(std::string(slot->name) + " is " + std::to_string(slot->size) + " x " + std::to_string(slot->size) + "; " + Shown(image.filename())
                                     + " is " + std::to_string(size.first) + " x " + std::to_string(size.second) + ". Give a PNG to have it fitted, or resize the file.");
        if (extension == ".dds")
        {
            // A DDS keeps its own format (the pixel format's FourCC at byte
            // 84), so the wrong one is refused before anything is imported.
            const auto fourCC = header.size() >= 88 ? header.substr(84, 4) : std::string();
            if (fourCC != slot->compress)
                throw std::runtime_error(std::string(slot->name) + " is a " + slot->compress + " texture; " + Shown(image.filename()) + " is "
                                         + (fourCC.empty() || fourCC[0] == '\0' ? std::string("not block-compressed") : fourCC) + ". Save it as " + slot->compress + ", or give a PNG to have it converted.");
            import.compress.clear();
        }
        CheckEnginePath(image);
    }
    else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" || extension == ".gif" || extension == ".tif" || extension == ".tiff")
    {
        EnsureGdiplus();
        Bytes rgba;
        {
            Gdiplus::Bitmap bitmap(image.wstring().c_str());
            if (bitmap.GetLastStatus() != Gdiplus::Ok) throw std::runtime_error("The image could not be opened: " + Shown(image));
            rgba = ResampleRgba(bitmap, slot->size);
        }
        if (!slot->alpha) for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        source = Editor::Directory() / "snapshots" / (stem + "-" + Thumbnail::Lower(slot->name) + ".tga");
        fs::create_directories(source.parent_path(), error);
        WriteFile(source, Minimap::Tga32(slot->size, slot->size, rgba));
        CheckEnginePath(source);
    }
    else throw std::runtime_error("Interface pictures are read from PNG, JPEG, BMP, GIF, TIFF, TGA or DDS files: " + Shown(image.filename()));
    const Written written = Apply(source, import);
    Logger::log("Level snapshot: wrote the " + std::string(slot->name) + " texture of " + written.utc.string());
    Workflow::Json result{ { "slot", slot->name }, { "package", package }, { "file", written.utc.string() },
                           { "width", written.verified.menu.width }, { "height", written.verified.menu.height },
                           { "format", FormatName(written.verified.menu.format) }, { "exports", written.verified.exportCount } };
    if (!written.utx.empty()) result["utx"] = written.utx.string();
    return result;
}

HBITMAP LevelSnapshot::LoadThumbnail(const fs::path& file, int width, int height)
{
    try
    {
        std::error_code error;
        const auto bytes = fs::file_size(file, error);
        if (error || bytes == 0 || bytes > Thumbnail::kMaxFileBytes) return nullptr;
        const Bytes data = ReadFile(file);
        EnsureGdiplus();
        const HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, data.size());
        if (!global) return nullptr;
        if (void* at = GlobalLock(global)) { memcpy(at, data.data(), data.size()); GlobalUnlock(global); }
        else { GlobalFree(global); return nullptr; }
        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(global, TRUE, &stream)) || !stream) { GlobalFree(global); return nullptr; }
        HBITMAP result = nullptr;
        {
            std::unique_ptr<Gdiplus::Bitmap> source(Gdiplus::Bitmap::FromStream(stream));
            if (source && source->GetLastStatus() == Gdiplus::Ok && source->GetWidth() && source->GetHeight() && source->GetWidth() <= 8192 && source->GetHeight() <= 8192)
            {
                const int sourceWidth = static_cast<int>(source->GetWidth()), sourceHeight = static_cast<int>(source->GetHeight());
                const auto box = Thumbnail::Letterbox(sourceWidth, sourceHeight, width, height);
                result = Cell(width, height, [&](Gdiplus::Graphics& graphics) {
                    graphics.DrawImage(source.get(), Gdiplus::Rect(box.x, box.y, box.width, box.height), 0, 0, sourceWidth, sourceHeight, Gdiplus::UnitPixel);
                });
            }
        }
        stream->Release();
        return result;
    }
    catch (const std::exception& e)
    {
        Logger::log("Thumbnail: could not read " + file.string() + ": " + e.what());
        return nullptr;
    }
}

HBITMAP LevelSnapshot::ThumbnailPlaceholder(int width, int height)
{
    return Cell(width, height, [&](Gdiplus::Graphics& graphics) {
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 200, 200, 200));
        Gdiplus::Pen border(Gdiplus::Color(255, 150, 150, 150));
        graphics.FillRectangle(&fill, 2, 2, width - 5, height - 5);
        graphics.DrawRectangle(&border, 2, 2, width - 5, height - 5);
        Gdiplus::Font font(L"Segoe UI", 8.0f);
        Gdiplus::SolidBrush text(Gdiplus::Color(255, 90, 90, 90));
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        graphics.DrawString(L"No thumbnail", -1, &font, Gdiplus::RectF(0, 0, static_cast<Gdiplus::REAL>(width), static_cast<Gdiplus::REAL>(height)), &format, &text);
    });
}
