// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <Windows.h>
#include <memory>
#include "logger.h"
#include "Version.h"
#include "Rendering.h"
#include "Debug.h"
#include "UI.h"
#include "General.h"
#include "Shadows.h"
#include "SoundBrowser.h"
#include "TextureBrowser.h"
#include "GEWireframeFix.h"
#include "GEKeybindSwap.h"
#include "LightmapFix.h"
#include "RealtimeFix.h"
#include "SaveBackups.h"
#include "ReloadedOptions.h"
#include "DialogFix.h"
#include "BrowserWindowClose.h"
#include "BrowserOpenDir.h"
#include "AnimationBrowser.h"
#include "AmbientSoundZone.h"
#include "WindowDriftFix.h"
#include "DeintersectFix.h"
#include "MapUnlock.h"
#include "MeasureTool.h"
#include "StaticMeshCollisionFix.h"
#include "StaticMeshBrowserFavorites.h"
#include "SoundBrowserFavorites.h"
#include "BspTextureClipboard.h"
#include "DdsImportFix.h"
#include "LightCullFix.h"
#include "BspLeafLightFix.h"
#include "BspCollisionFix.h"
#include "ProjectorDetachFix.h"
#include "SizingBoxFix.h"
#include "PackageLoadFix.h"
#include "MapRecovery.h"
#include "CrashDiagnostics.h"
#include "CrashRecovery.h"
#include "BspDiagnostics.h"
#include "LightmapPacker.h"
#include "RebuildAllMaps.h"
#include "ViewportConfigFix.h"
#include "EngineLog.h"
#include "MapCheckLog.h"
#include "WorkflowTools.h"
#include "PasteFix.h"
#include "LargeAddress.h"
#include "PropertySearch.h"
#include "EmitterPreview.h"
#include "SelfUpdater.h"
#include "AssetNameGloss.h"
#include "FavoritesWindow.h"
#include "Appearance.h"

INIT_ONCE g_InitOnce = INIT_ONCE_STATIC_INIT;
HINSTANCE g_hReloadedDll = nullptr;

std::wstring GetDllPath(HINSTANCE hModule) {
    std::vector<wchar_t> pathBuffer(MAX_PATH);
    const DWORD result = GetModuleFileName(hModule, pathBuffer.data(), pathBuffer.size());
    if (result == 0) {
        return L"";
    }

    pathBuffer.resize(result);
    return std::wstring(pathBuffer.begin(), pathBuffer.end());
}

static std::wstring GetExecutableDirectory(std::wstring executablePath) {
    std::wstring::size_type pos = std::wstring(executablePath).find_last_of(L"\\/");
    return std::wstring(executablePath).substr(0, pos);
}

// The engine's package lookup (ChaosTheory_Editor+0x19F300) reads '[' as the
// start of a Name[Platform] tag and cuts the path there, so every map opened
// by absolute path finds no file and the open map stays as it was. LoadPackage
// used to leave that load's BeginLoad unbalanced, and check(GObjBeginLoadCount
// ==0) then closed the editor on the next tick; PackageLoadFix ends the load
// now, so the map just does not open.
static DWORD WINAPI BracketedInstallWarning(LPVOID parameter)
{
    std::unique_ptr<std::wstring> directory(static_cast<std::wstring*>(parameter));
    const std::wstring message =
        L"The editor is installed in a folder whose path contains '[':\n\n" + *directory +
        L"\n\nThe engine treats '[' in a file path as a platform tag and cuts the path there, "
        L"so maps will not open: the map already open stays open.\n\n"
        L"Close the editor and rename the folder without square brackets.";
    MessageBoxW(nullptr, message.c_str(), L"" RE_PLUS_NAME,
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    return 0;
}

static void WarnIfInstallPathBracketed(const std::wstring& directory)
{
    if (directory.find(L'[') == std::wstring::npos)
        return;

    Logger::log(L"InstallPath: '[' in " + directory + L" - maps opened by absolute path will fail to load");
    // DllMain holds the loader lock; the dialog gets its own thread.
    HANDLE h = CreateThread(nullptr, 0, BracketedInstallWarning, new std::wstring(directory), 0, nullptr);
    if (h) CloseHandle(h);
}

void RedirectToConsole()
{
#ifdef _DEBUG
    AllocConsole();
#else
    return;
#endif
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONIN$", "r", stdin);
    freopen_s(&fp, "CONOUT$", "w", stderr);

    std::cout << RE_PLUS_DISPLAY_VERSION " injected successfully" << "\n";
}

// Custom unhandled exception filter
LONG WINAPI CustomUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
    // The BSP handler normally captures these faults at first chance, before
    // Unreal's guard/unguard chain consumes them. Calling it again here is a
    // safe fallback for failures raised outside that guarded path.
    BspDiagnostics::LogException(exceptionInfo);
    CrashDiagnostics::LogUnhandledException(exceptionInfo);
    // The process exits from here; the next start should offer the autosave.
    CrashRecovery::NoteCrash();
    return EXCEPTION_EXECUTE_HANDLER;
}

