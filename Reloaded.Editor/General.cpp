#include "pch.h"
#include "General.h"
#include "BspTransientRenderFix.h"
#include "ShadowEdgeCacheFix.h"
#include "Hooks.h"
#include "ReloadedOptions.h"
#include "RealtimeFix.h"
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include "MemoryWriter.h"
#include "AnimationBrowser.h"
#include "MapRecovery.h"
#include "WindowDriftFix.h"
#include "RebuildAllMaps.h"
#include "LightingBudget.h"
#include "BspTextureClipboard.h"
#include "WorkflowTools.h"
#include "GridSizeShortcut.h"
#include "PlayLevelConfig.h"
#include "PlayLevelCommand.h"
#include "ViewportKeysModel.h"
#include <mimalloc.h>
#include <cstring>
#include <wincrypt.h>
#include <filesystem>
#include <fstream>
#pragma comment(lib, "advapi32.lib")
#include <unordered_map>

INIT_HOOKS;

JMP_HOOK(0x110518AD, RemoveAudioSizeLimit) {
    static int Return = 0x110518B3;
    __asm {
        jmp dword ptr[Return]
    }
}

void InstallMemoryHooks() {
    uintptr_t fn_ptr;

    fn_ptr = reinterpret_cast<uintptr_t>(mi_malloc);
    MemoryWriter::WriteBytes(0x11AF2114, &fn_ptr, sizeof(fn_ptr));
    fn_ptr = reinterpret_cast<uintptr_t>(mi_free);
    MemoryWriter::WriteBytes(0x11AF209C, &fn_ptr, sizeof(fn_ptr));
    fn_ptr = reinterpret_cast<uintptr_t>(mi_realloc);
    MemoryWriter::WriteBytes(0x11AF21F0, &fn_ptr, sizeof(fn_ptr));
    fn_ptr = reinterpret_cast<uintptr_t>(mi_calloc);
    MemoryWriter::WriteBytes(0x11AF2098, &fn_ptr, sizeof(fn_ptr));
    fn_ptr = reinterpret_cast<uintptr_t>(mi_strdup);
    MemoryWriter::WriteBytes(0x11AF21C0, &fn_ptr, sizeof(fn_ptr));
}

// Play Map minimizes the editor by calling CloseWindow. Route that call
// through this wrapper so minimizing only happens when enabled.
static BOOL WINAPI ReloadedCloseWindow(HWND hWnd)
{
    if (g_ReloadedMinimizeOnPlay)
        return CloseWindow(hWnd);
    return TRUE;
}

static void InstallMinimizeOnPlayHook()
{
    uintptr_t fn_ptr = reinterpret_cast<uintptr_t>(ReloadedCloseWindow);
    MemoryWriter::WriteBytes(0x11AF23CC, &fn_ptr, sizeof(fn_ptr));
}

// Force Play Map's launch HWND argument to 0 so the game opens in its own
// window instead of reparenting into the editor.
static void InstallNoEmbedOnPlayPatch()
{
    const uint8_t patch[] = { 0xB9, 0x00, 0x00, 0x00, 0x00, 0x90 };
    MemoryWriter::WriteBytes(0x10E2131A, patch, sizeof(patch));
}

// This legacy Reloaded launcher passes WinMain's arguments directly to
// CreateProcess as the child's entire command line. The game consumes the map
// URL as argv[0] and then attempts to load HWND=0. New launchers may already
// supply argv[0], so restrict the compatibility shim to the verified old binary.
static bool IsLegacyPlayLauncher(const std::filesystem::path& path)
{
    static constexpr unsigned char expected[] = {
        0x03,0xcf,0x76,0x57,0x3f,0xb1,0x5b,0x0f,0x35,0x7f,0x11,0xf1,0x5d,0xd4,0xc3,0xf9,
        0xab,0x31,0xe0,0xb0,0xd1,0x81,0xa1,0x49,0x3e,0x11,0x50,0xe5,0x2b,0xdc,0x25,0x86
    };
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    HCRYPTPROV provider{};
    HCRYPTHASH hash{};
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
    bool ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    char buffer[16384];
    while (ok && file) {
        file.read(buffer, sizeof(buffer));
        const auto count = file.gcount();
        if (count) ok = CryptHashData(hash, reinterpret_cast<const BYTE*>(buffer), static_cast<DWORD>(count), 0) != FALSE;
    }
    unsigned char actual[32]{};
    DWORD size = sizeof(actual);
    ok = ok && file.eof() && CryptGetHashParam(hash, HP_HASHVAL, actual, &size, 0)
        && size == sizeof(expected) && memcmp(actual, expected, sizeof(expected)) == 0;
    if (hash) CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    return ok;
}

static HINSTANCE WINAPI LaunchPlayLevel(HWND window, LPCSTR operation, LPCSTR file,
    LPCSTR parameters, LPCSTR directory, INT show)
{
    try {
        if (file && parameters && _strnicmp(parameters, "Autoplay.sdc?", 13) == 0
            && strstr(parameters, "?Editeur=true")) {
            std::filesystem::path path(file);
            if (_stricmp(path.filename().string().c_str(), "SCCT_Versus.exe") == 0) {
                if (path.is_relative()) {
                    if (directory && *directory) path = std::filesystem::path(directory) / path;
                    else {
                        char module[MAX_PATH]{};
                        GetModuleFileNameA(nullptr, module, MAX_PATH);
                        path = std::filesystem::path(module).parent_path() / path;
                    }
                }
                std::string corrected(parameters);
                if (!PlayLevelCommand::HasConfigurationOverride(parameters)) {
                    const auto gameDirectory = path.parent_path();
                    const auto options = (gameDirectory / "Reloaded_Editor.ini").string();
                    const auto resolution = PlayLevelConfig::SelectResolution(
                        GetPrivateProfileIntA("PlayLevel", "ResolutionX", 0, options.c_str()),
                        GetPrivateProfileIntA("PlayLevel", "ResolutionY", 0, options.c_str()),
                        PlayLevelConfig::CurrentDisplayResolution(window ? window : GetActiveWindow()));
                    const auto source = (gameDirectory / "Default.ini").string();
                    const auto configuration = (gameDirectory / "Reloaded_PlayLevel.ini").string();
                    if (!PlayLevelConfig::WriteConfiguration(source.c_str(), configuration.c_str(), resolution)) {
                        Logger::log("Play Level: could not prepare resolution settings, error "
                            + std::to_string(GetLastError()));
                        return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(SE_ERR_ACCESSDENIED));
                    }
                    corrected += " INI=\"" + configuration + "\"";
                    Logger::log("Play Level: render resolution " + std::to_string(resolution.width)
                        + "x" + std::to_string(resolution.height));
                }
                if (IsLegacyPlayLauncher(path)) {
                    corrected = "\"SCCT Versus\" " + corrected;
                    Logger::log("Play Level: supplied executable argument for the legacy Reloaded launcher.");
                }
                return ShellExecuteA(window, operation, file, corrected.c_str(), directory, show);
            }
        }
    } catch (const std::exception& e) {
        Logger::log(std::string("Play Level: launcher compatibility check failed: ") + e.what());
    }
    return ShellExecuteA(window, operation, file, parameters, directory, show);
}

