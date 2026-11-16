#pragma once
// The MCP server: JSON-RPC 2.0, one UTF-8 message per line, over any pair of
// streams (stdin and stdout in the exe, strings in the tests). Portable.
//
// The reader (the caller of Run or Handle) answers initialize, ping, the
// lists, subscriptions and logging at once. tools/call, prompts/get and reads
// of live resources go to one worker thread and run one at a time, since
// they wait on the editor; notifications/cancelled drops a queued request or
// stops the running one (the bridge call in flight, and the tool at its next
// wait or progress report), whose reply is then not sent. A batch (protocol
// 2025-03-26 and older) is answered with one array once all of it is; later
// protocols have no batches and get an error. Subscribers of the live
// resources hear of a change after a tool call that changed something in the
// editor or connected to another editor. A resource URI may carry a
// #fragment (a section of a document), which is read and watched as the whole
// resource. Every write is one whole line under a lock.
#include "McpProtocolModel.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iosfwd>
#include <mutex>
#include <set>
#include <thread>

namespace Mcp
{
    struct ServerOptions
    {
        std::filesystem::path systemDir, workDir;
        std::string version;      // RE+ version, for serverInfo
        std::string instructions; // empty: DefaultInstructions()
        // The bridge's cancel and resume (BridgeClient::Cancel/Resume): cancel
        // runs on the reader thread when the running request is cancelled or
        // the input ends; resume before each queued request starts.
        std::function<void()> cancel, resume;
        // Diagnostics for stderr; stdout only ever carries JSON-RPC.
        std::function<void(const std::string& line)> log;
    };
    // What initialize tells the agent about using the server.
    std::string DefaultInstructions();

    class Server
    {
    public:
        using Writer = std::function<void(const std::string& line)>;
        Server(Registry& registry, IBridge& bridge, ServerOptions options, Writer write);
        ~Server();
        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;

        // One line from the client (a message or a batch array).
        void Handle(const std::string& line);
        // Handles lines until the stream ends, then Finish(5000, 3000). Returns the exit code.
        int Run(std::istream& input);
        // Stops the worker: first lets queued requests finish for up to
        // drainMs, then drops what is left, cancels the running request and
        // waits waitMs for it. False when it was still busy (it is then left
        // to the process exit).
        bool Finish(unsigned waitMs = 10000, unsigned drainMs = 0);
        // Blocks until no request is queued or running (tests).
        void Wait();
        // A bridge event ({t:"event", kind, data}); map changes notify
        // subscribers of the live resources. Any thread.
        void OnBridgeEvent(const Json& event);
        // A log message: to options.log, and to the client as
        // notifications/message when at or above its logging/setLevel.
        void Log(const std::string& level, const std::string& text);
        // Subscribers of live resources hear that they changed.
        void MapChanged();
        std::string NegotiatedVersion() const;

        // A batch's replies, sent as one array once every request in it is answered.
        struct Batch { std::mutex lock; Json replies = Json::array(); size_t pending = 0; bool sealed = false, sent = false; };

    private:
        struct Job { Json id; std::string key, method; Json params; std::shared_ptr<Batch> batch; };
        void Dispatch(const Json& message);
        void Request(const Protocol::Message& m);
        void Notify(const Protocol::Message& m);
        void Enqueue(const Protocol::Message& m);
        void Work();
        Json Execute(const Job& job);
        Json CallTool(const Job& job, ToolContext& context);
        Json ReadResource(const Json& id, const std::string& uri, ToolContext& context);
        Json GetPrompt(const Job& job, ToolContext& context);
        const Resource* FindResource(const std::string& uri) const;
        std::function<void(double, double, const std::string&)> Progress(const Json& params);
        void Send(const Json& message);
        void Write(const Json& message);
        // One request of a batch answered (reply) or dropped (null).
        void Settle(const std::shared_ptr<Batch>& batch, const Json* reply);

        Registry& registry;
        IBridge& bridge;
        ServerOptions options;
        Writer write;
        std::mutex output;
        mutable std::mutex lock; // everything below
        std::condition_variable wake, idle;
        std::deque<Job> queue;
        std::set<std::string> cancelled, subscriptions;
        std::string running, protocol = Protocol::kFallback, client;
        bool busy = false, stopping = false, finished = false, initialized = false;
        // The running request was cancelled: Tools::Cancelled() on the worker.
        std::atomic<bool> stopRunning{false};
        int logRank = 1; // info
        unsigned long editor = 0; // the editor that last said hello in a tool call
        std::thread worker;
    };

    // --self-test: the embedded examples through the level-spec compiler and
    // the rules, and the registry's own checks; 0 when everything passes.
    // Defined in main.cpp, which builds with the whole server.
    int SelfTest(std::ostream& out);
}
