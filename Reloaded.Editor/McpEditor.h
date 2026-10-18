#pragma once
// Editor access the MCP bridge adds to Workflow::Editor (WorkflowEditor.h).
// Implemented in McpNative.inl, inside WorkflowEditor.cpp, beside the engine
// helpers it needs. UI thread only, like the rest of Workflow::Editor.
#include "WorkflowModel.h"
#include <memory>
#include <string>

namespace Workflow::Editor
{
    // One Undo step around several bridge ops. While a JoinScope is open on
    // this thread, the Transactions RE+ opens join it instead of starting
    // their own, so a plan undoes in one step and a failure anywhere rolls the
    // whole plan back. Native commands nest inside it as they nest inside any
    // RE+ Transaction.
    class JoinScope
    {
    public:
        explicit JoinScope(const std::string& label);
        ~JoinScope();
        void Commit();
        JoinScope(const JoinScope&) = delete;
        JoinScope& operator=(const JoinScope&) = delete;
    private:
        bool committed = false, owner = false;
        uintptr_t buffer = 0;
    };
    bool Joining();
    // Prefix for the labels of Transactions begun on this thread ("MCP 12: "),
    // so Undo History names the bridge call; empty for none.
    void SetTransactionPrefix(const std::string& prefix);
    // The authoring identity of the open map (the key Map JSON documents name),
    // saved or not: the canonical file, or unsaved:<level>:<generation>.
    std::string AuthoringMap();
    // Transaction stamps (UndoMark) of the undoable records, oldest first, and
    // how many of the newest are undone (redo queue).
    struct UndoState { std::vector<unsigned> stamps; int undone = 0; bool open = false; };
    UndoState UndoRecords();
    // Live actors filtered for actors.query (see McpOps.cpp for the shape).
    Json QueryActors(const Json& query);
    // The classes' property schemas as authoring.export lists them, with
    // writability ("writable": false and a reason for paste-only fields).
    Json AuthoringClasses(const Json& names);
    // Loaded assets of a kind ("sound", "material", "staticmesh", "texture",
    // "class") whose path contains query (case-insensitive).
    Json SearchAssets(const std::string& kind, const std::string& query, size_t limit);
    // LevelInfo edits (settings.edit): the snapshot is settings.inspect's.
    Json EditSettings(const Json& snapshot, const Json& changes);
    // Trace segments through the built level: [{hit, at, normal, actor}].
    Json TraceSegments(const Json& segments);
    // GE ADD for an element ("ZL", "HOH", "PIPE", "LADDER", "FENCE", "POLE",
    // "LG"), points as the Map Design element tool orders them.
    Json AddGeometricEvent(const std::string& command, const Json& points);
}
