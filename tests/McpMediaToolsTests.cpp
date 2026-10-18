// The media and import tools without Windows or an editor: Media.h's pure
// helpers (frame sampling, scene picking, ffmpeg's report, command-line
// quoting, palettes, grids, occupancy, fitting, OBJ to ASE), Media.cpp's
// portable paths (ImageCodec decoding, processes, ffmpeg when it is
// installed, the error when no decoder is), and the five tools against a
// FakeBridge: media_extract_frames, media_read_image, image_convert,
// import_to_spec and assets_import. Links Media.cpp, Tools/MediaTools.cpp,
// Tools/ImportTools.cpp, WorkflowModel.cpp and RecoveredPolygonImport.cpp.
#include "../Reloaded.Editor.Mcp/Media.h"
#include "../Reloaded.Editor.Mcp/FakeBridge.h"
#include "../Reloaded.Editor.Mcp/McpProtocolModel.h"
#include "../Reloaded.Editor.Mcp/Importers/Import.h"
#include <cstdlib>
#include <iostream>
#include <source_location>
using namespace Mcp;
using Json=Workflow::Json;
namespace fs=std::filesystem;
void Check(bool value,const std::string& message,std::source_location where=std::source_location::current())
{
    if(!value)throw std::runtime_error(message+" (line "+std::to_string(where.line())+")");
}
template<class F> void Reject(F f,const std::string& expected={},std::source_location where=std::source_location::current())
{
    bool caught=false;
    try{f();}catch(const std::exception& e){caught=true;if(!expected.empty() && std::string(e.what()).find(expected)==std::string::npos)throw std::runtime_error("line "+std::to_string(where.line())+": wrong error: "+e.what());}
    if(!caught)throw std::runtime_error("invalid input accepted at line "+std::to_string(where.line()));
}
fs::path Fixtures()
{
    for(const auto& dir:{fs::path("tests/mcp/fixtures"),fs::path("mcp/fixtures"),fs::path(__FILE__).parent_path()/"mcp/fixtures"})
        if(fs::exists(dir/"crate.obj"))return fs::absolute(dir).lexically_normal();
    throw std::runtime_error("tests/mcp/fixtures not found; run from the repository root");
}
Image::Rgba Expected(const fs::path& file)
{
    const auto bytes=Media::ReadFile(file);
    Check(bytes.compare(0,4,"RGBA")==0,"expected-pixels file");
    Image::Rgba out(static_cast<int>(Image::Detail::Le32(Image::Detail::Bytes(bytes)+4)),static_cast<int>(Image::Detail::Le32(Image::Detail::Bytes(bytes)+8)));
    std::memcpy(out.pixels.data(),bytes.data()+12,out.pixels.size());
    return out;
}
std::string Text(const ToolResult& r){std::string t;for(const auto& c:r.content)if(c.type=="text")t+=c.text+"\n";return t;}
size_t Images(const ToolResult& r){size_t n=0;for(const auto& c:r.content)n+=c.type=="image";return n;}
Image::Rgba ImageAt(const ToolResult& r,size_t which)
{
    for(const auto& c:r.content)if(c.type=="image" && which--==0)return Image::DecodePng(Bridge::Unbase64(c.data));
    throw std::runtime_error("no such image");
}
// A temporary folder, removed afterwards.
struct Scratch
{
    fs::path dir;
    Scratch(){dir=fs::temp_directory_path()/("mcp-media-tests-"+std::to_string(reinterpret_cast<uintptr_t>(this))+"-"+Media::Stamp());fs::create_directories(dir);}
    ~Scratch(){std::error_code e;fs::remove_all(dir,e);}
};
// PATH set to an empty folder for a while, so no ffmpeg is found.
struct NoPath
{
    std::string saved;
    NoPath(){saved=Get();Set("");}
    ~NoPath(){Set(saved);}
    static std::string Get()
    {
#ifdef _WIN32
        char* value=nullptr;size_t size=0;std::string out;
        if(!_dupenv_s(&value,&size,"PATH") && value)out=value;
        std::free(value);return out;
#else
        const char* value=std::getenv("PATH");return value?value:"";
#endif
    }
    static void Set(const std::string& value)
    {
#ifdef _WIN32
        _putenv_s("PATH",value.c_str());
#else
        setenv("PATH",value.c_str(),1);
#endif
    }
};
struct Harness
{
    FakeBridge fake;Registry registry;Scratch scratch;fs::path work,system;std::unique_ptr<ToolContext> context;std::vector<std::string> progress;
    Harness()
    {
        work=scratch.dir/"work";system=scratch.dir/"System";fs::create_directories(work);fs::create_directories(system);
        Ini("");
        RegisterMediaTools(registry);RegisterImportTools(registry);
        context=std::make_unique<ToolContext>(ToolContext{fake,system,work});
        context->progress=[this](double,double,const std::string& m){progress.push_back(m);};
    }
    void Ini(const std::string& lines){Media::WriteFile(system/"Reloaded_Editor.ini","[Mcp]\r\nEnabled=1\r\n"+lines);}
    ToolResult Run(const std::string& name,const Json& args,bool checkSchema=true)
    {
        const auto* tool=registry.FindTool(name);
        Check(tool!=nullptr,"tool "+name+" is registered");
        if(checkSchema){const auto problem=Protocol::CheckArguments(tool->inputSchema,args);Check(problem.empty(),name+" arguments fit its schema: "+problem);}
        return tool->run(args,*context);
    }
};
void Png(const fs::path& file,const Image::Rgba& image){Media::WriteFile(file,Image::EncodePng(image));}

