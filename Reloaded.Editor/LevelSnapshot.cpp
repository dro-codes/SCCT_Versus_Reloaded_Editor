#include "pch.h"
#include "LevelSnapshot.h"
#include "Appearance.h"
#include "EditorConfigBits.h"
#include "LevelSnapshotModel.h"
#include "EntryThumbnailModel.h"
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

    // The largest visible perspective level viewport's window and camera.
    LevelSnapshot::Perspective PerspectiveViewport()
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
            if (mode == 13 || mode == 14 || mode == 15) continue; // The orthographic views.
            const auto window = Read<uintptr_t>(view + 0x1b4);
            const HWND hwnd = window ? Read<HWND>(window + 4) : nullptr;
            if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) continue;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const long area = static_cast<long>(rc.right - rc.left) * (rc.bottom - rc.top);
            if (area > bestArea) { best = { hwnd, camera, view }; bestArea = area; }
        }
        if (!best.window) throw std::runtime_error("No perspective viewport is open to take the snapshot from.");
        return best;
    }

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

    // Into the map's interface package: the BMP for the editor's importer,
    // the existing package loaded so nothing else in it is lost, the import,
    // the stock save, and a check of what landed on disk.
    Written Apply(const Image& image)
    {
        const fs::path map = Editor::MapFile();
        const std::string stem = map.stem().string();
        const std::string package = Snapshot::PackageName(stem);
        const fs::path textures = Editor::Directory().parent_path().parent_path() / "Packages" / "Textures";
        const fs::path utc = textures / (package + ".utc"), utx = textures / (package + ".utx");
        std::error_code error;
        fs::create_directories(textures, error);

        Written written;
        written.bmp = Editor::Directory() / "snapshots" / (stem + "-menu.bmp");
        fs::create_directories(written.bmp.parent_path(), error);
        WriteFile(written.bmp, Snapshot::Bmp24(Snapshot::kSize, Snapshot::kSize, image.bgr));

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

        if (!Editor::Exec("TEXTURE IMPORT FILE=" + Quote(written.bmp) + " NAME=\"Menu\" PACKAGE=\"" + package + "\" MIPS=0"))
            throw std::runtime_error("The editor's texture importer refused the snapshot.");

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
            written.verified = Snapshot::Verify(packageBytes, previousExports);
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
        const Written written = Apply(image);
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

void LevelSnapshot::SaveViewportThumbnail(const Perspective& perspective, const fs::path& file)
{
    EnsureGdiplus();
    const HWND viewport = perspective.window;
    if (!viewport || !IsWindow(viewport)) throw std::runtime_error("The perspective viewport is gone.");
    CLSID png{};
    if (!PngEncoder(png)) throw std::runtime_error("GDI+ has no PNG encoder.");
    int width = 0, height = 0;
    const HBITMAP bitmap = GrabViewport(viewport, perspective.viewport, width, height);
    const auto size = Thumbnail::Fit(width, height);
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    fs::path temporary = file;
    temporary += ".tmp";
    Gdiplus::Status status = Gdiplus::GenericError;
    {
        Gdiplus::Bitmap source(bitmap, nullptr);
        Gdiplus::Bitmap target(size.width, size.height, PixelFormat24bppRGB);
        if (source.GetLastStatus() == Gdiplus::Ok && target.GetLastStatus() == Gdiplus::Ok)
        {
            {
                Gdiplus::Graphics graphics(&target);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                graphics.DrawImage(&source, Gdiplus::Rect(0, 0, size.width, size.height), 0, 0, width, height, Gdiplus::UnitPixel);
            }
            status = target.Save(temporary.c_str(), &png, nullptr);
        }
    }
    DeleteObject(bitmap);
    if (status != Gdiplus::Ok || !MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        fs::remove(temporary, error);
        throw std::runtime_error("Cannot write the thumbnail " + file.string());
    }
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
    // Modern Dark: a dark grey box (the light one stands out in the dark lists).
    const bool dark = Appearance::Dark();
    return Cell(width, height, [&](Gdiplus::Graphics& graphics) {
        Gdiplus::SolidBrush fill(dark ? Gdiplus::Color(255, 52, 52, 52) : Gdiplus::Color(255, 200, 200, 200));
        Gdiplus::Pen border(dark ? Gdiplus::Color(255, 85, 85, 85) : Gdiplus::Color(255, 150, 150, 150));
        graphics.FillRectangle(&fill, 2, 2, width - 5, height - 5);
        graphics.DrawRectangle(&border, 2, 2, width - 5, height - 5);
        Gdiplus::Font font(L"Segoe UI", 8.0f);
        Gdiplus::SolidBrush text(dark ? Gdiplus::Color(255, 160, 160, 160) : Gdiplus::Color(255, 90, 90, 90));
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        graphics.DrawString(L"No thumbnail", -1, &font, Gdiplus::RectF(0, 0, static_cast<Gdiplus::REAL>(width), static_cast<Gdiplus::REAL>(height)), &format, &text);
    });
}
