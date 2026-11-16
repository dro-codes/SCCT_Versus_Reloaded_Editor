#pragma once
// Bridge ops added for the MCP server (McpBridgeModel.h lists them with their
// policies). WorkflowTools::Request looks here before its own chain, so an op
// registered here answers both the pipe and ReloadedWorkflowRequest. Every
// handler runs on the editor UI thread and receives the whole request,
// {"op": name, ...arguments}, like the ops in WorkflowTools.cpp.
#include "WorkflowModel.h"
#include <functional>
#include <string>

namespace McpOps
{
    using Json = Workflow::Json;
    // The bridge call a handler serves. call is 0 for ReloadedWorkflowRequest
    // and for steps run outside the bridge.
    struct Context
    {
        unsigned call = 0;
        std::string label;   // the Undo History label, "MCP 12: level_build"
        bool dryRun = false; // preview only: change nothing
        bool approved = false;
    };
    using Handler = std::function<Json(const Json& request)>;
    void Add(const std::string& op, Handler handler);
    const Handler* Find(const std::string& op);
    // The context of the call running on this thread (default outside one).
    const Context& Current();
    struct Scope
    {
        explicit Scope(const Context& context);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        Context previous;
    };
    // Each module adds its ops; Initialize (from WorkflowTools::Initialize)
    // calls them all once.
    void RegisterCore();      // McpOps.cpp: session, reading, edits, plans
    void RegisterLifecycle(); // McpLifecycle.cpp: map files, builds, views, checks, packages
    void Initialize();
}
