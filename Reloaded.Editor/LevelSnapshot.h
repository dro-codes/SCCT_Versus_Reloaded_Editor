#pragma once
#include <windows.h>
#include "WorkflowModel.h"
#include <cstdint>
#include <filesystem>
#include <string>

// The level snapshot the game shows in map selection: the Menu texture of the
// map's <Map>-i interface package. One command captures the perspective
// viewport into it, another takes an image file; both go through the
// editor's own texture importer and the stock SAVEMAPPROP save, so the
// loading screens and map settings in that package ride along and the next
// map save keeps the new picture. Errors propagate to the caller's dialog.
namespace LevelSnapshot
{
    void Attach(HWND frame);
    void FromViewport();
    void FromImageFile();

    // The largest visible perspective level viewport: its window, its camera
    // actor, the engine viewport and its place in the editor's viewport list
    // (the "slot" of Workflow::Editor::CaptureView). Throws when none is open.
    struct Perspective { HWND window = nullptr; uintptr_t camera = 0, viewport = 0; int slot = -1; };
    Perspective FindPerspective();
    // The largest visible viewport of a kind: "perspective", or "top" (the
    // orthographic view from above). Throws when none is open.
    Perspective FindViewport(const std::string& kind);
    // The viewport's picture, drawn afresh, fitted to Thumbnail::Fit and saved
    // as a PNG (written beside the file first, then moved into place). The
    // viewport draws itself into the picture, so the tool window that asked,
    // or any other window over the viewport, is not in the shot.
    void SaveViewportThumbnail(const Perspective& viewport, const std::filesystem::path& file);
    // The same picture at most maxEdge pixels on its longer side, never
    // enlarged (Thumbnail::Within): saved as a PNG file, or as PNG bytes.
    void SaveViewportImage(const Perspective& viewport, const std::filesystem::path& file, int maxEdge);
    struct Picture { std::string png; int width = 0, height = 0; };
    Picture ViewportImage(const Perspective& viewport, int maxEdge);
    // One of the four pictures of the map's interface package (<Map>-i), from
    // an image file, the way the wiki's Map Settings page sets them: Menu and
    // Briefing 256 x 256 DXT5 alpha textures, LoadingATT0 and LoadingDEF0
    // 512 x 512 DXT1. A PNG, JPEG, BMP, GIF or TIFF is fitted to the size; a
    // TGA or DDS must have it already (a DDS keeps its own format, which must
    // be the slot's). Backed up, loaded, imported, saved by the stock route
    // and checked as the snapshot is; the map must have a file name. Returns
    // {slot, package, file, utx?, width, height, format, exports}.
    Workflow::Json ApplyInterfaceTexture(const std::string& slot, const std::filesystem::path& image);
    // What the open map's interface package file holds, as the game reads it
    // (Packages\Textures\<Map>-i.utc): {package, file, saved, settings (an
    // SMapSettings object is there), exports, textures:{Menu: {width, height,
    // format, mips}, ...}}; saved is false while there is no file, package
    // null while the map has no name. Reads the file only.
    Workflow::Json DescribeInterfacePackage();
    // A thumbnail letterboxed into a width x height cell for an image list,
    // or nullptr when the file is missing, too large or not a readable image.
    // The file is read into memory first, so it is never left locked.
    HBITMAP LoadThumbnail(const std::filesystem::path& file, int width, int height);
    // The cell shown for entries saved before thumbnails existed.
    HBITMAP ThumbnailPlaceholder(int width, int height);
}
