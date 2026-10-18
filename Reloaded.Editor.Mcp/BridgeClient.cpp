#ifdef _WIN32
#define NOMINMAX // <windows.h> would turn std::min and std::max into macros
#endif
#include "BridgeClient.h"
#include "../Reloaded.Editor/McpGuardModel.h"
#include <algorithm>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Mcp
{
#ifdef _WIN32
namespace
{
    using Clock = std::chrono::steady_clock;
    using Bridge::Code::Busy;
    using Bridge::Code::Cancelled;
    using Bridge::Code::NotConnected;
    using Bridge::Code::Timeout;
    constexpr unsigned kHelloMs = 10000, kWriteMs = 10000, kApprovalMs = 50000, kCancelGraceMs = 3000;
    constexpr unsigned kBackoffFirstMs = 250, kBackoffLastMs = 4000;
    constexpr const char* kStartHint =
        "Start the editor with editor_launch, or ask the user to open Reloaded_Editor.exe and turn on RE+ Tools > MCP > Enable MCP server.";

    // The pipe broke (or sent bytes that cannot be framed): the connection is
    // closed and, when it is safe, made again. Never leaves the client.
    struct Broken : std::runtime_error { using std::runtime_error::runtime_error; };

    uint64_t MessageId(const Json& message)
    {
        if (!message.is_object() || !message.contains("id")) return 0;
        const auto& id = message.at("id");
        return id.is_number_unsigned() || id.is_number_integer() ? id.get<uint64_t>() : 0;
    }
    std::string TicketText(const Json& value)
    {
        if (value.is_string()) return value.get<std::string>();
        return value.is_null() ? std::string() : value.dump();
    }
    std::string Seconds(unsigned ms) { return std::to_string((ms + 500) / 1000) + " s"; }
    Bridge::Error FromBody(const Json& error)
    {
        if (!error.is_object()) return Bridge::Error(Bridge::Code::EditorError, "The editor reported an error without details.");
        return Bridge::Error(error.value("code", std::string(Bridge::Code::EditorError)), error.value("message", std::string("The editor reported an error.")),
                             error.value("reason", std::string()), error.contains("details") ? error.at("details") : Json());
    }
    Json LastCall(uint64_t id, const std::string& op, const Json& message)
    {
        Json last{{"id", id}, {"op", op}, {"ok", message.value("ok", false)}};
        for (const char* key : {"undo", "log", "dialogs", "warnings"}) if (message.contains(key)) last[key] = message.at(key);
        return last;
    }
    std::string Describe(DWORD error)
    {
        char* text = nullptr;
        const DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                                          reinterpret_cast<char*>(&text), 0, nullptr);
        std::string out = size && text ? std::string(text, size) : "Windows error " + std::to_string(error);
        if (text) LocalFree(text);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.')) out.pop_back();
        return out + " (" + std::to_string(error) + ")";
    }
    DWORD Remaining(Clock::time_point deadline)
    {
        const auto now = Clock::now();
        if (now >= deadline) return 0;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        return static_cast<DWORD>(std::min<long long>(ms + 1, 0x7ffffffeLL));
    }
    enum class Waited { Done, TimedOut, Stopped };
}

struct BridgeClient::Impl
{
    std::mutex lock;                 // one call at a time, and everything below
    std::atomic<bool> stopped{false};// Cancel() until Resume(): every call fails fast
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE io = nullptr;             // the overlapped operation's event
    HANDLE cancel = nullptr;         // manual reset; Cancel() sets it
    Bridge::FrameReader reader;
    std::vector<char> buffer = std::vector<char>(64 * 1024);
    unsigned long pid = 0, selected = 0;
    Json hello, state = Json::object();
    uint64_t next = 0;
    std::map<uint64_t, std::string> abandoned; // call id -> approval ticket ("" when nobody wants the result)
    std::map<std::string, Json> late;          // approval ticket -> the call's result message
    const Options* options = nullptr;

