#include "../Reloaded.Editor/McpPlanModel.h"
#include <iostream>
#include <source_location>
using namespace Mcp;
using Workflow::Json;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void Reject(F f,std::source_location where=std::source_location::current())
{
    bool caught=false;
    try{f();}catch(const std::exception&){caught=true;}
    if(!caught)throw std::runtime_error("invalid plan accepted at line "+std::to_string(where.line()));
}
Json Step(const char* id,const char* op,Json args=Json::object()){return {{"id",id},{"op",op},{"args",args}};}
Json Document(std::initializer_list<const char*> ops)
{
    Json operations=Json::array();for(auto op:ops)operations.push_back({{"op",op}});
    return {{"document",{{"format","scct.map-changes"},{"operations",operations}}}};
}
void Parsing()
{
    auto plan=Plan::Parse({{"label","Build"},{"steps",{Step("a","design.blockbatch"),Step("b","authoring.apply",Document({"create"}))}},{"post",{Step("c","design.build")}}});
    Check(plan.label=="Build" && plan.steps.size()==2 && plan.post.size()==1 && plan.pre.empty(),"sections");
    auto unnamed=Plan::Parse({{"steps",{{{"op","select"}}}}});
    Check(unnamed.steps[0].id=="steps1","default ids");
    Reject([]{Plan::Parse(Json::array());});
    Reject([]{Plan::Parse({{"steps",Json::array()}});});
    Reject([]{Plan::Parse({{"steps",{Step("a","select"),Step("a","select")}}});});
    Reject([]{Plan::Parse({{"steps",{Step("1a","select")}}});});
    Reject([]{Plan::Parse({{"step",{Step("a","select")}}});});
    Reject([]{Plan::Parse({{"steps",{{{"op","select"},{"when","later"}}}}});});
    Reject([]{Plan::Parse({{"steps",{Step("a","mcp.plan")}}});});
    Json many=Json::array();for(size_t i=0;i<=Bridge::kMaxPlanSteps;++i)many.push_back({{"op","select"}});
    Reject([&]{Plan::Parse({{"steps",many}});});
}
void Policies()
{
    Plan::Check({{"pre",{Step("p","authoring.apply",Document({"load","texture","save"}))}},{"steps",{Step("a","authoring.apply",Document({"create","link"})),Step("r","actors.query")}},{"post",{Step("b","design.build"),Step("s","map.save"),Step("t","surface.texture")}}});
    Reject([]{Plan::Check({{"steps",{Step("a","map.save")}}});});
    Reject([]{Plan::Check({{"steps",{Step("a","design.build")}}});});
    Reject([]{Plan::Check({{"pre",{Step("a","design.block")}}});});
    Reject([]{Plan::Check({{"steps",{Step("a","surface.texture")}}});});
    Reject([]{Plan::Check({{"steps",{Step("a","authoring.apply",Document({"texture","create"}))}}});});
    Reject([]{Plan::Check({{"post",{Step("a","exec")}}});});
    Reject([]{Plan::Check({{"post",{Step("a","light.set")}}});});
    Reject([]{Plan::Check({{"post",{Step("a","undo.steps")}}});});
    Reject([]{Plan::Check({{"post",{Step("a","made.up")}}});});
}
void Ordering()
{
    // Deletes and one replacement batch first, then pastes.
    Plan::Check({{"steps",{Step("d","authoring.apply",Document({"delete"})),Step("r","design.blockbatch",{{"items",{{{"previous",{{"members",Json::array()}}}}}}}),Step("n","design.blockbatch"),Step("a","authoring.apply",Document({"create"}))}}});
    Reject([]{Plan::Check({{"steps",{Step("n","design.block"),Step("d","authoring.apply",Document({"delete"}))}}});});
    Reject([]{Plan::Check({{"steps",{Step("r1","design.block",{{"previous",{{"members",Json::array()}}}}),Step("r2","design.block",{{"previous",{{"members",Json::array()}}}})}}});});
    Reject([]{Plan::Check({{"steps",{Step("s","start.create"),Step("x","security.delete")}}});});
    // Updates and links alone neither delete nor paste.
    Plan::Check({{"steps",{Step("n","design.block"),Step("u","authoring.apply",Document({"update","link"})),Step("m","magic.edit")}}});
    Check(Plan::Pastes({"a","authoring.apply",Document({"component"})}) && !Plan::Deletes({"a","authoring.apply",Document({"update"})}),"component pastes");
}
void Binding()
{
    std::map<std::string,Json> results;
    results["a"]={{"identities",{{"Obj1",{{"path","MyLevel.SObjective3"},{"class","SBase.SObjective"}}}}}};
    results["b"]=Json::array({1,2});
    auto bound=Plan::Bind({{"objective",{{"$bind",{{"step","a"},{"ptr","/identities/Obj1"}}}}},{"all",{{"$bind",{{"step","b"}}}}},{"list",{1,{{"$bind",{{"step","b"},{"ptr","/1"}}}}}},{"min",2}},results);
    Check(bound.at("objective").at("path")=="MyLevel.SObjective3","pointer into a result");
    Check(bound.at("all")==Json::array({1,2}),"whole result");
    Check(bound.at("list")==Json::array({1,2}) && bound.at("min")==2,"nested and untouched");
    Reject([&]{Plan::Bind({{"x",{{"$bind",{{"step","later"}}}}}},results);});
    Reject([&]{Plan::Bind({{"x",{{"$bind",{{"step","a"},{"ptr","/identities/Nope"}}}}}},results);});
    // An object with $bind beside other keys is data, not a binding.
    Check(Plan::Bind({{"x",{{"$bind",1},{"y",2}}}},results).at("x").at("y")==2,"not a bind");
}
void Single()
{
    auto edit=Plan::Single("authoring.apply",Document({"create"}),"MCP 1");
    Check(edit.contains("steps") && edit.at("label")=="MCP 1","joinable edit in steps");
    Check(Plan::Single("map.save",{{"path","x"}},"").contains("pre"),"file op in pre");
    Check(Plan::Single("design.build",Json::object(),"").contains("post"),"build in post");
    Check(Plan::Single("surface.texture",Json::object(),"").contains("post"),"own-step edit in post");
    Check(Plan::Single("authoring.apply",Document({"load"}),"").contains("pre"),"package document first");
    Check(Plan::Single("authoring.apply",Document({"load","create"}),"").contains("post"),"mixed document on its own");
    Plan::Check(Plan::Single("authoring.apply",Document({"load","create"}),""));
    Check(!Plan::Mutates(Plan::Parse({{"post",{Step("q","actors.query")}}})) && Plan::Mutates(Plan::Parse(edit)),"mutates");
}
// map.recover runs only as its own call: a client's plan holding it is
// refused by the bridge, while the bridge's own one-step plan of the call is
// checked as any plan.
void Alone()
{
    const Json recover{{"source","OffsD"},{"destination","OffsR"}};
    const auto single=Plan::Single("map.recover",recover,"");
    Check(single.contains("pre"),"map.recover's own call is a plan of one step in pre");
    Plan::Check(single);
    for(const char* section:{"pre","post"})
    {
        const Json plan{{section,{Step("r","map.recover",recover)}},{"steps",{Step("q","actors.query")}}};
        Plan::Check(plan);
        const auto refusal=Plan::NotInPlan(Plan::Parse(plan));
        Check(refusal.find("map.recover cannot run inside a plan")==0 && refusal.find("step \"r\" in "+std::string(section))!=std::string::npos
              && refusal.find("map_recover")!=std::string::npos,"a client's plan holding map.recover is refused, naming the step and the tool");
    }
    Check(!Plan::NotInPlan(Plan::Parse({{"pre",{Step("r","map.recover",recover)}}})).empty(),"even alone in a client's plan, which cannot cancel it");
    Check(Plan::NotInPlan(Plan::Parse({{"pre",{Step("o","map.open")}},{"post",{Step("s","map.save")}}})).empty(),"other file ops run in plans as before");
}
int main()
{
    try{Parsing();Policies();Ordering();Binding();Single();Alone();}
    catch(const std::exception& e){std::cerr<<"MCP plan model tests failed: "<<e.what()<<"\n";return 1;}
    std::cout<<"MCP plan model tests passed\n";
}
