#include "pch.h"
#undef min
#undef max
#include "McpOps.h"
#include "McpBridge.h"
#include "McpBridgeModel.h"
#include "McpEditor.h"
#include "McpPlanModel.h"
#include "McpSafetyModel.h"
#include "WorkflowEditor.h"
#include "WorkflowTools.h"
#include "EditorExtras.h"
#include "Version.h"
#include "logger.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>

// The ops the MCP bridge added to ReloadedWorkflowRequest (McpOps.h): the
// session, reading the map, the edits the older ops did not cover, and
// mcp.plan, which runs several of them as one Undo step. Each handler takes
// the whole request, {"op": name, ...arguments}, on the editor's UI thread.
namespace McpOps
{
namespace Editor = Workflow::Editor;
namespace Bridge = Mcp::Bridge;
namespace Plan = Mcp::Plan;
namespace
{
    std::map<std::string,Handler>& Table() { static std::map<std::string,Handler> table; return table; }
    thread_local Context current;

    [[noreturn]] void Invalid(const std::string& message) { throw Bridge::Error(Bridge::Code::Invalid,message); }
    [[noreturn]] void Refuse(const std::string& message,const std::string& reason) { throw Bridge::Error(Bridge::Code::Refused,message,reason); }

    // editor.state: what a caller checks before and after a change. Each
    // field is read on its own, so one the editor cannot answer leaves the rest.
    Json EditorState()
    {
        Json state={{"version",RE_PLUS_VERSION}};
        try{state["level"]=static_cast<uint64_t>(Editor::LevelIdentity());}
        catch(const std::exception& e){state["open"]=false;state["reason"]=e.what();return state;}
        state["open"]=true;
        auto field=[&](const char* name,const std::function<Json()>& read){try{state[name]=read();}catch(const std::exception&){state[name]=nullptr;}};
        field("generation",[]{return Json(Editor::MapGeneration());});
        field("revision",[]{return Json(Editor::Revision());});
        field("contentRevision",[]{return Json(Editor::ContentRevision());});
        field("mapKey",[]{return Json(Editor::MapKey());});
        field("mapFile",[]{return Json(Editor::MapFile());});
        field("map",[]{return Json(Editor::AuthoringMap());});
        field("levelPath",[]{return Json(Editor::LevelPath());});
        field("dirty",[]{return Json(EditorExtras::HasUnsavedChanges());});
        field("canUndo",[]{return Json(Editor::CanUndo());});
        field("canRedo",[]{return Json(Editor::CanRedo());});
        field("undoMark",[]{return Json(Editor::UndoMark());});
        field("geometryBuilds",[]{return Json(Editor::GeometryBuilds());});
        field("joining",[]{return Json(Editor::Joining());});
        return state;
    }

