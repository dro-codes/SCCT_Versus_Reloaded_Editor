// Every op the editor answers has a policy row in McpBridgeModel.h (the bridge
// refuses an op without one), and every row names an op the editor answers.
// Reads the DLL's sources as text: op=="name" in the request chains of
// WorkflowTools.cpp, and Add("name", ...) in the MCP op registries. Run from
// the repository root, or pass it as the first argument.
//
// Also what the bridge refuses before an op writes (McpSafetyModel.h): stock
// packages in Map JSON texture and save operations and OBJ imports, folders outside the
// write roots, raw commands that write files, and the dialog watchdog's
// button; and that no DLL source uses std::min/std::max where windows.h's
// min/max macros are live, which only MSVC's windows.h defines.
#include "../Reloaded.Editor/McpBridgeModel.h"
#include "../Reloaded.Editor/McpSafetyModel.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <vector>
namespace fs = std::filesystem;
std::string ReadText(const fs::path& file)
{
    std::ifstream in(file,std::ios::binary);
    if(!in)throw std::runtime_error("Cannot read "+file.string());
    std::ostringstream text;text<<in.rdbuf();return text.str();
}
// The op names a source answers: op=="x" in a request chain, and Add("x",
// Read("x", Write("x", in a registry (McpLifecycle.cpp wraps Add in Read and
// Write). A comparison on a member (step.op=="x", step->op=="x") reads a
// plan, so it is not one; the bridge's own call->op=="x" answers are listed
// in its source with an "// answers:" comment instead.
std::set<std::string> OpsIn(const std::string& text)
{
    static const std::regex pattern(R"re((?:^|[^A-Za-z0-9_.>])(?:op\s*==\s*|(?:Add|Read|Write)\s*\(\s*)"([A-Za-z][A-Za-z0-9_.]*)")re");
    static const std::regex answers(R"re(//\s*answers:\s*([A-Za-z][A-Za-z0-9_.]*))re");
    std::set<std::string> ops;
    for(std::sregex_iterator it(text.begin(),text.end(),pattern),end;it!=end;++it)ops.insert((*it)[1].str());
    for(std::sregex_iterator it(text.begin(),text.end(),answers),end;it!=end;++it)ops.insert((*it)[1].str());
    return ops;
}
void Check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
void Patterns()
{
    const auto ops=OpsIn("else if(op==\"design.block\")x;\nif(op == \"map\")y;\nAdd(\"editor.state\",f);\nWrite(\"map.save\",g);\nRead(\"view.screenshot\",h);\nif(step.op==\"stage.apply\")z;\nif(step->op==\"select\")w;\nAppendMenuAdd(\"no\");\nif(c->op==\"x\")v; // answers: approval.status");
    Check(ops==std::set<std::string>({"design.block","map","editor.state","map.save","view.screenshot","approval.status"}),"op patterns");
}
namespace Safety=Mcp::Safety;
using Json=Workflow::Json;

// The watchdog's answer to a question it does not know: by id, as Windows
// translates a message box's labels; never the first button.
void Buttons()
{
    namespace Id=Safety::Id;
    using List=std::vector<Safety::Button>;
    auto pick=[](const List& buttons){return Safety::CautiousButton(buttons);};
    Check(pick(List{{Id::Yes,L"&Ja"},{Id::No,L"&Nein"}})==1,"a German Yes/No box is answered No, not its first button");
    Check(pick(List{{Id::Ok,L"OK"},{Id::Cancel,L"Annuler"}})==1,"a French OK/Cancel box is answered Cancel, not by its OK label");
    Check(pick(List{{Id::Yes,L"&Ja"},{Id::No,L"&Nein"},{Id::Cancel,L"Abbrechen"}})==2,"Yes/No/Cancel: Cancel");
    Check(pick(List{{Id::Abort,L"&Abbrechen"},{Id::Retry,L"&Wiederholen"},{Id::Ignore,L"&Ignorieren"}})==0,"Abort/Retry/Ignore: Abort");
    Check(pick(List{{Id::Retry,L"&Wiederholen"},{Id::Cancel,L"Abbrechen"}})==1,"Retry/Cancel: Cancel");
    Check(pick(List{{Id::Close,L"Schlie\u00dfen"},{1001,L"Speichern"}})==0,"Close by its id");
    Check(pick(List{{Id::Ok,L"OK"}})==0,"a notice's only OK is pressed");
    Check(pick(List{{1001,L"&OK"}})==0,"an only button labelled OK is pressed");
    Check(pick(List{{1001,L"&Delete"},{1002,L"&No"}})==1,"a dialog with ids of its own: the English No");
    Check(pick(List{{1001,L"Replace"},{1002,L" &Cancel "}})==1,"a dialog with ids of its own: the English Cancel");
    Check(pick(List{{1001,L"L\u00f6schen"},{1002,L"Weiter"}})==-1,"nothing safe to press: none, not the first");
    Check(pick(List{{Id::Ok,L"OK"},{1001,L"Details"}})==-1,"OK is pressed only when it is the only button");
    Check(pick(List{{Id::Yes,L"&Ja"}})==-1,"an only Yes is not pressed");
    Check(pick(List{})==-1,"no buttons");
    Check(Safety::PlainLabel(L" &Cancel ")==L"cancel","plain labels");
}

