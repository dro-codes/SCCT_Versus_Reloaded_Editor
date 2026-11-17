The PS2 Vial Case is a Neutralization computer objective, implemented by
`P2_PS2Objectives.PS2Computer` without replacing stock SBase classes.

`P2_PS2Objectives.usx` contains the compiled class in `PS2Computer.uc`.
`P2_nd133.utx` contains the original PS2 console materials and textures recovered
from the user's Chaos Theory PS2 disc. The console uses the retained original
PS2 ND133 mesh and animation clips in the stock `nd133_1mesh.ukx`; that game file
is required and is not redistributed here.

The DLL embeds both packages and verifies their SHA-256 before installing them
under `Packages/StaticMeshes` and `Packages/Textures` on first placement. Existing
different files are backed up; loaded conflicting packages must be unloaded by
restarting the editor. Map packaging follows the saved actor's actual package
imports, including the class USX and material UTX.

For a new native PC map, set the containing `SMission.GameMode` to **Story**
(`GM_Multi`, 5), and advertise Story in the map's settings. The case still uses
the original Neutralization hack and animation. The shipped PC mode selector
cannot select `GM_Neutralization` (3); using that mission value makes ordinary
Story hosting leave its objectives inactive. An explicit editor launch with
`GameMode=3` is therefore not a sufficient normal-hosting test.

Verified files:

- `P2_PS2Objectives.usx`: `d65019c7b514701dc908c0ace29460118eb9bdabf056d766e1b35df06668c9c3`
- `P2_nd133.utx`: `8cc92fd095a703c8c75ec8815982d6075c4266f832e484088297cbdd0cc30654`
- Required stock `nd133_1mesh.ukx`: `2394167a4c1149d0503103a5575147aeedcad5e69c9c40f0ca39c1eeda148928`
