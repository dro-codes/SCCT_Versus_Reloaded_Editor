// Every op the editor answers has a policy row in McpBridgeModel.h (the bridge
// refuses an op without one), and every row names an op the editor answers.
// Reads the DLL's sources as text: op=="name" in the request chains of
// WorkflowTools.cpp, and Add("name", ...) in the MCP op registries. Run from
// the repository root, or pass it as the first argument.
#include "../Reloaded.Editor/McpBridgeModel.h"
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
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Patterns()
{
    const auto ops=OpsIn("else if(op==\"design.block\")x;\nif(op == \"map\")y;\nAdd(\"editor.state\",f);\nWrite(\"map.save\",g);\nRead(\"view.screenshot\",h);\nif(step.op==\"stage.apply\")z;\nif(step->op==\"select\")w;\nAppendMenuAdd(\"no\");\nif(c->op==\"x\")v; // answers: approval.status");
    Check(ops==std::set<std::string>({"design.block","map","editor.state","map.save","view.screenshot","approval.status"}),"op patterns");
}
int main(int argc,char** argv)
{
    try
    {
        Patterns();
        fs::path root=argc>1?fs::path(argv[1]):fs::path(".");
        if(!fs::exists(root/"Reloaded.Editor"/"WorkflowTools.cpp") && fs::exists(root/".."/"Reloaded.Editor"/"WorkflowTools.cpp"))root=root/"..";
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
        std::cout<<"McpOpCoverage passed ("<<answered.size()<<" ops, "<<rows.size()<<" policy rows)\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
