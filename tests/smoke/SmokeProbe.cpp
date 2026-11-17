// Smoke probe for a disposable RE+ editor (test root from run_native_map_recovery_test.ps1 -PrepareOnly).
// Injected by NativeMapRecoveryDriver.exe after Reloaded.Editor.dll. Runs its steps on the editor's UI
// thread, answers message boxes (recording their text), writes smoke_report.txt and ends the process.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <dlgs.h>
#include <share.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace {
HHOOK hook;
HWND frameWindow;
std::filesystem::path directory;
FILE* report;
constexpr UINT kRun = WM_APP + 0x319;
constexpr uintptr_t kEditor = 0x1165DFA0;
constexpr uintptr_t kWarn = 0x115BEFB0;
int failures = 0;
DWORD finishAfter = 0; // ms the editor keeps running after the report is written (0: ends at once)

void Line(const char* kind, const std::string& name, const std::string& detail = "")
{
    if (!report) return;
    fprintf(report, "%s %s %s\n", kind, name.c_str(), detail.c_str());
    fflush(report);
}
void Pass(const std::string& n, const std::string& d = "") { Line("PASS", n, d); }
void Fail(const std::string& n, const std::string& d = "") { ++failures; Line("FAIL", n, d); }
void Info(const std::string& n, const std::string& d = "") { Line("INFO", n, d); }
void Check(bool ok, const std::string& n, const std::string& d = "") { ok ? Pass(n, d) : Fail(n, d); }

std::string Ini(const char* key, const char* fallback = "")
{
    char value[2048] = {};
    GetPrivateProfileStringA("smoke", key, fallback, value, sizeof(value), (directory / "smoke.ini").string().c_str());
    return value;
}

int Exec(const std::string& command)
{
    Info("exec", command.substr(0, command.find_first_of("\r\n")));
    auto editor = *reinterpret_cast<unsigned char**>(kEditor);
    void* output = *reinterpret_cast<void**>(kWarn);
    if (!editor || !output) return 0;
    void* exec = editor + 0x28;
    using Fn = int(__thiscall*)(void*, const char*, void*);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(exec))[0])(exec, command.c_str(), output);
}

void Pump(DWORD ms)
{
    const DWORD until = GetTickCount() + ms;
    MSG message;
    while (static_cast<int>(until - GetTickCount()) > 0)
    {
        while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        Sleep(10);
    }
}

// ---- engine reads -------------------------------------------------------------------------------
unsigned char* Level()
{
    auto editor = *reinterpret_cast<unsigned char**>(kEditor);
    return editor ? *reinterpret_cast<unsigned char**>(editor + 0x130) : nullptr;
}
std::vector<unsigned char*> Actors()
{
    std::vector<unsigned char*> result;
    auto level = Level();
    if (!level) return result;
    auto data = *reinterpret_cast<unsigned char***>(level + 0x2c);
    int count = *reinterpret_cast<int*>(level + 0x30);
    for (int i = 0; i < count && data; ++i) if (data[i]) result.push_back(data[i]);
    return result;
}
unsigned Flags(unsigned char* actor) { return *reinterpret_cast<unsigned*>(actor + 0x2f4); }
int CountFlag(unsigned bit)
{
    int n = 0;
    for (auto a : Actors()) if (Flags(a) & bit) ++n;
    return n;
}
int Selected() { return CountFlag(0x40); }
int Hidden() { return CountFlag(0x10); }

const char* NameText(int index)
{
    auto names = *reinterpret_cast<unsigned char***>(0x1169CFBC);
    int count = *reinterpret_cast<int*>(0x1169CFC0);
    return names && index >= 0 && index < count && names[index] ? reinterpret_cast<const char*>(names[index] + 12) : "<invalid>";
}
const char* ObjectName(void* object) { return object ? NameText(*reinterpret_cast<int*>(static_cast<unsigned char*>(object) + 0x20)) : "<null>"; }
std::string ClassName(unsigned char* actor) { return ObjectName(*reinterpret_cast<unsigned char**>(actor + 0x24)); }
std::string Name(unsigned char* actor) { return ObjectName(actor); }