void PureFrames()
{
    Check(Media::TimeText(0)=="0:00.0" && Media::TimeText(62.54)=="1:02.5" && Media::TimeText(3600)=="60:00.0","time text");
    Check(Media::QuoteArgument("plain")=="plain" && Media::QuoteArgument("")=="\"\"" && Media::QuoteArgument("a b")=="\"a b\"","plain quoting");
    Check(Media::QuoteArgument("say \"hi\"")=="\"say \\\"hi\\\"\"","quotes escaped");
    Check(Media::QuoteArgument("C:\\My Videos\\")=="\"C:\\My Videos\\\\\"","trailing backslashes doubled before the closing quote");
    Check(Media::QuoteArgument("a\\\\\"b c")=="\"a\\\\\\\\\\\"b c\"","backslashes before a quote doubled, plus one");
    Check(Media::QuoteArgument("C:\\x\\y.mp4")=="C:\\x\\y.mp4","backslashes alone kept");
    const std::string log="Input #0, avi, from 'two.avi':\n  Duration: 00:01:02.50, start: 0.000000, bitrate: 377 kb/s\n"
                          "  Stream #0:0: Video: h264 (High) (avc1 / 0x31637661), yuv420p(tv, bt709), 1920x1080 [SAR 1:1 DAR 16:9], 25 fps\n";
    Check(std::abs(Media::FfmpegDuration(log)-62.5)<1e-9,"duration parsed");
    Check(Media::FfmpegVideoSize(log)==std::make_pair(1920,1080),"size parsed past the codec tag");
    Check(Media::FfmpegVideoSize("Stream #0:0: Video: mpeg2video, yuv420p, 720x576 [SAR 64:45 DAR 16:9], 25 fps\n")==std::make_pair(1024,576),"anamorphic size in display proportions");
    Check(Media::FfmpegDuration("  Duration: N/A, bitrate: N/A")<0 && Media::FfmpegDuration("nothing")<0,"no duration");
    Check(Media::FfmpegVideoSize("Stream #0:1: Audio: aac")==std::make_pair(0,0),"no video");

    Media::FrameOptions o;o.outDir="x";
    std::vector<std::string> notes;
    auto times=Media::SampleTimes(o,10,notes);
    Check(times.size()==5 && times[0]==0 && times[4]==8 && notes.empty(),"interval every 2 s over 10 s");
    o.count=3;times=Media::SampleTimes(o,10,notes);
    Check(times.size()==3 && notes.size()==1 && notes[0].find("Stopped at 3")!=std::string::npos,"interval capped by count, with a note");
    notes.clear();o.mode="count";o.count=4;o.start=2;o.end=6;
    times=Media::SampleTimes(o,10,notes);
    Check(times.size()==4 && std::abs(times[0]-2.5)<1e-9 && std::abs(times[3]-5.5)<1e-9,"count takes slice middles");
    o.end=20;times=Media::SampleTimes(o,10,notes);
    Check(std::abs(times[3]-(2+3.5*2))<1e-9 && notes.size()==1,"end past the video stops at its end");
    notes.clear();o.mode="scene";o.count=4;o.start=0;o.end=-1;
    times=Media::SampleTimes(o,2,notes);
    Check(times.size()==16 && std::abs(times[8]-1.0)<1e-9,"scene candidates at slice starts");
    o.count=64;times=Media::SampleTimes(o,600,notes);
    Check(times.size()==192,"scene candidates capped");
    Reject([&]{auto oo=o;oo.mode="count";Media::SampleTimes(oo,-1,notes);},"needs end");
    Reject([&]{auto oo=o;oo.start=700;Media::SampleTimes(oo,600,notes);},"past the end");
    o.mode="interval";o.count=5;times=Media::SampleTimes(o,-1,notes);
    Check(times.size()==5,"interval with an unknown length reads until count");
    Reject([&]{auto oo=o;oo.mode="slow";Media::CheckFrameOptions(oo);},"interval, count or scene");
    Reject([&]{auto oo=o;oo.count=65;Media::CheckFrameOptions(oo);},"1 to 64");
    Reject([&]{auto oo=o;oo.maxEdge=4000;Media::CheckFrameOptions(oo);},"maxEdge");
    Reject([&]{auto oo=o;oo.end=0;Media::CheckFrameOptions(oo);},"after start");

    // Eight red frames, then eight different ones with a slow two-frame fade.
    Image::Rgba red(16,16,{250,0,0,255}),grey(16,16,{128,128,128,255}),fade(16,16,{190,64,64,255}),white(16,16,{255,255,255,255});
    std::vector<Media::Histogram> h;
    for(int i=0;i<8;++i)h.push_back(Media::LumaHistogram(red));
    h.push_back(Media::LumaHistogram(fade));h.push_back(Media::LumaHistogram(grey));
    for(int i=0;i<4;++i)h.push_back(Media::LumaHistogram(grey));
    h.push_back(Media::LumaHistogram(white));h.push_back(Media::LumaHistogram(white));
    Check(Media::HistogramChange(h[0],h[1])==0 && Media::HistogramChange(h[0],h.back())==1,"histogram change bounds");
    std::vector<double> change;
    auto picks=Media::PickScenes(h,8,0.3,change);
    Check(picks.size()==3 && picks[0]==0 && (picks[1]==8 || picks[1]==9) && picks[2]==14,"cuts picked once each, fades not twice: "+Json(picks).dump());
    picks=Media::PickScenes(h,2,0.3,change);
    Check(picks.size()==2 && picks[0]==0,"count caps the scenes");
    picks=Media::PickScenes(h,8,1.01,change);
    Check(picks.size()==1,"nothing over threshold keeps the first frame");
    Check(Media::FrameName(3,12.5)=="frame_003_12.500s.png","frame names");
}

