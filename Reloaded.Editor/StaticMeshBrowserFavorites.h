#pragma once
#include <string>
#include <vector>

namespace StaticMeshBrowserFavorites
{
    void Initialize();
    // The saved favourites changed elsewhere (the Favorites window): reload them.
    void FavoritesChanged();
    // Switches the browser to Favorites and selects the mesh's row; false
    // while the browser has not attached its Favorites controls yet.
    bool ShowFavorite(const std::string& path);
    // Makes the mesh the current one as a click in the browser does (its
    // properties and preview follow); false without a browser.
    bool UseMesh(void* mesh);
    // The saved favourite meshes' paths, in the order they were added.
    std::vector<std::string> FavoritePaths();
}