unsigned char* FindProperty(unsigned char* actor, const char* name)
{
    auto type = *reinterpret_cast<unsigned char**>(actor + 0x24);
    for (auto property = *reinterpret_cast<unsigned char**>(type + 0x58); property; property = *reinterpret_cast<unsigned char**>(property + 0x40))
        if (!strcmp(ObjectName(property), name)) return property;
    return nullptr;
}
struct Vec { float x = 0, y = 0, z = 0; };
Vec ReadVector(unsigned char* actor, const char* name)
{
    auto p = FindProperty(actor, name);
    if (!p) return {};
    return *reinterpret_cast<Vec*>(actor + *reinterpret_cast<unsigned*>(p + 0x3C));
}
void WriteVector(unsigned char* actor, const char* name, Vec v)
{
    if (auto p = FindProperty(actor, name)) *reinterpret_cast<Vec*>(actor + *reinterpret_cast<unsigned*>(p + 0x3C)) = v;
}
struct Rot { int pitch = 0, yaw = 0, roll = 0; };
Rot ReadRotation(unsigned char* actor)
{
    auto p = FindProperty(actor, "Rotation");
    if (!p) return {};
    return *reinterpret_cast<Rot*>(actor + *reinterpret_cast<unsigned*>(p + 0x3C));
}
void SetSelected(unsigned char* actor, bool on)
{
    auto& flags = *reinterpret_cast<unsigned*>(actor + 0x2f4);
    flags = on ? (flags | 0x40u) : (flags & ~0x40u);
}
std::vector<unsigned char*> OfClass(const char* cls, size_t limit = 1000000)
{
    std::vector<unsigned char*> result;
    for (auto a : Actors()) if (ClassName(a) == cls && !(Flags(a) & 0x10)) { result.push_back(a); if (result.size() >= limit) break; }
    return result;
}

// The editor's undo buffer (UTransBuffer at GEditor+0x148; see UndoHistory.cpp for the layout).
struct UndoState { int count = 0, undone = 0; std::string current; };
UndoState Undo()
{
    UndoState s;
    auto editor = *reinterpret_cast<unsigned char**>(kEditor);
    auto trans = editor ? *reinterpret_cast<unsigned char**>(editor + 0x148) : nullptr;
    if (!trans) return s;
    auto data = *reinterpret_cast<unsigned char**>(trans + 0x28);
    s.count = *reinterpret_cast<int*>(trans + 0x2c);
    s.undone = *reinterpret_cast<int*>(trans + 0x34);
    const int at = s.count - s.undone - 1;
    if (data && at >= 0)
        if (auto title = *reinterpret_cast<const char**>(data + at * 0x28 + 0x14)) s.current = title;
    return s;
}

// ---- message boxes and commands -----------------------------------------------------------------
std::vector<std::string> boxes;      // text of every message box answered during the last command
int boxAnswer = IDOK;                // IDOK, IDYES, IDNO, IDCANCEL
std::function<bool(HWND, const std::string&)> dialogHandler; // custom handler for non-message-box dialogs