// Map JSON texture and save operations over the bridge: the map's own
// packages only (the stock-package save of the review's scenario).
void Packages()
{
    const std::set<std::string> files{"StaticMeshes/NEWD_STM.usx","Textures/Stock_Tex.utx"};
    const Safety::Existing existing=[&](const std::string& folder,const std::string& file){return files.count(folder+"/"+file)?folder+"/"+file:std::string();};
    auto reason=[&](const std::string& package,const Safety::PackageKind& kind){return Safety::PackageRefusal(package,"NEWD",kind,existing).reason;};
    Check(reason("NEWD_TXT",Safety::kTextures).empty() && reason("newd_Walls",Safety::kTextures).empty(),"the map's own texture packages");
    Check(reason("Stock_Tex",Safety::kTextures)=="not_map_owned","a stock package is refused");
    Check(reason("NEWD-i",Safety::kTextures)=="interface_package","the interface package is interface.texture's");
    Check(reason("NEWD",Safety::kTextures)=="package_name" && reason("Amb_NEWD",Safety::kTextures)=="package_name","the map's and its sounds' names are refused");
    Check(reason("NEWD_STM",Safety::kTextures)=="package_name","a texture package may not share a static mesh package's base name");
    Check(reason("NEWD_STM",Safety::kMeshes).empty(),"the static mesh package itself");
    Check(Safety::PackageRefusal("NEWD TXT","NEWD",Safety::kTextures,existing).code==Mcp::Bridge::Code::Invalid,"a name the console cannot read");
    auto document=[](const Json& operations){return Json{{"format","scct.map-changes"},{"version",2},{"map","*"},{"operations",operations}};};
    auto op=[](const char* kind,const char* package){return Json{{"op",kind},{"package",package}};};
    const auto stock=document(Json::array({op("load","Stock_Tex"),op("texture","Stock_Tex"),Json{{"op","save"},{"package","Stock_Tex"},{"overwrite",true}}}));
    Check(Safety::PackageWrites(stock).size()==2,"a document's package writes: its texture and save, not its load");
    const auto refused=Safety::DocumentRefusal(stock,"NEWD",existing);
    Check(refused && refused.code==Mcp::Bridge::Code::Refused && refused.reason=="not_map_owned" && refused.message.find("Stock_Tex")!=std::string::npos,"re-saving a stock package is refused");
    Check(!Safety::DocumentRefusal(document(Json::array({op("load","Stock_Tex")})),"NEWD",existing),"a load of a stock package stays allowed");
    Check(!Safety::DocumentRefusal(document(Json::array({op("texture","NEWD_TXT"),op("save","NEWD_TXT")})),"NEWD",existing),"the map's own package is imported into and saved");
    Check(Safety::DocumentRefusal(document(Json::array({op("load","NEWD_STM"),op("save","NEWD_STM")})),"NEWD",existing).reason=="package_name",
          "a save would make Textures\\NEWD_STM.utx beside the static mesh package");
    Check(Safety::DocumentRefusal(document(Json::array({op("texture","NEWD_TXT")})),"",existing).reason=="unsaved_map","an unsaved map has no packages of its own yet");
    Check(!Safety::DocumentRefusal(document(Json::array({op("texture","MyLevel")})),"",existing),"a texture inside the map itself");
    Check(Safety::DocumentRefusal(document(Json::array({op("texture","MyLevel"),op("save","MyLevel")})),"NEWD",existing).reason=="not_map_owned","the map's package is not saved as a texture package");
    // objimport.import (File > Import OBJ Mesh) writes its mesh and textures into one package.
    auto obj=[&](const Json& args,const std::string& stem){return Safety::ObjImportRefusal(args,stem,existing);};
    const auto stockObj=obj(Json{{"file","C:/in/crate.obj"},{"package","SCCT_Generic"},{"place",true}},"NEWD");
    Check(stockObj && stockObj.code==Mcp::Bridge::Code::Refused && stockObj.reason=="not_map_owned" && stockObj.message.find("SCCT_Generic")!=std::string::npos,"an OBJ import into a stock package is refused");
    Check(obj(Json{{"package","Engine"}},"NEWD").reason=="not_map_owned","an OBJ import into Engine is refused");
    Check(!obj(Json{{"package","NEWD_STM"}},"NEWD") && !obj(Json{{"package","newd_Props"}},"NEWD"),"an OBJ import into the map's own mesh package");
    Check(!obj(Json{{"file","C:/in/crate.obj"}},"") && !obj(Json{{"package","mylevel"}},""),"an OBJ import into the map itself (the default), even before its first save");
    Check(obj(Json{{"package","NEWD_STM"}},"").reason=="unsaved_map","the map's own package needs its first save");
    Check(obj(Json{{"package","Stock_Tex"}},"NEWD").reason=="not_map_owned" && obj(Json{{"package","NEWD"}},"NEWD").reason=="package_name","stock and clashing names");
    Check(obj(Json{{"package",3}},"NEWD").code==Mcp::Bridge::Code::Invalid && obj(Json{{"package","NEWD STM"}},"NEWD").code==Mcp::Bridge::Code::Invalid,"a package that is not a console name");
}