static void InstallLegacyPlayLaunchHook()
{
    const auto function = reinterpret_cast<uintptr_t>(&LaunchPlayLevel);
    MemoryWriter::WriteBytes(0x11AF228C, &function, sizeof(function));
}

static const char s_github_url[] = "https://github.com/Lumbridge/SCCT_Versus_Reloaded_Editor";
static const char s_wiki_url[]    = "https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki";

static void __cdecl OpenURL(const char* url)
{
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

static void __cdecl OpenReloadedOptions()
{
    ShowReloadedOptionsDialog(GetActiveWindow());
}

static void __cdecl OpenAnimationBrowser()
{
    AnimationBrowser::Show(GetActiveWindow());
}

static void __cdecl ResetPropertyWindows()
{
    WindowDriftFix::ResetPropertyWindowPositions(GetActiveWindow());
}

static void __cdecl RecoverCompiledMap()
{
    MapRecovery::Run(GetActiveWindow());
}

static void __cdecl OpenRecoveredMap()
{
    MapRecovery::OpenRecovered(GetActiveWindow());
}

static void __cdecl ExportRecoveredBrushes()
{
    MapRecovery::ExportRecoveredBrushes(GetActiveWindow());
}

static void __cdecl RecoverCompiledMapAsEditable()
{
    MapRecovery::RunEditable(GetActiveWindow());
}

static bool __cdecl HandleRecoveredMapSave(UINT commandId)
{
    return MapRecovery::HandleSaveCommand(commandId);
}

// The D and J key-ups go by their press when a level viewport saw it (GridSizeShortcut's hook).
static ViewportKeys::KeyUps g_viewportKeyUps;

void General::ViewportKeyPressed(const ViewportKeys::Chord& press)
{
    g_viewportKeyUps.Pressed(press);
}

static ViewportKeys::Chord HeldChord(unsigned key)
{
    return { key, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0,
             GetKeyState(VK_MENU) < 0 };
}

// D's key-up: kept from the stock handler while Ctrl is down, and a Ctrl+D the frame got
// itself (the viewport duplicated the ones it saw on the press) duplicates now.
static bool __cdecl CtrlDKeyUpTaken()
{
    const ViewportKeys::Chord keyUp = HeldChord('D');
    if (g_viewportKeyUps.DuplicateOnKeyUp(keyUp))
        General::DuplicateSelection();
    return keyUp.ctrl;
}

static bool __cdecl GameViewKeyUpWanted()
{
    return g_viewportKeyUps.GameViewOnKeyUp(HeldChord('J'));
}

// Build All and the lighting rebuilds go through the Lighting Budget check
// first; false cancels the command.
static bool __cdecl LightingBudgetAllowsBuild(UINT command)
{
    return LightingBudget::AllowBuild(command);
}

static void __cdecl OpenRebuildAllMaps()
{
    RebuildAllMaps::Show(GetActiveWindow());
}

// Game View (J) - Simulates the in-game view in the viewport
static const uint32_t kGEditor            = 0x1165dfa0;
static const uint32_t kGWarn              = 0x115befb0;
static const uint32_t kEditor_Level       = 0x130;
static const uint32_t kEditor_RedrawVtbl  = 0xE8;   // RedrawLevel(ULevel*)
static const uint32_t kActor_Flags        = 0x2E8;  // dword holding bHidden
static const uint32_t kMask_Hidden        = 0x1000; // its bit
static const uint32_t kActor_Texture      = 0x228;  // editor icon sprite
static const uint32_t kActor_DrawType     = 0x2D9;
static const uint8_t  kDrawType_Particle  = 10;
static const uint32_t kObj_Class          = 0x24;
static const uint32_t kObj_Name           = 0x20;
static const uint32_t kClass_Super        = 0x28;
static const uint32_t kGNames             = 0x1169cfbc;

// Manual exclusion for SComputerObjectiveTrigger because it has bHidden=true but is visible in game
static const char* const kGameViewKeep[] = { "SComputerObjectiveTrigger" };

// Visible in game (corona, light beam, rain) but the editor also billboards their icon
static const char* const kGameViewIconOnly[] = { "Light", "ERainVolume" };

void General::DuplicateSelection()
{
    static constexpr char kCommand[] = "ACTOR DUPLICATE";

    void* editor = *reinterpret_cast<void**>(kGEditor);
    void* output = *reinterpret_cast<void**>(kGWarn);
    if (!editor || !output)
        return;

    // UUnrealEdEngine exposes its FExec interface at +0x28. Calling the
    // interface's first virtual method mirrors the editor's Duplicate menu
    // command, including transactions, multi-selection and brush handling.
    void* execInterface = static_cast<char*>(editor) + 0x28;
    void** vtable = *reinterpret_cast<void***>(execInterface);
    if (!vtable || !vtable[0])
        return;

    using ExecFn = int(__thiscall*)(void*, const char*, void*);
    reinterpret_cast<ExecFn>(vtable[0])(execInterface, kCommand, output);
}

static bool ExecEditorCommand(const char* command)
{
    void* editor = *reinterpret_cast<void**>(kGEditor);
    void* output = *reinterpret_cast<void**>(kGWarn);
    if (!editor || !output)
        return false;

    void* execInterface = static_cast<char*>(editor) + 0x28;
    void** vtable = *reinterpret_cast<void***>(execInterface);
    if (!vtable || !vtable[0])
        return false;

    using ExecFn = int(__thiscall*)(void*, const char*, void*);
    return reinterpret_cast<ExecFn>(vtable[0])(
        execInterface, command, output) != 0;
}

static constexpr char kDefaultBuilderCube[] =
    "Begin PolyList\r\n"
    "   Begin Polygon Item=DefaultCube\r\n"
    "      Origin   +00128.000000,-00128.000000,-00128.000000\r\n"
    "      Normal   +00000.000000,-00001.000000,+00000.000000\r\n"
    "      TextureU -00001.000000,+00000.000000,+00000.000000\r\n"
    "      TextureV +00000.000000,+00000.000000,-00001.000000\r\n"
    "      Vertex   +00128.000000,-00128.000000,-00128.000000\r\n"
    "      Vertex   +00128.000000,-00128.000000,+00128.000000\r\n"
    "      Vertex   -00128.000000,-00128.000000,+00128.000000\r\n"
    "      Vertex   -00128.000000,-00128.000000,-00128.000000\r\n"
    "   End Polygon\r\n"
    "   Begin Polygon Item=DefaultCube\r\n"
    "      Origin   -00128.000000,-00128.000000,-00128.000000\r\n"
    "      Normal   -00001.000000,+00000.000000,+00000.000000\r\n"
    "      TextureU +00000.000000,+00001.000000,+00000.000000\r\n"
    "      TextureV +00000.000000,+00000.000000,-00001.000000\r\n"
    "      Vertex   -00128.000000,-00128.000000,-00128.000000\r\n"
    "      Vertex   -00128.000000,-00128.000000,+00128.000000\r\n"
    "      Vertex   -00128.000000,+00128.000000,+00128.000000\r\n"
    "      Vertex   -00128.000000,+00128.000000,-00128.000000\r\n"
    "   End Polygon\r\n"
    "   Begin Polygon Item=DefaultCube\r\n"
    "      Origin   -00128.000000,+00128.000000,-00128.000000\r\n"
    "      Normal   +00000.000000,+00001.000000,+00000.000000\r\n"
    "      TextureU +00001.000000,+00000.000000,+00000.000000\r\n"
    "      TextureV +00000.000000,+00000.000000,-00001.000000\r\n"
    "      Vertex   -00128.000000,+00128.000000,-00128.000000\r\n"
    "      Vertex   -00128.000000,+00128.000000,+00128.000000\r\n"
    "      Vertex   +00128.000000,+00128.000000,+00128.000000\r\n"
    "      Vertex   +00128.000000,+00128.000000,-00128.000000\r\n"
    "   End Polygon\r\n"
    "   Begin Polygon Item=DefaultCube\r\n"
    "      Origin   +00128.000000,+00128.000000,-00128.000000\r\n"
    "      Normal   +00001.000000,+00000.000000,+00000.000000\r\n"
    "      TextureU +00000.000000,-00001.000000,+00000.000000\r\n"
    "      TextureV +00000.000000,+00000.000000,-00001.000000\r\n"
    "      Vertex   +00128.000000,+00128.000000,-00128.000000\r\n"
    "      Vertex   +00128.000000,+00128.000000,+00128.000000\r\n"
    "      Vertex   +00128.000000,-00128.000000,+00128.000000\r\n"
    "      Vertex   +00128.000000,-00128.000000,-00128.000000\r\n"
    "   End Polygon\r\n"
    "   Begin Polygon Item=DefaultCube\r\n"
    "      Origin   -00128.000000,-00128.000000,+00128.000000\r\n"
    "      Normal   +00000.000000,+00000.000000,+00001.000000\r\n"
    "      TextureU +00000.000000,+00001.000000,+00000.000000\r\n"
    "      TextureV -00001.000000,+00000.000000,+00000.000000\r\n"
    "      Vertex   -00128.000000,-00128.000000,+00128.000000\r\n"
    "      Vertex   +00128.000000,-00128.000000,+00128.000000\r\n"
    "      Vertex   +00128.000000,+00128.000000,+00128.000000\r\n"
    "      Vertex   -00128.000000,+00128.000000,+00128.000000\r\n"
    "   End Polygon\r\n"
    "   Begin Polygon Item=DefaultCube\r\n"
    "      Origin   +00128.000000,-00128.000000,-00128.000000\r\n"
    "      Normal   +00000.000000,+00000.000000,-00001.000000\r\n"
    "      TextureU +00000.000000,+00001.000000,+00000.000000\r\n"
    "      TextureV +00001.000000,+00000.000000,+00000.000000\r\n"
    "      Vertex   +00128.000000,-00128.000000,-00128.000000\r\n"
    "      Vertex   -00128.000000,-00128.000000,-00128.000000\r\n"
    "      Vertex   -00128.000000,+00128.000000,-00128.000000\r\n"
    "      Vertex   +00128.000000,+00128.000000,-00128.000000\r\n"
    "   End Polygon\r\n"
    "End PolyList\r\n";

static bool WriteDefaultBuilderCube(char* path, size_t pathSize)
{
    char tempDirectory[MAX_PATH] = {};
    char seedPath[MAX_PATH] = {};
    if (!GetTempPathA(static_cast<DWORD>(std::size(tempDirectory)),
                      tempDirectory)
        || !GetTempFileNameA(tempDirectory, "RBB", 0, seedPath))
        return false;

    DeleteFileA(seedPath);
    char* extension = strrchr(seedPath, '.');
    if (!extension)
        return false;
    strcpy_s(extension, static_cast<size_t>(seedPath + sizeof(seedPath) - extension),
             ".t3d");

    HANDLE file = CreateFileA(seedPath, GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(sizeof(kDefaultBuilderCube) - 1);
    const bool success = WriteFile(file, kDefaultBuilderCube, size,
                                   &written, nullptr) != FALSE
        && written == size;
    CloseHandle(file);
    if (!success)
    {
        DeleteFileA(seedPath);
        return false;
    }

    strncpy_s(path, pathSize, seedPath, _TRUNCATE);
    return true;
}

void General::RebuildBuilderBrushAsDefaultCube()
{
    char brushFile[MAX_PATH] = {};
    if (!WriteDefaultBuilderCube(brushFile, std::size(brushFile)))
        throw std::runtime_error("Could not create the temporary default-cube brush file.");

    char importCommand[MAX_PATH + 32] = {};
    _snprintf_s(importCommand, sizeof(importCommand), _TRUNCATE,
                "BRUSH IMPORT FILE=\"%s\"", brushFile);
    const bool reset = ExecEditorCommand("BRUSH RESET");
    const bool imported = ExecEditorCommand(importCommand);
    DeleteFileA(brushFile);

    if (!reset || !imported)
        throw std::runtime_error("The editor did not accept the builder-brush rebuild command.");
}

static void __cdecl RebuildDefaultBuilderBrush()
{
    try { General::RebuildBuilderBrushAsDefaultCube(); }
    catch (const std::exception& e)
    {
        MessageBoxA(GetActiveWindow(), e.what(), "Rebuild Builder Brush",
                    MB_OK | MB_ICONERROR);
    }
}

static bool g_gameView = false;

enum : uint8_t { GV_None = 0, GV_IconOnly = 1, GV_Keep = 2 };
static std::unordered_map<void*, uint8_t> g_gameViewClassCache;

// Hides the rain volume wireframe but keeps the rain visible
JMP_HOOK(0x1114aa20, RainVolumeBoundsHook)
{
    static int s_resume = 0x1114aa25;

    __asm
    {
        cmp  byte ptr [g_gameView], 0
        jnz  skip_bounds

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]

    skip_bounds:
        ret  8
    }
}

// AActor::RenderEditorInfo draws overlays used by the editor (icons, radii, etc.)
JMP_HOOK(0x11191110, ActorEditorInfoHook)
{
    static int s_resume = 0x11191115;

    __asm
    {
        cmp  byte ptr [g_gameView], 0
        jnz  skip_info

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]

    skip_info:
        ret  0xc
    }
}