void PureImages()
{
    // 60% white, 30% black, 10% red, with slight noise in the white.
    Image::Rgba image(100,10,{255,255,255,255});
    for(int y=0;y<10;++y)for(int x=0;x<100;++x)
    {
        auto* p=image.At(x,y);
        if(x>=60 && x<90){p[0]=p[1]=p[2]=0;}
        else if(x>=90){p[0]=220;p[1]=10;p[2]=10;}
        else if((x+y)%7==0){p[0]=p[1]=p[2]=250;}
    }
    auto palette=Media::Palette(image,8);
    Check(palette.size()==3,"near colours merge: "+std::to_string(palette.size()));
    Check(Media::Hex(palette[0].rgb).substr(0,3)=="#fe" || Media::Hex(palette[0].rgb)=="#ffffff","white first: "+Media::Hex(palette[0].rgb));
    Check(std::abs(palette[0].share-0.6)<1e-9 && std::abs(palette[1].share-0.3)<1e-9 && Media::Hex(palette[1].rgb)=="#000000" && Media::Hex(palette[2].rgb)=="#dc0a0a","palette shares");
    Check(Media::ParseHex("#0a0B0c")==Media::Rgb{10,11,12} && Media::ParseHex("fff")==Media::Rgb{255,255,255},"hex parsing");
    Reject([]{Media::ParseHex("#12345");},"#rrggbb");

    // Occupancy: a black 20x20 block in the top-left of a white 40x40.
    Image::Rgba plan(40,40,{255,255,255,255});
    for(int y=0;y<20;++y)for(int x=0;x<20;++x){auto* p=plan.At(x,y);p[0]=p[1]=p[2]=0;}
    for(int y=30;y<40;++y)for(int x=30;x<40;++x)plan.At(x,y)[3]=0;
    auto occupancy=Media::Occupancy(plan,{255,255,255},48,{0,0});
    Check(occupancy.at("ink").get<double>()==0.25 && std::abs(occupancy.at("transparent").get<double>()-0.0625)<1e-9,"ink and transparent shares");
    Check(occupancy.at("inkBox")==Json({{"x",0.0},{"y",0.0},{"width",20},{"height",20}}),"ink box: "+occupancy.at("inkBox").dump());
    Media::GridSpec grid;grid.spacing=10;
    auto cells=Media::CellOccupancy(plan,{255,255,255},48,{0,0},grid);
    Check(cells.at("columns")==4 && cells.at("rows")==4,"cells");
    Check(cells.at("cells")==Json({"##..","##..","....","...."}),"cell map: "+cells.at("cells").dump());
    grid.spacing=15;
    cells=Media::CellOccupancy(plan,{255,255,255},48,{0,0},grid);
    Check(cells.at("cells").at(0)=="#3." && cells.at("cells").at(1)=="31.","partial cells in tenths: "+cells.at("cells").dump());
    // A crop offset: the region (10..30, 10..30) of the plan.
    const auto region=Image::Crop(plan,10,10,20,20);
    grid.spacing=10;
    cells=Media::CellOccupancy(region,{255,255,255},48,{10,10},grid);
    Check(cells.at("firstColumn")==1 && cells.at("columns")==2 && cells.at("cells")==Json({"#.",".."}),"cells of a crop: "+cells.dump());
    grid.origin={5,5};
    cells=Media::CellOccupancy(region,{255,255,255},48,{10,10},grid);
    Check(cells.at("columns")==3 && cells.at("cells").at(0)=="#5.","an origin moves the cells: "+cells.dump());

    // The grid: lines every 10 px, the origin cyan, labels on top and left.
    Image::Rgba shown(100,100,{255,255,255,255});
    Media::GridSpec g;g.spacing=10;
    auto drawn=Media::DrawGrid(shown,1,{0,0},g);
    Check(drawn.at("unit")=="px" && drawn.at("columns").at("first")==0 && drawn.at("columns").at("last")==9 && drawn.at("drawnEvery")==1,"grid lines: "+drawn.dump());
    Check(shown.At(0,45)[0]<100 && shown.At(0,45)[2]>200,"origin line is cyan");
    Check(shown.At(30,45)[1]<230 && shown.At(30,45)[0]==255,"minor line is magenta");
    Check(shown.At(50,45)[1]<shown.At(30,45)[1],"every fifth line is brighter");
    Check(shown.At(35,45)[0]==255 && shown.At(35,45)[1]==255,"between lines untouched");
    bool label=false;for(int x=12;x<30;++x)for(int y=2;y<10;++y)label=label||shown.At(x,y)[0]<40;
    Check(label,"a label's dark halo at the top");
    Image::Rgba scaled(200,100,{255,255,255,255});
    Media::GridSpec uu;uu.spacing=8;uu.uuPerPx=16;uu.origin={4,0};
    drawn=Media::DrawGrid(scaled,2,{0,0},uu);
    Check(drawn.at("unit")=="uu" && drawn.at("perLine")==128 && drawn.at("shownSpacingPx")==16 && drawn.at("columns").at("first")==0,"uu grid: "+drawn.dump());
    Check(scaled.At(8,45)[2]>200 && scaled.At(8,45)[0]<100,"origin at source 4 is shown 8");
    Image::Rgba dense(50,50,{255,255,255,255});
    Media::GridSpec tight;tight.spacing=1;
    drawn=Media::DrawGrid(dense,1,{0,0},tight);
    Check(drawn.at("drawnEvery")==4,"dense grids skip lines");
    Reject([&]{Media::GridSpec bad;bad.spacing=0;Media::DrawGrid(dense,1,{0,0},bad);},"more than 0");

    // Fitting a 4x2 picture into 4x4.
    Image::Rgba wide(4,2,{200,100,50,255});
    auto padded=Media::Fit(wide,4,4,"pad",true,{0,0,0});
    Check(padded.width==4 && padded.height==4 && padded.At(0,0)[3]==0 && padded.At(0,1)[3]==255 && padded.At(0,3)[3]==0,"pad letterboxes");
    auto flat=Media::Fit(wide,4,4,"pad",false,{10,20,30});
    Check(flat.Opaque() && flat.At(0,0)[0]==10 && flat.At(0,0)[2]==30 && flat.At(1,1)[0]==200,"pad without alpha uses the background");
    auto cropped=Media::Fit(wide,4,4,"crop",true,{0,0,0});
    Check(cropped.Opaque() && cropped.At(0,0)[0]==200,"crop fills");
    auto stretched=Media::Fit(wide,8,8,"stretch",true,{0,0,0});
    Check(stretched.width==8 && stretched.Opaque(),"stretch");
    Reject([&]{Media::Fit(wide,4,4,"zoom",true,{0,0,0});},"pad, crop or stretch");
    Check(Media::PowerOfTwo(256) && !Media::PowerOfTwo(255) && !Media::PowerOfTwo(0),"powers of two");

    // Paths and roots.
    Scratch s;
    Check(Media::Inside(s.dir/"a"/"b.png",s.dir) && Media::Inside(s.dir,s.dir) && !Media::Inside(s.dir.parent_path(),s.dir),"inside");
    Check(!Media::Inside(fs::path(s.dir.string()+"-other")/"x",s.dir),"a sibling with the same prefix is outside");
    Check(!Media::Inside(s.dir/".."/"x",s.dir),"dot-dot leaves the root");
    const auto roots=Media::WriteRoots(s.dir/"work"," \"/srv/one\" ;; /srv/two ");
    Check(roots.size()==3 && roots[1]==fs::path("/srv/one") && roots[2]==fs::path("/srv/two"),"WriteRoots list");
    Reject([&]{Media::CheckWritable("/etc/passwd",roots);},"only inside its work folder");
    Media::CheckWritable(s.dir/"work"/"frames"/"f.png",roots);
    Check(Media::SafeName("My Plan (v2).png")=="My_Plan__v2_.png" && Media::SafeName("..")=="file","safe names");
    Check(Media::Stamp()!=Media::Stamp(),"stamps are unique");
}

