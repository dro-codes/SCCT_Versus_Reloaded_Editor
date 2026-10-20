#pragma once
// An editor for tests: replays scripted results per op and records every call
// it was given. Portable; the tools cannot tell it from BridgeClient.
//
//   FakeBridge fake;
//   fake.On("editor.state", {{"open", true}, {"level", 7}});
//   fake.Fail("map.save", Bridge::Code::Refused, "The map has no file yet.");
//   fake.On("design.build", [](const Json& args, const CallOptions&) { return Json(true); });
//
// Several results queued for one op are returned in order and the last one
// repeats. A call with no script throws Bridge::Error(refused), as the bridge
// does for an op it does not know.
#include "McpTool.h"
#include <deque>
#include <mutex>

namespace Mcp
{
    class FakeBridge : public IBridge
    {
    public:
        using Handler = std::function<Json(const Json& args, const CallOptions& options)>;
        struct Received { std::string op; Json args; std::string label; bool dryRun = false; bool expect = false; unsigned timeoutMs = 0, busyMs = 0; };

        FakeBridge()
        {
            hello = Bridge::HelloReply("test", 4242, true, false, Bridge::OpList());
            editors = Json::array({{{"pid", 4242}}});
        }
        // Queues a result (or a handler) for op.
        void On(const std::string& op, Json result)
        {
            std::lock_guard<std::mutex> guard(lock);
            scripts[op].push_back({false, std::move(result), {}, {}, {}, {}, nullptr});
        }
        void On(const std::string& op, Handler handler)
        {
            std::lock_guard<std::mutex> guard(lock);
            Script script;
            script.handler = std::move(handler);
            scripts[op].push_back(std::move(script));
        }
        void Fail(const std::string& op, const std::string& code, const std::string& message, const std::string& reason = {}, Json details = Json())
        {
            std::lock_guard<std::mutex> guard(lock);
            scripts[op].push_back({true, Json(), code, message, reason, std::move(details), nullptr});
        }
        // Progress messages delivered to the next call of op before its result.
        void Progress(const std::string& op, Json messages)
        {
            std::lock_guard<std::mutex> guard(lock);
            progress[op] = std::move(messages);
        }
        void Clear(const std::string& op)
        {
            std::lock_guard<std::mutex> guard(lock);
            scripts.erase(op);
        }

        Json Call(const std::string& op, const Json& args, const CallOptions& options = {}) override
        {
            Script script;
            Json messages;
            uint64_t id = 0;
            {
                std::lock_guard<std::mutex> guard(lock);
                calls.push_back({op, args.is_null() ? Json::object() : args, options.label, options.dryRun, options.expect.set, options.timeoutMs, options.busyMs});
                id = ++nextCall;
                if (!connected) throw Bridge::Error(Bridge::Code::NotConnected, "No editor is connected (FakeBridge).");
                auto found = scripts.find(op);
                if (found == scripts.end() || found->second.empty())
                    throw Bridge::Error(Bridge::Code::Refused, "FakeBridge has no script for \"" + op + "\".", "unknown_op");
                script = found->second.front();
                if (found->second.size() > 1) found->second.pop_front();
                if (auto p = progress.find(op); p != progress.end()) { messages = p->second; progress.erase(p); }
            }
            if (options.progress && messages.is_array()) for (const auto& m : messages) options.progress(m);
            const auto* policy = Bridge::FindPolicy(op);
            const bool mutates = policy && Bridge::Mutates(policy->policy) && !options.dryRun;
            Json last{{"id", id}, {"op", op}, {"ok", !script.fail}, {"undo", {{"call", id}, {"steps", mutates && !script.fail ? 1 : 0}}},
                      {"log", Json::array()}, {"dialogs", Json::array()}, {"warnings", warnings}};
            {
                std::lock_guard<std::mutex> guard(lock);
                if (mutates && !script.fail) state["revision"] = state.value("revision", 0) + 1;
                state["lastCall"] = last;
            }
            if (script.fail) throw Bridge::Error(script.code, script.message, script.reason, script.details);
            if (script.handler) return script.handler(args.is_null() ? Json::object() : args, options);
            return script.result;
        }
        Json Hello() override
        {
            std::lock_guard<std::mutex> guard(lock);
            if (!connected) throw Bridge::Error(Bridge::Code::NotConnected, "No editor is connected (FakeBridge).");
            return hello;
        }
        bool Connected() override { std::lock_guard<std::mutex> guard(lock); return connected; }
        Json State() override { std::lock_guard<std::mutex> guard(lock); return state; }
        Json Editors() override { std::lock_guard<std::mutex> guard(lock); return editors; }
        void Select(unsigned long pid) override { std::lock_guard<std::mutex> guard(lock); selected = pid; }
        void Disconnect() override { std::lock_guard<std::mutex> guard(lock); ++disconnects; }

        // What the tools asked for, oldest first.
        size_t Count(const std::string& op) const
        {
            std::lock_guard<std::mutex> guard(lock);
            size_t n = 0;
            for (const auto& c : calls) n += c.op == op;
            return n;
        }
        const Received* Last(const std::string& op) const
        {
            std::lock_guard<std::mutex> guard(lock);
            for (auto it = calls.rbegin(); it != calls.rend(); ++it) if (it->op == op) return &*it;
            return nullptr;
        }
        std::vector<std::string> Ops() const
        {
            std::lock_guard<std::mutex> guard(lock);
            std::vector<std::string> ops;
            for (const auto& c : calls) ops.push_back(c.op);
            return ops;
        }

        std::deque<Received> calls;
        Json hello, editors, state = Json::object(), warnings = Json::array();
        bool connected = true;
        unsigned long selected = 0;
        int disconnects = 0;

    private:
        struct Script { bool fail = false; Json result; std::string code, message, reason; Json details; Handler handler; };
        mutable std::mutex lock;
        std::map<std::string, std::deque<Script>> scripts;
        std::map<std::string, Json> progress;
        uint64_t nextCall = 0;
    };
}
