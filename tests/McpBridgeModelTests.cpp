#include "../Reloaded.Editor/McpBridgeModel.h"
#include "../Reloaded.Editor/McpGuardModel.h"
#include <iostream>
#include <set>
#include <source_location>
using namespace Mcp;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void Reject(F f,std::source_location where=std::source_location::current())
{
    bool caught=false;
    try{f();}catch(const std::exception&){caught=true;}
    if(!caught)throw std::runtime_error("invalid input accepted at line "+std::to_string(where.line()));
}
void Frames()
{
    Bridge::FrameReader reader;std::string out;
    const auto a=Bridge::EncodeFrame("{\"t\":\"hello\"}"),b=Bridge::EncodeFrame("[1,2]");
    Check(a.size()==4+13 && a[0]==13 && a[1]==0,"length prefix is little-endian");
    const auto both=a+b;
    // Byte by byte: a frame only comes out whole.
    for(size_t i=0;i<both.size();++i)
    {
        reader.Append(both.data()+i,1);
        if(i+1<a.size())Check(!reader.Next(out),"partial frame withheld");
    }
    Check(reader.Next(out) && out=="{\"t\":\"hello\"}","first frame");
    Check(reader.Next(out) && out=="[1,2]","second frame");
    Check(!reader.Next(out) && reader.Pending()==0,"nothing left");
    Bridge::FrameReader big;const char huge[4]={'\xff','\xff','\xff','\x7f'};big.Append(huge,4);
    Reject([&]{big.Next(out);});
    Reject([]{Bridge::EncodeFrame(std::string(Bridge::kMaxFrame+1,' '));});
    std::string deep(70,'[');deep+=std::string(70,']');
    Reject([&]{Bridge::Parse(deep);});
    Check(Bridge::Parse("{\"a\":[1]}").at("a")[0]==1,"parse");
}
void Policies()
{
    std::set<std::string> names;
    for(const auto& p:Bridge::OpPolicies())Check(names.insert(p.name).second,"op listed twice");
    Check(Bridge::FindPolicy("authoring.apply")->policy==Bridge::Policy::Undoable,"apply undoable");
    Check(Bridge::FindPolicy("light.set")->policy==Bridge::Policy::Blocked,"light.set blocked");
    Check(Bridge::FindPolicy("exec")->policy==Bridge::Policy::Raw,"exec raw");
    Check(!Bridge::FindPolicy("no.such.op"),"unknown op");
    Check(!Bridge::FindPolicy("surface.texture")->joinable,"surface texturing keeps its own step");
    for(const auto& op:Bridge::OpList())Check(op.at("policy")!="blocked","blocked ops are not advertised");
    Check(!Bridge::Mutates(Bridge::Policy::Read) && Bridge::Mutates(Bridge::Policy::File),"mutates");
}
void Messages()
{
    auto hello=Bridge::Hello("Reloaded.Editor.Mcp","2.8.0");std::string why;
    auto reply=Bridge::HelloReply("2.8.0",42,true,false,Bridge::OpList());
    Check(Bridge::Compatible(reply,why),"same major");
    reply["protocol"]["major"]=2;
    Check(!Bridge::Compatible(reply,why) && why.find("protocol 2")!=std::string::npos,"major mismatch explained");
    Check(!Bridge::Compatible(Workflow::Json::object(),why),"not a hello");
    Bridge::Expect expect;expect.set=true;expect.level=7;expect.generation=2;expect.revision=9;
    auto call=Bridge::CallMessage(5,"map.save",{{"path","X"}},"MCP 5: save",true,expect);
    auto read=Bridge::ReadExpect(call);
    Check(read.set && read.level==7 && read.generation==2 && read.revision==9,"expect round trip");
    Check(call.at("dryRun")==true && call.at("label")=="MCP 5: save","call fields");
    Check(!Bridge::ReadExpect(Bridge::CallMessage(1,"map",nullptr)).set,"no expect");
    auto error=Bridge::ErrorMessage(5,Bridge::ErrorBody(Bridge::Code::Busy,"wait","modal"));
    Check(error.at("ok")==false && error.at("error").at("retryable")==true && error.at("error").at("reason")=="modal","busy retryable");
    Check(Bridge::ErrorBody(Bridge::Code::Stale,"x").at("retryable")==false,"stale not retryable");
    auto result=Bridge::ResultMessage(5,true,{{"revision",3}},{{"log",Workflow::Json::array()}});
    Check(result.at("state").at("revision")==3 && result.contains("log"),"result extras");
}
void Helpers()
{
    Check(Bridge::EngineSafePath("C:\\Games\\SCCT\\Packages\\MapsEd\\AquaD.sdc"),"plain path");
    Check(!Bridge::EngineSafePath("C:\\a\"b") && !Bridge::EngineSafePath("C:\\caf\xc3\xa9") && !Bridge::EngineSafePath(""),"unsafe paths");
    Check(Bridge::MapOwnedPackage("AquaD_TXT","AquaD") && Bridge::MapOwnedPackage("aquad-i","AquaD") && Bridge::MapOwnedPackage("Amb_AquaD","AquaD"),"map packages");
    Check(!Bridge::MapOwnedPackage("Bunker","AquaD") && !Bridge::MapOwnedPackage("AquaDX","AquaD") && !Bridge::MapOwnedPackage("x",""),"stock packages");
    Check(Bridge::HistoryName("20261017-101500",12,"level build/x")=="20261017-101500-12-level_build_x.json","history name");
    for(const std::string& text:std::vector<std::string>{"","f","fo","foo","foob","fooba","foobar",std::string("\0\xff\x10",3)})
        Check(Bridge::Unbase64(Bridge::Base64(text))==text,"base64 round trip");
    Check(Bridge::Base64("foobar")=="Zm9vYmFy" && Bridge::Base64("fo")=="Zm8=","base64 vectors");
    Reject([]{Bridge::Unbase64("ab*c");});
}
void Guards()
{
    const auto& read=*Bridge::FindPolicy("actors.query");const auto& write=*Bridge::FindPolicy("authoring.apply");
    Guard::Inputs in;
    Check(Guard::BusyReason(in,write).empty(),"idle editor takes calls");
    in.captured=true;Check(Guard::BusyReason(in,read).empty() && Guard::BusyReason(in,write)=="drag","drag blocks writes only");
    in={};in.frameDisabled=true;Check(Guard::BusyReason(in,read)=="modal","modal blocks reads");
    in={};in.depth=1;in.approvalPending=true;Check(Guard::BusyReason(in,read)=="approval_pending","re-entry during review");
    in={};in.minimised=true;Check(Guard::BusyReason(in,read,true)=="minimised" && Guard::BusyReason(in,read).empty(),"minimised only for screenshots");
    in={};in.startupDialog="Update available";Check(Guard::BusyReason(in,read)=="startup_dialog","startup window");
    Check(!Guard::BusyHint("modal").empty(),"hint");
}
int main()
{
    try{Frames();Policies();Messages();Helpers();Guards();}
    catch(const std::exception& e){std::cerr<<"MCP bridge model tests failed: "<<e.what()<<"\n";return 1;}
    std::cout<<"MCP bridge model tests passed\n";
}