// The release folder and raw commands: only inside the write roots.
void Folders()
{
    Check(Safety::Inside("/game/Releases/NEWD","/game/Releases") && Safety::Inside("/game/Releases","/game/Releases/"),"inside a root, or the root");
    Check(!Safety::Inside("/game/Releases2/NEWD","/game/Releases") && !Safety::Inside("/game","/game/Releases"),"a sibling or a parent is outside");
    Check(!Safety::Inside("/game/Releases/../System","/game/Releases"),"..");
    Check(Safety::Inside("/Game/RELEASES/x","/game/releases"),"any case, as Windows compares");
    Check(Safety::Inside("C:\\Games\\SCCT\\Releases\\NEWD","C:/Games/SCCT/Releases"),"either slash");
    Check(!Safety::Inside("/game/x",""),"an empty root holds nothing");
    Check(Safety::SplitRoots(" \"D:\\One\" ;; E:\\Two ")==std::vector<std::string>({"D:\\One","E:\\Two"}) && Safety::SplitRoots("").empty(),"WriteRoots as the server reads it");
}

// exec's refusals: commands that write files (MAP EXPORT, SAVEMAPPROP),
// run a file of commands (EXEC), and any FILE= or PACKAGE= the bridge
// would not write itself.
void RawCommands()
{
    Safety::RawContext context;
    context.mapStem="NEWD";context.base="/game/System";context.roots={"/work","/extra/out"};
    auto reason=[&](const std::string& command){return Safety::RawCommandRefusal(command,context).reason;};
    auto code=[&](const std::string& command){return Safety::RawCommandRefusal(command,context).code;};
    Check(!Safety::RawCommandRefusal("STAT FPS",context) && !Safety::RawCommandRefusal("ACTOR SELECT ALL",context),"harmless commands run");
    Check(reason("SAVEMAPPROP MAP=\"StockMap\"")=="denied_command","SAVEMAPPROP writes any map's interface package");
    Check(reason("map  export file=\"C:\\Users\\x\\Desktop\\important.txt\"")=="denied_command","MAP EXPORT writes any file");
    Check(reason("  Exec commands.txt")=="denied_command","EXEC runs a file of commands");
    for(const std::string denied:Safety::DeniedCommands())
    {
        Check(reason(denied)=="denied_command" && reason(Safety::Fold(denied)+" X=1")=="denied_command","denied: "+denied);
        std::string spaced;for(char c:denied)spaced+=c==' '?std::string("\t  "):std::string(1,c);
        Check(reason(spaced)=="denied_command","denied with other spacing: "+denied);
    }
    Check(reason("MAPEXPORT")!="denied_command" && reason("STAT AUDIOX")!="denied_command","whole words only");
    // The engine's ParseCommand ends a word at anything but a letter or
    // digit: QUIT; quits, EXEC"x" runs x, SAVEMAPPROP;MAP=X saves X's -i.utc.
    Check(reason("QUIT;")=="denied_command" && reason("EXEC\"x\"")=="denied_command" && reason("SAVEMAPPROP;MAP=X")=="denied_command","a word ends at punctuation");
    Check(reason("SAVEMAPPROP;MAP=\"StockMap\"")=="denied_command" && reason("SAVEMAPPROP,MAP=StockMap")=="denied_command" && reason("Exec\"cmds.txt\"")=="denied_command" &&
          reason("MAP NEW;")=="denied_command" && reason("LOADMAPPROP;MAP=Stock")=="denied_command" && reason("DUMPINT;Engine")=="denied_command" &&
          reason("RELAUNCH\"\"")=="denied_command" && reason("AUDIO;")=="denied_command" && reason("map\tload\"x.sdc\"")=="denied_command","the review's punctuation cases");
    for(const std::string denied:Safety::DeniedCommands())
        for(const char* after:{";","\"x\"",",","(","=1",".","/","-","_","\xc3\xa9"})
            Check(reason(denied+after)=="denied_command" && reason(" "+Safety::Fold(denied)+after+" X=1")=="denied_command","denied before "+std::string(after)+": "+denied);
    Check(reason("QUIT2")!="denied_command" && reason("EXECUTE x")!="denied_command" && reason("MAP;NEW")!="denied_command" && reason("STAT;QUIT")!="denied_command",
          "a word goes on through letters and digits, and the second word of a phrase is read after the first's blanks only");
    Check(!Safety::RawCommandRefusal("OBJ CLASSES FILE=/work/classes.txt",context) && !Safety::RawCommandRefusal("OBJ CLASSES FILE=\"/extra/out/a b.txt\"",context),"files in the write roots");
    Check(reason("OBJ CLASSES FILE=/etc/passwd")=="outside_write_roots","a file elsewhere");
    Check(reason("OBJ CLASSES FILE=/work/../etc/passwd")=="outside_write_roots","out of a root by ..");
    Check(reason("OBJ CLASSES FILE=classes.txt")=="outside_write_roots","relative to System");
    Check(reason("OBJ CLASSES FILE=")=="outside_write_roots","an empty file");
    Check(reason("XYZ PROFILE=\"/tmp/x\"")=="outside_write_roots","the engine finds FILE= inside any word");
    Check(reason("OBJ CLASSES FILE=/work/a.txt FILE=/etc/b")=="outside_write_roots","every FILE=");
    Check(reason("NEW TEXTURE PACKAGE=Stock_Props NAME=X")=="not_map_owned" && reason("NEW TEXTURE PACKAGE=NEWD NAME=X")=="not_map_owned","stock packages and the map's file name");
    Check(!Safety::RawCommandRefusal("NEW TEXTURE PACKAGE=NEWD_TXT NAME=X",context) && !Safety::RawCommandRefusal("NEW TEXTURE package=\"MyLevel\" NAME=X",context),"the map's own packages");
    auto unsaved=context;unsaved.mapStem.clear();
    Check(Safety::RawCommandRefusal("NEW TEXTURE PACKAGE=NEWD_TXT",unsaved).reason=="not_map_owned","an unsaved map owns no package yet");
    Check(code("STAT FPS\nQUIT")==Mcp::Bridge::Code::Invalid && code("STAT FPS|QUIT")==Mcp::Bridge::Code::Invalid && code("")==Mcp::Bridge::Code::Invalid,"one command, one line");
    Check(Safety::ParameterValues("A FILE=\"x y\" file=z,w File=",std::string("FILE="))==std::vector<std::string>({"x y","z",""}),"FILE= values as the engine reads them");
}

