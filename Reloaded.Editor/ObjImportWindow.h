#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
// Import OBJ: Static Mesh Browser > File > Import OBJ... and File > Import OBJ Mesh... (41640).
// Reads a Wavefront OBJ with its MTL, makes it a static mesh with collision and places it.
// See ObjImportModel.h.
namespace ObjImportWindow
{
    constexpr unsigned Command = 41640;
    // `package` is the package to start with (the Static Mesh Browser's), or empty for the last one used.
    void Open(HWND owner, const std::string& package = {});
    // An image GDI+ reads (PNG, JPG, GIF, TIFF, BMP) scaled to the nearest powers of two (at most
    // 2048 a side) and written as a 32-bit TGA the editor's texture importer reads. alpha: whether
    // any pixel is not opaque.
    bool ImageToTga(const std::filesystem::path& image, const std::filesystem::path& tga, int& width, int& height, bool& alpha, std::string& error);
}
