// Lights on the plan: presets placed from the right-click menu, dragging a
// light to move it, dragging its rim to set the reach, an edit form, shading of
// what no light reaches, and the design check's dark rooms. Included inside
// MapDesignPanel.inl after SecurityPanel.inl.
struct LightPreset { const char* name; const char* type; int radius,brightness,hue,saturation; const char* lightType; const char* effect; };
inline const std::vector<LightPreset>& LightPresets()
{
    // Radius is in units; the actor stores it in steps of 25. Hue 0 with
    // saturation 0 is red; saturation 255 is white.
    static const std::vector<LightPreset> presets={
        {"Ceiling light","Engine.Light",400,64,0,255,"LT_Steady","LE_None"},
        {"Small lamp","Engine.Light",200,48,24,150,"LT_Steady","LE_None"},
        {"Bright floodlight","Engine.Light",800,120,0,255,"LT_Steady","LE_None"},
        {"Dim blue night light","Engine.Light",300,32,160,90,"LT_Steady","LE_None"},
        {"Red alarm light","Engine.Light",350,96,0,0,"LT_Pulse","LE_None"},
        {"Flickering light","Engine.Light",300,56,24,200,"LT_Flicker","LE_None"},
        {"Switchable light (STriggerLight)","SBase.STriggerLight",400,64,0,255,"LT_Steady","LE_None"}};
    return presets;
}
const char* const kLightTypes[]={"LT_Steady","LT_Pulse","LT_Blink","LT_Flicker","LT_Strobe","LT_SubtlePulse"};
const char* const kLightEffects[]={"LE_None","LE_Searchlight","LE_Spotlight","LE_Sunlight","LE_TorchWaver","LE_FireWaver","LE_Disco","LE_Cylinder","LE_NonIncidence"};
std::string LightLabel(const Json& light)
{
    return (light.value("switchable",false)?"switchable light ":"light ")+SecurityName(light);
}
// The light body or its rim handle under a point in the view. The rim handle
// sits at the right edge of the light's reach.
bool LightHandleAt(DesignState& s,double x,double y,Json& light,bool& radiusHandle)
{
    double best=10;
    light=Json{};
    for(const auto& candidate:s.lights)
    {
        const Vector position=candidate.at("position");
        if(!DesignPointShown(s,candidate) || !DesignOnFloor(s,position[2],position[2]))continue;
        const auto at=DesignScreen(s,position);
        const auto rim=Gdiplus::PointF(static_cast<float>(at.X+candidate.value("radius",0.0)*s.zoom),at.Y);
        const double rimDistance=DesignPointDistance(rim,x,y);
        if(rimDistance<best){best=rimDistance;light=candidate;radiusHandle=true;}
        const double distance=DesignPointDistance(at,x,y);
        if(distance<best){best=distance;light=candidate;radiusHandle=false;}
    }
    return !light.is_null();
}
Json LightAt(DesignState& s,double x,double y)
{
    Json light;bool rim=false;
    if(LightHandleAt(s,x,y,light,rim) && !rim)return light;
    return Json{};
}
// Whether a point of the plan lies inside a convex hull.
bool LightInsideHull(const std::vector<Design::Point>& hull,double x,double y)
{
    if(hull.size()<3)return false;
    int sign=0;
    for(size_t i=0;i<hull.size();++i)
    {
        const auto& a=hull[i];
        const auto& b=hull[(i+1)%hull.size()];
        const double cross=(b[0]-a[0])*(y-a[1])-(b[1]-a[1])*(x-a[0]);
        if(std::abs(cross)<1e-9)continue;
        const int side=cross>0?1:-1;
        if(sign==0)sign=side;
        else if(sign!=side)return false;
    }
    return true;
}
// Shades the parts of the map's brushes that no light reaches on this view's
// depth: a light's reach shrinks with its height above the floor being shown.
void LightUnlitPaint(DesignState& s,Gdiplus::Graphics& g)
{
    if(!s.unlit)return;
    using namespace Gdiplus;
    RECT rc{};
    GetClientRect(s.canvas,&rc);
    const int h=DesignHorizontal(s),v=DesignVertical(s),third=3-h-v;
    struct Reach { double x,y,r2; };
    std::vector<Reach> lights;
    for(const auto& light:s.lights)
    {
        const Vector p=light.at("position");
        const double r=light.value("radius",0.0),d=p[third]-s.depth;
        const double r2=r*r-d*d;
        if(r2>0)lights.push_back({p[h],p[v],r2});
    }
    struct Box { double lo0,lo1,hi0,hi1; const std::vector<Design::Point>* hull; };
    std::vector<Box> boxes;
    for(size_t i=0;i<s.hulls.size() && i<s.scene.size();++i)
    {
        const auto& hull=s.hulls[i];
        const auto& actor=s.scene[i];
        if(hull.size()<3 || !DesignShows(s,actor) || actor.value("volume",false) || actor.value("portal",false))continue;
        Box box{hull[0][0],hull[0][1],hull[0][0],hull[0][1],&hull};
        for(const auto& point:hull)
        {
            box.lo0=std::min(box.lo0,point[0]);box.lo1=std::min(box.lo1,point[1]);
            box.hi0=std::max(box.hi0,point[0]);box.hi1=std::max(box.hi1,point[1]);
        }
        boxes.push_back(box);
    }
    if(boxes.empty())return;
    SolidBrush shade(Color(95,10,18,60));
    const int cell=10;
    for(int y=0;y<rc.bottom;y+=cell)
        for(int x=0;x<rc.right;x+=cell)
        {
            const auto world=DesignWorld(s,x+cell/2.0,y+cell/2.0);
            const double wx=world[h],wy=world[v];
            bool inside=false;
            for(const auto& box:boxes)
            {
                if(wx<box.lo0 || wx>box.hi0 || wy<box.lo1 || wy>box.hi1)continue;
                if(LightInsideHull(*box.hull,wx,wy)){inside=true;break;}
            }
            if(!inside)continue;
            bool lit=false;
            for(const auto& light:lights)
            {
                const double dx=wx-light.x,dy=wy-light.y;
                if(dx*dx+dy*dy<=light.r2){lit=true;break;}
            }
            if(!lit)g.FillRectangle(&shade,x,y,cell,cell);
        }
}
// The Light and Shadow map as a layer: each walkable floor patch on this
// storey as a square in the top view (blue hidden, yellow partly visible, red
// exposed), or as a bar at its height in an elevation.
std::string LightShadowStatus()
{
    const auto& cells=LightShadowMap::Patches();
    if(cells.empty())return "Light and shadow map: no floor patches. Build the geometry (and lighting) first; RE+ Tools > Light and Shadow Settings shows why.";
    const auto summary=LightShadow::Summarise(cells);
    return "Light and shadow map: "+std::to_string(summary.total)+" floor patches; blue hidden "+std::to_string(LightShadow::Percent(summary.bands[0],summary.total))
        +"%, yellow partly visible "+std::to_string(LightShadow::Percent(summary.bands[1],summary.total))+"%, red exposed "
        +std::to_string(LightShadow::Percent(summary.bands[2],summary.total))+"%. An estimate: see RE+ Tools > Light and Shadow Settings.";
}
void LightShadowPaint(DesignState& s,Gdiplus::Graphics& g)
{
    if(!s.lightShadow)return;
    using namespace Gdiplus;
    const auto& cells=LightShadowMap::Patches();
    const double size=LightShadowMap::PatchSize();
    RECT rc{};
    GetClientRect(s.canvas,&rc);
    SolidBrush brushes[3]={SolidBrush(Color(120,60,130,255)),SolidBrush(Color(120,255,210,30)),SolidBrush(Color(130,255,60,60))};
    const int h=DesignHorizontal(s),v=DesignVertical(s);
    const bool top=3-h-v==2;
    const float side=static_cast<float>(size*0.9*s.zoom),bar=(std::max)(2.f,static_cast<float>(size*0.12*s.zoom));
    for(const auto& cell:cells)
    {
        if(!DesignOnFloor(s,cell.at.z,cell.at.z))continue;
        const auto at=DesignScreen(s,Vector{cell.at.x,cell.at.y,cell.at.z});
        if(at.X<-side || at.Y<-side || at.X>rc.right+side || at.Y>rc.bottom+side)continue;
        auto& brush=brushes[static_cast<int>(cell.band)];
        if(top)g.FillRectangle(&brush,at.X-side/2,at.Y-side/2,side,side);
        else g.FillRectangle(&brush,at.X-side/2,at.Y-bar,side,bar);
    }
}
// Where each team can walk as a layer: each cell on this storey as a square in
// the top view (green both teams, blue spies only, red mercs only), or as a bar
// at its height in an elevation.
void ReachPaint(DesignState& s,Gdiplus::Graphics& g)
{
    if(!s.reach)return;
    using namespace Gdiplus;
    const auto& patches=Reachability::Patches();
    const double size=Reachability::CellSize();
    RECT rc{};
    GetClientRect(s.canvas,&rc);
    SolidBrush both(Color(110,60,210,90)),spies(Color(130,60,130,255)),mercs(Color(130,255,60,60));
    const int h=DesignHorizontal(s),v=DesignVertical(s);
    const bool top=3-h-v==2;
    // At least 2 pixels, so a whole map's cells still show zoomed out.
    const float side=(std::max)(2.f,static_cast<float>(size*0.9*s.zoom)),bar=(std::max)(2.f,static_cast<float>(size*0.12*s.zoom));
    for(const auto& patch:patches)
    {
        if(!DesignOnFloor(s,patch.at[2],patch.at[2]))continue;
        const auto at=DesignScreen(s,Vector{patch.at[0],patch.at[1],patch.at[2]});
        if(at.X<-side || at.Y<-side || at.X>rc.right+side || at.Y>rc.bottom+side)continue;
        auto& brush=patch.teams==3?both:patch.teams==1?spies:mercs;
        if(top)g.FillRectangle(&brush,at.X-side/2,at.Y-side/2,side,side);
        else g.FillRectangle(&brush,at.X-side/2,at.Y-bar,side,bar);
    }
}
// Light icons, their rim handles, and the live outline of a light being dragged.
void LightPaint(DesignState& s,Gdiplus::Graphics& g,const std::function<void(const std::string&,Gdiplus::PointF)>& label)
{
    using namespace Gdiplus;
    const bool dragging=(s.drag.kind==DesignDrag::Kind::Device || s.drag.kind==DesignDrag::Kind::LightRadius) && s.drag.moved && !s.drag.device.is_null();
    for(const auto& stored:s.lights)
    {
        Json light=stored;
        if(!DesignPointShown(s,light))continue;
        const bool live=dragging && light.at("path")==s.drag.device.at("path");
        if(live)
        {
            light["position"]=s.drag.devicePose.position;
            light["radius"]=s.drag.deviceLength;
        }
        const Vector position=light.at("position");
        if(!DesignOnFloor(s,position[2],position[2]))continue;
        const auto at=DesignScreen(s,position);
        const float radius=static_cast<float>(light.value("radius",0.0)*s.zoom);
        if(live)
        {
            Pen outline(Color(200,230,190,60),1.5f);
            outline.SetDashStyle(DashStyleDash);
            g.DrawEllipse(&outline,at.X-radius,at.Y-radius,2*radius,2*radius);
        }
        // A small sun: the body and four rays.
        const bool selected=light.value("selected",false);
        SolidBrush body(selected?Color(255,255,200,40):Color(235,240,190,60));
        Pen ray(Color(220,200,150,40),1.5f);
        g.FillEllipse(&body,at.X-5,at.Y-5,10.f,10.f);
        for(int i=0;i<4;++i)
        {
            const double angle=i*3.14159265358979323846/2;
            g.DrawLine(&ray,static_cast<float>(at.X+std::cos(angle)*7),static_cast<float>(at.Y+std::sin(angle)*7),
                       static_cast<float>(at.X+std::cos(angle)*11),static_cast<float>(at.Y+std::sin(angle)*11));
        }
        // The rim handle: drag it to set the reach.
        Pen ring(Color(230,40,40,40),1.5f);
        SolidBrush fill(Color(230,255,255,255));
        g.FillEllipse(&fill,at.X+radius-5,at.Y-5,10.f,10.f);
        g.DrawEllipse(&ring,at.X+radius-5,at.Y-5,10.f,10.f);
        label(LightLabel(light)+" "+Design::Round(light.value("radius",0.0))+" / "+Design::Round(light.value("brightness",0.0)),{at.X+8,at.Y-18});
    }
}
// Rooms and corridors that no light reaches at all.
std::vector<std::string> LightIssues(DesignState& s)
{
    std::vector<std::string> issues;
    Json data;
    try{data=DesignData(s);}catch(const std::exception&){return issues;}
    for(const auto& piece:data.at("pieces"))
    {
        const auto kind=piece.at("spec").at("kind").get<std::string>();
        if(kind!="Room" && kind!="Corridor")continue;
        const bool live=DesignAnyLive(s,piece.at("members"));
        if(!live)continue;
        Design::Extent bounds;
        try{bounds=DesignBoundsOf(piece.at("spec"),{piece.at("position").get<Vector>(),piece.at("rotation").get<Rotation>()});}
        catch(const std::exception&){continue;}
        bool lit=false;
        for(const auto& light:s.lights)
        {
            const Vector p=light.at("position");
            double distance2=0;
            for(int axis=0;axis<3;++axis)
            {
                const double nearest=std::clamp(p[axis],bounds.lo[axis],bounds.hi[axis]);
                distance2+=(p[axis]-nearest)*(p[axis]-nearest);
            }
            const double r=light.value("radius",0.0);
            if(distance2<=r*r){lit=true;break;}
        }
        if(!lit)issues.push_back(piece.at("spec").value("name",kind)+" ("+kind+") has no light reaching it: dark for spies and blind for mercs.");
    }
    return issues;
}
// Places a preset light at a point: at the ceiling of the room it lands in,
// or 160 units above the floor clicked on.
void LightPlace(DesignState& s,const LightPreset& preset,const Vector& at)
{
    const auto room=DesignRoomAt(s,at);
    Vector position=at;
    position[2]=room.found?room.base+room.height-24:DesignFloorAt(s,at)+160;
    Json properties={{"LightRadius",Design::Round(preset.radius/25.0,3)},{"EchelonRange",std::to_string(preset.radius)},{"LightBrightness",std::to_string(preset.brightness)},
                     {"LightHue",std::to_string(preset.hue)},{"LightSaturation",std::to_string(preset.saturation)},
                     {"LightType",preset.lightType},{"LightEffect",preset.effect}};
    const auto created=Editor::CreateSecurityActor(preset.type,{position,{}},properties);
    DesignRefresh(s);
    DesignStatus(s,std::string(preset.name)+" placed at "+Design::Round(position[0])+", "+Design::Round(position[1])+", "+Design::Round(position[2])
        +". Drag it to move it, drag its rim to set the reach, right-click to edit it. Rebuild lighting to see it in the viewports.");
}
// The light's editable settings, and the properties they become.
std::vector<InputField> LightFields(const Json& light)
{
    auto choice=[](const std::string& current,const char* const* options,size_t count,const char* fallback)
    {
        for(size_t i=0;i<count;++i)if(current==options[i])return current;
        return std::string(fallback);
    };
    return {
        {"Brightness (0 to 255)",Design::Round(light.value("brightness",64.0)),{}},
        {"Reach (units)",Design::Round(light.value("radius",400.0)),{}},
        {"Hue (0 red, 42 yellow, 85 green, 170 blue)",Design::Round(light.value("hue",0.0)),{}},
        {"Saturation (0 full colour, 255 white)",Design::Round(light.value("saturation",255.0)),{}},
        {"Type",choice(light.value("type",std::string()),kLightTypes,std::size(kLightTypes),"LT_Steady"),std::vector<std::string>(std::begin(kLightTypes),std::end(kLightTypes))},
        {"Effect",choice(light.value("effect",std::string()),kLightEffects,std::size(kLightEffects),"LE_None"),std::vector<std::string>(std::begin(kLightEffects),std::end(kLightEffects))}};
}
// The lightmap bake reads LightRadius (in 25-unit steps); the game's own
// Echelon lighting reads EchelonRange in units. Both follow the reach.
Json LightFieldsToProperties(const std::vector<std::string>& values)
{
    if(values.size()<6)throw std::runtime_error("Incomplete light settings.");
    const int brightness=static_cast<int>(Design::Number(values[0],0,255));
    const double reach=Design::Number(values[1],8,16000);
    const int hue=static_cast<int>(Design::Number(values[2],0,255));
    const int saturation=static_cast<int>(Design::Number(values[3],0,255));
    return {{"LightBrightness",std::to_string(brightness)},{"LightRadius",Design::Round(reach/25,3)},{"EchelonRange",Design::Round(reach,1)},
            {"LightHue",std::to_string(hue)},{"LightSaturation",std::to_string(saturation)},{"LightType",values[4]},{"LightEffect",values[5]}};
}
void SheetShowLight(DesignState& s,const Json& light);
void LightEditForm(DesignState& s,const Json& light){SheetShowLight(s,light);}
