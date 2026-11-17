#include "pch.h"
#undef min
#undef max
#include "ObjImportWindow.h"
#include "CharacterSkinsImage.h"
#include "ObjImportModel.h"
#include "WindowPlacement.h"
#include "WorkflowEditor.h"
#include <commdlg.h>
#include <objidl.h> // IStream and PROPID for gdiplus.h, which the lean Windows header leaves out.
#include <gdiplus.h>
#include <algorithm>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")

namespace ObjImportWindow
{
namespace
{
using Workflow::Json;
namespace Editor = Workflow::Editor;
constexpr int kFile = 200, kBrowse = 201, kPackage = 202, kGroup = 203, kName = 204, kScale = 205, kUp = 206, kCollision = 207,
              kSplit = 208, kPlace = 209, kSummary = 210, kStatus = 211;
constexpr UINT_PTR kPreviewTimer = 1;
constexpr const char* kSection = "ObjImport";
// The collision choices, in the box's order, with the names the import takes.
const struct { const char* key; const char* label; } kCollisions[] = {
    {"parts", "Convex hulls, one per connected part"},
    {"groups", "Convex hulls, one per OBJ object or group"},
    {"file", "Convex hulls from the file's UCX_ or MCD objects"},
    {"box", "Simple box"},
    {"polygons", "Per polygon (the mesh's own triangles)"},
    {"none", "None"},
};
std::string startPackage;
std::string Utf8(const std::filesystem::path& path) { const auto text = path.u8string(); return std::string(text.begin(), text.end()); }
std::filesystem::path FromUtf8(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }
bool previewing = false;

std::string IniPath()
{
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    return (std::filesystem::path(exe).parent_path() / "Reloaded_Editor.ini").string();
}
std::string Setting(const char* key, const char* fallback)
{
    char value[2048] = {};
    GetPrivateProfileStringA(kSection, key, fallback, value, sizeof(value), IniPath().c_str());
    return value;
}
void Store(const char* key, const std::string& value) { WritePrivateProfileStringA(kSection, key, value.c_str(), IniPath().c_str()); }

HWND Control(HWND window, const char* type, const char* text, DWORD style, int id, int x, int y, int width, int height)
{
    auto control = CreateWindowExA(std::string(type) == "EDIT" ? WS_EX_CLIENTEDGE : 0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, width,
                                   height, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
    SendMessage(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}
// Edit text in UTF-8 (file names may hold any letters).
std::string Text(HWND window, int id)
{
    wchar_t buffer[2048]{};
    GetDlgItemTextW(window, id, buffer, 2048);
    std::wstring wide(buffer);
    const auto first = wide.find_first_not_of(L" \t"), last = wide.find_last_not_of(L" \t");
    wide = first == std::wstring::npos ? std::wstring() : wide.substr(first, last - first + 1);
    if (wide.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}
void SetText(HWND window, int id, const std::string& utf8)
{
    const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(count, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), count);
    SetDlgItemTextW(window, id, wide.c_str());
}
void Status(HWND window, const std::string& text) { SetText(window, kStatus, text); }
std::string Thousands(size_t n)
{
    auto s = std::to_string(n);
    for (int at = static_cast<int>(s.size()) - 3; at > 0; at -= 3) s.insert(static_cast<size_t>(at), ",");
    return s;
}
std::string Round(double v)
{
    char text[32];
    snprintf(text, sizeof(text), "%.0f", v);
    return text;
}

Json Spec(HWND window)
{
    double scale = 1;
    if (!ObjImport::Number(Text(window, kScale), scale) || !(scale > 0)) throw std::runtime_error("The scale must be a number above 0.");
    const int collision = static_cast<int>(SendDlgItemMessageA(window, kCollision, CB_GETCURSEL, 0, 0));
    return {{"file", Text(window, kFile)}, {"package", Text(window, kPackage)}, {"group", Text(window, kGroup)}, {"name", Text(window, kName)},
            {"scale", scale}, {"up", SendDlgItemMessageA(window, kUp, CB_GETCURSEL, 0, 0) == 1 ? "z" : "y"},
            {"collision", kCollisions[std::clamp(collision, 0, static_cast<int>(std::size(kCollisions)) - 1)].key},
            {"split", IsDlgButtonChecked(window, kSplit) == BST_CHECKED}, {"place", IsDlgButtonChecked(window, kPlace) == BST_CHECKED}};
}
void Remember(HWND window)
{
    Store("File", Text(window, kFile));
    Store("Package", Text(window, kPackage));
    Store("Group", Text(window, kGroup));
    Store("Scale", Text(window, kScale));
    Store("Up", SendDlgItemMessageA(window, kUp, CB_GETCURSEL, 0, 0) == 1 ? "z" : "y");
    const int collision = static_cast<int>(SendDlgItemMessageA(window, kCollision, CB_GETCURSEL, 0, 0));
    Store("Collision", kCollisions[std::clamp(collision, 0, static_cast<int>(std::size(kCollisions)) - 1)].key);
    Store("Split", IsDlgButtonChecked(window, kSplit) == BST_CHECKED ? "1" : "0");
    Store("Place", IsDlgButtonChecked(window, kPlace) == BST_CHECKED ? "1" : "0");
}

std::string SlotLine(const Json& slot)
{
    const auto name = slot.value("name", std::string());
    std::string line = (name.empty() ? std::string("(no material)") : name) + ": ";
    const auto kind = slot.value("kind", std::string());
    if (kind == "loaded") line += "loaded, " + slot.value("material", std::string());
    else if (kind == "image")
        line += "imported from " + Utf8(std::filesystem::path(FromUtf8(slot.value("image", std::string()))).filename()) + " as " +
                slot.value("texture", std::string());
    else line += "the default texture";
    if (!slot.value("note", std::string()).empty()) line += " (" + slot.value("note", std::string()) + ")";
    return line;
}
void Preview(HWND window)
{
    KillTimer(window, kPreviewTimer);
    if (Text(window, kFile).empty()) { SetText(window, kSummary, "Choose an OBJ file. Its MTL file is read from beside it."); return; }
    try
    {
        previewing = true;
        const auto p = Editor::ObjImportPreview(Spec(window));
        previewing = false;
        const auto& size = p.at("size");
        std::string text = Thousands(p.at("triangles").get<size_t>()) + " triangles, " + Round(size[0].get<double>()) + " x " + Round(size[1].get<double>()) +
                           " x " + Round(size[2].get<double>()) + " units. Collision: " + p.at("collision").get<std::string>();
        if (p.at("hulls").get<size_t>())
            text += ", " + Thousands(p.at("hulls").get<size_t>()) + " hulls of " + Thousands(p.at("hullTriangles").get<size_t>()) + " triangles";
        text += ".\r\n";
        for (const auto& slot : p.at("slots")) text += SlotLine(slot) + "\r\n";
        const auto& parts = p.at("parts");
        if (!p.at("error").get<std::string>().empty()) text += p.at("error").get<std::string>() + "\r\n";
        else if (parts.size() == 1) text += "One mesh: within the game's limits (5,512 triangles and 5,512 hull triangles).\r\n";
        else
        {
            text += Thousands(parts.size()) + " parts, each within the game's limits, placed together:";
            for (size_t i = 0; i < parts.size(); ++i)
                text += (i ? ", " : " ") + parts[i].at("name").get<std::string>() + " (" + Thousands(parts[i].at("triangles").get<size_t>()) + ")";
            text += ".\r\n";
        }
        for (const auto& part : parts)
            if (part.at("exists").get<bool>()) { text += part.at("name").get<std::string>() + " exists already in that package: choose another name.\r\n"; break; }
        for (const auto& w : p.at("warnings")) text += w.get<std::string>() + "\r\n";
        SetText(window, kSummary, text);
    }
    catch (const std::exception& e)
    {
        previewing = false;
        SetText(window, kSummary, e.what());
    }
}
void SchedulePreview(HWND window) { SetTimer(window, kPreviewTimer, 400, nullptr); }

void Browse(HWND window)
{
    wchar_t path[32768]{};
    const auto current = Text(window, kFile);
    std::wstring initial;
    if (!current.empty()) initial = std::filesystem::path(FromUtf8(current)).parent_path().wstring();
    OPENFILENAMEW choose{sizeof(choose)};
    choose.hwndOwner = window;
    choose.lpstrFile = path;
    choose.nMaxFile = 32768;
    choose.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    choose.lpstrFilter = L"Wavefront OBJ (*.obj)\0*.obj\0All files (*.*)\0*.*\0";
    choose.lpstrTitle = L"Import OBJ";
    choose.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&choose)) return;
    const std::filesystem::path file(path);
    SetText(window, kFile, Utf8(file));
    SetText(window, kName, ObjImport::SafeName(Utf8(file.stem())));
    Preview(window);
}

void Import(HWND window)
{
    Status(window, "Importing...");
    UpdateWindow(window);
    const auto result = Editor::ImportObjMesh(Spec(window));
    Remember(window);
    std::string text;
    const auto& parts = result.at("parts");
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const auto& part = parts[i];
        text += (i ? "; " : "Imported ") + part.at("path").get<std::string>() + " (" + Thousands(part.at("triangles").get<size_t>()) + " triangles, ";
        if (part.at("hullTriangles").get<int>() > 0)
            text += Thousands(part.at("hulls").get<size_t>()) + " hulls of " + Thousands(static_cast<size_t>(part.at("hullTriangles").get<int>())) + " triangles)";
        else if (part.at("polygonCollision").get<int>() > 0)
            text += "colliding on " + Thousands(static_cast<size_t>(part.at("polygonCollision").get<int>())) + " of its own triangles)";
        else text += "no collision)";
    }
    text += ".";
    if (result.contains("placed")) text += " Placed " + std::to_string(result.at("placed").size()) + (result.at("placed").size() == 1 ? " actor" : " actors") + " where the viewport was last clicked (one Undo step).";
    for (const auto& w : result.at("warnings")) text += " " + w.get<std::string>();
    text += Text(window, kPackage).empty() || ObjImport::Lower(Text(window, kPackage)) == "mylevel"
                ? " Save the map to keep it."
                : " Save the package (Static Mesh Browser, File > Save) to keep it.";
    Status(window, text);
    Preview(window);
}

INT_PTR CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM)
{
    try
    {
        if (message == WM_INITDIALOG)
        {
            SetWindowTextA(window, "Import OBJ");
            RECT r{};
            GetClientRect(window, &r);
            const int width = r.right - 24;
            int y = 12;
            Control(window, "STATIC", "OBJ file", SS_CENTERIMAGE, -1, 12, y, 70, 23);
            Control(window, "EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, kFile, 86, y, width - 162, 23);
            Control(window, "BUTTON", "Browse...", WS_TABSTOP, kBrowse, width - 64, y, 76, 23);
            y += 32;
            const int third = (width - 74) / 3;
            Control(window, "STATIC", "Package", SS_CENTERIMAGE, -1, 12, y, 70, 23);
            Control(window, "EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, kPackage, 86, y, third - 54, 23);
            Control(window, "STATIC", "Group", SS_CENTERIMAGE | SS_RIGHT, -1, 86 + third - 50, y, 46, 23);
            Control(window, "EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, kGroup, 86 + third, y, third - 54, 23);
            Control(window, "STATIC", "Name", SS_CENTERIMAGE | SS_RIGHT, -1, 86 + 2 * third - 50, y, 46, 23);
            Control(window, "EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, kName, 86 + 2 * third, y, width - 74 - 2 * third, 23);
            y += 32;
            Control(window, "STATIC", "Scale", SS_CENTERIMAGE, -1, 12, y, 70, 23);
            Control(window, "EDIT", "1", ES_AUTOHSCROLL | WS_TABSTOP, kScale, 86, y, 60, 23);
            // Up to the combo box: the larger modern font needs more than 250.
            Control(window, "STATIC", "Unreal units per OBJ unit (1: an OBJ unit is 1 cm)", SS_CENTERIMAGE, -1, 152, y, (std::max)(250, width - 214 - 8 - 152), 23);
            auto up = Control(window, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kUp, width - 214, y, 226, 200);
            SendMessageA(up, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Y up (Blender, Maya, most exporters)"));
            SendMessageA(up, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Z up (3ds Max, Blender set to Z up)"));
            y += 32;
            Control(window, "STATIC", "Collision", SS_CENTERIMAGE, -1, 12, y, 70, 23);
            auto collision = Control(window, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kCollision, 86, y, 300, 300);
            for (const auto& c : kCollisions) SendMessageA(collision, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.label));
            y += 32;
            Control(window, "BUTTON", "Split into parts placed together when over the game's limit (5,512 triangles or hull triangles a mesh)",
                    BS_AUTOCHECKBOX | WS_TABSTOP, kSplit, 12, y, width, 22);
            y += 24;
            Control(window, "BUTTON", "Place in the map where the viewport was last clicked", BS_AUTOCHECKBOX | WS_TABSTOP, kPlace, 12, y, width, 22);
            y += 30;
            Control(window, "EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, kSummary, 12, y, width, 130);
            y += 138;
            Control(window, "STATIC", "Materials: a loaded material with the MTL material's name, else its map_Kd image imported into the package "
                                      "(TGA, BMP, PCX, DDS, PNG, JPG), else the default texture. Hulls: clean convex hulls the game collides with.",
                    0, -1, 12, y, width, 30);
            y += 34;
            Control(window, "STATIC", "", 0, kStatus, 12, y, width, r.bottom - 50 - y);
            Control(window, "BUTTON", "Import", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, r.right - 215, r.bottom - 38, 95, 27);
            Control(window, "BUTTON", "Close", WS_TABSTOP, IDCANCEL, r.right - 110, r.bottom - 38, 95, 27);

            SetText(window, kFile, Setting("File", ""));
            SetText(window, kPackage, startPackage.empty() ? Setting("Package", "MyLevel") : startPackage);
            SetText(window, kGroup, Setting("Group", ""));
            SetText(window, kScale, Setting("Scale", "1"));
            const auto file = Text(window, kFile);
            if (!file.empty()) SetText(window, kName, ObjImport::SafeName(Utf8(std::filesystem::path(FromUtf8(file)).stem())));
            SendMessageA(up, CB_SETCURSEL, Setting("Up", "y") == "z" ? 1 : 0, 0);
            const auto key = Setting("Collision", "parts");
            int index = 0;
            for (int i = 0; i < static_cast<int>(std::size(kCollisions)); ++i) if (key == kCollisions[i].key) index = i;
            SendMessageA(collision, CB_SETCURSEL, index, 0);
            CheckDlgButton(window, kSplit, Setting("Split", "1") == "1" ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(window, kPlace, Setting("Place", "1") == "1" ? BST_CHECKED : BST_UNCHECKED);
            WindowPlacement::Restore(window, "ObjImport");
            Preview(window);
            return TRUE;
        }
        if (message == WM_TIMER && w == kPreviewTimer)
        {
            Preview(window);
            return TRUE;
        }
        if (message == WM_DESTROY)
        {
            KillTimer(window, kPreviewTimer);
            WindowPlacement::Remember(window, "ObjImport");
            return FALSE;
        }
        if (message != WM_COMMAND) return FALSE;
        const int id = LOWORD(w), code = HIWORD(w);
        if (id == kBrowse) { Browse(window); return TRUE; }
        if ((code == EN_CHANGE && id >= kFile && id <= kScale) || (code == CBN_SELCHANGE && (id == kUp || id == kCollision)) ||
            (code == BN_CLICKED && id == kSplit))
        {
            if (id == kFile && code == EN_CHANGE && !previewing)
            {
                // A file typed or pasted: the name follows it.
                const auto file = Text(window, kFile);
                if (!file.empty()) SetText(window, kName, ObjImport::SafeName(Utf8(std::filesystem::path(FromUtf8(file)).stem())));
            }
            SchedulePreview(window);
            return TRUE;
        }
        if (id == IDOK) { Import(window); return TRUE; }
        if (id == IDCANCEL)
        {
            Remember(window);
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
    }
    catch (const std::exception& e)
    {
        // Reported in the window, never in a message box.
        Status(window, e.what());
        return TRUE;
    }
    return FALSE;
}

ULONG_PTR gdiplusToken = 0;
}

bool ImageToTga(const std::filesystem::path& image, const std::filesystem::path& tga, int& width, int& height, bool& alpha, std::string& error)
{
    if (!gdiplusToken)
    {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr) != Gdiplus::Ok) { error = "GDI+ is not available"; return false; }
    }
    std::unique_ptr<Gdiplus::Bitmap> source(Gdiplus::Bitmap::FromFile(image.wstring().c_str()));
    if (!source || source->GetLastStatus() != Gdiplus::Ok || !source->GetWidth() || !source->GetHeight())
    {
        error = "not an image this editor can read (TGA, BMP, PCX and DDS need sides that are powers of two)";
        return false;
    }
    auto power = [](UINT n) {
        UINT p = 1;
        while (p * 2 <= n && p < 2048) p *= 2;
        if (p < 2048 && p * 2 - n < n - p) p *= 2;
        return static_cast<int>(p);
    };
    width = power(source->GetWidth());
    height = power(source->GetHeight());
    Gdiplus::Bitmap target(width, height, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&target);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        Gdiplus::ImageAttributes wrap;
        wrap.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        graphics.DrawImage(source.get(), Gdiplus::Rect(0, 0, width, height), 0, 0, static_cast<INT>(source->GetWidth()),
                           static_cast<INT>(source->GetHeight()), Gdiplus::UnitPixel, &wrap);
    }
    Gdiplus::BitmapData data{};
    Gdiplus::Rect all(0, 0, width, height);
    if (target.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) { error = "the image could not be read"; return false; }
    CharacterSkins::Image picture;
    picture.width = width;
    picture.height = height;
    picture.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    alpha = false;
    for (int y = 0; y < height; ++y)
    {
        const auto* row = static_cast<const uint8_t*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
        for (int x = 0; x < width; ++x)
        {
            const auto* bgra = row + x * 4;
            auto* out = &picture.rgba[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
            out[0] = bgra[2];
            out[1] = bgra[1];
            out[2] = bgra[0];
            out[3] = bgra[3];
            alpha = alpha || bgra[3] < 250;
        }
    }
    target.UnlockBits(&data);
    const auto bytes = CharacterSkins::Tga(picture);
    std::ofstream out(tga, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) { error = "cannot write " + Utf8(tga); return false; }
    return true;
}

void Open(HWND owner, const std::string& package)
{
    startPackage = package;
    std::vector<WORD> bytes((sizeof(DLGTEMPLATE) + 1) / 2 + 3, 0);
    auto dialog = reinterpret_cast<DLGTEMPLATE*>(bytes.data());
    dialog->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    dialog->cx = 420;
    dialog->cy = 250;
    if (DialogBoxIndirectParamW(GetModuleHandle(nullptr), dialog, owner, Proc, 0) == -1)
        throw std::runtime_error("Cannot open the Import OBJ window.");
}
}