// Skip ULevel::RenderGEs in the editor viewport to hide GE lines without affecting hit-testing
JMP_HOOK(0x10eced35, GEViewportRenderHook)
{
    static int s_renderGEs = 0x11119b20;
    static int s_resume    = 0x10eced3a;

    __asm
    {
        cmp  byte ptr [g_gameView], 0
        jnz  skip_ge

        call dword ptr [s_renderGEs]   // callee cleans its arg
        jmp  dword ptr [s_resume]

    skip_ge:
        add  esp, 4                    // drop the pushed render context
        jmp  dword ptr [s_resume]
    }
}

static bool GV_ClassChainHas(void* cls, const char* want)
{
    int* gnames = *reinterpret_cast<int**>(kGNames);
    if (!gnames) return false;

    for (int depth = 0; cls && depth < 16; ++depth)
    {
        int entry = gnames[*reinterpret_cast<int*>(static_cast<char*>(cls) + kObj_Name)];
        if (entry && _stricmp(reinterpret_cast<const char*>(entry + 0x0C), want) == 0)
            return true;
        cls = *reinterpret_cast<void**>(static_cast<char*>(cls) + kClass_Super);
    }
    return false;
}

static uint8_t GV_ClassifyClass(void* cls)
{
    auto it = g_gameViewClassCache.find(cls);
    if (it != g_gameViewClassCache.end())
        return it->second;

    uint8_t kind = GV_None;
    for (const char* keep : kGameViewKeep)
        if (GV_ClassChainHas(cls, keep)) { kind = GV_Keep; break; }

    if (kind == GV_None)
        for (const char* icon : kGameViewIconOnly)
            if (GV_ClassChainHas(cls, icon)) { kind = GV_IconOnly; break; }

    g_gameViewClassCache[cls] = kind;
    return kind;
}

