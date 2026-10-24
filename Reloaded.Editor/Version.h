#pragma once

// RE+ (Reloaded Editor Plus) versioning. This is the one place the version
// lives: the window title, the About box, the self-updater, the DLL's file
// properties (Reloaded.Editor.rc) and tools/package_release.ps1 all read it.
//
// RE+ releases are MAJOR.MINOR.PATCH, tagged v<version> on GitHub:
//   MAJOR  2 is RE+. 1.x was AllyPal's Reloaded Editor (1.1 to 1.21) and the
//          1.3.0 betas. Raise it only for a break in compatibility: the
//          install layout, or files earlier versions wrote that it no longer
//          reads.
//   MINOR  a feature release: new tools, windows or workflows. Reset PATCH.
//   PATCH  a release of fixes only.
// PRERELEASE is "" for a release. A candidate that needs testing first is
// "-rc.1", "-rc.2", ... published as a GitHub pre-release; the updater only
// offers pre-releases to editors already running one.
//
// Bump this before packaging each release, or its users are offered the same
// release again on every start.
#define RE_PLUS_VERSION_MAJOR 2
#define RE_PLUS_VERSION_MINOR 8
#define RE_PLUS_VERSION_PATCH 1
#define RE_PLUS_VERSION_PRERELEASE ""

#define RE_PLUS_STRINGIZE_(x) #x
#define RE_PLUS_STRINGIZE(x) RE_PLUS_STRINGIZE_(x)

#define RE_PLUS_NAME "RE+"
#define RE_PLUS_FULL_NAME "Reloaded Editor Plus"
// "2.0.0" or "2.1.0-rc.1", as the tag without its 'v'.
#define RE_PLUS_VERSION RE_PLUS_STRINGIZE(RE_PLUS_VERSION_MAJOR) "." RE_PLUS_STRINGIZE(RE_PLUS_VERSION_MINOR) "." RE_PLUS_STRINGIZE(RE_PLUS_VERSION_PATCH) RE_PLUS_VERSION_PRERELEASE
// How the app shows it: "RE+ 2.0.0".
#define RE_PLUS_DISPLAY_VERSION RE_PLUS_NAME " " RE_PLUS_VERSION
// The editor frame's title, before a map is open and in front of its name.
#define RE_PLUS_WINDOW_TITLE RE_PLUS_DISPLAY_VERSION " - Chaos Theory Editor"