void ObjToAse()
{
    const auto obj=Media::ReadFile(Fixtures()/"crate.obj");
    const std::string mtl="newmtl Wood\nKd 0.5 0.4 0.3\nmap_Kd -s 1 1 1 textures/wood.tga\n\nnewmtl Metal\nmap_Kd metal.png\n";
    const auto ase=Media::ObjToAse(obj,{{"name","Crate"},{"mtl",mtl}});
    Check(ase.info.at("triangles")==12 && ase.info.at("vertices")==8 && ase.info.at("skippedFaces")==1,"crate: 6 quads, one bad face: "+ase.info.dump());
    Check(ase.info.at("materials")==Json({"Wood","Metal"}) && ase.info.at("bitmaps").at("Wood")=="textures/wood.tga" && ase.info.at("bitmaps").at("Metal")=="metal.png","materials and bitmaps");
    Check(ase.text.find("*MATERIAL_CLASS \"Multi/Sub-Object\"")!=std::string::npos && ase.text.find("*NUMSUBMTLS 2")!=std::string::npos,"two materials are a Multi/Sub-Object");
    Check(ase.text.find("*MESH_MTLID 1")!=std::string::npos && ase.text.find("*BITMAP \"textures/wood.tga\"")!=std::string::npos,"face material ids and bitmaps");
    Check(ase.text.find("*MESH_NUMTVERTEX 4")!=std::string::npos && ase.text.find("*MESH_NUMTVFACES 12")!=std::string::npos,"texture coordinates kept");
    Check(ase.info.at("warnings").size()==3 && ase.info.at("warnings").at(2).get<std::string>().find("scale 100")!=std::string::npos,"warnings: the bad face, corners without uvs, a 2-unit mesh: "+ase.info.at("warnings").dump());
    // The ASE reads back through the importers with the same Unreal bounds
    // the OBJ importer reports for the OBJ itself.
    Json objReport,aseReport;
    Import::ImportObj(obj,{{"path","crate.obj"}},objReport);
    Import::ImportAse(ase.text,{{"path","crate.ase"}},aseReport);
    const auto& objAsset=objReport.at("assets").at(0);const auto& aseAsset=aseReport.at("assets").at(0);
    Check(aseAsset.at("vertices")==8 && aseAsset.at("triangles")==12 && aseAsset.at("groups")==Json({"Crate"}) && aseAsset.at("materials")==Json({"Crate_Multi","Wood","Metal"}),"the ASE importer reads it: "+aseAsset.dump());
    auto same3=[](const Json& a,const Json& b){for(size_t i=0;i<3;++i)if(std::abs(a[i].get<double>()-b[i].get<double>())>1e-6)return false;return true;};
    Check(same3(objAsset.at("unrealBounds").at("min"),aseAsset.at("unrealBounds").at("min")) && same3(objAsset.at("unrealBounds").at("max"),aseAsset.at("unrealBounds").at("max")),
          "OBJ and converted ASE land in the same place: "+objAsset.at("unrealBounds").dump()+" vs "+aseAsset.at("unrealBounds").dump());
    Check(same3(ase.info.at("unrealBounds").at("max"),objAsset.at("unrealBounds").at("max")) && ase.info.at("size")==Json({2.0,2.0,2.0}),"reported size");
    // Z-up and scale.
    const auto zup=Media::ObjToAse("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf 1 2 3\nf 1 2 4\n",{{"up","z"},{"scale",100}});
    Check(zup.info.at("size")==Json({100.0,100.0,100.0}) && zup.info.at("materials")==Json({"Default"}) && zup.info.at("warnings").size()==1,"z-up, scaled, one material, no uvs: "+zup.info.dump());
    Check(zup.text.find("*MESH_VERTEX     3\t0.0000\t0.0000\t100.0000")!=std::string::npos,"z-up keeps axes");
    Check(zup.text.find("*MATERIAL_CLASS \"Standard\"")!=std::string::npos,"one material is Standard");
    const auto tiny=Media::ObjToAse("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n");
    Check(tiny.info.at("warnings").size()==1 && tiny.info.at("warnings").at(0).get<std::string>().find("scale 100")!=std::string::npos,"a metre-sized mesh is flagged");
    Check(tiny.text.find("*MESH_TFACE 0\t0\t1\t2")!=std::string::npos,"tfaces index uvs");
    const auto negative=Media::ObjToAse("v 0 0 0\nv 9 0 0\nv 0 9 0\nv 0 0 9\nf -4 -3 -2 -1\n");
    Check(negative.info.at("triangles")==2,"negative indices and a fan");
    Reject([]{Media::ObjToAse("v 0 0 0\n");},"no faces");
    Reject([]{Media::ObjToAse("v 0 0\nf 1 1 1\n");},"line 1");
    Reject([]{Media::ObjToAse("v 0 0 0\nf 1 1 1\n",{{"up","x"}});},"up is y");
    Reject([]{Media::ObjToAse("v 0 0 0\nf 1 1 1\n",{{"scale",0}});},"scale");
}

void Decoding()
{
    const auto dir=Fixtures();
    const std::pair<const char*,const char*> fixtures[]={{"rgba8.png","rgba8"},{"rgb8.png","rgb8"},{"grey8.png","grey8"},{"palette4.png","palette4"},{"rgb24.bmp","rgb24"},
                                                          {"rgba32.tga","rgba32"},{"rgb24rle.tga","rgb24"}};
    for(const auto& [name,expect]:fixtures)
    {
        const auto image=Media::DecodeImageFile(dir/name);
        const auto expected=Expected(dir/(std::string(expect)+".rgba"));
        Check(image.width==expected.width && image.height==expected.height && image.pixels==expected.pixels,std::string("decodes ")+name);
    }
    Reject([&]{Media::DecodeImageFile(dir/"crate.obj");},"not an image");
    Reject([&]{Media::DecodeImageFile(dir/"interlaced.png");},"interlaced.png");
    Reject([&]{Media::DecodeImageFile(dir/"missing.png");},"does not exist");
    const auto image=Media::DecodeImageFile(dir/"rgba8.png");
    Check(Image::DecodePng(Media::PngBytes(image)).pixels==image.pixels,"PngBytes round trip");
    Check(Media::ImageFormats().find("PNG")!=std::string::npos,"formats named");
}