// Emitters, coronas, and light beams are visible in game, so only their editor icon is suppressed
static bool GV_IsIconOnly(void* actor)
{
    if (*reinterpret_cast<uint8_t*>(static_cast<char*>(actor) + kActor_DrawType) == kDrawType_Particle)
        return true;
    void* cls = *reinterpret_cast<void**>(static_cast<char*>(actor) + kObj_Class);
    return cls && GV_ClassifyClass(cls) == GV_IconOnly;
}

static int __cdecl GV_ShouldHide(void* actor)
{
    if (!g_gameView || !actor) return 0;

    DWORD flags = *reinterpret_cast<DWORD*>(static_cast<char*>(actor) + kActor_Flags);
    if (!(flags & kMask_Hidden)) return 0;

    if (GV_IsIconOnly(actor)) return 0;

    void* cls = *reinterpret_cast<void**>(static_cast<char*>(actor) + kObj_Class);
    if (cls && GV_ClassifyClass(cls) == GV_Keep) return 0;

    return 1;
}

static int __cdecl GV_SkipSprite(void* actor)
{
    if (!actor) return 1;
    if (!*reinterpret_cast<void**>(static_cast<char*>(actor) + kActor_Texture)) return 1; // engine assumes non-null
    return (g_gameView && GV_IsIconOnly(actor)) ? 1 : 0;
}

// FLevelSceneNode::FilterActor(AActor*) handles actor visibility for rendering and hit-testing
JMP_HOOK(0x110a15e0, SceneNodeFilterActorHook)
{
    static int s_resume = 0x110a15e6;

    __asm
    {
        cmp  byte ptr [g_gameView], 0
        jz   pass_through

        push ecx                           // this
        push dword ptr [esp + 8]           // actor
        call GV_ShouldHide
        add  esp, 4
        pop  ecx
        test eax, eax
        jnz  hide_actor

    pass_through:
        mov  eax, dword ptr [ecx + 4]      // replay overwritten prologue
        mov  ecx, dword ptr [eax + 0x30]
        jmp  dword ptr [s_resume]

    hide_actor:
        xor  eax, eax
        ret  4
    }
}

// DrawSprite assumes Actor->Texture is non-null, and in Game View icon-only actors skip their icon
JMP_HOOK(0x110a3270, DrawSpriteNullTextureFix)
{
    static int s_resume = 0x110a3275;

    __asm
    {
        push ecx
        push edx
        push dword ptr [esp + 12]          // the actor
        call GV_SkipSprite
        add  esp, 4
        pop  edx
        pop  ecx
        test eax, eax
        jnz  skip_sprite

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]

    skip_sprite:
        ret                                // caller cleans the arg
    }
}