    void Log(const std::string& line) const { if (options && options->log) options->log(line); }
    void Close()
    {
        if (pipe != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(pipe, nullptr);
            CloseHandle(pipe);
            Log("Disconnected from the editor (process " + std::to_string(pid) + ").");
        }
        pipe = INVALID_HANDLE_VALUE;
        reader = Bridge::FrameReader();
        hello = Json();
        abandoned.clear();
    }
    // One overlapped transfer. Done also covers a transfer that completed as
    // it was being cancelled, so no bytes are lost.
    Waited Transfer(bool write, char* data, DWORD size, DWORD& moved, DWORD timeoutMs, bool cancellable)
    {
        moved = 0;
        OVERLAPPED overlapped{};
        overlapped.hEvent = io;
        ResetEvent(io);
        const BOOL ok = write ? WriteFile(pipe, data, size, nullptr, &overlapped) : ReadFile(pipe, data, size, nullptr, &overlapped);
        if (!ok)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) throw Broken("The editor's pipe failed: " + Describe(error) + ".");
            HANDLE handles[2] = {io, cancel};
            const DWORD waited = WaitForMultipleObjects(cancellable ? 2 : 1, handles, FALSE, timeoutMs);
            if (waited != WAIT_OBJECT_0)
            {
                CancelIoEx(pipe, &overlapped);
                DWORD done = 0;
                if (GetOverlappedResult(pipe, &overlapped, &done, TRUE)) { moved = done; return Waited::Done; }
                return waited == WAIT_OBJECT_0 + 1 ? Waited::Stopped : Waited::TimedOut;
            }
        }
        if (!GetOverlappedResult(pipe, &overlapped, &moved, FALSE))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_MORE_DATA) throw Broken("The editor's pipe failed: " + Describe(error) + ".");
        }
        return Waited::Done;
    }
    void Write(const Json& message)
    {
        auto frame = Bridge::EncodeFrame(message.dump(-1, ' ', false, Json::error_handler_t::replace));
        size_t offset = 0;
        const auto deadline = Clock::now() + std::chrono::milliseconds(kWriteMs);
        while (offset < frame.size())
        {
            DWORD moved = 0;
            const auto chunk = static_cast<DWORD>(std::min<size_t>(frame.size() - offset, 1u << 20));
            if (Transfer(true, frame.data() + offset, chunk, moved, Remaining(deadline), false) != Waited::Done || !moved)
                throw Broken("The editor stopped reading its pipe.");
            offset += moved;
        }
    }
    // The next whole message, or Timeout/Cancelled when none came in time.
    Waited Read(Clock::time_point deadline, bool cancellable, Json& out)
    {
        std::string frame;
        for (;;)
        {
            bool whole = false;
            try { whole = reader.Next(frame); }
            catch (const std::exception& e) { throw Broken(e.what()); }
            if (whole) break;
            const DWORD wait = Remaining(deadline);
            if (!wait) return Waited::TimedOut;
            DWORD moved = 0;
            const auto waited = Transfer(false, buffer.data(), static_cast<DWORD>(buffer.size()), moved, wait, cancellable);
            if (waited != Waited::Done) return waited;
            if (!moved) throw Broken("The editor closed its pipe.");
            reader.Append(buffer.data(), moved);
        }
        try { out = Bridge::Parse(frame); }
        catch (const std::exception&) { throw Broken("The editor sent a message that is not JSON."); }
        return Waited::Done;
    }
    // Events and results nobody is waiting for, seen while waiting for something else.
    void Aside(const Json& message)
    {
        const auto t = message.value("t", std::string());
        if (t == "event") { if (options && options->onEvent) options->onEvent(message); return; }
        if (t != "result") return;
        const auto found = abandoned.find(MessageId(message));
        if (found == abandoned.end()) return;
        if (!found->second.empty()) late[found->second] = message;
        abandoned.erase(found);
    }
    void Connect()
    {
        unsigned long target = selected;
        if (!target)
        {
            const auto pids = Discover();
            if (pids.empty()) throw Bridge::Error(NotConnected, std::string("No RE+ editor is listening for MCP. ") + kStartHint);
            if (pids.size() > 1)
            {
                std::string list;
                for (auto p : pids) list += (list.empty() ? "" : ", ") + std::to_string(p);
                throw Bridge::Error(Bridge::Code::Invalid, "Several editors are running (process ids " + list + "). Call editor_status with pid to choose one.",
                                    "several_editors", Json{{"editors", pids}});
            }
            target = pids.front();
        }
        const std::wstring name = std::wstring(Bridge::kPipePrefix) + std::to_wstring(target);
        HANDLE handle = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 3 && handle == INVALID_HANDLE_VALUE; ++attempt)
        {
            handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (handle != INVALID_HANDLE_VALUE) break;
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_BUSY) { WaitNamedPipeW(name.c_str(), 2000); continue; }
            if (error == ERROR_FILE_NOT_FOUND)
                throw Bridge::Error(NotConnected, "The editor with process id " + std::to_string(target) + " has no MCP pipe: it has closed, or its MCP server is off. " + kStartHint);
            if (error == ERROR_ACCESS_DENIED)
                throw Bridge::Error(NotConnected, "Windows refused the editor's pipe: the editor runs as another user or elevated. Run the editor and the MCP client as the same user.", "access_denied");
            throw Bridge::Error(NotConnected, "Could not open the editor's pipe: " + Describe(error) + ".");
        }
        if (handle == INVALID_HANDLE_VALUE)
            throw Bridge::Error(Busy, "Another MCP client is connected to the editor (process " + std::to_string(target) + "). Close it, or use RE+ Tools > MCP > Disconnect client.", "another_client");
        pipe = handle;
        pid = target;
        reader = Bridge::FrameReader();
        try
        {
            Write(Bridge::Hello(options ? options->client : std::string("Reloaded.Editor.Mcp"), options ? options->version : std::string()));
            const auto deadline = Clock::now() + std::chrono::milliseconds(kHelloMs);
            Json reply;
            for (;;)
            {
                if (Read(deadline, false, reply) != Waited::Done)
                    throw Bridge::Error(Timeout, "The editor did not answer the MCP handshake within " + Seconds(kHelloMs) + "; it may be stuck in a dialog.");
                if (reply.value("t", std::string()) == "event") { Aside(reply); continue; }
                break;
            }
            if (reply.value("t", std::string()) == "result" && !reply.value("ok", false)) throw FromBody(reply.value("error", Json()));
            std::string why;
            if (!Bridge::Compatible(reply, why)) throw Bridge::Error(Bridge::Code::ProtocolMismatch, why);
            hello = reply;
            Log("Connected to the editor (process " + std::to_string(pid) + ", RE+ " + reply.value("dll", std::string("?")) + ").");
        }
        catch (const Broken& e) { Close(); throw Bridge::Error(NotConnected, std::string("The editor closed the connection during the handshake: ") + e.what()); }
        catch (...) { Close(); throw; }
    }
    void Cancel(uint64_t id)
    {
        try { Write({{"t", "cancel"}, {"id", id}}); } catch (const std::exception&) {}
    }
    // Waits for call id's result; progress goes to the caller, events aside.
    Json Await(uint64_t id, const std::string& op, Clock::time_point deadline, const CallOptions& call, unsigned timeoutMs)
    {
        std::string ticket;
        Clock::time_point approvalDeadline{};
        for (;;)
        {
            Json message;
            const auto waited = Read(ticket.empty() ? deadline : approvalDeadline, true, message);
            if (waited == Waited::TimedOut)
            {
                if (!ticket.empty())
                {
                    abandoned[id] = ticket;
                    throw Bridge::Error(Timeout, "The change is waiting for the user's approval in the editor (ticket " + ticket +
                                        "). Call approval_wait with this ticket; the editor applies it once approved.", "awaiting_approval",
                                        Json{{"ticket", ticket}, {"call", id}, {"op", op}});
                }
                Cancel(id);
                abandoned[id] = "";
                throw Bridge::Error(Timeout, op + " did not finish within " + Seconds(timeoutMs) +
                                    "; the editor was asked to cancel it. Check editor_status before repeating it, since it may still have run.");
            }
            if (waited == Waited::Stopped)
            {
                Cancel(id);
                const auto grace = Clock::now() + std::chrono::milliseconds(kCancelGraceMs);
                for (;;)
                {
                    Json reply;
                    if (Read(grace, false, reply) != Waited::Done) { abandoned[id] = ""; break; }
                    if (reply.value("t", std::string()) == "result" && MessageId(reply) == id) { Remember(id, op, reply); break; }
                    Aside(reply);
                }
                throw Bridge::Error(Cancelled, op + " was cancelled; it may still have run. Check editor_status before repeating it.");
            }
            const auto t = message.value("t", std::string());
            if (t == "accepted") continue;
            if (t == "progress")
            {
                if (MessageId(message) != id) continue;
                const auto stage = message.value("stage", std::string());
                if (stage == "awaiting_approval" && ticket.empty())
                {
                    ticket = TicketText(message.value("ticket", Json(std::to_string(id))));
                    approvalDeadline = Clock::now() + std::chrono::milliseconds(kApprovalMs);
                }
                // Approved: the change runs now, with the call's whole timeout again.
                if (stage == "approved" && !ticket.empty())
                {
                    ticket.clear();
                    deadline = std::max(deadline, Clock::now() + std::chrono::milliseconds(timeoutMs));
                }
                if (call.progress) try { call.progress(message); } catch (const std::exception&) {}
                continue;
            }
            if (t == "result" && MessageId(message) == id) return message;
            Aside(message);
        }
    }
    void Remember(uint64_t id, const std::string& op, const Json& message)
    {
        if (message.contains("state") && message.at("state").is_object()) state = message.at("state");
        state["lastCall"] = LastCall(id, op, message);
    }
    // approval.status for a call this client stopped waiting for: its result
    // when it has come, else whatever the editor says about the ticket.
    bool LocalApproval(const Json& args, Json& out)
    {
        const auto ticket = TicketText(args.value("ticket", Json()));
        if (ticket.empty()) return false;
        auto finished = [&]
        {
            const auto found = late.find(ticket);
            if (found == late.end()) return false;
            const auto& m = found->second;
            out = {{"pending", false}, {"ticket", ticket}, {"ok", m.value("ok", false)}};
            if (m.contains("result")) out["result"] = m.at("result");
            if (m.contains("error")) out["error"] = m.at("error");
            if (m.contains("state")) out["state"] = m.at("state");
            Remember(MessageId(m), "approval", m);
            late.erase(found);
            return true;
        };
        if (finished()) return true;
        const bool ours = std::any_of(abandoned.begin(), abandoned.end(), [&](const auto& entry) { return entry.second == ticket; });
        if (!ours || pipe == INVALID_HANDLE_VALUE) return false;
        const auto waitMs = static_cast<unsigned>(std::clamp<long long>(args.value("waitMs", 0LL), 0, 60000));
        const auto deadline = Clock::now() + std::chrono::milliseconds(waitMs);
        while (Remaining(deadline))
        {
            Json message;
            if (Read(deadline, true, message) != Waited::Done) break;
            Aside(message);
            if (finished()) return true;
        }
        return false;
    }
};