void Processes(const fs::path& ffmpeg)
{
    Scratch s;
    const auto log=s.dir/"echo.log";
#ifndef _WIN32
    Check(Media::RunProcess("/bin/echo",{"one two","th\"ree"},log,10000)==0,"echo runs");
    Check(Media::ReadFile(log)=="one two th\"ree\n","arguments arrive as given, no shell: "+Media::ReadFile(log));
    Check(Media::RunProcess("/bin/sh",{"-c","read x; echo got${x}end; exit 3"},log,10000)==3 && Media::ReadFile(log)=="gotend\n","stdin is empty, exit codes come back");
    Reject([&]{Media::RunProcess("/bin/sleep",{"5"},log,200);},"was stopped");
#endif
    Reject([&]{Media::RunProcess(s.dir/"nothing",{},log,1000);},"Cannot start");
    Reject([&]{Media::FindFfmpeg(s.dir/"no-ffmpeg-here");},"[Mcp] Ffmpeg");
    {
        NoPath none;
        Check(Media::FindFfmpeg({}).empty(),"no ffmpeg without PATH");
        Media::FrameOptions o;o.outDir=s.dir/"frames";
        Media::WriteFile(s.dir/"clip.mp4","not really a video");
        Reject([&]{Media::ExtractFrames(s.dir/"clip.mp4",o);},"no ffmpeg was found");
#ifndef _WIN32
        o.backend="mediafoundation";
        Reject([&]{Media::ExtractFrames(s.dir/"clip.mp4",o);},"only on Windows");
        o.backend="auto";
#endif
        Reject([&]{Media::ExtractFrames(s.dir/"missing.mp4",o);},"does not exist");
    }
    if(ffmpeg.empty()){std::cout<<"  (ffmpeg is not installed: the ffmpeg frame tests are skipped)\n";return;}
    Check(Media::FindFfmpeg(ffmpeg.parent_path())==ffmpeg,"a folder holding ffmpeg");
    // Two seconds: one red, then a test pattern.
    const auto video=s.dir/"two scenes.avi";
    Check(Media::RunProcess(ffmpeg,{"-hide_banner","-nostdin","-loglevel","error","-f","lavfi","-i","color=c=red:s=320x240:d=1:r=10","-f","lavfi","-i","testsrc=s=320x240:d=1:r=10",
                                    "-filter_complex","[0:v][1:v]concat=n=2:v=1[v]","-map","[v]","-c:v","mjpeg","-q:v","3","-y",Media::Utf8(video)},s.dir/"make.log",60000)==0,"test video made");
    Media::FrameOptions o;o.outDir=s.dir/"interval";o.intervalS=0.5;o.maxEdge=160;o.ffmpeg=ffmpeg;o.backend="ffmpeg";
    std::vector<std::string> said;o.progress=[&](double,double,const std::string& m){said.push_back(m);};
    auto out=Media::ExtractFrames(video,o);
    Check(out.backend=="ffmpeg" && std::abs(out.duration-2)<0.05 && out.width==320 && out.height==240,"probe");
    Check(out.frames.size()==4 && out.frames[2].time==1.0 && out.frames[0].width==160 && out.frames[0].height==120,"interval frames");
    Check(!said.empty(),"progress reported");
    auto first=Media::DecodeImageFile(out.frames[0].path);
    Check(first.At(80,60)[0]>200 && first.At(80,60)[1]<60,"the first frame is red");
    Check(!fs::exists(o.outDir/"ffmpeg.log"),"the log is cleaned up");
    o.mode="count";o.count=2;o.outDir=s.dir/"count";
    out=Media::ExtractFrames(video,o);
    Check(out.frames.size()==2 && out.frames[0].time==0.5 && out.frames[1].time==1.5,"count frames");
    Check(Media::DecodeImageFile(out.frames[1].path).At(80,60)[0]<250 || Media::DecodeImageFile(out.frames[1].path).At(80,60)[1]>60,"the second is the pattern");
    o.mode="scene";o.count=6;o.outDir=s.dir/"scene";
    out=Media::ExtractFrames(video,o);
    Check(out.frames.size()==2 && out.frames[0].time==0 && std::abs(out.frames[1].time-1.0)<0.13 && out.frames[1].change>0.3,"scene finds the cut: "+std::to_string(out.frames.size()));
    size_t pngs=0;for(const auto& e:fs::directory_iterator(o.outDir))pngs+=e.path().extension()==".png";
    Check(pngs==2,"unpicked candidates are removed");
    o.mode="interval";o.start=5;o.outDir=s.dir/"late";
    Reject([&]{Media::ExtractFrames(video,o);},"past the end");
    const auto sheet=Media::ContactSheet(out.frames,0,400);
    Check(sheet.width<=400 && sheet.height<=400 && sheet.width>sheet.height,"contact sheet");
}