// Recovery still brackets native saves; Game View no longer changes saved actor data.
static void __cdecl BeginRecoverySavePackage(uintptr_t returnAddress)
{
    MapRecovery::BeginSavePackage(returnAddress);
}

static uintptr_t __cdecl EndRecoverySavePackage()
{
    return MapRecovery::EndSavePackage();
}

JMP_HOOK(0x10fb2610, RecoverySavePackageHook)
{
    static int s_resume = 0x10fb2615;
    static uintptr_t s_recoveryReturnAddress = 0;

    __asm
    {
        mov  eax, dword ptr [esp]
        push eax
        call BeginRecoverySavePackage
        add  esp, 4
        mov  dword ptr [esp], offset recovery_save_complete

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]

    recovery_save_complete:
        pushad
        call EndRecoverySavePackage
        mov  dword ptr [s_recoveryReturnAddress], eax
        popad
        push dword ptr [s_recoveryReturnAddress]
        ret
    }
}

static void __cdecl ToggleGameView()
{
    void* gEditor = *reinterpret_cast<void**>(kGEditor);
    if (!gEditor) return;

    void* level = *reinterpret_cast<void**>(static_cast<char*>(gEditor) + kEditor_Level);
    if (!level) return;

    g_gameView = !g_gameView;
    g_gameViewClassCache.clear();   // class pointers may have been recycled by a map load

    void* vtable = *reinterpret_cast<void**>(gEditor);
    void* redraw = *reinterpret_cast<void**>(static_cast<char*>(vtable) + kEditor_RedrawVtbl);
    __asm {
        mov  ecx, gEditor
        push level
        mov  eax, redraw
        call eax
    }
}

JMP_HOOK(0x10e57b30, MenuBarDispatch)
{
    static int s_continue = 0x10e57b35;
    static bool s_recoverySaveHandled = false;
    static bool s_lightingBuildAllowed = true;

    __asm {
        cmp dword ptr [esp+4], 40927
        je workflow_dispatch
        cmp dword ptr [esp+4], 40928 // Package Map for Sharing
        je workflow_dispatch
        cmp dword ptr [esp+4], 40929 // Explicit lighting recalculation
        je workflow_dispatch
        cmp dword ptr [esp+4], 40930 // Selected lighting recalculation
        je workflow_dispatch
        cmp dword ptr [esp+4], 40931 // Match selected BSP lighting
        je workflow_dispatch
        cmp dword ptr [esp+4], 40932 // SCamNetwork Manager
        je workflow_dispatch
        cmp dword ptr [esp+4], 40934 // Export Map to JSON
        je workflow_dispatch
        cmp dword ptr [esp+4], 40935 // Import Map from JSON
        je workflow_dispatch
        cmp dword ptr [esp+4], 40933 // Fit builder brush around static meshes
        je workflow_dispatch
        cmp dword ptr [esp+4], 40936 // Brush/surface edge snap commands
        jb workflow_legacy_range
        // Through brush fitting, vertex portals, Builder Brush > Place Here, the
        // update checks and About RE+ (up to 40986), then blocks reserved for
        // tools in progress (the stock menus stop at 40668):
        //   40987-40999 crash recovery        41000-41014 what's new / roll back
        //   41015-41029 mesh browser search   41030-41044 sound browser
        //   41045-41059 animation import      41060-41074 find usages in MapsEd
        //   41075-41089 copy/paste properties 41090-41104 measure tool
        //   41105-41119 undo history          41120-41134 assembly/view thumbnails
        //   41135-41149 emitter editing       41150-41164 lighting budget
        //   41165-41179 RE+ menu names        41180       character skins
        //   41181-41199 skin presets          41200-41219 light and shadow map
        //   41220-41239 map check             41240-41259 minimap
        //   41260-41279 placement tools       41280-41299 render budget
        //   41300-41319 map recovery          41320-41339 map JSON preview
        //   41340-41359 favorites window      41360-41379 updater / checkpoints
        //   41380-41384 spare             41385-41389 smooth staircase
        //   41390-41394 optimise map assets  41395-41399 conveyor belts
        //   41400-41419 objective player counts
        //   41420-41439 placement snap and push  41440-41459 camera bookmarks
        //   41460-41479 collision limits      41480-41499 reachability
        //   41500-41519 prop scatter          41520-41539 mover workbench
        //   41540-41559 linked prefabs        41560-41579 map outliner
        //   41580-41599 command palette       41600-41619 compare with backup
        //   41620-41639 host and client play  41640-41659 OBJ mesh import
        //   41660-41679 map OBJ export        41680-41699 light and shadow accuracy
        //   41700-41719 appearance
        cmp dword ptr [esp+4], 41719
        jbe workflow_dispatch
    workflow_legacy_range:
        cmp dword ptr [esp+4], 40920
        jb workflow_continue
        cmp dword ptr [esp+4], 40923
        ja workflow_continue
    workflow_dispatch:
        push dword ptr [esp+4]
        call WorkflowTools::HandleCommand
        add esp, 4
        retn 4
    workflow_continue:
        cmp  dword ptr [esp+4], 40066 // RE+ Options
        je   do_reloaded_options
        cmp  dword ptr [esp+4], 40067 // Show Animation Browser
        je   do_anim_browser
        cmp  dword ptr [esp+4], 40907 // Reset Property Window Positions
        je   do_reset_property_windows
        cmp  dword ptr [esp+4], 40911 // Rebuild Builder Brush as Default Cube
        je   do_reset_builder_brush
        cmp  dword ptr [esp+4], 40912 // Copy selected BSP texture
        je   do_copy_bsp_texture
        cmp  dword ptr [esp+4], 40913 // Paste copied BSP texture
        je   do_paste_bsp_texture
        cmp  dword ptr [esp+4], 40926 // Select source brushes of selected BSP faces
        je   do_select_surface_brush
        cmp  dword ptr [esp+4], 40902 // Rebuild All Maps
        je   do_rebuild_all
        cmp  dword ptr [esp+4], 40900 // RE+ on GitHub
        je   do_github
        cmp  dword ptr [esp+4], 40901 // Reloaded Wiki
        je   do_wiki
        cmp  dword ptr [esp+4], 40903 // Recover Compiled Map
        je   do_recover_map
        cmp  dword ptr [esp+4], 40904 // Open Recovered Map
        je   do_open_recovered_map
        cmp  dword ptr [esp+4], 40905 // Export Recovered BSP as Brushes
        je   do_export_recovered_brushes
        cmp  dword ptr [esp+4], 40906 // Recover Compiled Map as Editable
        je   do_recover_editable_map
        cmp  dword ptr [esp+4], 40007 // File > Save
        je   maybe_save_recovered_map
        cmp  dword ptr [esp+4], 40008 // File > Save As
        je   maybe_save_recovered_map
        cmp  dword ptr [esp+4], 40038 // Build All
        je   maybe_check_lighting_budget
        cmp  dword ptr [esp+4], 40162 // Rebuild Lighting Only
        je   maybe_check_lighting_budget
        cmp  dword ptr [esp+4], 30000 // Rebuild Changed Lighting Only
        je   maybe_check_lighting_budget

        // Fallthrough: replay overwritten prologue then continue
    continue_stock_command:
        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_continue]

    do_reloaded_options:
        call OpenReloadedOptions
        retn 4

    do_anim_browser:
        call OpenAnimationBrowser
        retn 4

    do_reset_property_windows:
        call ResetPropertyWindows
        retn 4

    do_reset_builder_brush:
        call RebuildDefaultBuilderBrush
        retn 4

    do_copy_bsp_texture:
        call BspTextureClipboard::CopySelectedTexture
        retn 4

    do_paste_bsp_texture:
        call BspTextureClipboard::PasteTexture
        retn 4

    do_select_surface_brush:
        call BspTextureClipboard::SelectBrush
        retn 4

    do_rebuild_all:
        call OpenRebuildAllMaps
        retn 4

    do_github:
        push offset s_github_url
        call OpenURL
        add  esp, 4
        retn 4

    do_wiki:
        push offset s_wiki_url
        call OpenURL
        add  esp, 4
        retn 4

    do_recover_map:
        call RecoverCompiledMap
        retn 4

    do_open_recovered_map:
        call OpenRecoveredMap
        retn 4

    do_export_recovered_brushes:
        call ExportRecoveredBrushes
        retn 4

    do_recover_editable_map:
        call RecoverCompiledMapAsEditable
        retn 4

    maybe_save_recovered_map:
        pushad
        mov  eax, dword ptr [esp+36]
        push eax
        call HandleRecoveredMapSave
        add  esp, 4
        mov  byte ptr [s_recoverySaveHandled], al
        popad
        cmp  byte ptr [s_recoverySaveHandled], 0
        je   continue_stock_command
        retn 4

    maybe_check_lighting_budget:
        pushad
        mov  eax, dword ptr [esp+36]
        push eax
        call LightingBudgetAllowsBuild
        add  esp, 4
        mov  byte ptr [s_lightingBuildAllowed], al
        popad
        cmp  byte ptr [s_lightingBuildAllowed], 0
        jne  continue_stock_command
        retn 4
    }
}