// windows.h's min(a,b) and max(a,b) macros are live after pch.h in every DLL
// file until it #undefs them (MSVC's windows.h; MinGW's libstdc++ defines
// NOMINMAX, so mgw.sh never sees them). There std::min(...) and
// numeric_limits<T>::max() do not compile: LevelSnapshot.cpp included
// MinimapModel.h before its #undef once. (std::min)(...) is safe.
bool Identifier(char c){return std::isalnum(static_cast<unsigned char>(c)) || c=='_';}
std::string Code(const std::string& text)
{
    // Comments and string literals out (#include lines keep their names).
    std::string out;out.reserve(text.size());
    enum class S{Code,Line,Block,String,Char} state=S::Code;
    bool include=false,start=true;
    for(size_t i=0;i<text.size();++i)
    {
        const char c=text[i],next=i+1<text.size()?text[i+1]:'\0';
        if(c=='\n'){if(state==S::Line)state=S::Code;out+=c;include=false;start=true;continue;}
        if(state==S::Code && start && c!=' ' && c!='\t')
        {
            start=false;
            if(c=='#'){const auto word=text.find_first_not_of(" \t",i+1);include=word!=std::string::npos && text.compare(word,7,"include")==0;}
        }
        switch(state)
        {
        case S::Code:
            if(c=='/' && next=='/'){state=S::Line;++i;}
            else if(c=='/' && next=='*'){state=S::Block;++i;out+=' ';}
            else if(c=='R' && next=='"' && (i==0 || !Identifier(text[i-1]) || text[i-1]=='8' || text[i-1]=='L' || text[i-1]=='u' || text[i-1]=='U'))
            {
                // R"delimiter( ... )delimiter"
                const auto paren=text.find('(',i+2);
                if(paren==std::string::npos){out+=c;break;}
                const auto end=text.find(")"+text.substr(i+2,paren-i-2)+"\"",paren);
                if(end==std::string::npos){out+=c;break;}
                for(size_t k=i;k<=end+paren-i-1 && k<text.size();++k)if(text[k]=='\n')out+='\n';
                out+="\"\"";i=end+paren-i-1;
            }
            else if(c=='"' && !include){state=S::String;out+=c;}
            else if(c=='\'' && !include && !(i>0 && std::isalnum(static_cast<unsigned char>(text[i-1])))){state=S::Char;out+=c;} // not a digit separator
            else out+=c;
            break;
        case S::Line: break;
        case S::Block: if(c=='*' && next=='/'){state=S::Code;++i;} break;
        case S::String: if(c=='\\')++i;else if(c=='"'){state=S::Code;out+=c;} break;
        case S::Char: if(c=='\\')++i;else if(c=='\''){state=S::Code;out+=c;} break;
        }
    }
    return out;
}
// ::min( or ::max( (std::max({...}) too), which the macros break.
bool Hazard(const std::string& line)
{
    for(size_t at=line.find("::");at!=std::string::npos;at=line.find("::",at+2))
    {
        size_t i=line.find_first_not_of(" \t",at+2);
        if(i==std::string::npos || (line.compare(i,3,"min")!=0 && line.compare(i,3,"max")!=0))continue;
        i+=3;
        if(i<line.size() && Identifier(line[i]))continue;
        i=line.find_first_not_of(" \t",i);
        if(i!=std::string::npos && line[i]=='(')return true;
    }
    return false;
}
std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> lines;std::istringstream in(text);
    for(std::string line;std::getline(in,line);)lines.push_back(line);
    return lines;
}
std::string Included(const std::string& line)
{
    const auto at=line.find_first_not_of(" \t");
    if(at==std::string::npos || line[at]!='#' || line.find("include",at)==std::string::npos)return {};
    const auto left=line.find('"',at),right=left==std::string::npos?left:line.find('"',left+1);
    return right==std::string::npos?std::string():line.substr(left+1,right-left-1);
}
// The first macro hazard in a local header and what it includes, as
// "Header.h:line" or "Header.h -> Other.h:line"; "" for none.
std::string HeaderHazard(const fs::path& folder,const std::string& name,std::set<std::string>& seen,std::map<std::string,std::string>& texts)
{
    if(!seen.insert(name).second)return {};
    const auto file=folder/name;
    if(!fs::exists(file))return {};
    auto found=texts.find(name);
    if(found==texts.end())found=texts.emplace(name,Code(ReadText(file))).first;
    const auto lines=Lines(found->second);
    for(size_t i=0;i<lines.size();++i)
    {
        if(const auto header=Included(lines[i]);!header.empty())
        {
            const auto inner=HeaderHazard(folder,header,seen,texts);
            if(!inner.empty())return name+" -> "+inner;
        }
        else if(Hazard(lines[i]))return name+":"+std::to_string(i+1);
    }
    return {};
}
void MinMaxMacros(const fs::path& root)
{
    const auto folder=root/"Reloaded.Editor";
    std::map<std::string,std::string> texts;std::vector<std::string> problems;size_t scanned=0;
    for(const auto& entry:fs::directory_iterator(folder))
    {
        if(entry.path().extension()!=".cpp")continue;
        const auto lines=Lines(Code(ReadText(entry.path())));
        size_t i=0;
        while(i<lines.size() && Included(lines[i])!="pch.h")++i;
        if(i==lines.size())continue;
        bool nominmax=false;
        for(size_t k=0;k<i;++k)if(lines[k].find("NOMINMAX")!=std::string::npos)nominmax=true;
        if(nominmax)continue;
        ++scanned;
        bool undefMin=false,undefMax=false;std::set<std::string> seen;
        for(++i;i<lines.size() && !(undefMin && undefMax);++i)
        {
            const auto& line=lines[i];
            const auto at=line.find_first_not_of(" \t");
            if(at!=std::string::npos && line[at]=='#' && line.find("undef")!=std::string::npos)
            {
                if(line.find(" min")!=std::string::npos || line.find("\tmin")!=std::string::npos)undefMin=true;
                if(line.find(" max")!=std::string::npos || line.find("\tmax")!=std::string::npos)undefMax=true;
                continue;
            }
            std::string hazard;
            if(const auto header=Included(line);!header.empty())hazard=HeaderHazard(folder,header,seen,texts);
            else if(Hazard(line))hazard="line "+std::to_string(i+1);
            if(!hazard.empty())
            {
                problems.push_back(entry.path().filename().string()+": "+hazard+" uses std::min/std::max while windows.h's min/max macros are live; add #undef min and #undef max after pch.h");
                break;
            }
        }
    }
    for(const auto& problem:problems)std::cerr<<problem<<"\n";
    Check(problems.empty(),std::to_string(problems.size())+" DLL source(s) break under MSVC's min/max macros");
    Check(scanned>10,"the DLL's sources were scanned ("+std::to_string(scanned)+")");
    // The scanner itself.
    Check(Hazard("x = std::min(a, b);") && Hazard("std::max({ a, b })") && Hazard("std::numeric_limits<int>::max ()"),"hazards found");
    Check(!Hazard("x = (std::min)(a, b);") && !Hazard("y = max(a, b);") && !Hazard("std::minmax(a, b)") && !Hazard("Foo::maxSize(1)"),"safe forms pass");
    Check(Code("a // std::min(x)\n\"std::max(y)\" /* ::min( */ b")=="a \n\"\"   b" && Included(Code("#include \"MinimapModel.h\" // x"))=="MinimapModel.h","comments and strings are skipped");
    Check(Code("x=R\"j(\"a\" std::min(\n)j\";y=1'000;c='\\'';z")=="x=\n\"\";y=1'000;c='';z","raw strings, digit separators and char literals");
}