void MediaTools(const fs::path& ffmpeg)
{
    Harness t;
    const auto dir=Fixtures();
    for(const auto& name:{"media_extract_frames","media_read_image","image_convert"})
    {
        const auto* tool=t.registry.FindTool(name);
        Check(tool && tool->description.size()>600 && tool->inputSchema.value("additionalProperties",true)==false,std::string(name)+" is described and closed");
        Check(!Protocol::CheckArguments(tool->inputSchema,{{"nonsense",1}}).empty(),std::string(name)+" refuses unknown arguments");
    }
    // media_read_image: a crop, enlarged, with a grid.
    auto r=t.Run("media_read_image",{{"path",(dir/"rgba8.png").string()},{"crop",{{"x",5},{"y",5},{"width",20},{"height",10}}},{"enlarge",true},{"maxEdge",256},
                                     {"grid",{{"spacingPx",5}}},{"paletteSize",4}});
    Check(!r.isError,"read image: "+Text(r));
    const auto& s=r.structured;
    Check(s.at("format")=="png" && s.at("source")==Json({{"width",37},{"height",23}}) && s.at("shown").at("width")==160 && s.at("shown").at("scale")==8,"read image sizes: "+s.dump());
    Check(s.at("palette").size()==4 && s.at("grid").at("unit")=="px" && s.at("cells").at("columns")==4 && s.at("cells").at("rows")==2,"palette, grid and cells");
    Check(s.at("occupancy").contains("inkBox") && Text(r).find("does not interpret")!=std::string::npos,"occupancy, and the server says it does not interpret");
    Check(Images(r)==1 && ImageAt(r,0).width==160,"the image comes back");
    // A plan in Unreal units.
    Image::Rgba plan(64,32,{255,255,255,255});
    for(int x=0;x<64;++x){plan.At(x,0)[0]=plan.At(x,0)[1]=plan.At(x,0)[2]=0;}
    Png(t.scratch.dir/"plan.png",plan);
    r=t.Run("media_read_image",{{"path",(t.scratch.dir/"plan.png").string()},{"grid",{{"cellUu",128},{"uuPerPx",16},{"origin",{0,0}}}},{"palette",false},{"background","#ffffff"}});
    Check(!r.isError && r.structured.at("grid").at("unit")=="uu" && r.structured.at("grid").at("spacingPx")==8 && !r.structured.contains("palette"),"uu grid: "+Text(r));
    Check(r.structured.at("cells").at("cells").at(0)=="11111111","the drawn top row is a tenth of each cell");
    Check(Text(r).find("Unreal units")!=std::string::npos,"labels named in the text");
    r=t.Run("media_read_image",{{"path",(t.scratch.dir/"plan.png").string()},{"grid",{{"cellUu",128}}}});
    Check(r.isError && Text(r).find("uuPerPx")!=std::string::npos,"cellUu needs uuPerPx");
    r=t.Run("media_read_image",{{"path","relative.png"}});
    Check(r.isError && Text(r).find("full path")!=std::string::npos,"relative paths refused");
    r=t.Run("media_read_image",{{"path",(dir/"rgba8.png").string()},{"crop",{{"x",30},{"y",0},{"width",20},{"height",5}}}});
    Check(r.isError && Text(r).find("outside")!=std::string::npos,"crop outside the image");

    // image_convert.
    r=t.Run("image_convert",{{"file",(dir/"rgba8.png").string()},{"size",16}});
    Check(!r.isError && r.structured.at("width")==16 && r.structured.at("format")=="tga" && r.structured.at("alpha")==true,"converted: "+Text(r));
    const fs::path tga=Media::Utf8Path(r.structured.at("file").get<std::string>());
    Check(Media::Inside(tga,t.work/"converted") && tga.filename()=="rgba8_16x16.tga","written in the work folder");
    const auto converted=Image::DecodeTga(Media::ReadFile(tga));
    Check(converted.width==16 && converted.height==16 && !converted.Opaque(),"a 16x16 TGA with alpha");
    Check(Images(r)==1 && r.structured.at("next").at(0).at("tool")=="assets_import","preview and next step");
    r=t.Run("image_convert",{{"file",(dir/"rgb24.bmp").string()}});
    Check(!r.isError && r.structured.at("width")==32 && r.structured.at("height")==16,"default sizes are the nearest powers of two");
    r=t.Run("image_convert",{{"file",(dir/"rgb24.bmp").string()},{"slot","Menu"}});
    Check(!r.isError && r.structured.at("width")==256 && r.structured.at("slot")=="Menu" && r.structured.at("next").at(0).at("tool")=="map_settings","a Menu slot is 256");
    r=t.Run("image_convert",{{"file",(dir/"rgba8.png").string()},{"slot","LoadingATT0"},{"format","bmp"},{"fit","crop"}});
    Check(!r.isError && r.structured.at("alpha")==false && r.structured.at("width")==512 && Image::DecodeBmp(Media::ReadFile(Media::Utf8Path(r.structured.at("file").get<std::string>()))).Opaque(),"loading screens drop alpha");
    r=t.Run("image_convert",{{"file",(dir/"rgba8.png").string()},{"format","bmp"},{"size",{8,4}}});
    Check(!r.isError && r.structured.at("alpha")==false && r.structured.at("warnings").at(0).get<std::string>().find("BMP")!=std::string::npos,"BMP flattens with a warning");
    r=t.Run("image_convert",{{"file",(dir/"rgba8.png").string()},{"size",100}});
    Check(r.isError && Text(r).find("power of two")!=std::string::npos,"sizes are powers of two");
    r=t.Run("image_convert",{{"file",(dir/"rgba8.png").string()},{"slot","Menu"},{"size",512}});
    Check(r.isError && Text(r).find("256")!=std::string::npos,"slot sizes are fixed");
    r=t.Run("image_convert",{{"file",(dir/"rgba8.png").string()},{"size",16},{"out","/etc/x.tga"}});
    Check(r.isError && Text(r).find("WriteRoots")!=std::string::npos,"writes stay in the work folder");
    const auto out=t.work/"mine"/"tex.tga";
    r=t.Run("image_convert",{{"file",(dir/"rgba8.png").string()},{"size",16},{"out",out.string()}});
    Check(!r.isError && fs::exists(out),"an out path in the work folder");
    r=t.Run("image_convert",{{"file",(dir/"rgba8.png").string()},{"size",16},{"out",out.string()}});
    Check(r.isError && Text(r).find("overwrite")!=std::string::npos,"no silent overwrite");
    r=t.Run("image_convert",{{"file",(dir/"rgba8.png").string()},{"size",16},{"out",out.string()},{"overwrite",true}});
    Check(!r.isError,"overwrite replaces");
    r=t.Run("image_convert",{{"file",(dir/"rgba8.png").string()},{"size",16},{"out",(t.work/"x.png").string()}});
    Check(r.isError && Text(r).find("agree")!=std::string::npos,"extension and format agree");
    const auto extra=t.scratch.dir/"extra";
    t.Ini("WriteRoots="+extra.string()+"\r\n");
    r=t.Run("image_convert",{{"file",(dir/"rgba8.png").string()},{"size",16},{"out",(extra/"t.tga").string()}});
    Check(!r.isError && fs::exists(extra/"t.tga"),"WriteRoots allow other folders");
    t.Ini("");

    // media_extract_frames without a decoder.
    Media::WriteFile(t.scratch.dir/"clip.mp4","not a video");
    {
        NoPath none;
        r=t.Run("media_extract_frames",{{"video",(t.scratch.dir/"clip.mp4").string()}});
        Check(r.isError && Text(r).find("no ffmpeg was found")!=std::string::npos && Text(r).find("[Mcp] Ffmpeg")!=std::string::npos,"a clear error without a decoder: "+Text(r));
    }
    t.Ini("Ffmpeg="+(t.scratch.dir/"nowhere").string()+"\r\n");
    r=t.Run("media_extract_frames",{{"video",(t.scratch.dir/"clip.mp4").string()}});
    Check(r.isError && Text(r).find("[Mcp] Ffmpeg")!=std::string::npos,"a configured ffmpeg that is missing is reported");
    t.Ini("");
    r=t.Run("media_extract_frames",{{"video",(t.scratch.dir/"clip.mp4").string()},{"attach","sheet"}});
    Check(r.isError && Text(r).find("sheet: true")!=std::string::npos,"attach sheet needs a sheet");
    r=t.Run("media_extract_frames",{{"video",(t.scratch.dir/"clip.mp4").string()},{"outDir","/tmp/elsewhere"}});
    Check(r.isError && Text(r).find("WriteRoots")!=std::string::npos,"frames stay in the work folder");
    if(ffmpeg.empty())return;
    const auto video=t.scratch.dir/"scenes.avi";
    Check(Media::RunProcess(ffmpeg,{"-hide_banner","-nostdin","-loglevel","error","-f","lavfi","-i","color=c=blue:s=256x144:d=1:r=10","-f","lavfi","-i","testsrc=s=256x144:d=1:r=10",
                                    "-filter_complex","[0:v][1:v]concat=n=2:v=1[v]","-map","[v]","-c:v","mjpeg","-y",Media::Utf8(video)},t.scratch.dir/"make.log",60000)==0,"video made");
    t.Ini("Ffmpeg="+ffmpeg.string()+"\r\n");
    t.progress.clear();
    r=t.Run("media_extract_frames",{{"video",Media::Utf8(video)},{"mode","count"},{"count",4},{"maxEdge",128},{"sheet",true},{"attach","both"},{"backend","ffmpeg"}});
    Check(!r.isError,"frames extracted: "+Text(r));
    const auto& f=r.structured;
    Check(f.at("backend")=="ffmpeg" && f.at("frames").size()==4 && f.at("frames").at(0).at("width")==128 && f.at("frames").at(0).at("inline")==true,"frame list: "+f.dump());
    Check(f.at("sheet").at("inline")==true && Images(r)==5 && f.at("attached").at("images")==5,"sheet and frames attached");
    Check(Media::Inside(Media::Utf8Path(f.at("outDir").get<std::string>()),t.work/"frames") && fs::exists(Media::Utf8Path(f.at("sheet").at("file").get<std::string>())),"files in the work folder");
    Check(Text(r).find("does not interpret")!=std::string::npos && f.at("interpretation").get<std::string>().find("scct.level-spec")!=std::string::npos,"the server says it does not interpret frames");
    Check(!t.progress.empty(),"progress forwarded");
    r=t.Run("media_extract_frames",{{"video",Media::Utf8(video)},{"mode","scene"},{"attach","none"},{"backend","ffmpeg"}});
    Check(!r.isError && r.structured.at("frames").size()==2 && Images(r)==0 && r.structured.at("frames").at(1).contains("change"),"scene mode through the tool: "+Text(r));
    t.Ini("");
}