// Cooked maps retain the builder brush actor slot but strip its editor-only
// UModel. During compiled-map recovery, provide the blank editor map's model
// before UnrealEd's GetBrush() invariant is evaluated.
static void* __fastcall ResolveRecoveryBuilderBrush(void* level, void*)
{
    return MapRecovery::ResolveBuilderBrushActor(level);
}

JMP_HOOK(0x10E632B0, RecoveryBuilderBrushHook)
{
    __asm
    {
        jmp ResolveRecoveryBuilderBrush
    }
}

static bool __cdecl IsRecoveredBspModelForHooks(void* model)
{
    return MapRecovery::IsRecoveredBspModel(model);
}

static bool __cdecl UseRecoveredBspSurfaceLayoutForHooks()
{
    return MapRecovery::UsesRecoveredBspLayout();
}

static bool __cdecl SkipRecoveredEditorPolyForHooks()
{
    return MapRecovery::ShouldSkipRecoveredEditorPoly();
}

// MouseDelta snapshots every affected editor object on the first mouse hit,
// before it has decided whether the gesture is only a viewport pan. A compiled
// level can be reached through those object references, causing the undo
// archive to traverse cooked BSP with source-editor serialization rules. Keep
// that one MouseDelta snapshot disabled for recovered maps; its following null
// check already supports running without an undo archive.
JMP_HOOK(0x10EC6F92, RecoveredMouseDeltaTransactionHook)
{
    static int s_continue = 0x10EC6F98;

    __asm
    {
        pushad
        call UseRecoveredBspSurfaceLayoutForHooks
        test al, al
        popad
        jnz  without_undo

        // Replay the overwritten six-byte GUndo load for normal source maps.
        mov  ecx, dword ptr ds:[11691D6Ch]
        jmp  dword ptr [s_continue]

    without_undo:
        xor  ecx, ecx
        jmp  dword ptr [s_continue]
    }
}

// FBspSurf::Serialize shares its common fields between source and PC-runtime
// layouts, then selects whether to serialize the editor-only FPoly pointer.
// MouseDelta starts transactions before it knows whether a click will pan or
// select. That path can therefore serialize a cooked surface without going
// through UModel::ModifySurf, where the narrower transaction guard below
// lives. Transactions can serialize temporary copies rather than exact
// elements of the model's surface array, so use the PC-runtime branch for all
// BSP surface serialization while a recovered map is active. Otherwise the
// cooked +0x20 field is mistaken for an FPoly pointer and UnModel.cpp asserts
// while deserializing its vertex count.
JMP_HOOK(0x110CDADB, RecoveredBspSurfaceLayoutHook)
{
    static int s_loadConfiguredPlatform = 0x110CDAE1;
    static int s_comparePlatform = 0x110CDAE7;

    __asm
    {
        pushad
        call UseRecoveredBspSurfaceLayoutForHooks
        test al, al
        popad
        jnz  use_runtime_layout

        // Replay the complete overwritten six-byte instruction. The five-byte
        // jump replaces its first five bytes, so resume at the next boundary.
        mov  ecx, dword ptr ds:[11691D7Ch]
        jmp  dword ptr [s_loadConfiguredPlatform]

    use_runtime_layout:
        mov  eax, 1
        jmp  dword ptr [s_comparePlatform]
    }
}

