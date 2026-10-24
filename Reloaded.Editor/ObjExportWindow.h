#pragma once
#include <windows.h>
// File > Export > Map to OBJ... (41660): the built level's BSP surfaces and its
// static meshes as <Map>.obj + <Map>.mtl with the textures as PNGs, Z up, for
// Blender (ObjExportModel.h has the conventions). File > Export > Map... (the
// stock 40017) sits beside it in the same submenu.
namespace ObjExportWindow
{
    constexpr UINT Command = 41660;
    // Control ids inside the window; a test can set the fields and post
    // WM_COMMAND with kExport, then read kStatus.
    constexpr int kFolder = 301, kBrowse = 302, kMeshes = 303, kTextures = 304, kSelection = 305, kScale = 306,
                  kExport = 307, kOpenFolder = 308, kStatus = 309, kIntro = 310;
    void Open(HWND owner);
    // From the frame's command dispatcher. True when the command was ours.
    bool HandleCommand(UINT command);
    // File > Export: the stock Export... item and this one in a submenu.
    void InstallMenu(HMENU file);
}