void ImportTools()
{
    Harness t;
    const auto dir=Fixtures();
    for(const auto& name:{"import_to_spec","assets_import"})
    {
        const auto* tool=t.registry.FindTool(name);
        Check(tool && tool->description.size()>800 && tool->inputSchema.value("additionalProperties",true)==false,std::string(name)+" is described and closed");
    }
    Check(t.registry.FindTool("import_to_spec")->description.find("tiled (.tmj .tmx)")!=std::string::npos,"formats listed from Import::Formats");
    Check(t.registry.FindTool("assets_import")->inputSchema.at("properties").contains("dryRun"),"assets_import takes dryRun and label");
    // A grid.
    auto r=t.Run("import_to_spec",{{"source",(dir/"grid-basic.txt").string()}});
    Check(!r.isError,"grid import: "+Text(r));
    Check(r.structured.at("format")=="grid" && r.structured.at("spec").at("format")=="scct.level-spec" && r.structured.contains("report"),"a spec and its report");
    const fs::path specFile=Media::Utf8Path(r.structured.at("specFile").get<std::string>());
    Check(Media::Inside(specFile,t.work/"imports") && Bridge::Parse(Media::ReadFile(specFile))==r.structured.at("spec"),"the spec is saved too");
#ifndef RE_MCP_STANDALONE
    Check(Images(r)==1 && ImageAt(r,0).width<=1024 && r.structured.contains("summary"),"a plan of the compiled spec");
#endif
    Check(Text(r).find("starting point")!=std::string::npos && r.structured.at("next").at(0).at("tool")=="plan_render","next steps");
    r=t.Run("import_to_spec",{{"source",(dir/"plan.svg").string()},{"plan",false}});
    Check(!r.isError && r.structured.at("format")=="svg" && Images(r)==0,"svg, no plan");
    r=t.Run("import_to_spec",{{"source",(dir/"cube.vmf").string()},{"maxEdge",256}});
    Check(!r.isError && r.structured.at("format")=="vmf" && (Images(r)==0 || ImageAt(r,0).width<=256),"vmf, with polygon brushes drawn");
    // A raster plan: walls, floor, a spy start and a merc start.
    Image::Rgba picture(48,32,{0,0,0,255});
    for(int y=4;y<28;++y)for(int x=4;x<44;++x){auto* p=picture.At(x,y);p[0]=p[1]=p[2]=255;}
    for(int y=8;y<11;++y)for(int x=8;x<11;++x){auto* p=picture.At(x,y);p[0]=0;p[1]=0;p[2]=255;}
    for(int y=20;y<23;++y)for(int x=36;x<39;++x){auto* p=picture.At(x,y);p[0]=255;p[1]=0;p[2]=0;}
    Png(t.scratch.dir/"floor plan.png",picture);
    r=t.Run("import_to_spec",{{"source",(t.scratch.dir/"floor plan.png").string()},{"options",{{"pixelsPerCell",2}}}});
    Check(!r.isError && r.structured.at("format")=="image","image plan: "+Text(r));
    Check(r.structured.at("report").at("overlay").get<std::string>().find("Attached")==0 && Images(r)>=1 && ImageAt(r,0).width>0,"the overlay comes back as an image, not base64 in the report");
    // An OBJ: the open map names the suggested package.
    t.fake.On("editor.state",Json{{"open",true},{"mapFile","C:\\Games\\SCCT\\Packages\\MapsEd\\AquaD.sdc"}});
    r=t.Run("import_to_spec",{{"source",(dir/"crate.obj").string()}});
    Check(!r.isError && r.structured.at("report").at("assets").at(0).at("suggestedPackage")=="AquaD_STM","the mesh's package follows the map: "+Text(r));
    Check(r.structured.at("next").at(0).at("tool")=="assets_import" && r.structured.at("next").at(0).at("args").at("items").at(0).at("package")=="AquaD_STM","assets_import is next");
    // The open map, through a blob.
    const auto exportText=Media::ReadFile(dir/"authoring-export.json");
    t.fake.On("authoring.export",Json{{"blob","b1"},{"size",exportText.size()}});
    t.fake.On("blob.read",[&](const Json& a,const CallOptions&){Check(a.at("blob")=="b1","blob handle");return Json{{"data",Bridge::Base64(exportText)},{"eof",true}};});
    t.fake.On("blob.free",Json(true));
    r=t.Run("import_to_spec",{{"source","live"}});
    Check(!r.isError && r.structured.at("format")=="map-authoring" && r.structured.at("source")=="live" && t.fake.Count("blob.free")==1,"live import: "+Text(r));
    Check(r.structured.at("spec").contains("existing"),"refs mode lists the existing actors");
    r=t.Run("import_to_spec",{{"source","live"},{"format","grid"}});
    Check(r.isError && Text(r).find("leave format auto")!=std::string::npos,"live is map-authoring only");
    r=t.Run("import_to_spec",{{"source","plans/x.txt"}});
    Check(r.isError && Text(r).find("full file path")!=std::string::npos,"relative sources refused");
    r=t.Run("import_to_spec",{{"source",(dir/"grid-basic.txt").string()},{"format","nonsense"}},false);
    Check(r.isError,"unknown formats refused");
}