// settings.edit writes a LevelInfo property the sheet does not offer only
// when McpNative.inl's McpSettingsExtra lists it: every name the level spec
// compiler's Settings() writes is there, and no sheet heading is.
void SettingsNames(const fs::path& root)
{
    const auto native=ReadText(root/"Reloaded.Editor"/"McpNative.inl");
    const auto at=native.find("bool McpSettingsExtra(");
    Check(at!=std::string::npos,"McpNative.inl has McpSettingsExtra");
    const auto left=native.find('{',native.find("extra:",at)),right=native.find('}',left);
    Check(left!=std::string::npos && right!=std::string::npos,"McpSettingsExtra's list");
    std::set<std::string> listed;
    const std::regex quoted(R"re("([A-Za-z0-9_]+)")re");
    const auto list=native.substr(left,right-left);
    for(std::sregex_iterator it(list.begin(),list.end(),quoted),end;it!=end;++it)listed.insert(Safety::Fold((*it)[1].str()));
    const auto compiler=ReadText(root/"Reloaded.Editor.Mcp"/"LevelSpecCompiler.h");
    const auto from=compiler.find("void Settings()"),to=compiler.find("low.settings",from);
    Check(from!=std::string::npos && to!=std::string::npos,"LevelSpecCompiler.h's Settings()");
    const auto body=compiler.substr(from,to-from);
    const std::regex written(R"re(changes\["([A-Za-z0-9_]+)"\])re");
    std::set<std::string> names;
    for(std::sregex_iterator it(body.begin(),body.end(),written),end;it!=end;++it)names.insert((*it)[1].str());
    Check(names.size()>=7,"the compiler's level settings were found ("+std::to_string(names.size())+")");
    for(const auto& name:names)Check(listed.count(Safety::Fold(name))>0,"settings.edit refuses "+name+", which the compiler writes: add it to McpSettingsExtra");
    for(const char* heading:{"MapSnapshotInfo","MotionTrackingTweak","Events"})
        Check(!listed.count(Safety::Fold(heading)),std::string(heading)+" is a heading of the Level Properties sheet, not a property");
    Check(listed.count("snapshotcamera") && listed.count("mapfloors"),"the minimap's SnapshotCamera and MapFloors");
}