BridgeClient::BridgeClient(Options o) : options(std::move(o)), impl(std::make_unique<Impl>())
{
    impl->options = &options;
    impl->io = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl->cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
BridgeClient::~BridgeClient()
{
    {
        std::lock_guard<std::mutex> guard(impl->lock);
        impl->Close();
    }
    if (impl->io) CloseHandle(impl->io);
    if (impl->cancel) CloseHandle(impl->cancel);
}

Json BridgeClient::Call(const std::string& op, const Json& args, const CallOptions& call)
{
    std::lock_guard<std::mutex> guard(impl->lock);
    auto& s = *impl;
    if (s.stopped) throw Bridge::Error(Cancelled, op + " was not sent: the request was cancelled.");
    Json local;
    bool ownTicket = false;
    if (op == "approval.status")
    {
        try { if (s.LocalApproval(args, local)) return local; }
        catch (const Broken&) { s.Close(); }
        const auto ticket = TicketText(args.value("ticket", Json()));
        ownTicket = std::any_of(s.abandoned.begin(), s.abandoned.end(), [&](const auto& entry) { return entry.second == ticket; });
    }
    Json sent = args.is_null() ? Json::object() : args;
    if (op == "approval.status") sent.erase("waitMs");
    const unsigned timeoutMs = call.timeoutMs ? call.timeoutMs : 120000;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    const auto* policy = Bridge::FindPolicy(op);
    const bool reading = policy && policy->policy == Bridge::Policy::Read;
    unsigned backoff = kBackoffFirstMs;
    bool retried = false;
    for (;;)
    {
        bool delivered = false;
        try
        {
            if (s.pipe == INVALID_HANDLE_VALUE) s.Connect();
            const uint64_t id = ++s.next;
            s.Write(Bridge::CallMessage(id, op, sent, call.label, call.dryRun, call.expect));
            delivered = true;
            const auto message = s.Await(id, op, deadline, call, timeoutMs);
            s.Remember(id, op, message);
            if (message.value("ok", false)) return message.contains("result") ? message.at("result") : Json();
            auto error = FromBody(message.value("error", Json()));
            if (error.code == Busy && Clock::now() + std::chrono::milliseconds(backoff) < deadline)
            {
                // The wait shows as progress, so the client sees why the call is slow.
                if (call.progress)
                    try { call.progress({{"t", "progress"}, {"id", id}, {"stage", "busy"}, {"reason", error.reason}, {"message", Guard::BusyHint(error.reason)}}); }
                    catch (const std::exception&) {}
                if (WaitForSingleObject(s.cancel, backoff) == WAIT_OBJECT_0) throw Bridge::Error(Cancelled, op + " was cancelled while the editor was busy.");
                backoff = std::min(backoff * 2, kBackoffLastMs);
                continue;
            }
            if (op == "approval.status" && ownTicket && (error.code == Bridge::Code::Refused || error.code == Bridge::Code::Invalid))
                return Json{{"pending", true}, {"ticket", TicketText(args.value("ticket", Json()))}};
            throw error;
        }
        catch (const Broken& e)
        {
            s.Close();
            if (!s.selected) s.pid = 0;
            // A call the editor may have received is only repeated when it
            // cannot have changed anything.
            if (retried || (delivered && !reading))
                throw Bridge::Error(NotConnected, delivered ? "The connection to the editor broke during " + op + " (" + e.what() +
                                    "); it may or may not have run. Check editor_status and map_query before repeating it."
                                    : std::string("Lost the connection to the editor: ") + e.what() + " " + kStartHint);
            retried = true;
        }
    }
}
Json BridgeClient::Hello()
{
    std::lock_guard<std::mutex> guard(impl->lock);
    if (impl->pipe == INVALID_HANDLE_VALUE) impl->Connect();
    return impl->hello;
}
bool BridgeClient::Connected()
{
    std::lock_guard<std::mutex> guard(impl->lock);
    return impl->pipe != INVALID_HANDLE_VALUE;
}
Json BridgeClient::State()
{
    std::lock_guard<std::mutex> guard(impl->lock);
    return impl->state;
}
Json BridgeClient::Editors()
{
    std::lock_guard<std::mutex> guard(impl->lock);
    Json out = Json::array();
    for (auto pid : Discover())
        out.push_back({{"pid", pid}, {"selected", pid == impl->selected}, {"connected", impl->pipe != INVALID_HANDLE_VALUE && pid == impl->pid}});
    return out;
}
void BridgeClient::Select(unsigned long pid)
{
    std::lock_guard<std::mutex> guard(impl->lock);
    if (impl->pipe != INVALID_HANDLE_VALUE && impl->pid != pid) impl->Close();
    impl->selected = pid;
}
void BridgeClient::Disconnect()
{
    std::lock_guard<std::mutex> guard(impl->lock);
    impl->Close();
    if (!impl->selected) impl->pid = 0;
}
void BridgeClient::Cancel()
{
    impl->stopped = true;
    if (impl->cancel) SetEvent(impl->cancel);
}
void BridgeClient::Resume()
{
    impl->stopped = false;
    if (impl->cancel) ResetEvent(impl->cancel);
}
std::vector<unsigned long> BridgeClient::Discover()
{
    std::vector<unsigned long> pids;
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileW(L"\\\\.\\pipe\\*", &data);
    if (find == INVALID_HANDLE_VALUE) return pids;
    const std::wstring prefix = std::wstring(Bridge::kPipePrefix).substr(9); // past \\.\pipe\ (9 characters)
    do
    {
        const std::wstring name = data.cFileName;
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
        const auto digits = name.substr(prefix.size());
        if (digits.size() > 10 || digits.find_first_not_of(L"0123456789") != std::wstring::npos) continue;
        const auto pid = std::stoull(digits);
        if (pid && pid <= 0xffffffffull) pids.push_back(static_cast<unsigned long>(pid));
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(pids.begin(), pids.end());
    pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    return pids;
}

Processes& Process()
{
    static Processes processes{
        [](const std::filesystem::path& exe, const std::string& arguments, const std::filesystem::path& folder) -> unsigned long
        {
            std::wstring command = L"\"" + exe.wstring() + L"\"";
            if (!arguments.empty()) command += L" " + std::wstring(arguments.begin(), arguments.end());
            std::vector<wchar_t> line(command.begin(), command.end());
            line.push_back(L'\0');
            STARTUPINFOW startup{};
            startup.cb = sizeof startup;
            PROCESS_INFORMATION process{};
            // Out of the MCP client's job, so closing the client leaves the
            // editor (and its unsaved map) alone; not every job allows it.
            DWORD flags = CREATE_BREAKAWAY_FROM_JOB | CREATE_DEFAULT_ERROR_MODE;
            BOOL ok = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, flags, nullptr, folder.c_str(), &startup, &process);
            if (!ok && GetLastError() == ERROR_ACCESS_DENIED)
            {
                flags &= ~static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB);
                ok = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, flags, nullptr, folder.c_str(), &startup, &process);
            }
            if (!ok)
            {
                const auto name = exe.filename().u8string();
                throw std::runtime_error("Could not start " + std::string(name.begin(), name.end()) + ": " + Describe(GetLastError()) + ".");
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return process.dwProcessId;
        },
        [](unsigned long pid)
        {
            const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
            const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
            CloseHandle(process);
            return alive;
        },
        [](unsigned long pid, unsigned timeoutMs)
        {
            const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (!process) return GetLastError() != ERROR_ACCESS_DENIED;
            const bool gone = WaitForSingleObject(process, timeoutMs) == WAIT_OBJECT_0;
            CloseHandle(process);
            return gone;
        }};
    return processes;
}

