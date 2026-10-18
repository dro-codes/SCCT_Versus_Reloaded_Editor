#include "../Reloaded.Editor.Mcp/McpProtocolModel.h"
#include <iostream>
#include <source_location>
using namespace Mcp;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void Reject(F f,std::source_location where=std::source_location::current())
{
    bool caught=false;
    try{f();}catch(const std::exception&){caught=true;}
    if(!caught)throw std::runtime_error("invalid input accepted at line "+std::to_string(where.line()));
}
void Versions()
{
    for(const auto& v:Protocol::Versions())Check(Protocol::Negotiate(v)==v,"supported versions are kept");
    Check(Protocol::Negotiate("2026-01-01")=="2025-06-18","an unknown version gets the fallback");
    Check(Protocol::Negotiate(Json(20250618))=="2025-06-18","a non-string version gets the fallback");
    Check(Protocol::Negotiate(Json())=="2025-06-18","no version gets the fallback");
    Check(Protocol::Structured("2025-06-18") && Protocol::Structured("2025-11-25") && !Protocol::Structured("2025-03-26"),"structured output from 2025-06-18");
    Check(Protocol::Annotated("2025-03-26") && !Protocol::Annotated("2024-11-05"),"annotations from 2025-03-26");
}
void Envelopes()
{
    auto m=Protocol::Classify(Json::parse(R"({"jsonrpc":"2.0","id":7,"method":"tools/list","params":{"cursor":null}})"));
    Check(m.kind==Protocol::Message::Kind::Request && m.id==7 && m.method=="tools/list","request");
    m=Protocol::Classify(Json::parse(R"({"jsonrpc":"2.0","id":"a","method":"ping"})"));
    Check(m.kind==Protocol::Message::Kind::Request && m.id=="a" && m.params.is_object(),"string id, no params");
    m=Protocol::Classify(Json::parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    Check(m.kind==Protocol::Message::Kind::Notification,"notification");
    m=Protocol::Classify(Json::parse(R"({"jsonrpc":"2.0","id":3,"result":{}})"));
    Check(m.kind==Protocol::Message::Kind::Response,"response");
    m=Protocol::Classify(Json::parse(R"({"id":3,"method":"ping"})"));
    Check(m.kind==Protocol::Message::Kind::Invalid && m.id==3,"jsonrpc is required; the id is kept for the error");
    m=Protocol::Classify(Json::parse(R"({"jsonrpc":2,"id":3,"method":"ping"})"));
    Check(m.kind==Protocol::Message::Kind::Invalid,"jsonrpc must be the string 2.0");
    m=Protocol::Classify(Json::parse(R"({"jsonrpc":"2.0","id":3,"method":"ping","params":[1]})"));
    Check(m.kind==Protocol::Message::Kind::Invalid,"params must be an object");
    m=Protocol::Classify(Json::parse(R"({"jsonrpc":"2.0","id":{"x":1},"method":"ping"})"));
    Check(m.kind==Protocol::Message::Kind::Invalid && m.id.is_null(),"an object id is refused");
    m=Protocol::Classify(Json::parse(R"({"jsonrpc":"2.0","id":3,"method":5})"));
    Check(m.kind==Protocol::Message::Kind::Invalid,"method must be a string");
    Check(Protocol::Classify(Json::array()).kind==Protocol::Message::Kind::Invalid,"not an object");
    const auto response=Protocol::Response(7,{{"ok",true}});
    Check(response.at("jsonrpc")=="2.0" && response.at("id")==7 && response.at("result").at("ok")==true,"response envelope");
    const auto error=Protocol::Error(nullptr,Protocol::ErrorCode::Parse,"bad",{{"at",3}});
    Check(error.at("id").is_null() && error.at("error").at("code")==-32700 && error.at("error").at("data").at("at")==3,"error envelope");
    Check(!Protocol::Error(1,-32601,"x").at("error").contains("data"),"no data unless given");
    const auto note=Protocol::Notification("notifications/progress",{{"progress",1}});
    Check(!note.contains("id") && note.at("method")=="notifications/progress","notification envelope");
    // Engine text is ANSI: invalid UTF-8 must not break a reply.
    const auto line=Protocol::Line({{"text",std::string("caf\xe9\nx")}});
    Check(line.find('\n')==std::string::npos && line.find("caf")!=std::string::npos,"one line, bad bytes replaced");
    std::string deep(200,'[');deep+=std::string(200,']');
    Reject([&]{Protocol::ParseLine(deep);});
    Check(Protocol::ParseLine("{\"a\":[[1]]}").at("a")[0][0]==1,"parse line");
}
Tool Sample()
{
    Tool t;t.name="map_query";t.title="Read the map";t.description="Reads.";
    t.inputSchema={{"type","object"},{"properties",{{"what",{{"type","string"}}}}}};
    t.outputSchema={{"type","object"}};t.readOnly=true;
    return t;
}
void Lists()
{
    const auto caps=Protocol::Capabilities();
    Check(caps.at("tools").at("listChanged")==true && caps.at("resources").at("subscribe")==true && caps.at("resources").at("listChanged")==true,"tool and resource capabilities");
    Check(caps.at("prompts").at("listChanged")==true && caps.at("logging").is_object(),"prompt and logging capabilities");
    auto init=Protocol::InitializeResult("2025-06-18","2.7.0","Use editor_status first.");
    Check(init.at("protocolVersion")=="2025-06-18" && init.at("serverInfo").at("name")=="scct-reloaded","initialize result");
    Check(init.at("serverInfo").at("title")=="SCCT Versus Reloaded Editor (RE+)" && init.at("instructions")=="Use editor_status first.","title and instructions");
    Check(!Protocol::InitializeResult("2024-11-05","2.7.0","").at("serverInfo").contains("title"),"no title for old clients");
    Check(!Protocol::InitializeResult("2024-11-05","2.7.0","").contains("instructions"),"no empty instructions");

    auto tool=Protocol::ToolJson(Sample(),"2025-06-18");
    Check(tool.at("name")=="map_query" && tool.at("title")=="Read the map" && tool.contains("outputSchema"),"tool for 2025-06-18");
    Check(tool.at("annotations").at("readOnlyHint")==true && tool.at("annotations").at("openWorldHint")==false && !tool.at("annotations").contains("destructiveHint"),"read-only annotations");
    Check(!tool.at("annotations").contains("idempotentHint") && !tool.at("annotations").contains("title"),"no hints that mean nothing here, the title once");
    Check(Protocol::ToolJson(Sample(),"2025-03-26").at("annotations").at("title")=="Read the map","before 2025-06-18 the title is an annotation");
    auto old=Protocol::ToolJson(Sample(),"2024-11-05");
    Check(!old.contains("title") && !old.contains("outputSchema") && !old.contains("annotations"),"old clients see the 2024 shape");
    auto writer=Sample();writer.readOnly=false;writer.destructive=true;writer.outputSchema=Json();
    auto written=Protocol::ToolJson(writer,"2025-03-26");
    Check(written.at("annotations").at("destructiveHint")==true && written.at("annotations").at("idempotentHint")==false && !written.contains("outputSchema"),"mutating annotations");

    ToolResult result=Ok({{"a",1}},"Done.");
    result.content.push_back(ImageContent("iVBO"));
    auto modern=Protocol::ToolResultJson(result,"2025-11-25");
    Check(modern.at("structuredContent").at("a")==1 && modern.at("isError")==false,"structured content");
    Check(modern.at("content")[0].at("type")=="text" && modern.at("content")[0].at("text").get<std::string>().find("Done.")==0,"text first");
    Check(modern.at("content")[1].at("type")=="image" && modern.at("content")[1].at("mimeType")=="image/png" && modern.at("content")[1].at("data")=="iVBO","image block");
    auto legacy=Protocol::ToolResultJson(result,"2025-03-26");
    Check(!legacy.contains("structuredContent") && legacy.at("content").size()==2,"no structured content before 2025-06-18");
    auto failed=Protocol::ToolResultJson(Fail("No editor.",{{"code","not_connected"}}),"2025-06-18");
    Check(failed.at("isError")==true && failed.at("structuredContent").at("error")=="No editor." && failed.at("structuredContent").at("code")=="not_connected","error result");
    // An error whose details do not fit the tool's outputSchema leaves
    // structuredContent out (clients validate it even with isError); the
    // text still carries the message and the details.
    const Json output=Json::parse(R"({"type":"object","required":["valid","summary"],"properties":{"valid":{"type":"boolean"},"summary":{"type":"object","required":["errors"]}}})");
    auto strict=Protocol::ToolResultJson(Fail("No map is open.",{{"code","editor_error"},{"hint","map_file"}}),"2025-06-18",output);
    Check(strict.at("isError")==true && !strict.contains("structuredContent") && strict.at("content")[0].at("text").get<std::string>().find("map_file")!=std::string::npos,"a non-conforming error has no structured content");
    auto loose=Protocol::ToolResultJson(Fail("Busy.",{{"code","busy"}}),"2025-06-18",Json{{"type","object"},{"properties",{{"undo",{{"type",Json::array({"object","null"})}}}}}});
    Check(loose.at("structuredContent").at("code")=="busy","an error that fits keeps its details");
    auto fits=Protocol::ToolResultJson(Ok({{"valid",true},{"summary",{{"errors",0}}}}),"2025-06-18",output);
    Check(fits.at("structuredContent").at("valid")==true,"results keep structured content");
    Check(Protocol::CheckStructured(output,{{"valid",true},{"summary",{{"errors",0}}}}).empty(),"a conforming result");
    Check(Protocol::CheckStructured(output,{{"valid",true},{"summary",Json::object()}}).find("summary.errors is required")!=std::string::npos,"nested required named");
    Check(Protocol::CheckStructured(output,{{"valid","yes"},{"summary",{{"errors",0}}}}).find("valid must be boolean")!=std::string::npos,"types checked");
    Check(Protocol::CheckStructured(Json(),{{"anything",1}}).empty(),"no outputSchema fits anything");
    ToolResult empty;empty.structured={{"x",2}};
    Check(Protocol::ToolResultJson(empty,"2025-06-18").at("content")[0].at("text").get<std::string>().find("\"x\"")!=std::string::npos,"empty content gets the JSON");

    Resource r;r.uri="scct://wiki/{page}";r.name="wiki";r.title="Wiki page";r.isTemplate=true;r.description="A page.";
    auto listed=Protocol::ResourceJson(r,"2025-06-18");
    Check(listed.at("uriTemplate")=="scct://wiki/{page}" && !listed.contains("uri") && listed.at("title")=="Wiki page" && listed.at("mimeType")=="text/markdown","template listing");
    r.isTemplate=false;r.uri="scct://rules";
    Check(Protocol::ResourceJson(r,"2024-11-05").at("uri")=="scct://rules" && !Protocol::ResourceJson(r,"2024-11-05").contains("title"),"resource listing");
    Prompt p;p.name="versus_starter";p.title="Starter";p.arguments={{"theme","What the map looks like.",true}};
    auto prompt=Protocol::PromptJson(p,"2025-06-18");
    Check(prompt.at("arguments")[0].at("name")=="theme" && prompt.at("arguments")[0].at("required")==true && prompt.at("title")=="Starter","prompt listing");

    auto text=Protocol::ReadContents("scct://rules",{{"text","# Rules"}},"text/markdown");
    Check(text.at("contents")[0].at("text")=="# Rules" && text.at("contents")[0].at("mimeType")=="text/markdown" && text.at("contents")[0].at("uri")=="scct://rules","text read");
    auto blob=Protocol::ReadContents("scct://map/screenshot/latest",{{"blob","AAAA"},{"mimeType","image/png"}},"application/octet-stream");
    Check(blob.at("contents")[0].at("blob")=="AAAA" && blob.at("contents")[0].at("mimeType")=="image/png","blob read keeps its mime type");
    auto progress=Protocol::ProgressParams("t1",2,0,"Building","2025-03-26");
    Check(progress.at("progressToken")=="t1" && !progress.contains("total") && progress.at("message")=="Building","progress");
    Check(!Protocol::ProgressParams(5,1,4,"x","2024-11-05").contains("message") && Protocol::ProgressParams(5,1,4,"x","2024-11-05").at("total")==4,"no message for old clients");
    Check(Protocol::LogRank("debug")==0 && Protocol::LogRank("warning")==3 && Protocol::LogRank("emergency")==7 && Protocol::LogRank("loud")==-1,"log levels");
}
void Arguments()
{
    const auto schema=Json::parse(R"({"type":"object","additionalProperties":false,"required":["what"],"properties":{
        "what":{"type":"string","enum":["summary","actors"]},
        "limit":{"type":"integer","minimum":1,"maximum":5000},
        "near":{"type":"object","required":["at"],"properties":{"at":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}}},
        "fields":{"anyOf":[{"type":"array","items":{"type":"string"}},{"type":"string","enum":["all"]}]},
        "actor":{"anyOf":[{"type":"string"},{"type":"object","required":["path","class"]}]},
        "name":{"type":"string","minLength":1,"maxLength":4},
        "flag":{"type":["boolean","null"]}}})");
    auto ok=[&](const char* text){return Protocol::CheckArguments(schema,Json::parse(text));};
    Check(ok(R"({"what":"summary"})").empty(),"minimal arguments fit");
    Check(ok(R"({"what":"actors","limit":200,"near":{"at":[0,0,1.5]},"fields":"all","actor":"MyLevel.Light1","flag":null})").empty(),"full arguments fit");
    Check(ok(R"({"what":"actors","limit":200.0})").empty(),"an integral float is an integer");
    Check(ok(R"({"what":"actors","fields":["Tag"],"actor":{"path":"a","class":"b"}})").empty(),"anyOf second forms");
    Check(ok(R"({})").find("arguments.what is required")!=std::string::npos,"required named");
    Check(ok(R"({"what":"everything"})").find("one of summary, actors")!=std::string::npos,"enum choices listed");
    Check(ok(R"({"what":3})").find("must be string")!=std::string::npos,"type named");
    Check(ok(R"({"what":"actors","limit":0})").find("at least 1")!=std::string::npos,"minimum");
    Check(ok(R"({"what":"actors","limit":2.5})").find("integer")!=std::string::npos,"fraction is no integer");
    Check(ok(R"({"what":"actors","limits":5})").find("limits is not a known field")!=std::string::npos,"unknown field refused, known ones listed");
    Check(ok(R"({"what":"actors","near":{"at":[0,0]}})").find("near.at needs at least 3")!=std::string::npos,"nested arrays checked");
    Check(ok(R"({"what":"actors","near":{"at":[0,"x",0]}})").find("near.at[1]")!=std::string::npos,"items checked");
    Check(!ok(R"({"what":"actors","near":{}})").empty(),"nested required");
    Check(!ok(R"({"what":"actors","fields":"some"})").empty(),"anyOf none fits");
    Check(!ok(R"({"what":"actors","actor":{"path":"a"}})").empty(),"anyOf object form checked");
    Check(!ok(R"({"what":"actors","name":""})").empty() && !ok(R"({"what":"actors","name":"toolong"})").empty(),"string lengths");
    Check(!Protocol::CheckArguments(schema,Json::array()).empty(),"arguments are an object");
    Check(Protocol::CheckArguments(Json{{"type","object"}},Json::parse(R"({"anything":1})")).empty(),"open schemas take anything");
}
void Templates()
{
    std::map<std::string,std::string> parts;
    Check(MatchTemplate("scct://wiki/{page}","scct://wiki/game-modes",parts) && parts["page"]=="game-modes","template part");
    parts.clear();
    Check(MatchTemplate("scct://map/actor/{path}","scct://map/actor/MyLevel.Light12",parts) && parts["path"]=="MyLevel.Light12","dots in the last part");
    Check(!MatchTemplate("scct://wiki/{page}","scct://schema/x",parts),"different prefix");
    Check(!MatchTemplate("scct://wiki/{page}","scct://wiki/",parts),"empty part");
}
int main()
{
    try{Versions();Envelopes();Lists();Arguments();Templates();}
    catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    std::cout<<"McpProtocolModel passed\n";
    return 0;
}
