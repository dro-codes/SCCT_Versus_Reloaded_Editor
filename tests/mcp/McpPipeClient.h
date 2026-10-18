#pragma once
// A small synchronous client for the editor's MCP bridge pipe
// (\\.\pipe\ReloadedEditor.Mcp.<pid>, framed as McpBridgeModel.h says), for
// the native probe (WorkflowNativeTests.h) and other test tools. One thread
// uses a client at a time, and never the editor's frame thread: the bridge
// runs every call there, so a client on that thread would wait on itself.
#include <windows.h>
#include "../../Reloaded.Editor/McpBridgeModel.h"
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace McpPipe
{
    using Json = Mcp::Bridge::Json;

    inline std::wstring PipeName(unsigned long pid) { return std::wstring(Mcp::Bridge::kPipePrefix) + std::to_wstring(pid); }
    // Whether the editor has its pipe now, with an instance free or not.
    inline bool Listening(unsigned long pid)
    {
        if (WaitNamedPipeW(PipeName(pid).c_str(), 1)) return true;
        return GetLastError() == ERROR_SEM_TIMEOUT;
    }
    // The editor closed the pipe, or it broke.
    struct Closed : std::runtime_error { using std::runtime_error::runtime_error; };

    // What a result message says.
    inline bool Ok(const Json& reply) { return reply.is_object() && reply.value("t", std::string()) == "result" && reply.value("ok", false); }
    inline std::string ErrorField(const Json& reply, const char* key)
    {
        if (!reply.is_object() || !reply.contains("error") || !reply.at("error").is_object()) return {};
        return reply.at("error").value(key, std::string());
    }
    inline std::string Code(const Json& reply) { return ErrorField(reply, "code"); }
    inline std::string Reason(const Json& reply) { return ErrorField(reply, "reason"); }

    class Client
    {
    public:
        Client() = default;
        ~Client() { Close(); }
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        // The editor's pipe, waiting up to timeoutMs for the editor to create
        // it or to free an instance.
        void Open(unsigned long pid, DWORD timeoutMs = 10000)
        {
            Close();
            const auto name = PipeName(pid);
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            for (;;)
            {
                pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (pipe != INVALID_HANDLE_VALUE) break;
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY)
                    throw std::runtime_error("Cannot open the bridge pipe of editor " + std::to_string(pid) + " (error " + std::to_string(error) + ").");
                if (GetTickCount64() >= deadline)
                    throw std::runtime_error("Editor " + std::to_string(pid) + " has no bridge pipe: is [Mcp] Enabled=1, or RE+ Tools > MCP > Enable MCP server on?");
                if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 250);
                else Sleep(100);
            }
            reader = Mcp::Bridge::FrameReader();
            aside.clear();
        }
        bool IsOpen() const { return pipe != INVALID_HANDLE_VALUE; }
        void Close()
        {
            if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }

        // Bytes as they are: a frame, or a broken one for a test.
        void Write(const std::string& bytes)
        {
            Require();
            // In pieces, reading what arrives between them: the editor writes
            // replies while it reads, and a pipe full both ways never drains.
            for (size_t at = 0; at < bytes.size();)
            {
                const DWORD piece = static_cast<DWORD>((std::min)(bytes.size() - at, size_t{16384}));
                DWORD written = 0;
                if (!WriteFile(pipe, bytes.data() + at, piece, &written, nullptr))
                    throw Closed("The bridge pipe closed while a message was written (error " + std::to_string(GetLastError()) + ").");
                at += written;
                Drain();
            }
        }
        void Send(const Json& message) { Write(Mcp::Bridge::EncodeFrame(message.dump())); }

        // The next message (one kept aside first), or null when none came
        // within timeoutMs.
        Json Receive(DWORD timeoutMs)
        {
            if (!aside.empty())
            {
                auto message = std::move(aside.front());
                aside.erase(aside.begin());
                return message;
            }
            return Read(timeoutMs);
        }
        // hello: the editor's reply, or the refusal it sends as result 0.
        Json Hello(const std::string& client = "native-probe", const std::string& version = "test", DWORD timeoutMs = 10000)
        {
            Send(Mcp::Bridge::Hello(client, version));
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            for (;;)
            {
                const ULONGLONG now = GetTickCount64();
                if (now >= deadline) throw std::runtime_error("The editor did not answer hello within " + std::to_string(timeoutMs / 1000) + " s.");
                auto message = Read(static_cast<DWORD>(deadline - now));
                if (message.is_null()) continue;
                const auto type = message.value("t", std::string());
                if (type == "hello" || (type == "result" && Of(message, 0))) return message;
                aside.push_back(std::move(message));
            }
        }

        // A call; options may hold label, dryRun and expect {level, generation, revision}.
        uint64_t Post(const std::string& op, const Json& args = Json::object(), const Json& options = Json::object())
        {
            const auto id = ++last;
            const bool object = options.is_object();
            Send(Mcp::Bridge::CallMessage(id, op, args, object ? options.value("label", std::string()) : std::string(),
                                          object && options.value("dryRun", false), Mcp::Bridge::ReadExpect(options)));
            return id;
        }
        void Cancel(uint64_t id) { Send(Json{{"t", "cancel"}, {"id", id}}); }
        // The next message of call id: a progress or its result.
        Json Next(uint64_t id, DWORD timeoutMs) { return NextBy(id, GetTickCount64() + timeoutMs, timeoutMs); }
        // Call id's result; its progress messages go to progress on the way.
        Json Wait(uint64_t id, DWORD timeoutMs = 120000, const std::function<void(const Json&)>& progress = {})
        {
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            for (;;)
            {
                auto message = NextBy(id, deadline, timeoutMs);
                if (message.value("t", std::string()) == "result") return message;
                if (progress) progress(message);
            }
        }
        // Call id's progress at stage, or its result if that comes first.
        Json Until(uint64_t id, const std::string& stage, DWORD timeoutMs)
        {
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            for (;;)
            {
                auto message = NextBy(id, deadline, timeoutMs);
                if (message.value("t", std::string()) == "result" || message.value("stage", std::string()) == stage) return message;
            }
        }
        Json Call(const std::string& op, const Json& args = Json::object(), const Json& options = Json::object(), DWORD timeoutMs = 120000,
                  std::vector<Json>* progress = nullptr)
        {
            return Wait(Post(op, args, options), timeoutMs, [progress](const Json& message) { if (progress) progress->push_back(message); });
        }

    private:
        HANDLE pipe = INVALID_HANDLE_VALUE;
        Mcp::Bridge::FrameReader reader;
        std::vector<Json> aside; // messages read while waiting for another call
        uint64_t last = 0;

        static bool Of(const Json& message, uint64_t id)
        {
            return message.is_object() && message.contains("id") && message.at("id").is_number_unsigned() && message.at("id").get<uint64_t>() == id;
        }
        Json NextBy(uint64_t id, ULONGLONG deadline, DWORD timeoutMs)
        {
            for (size_t i = 0; i < aside.size(); ++i)
                if (Of(aside[i], id))
                {
                    auto message = std::move(aside[i]);
                    aside.erase(aside.begin() + static_cast<std::ptrdiff_t>(i));
                    return message;
                }
            for (;;)
            {
                const ULONGLONG now = GetTickCount64();
                if (now >= deadline) throw std::runtime_error("No reply to bridge call " + std::to_string(id) + " within " + std::to_string(timeoutMs / 1000) + " s.");
                auto message = Read(static_cast<DWORD>(deadline - now));
                if (message.is_null()) continue;
                if (Of(message, id)) return message;
                aside.push_back(std::move(message));
            }
        }
        void Require() const
        {
            if (pipe == INVALID_HANDLE_VALUE) throw Closed("The bridge pipe is not open.");
        }
        // What has arrived, without waiting; false when nothing had.
        bool Drain()
        {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
                throw Closed("The editor closed the bridge pipe (error " + std::to_string(GetLastError()) + ").");
            if (!available) return false;
            std::vector<char> buffer((std::min)(available, DWORD{1u << 20}));
            DWORD read = 0;
            if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
                throw Closed("The editor closed the bridge pipe (error " + std::to_string(GetLastError()) + ").");
            reader.Append(buffer.data(), read);
            return read != 0;
        }
        Json Read(DWORD timeoutMs)
        {
            Require();
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            std::string frame;
            for (;;)
            {
                if (reader.Next(frame)) return Mcp::Bridge::Parse(frame);
                if (Drain()) continue;
                if (GetTickCount64() >= deadline) return Json();
                Sleep(5);
            }
        }
    };

    // A job (a client's pipe work) on a worker thread while this thread pumps
    // its messages. On the editor's frame thread that pumping is what runs
    // the job's calls, and any modal loop a call opens.
    class Worker
    {
    public:
        Worker() = default;
        ~Worker()
        {
            // The job may still use what it captured: it ends first.
            if (thread) { Pump(); CloseHandle(thread); }
        }
        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;
        void Start(std::function<void()> body)
        {
            if (thread) throw std::runtime_error("A bridge client job is still running.");
            job = std::move(body);
            error = nullptr;
            thread = CreateThread(nullptr, 0, Main, this, 0, nullptr);
            if (!thread) throw std::runtime_error("Could not start the bridge client thread (error " + std::to_string(GetLastError()) + ").");
        }
        // Pumps until the job ends (its own pipe timeouts bound it), then
        // throws what it threw; one slower than budgetMs fails too.
        void Finish(DWORD budgetMs = INFINITE)
        {
            if (!thread) return;
            const ULONGLONG started = GetTickCount64();
            Pump();
            CloseHandle(thread);
            thread = nullptr;
            if (error) std::rethrow_exception(error);
            if (budgetMs != INFINITE && GetTickCount64() - started > budgetMs)
                throw std::runtime_error("A bridge client job took longer than " + std::to_string(budgetMs / 1000) + " s.");
        }
        void Run(std::function<void()> body, DWORD budgetMs = INFINITE)
        {
            Start(std::move(body));
            Finish(budgetMs);
        }

    private:
        std::function<void()> job;
        std::exception_ptr error;
        HANDLE thread = nullptr;

        static DWORD WINAPI Main(void* self)
        {
            auto& worker = *static_cast<Worker*>(self);
            try { worker.job(); }
            catch (...) { worker.error = std::current_exception(); }
            return 0;
        }
        void Pump()
        {
            bool quit = false;
            int code = 0;
            while (MsgWaitForMultipleObjects(1, &thread, FALSE, 100, quit ? DWORD{0} : static_cast<DWORD>(QS_ALLINPUT)) != WAIT_OBJECT_0)
            {
                MSG message{};
                while (!quit && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    // The editor is closing: stop dispatching and hand the
                    // quit on once the job has ended.
                    if (message.message == WM_QUIT) { quit = true; code = static_cast<int>(message.wParam); break; }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            if (quit) PostQuitMessage(code);
        }
    };

    // A top-level window of this process by class and title, from any of its threads.
    inline HWND OwnWindow(const wchar_t* type, const wchar_t* title)
    {
        for (HWND window = FindWindowExW(nullptr, nullptr, type, title); window; window = FindWindowExW(nullptr, window, type, title))
        {
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            if (process == GetCurrentProcessId()) return window;
        }
        return nullptr;
    }
}