#else
// Not Windows: the server and tools build and run their offline parts, and
// anything that needs the editor says so.
struct BridgeClient::Impl { Json state = Json::object(); };
namespace
{
    [[noreturn]] void Unavailable()
    {
        throw Bridge::Error(Bridge::Code::NotConnected, "The editor bridge needs Windows: run Reloaded.Editor.Mcp.exe from the game's System folder, beside Reloaded_Editor.exe.");
    }
}
BridgeClient::BridgeClient(Options o) : options(std::move(o)), impl(std::make_unique<Impl>()) {}
BridgeClient::~BridgeClient() = default;
Json BridgeClient::Call(const std::string&, const Json&, const CallOptions&) { Unavailable(); }
Json BridgeClient::Hello() { Unavailable(); }
bool BridgeClient::Connected() { return false; }
Json BridgeClient::State() { return impl->state; }
Json BridgeClient::Editors() { return Json::array(); }
void BridgeClient::Select(unsigned long) {}
void BridgeClient::Disconnect() {}
void BridgeClient::Cancel() {}
void BridgeClient::Resume() {}
std::vector<unsigned long> BridgeClient::Discover() { return {}; }

Processes& Process()
{
    static Processes processes{
        [](const std::filesystem::path&, const std::string&, const std::filesystem::path&) -> unsigned long
        { throw std::runtime_error("Starting the editor needs Windows."); },
        [](unsigned long) { return false; },
        [](unsigned long, unsigned) { return true; }};
    return processes;
}
#endif
}