    // Blobs: results too big for one pipe message (a map export), written to
    // <System>\ReloadedEditor\Mcp\blobs\<handle>.json and read back in chunks.
    // A handle is random hex, so a request can never name another file.
    std::filesystem::path BlobFolder() { return McpBridge::Directory()/"blobs"; }
    std::filesystem::path BlobFile(const std::string& blob)
    {
        if(blob.size()<16 || blob.size()>64 || blob.find_first_not_of("0123456789abcdef")!=std::string::npos)Invalid("Unknown blob handle \""+blob.substr(0,80)+"\".");
        return BlobFolder()/(blob+".json");
    }
    std::string NewHandle()
    {
        std::random_device random;const char* digits="0123456789abcdef";std::string handle;
        for(int i=0;i<32;++i)handle+=digits[random()%16];
        return handle;
    }
    // Blobs a client never freed go when the next is written: a day is longer
    // than any read of one takes.
    void SweepBlobs()
    {
        std::error_code error;const auto now=std::filesystem::file_time_type::clock::now();
        for(std::filesystem::directory_iterator it(BlobFolder(),error),end;!error && it!=end;it.increment(error))
        {
            std::error_code ignored;const auto time=std::filesystem::last_write_time(it->path(),ignored);
            if(!ignored && now-time>std::chrono::hours(24))std::filesystem::remove(it->path(),ignored);
        }
    }
    // Streamed, as the Map JSON export is: one string of a large map's
    // export needs a block the 32-bit editor rarely has.
    Json WriteBlob(const Json& document)
    {
        const auto folder=BlobFolder();std::error_code error;
        std::filesystem::create_directories(folder,error);
        if(error)throw std::runtime_error("Cannot create "+folder.string()+".");
        SweepBlobs();
        const auto blob=NewHandle();const auto file=folder/(blob+".json");auto temp=file;temp+=".tmp";
        try
        {
            std::ofstream out(temp,std::ios::binary|std::ios::trunc);
            if(!out)throw std::runtime_error("Cannot create "+temp.string()+".");
            out<<document;out.flush();
            if(!out)throw std::runtime_error("Could not write "+temp.string()+".");
        }
        catch(...){std::filesystem::remove(temp,error);throw;}
        std::filesystem::rename(temp,file,error);
        if(error){std::filesystem::remove(temp,error);throw std::runtime_error("Could not write "+file.string()+".");}
        return {{"blob",blob},{"size",std::filesystem::file_size(file)}};
    }
    Json ReadBlob(const Json& q)
    {
        const auto file=BlobFile(q.at("blob").get<std::string>());
        const auto offset=q.value("offset",uint64_t{0});
        const auto size=std::min<uint64_t>(q.value("size",static_cast<uint64_t>(Bridge::kBlobChunk)),Bridge::kBlobChunk);
        std::error_code error;const auto total=static_cast<uint64_t>(std::filesystem::file_size(file,error));
        if(error)Invalid("That blob is gone: freed, or swept a day after it was written.");
        if(offset>total)Invalid("offset is past the blob's end ("+std::to_string(total)+" bytes).");
        std::ifstream in(file,std::ios::binary);
        if(!in)throw std::runtime_error("Cannot read "+file.string()+".");
        in.seekg(static_cast<std::streamoff>(offset));
        std::string bytes(static_cast<size_t>(std::min<uint64_t>(size,total-offset)),'\0');
        in.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<size_t>(std::max<std::streamsize>(in.gcount(),0)));
        return {{"data",Bridge::Base64(bytes)},{"eof",offset+bytes.size()>=total},{"size",total}};
    }

    // What exec refuses (McpSafetyModel.h): commands that end the editor,
    // run a file of commands, load, save, import or export maps and
    // packages, undo blindly or reach the sound driver; and any other that
    // names a file outside the work folder and [Mcp] WriteRoots, or a
    // package that is not the map's own.
    void CheckRawCommand(const std::string& command)
    {
        Mcp::Safety::RawContext context;
        try{const auto file=Editor::MapFile();if(!file.empty())context.mapStem=std::filesystem::path(file).stem().string();}
        catch(const std::exception&){}
        std::error_code error;context.base=std::filesystem::current_path(error);
        context.roots=McpBridge::WriteRoots();
        if(const auto refusal=Mcp::Safety::RawCommandRefusal(command,context))throw Bridge::Error(refusal.code,refusal.message,refusal.reason);
    }