std::string WindowText(HWND w)
{
    char text[4096] = {};
    GetWindowTextA(w, text, sizeof(text));
    return text;
}
std::string DialogText(HWND dialog)
{
    std::string text;
    EnumChildWindows(dialog, [](HWND child, LPARAM p) -> BOOL {
        char cls[64] = {};
        GetClassNameA(child, cls, sizeof(cls));
        if (!_stricmp(cls, "Static")) { auto& t = *reinterpret_cast<std::string*>(p); auto s = WindowText(child); if (!s.empty()) { if (!t.empty()) t += " | "; t += s; } }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&text));
    return text;
}
// A message box has only Static and Button children (icons are Statics too).
bool IsMessageBox(HWND w)
{
    bool plain = true;
    EnumChildWindows(w, [](HWND child, LPARAM p) -> BOOL {
        char cls[64] = {};
        GetClassNameA(child, cls, sizeof(cls));
        if (_stricmp(cls, "Static") && _stricmp(cls, "Button")) { *reinterpret_cast<bool*>(p) = false; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&plain));
    return plain;
}
std::vector<std::pair<HWND, int>> stuck; // other dialogs, cancelled after a while so nothing hangs
void CALLBACK AnswerBoxes(HWND, UINT, UINT_PTR, DWORD)
{
    EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM) -> BOOL {
        if (!IsWindowVisible(w)) return TRUE;
        char cls[64] = {};
        GetClassNameA(w, cls, sizeof(cls));
        const auto title = WindowText(w);
        if (dialogHandler && dialogHandler(w, title)) return TRUE;
        if (strcmp(cls, "#32770")) return TRUE;
        if (IsMessageBox(w))
        {
            const auto text = DialogText(w);
            if (text.empty()) return TRUE;
            boxes.push_back(title + ": " + text);
            Info("box", title + ": " + text);
            int answer = boxAnswer;
            if (!GetDlgItem(w, answer)) answer = GetDlgItem(w, IDOK) ? IDOK : GetDlgItem(w, IDCANCEL) ? IDCANCEL : IDYES;
            PostMessage(w, WM_COMMAND, answer, 0);
            return TRUE;
        }
        for (auto& s : stuck)
            if (s.first == w)
            {
                if (++s.second == 60) { Info("cancel-dialog", title); PostMessage(w, WM_COMMAND, IDCANCEL, 0); }
                return TRUE;
            }
        stuck.push_back({w, 0});
        Info("dialog", title);
        return TRUE;
    }, 0);
}
double Command(UINT id, HWND target = nullptr)
{
    boxes.clear();
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    UINT_PTR timer = SetTimer(nullptr, 0, 100, AnswerBoxes);
    SendMessageA(target ? target : frameWindow, WM_COMMAND, id, 0);
    KillTimer(nullptr, timer);
    QueryPerformanceCounter(&b);
    Pump(150);
    return 1000.0 * double(b.QuadPart - a.QuadPart) / double(f.QuadPart);
}
bool BoxContains(const char* text)
{
    for (auto& b : boxes) if (b.find(text) != std::string::npos) return true;
    return false;
}
HWND FindTopWindow(const char* title)
{
    struct S { const char* t; HWND r; } s{title, nullptr};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM p) -> BOOL {
        auto s = reinterpret_cast<S*>(p);
        if (WindowText(w).find(s->t) != std::string::npos) { s->r = w; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.r;
}

std::string Fixed(double v, int decimals = 1)
{
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(decimals);
    o << v;
    return o.str();
}

// A suite that needs the editor's own loop to run (the viewports only repaint there) sets this: the
// run then ends a few seconds after the suites return, with this called first.
std::function<void()> afterRun;
void CALLBACK FinishAfterRun(HWND, UINT, UINT_PTR timer, DWORD)
{
    KillTimer(nullptr, timer);
    try { if (afterRun) afterRun(); }
    catch (const std::exception& e) { Fail("after-run", e.what()); }
    Line("DONE", "failures", std::to_string(failures));
    if (report) fclose(report);
    report = nullptr;
    TerminateProcess(GetCurrentProcess(), failures ? 3 : 0);
}

// ---- steps --------------------------------------------------------------------------------------
#include "SmokeSteps.inl"
#include "SmokeSuites.inl"

void RunAll()
{
    const auto map = Ini("map");
    if (!map.empty())
    {
        // File > Open Recent's first entry (run_smoke.ps1 writes recent_maps.json), as a user opens a map,
        // so the editor knows the map's file; MAP LOAD otherwise.
        const bool recent = Ini("open") == "recent";
        const bool loaded = recent ? (Command(40970), true) : Exec("MAP LOAD FILE=\"" + map + "\"") != 0;
        Pump(1500);
        Check(loaded && Actors().size() > 2, "map-load", std::string(recent ? "recent " : "") + map + " actors=" + std::to_string(Actors().size()) + " boxes=" + std::to_string(boxes.size()));
    }
    try { const auto suite = Ini("suite"); if (suite.empty()) RunSteps(); else RunSuites(suite); }
    catch (const std::exception& e) { Fail("exception", e.what()); }
    catch (...) { Fail("exception", "unknown"); }
    if (afterRun) { SetTimer(nullptr, 0, 4000, FinishAfterRun); return; }
    Line("DONE", "failures", std::to_string(failures));
    if (report) fclose(report);
    report = nullptr;
    // A suite that asked for it leaves the editor running that long first: its viewports only draw from the
    // editor's own loop, which this one keeps waiting, so a picture of them is taken then.
    if (finishAfter)
    {
        SetTimer(nullptr, 0, finishAfter, [](HWND, UINT, UINT_PTR, DWORD) { TerminateProcess(GetCurrentProcess(), failures ? 3 : 0); });
        return;
    }
    // hold=<seconds> in -Extra: give the editor back its own loop for a while, so the viewports
    // repaint, then save pictures of the frame and the tool windows (shot=<folder>) and end.
    if (const int hold = atoi(Ini("hold").c_str()); hold > 0)
    {
        SetTimer(nullptr, 0, static_cast<UINT>(hold) * 1000, [](HWND, UINT, UINT_PTR, DWORD) {
            SaveShot(frameWindow, "hold-frame");
            if (HWND w = OpenedWindow("Map Check")) SaveShot(w, "hold-mapcheck");
            if (HWND w = OpenedWindow("Map Design")) SaveShot(w, "hold-design");
            TerminateProcess(GetCurrentProcess(), failures ? 3 : 0);
        });
        return;
    }
    TerminateProcess(GetCurrentProcess(), failures ? 3 : 0);
}

LRESULT CALLBACK OnMessage(int code, WPARAM removed, LPARAM parameter)
{
    if (code >= 0)
    {
        auto message = reinterpret_cast<CWPSTRUCT*>(parameter);
        if (message->message == kRun)
        {
            UnhookWindowsHookEx(hook);
            hook = nullptr;
            RunAll();
        }
    }
    return CallNextHookEx(hook, code, removed, parameter);
}

BOOL CALLBACK FindEditorWindow(HWND window, LPARAM threadAddress)
{
    DWORD process = 0;
    DWORD thread = GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId()) return TRUE;
    char klass[128] = {};
    GetClassNameA(window, klass, sizeof(klass));
    if (strstr(klass, "WEditorFrame"))
    {
        *reinterpret_cast<DWORD*>(threadAddress) = thread;
        frameWindow = window;
    }
    return TRUE;
}

