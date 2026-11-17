class PS2Computer extends SComputerObjectiveTrigger nativereplication;

// Visual adaptation only. The inherited native objective, interaction, collision
// StaticMesh, capture timer and replicated SkinState remain the PC game logic.
// Animation and screen selections follow original PS2 SComputerObjectiveTrigger
// COM_ONL 0x13F888..0x140641; the retained retail nd133 asset has the exact clips.
var transient byte PS2VisualState;
var transient bool bPS2VisualReady;

simulated function SetupPS2Console()
{
    LinkMesh(SkeletalMesh'nd133_1mesh.nd133_mesh');
    LinkSkelAnim(MeshAnimation'nd133_1mesh.nd133_mesh');
    SetDrawType(DT_Mesh);
    Skins[0]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_858141';
    Skins[1]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_858143';
    Skins[2]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_858135';
    Skins[3]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_856859';
    bPS2VisualReady=true;
    PS2VisualState=255;
}

simulated function PostBeginPlay()
{
    Super.PostBeginPlay();
    SetupPS2Console();
}

simulated function PS2ChangeVisual(byte NewState)
{
    PS2VisualState=NewState;
    if(NewState==0)
    {
        Skins[2]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_856858';
        PlayAnim('nd133_hack_ps2',5.0,,,true,true);
    }
    else if(NewState==1)
    {
        Skins[2]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_856858';
        PlayAnim('nd133_hack_ps2',10.0/FMax(1.0,float(CaptureTime)),,,,true);
    }
    else if(NewState==2)
    {
        Skins[2]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_856848';
        PlayAnim('nd133_alarm_ps2',1.0);
    }
    else if(NewState==3)
        PlayAnim('nd133_keyboard_in_ps2',1.0);
}

simulated function Tick(float DeltaTime)
{
    Super.Tick(DeltaTime);
    if(!bPS2VisualReady) SetupPS2Console();
    // Native ASComputerObjectiveTrigger::Tick runs after AActor::Tick and would
    // otherwise put the PC laptop screen into slot1. Consume its visual cache
    // while preserving the authoritative replicated state and progress fields.
    LocalSkinState=SkinState;
    if(PS2VisualState!=SkinState) PS2ChangeVisual(SkinState);
}

function Triggering(bool Status)
{
    Super.Triggering(Status);
    if(Status) PS2ChangeVisual(1);
    else StopAnimating();
}

simulated function AnimEnd(int Channel)
{
    if(Channel!=0) return;
    if(bActive)
    {
        Skins[2]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_856858';
        Skins[3]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_856859';
    }
    else if(Objective!=None && !Objective.ObjectiveTaken)
    {
        Skins[2]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_856848';
        Skins[3]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_856859';
    }
    else
    {
        Skins[2]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_856854';
        Skins[3]=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_856841';
    }
}

defaultproperties
{
    DrawType=DT_Mesh
    Mesh=SkeletalMesh'nd133_1mesh.nd133_mesh'
    Skins(0)=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_858141'
    Skins(1)=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_858143'
    Skins(2)=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_858135'
    Skins(3)=Material'P2_nd133.UnknownGlobalPS2_856860.UnknownGlobalPS2_856859'
}