// PC-runtime surfaces with either of the two editor-poly flags can contain an
// attached FPoly in the package, so MAP LOAD must follow the normal flag test.
// Later editor transactions can serialize temporary 0x2c-byte cooked-surface
// copies whose attached pointer is not safe to traverse. Skip the editor poly
// only after the recovered level has finished loading.
JMP_HOOK(0x110CDAF6, RecoveredBspSurfaceEditorPolyHook)
{
    static int s_testEditorPolyFlags = 0x110CDAFD;
    static int s_skipEditorPoly = 0x110CDAFF;

    __asm
    {
        pushad
        call SkipRecoveredEditorPolyForHooks
        test al, al
        popad
        jnz  skip_editor_poly

        // Replay the overwritten seven-byte flag test and resume at the
        // conditional branch which consumes its result.
        test dword ptr [ebx+14h], 0C000000h
        jmp  dword ptr [s_testEditorPolyFlags]

    skip_editor_poly:
        jmp  dword ptr [s_skipEditorPoly]
    }
}

// UModel::ModifySurf normally snapshots the selected FBspSurf through GUndo
// before changing it. Recovered maps contain the cooked 0x2c-byte surface
// layout, while the editor transaction serializer expects the larger source
// layout; attempting that snapshot reads stripped fields and crashes. Skip
// only the undo snapshot for the active recovered level, then resume the rest
// of ModifySurf so selection and master-surface propagation still work.
JMP_HOOK(0x110D0E37, RecoveredBspSurfaceTransactionHook)
{
    static int s_continue = 0x110D0E3D;
    static int s_skipTransaction = 0x110D0E66;

    __asm
    {
        pushad
        lea  eax, [edi-94h]
        push eax
        call IsRecoveredBspModelForHooks
        add  esp, 4
        test al, al
        popad
        jnz  skip_transaction

        // Replay the overwritten UModel::ModifySurf instruction.
        mov  ecx, dword ptr ds:[11691D6Ch]
        jmp  dword ptr [s_continue]

    skip_transaction:
        jmp  dword ptr [s_skipTransaction]
    }
}

// Cooked packages already contain render data that the runtime loader can use.
// Surface selection clears that cache before UModel::BuildRenderData rebuilds
// it through editor-only assumptions; recovered maps no longer have all of
// that source state. Preserve both the loaded cache and its geometry whenever
// the viewport requests a rebuild. Normal source maps remain unchanged.
JMP_HOOK(0x110D1230, RecoveredBspClearRenderDataHook)
{
    static int s_continue = 0x110D1235;

    __asm
    {
        pushad
        push ecx
        call IsRecoveredBspModelForHooks
        add  esp, 4
        test al, al
        popad
        jnz  keep_cooked_render_data

        // Replay the complete five-byte UModel::ClearRenderData prologue.
        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_continue]

    keep_cooked_render_data:
        ret  4
    }
}

JMP_HOOK(0x110D13D0, RecoveredBspRenderDataHook)
{
    static int s_continue = 0x110D13D5;

    __asm
    {
        pushad
        push ecx
        call IsRecoveredBspModelForHooks
        add  esp, 4
        test al, al
        popad
        jnz  keep_cooked_render_data

        // Replay the complete five-byte UModel::BuildRenderData prologue.
        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_continue]

    keep_cooked_render_data:
        ret
    }
}

static void __fastcall RepairRecoveredZoneInfo(void* actor, void*)
{
    MapRecovery::RepairZoneInfoAssignment(actor);
}

// Cooked UModel serialization can omit the editor-side ZoneActor reference.
// AZoneInfo::CheckForErrors assumes every nonzero zone already has that link
// and dereferences it while formatting an error message. Restore a missing
// association immediately before the stock validator runs.
JMP_HOOK(0x111906F0, RecoveredZoneInfoValidationHook)
{
    static int s_continue = 0x111906F5;

    __asm
    {
        pushad
        call RepairRecoveredZoneInfo
        popad

        // Replay the overwritten five-byte function prologue.
        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_continue]
    }
}

// The build dialog's geometry, BSP and lighting stages consume editor-only
// brush/FPoly state which a compiled recovery cannot contain. MAP REBUILD
// otherwise replaces the valid cooked BSP with an empty wireframe result.
// Preserve the recovered geometry and lightmaps; the following path stage is
// still allowed to validate actors and rebuild navigation data.
JMP_HOOK(0x10E10C00, RecoveredBuildGeometryHook)
{
    static int s_continue = 0x10E10C05;

    __asm
    {
        pushad
        call UseRecoveredBspSurfaceLayoutForHooks
        test al, al
        popad
        jnz  skip_build

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_continue]

    skip_build:
        ret
    }
}

JMP_HOOK(0x10E10CC0, RecoveredBuildBspHook)
{
    static int s_continue = 0x10E10CC5;

    __asm
    {
        pushad
        call UseRecoveredBspSurfaceLayoutForHooks
        test al, al
        popad
        jnz  skip_build

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_continue]

    skip_build:
        ret
    }
}

JMP_HOOK(0x10E10D80, RecoveredBuildLightingHook)
{
    static int s_continue = 0x10E10D85;

    __asm
    {
        pushad
        call UseRecoveredBspSurfaceLayoutForHooks
        test al, al
        popad
        jnz  skip_build

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_continue]

    skip_build:
        ret
    }
}

static void __cdecl BeginRecoveredActorTick(void* actor, int actorIndex)
{
    MapRecovery::RecordActorTick(actor, actorIndex);
}

static void __cdecl EndRecoveredActorTick()
{
    MapRecovery::ClearActorTick();
}

static void __cdecl BeginRecoveredActorTickLoop(void* level)
{
    MapRecovery::BeginActorTickLoop(level);
}

static void __cdecl EndRecoveredActorTickLoop()
{
    MapRecovery::EndActorTickLoop();
}

// ULevel::Tick has a normal actor-array pass and a deferred linked-list pass.
// Bracket both virtual Tick calls so a first-chance AV can be attributed to
// the actor which owns the invalid reconstructed runtime state.
JMP_HOOK(0x11184ADF, RecoveredActorTickLoopBeginHook)
{
    static int s_resume = 0x11184AE4;

    __asm
    {
        pushad
        mov  eax, dword ptr [ebp-14h]
        push eax
        call BeginRecoveredActorTickLoop
        add  esp, 4
        popad
        // Replay: mov eax, [ebp-14h]; xor ebx, ebx
        mov  eax, dword ptr [ebp-14h]
        xor  ebx, ebx
        jmp  dword ptr [s_resume]
    }
}

