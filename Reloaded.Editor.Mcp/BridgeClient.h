#pragma once
// The editor over its named pipe (\\.\pipe\ReloadedEditor.Mcp.<pid>), with the
// frames and messages of McpBridgeModel.h. Connects on first use and says
// hello; a busy editor is retried with backoff (250 ms doubling to 4 s) until
// the call's timeout, except while a change waits in the review window or
// another client holds the editor (busy approval_pending, another_client),
// which fail at once; a pipe that broke since the last call is reconnected
// once. Off Windows every call throws Bridge::Error(not_connected), so the
// server and its tests still build there.
//
// State() is the state block of the last reply, plus "lastCall": {id, op, ok,
// undo:{call, steps}, log, dialogs, warnings} from that reply, which the tools
// use to say how to undo what they did.
//
// Approval: when the editor asks the user to review a change it sends
// progress {stage:"awaiting_approval", ticket}. The client waits 50 s more for
// the answer, then gives up on the call with Bridge::Error(timeout, reason
// "awaiting_approval", details {ticket, call}). The result still arrives
// later; Call("approval.status", {ticket, waitMs}) returns it once it has
// ({pending:false, ok, result | error, state}) and otherwise asks the editor.
#include "McpTool.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace Mcp
{
    class BridgeClient : public IBridge
    {
    public:
        struct Options
        {
            std::string client = "Reloaded.Editor.Mcp", version;
            // Bridge events ({t:"event", kind, data}), such as map_changed.
            // Called on the thread that is making a call.
            std::function<void(const Json& event)> onEvent;
            std::function<void(const std::string& line)> log;
        };
        explicit BridgeClient(Options options);
        ~BridgeClient() override;
        BridgeClient(const BridgeClient&) = delete;
        BridgeClient& operator=(const BridgeClient&) = delete;

        Json Call(const std::string& op, const Json& args, const CallOptions& options = {}) override;
        Json Hello() override;
        bool Connected() override;
        Json State() override;
        Json Editors() override;
        void Select(unsigned long pid) override;
        void Disconnect() override;
        // Stops waiting for the call in flight (any thread): the editor is
        // asked to cancel it and the call throws Bridge::Error(cancelled), as
        // does every later Call, Hello and Editors until Resume(), so a
        // cancelled tool stops at its next editor call. The server resumes
        // before each request.
        void Cancel();
        void Resume();
        // Process ids of editors with a bridge pipe, from the pipe namespace.
        static std::vector<unsigned long> Discover();

    private:
        struct Impl;
        Options options;
        std::unique_ptr<Impl> impl;
    };

    // Starting and watching the editor process, replaceable by tests.
    struct Processes
    {
        // Starts exe with arguments in folder and returns its process id;
        // throws std::runtime_error with the reason when it cannot.
        std::function<unsigned long(const std::filesystem::path& exe, const std::string& arguments, const std::filesystem::path& folder)> launch;
        std::function<bool(unsigned long pid)> running;
        // True once the process has exited (or was never there).
        std::function<bool(unsigned long pid, unsigned timeoutMs)> waitExit;
    };
    Processes& Process();
}