int main(int argc,char** argv)
{
    try
    {
        Patterns();
        Buttons();
        Packages();
        Folders();
        RawCommands();
        fs::path root=argc>1?fs::path(argv[1]):fs::path(".");
        if(!fs::exists(root/"Reloaded.Editor"/"WorkflowTools.cpp") && fs::exists(root/".."/"Reloaded.Editor"/"WorkflowTools.cpp"))root=root/"..";
        MinMaxMacros(root);
        SettingsNames(root);
        // The request chains and the registries. McpLifecycle.cpp (map files,
        // builds, views, packages) and McpBridge.cpp (approval.status, which
        // only the bridge can answer) may not exist yet in a partial tree:
        // their ops are then reported as unanswered.
        struct Source { const char* file; bool required; };
        const Source sources[]={{"WorkflowTools.cpp",true},{"McpOps.cpp",true},{"McpLifecycle.cpp",false},{"McpBridge.cpp",false}};
        std::map<std::string,std::string> answered;
        for(const auto& source:sources)
        {
            const auto file=root/"Reloaded.Editor"/source.file;
            if(!fs::exists(file))
            {
                if(source.required)throw std::runtime_error("Missing "+file.string());
                std::cout<<"note: "<<source.file<<" is not there yet; its ops count as unanswered\n";
                continue;
            }
            for(const auto& op:OpsIn(ReadText(file)))answered.emplace(op,source.file);
        }
        std::set<std::string> rows;std::vector<std::string> problems;
        for(const auto& policy:Mcp::Bridge::OpPolicies())
            if(!rows.insert(policy.name).second)problems.push_back(std::string("two policy rows for ")+policy.name);
        for(const auto& [op,file]:answered)
            if(!rows.count(op))problems.push_back(op+" ("+file+") has no policy row in McpBridgeModel.h, so the bridge refuses it");
        // Rows the editor answers somewhere these files do not show: none.
        const std::set<std::string> elsewhere{};
        for(const auto& row:rows)
            if(!answered.count(row) && !elsewhere.count(row))problems.push_back(row+" has a policy row but no file answers it");
        if(!problems.empty())
        {
            for(const auto& problem:problems)std::cerr<<problem<<"\n";
            std::cerr<<problems.size()<<" problem(s)\n";
            return 1;
        }
        std::cout<<"McpOpCoverage passed ("<<answered.size()<<" ops, "<<rows.size()<<" policy rows, bridge refusals, min/max macros, level settings)\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