    // Undo and redo through the stock TRANSACTION UNDO / REDO, one step at a
    // time, stopping when a step does not move the buffer.
    void CheckUndoable()
    {
        if(Editor::Joining())throw std::runtime_error("Undo and redo cannot run inside a plan.");
        if(Editor::UndoRecords().open)throw Bridge::Error(Bridge::Code::Busy,"An editor operation is still open.","transaction");
    }
    size_t Step(bool redo,size_t count)
    {
        CheckUndoable();
        size_t done=0;
        for(;done<count;++done)
        {
            if(!(redo?Editor::CanRedo():Editor::CanUndo()))break;
            const auto before=Editor::UndoRecords().undone;
            Editor::Exec(redo?"TRANSACTION REDO":"TRANSACTION UNDO");
            if(Editor::UndoRecords().undone!=before+(redo?-1:1))break;
        }
        return done;
    }
    // undo.call: a bridge call's Undo steps, only while they are the newest
    // undoable ones, so nothing anyone did since is taken with them.
    Json UndoCall(unsigned call)
    {
        CheckUndoable();
        const auto records=McpBridge::Records();
        const auto record=std::find_if(records.begin(),records.end(),[&](const McpBridge::CallRecord& r){return r.call==call;});
        // No record: the call changed nothing, is older than the records the
        // bridge keeps, or was made in another editor session. The client
        // can forget it.
        if(record==records.end())
            throw Bridge::Error(Bridge::Code::Invalid,"This editor has no record of bridge call "+std::to_string(call)+
                                " with Undo steps: it changed nothing, is too old, or was made before the editor last started.","unknown_call");
        const auto name="Call "+std::to_string(call)+(record->label.empty()?std::string():" ("+record->label+")");
        const auto state=Editor::UndoRecords();
        const size_t undoable=state.stamps.size()-static_cast<size_t>(state.undone);
        auto mine=[&](unsigned stamp){return stamp>=record->markFrom && stamp<record->markTo;};
        size_t own=0,newer=0,first=undoable,redoable=0;
        for(size_t i=0;i<undoable;++i)
        {
            if(state.stamps[i]>=record->markTo)++newer;
            else if(mine(state.stamps[i])){++own;first=std::min(first,i);}
        }
        for(size_t i=undoable;i<state.stamps.size();++i)if(mine(state.stamps[i]))++redoable;
        if(!own)Refuse(redoable?name+" is undone already.":name+" has no steps left in the undo buffer.",redoable?"already_undone":"gone");
        if(redoable)Refuse(name+" is partly undone already; undo or redo by steps instead.","partly_undone");
        if(newer)Refuse(name+" is not the latest change: "+std::to_string(newer)+" newer Undo step(s) came after it. Undo those first, or undo by steps.","not_newest");
        if(first+own!=undoable)Refuse("Other changes are mixed in with "+name+"'s Undo steps; undo by steps instead.","interleaved");
        // A full buffer drops its oldest steps: a call that opens the buffer
        // without its first step may have lost some.
        if(first==0 && state.stamps.front()!=record->markFrom && record->steps>own)
            Refuse("The undo buffer dropped some of "+name+"'s steps when it filled; undo by steps instead.","trimmed");
        const auto done=Step(false,own);
        if(done!=own)throw std::runtime_error("The editor stopped after undoing "+std::to_string(done)+" of "+name+"'s "+std::to_string(own)+" steps.");
        return {{"call",call},{"done",done},{"steps",own}};
    }