DWORD WINAPI WaitUntilReady(void*)
{
    report = _fsopen((directory / "smoke_report.txt").string().c_str(), "w", _SH_DENYNO);
    Info("waiting");
    for (int attempt = 0; attempt < 480; ++attempt)
    {
        DWORD uiThread = 0;
        EnumWindows(FindEditorWindow, reinterpret_cast<LPARAM>(&uiThread));
        auto editor = *reinterpret_cast<unsigned char**>(kEditor);
        if (uiThread && editor && *reinterpret_cast<void**>(editor + 0x130))
        {
            Sleep(5000);
            hook = SetWindowsHookExA(WH_CALLWNDPROC, OnMessage, nullptr, uiThread);
            DWORD_PTR result = 0;
            // A UI thread still busy starting up counts as hung and the message is not sent;
            // with several editors starting at once that can last a while, so try again.
            for (int tries = 0; hook && tries < 60; ++tries)
            {
                if (SendMessageTimeoutA(frameWindow, kRun, 0, 0, SMTO_ABORTIFHUNG, 1800000, &result)) return 0;
                if (!hook) return 0; // the run started, and unhooked itself
                Info("startup-retry", "error " + std::to_string(GetLastError()));
                Sleep(3000);
            }
            Fail("startup", "could not run on the UI thread (error " + std::to_string(GetLastError()) + ")");
            return 1;
        }
        Sleep(250);
    }
    Fail("startup", "editor never became ready");
    if (report) fclose(report);
    report = nullptr;
    return 1;
}
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        directory = std::filesystem::path(path).parent_path();
        if (HANDLE worker = CreateThread(nullptr, 0, WaitUntilReady, nullptr, 0, nullptr)) CloseHandle(worker);
    }
    return TRUE;
}
