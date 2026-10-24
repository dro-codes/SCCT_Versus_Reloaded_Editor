#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Conveyor belts: RE+ Tools > Add Conveyor Belt compiles four classes into the map
// package (MyLevel) and places a ConveyorBelt. Its pivot is its start; its rotation
// gives the run's direction and slope, and Length and Width size it. In game it
// spawns its rollers (seen only, spinning) and the items riding it, and moves walking
// players standing on it with Pawn.Move: nothing a player stands on turns, so the
// view is never turned with it. Belts link with NextBelt into a chain whose first
// belt runs the items of all of them: items carry on from belt to belt and shrink
// away over FadeDistance only at the chain's two ends (never in a closed loop).
// The frame's mesh and the rollers' are made here and imported into the map with
// one small texture, so a map needs nothing installed beside it.
//
// The belt is static, so the lighting build lights its frame as any static mesh; at
// round start it spawns a ConveyorMotor, which ticks it. Every machine runs its own
// belt: the map's copy stays on each client, RemoteRole=ROLE_SimulatedProxy with
// simulated functions lets it run there (as Character Skins' actor does), and
// nothing is replicated. Motor, rollers and items are spawned by each machine with
// RemoteRole=ROLE_None. The host moves every player; a client moves only its own
// player, so riding is smooth there.
namespace Conveyor
{
// Raise with every change to a script or mesh: a map's older classes and meshes are
// made again over their old versions, and its placed belts keep their settings.
constexpr int Version = 2;
constexpr const char* BeltClass = "ConveyorBelt";
constexpr const char* MotorClass = "ConveyorMotor";
constexpr const char* ItemClass = "ConveyorItem";
constexpr const char* RollerClass = "ConveyorRoller";
constexpr const char* Group = "Conveyor";
constexpr const char* FrameMesh = "ConveyorFrame";
// The frame without its left (-Y) or right (+Y) channel, for a joint a turn comes
// into from that side: [left wall][right wall].
inline const char* FrameMeshName(bool left, bool right)
{
    return left ? (right ? FrameMesh : "ConveyorFrameNoRight") : (right ? "ConveyorFrameNoLeft" : "ConveyorFrameOpen");
}
constexpr const char* RollerMesh = "ConveyorRoller";
constexpr const char* Texture = "ConveyorParts";
constexpr int ItemSlots = 8;
constexpr int MaxItems = 128;
// Belts in one chain (NextBelt links) that share its items: a closed loop of up to
// this many, or the first this many of a longer line (the editor warns past it).
constexpr int MaxChain = 64;
// The meshes are 100 units long (X) and wide (Y); a belt's DrawScale3D is
// (Length / 100, Width / 100, 1), its rollers' Y scale (Width * RollerSpan / 100).
constexpr double Unit = 100.0;
constexpr double RollerRadius = 6.0, RollerGap = 16.0, RollerSpan = 0.88;
// How far the frame reaches under the riding surface (the pivot): a belt added on
// a floor stands on it.
constexpr double FrameDepth = 30.0;
// How high a carried player steps each frame: onto the next belt's edge at a joint.
constexpr double CarryStep = 18.0;

// Stock cases and boxes riding a new belt, as many as fit the slots.
inline constexpr std::array<const char*, 6> DefaultItems = {{
    "Terminus_STM.hangar.Ind03_caisseOutilFerme",
    "Terminus_STM.hangar.Ind03_caisseOutilFerme01",
    "Terminus_STM.hangar.Ind03_caisseOutilFerme02",
    "STM_ORP.Objets.ORP_carton",
    "SLH07_STM.Objects.Boite_Multi",
    "Terminus_STM.Green.IND03_Caisse_Fragile",
}};

// The belt's DrawScale3D for a length and width (never below a unit's hundredth).
inline std::array<float, 3> FrameScale(double length, double width)
{
    return {static_cast<float>(std::max(length, 1.0) / Unit), static_cast<float>(std::max(width, 1.0) / Unit), 1.0f};
}

// Where an item's mesh pivot goes, from the centre of its base on the belt, at full
// size: its bounds' (min X, min Y, min Z, max X, max Y, max Z) centred and lifted.
inline std::array<float, 3> ItemOffset(const std::array<float, 6>& bounds)
{
    return {-(bounds[0] + bounds[3]) / 2.0f, -(bounds[1] + bounds[4]) / 2.0f, -bounds[2]};
}

inline std::string RollerScript(const std::string& package)
{
    return
        "//=============================================================================\r\n"
        "// ConveyorRoller: one of a ConveyorBelt's rollers, spun by the belt. It is only\r\n"
        "// seen: the belt's frame is what players and items stand on.\r\n"
        "//=============================================================================\r\n"
        "class ConveyorRoller extends Actor;\r\n"
        "\r\n"
        "defaultproperties\r\n"
        "{\r\n"
        "\tDrawType=DT_StaticMesh\r\n"
        "\tStaticMesh=StaticMesh'" + package + "." + Group + "." + RollerMesh + "'\r\n"
        "\tPhysics=PHYS_Rotating\r\n"
        "\tbFixedRotationDir=True\r\n"
        "\tbCollideActors=False\r\n"
        "\tbCollideWorld=False\r\n"
        "\tbBlockActors=False\r\n"
        "\tbBlockPlayers=False\r\n"
        "\tRemoteRole=ROLE_None\r\n"
        "}\r\n";
}

// The script text below is written with \n and placeholders (@NAME@); the stock
// compiler wants CRLF.
inline std::string Script(std::string text, const std::vector<std::pair<std::string, std::string>>& values)
{
    for (const auto& [key, value] : values)
        for (auto at = text.find(key); at != std::string::npos; at = text.find(key, at + value.size()))
            text.replace(at, key.size(), value);
    std::string out;
    for (char c : text)
    {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

inline std::string MotorScript()
{
    return Script(R"UC(//=============================================================================
// ConveyorMotor: ticks a ConveyorBelt, which is static so that the map's lighting
// lights its frame. It has no owner: an actor ticks after its owner, and a static
// owner never ticks.
//=============================================================================
class ConveyorMotor extends Info;

var Actor Belt;

function Tick(float DeltaTime)
{
	if (Belt != None)
		Belt.Tick(DeltaTime);
}

defaultproperties
{
	RemoteRole=ROLE_None
}
)UC", {});
}

inline std::string ItemScript()
{
    return Script(R"UC(//=============================================================================
// ConveyorItem: something riding a ConveyorBelt, moved by the belt. It blocks
// other actors but not players, so soft-body strip curtains part round it and
// players walk through it.
//=============================================================================
class ConveyorItem extends Actor;

var vector Offset;  // from the centre of its base to its pivot, at full size
var float Size;     // its full DrawScale

defaultproperties
{
	DrawType=DT_StaticMesh
	bCollideActors=True
	bCollideWorld=False
	bBlockActors=True
	bBlockPlayers=False
	bBlockZeroExtentTraces=False
	bBlockNonZeroExtentTraces=False
	CollisionRadius=30
	CollisionHeight=14
	Size=1
	RemoteRole=ROLE_None
}
)UC", {});
}

inline std::string BeltScript(const std::string& package)
{
    auto number = [](double v) { char text[32]; std::snprintf(text, sizeof(text), "%g", v); return std::string(text); };
    return Script(R"UC(//=============================================================================
// ConveyorBelt: a conveyor, placed by RE+ Tools > Add Conveyor Belt. Its pivot is
// its start; its rotation gives the run's direction (X) and slope, and Length and
// Width size it. In game it spawns its rollers, moves walking players standing on
// it, and its items ride it from end to end. Belts link with NextBelt: items carry
// on from one onto the next, shrinking away only where the chain starts and ends
// (nowhere when it closes into a loop). Each machine runs its own (simulated on
// clients); nothing replicates.
//=============================================================================
class ConveyorBelt extends Actor
	placeable;

var(Conveyor) float Length;           // from the start along X, in units
var(Conveyor) float Width;
var(Conveyor) float Speed;            // units a second; below 0 runs backwards
var(Conveyor) ConveyorBelt NextBelt;  // items carry on onto this belt
var(Conveyor) float ItemSpacing;      // between items; 0 for none
var(Conveyor) float FadeDistance;     // items shrink away over this at the chain's ends
var(Conveyor) float ItemScale;
var(Conveyor) bool bCarryPlayers;
var(Conveyor) bool bShowItems;
var(Conveyor) bool bShowRollers;
var(Conveyor) bool bLeftWall;     // the channels at its sides; open one where a turn comes in
var(Conveyor) bool bRightWall;
var(Conveyor) StaticMesh ItemMeshes[@ITEMS@];
var vector ItemOffsets[@ITEMS@];  // kept by the editor from the meshes' bounds
var const int Version;
// A chain's first belt runs the items of the whole chain.
// Between belts items cross from one's end to the nearest point of the next one's
// centre line (Gap units, none on a straight joint) and ride on from there (Entry
// units along it): at a corner they cross the corner piece to its centre line.
var ConveyorBelt Seg[@SEGS@];
var float SegStart[@SEGS@];  // where each belt's part of the chain starts
var float Gap[@SEGS@];
var float Entry[@SEGS@];
var vector From[@SEGS@];
var vector To[@SEGS@];
var rotator FromTurn[@SEGS@];
var int Segs;
var float Total;
var bool bLoop;
var ConveyorItem Items[@MOST@];
var float Along[@MOST@];     // along the chain
var int ItemSeg[@MOST@];
var int Jitter[@MOST@];
var int ItemCount;

simulated function PostBeginPlay()
{
	local ConveyorMotor M;

	Super.PostBeginPlay();
	M = Spawn(class'ConveyorMotor');
	if (M != None)
		M.Belt = self;
	if (bShowRollers)
		SpawnRollers();
	if (Leads())
	{
		BuildChain();
		if (bShowItems)
			SpawnItems();
	}
}

// Whether this belt runs its chain's items: no belt leads onto it, or it is the
// belt named first in a closed loop.
simulated function bool Leads()
{
	local ConveyorBelt B, First;
	local bool bFed;
	local int n;

	foreach AllActors(class'ConveyorBelt', B)
		if (B.NextBelt == self)
			bFed = true;
	if (!bFed)
		return true;
	First = self;
	for (B = NextBelt; B != None && B != self && n < @SEGS@; B = B.NextBelt)
	{
		if (string(B.Name) < string(First.Name))
			First = B;
		n++;
	}
	return B == self && First == self;
}

simulated function BuildChain()
{
	local ConveyorBelt B;
	local int s;

	// Back at this belt is a loop, even one of exactly as many belts as fit.
	for (B = self; B != None; B = B.NextBelt)
	{
		if (Segs > 0 && B == self)
		{
			bLoop = true;
			break;
		}
		if (Segs >= @SEGS@)
			break;
		Seg[Segs] = B;
		Segs++;
	}
	for (s = 0; s < Segs; s++)
	{
		if (s > 0)
			Join(s, Seg[s - 1]);
		else if (bLoop)
			Join(0, Seg[Segs - 1]);
		SegStart[s] = Total;
		Total += Gap[s] + FMax(Seg[s].Length - Entry[s], 1);
	}
}

// Where items come onto belt s from the end of the belt before it.
simulated function Join(int s, ConveyorBelt Before)
{
	local vector X;
	local ConveyorBelt B;

	B = Seg[s];
	X = vector(B.Rotation);
	From[s] = Before.Location + vector(Before.Rotation) * Before.Length;
	FromTurn[s] = Before.Rotation;
	Entry[s] = FClamp((From[s] - B.Location) dot X, 0, FMax(B.Length - 1, 0));
	To[s] = B.Location + X * Entry[s];
	Gap[s] = VSize(To[s] - From[s]);
	if (Gap[s] < 1)
		Gap[s] = 0;
}

simulated function SpawnRollers()
{
	local int i, n;
	local float Gap;
	local vector X, Y, Z, S;
	local ConveyorRoller R;

	GetAxes(Rotation, X, Y, Z);
	n = Clamp(int(Length / @GAP@), 1, 400);
	Gap = Length / n;
	S.X = 1;
	S.Y = FMax(Width * @SPAN@, 1) / 100.0;
	S.Z = 1;
	for (i = 0; i < n; i++)
	{
		R = Spawn(class'ConveyorRoller',,, Location + X * (Gap * (i + 0.5)) - Z * @RADIUS@, Rotation);
		if (R == None)
			continue;
		R.SetDrawScale3D(S);
		// A turn in pitch takes a roller's top back along X: run it the other way.
		R.RotationRate.Pitch = int(-Speed / @RADIUS@ * 10430.378);
		R.RotationRate.Yaw = 0;
		R.RotationRate.Roll = 0;
	}
}

simulated function SpawnItems()
{
	local int i, k, n, s, Kinds;
	local int Pick[@ITEMS@];
	local ConveyorItem Item;

	for (k = 0; k < @ITEMS@; k++)
	{
		if (ItemMeshes[k] != None)
		{
			Pick[Kinds] = k;
			Kinds++;
		}
	}
	if (Kinds == 0 || ItemSpacing <= 0 || Total <= 0)
		return;
	n = Min(int(Total / ItemSpacing), @MOST@);
	for (i = 0; i < n; i++)
	{
		Item = Spawn(class'ConveyorItem',,, Location, Rotation);
		if (Item == None)
			continue;
		k = Pick[Rand(Kinds)];
		Item.SetStaticMesh(ItemMeshes[k]);
		Item.Size = ItemScale;
		Item.Offset = ItemOffsets[k] * ItemScale;
		Items[ItemCount] = Item;
		Along[ItemCount] = (i + FRand() * 0.6) * Total / n;
		s = 0;
		while (s + 1 < Segs && SegStart[s + 1] <= Along[ItemCount])
			s++;
		ItemSeg[ItemCount] = s;
		Jitter[ItemCount] = Rand(2001) - 1000;
		ItemCount++;
	}
	MoveItems(0);
}

simulated function MoveItems(float DeltaTime)
{
	local int i, s;
	local float A, F;
	local rotator R;
	local vector P;
	local ConveyorBelt B;

	for (i = 0; i < ItemCount; i++)
	{
		if (Items[i] == None)
			continue;
		s = ItemSeg[i];
		A = Along[i] + Seg[s].Speed * DeltaTime;
		// Onto the next belt past this one's end, back to the start past the chain's.
		while (s + 1 < Segs && A >= SegStart[s + 1])
			s++;
		if (A >= Total)
		{
			A -= Total;
			s = 0;
		}
		while (A < SegStart[s])
		{
			if (s > 0)
				s--;
			else
			{
				A += Total;
				s = Segs - 1;
			}
		}
		Along[i] = A;
		ItemSeg[i] = s;
		B = Seg[s];
		F = 1;
		if (!bLoop && FadeDistance > 0)
			F = FClamp(FMin(A, Total - A) / FadeDistance, 0.02, 1.0);
		A -= SegStart[s];
		if (A < Gap[s])
		{
			R = FromTurn[s];
			P = From[s] + (To[s] - From[s]) * (A / Gap[s]);
		}
		else
		{
			R = B.Rotation;
			P = B.Location + vector(B.Rotation) * (Entry[s] + A - Gap[s]);
		}
		R.Yaw += Jitter[i];
		Items[i].SetDrawScale(Items[i].Size * F);
		Items[i].SetRotation(R);
		Items[i].SetLocation(P + ((Items[i].Offset * F) >> R));
	}
}

// Whether a walking pawn stands on the belt: its feet on the top (measured from
// them, not its middle, which on a slope reads as further along), within its
// length, and over its width or half its radius past an edge (onto a corner).
simulated function bool Riding(Pawn P, vector X, vector Y, vector Z)
{
	local vector L;
	local float H;

	if (P == None || P.Physics != PHYS_Walking)
		return false;
	L = P.Location - Location;
	L.Z -= P.CollisionHeight;
	if (L dot X < 0 || L dot X > Length || Abs(L dot Y) > Width / 2 + P.CollisionRadius / 2)
		return false;
	H = L dot Z;
	return H > -16 && H < @STEP@ + 16;
}

// A move along a slope sticks against it, and a move never steps up onto the next
// belt's edge, so step as walking does: up, along level, then down to where the
// move along the belt ends (the floor stops it sooner).
simulated function Carry(Pawn P, vector Delta)
{
	local vector Up, Down;

	Up.Z = FMax(Delta.Z, 0) + @STEP@;
	P.Move(Up);
	Down.Z = Delta.Z - Up.Z;
	Delta.Z = 0;
	P.Move(Delta);
	P.Move(Down);
}

// The move for one frame: along the belt, and in from its sides as far as the
// rider overlaps a channel (so a corner piece hands riders on centred).
simulated function vector Push(Pawn P, vector X, vector Y, float DeltaTime)
{
	local float Across, Over;
	local vector Delta;

	Delta = X * Speed * DeltaTime;
	Across = (P.Location - Location) dot Y;
	Over = Abs(Across) - (Width * 0.45 - P.CollisionRadius);
	if (Over > 0)
		Delta -= Y * FMin(Over, Abs(Speed) * DeltaTime) * (Across / Abs(Across));
	return Delta;
}

simulated function CarryPlayers(float DeltaTime)
{
	local Controller C;
	local PlayerController PC;
	local vector X, Y, Z;

	GetAxes(Rotation, X, Y, Z);
	if (Role < ROLE_Authority)
	{
		PC = Level.GetLocalPlayerController();
		if (PC != None && Riding(PC.Pawn, X, Y, Z))
			Carry(PC.Pawn, Push(PC.Pawn, X, Y, DeltaTime));
		return;
	}
	for (C = Level.ControllerList; C != None; C = C.NextController)
		if (Riding(C.Pawn, X, Y, Z))
			Carry(C.Pawn, Push(C.Pawn, X, Y, DeltaTime));
}

// Called by the belt's ConveyorMotor every frame.
simulated function Tick(float DeltaTime)
{
	if (ItemCount > 0)
		MoveItems(DeltaTime);
	if (bCarryPlayers && Speed != 0)
		CarryPlayers(DeltaTime);
}

defaultproperties
{
	Length=600
	Width=120
	Speed=150
	ItemSpacing=140
	FadeDistance=60
	ItemScale=1
	bCarryPlayers=True
	bShowItems=True
	bShowRollers=True
	bLeftWall=True
	bRightWall=True
	Version=@VERSION@
	DrawType=DT_StaticMesh
	StaticMesh=StaticMesh'@PKG@.@GROUP@.@FRAME@'
	DrawScale3D=(X=6,Y=1.2,Z=1)
	bStatic=True
	bNoDelete=True
	bCollideActors=True
	bBlockActors=True
	bBlockPlayers=True
	bBlockZeroExtentTraces=True
	bBlockNonZeroExtentTraces=True
	bEdShouldSnap=True
	RemoteRole=ROLE_SimulatedProxy
}
)UC", {{"@ITEMS@", std::to_string(ItemSlots)}, {"@MOST@", std::to_string(MaxItems)}, {"@SEGS@", std::to_string(MaxChain)},
       {"@GAP@", number(RollerGap)}, {"@SPAN@", number(RollerSpan)}, {"@RADIUS@", number(RollerRadius)}, {"@STEP@", number(CarryStep)},
       {"@VERSION@", std::to_string(Version)}, {"@PKG@", package}, {"@GROUP@", Group}, {"@FRAME@", FrameMesh}});
}

// ----------------------------------------------------------------------------
// The meshes, as ASCII Scene Export text the editor's STATICMESH IMPORT reads,
// coloured from one small texture (a row of swatches).

using Point = std::array<double, 3>;
struct Triangle
{
    std::array<Point, 3> points;
    std::array<std::array<double, 2>, 3> uvs;
};
struct Mesh
{
    std::string name;
    std::vector<Triangle> triangles;
    std::vector<std::vector<Point>> hulls; // convex boxes, as their eight corners
};

// The texture: Swatches cells of 16x16 in one row.
constexpr int Swatches = 8, SwatchSize = 16;
enum Swatch { Paint, Plate, RollerDark, RollerLight, RollerRing, Hazard, Steel, SwatchCount };
inline constexpr std::array<std::array<uint8_t, 3>, Swatches> SwatchColours = {{
    {58, 78, 104},    // the frame's channels: slate blue
    {34, 35, 38},     // the bed under the rollers
    {64, 66, 70},     // roller faces, in turn a shade apart so they are seen turning
    {104, 107, 112},
    {196, 196, 188},  // the white bands at a roller's ends
    {206, 164, 32},   // the hazard yellow on the channels' tops
    {150, 154, 160},  // steel
    {128, 128, 128},
}};

// A face's corners mapped onto its swatch, inside a border so filtering never
// reaches the next one.
inline std::array<double, 2> SwatchUV(Swatch s, double u, double v)
{
    const double border = 3.0 / SwatchSize;
    return {(static_cast<double>(s) + border + u * (1 - 2 * border)) / Swatches, border + v * (1 - 2 * border)};
}

// A quad (corners in turn) as two triangles, its corners on its swatch's.
inline void Quad(Mesh& m, const Point& a, const Point& b, const Point& c, const Point& d, Swatch s)
{
    const auto ua = SwatchUV(s, 0, 0), ub = SwatchUV(s, 1, 0), uc = SwatchUV(s, 1, 1), ud = SwatchUV(s, 0, 1);
    m.triangles.push_back({{a, b, c}, {ua, ub, uc}});
    m.triangles.push_back({{a, c, d}, {ua, uc, ud}});
}

inline Point Sub(const Point& a, const Point& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Point Cross(const Point& a, const Point& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double Dot(const Point& a, const Point& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A face whose winding is turned, if need be, so (b - a) x (c - a) points along
// outward (the convention read_ase and the importer agree on).
inline void Face(Mesh& m, Point a, Point b, Point c, Point d, const Point& outward, Swatch s)
{
    if (Dot(Cross(Sub(b, a), Sub(c, a)), outward) < 0) std::swap(b, d);
    Quad(m, a, b, c, d, s);
}

inline void Box(Mesh& m, const Point& lo, const Point& hi, Swatch sides, Swatch top)
{
    const double x0 = lo[0], y0 = lo[1], z0 = lo[2], x1 = hi[0], y1 = hi[1], z1 = hi[2];
    Face(m, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {0, 0, 1}, top);
    Face(m, {x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, {0, 0, -1}, sides);
    Face(m, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, {0, -1, 0}, sides);
    Face(m, {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}, {0, 1, 0}, sides);
    Face(m, {x0, y0, z0}, {x0, y1, z0}, {x0, y1, z1}, {x0, y0, z1}, {-1, 0, 0}, sides);
    Face(m, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, {1, 0, 0}, sides);
}

inline void Hull(Mesh& m, const Point& lo, const Point& hi)
{
    std::vector<Point> corners;
    for (int i = 0; i < 8; ++i) corners.push_back({(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]});
    m.hulls.push_back(corners);
}

// The frame, pivot on the riding surface at its start (X runs 0 to 100): a dark bed under the rollers
// between two slate channels capped in yellow. Players stand on a still bed at the
// rollers' tops (its collision), and the channels' tops stand a little above it.
inline Mesh Frame(bool left = true, bool right = true)
{
    Mesh m{FrameMeshName(left, right), {}, {}};
    const double l = Unit, inner = 45, outer = 50, depth = FrameDepth;
    Box(m, {0, -inner, -28}, {l, inner, -25}, Plate, Plate);
    for (int s : {-1, 1})
    {
        const double y0 = s < 0 ? -outer : inner, y1 = s < 0 ? -inner : outer;
        if (!(s < 0 ? left : right))
        {
            // No channel: the bed's edge, flush with the riding surface.
            Box(m, {0, y0, -depth}, {l, y1, 0}, Paint, Paint);
            Hull(m, {0, y0, -depth}, {l, y1, 0});
            continue;
        }
        Box(m, {0, y0, -depth}, {l, y1, 3}, Paint, Paint);
        Box(m, {0, y0, 3}, {l, y1, 5}, Hazard, Hazard);
        Hull(m, {0, y0, -depth}, {l, y1, 5});
    }
    Hull(m, {0, -inner, -depth}, {l, inner, 0});
    return m;
}

// A roller along Y, 100 long, pivot on its axis: twelve faces in two shades, a white
// band near each end, and end caps.
inline Mesh Roller()
{
    Mesh m{RollerMesh, {}, {}};
    constexpr int sides = 12;
    const double r = RollerRadius, h = Unit / 2, band = 4;
    std::array<std::array<double, 2>, sides> ring{};
    for (int i = 0; i < sides; ++i)
    {
        const double a = 2 * 3.14159265358979323846 * (i + 0.5) / sides;
        ring[i] = {r * std::sin(a), r * std::cos(a)};
    }
    for (int i = 0; i < sides; ++i)
    {
        const auto& p = ring[i];
        const auto& q = ring[(i + 1) % sides];
        const Point out{(p[0] + q[0]) / 2, 0, (p[1] + q[1]) / 2};
        const Swatch face = i % 2 ? RollerDark : RollerLight;
        for (auto [y0, y1, s] : {std::tuple<double, double, Swatch>{-h, -h + band, RollerRing},
                                 {-h + band, h - band, face}, {h - band, h, RollerRing}})
            Face(m, {p[0], y0, p[1]}, {q[0], y0, q[1]}, {q[0], y1, q[1]}, {p[0], y1, p[1]}, out, s);
    }
    for (int s : {-1, 1})
    {
        const double y = s * h;
        const auto uv = SwatchUV(Steel, 0.5, 0.5);
        for (int i = 1; i + 1 < sides; ++i)
        {
            Point a{ring[0][0], y, ring[0][1]}, b{ring[i][0], y, ring[i][1]}, c{ring[i + 1][0], y, ring[i + 1][1]};
            if (Dot(Cross(Sub(b, a), Sub(c, a)), Point{0, static_cast<double>(s), 0}) < 0) std::swap(b, c);
            m.triangles.push_back({{a, b, c}, {uv, uv, uv}});
        }
    }
    return m;
}

inline std::string Number(double x)
{
    if (std::abs(x) < 5e-5) x = 0;
    char text[32];
    std::snprintf(text, sizeof(text), "%.4f", x);
    return text;
}

// One GEOMOBJECT. Unreal (x, y, z) is written as Max (x, -y, z); the mirror turns
// the winding, so each face lists its corners a, c, b; V is written 1 - v.
inline std::string GeomObject(const std::string& name, const std::vector<Triangle>& triangles)
{
    std::vector<Point> vertices;
    std::vector<std::array<int, 3>> faces;
    auto index = [&](const Point& p) {
        for (size_t i = 0; i < vertices.size(); ++i)
            if (std::abs(vertices[i][0] - p[0]) < 1e-4 && std::abs(vertices[i][1] - p[1]) < 1e-4 && std::abs(vertices[i][2] - p[2]) < 1e-4)
                return static_cast<int>(i);
        vertices.push_back(p);
        return static_cast<int>(vertices.size() - 1);
    };
    for (const auto& t : triangles) faces.push_back({index(t.points[0]), index(t.points[1]), index(t.points[2])});
    std::string out = "*GEOMOBJECT {\r\n\t*NODE_NAME \"" + name + "\"\r\n\t*MESH {\r\n\t\t*TIMEVALUE 0\r\n";
    out += "\t\t*MESH_NUMVERTEX " + std::to_string(vertices.size()) + "\r\n\t\t*MESH_NUMFACES " + std::to_string(faces.size()) + "\r\n";
    out += "\t\t*MESH_VERTEX_LIST {\r\n";
    for (size_t i = 0; i < vertices.size(); ++i)
        out += "\t\t\t*MESH_VERTEX " + std::to_string(i) + "\t" + Number(vertices[i][0]) + "\t" + Number(-vertices[i][1]) + "\t" + Number(vertices[i][2]) + "\r\n";
    out += "\t\t}\r\n\t\t*MESH_FACE_LIST {\r\n";
    for (size_t i = 0; i < faces.size(); ++i)
        out += "\t\t\t*MESH_FACE " + std::to_string(i) + ":\tA: " + std::to_string(faces[i][0]) + " B: " + std::to_string(faces[i][2]) +
               " C: " + std::to_string(faces[i][1]) + " AB: 1 BC: 1 CA: 1\t*MESH_SMOOTHING 0\t*MESH_MTLID 0\r\n";
    out += "\t\t}\r\n\t\t*MESH_NUMTVERTEX " + std::to_string(3 * triangles.size()) + "\r\n\t\t*MESH_TVERTLIST {\r\n";
    for (size_t i = 0; i < triangles.size(); ++i)
        for (int k = 0; k < 3; ++k)
            out += "\t\t\t*MESH_TVERT " + std::to_string(3 * i + k) + "\t" + Number(triangles[i].uvs[k][0]) + "\t" +
                   Number(1.0 - triangles[i].uvs[k][1]) + "\t0.0000\r\n";
    out += "\t\t}\r\n\t\t*MESH_NUMTVFACES " + std::to_string(triangles.size()) + "\r\n\t\t*MESH_TFACELIST {\r\n";
    for (size_t i = 0; i < triangles.size(); ++i)
        out += "\t\t\t*MESH_TFACE " + std::to_string(i) + "\t" + std::to_string(3 * i) + "\t" + std::to_string(3 * i + 2) + "\t" +
               std::to_string(3 * i + 1) + "\r\n";
    out += "\t\t}\r\n\t}\r\n\t*MATERIAL_REF 0\r\n}\r\n";
    return out;
}

// A box hull's twelve outward triangles. The importer turns MCD objects half a turn
// about Z against the rest of the mesh, so they are written turned the other way.
inline std::vector<Triangle> HullTriangles(const std::vector<Point>& corners)
{
    Point lo = corners[0], hi = corners[0];
    for (const auto& p : corners)
        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], p[a]); hi[a] = std::max(hi[a], p[a]); }
    Mesh box;
    Box(box, {-hi[0], -hi[1], lo[2]}, {-lo[0], -lo[1], hi[2]}, Plate, Plate);
    return box.triangles;
}

inline std::string Ase(const Mesh& m)
{
    std::string out = "*3DSMAX_ASCIIEXPORT\t200\r\n*COMMENT \"RE+ conveyor: " + m.name + "\"\r\n*MATERIAL_LIST {\r\n"
                      "\t*MATERIAL_COUNT 1\r\n\t*MATERIAL 0 {\r\n\t\t*MATERIAL_NAME \"" + m.name + "\"\r\n"
                      "\t\t*MATERIAL_CLASS \"Multi/Sub-Object\"\r\n\t\t*NUMSUBMTLS 1\r\n"
                      "\t\t*SUBMATERIAL 0 {\r\n\t\t\t*MATERIAL_NAME \"" + std::string(Texture) + "\"\r\n"
                      "\t\t\t*MATERIAL_CLASS \"Standard\"\r\n\t\t\t*MATERIAL_DIFFUSE 1.0000\t1.0000\t1.0000\r\n"
                      "\t\t\t*MAP_DIFFUSE {\r\n\t\t\t\t*MAP_NAME \"" + Texture + "\"\r\n\t\t\t\t*MAP_CLASS \"Bitmap\"\r\n"
                      "\t\t\t\t*BITMAP \"" + Texture + ".tga\"\r\n\t\t\t\t*UVW_U_TILING 1.0000\r\n\t\t\t\t*UVW_V_TILING 1.0000\r\n"
                      "\t\t\t}\r\n\t\t}\r\n\t}\r\n}\r\n";
    out += GeomObject(m.name, m.triangles);
    for (size_t i = 0; i < m.hulls.size(); ++i)
    {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "%02d", static_cast<int>(i));
        out += GeomObject("MCDCX_" + m.name + "_" + suffix, HullTriangles(m.hulls[i]));
    }
    return out;
}

// Bounds of the visible faces and hulls: (min X, min Y, min Z, max X, max Y, max Z).
inline std::array<double, 6> Bounds(const Mesh& m)
{
    std::array<double, 6> b{1e9, 1e9, 1e9, -1e9, -1e9, -1e9};
    auto add = [&](const Point& p) { for (int a = 0; a < 3; ++a) { b[a] = std::min(b[a], p[a]); b[a + 3] = std::max(b[a + 3], p[a]); } };
    for (const auto& t : m.triangles) for (const auto& p : t.points) add(p);
    for (const auto& h : m.hulls) for (const auto& p : h) add(p);
    return b;
}

// The swatch texture as a 32-bit TGA (top-left origin), with a faint grain.
inline std::vector<uint8_t> TextureTga()
{
    const int width = Swatches * SwatchSize, height = SwatchSize;
    std::vector<uint8_t> out = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                static_cast<uint8_t>(width & 0xff), static_cast<uint8_t>(width >> 8),
                                static_cast<uint8_t>(height & 0xff), static_cast<uint8_t>(height >> 8), 32, 0x28};
    uint32_t seed = 0x2545f491;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            seed = seed * 1664525u + 1013904223u;
            const int grain = static_cast<int>((seed >> 24) % 9) - 4;
            const auto& c = SwatchColours[x / SwatchSize];
            for (int k : {2, 1, 0}) out.push_back(static_cast<uint8_t>(std::clamp(c[k] + grain, 0, 255)));
            out.push_back(255);
        }
    return out;
}
}