    // mcp.plan (McpPlanModel.h): pre, then the steps in one Undo step, then post.
    Json ResultsJson(const std::map<std::string,Json>& results)
    {
        Json out=Json::object();
        for(const auto& [id,result]:results)out[id]=result;
        return out;
    }
    Json PlanJson(const Plan::Parsed& plan)
    {
        auto list=[](const std::vector<Plan::Step>& steps)
        {
            Json out=Json::array();
            for(const auto& step:steps)out.push_back({{"id",step.id},{"op",step.op},{"args",step.args},{"optional",step.optional}});
            return out;
        };
        return {{"label",plan.label},{"dryRun",plan.dryRun},{"pre",list(plan.pre)},{"steps",list(plan.steps)},{"post",list(plan.post)}};
    }
    // What a step works on, for a preview row: an actor, a document, a count.
    std::string Target(const Json& args)
    {
        auto pathOf=[](const Json& value){return value.is_object()?value.value("path",std::string()):value.is_string()?value.get<std::string>():std::string();};
        for(const char* key:{"actor","objective","mission","alarm","detector","owner","event","snapshot"})
            if(args.contains(key))
            {
                const auto& value=args.at(key);
                auto path=value.is_object() && value.contains("actor")?pathOf(value.at("actor")):pathOf(value);
                if(!path.empty())return path;
            }
        if(args.contains("document") && args.at("document").is_object())
        {
            const auto& document=args.at("document");
            const auto count=document.contains("operations") && document.at("operations").is_array()?document.at("operations").size():0;
            const auto description=document.value("description",std::string());
            return (description.empty()?std::string("map changes"):description)+" ("+std::to_string(count)+" operations)";
        }
        for(const char* key:{"items","members","moves","changes","targets","pieces","segments","points","copies"})
            if(args.contains(key) && args.at(key).is_array())return std::to_string(args.at(key).size())+" "+key;
        if(args.contains("spec") && args.at("spec").is_object())return args.at("spec").value("name",args.at("spec").value("kind",std::string()));
        if(args.contains("command") && args.at("command").is_string())return args.at("command").get<std::string>();
        return {};
    }
    // The ops that change the library document, which a rolled-back plan puts back.
    bool LibraryOp(const std::string& op)
    {
        static const std::set<std::string> ops{"design.register","design.unregister","mcp.sidecar.set"};
        return ops.count(op)!=0;
    }
    Bridge::Policy PolicyOf(const std::string& op)
    {
        const auto* policy=Bridge::FindPolicy(op);
        return policy?policy->policy:Bridge::Policy::Blocked;
    }
    // Steps whose effect Undo does not reach: files and packages written or
    // imported into, a process started or ended. A plan a dialog stopped
    // after one keeps the map changes that went with it.
    bool Lasting(const Plan::Step& step)
    {
        const auto policy=PolicyOf(step.op);
        return policy==Bridge::Policy::File || policy==Bridge::Policy::Process
            || (step.op=="authoring.apply" && (Plan::DocumentHas(step.args,"texture") || Plan::DocumentHas(step.args,"save")));
    }
    // The dialog watchdog answered a question it did not know during the
    // step: the answer may not be what the plan needs, so nothing more runs.
    void StopOnDialog()
    {
        if(McpBridge::DialogBlocked())
            throw Bridge::Error(Bridge::Code::DialogBlocked,"A dialog the bridge does not know opened during this step; it was answered, and the plan stopped here.","unknown_dialog");
    }
    Json StepRequest(const Plan::Step& step,const std::map<std::string,Json>& results)
    {
        auto request=Plan::Bind(step.args,results);
        if(!request.is_object())Invalid("Step \""+step.id+"\" args must be an object.");
        request["op"]=step.op;
        return request;
    }
    // ran: the ids of the steps that finished, in order; wrote: the lasting
    // ones; built: the builds, whose geometry an Undo of the map leaves.
    [[noreturn]] void Fail(const Plan::Step& step,Plan::Section section,const std::exception& error,bool rolledBack,bool earlier,
                           const std::map<std::string,Json>& results,const Json& errors,const Json& ran,const Json& wrote,const Json& built)
    {
        std::string code=Bridge::Code::EditorError,reason;Json cause;
        if(const auto* coded=dynamic_cast<const Bridge::Error*>(&error)){code=coded->code;reason=coded->reason;cause=coded->details;}
        // Retrying the whole plan would run again what already ran.
        if(earlier && Bridge::Retryable(code))code=Bridge::Code::EditorError;
        Json details={{"failedStep",step.id},{"op",step.op},{"section",Plan::SectionName(section)},{"rolledBack",rolledBack},{"steps",ResultsJson(results)},{"ran",ran}};
        if(!wrote.empty())details["wrote"]=wrote;
        if(!built.empty())details["built"]=built;
        if(!errors.empty())details["errors"]=errors;
        if(!cause.is_null())details["cause"]=cause;
        auto message="Plan step \""+step.id+"\" ("+step.op+") failed: "+error.what();
        if(rolledBack)message+=" The plan's map changes were undone.";
        throw Bridge::Error(code,message,reason,details);
    }
    // A dry run changes nothing: reads run (so later steps can bind to them),
    // a Map JSON or stages step shows its preview rows, and any other change
    // is a row saying it runs on apply.
    Json DryRun(const Plan::Parsed& plan,const std::string& label)
    {
        std::map<std::string,Json> results;Json preview=Json::array(),errors=Json::object();
        const Plan::Step* previous=nullptr;
        for(const auto& entry:Plan::Ordered(plan))
        {
            const auto section=entry.first;const auto& step=*entry.second;
            // A question nobody knows the answer to, opened by the step before:
            // the rest of the preview would rest on that answer.
            if(previous && McpBridge::DialogBlocked())
            {
                errors[previous->id]="A dialog the bridge does not know opened during this step, so the dry run stopped there.";
                break;
            }
            previous=&step;
            auto row=[&](const std::string& note)
            {
                return Json{{"step",step.id},{"section",Plan::SectionName(section)},{"change",step.op},{"target",Target(step.args)},{"note",note}};
            };
            Json request;
            try{request=StepRequest(step,results);}
            catch(const std::exception&){preview.push_back(row("runs on apply, with arguments from steps that only run then"));continue;}
            try
            {
                if(PolicyOf(step.op)==Bridge::Policy::Read){results[step.id]=WorkflowTools::Request(request);continue;}
                const bool authoring=step.op=="authoring.apply",stages=step.op=="stage.apply";
                if(authoring || stages)
                {
                    request["op"]=authoring?"authoring.preview":"stage.preview";
                    auto shown=WorkflowTools::Request(request);
                    if(shown.contains("rows") && shown.at("rows").is_array())
                        for(auto line:shown.at("rows"))
                        {
                            if(line.is_object()){line["step"]=step.id;line["section"]=Plan::SectionName(section);}
                            preview.push_back(std::move(line));
                        }
                    else{auto line=row("runs on apply");line["preview"]=shown;preview.push_back(std::move(line));}
                    // Later steps bind to the preview, whose identities are the names the creates would get.
                    results[step.id]=std::move(shown);
                    continue;
                }
                // An op whose dry run is its own (map.recover) makes its checks,
                // told it is a dry run whoever runs the plan.
                if(Bridge::PreviewsItself(step.op))
                {
                    Context dry=Current();dry.dryRun=true;
                    Json shown;
                    {
                        Scope scope(dry);
                        shown=WorkflowTools::Request(request);
                    }
                    auto line=row(shown.is_object()?shown.value("note",std::string("runs on apply")):std::string("runs on apply"));line["preview"]=shown;
                    preview.push_back(std::move(line));
                    results[step.id]=std::move(shown);
                    continue;
                }
                preview.push_back(row("runs on apply"));
            }
            catch(const std::exception& e)
            {
                // Its failure, when it is the plan's only step, is the call's
                // own answer, with its code and reason.
                if(Bridge::PreviewsItself(step.op) && Plan::Ordered(plan).size()==1)throw;
                auto line=row("fails as the map is now");line["error"]=e.what();
                preview.push_back(std::move(line));errors[step.id]=e.what();
            }
        }
        Json result={{"preview",preview},{"plan",PlanJson(plan)},{"label",label},{"dryRun",true},{"ok",errors.empty()}};
        if(!errors.empty())result["errors"]=errors;
        return result;
    }
    Json RunPlan(const Json& q)
    {
        Json given=q;given.erase("op");
        const auto plan=Plan::Check(given);
        const auto& context=Current();
        const auto label=!plan.label.empty()?plan.label:!context.label.empty()?context.label:std::string("MCP plan");
        if(plan.dryRun || context.dryRun)return DryRun(plan,label);
        if(Editor::Joining())throw std::runtime_error("A plan cannot run inside another plan.");
        std::map<std::string,Json> results;Json errors=Json::object(),done=Json::array(),wrote=Json::array(),built=Json::array();size_t ran=0,undoSteps=0;
        // A step that finished: in order, with what it wrote, then the stop
        // when it raised a dialog the bridge does not know.
        auto finished=[&](const Plan::Step& step,Plan::Section section)
        {
            ++ran;done.push_back(step.id);
            if(Lasting(step))
            {
                Json entry={{"step",step.id},{"op",step.op},{"section",Plan::SectionName(section)}};
                const auto& result=results[step.id];
                if(result.is_object() && result.contains("file") && result.at("file").is_string())entry["file"]=result.at("file");
                wrote.push_back(std::move(entry));
            }
            if(PolicyOf(step.op)==Bridge::Policy::Build)built.push_back(step.id);
            StopOnDialog();
        };
        // An optional step's failure is noted and the plan goes on, unless a
        // dialog came with it.
        auto skippable=[](const Plan::Step& step){return step.optional && !McpBridge::DialogBlocked();};
        // pre: package loads, imports and saves, which Undo does not reach.
        for(const auto& step:plan.pre)
        {
            bool answered=false;
            try{results[step.id]=WorkflowTools::Request(StepRequest(step,results));answered=true;finished(step,Plan::Section::Pre);}
            catch(const std::exception& e)
            {
                if(skippable(step)){errors[step.id]=e.what();continue;}
                // An op whose dry run is its own fails alone as that dry run
                // would: its refusal's details stay its own, not a plan's.
                if(!answered && Bridge::PreviewsItself(step.op) && Plan::Ordered(plan).size()==1)throw;
                Fail(step,Plan::Section::Pre,e,false,ran>0,results,errors,done,wrote,built);
            }
        }
        if(!plan.steps.empty())
        {
            const bool writes=std::any_of(plan.steps.begin(),plan.steps.end(),[](const Plan::Step& s){return Bridge::Mutates(PolicyOf(s.op));});
            std::optional<Editor::JoinScope> scope;
            if(writes)
            {
                try{scope.emplace(label);}
                catch(const std::exception& e){Fail(plan.steps.front(),Plan::Section::Steps,e,false,ran>0,results,errors,done,wrote,built);}
            }
            Json library;bool kept=false;
            for(const auto& step:plan.steps)
            {
                try
                {
                    if(LibraryOp(step.op) && !kept){library=WorkflowTools::LibraryEntry();kept=true;}
                    results[step.id]=WorkflowTools::Request(StepRequest(step,results));
                    finished(step,Plan::Section::Steps);
                }
                catch(const std::exception& e)
                {
                    // Every map change of the steps goes in one Undo, and the
                    // library goes back too, so Map Design lists no piece whose
                    // brushes that Undo took away.
                    scope.reset();
                    if(kept)
                        try{WorkflowTools::RestoreLibraryEntry(library);}
                        catch(const std::exception& again){Logger::log(std::string("MCP: could not put the workflow library back: ")+again.what());}
                    Fail(step,Plan::Section::Steps,e,writes,ran>0,results,errors,done,wrote,built);
                }
            }
            if(scope){scope->Commit();undoSteps=1;}
        }
        // post: builds, texturing, checks, saves and screenshots, each on its
        // own; a failure here leaves the steps' changes in place.
        for(const auto& step:plan.post)
        {
            try
            {
                results[step.id]=WorkflowTools::Request(StepRequest(step,results));
                if(PolicyOf(step.op)==Bridge::Policy::Undoable)++undoSteps;
                finished(step,Plan::Section::Post);
            }
            catch(const std::exception& e){if(skippable(step)){errors[step.id]=e.what();continue;}Fail(step,Plan::Section::Post,e,false,true,results,errors,done,wrote,built);}
        }
        Json result={{"steps",ResultsJson(results)},{"undo",{{"steps",undoSteps}}},{"label",label}};
        if(!wrote.empty())result["wrote"]=wrote;
        if(!errors.empty())result["errors"]=errors;
        return result;
    }