// Capture the array slot before ULevel::Tick reads any native AActor fields.
// The previous diagnostic started later, after the unsafe flag read.
JMP_HOOK(0x11184B03, RecoveredActorArrayFetchHook)
{
    static int s_resume = 0x11184B08;

    __asm
    {
        mov  eax, dword ptr [ecx + esi*4]
        pushad
        push esi
        push eax
        call BeginRecoveredActorTick
        add  esp, 8
        popad
        test eax, eax
        jmp  dword ptr [s_resume]
    }
}

JMP_HOOK(0x11184B53, RecoveredActorArrayTickBeginHook)
{
    static int s_resume = 0x11184B58;

    __asm
    {
        // Replay: mov ecx, [ecx + esi*4]; mov edx, [ecx]
        mov  ecx, dword ptr [ecx + esi*4]
        pushad
        push esi
        push ecx
        call BeginRecoveredActorTick
        add  esp, 8
        popad
        mov  edx, dword ptr [ecx]
        jmp  dword ptr [s_resume]
    }
}

JMP_HOOK(0x11184B66, RecoveredActorArrayTickEndHook)
{
    static int s_resume = 0x11184B6B;

    __asm
    {
        pushad
        call EndRecoveredActorTick
        popad
        // Replay: mov edx, [ecx + esi*4]; add ebx, eax
        mov  edx, dword ptr [ecx + esi*4]
        add  ebx, eax
        jmp  dword ptr [s_resume]
    }
}

JMP_HOOK(0x11184BE0, RecoveredDeferredActorTickBeginHook)
{
    static int s_resume = 0x11184BE5;

    __asm
    {
        pushad
        push -1
        push ecx
        call BeginRecoveredActorTick
        add  esp, 8
        popad
        // Replay: mov eax, [ebp+8]; mov edx, [ecx]
        mov  eax, dword ptr [ebp+8]
        mov  edx, dword ptr [ecx]
        jmp  dword ptr [s_resume]
    }
}

JMP_HOOK(0x11184BED, RecoveredDeferredActorTickEndHook)
{
    static int s_resume = 0x11184BF2;

    __asm
    {
        pushad
        call EndRecoveredActorTick
        popad
        // Replay: mov edx, [ebp-14h]; add ebx, eax
        mov  edx, dword ptr [ebp-14h]
        add  ebx, eax
        jmp  dword ptr [s_resume]
    }
}

JMP_HOOK(0x11184C30, RecoveredActorTickLoopEndHook)
{
    static int s_resume = 0x11184C35;

    __asm
    {
        pushad
        call EndRecoveredActorTickLoop
        popad
        // Replay: mov eax, dword ptr ds:[11825244h]
        mov  eax, dword ptr ds:[11825244h]
        jmp  dword ptr [s_resume]
    }
}

JMP_HOOK(0x10f00d10, ViewportKeyUpHook)
{
    static int s_resume = 0x10f00d15;

    __asm
    {
        // Ctrl+D: duplicated on the key press when the viewport saw it (GridSizeShortcut's
        // viewport hook), otherwise here; its key-up is still kept from the stock handler.
        cmp  dword ptr [esp + 4], 0x44
        jne  check_f12
        push ecx
        call CtrlDKeyUpTaken
        pop  ecx
        test al, al
        jnz  swallow

    check_f12:
        // F12: RE+ Options
        cmp  dword ptr [esp + 4], 0x7B
        je   do_f12

        // J on its own: Game View (Shift+J is the stock Select All Adjacent Surfaces)
        cmp  dword ptr [esp + 4], 0x4A
        jne  check_f7
        push ecx
        call GameViewKeyUpWanted
        pop  ecx
        test al, al
        jnz  do_game_view

    check_f7:
        // F7: Attempts to compile UnrealScript but fails (scripts are stripped). Disabled to prevent accidentally pressing F7 and crashing.
        cmp  dword ptr [esp + 4], 0x76
        je   swallow

        push ebp
        mov  ebp, esp
        push -1
        jmp  dword ptr [s_resume]

    do_game_view:
        call ToggleGameView
        ret  8

    do_f12:
        call OpenReloadedOptions
    swallow:
        ret  8
    }
}

// Duplication offset, and exact placement for paste
JMP_HOOK(0x10eb8573, DupOffsetHook)
{
    static int s_skip   = 0x10eb861c;
    static int s_is_dup = 0x10eb8579;

    __asm
    {
        cmp  dword ptr [ebp + 0xc], 0
        jz   skip_offset                // Real paste: honor the text exactly

        cmp  byte ptr [g_ReloadedNoDuplicateOffset], 0
        jnz  skip_offset

        jmp  dword ptr [s_is_dup]

    skip_offset:
        jmp  dword ptr [s_skip]
    }
}

JMP_HOOK(0x10eb8722, DupOffsetHook2)
{
    static int s_skip   = 0x10eb897b;
    static int s_is_dup = 0x10eb8728;

    __asm
    {
        cmp  dword ptr [ebp + 0xc], 0
        jz   skip_offset2               // Real paste: honor the text exactly

        cmp  byte ptr [g_ReloadedNoDuplicateOffset], 0
        jnz  skip_offset2

        jmp  dword ptr [s_is_dup]

    skip_offset2:
        // The skipped branches are the only code that writes [ebp-0x1c].
        // 0x10eb897b passes it to the selection-move code, or it runs on stack garbage.
        xor  eax, eax
        mov  dword ptr [ebp - 0x1c], eax
        mov  dword ptr [ebp - 0x18], eax
        mov  dword ptr [ebp - 0x14], eax
        jmp  dword ptr [s_skip]
    }
}

void General::Initialize()
{
    INSTALL_HOOKS;
    GridSizeShortcut::Initialize();
    InstallMemoryHooks();
    BspTransientRenderFix::Initialize();
    ShadowEdgeCacheFix::Initialize();
    InstallMinimizeOnPlayHook();
    InstallNoEmbedOnPlayPatch();
    InstallLegacyPlayLaunchHook();
}