// A first-chance handler runs on whatever stack the fault left behind. When the
// fault is the stack running out, the diagnostics must not try to use it.
static bool StackHeadroom(EXCEPTION_POINTERS* exceptionInfo, size_t needed)
{
    if (exceptionInfo && exceptionInfo->ExceptionRecord
        && exceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
        return false;
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    char probe = 0;
    return reinterpret_cast<ULONG_PTR>(&probe) > low
        && reinterpret_cast<ULONG_PTR>(&probe) - low > needed;
}

LONG CALLBACK RecoveryActorTickExceptionHandler(
    EXCEPTION_POINTERS* exceptionInfo)
{
    if (!StackHeadroom(exceptionInfo, 48 * 1024))
        return EXCEPTION_CONTINUE_SEARCH;
    BspDiagnostics::LogException(exceptionInfo);
    MapRecovery::LogActorTickException(exceptionInfo);
    MapRecovery::LogViewportException(exceptionInfo);
    return EXCEPTION_CONTINUE_SEARCH;
}

const int BaseAddress = 0x10900000;
BOOL CALLBACK InitFunction(PINIT_ONCE InitOnce, PVOID Parameter, PVOID* Context) {
    HMODULE hModule = static_cast<HMODULE>(Parameter);
    auto dllPath = GetDllPath(hModule);
    auto directoryPath = GetExecutableDirectory(dllPath);

    RedirectToConsole();

    Logger::Initialize(dllPath);
    Logger::log("");
    Logger::log(RE_PLUS_DISPLAY_VERSION);
    CrashDiagnostics::Initialize(dllPath);
    // Before anything makes a window: the modern themes patch the window calls.
    Appearance::Initialize();
    WarnIfInstallPathBracketed(directoryPath);

    Rendering::Initialize();
    UI::Initialize();
    General::Initialize();
    Shadows::Initialize();
    SoundBrowser::Initialize();
    SoundBrowserFavorites::Initialize();
    TextureBrowser::Initialize();
    StaticMeshBrowserFavorites::Initialize();
    AssetNameGloss::Initialize();
    FavoritesWindow::Initialize();
    BrowserWindowClose::Initialize();
    BspTextureClipboard::Initialize();
    GEWireframeFix::Initialize();
    LightmapFix::Initialize();
    SaveBackups::Initialize();
    ReloadedOptions::Initialize();
    GEKeybindSwap::Initialize();
    RealtimeFix::Initialize();
    DialogFix::Initialize();
    BrowserOpenDir::Initialize();
    AnimationBrowser::Initialize();
    AmbientSoundZone::Initialize();
    WindowDriftFix::Initialize();
    DeintersectFix::Initialize();
    MapUnlock::Initialize();
    MeasureTool::Initialize();
    StaticMeshCollisionFix::Initialize();
    DdsImportFix::Initialize();
    LightCullFix::Initialize();
    BspLeafLightFix::Initialize();
    BspCollisionFix::Initialize();
    MapRecovery::InitializeLightingProtection();
    ProjectorDetachFix::Initialize();
    SizingBoxFix::Initialize();
    PackageLoadFix::Initialize();
    LightmapPacker::Initialize();
    RebuildAllMaps::Initialize();
    ViewportConfigFix::Initialize();
    EngineLog::Initialize();
    MapCheckLog::Initialize();
    WorkflowTools::Initialize();
    EmitterPreview::Initialize();
    BspDiagnostics::Initialize(dllPath);
    PasteFix::Initialize();
    PropertySearch::Initialize();
    SelfUpdater::Initialize(dllPath);
    LargeAddress::Initialize();

#ifdef _DEBUG
    Debug::Initialize();
#endif

    AddVectoredExceptionHandler(1, RecoveryActorTickExceptionHandler);
    MapRecovery::ArmActorTickDiagnostic();
    SetUnhandledExceptionFilter(CustomUnhandledExceptionFilter);

    return TRUE;
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH:
            g_hReloadedDll = hModule;
            InitOnceExecuteOnce(&g_InitOnce, InitFunction, hModule, NULL);
            break;
        case DLL_PROCESS_DETACH:
            // The process is exiting normally (lpReserved is set): the frame's
            // destruction has usually ended the session already.
            if (lpReserved) CrashRecovery::EndSession();
            break;
    }
    return TRUE;
}