    // An identity, or a list given as one.
    Json List(const Json& q,const char* many,const char* one)
    {
        if(q.contains(many))return q.at(many);
        if(q.contains(one))return Json::array({q.at(one)});
        return Json();
    }
    Json Identity(const Json& value) { return {{"path",value.at("path")},{"class",value.at("class")}}; }
}

void Add(const std::string& op,Handler handler) { Table()[op]=std::move(handler); }
const Handler* Find(const std::string& op)
{
    const auto& table=Table();const auto found=table.find(op);
    return found==table.end()?nullptr:&found->second;
}
const Context& Current() { return current; }
Scope::Scope(const Context& context):previous(current) { current=context; }
Scope::~Scope() { current=previous; }
void Initialize()
{
    static bool done=false;
    if(done)return;
    done=true;
    RegisterCore();
    RegisterLifecycle();
}

void RegisterCore()
{
    // Session.
    Add("editor.state",[](const Json&)->Json{return EditorState();});
    Add("authoring.mapid",[](const Json&)->Json{return {{"map",Editor::AuthoringMap()}};});
    Add("log.tail",[](const Json& q)->Json
    {
        const auto max=std::clamp<size_t>(q.value("max",size_t{500}),1,2000);
        Json lines=Json::array();
        for(const auto& line:McpBridge::LogSince(q.value("since",uint64_t{0}),max))lines.push_back({{"seq",line.seq},{"level",line.level},{"text",line.text}});
        return {{"lines",lines},{"last",McpBridge::LogLast()}};
    });
    Add("exec",[](const Json& q)->Json
    {
        if(!McpBridge::Current().allowRawExec)Refuse("Raw editor commands are off. The user can allow them with [Mcp] AllowRawExec=1 in Reloaded_Editor.ini.","raw_exec_off");
        const auto command=q.at("command").get<std::string>();
        CheckRawCommand(command);
        std::string output;const bool ok=Editor::Exec(command,output);
        return {{"ok",ok},{"output",output}};
    });
    Add("mcp.plan",[](const Json& q)->Json{return RunPlan(q);});

    // Blobs.
    Add("blob.read",[](const Json& q)->Json{return ReadBlob(q);});
    Add("blob.free",[](const Json& q)->Json
    {
        std::error_code error;std::filesystem::remove(BlobFile(q.at("blob").get<std::string>()),error);
        return true;
    });

    // Undo and redo.
    Add("undo.call",[](const Json& q)->Json{return UndoCall(q.at("call").get<unsigned>());});
    Add("undo.steps",[](const Json& q)->Json
    {
        const auto count=q.value("count",1);if(count<1 || count>500)Invalid("count is 1 to 500 steps.");
        return {{"done",Step(false,static_cast<size_t>(count))}};
    });
    Add("redo.steps",[](const Json& q)->Json
    {
        const auto count=q.value("count",1);if(count<1 || count>500)Invalid("count is 1 to 500 steps.");
        return {{"done",Step(true,static_cast<size_t>(count))}};
    });

    // Reading the map. actors.query {classes, tags, names, near:{at, radius},
    // box:{min, max}, selected, fields: [names] | "all", limit (500, at most
    // 5000), cursor} -> {actors:[{path, class, name, tag, event, position,
    // rotation, selected, hidden, values?}], total, next?}; values as
    // InspectActor encodes them, any property by name or every editable one.
    Add("actors.query",[](const Json& q)->Json{return Editor::QueryActors(q);});
    Add("settings.inspect",[](const Json&)->Json{return Editor::LevelSettings();});
    Add("authoring.schema",[](const Json& q)->Json{return {{"classes",Editor::AuthoringClasses(List(q,"classes","class"))}};});
    Add("assets.search",[](const Json& q)->Json
    {
        const auto limit=q.value("limit",200);if(limit<1 || limit>5000)Invalid("limit is 1 to 5000 assets.");
        return Editor::SearchAssets(q.at("kind").get<std::string>(),q.value("query",std::string()),static_cast<size_t>(limit));
    });
    // Over the pipe the export goes to a blob (a large map's is tens of MB);
    // ReloadedWorkflowRequest callers keep the document itself, as before.
    Add("authoring.export",[](const Json& q)->Json
    {
        const bool blob=q.value("blob",Current().call!=0);
        auto document=Editor::ExportMapAuthoring();
        return blob?WriteBlob(document):document;
    });
    Add("scene.trace",[](const Json& q)->Json{return {{"hits",Editor::TraceSegments(q.at("segments"))}};});
    Add("props.copy",[](const Json& q)->Json{return Editor::CopyActorProperties(Identity(q.at("actor")));});
    Add("ge.list",[](const Json&)->Json
    {
        return {{"elements",WorkflowTools::DesignElements()},
                {"note","Only the geometric events Map Design or the bridge added are listed: the editor keeps no list RE+ can read."}};
    });
    Add("mcp.sidecar.get",[](const Json&)->Json{return {{"sidecar",WorkflowTools::McpSidecar()}};});

    // Edits, each one Undo step (or part of a plan's).
    Add("settings.edit",[](const Json& q)->Json{return Editor::EditSettings(q.at("snapshot"),q.at("changes"));});
    Add("placement.move",[](const Json& q)->Json
    {
        Json moves=Json::array();
        for(const auto& move:q.at("moves"))
        {
            auto entry=Identity(move.contains("actor")?move.at("actor"):move);entry["delta"]=move.at("delta");
            moves.push_back(std::move(entry));
        }
        return {{"moved",Editor::PlacementMove(moves,q.value("label",std::string("Move actors")))}};
    });
    Add("placement.turn",[](const Json& q)->Json
    {
        Json changes=Json::array();
        for(const auto& change:q.at("changes"))
        {
            auto entry=Identity(change.contains("actor")?change.at("actor"):change);entry["rotation"]=change.at("rotation");
            changes.push_back(std::move(entry));
        }
        return {{"turned",Editor::PlacementTurn(changes,q.value("label",std::string("Turn actors")))}};
    });
    Add("placement.copy",[](const Json& q)->Json
    {
        Json members=Json::array();for(const auto& member:q.at("members"))members.push_back(Identity(member));
        return {{"created",Editor::PlacementCopy(members,q.at("copies"),q.value("label",std::string("Copy actors")))}};
    });
    Add("props.paste",[](const Json& q)->Json
    {
        // The properties props.copy returned, or its whole result.
        auto properties=q.at("properties");
        if(properties.is_object() && properties.contains("properties"))properties=properties.at("properties");
        Json targets=Json::array();for(const auto& target:q.at("targets"))targets.push_back(Identity(target));
        return Editor::PasteActorProperties(targets,properties);
    });
    Add("ge.add",[](const Json& q)->Json{return Editor::AddGeometricEvent(q.at("command").get<std::string>(),q.at("points"));});
    Add("surface.texture",[](const Json& q)->Json
    {
        return Editor::TextureSurfaces(q.at("texture").get<std::string>(),q.value("surfaces",Json()),List(q,"brushes","brush"),List(q,"faces","face"));
    });
    // Map Design's library: metadata only, which a plan that fails puts back.
    Add("design.register",[](const Json& q)->Json{return WorkflowTools::RegisterDesignPieces(q.at("pieces"),q.value("elements",Json::array()));});
    Add("design.unregister",[](const Json& q)->Json{return {{"unregistered",WorkflowTools::UnregisterDesignPieces(q.at("members"))}};});
    Add("mcp.sidecar.set",[](const Json& q)->Json{WorkflowTools::SetMcpSidecar(q.at("sidecar"));return true;});
}
}