void AssetsImport()
{
    Harness t;
    const auto dir=Fixtures();
    t.fake.On("editor.state",Json{{"open",true},{"mapFile","C:\\Games\\SCCT\\Packages\\MapsEd\\AquaD.sdc"},{"level",1}});
    t.fake.On("asset.texture",[](const Json& a,const CallOptions&){return Json{{"texture",a.at("package").get<std::string>()+"."+a.at("name").get<std::string>()},{"package",a.at("package")}};});
    t.fake.On("asset.mesh",[](const Json& a,const CallOptions&){return Json{{"mesh",a.at("package").get<std::string>()+"."+a.at("name").get<std::string>()}};});
    t.fake.On("package.save",[](const Json& a,const CallOptions&){return Json{{"package",a.at("package")},{"file","C:\\Games\\SCCT\\Packages\\Textures\\"+a.at("package").get<std::string>()+".utx"}};});
    Image::Rgba square(8,8,{10,200,30,255});
    Png(t.scratch.dir/"grass.png",square);
    Media::WriteFile(t.scratch.dir/"Brick.tga",Image::EncodeTga(Image::Rgba(16,16,{120,40,30,255})));
    fs::copy_file(dir/"crate.obj",t.scratch.dir/"crate.obj");
    Media::WriteFile(t.scratch.dir/"crate.mtl","newmtl Wood\nmap_Kd wood.tga\n");
    const Json items=Json::array({{{"kind","texture"},{"file",(t.scratch.dir/"Brick.tga").string()},{"group","Walls"}},
                                  {{"kind","texture"},{"file",(t.scratch.dir/"grass.png").string()},{"name","Grass"},{"alpha",true}},
                                  {{"kind","staticmesh"},{"file",(t.scratch.dir/"crate.obj").string()},{"scale",64}}});
    // A dry run checks and converts nothing.
    auto r=t.Run("assets_import",{{"items",items},{"dryRun",true}});
    Check(!r.isError && r.structured.at("plan").size()==3 && t.fake.Count("asset.texture")==0 && t.fake.Count("asset.mesh")==0,"dry run: "+Text(r));
    Check(r.structured.at("undo").is_null() && Text(r).find("nothing was changed")!=std::string::npos,"a dry run changes nothing");
    Check(!fs::exists(t.work/"imports") || fs::is_empty(t.work/"imports") || [&]{for(const auto& e:fs::recursive_directory_iterator(t.work/"imports"))if(e.is_regular_file())return false;return true;}(),"a dry run writes no files");
    // The real thing.
    r=t.Run("assets_import",{{"items",items},{"label","Props"}});
    Check(!r.isError,"imported: "+Text(r));
    const auto ops=t.fake.Ops();
    Check(ops==std::vector<std::string>({"editor.state","editor.state","asset.texture","asset.texture","asset.mesh","package.save","package.save"}),"calls in order: "+Json(ops).dump());
    const auto* brick=&t.fake.calls[2];
    Check(brick->args.at("package")=="AquaD_TXT" && brick->args.at("group")=="Walls" && brick->args.at("name")=="Brick" && brick->args.at("format")=="DXT1" && brick->args.at("mips")==true,"a TGA as it is: "+brick->args.dump());
    Check(brick->args.at("file")==(t.scratch.dir/"Brick.tga").string() && brick->label=="Props","the TGA's own path and the label");
    const auto& grass=t.fake.calls[3].args;
    Check(grass.at("format")=="DXT5" && grass.at("alphaTexture")==true && grass.at("name")=="Grass","alpha means DXT5");
    const fs::path grassFile=grass.at("file").get<std::string>();
    Check(Media::Inside(grassFile,t.work/"imports") && grassFile.extension()==".tga" && Image::DecodeTga(Media::ReadFile(grassFile)).width==8,"the PNG became a TGA in the work folder");
    const auto& mesh=t.fake.calls[4].args;
    const fs::path aseFile=mesh.at("file").get<std::string>();
    Check(mesh.at("package")=="AquaD_STM" && mesh.at("name")=="crate" && aseFile.extension()==".ase","the OBJ became an ASE: "+mesh.dump());
    const auto aseText=Media::ReadFile(aseFile);
    Check(aseText.find("*3DSMAX_ASCIIEXPORT")==0 && aseText.find("*BITMAP \"wood.tga\"")!=std::string::npos,"the material library was read");
    Check(t.fake.calls[5].args==Json({{"package","AquaD_TXT"},{"overwrite",false}}) && t.fake.calls[6].args.at("package")=="AquaD_STM","each package saved once");
    const auto& s=r.structured;
    Check(s.at("created").size()==3 && s.at("created").at(2).at("mesh")=="StaticMesh'AquaD_STM.crate'" && s.at("created").at(0).at("texture")=="AquaD_TXT.Brick","created assets: "+s.at("created").dump());
    Check(s.at("created").at(2).at("source").at("mesh").at("size")==Json({128.0,128.0,128.0}),"the mesh's size in Unreal units");
    Check(s.at("changed").size()==2 && s.at("changed").at(0).at("saved")==true && s.at("next").at(0).at("tool")=="level_build","packages and next steps");
    // Package saves that need overwrite say so.
    t.fake.Clear("package.save");
    t.fake.Fail("package.save",Bridge::Code::Refused,"C:\\Games\\SCCT\\Packages\\Textures\\AquaD_TXT.utx exists.","exists");
    r=t.Run("assets_import",{{"items",Json::array({items[0]})}});
    Check(!r.isError && r.structured.at("changed").at(0).at("saved")==false && Text(r).find("overwrite: true")!=std::string::npos,"an existing package needs overwrite: "+Text(r));
    r=t.Run("assets_import",{{"items",Json::array({items[0]})},{"save",false}});
    Check(!r.isError && Text(r).find("only in memory")!=std::string::npos,"unsaved packages are flagged");
    // Failures.
    t.fake.Clear("asset.texture");
    t.fake.Fail("asset.texture",Bridge::Code::EditorError,"Importing over an existing texture is rejected.");
    r=t.Run("assets_import",{{"items",Json::array({items[0]})}});
    Check(r.isError && Text(r).find("existing texture")!=std::string::npos,"nothing imported is an error");
    auto refused=[&](const Json& item,const std::string& expected)
    {
        const auto before=t.fake.calls.size();
        auto result=t.Run("assets_import",{{"items",Json::array({item})}});
        bool imported=false;for(size_t i=before;i<t.fake.calls.size();++i)imported=imported||t.fake.calls[i].op.rfind("asset.",0)==0;
        Check(result.isError && Text(result).find(expected)!=std::string::npos && !imported,"refused ("+expected+"): "+Text(result));
    };
    Png(t.scratch.dir/"odd.png",Image::Rgba(12,8));
    refused({{"kind","texture"},{"file",(t.scratch.dir/"odd.png").string()}},"image_convert");
    Media::WriteFile(t.scratch.dir/"odd.tga",Image::EncodeTga(Image::Rgba(12,8)));
    refused({{"kind","texture"},{"file",(t.scratch.dir/"odd.tga").string()}},"powers of two");
    refused({{"kind","texture"},{"file",(t.scratch.dir/"Brick.tga").string()},{"package","Engine"}},"map's own packages");
    refused({{"kind","texture"},{"file",(t.scratch.dir/"Brick.tga").string()},{"package","AquaD"}},"map's own packages");
    refused({{"kind","texture"},{"file",(t.scratch.dir/"Brick.tga").string()},{"name","Bad Name"}},"letters, digits and underscores");
    refused({{"kind","texture"},{"file",(t.scratch.dir/"Brick.tga").string()},{"scale",2}},"staticmesh items");
    refused({{"kind","staticmesh"},{"file",(t.scratch.dir/"Brick.tga").string()}},"OBJ or ASE");
    refused({{"kind","staticmesh"},{"file",(t.scratch.dir/"crate.obj").string()},{"mips",true}},"texture items");
    refused({{"kind","texture"},{"file","relative.tga"}},"full path");
    // Meshes and textures in one package.
    auto r2=t.Run("assets_import",{{"items",Json::array({{{"kind","texture"},{"file",(t.scratch.dir/"Brick.tga").string()},{"package","AquaD_Mix"}},
                                                          {{"kind","staticmesh"},{"file",(t.scratch.dir/"crate.obj").string()},{"package","AquaD_Mix"}}})}});
    Check(r2.isError && Text(r2).find("separate packages")!=std::string::npos,"one package, one kind");
    // A map without a file.
    t.fake.Clear("editor.state");
    t.fake.On("editor.state",Json{{"open",true},{"mapFile",""}});
    refused(items[0],"Save the map first");
    t.fake.Clear("editor.state");
    t.fake.On("editor.state",Json{{"open",false}});
    refused(items[0],"No map is open");
}

int main()
{
    try
    {
        const auto ffmpeg=Media::FindFfmpeg({});
        PureFrames();
        PureImages();
        ObjToAse();
        Decoding();
        Processes(ffmpeg);
        MediaTools(ffmpeg);
        ImportTools();
        AssetsImport();
        std::cout<<"McpMediaTools passed"<<(ffmpeg.empty()?" (without ffmpeg)":" (with ffmpeg)")<<"\n";
        return 0;
    }
    catch(const std::exception& e)
    {
        std::cerr<<"McpMediaTools FAILED: "<<e.what()<<"\n";
        return 1;
    }
}
