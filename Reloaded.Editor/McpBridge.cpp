#include "pch.h"
#undef min
#undef max
#include "McpBridge.h"
#include "McpBridgeModel.h"
#include "McpGuardModel.h"
#include "McpPlanModel.h"
#include "McpOps.h"
#include "McpEditor.h"
#include "WorkflowEditor.h"
#include "WorkflowTools.h"
#include "MapAuthoringDialog.h"
#include "RebuildAllMaps.h"
#include "EngineLog.h"
#include "Version.h"
#include "logger.h"
#include <sddl.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

// The pipe Reloaded.Editor.Mcp.exe talks to (McpBridge.h). Two threads:
//  - the pipe thread owns the pipe: it reads frames, answers hello and
//    approval.status, refuses what the policies refuse, and writes whatever
//    the UI thread queued; it never touches the engine;
//  - the frame's thread runs each call, posted to a message-only window, so a
//    call arrives through the editor's own message loop (and any modal loop
//    inside it, which the guards turn away).
// One call is in the bridge at a time: the slot holds it from the pipe
// thread's acceptance to the UI thread's reply.
namespace
{
    namespace Bridge = Mcp::Bridge;
    namespace Guard = Mcp::Guard;
    namespace Plan = Mcp::Plan;
    namespace Editor = Workflow::Editor;
    namespace fs = std::filesystem;
    using Json = Workflow::Json;

    constexpr UINT kRun = WM_APP + 0x3C0;           // a call waits in the slot
    constexpr UINT kCancelReview = WM_APP + 0x3C1;  // the approval window closes as Cancel
    constexpr UINT kClientChanged = WM_APP + 0x3C2; // a client came or went
    constexpr DWORD kPickupMs = 10000;   // a call the frame's thread has not taken by then is answered busy
    constexpr DWORD kWriteMs = 60000;    // a client that reads nothing for this long is dropped
    constexpr DWORD kInstances = 8;      // the client, the listener, and ones closing
    constexpr size_t kLogRing = 2000, kCallLog = 200, kRecordsMax = 256, kTicketsMax = 64;
    constexpr uintptr_t kSlowTaskFlag = 0x11692EC4; // GIsSlowTask (SoundBrowser.cpp)

    // Text: the engine's is in the ANSI code page, JSON is UTF-8.
    std::wstring Wide(const std::string& text, UINT page = CP_UTF8)
    {
        if (text.empty()) return {};
        const int size = MultiByteToWideChar(page, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(std::max(size, 0)), L'\0');
        if (size > 0) MultiByteToWideChar(page, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
        return out;
    }
    std::string Narrow(const std::wstring& text, UINT page = CP_UTF8)
    {
        if (text.empty()) return {};
        const int size = WideCharToMultiByte(page, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(std::max(size, 0)), '\0');
        if (size > 0) WideCharToMultiByte(page, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
        return out;
    }
    bool ValidUtf8(const std::string& text)
    {
        for (size_t i = 0; i < text.size();)
        {
            const auto c = static_cast<unsigned char>(text[i]);
            const size_t extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : 4;
            if (extra > 3 || (extra && i + extra >= text.size())) return false;
            for (size_t k = 1; k <= extra; ++k)
                if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
            i += extra + 1;
        }
        return true;
    }
    std::string Utf8(const std::string& text) { return ValidUtf8(text) ? text : Narrow(Wide(text, CP_ACP)); }
    // Replies never fail on a stray byte in an engine string.
    std::string Dump(const Json& value, int indent = -1) { return value.dump(indent, ' ', false, Json::error_handler_t::replace); }
    bool Flag(const Json& value, const char* key) { return value.is_object() && value.contains(key) && value.at(key).is_boolean() && value.at(key).get<bool>(); }
    std::string Text(const Json& value, const char* key)
    {
        return value.is_object() && value.contains(key) && value.at(key).is_string() ? value.at(key).get<std::string>() : std::string();
    }

    fs::path SystemDirectory()
    {
        std::wstring exe(32768, L'\0');
        exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
        return fs::path(exe).parent_path();
    }
    std::wstring IniPath() { return (SystemDirectory() / L"Reloaded_Editor.ini").wstring(); }

    // [Mcp] in Reloaded_Editor.ini, read by both threads.
    std::mutex settingsLock;
    McpBridge::Settings settings;
    McpBridge::Settings LoadSettings()
    {
        const auto ini = IniPath();
        auto flag = [&](const wchar_t* key) { return GetPrivateProfileIntW(L"Mcp", key, 0, ini.c_str()) != 0; };
        McpBridge::Settings loaded;
        loaded.enabled = flag(L"Enabled");
        loaded.requireApproval = flag(L"RequireApproval");
        loaded.allowRawExec = flag(L"AllowRawExec");
        loaded.allowPlay = flag(L"AllowPlay");
        const int megabytes = static_cast<int>(GetPrivateProfileIntW(L"Mcp", L"HistoryMaxMB", 200, ini.c_str()));
        loaded.historyMaxMB = static_cast<unsigned>(std::clamp(megabytes, 1, 100000));
        return loaded;
    }
    void Store(const McpBridge::Settings& next) { std::lock_guard lock(settingsLock); settings = next; }
    void WriteFlag(const wchar_t* key, bool on) { WritePrivateProfileStringW(L"Mcp", key, on ? L"1" : L"0", IniPath().c_str()); }
    // The keys are written once, off, so the user can find the ones only the
    // INI sets (AllowRawExec, AllowPlay, HistoryMaxMB, and Ffmpeg and
    // WriteRoots, which Reloaded.Editor.Mcp.exe reads: the ffmpeg to use and
    // the folders besides its work folder it may write in, empty for none).
    void WriteMissingDefaults()
    {
        static const std::pair<const wchar_t*, const wchar_t*> defaults[] = {
            {L"Enabled", L"0"}, {L"RequireApproval", L"0"}, {L"AllowRawExec", L"0"}, {L"AllowPlay", L"0"}, {L"HistoryMaxMB", L"200"}, {L"Ffmpeg", L""}, {L"WriteRoots", L""}};
        // Returned only for a key that is not there, so an empty value counts as set.
        constexpr const wchar_t* absent = L"\x1" L"absent";
        const auto ini = IniPath();
        for (const auto& [key, value] : defaults)
        {
            wchar_t present[16] = L"";
            GetPrivateProfileStringW(L"Mcp", key, absent, present, 16, ini.c_str());
            if (!wcscmp(present, absent)) WritePrivateProfileStringW(L"Mcp", key, value, ini.c_str());
        }
    }

    // The engine's errors and warnings ("Warning: text") in a ring for
    // log.tail and each call's reply. Fed on whichever thread logs.
    std::mutex logLock;
    std::deque<McpBridge::LogLine> logRing;
    uint64_t logSeq = 0;
    void __cdecl OnEngineLog(const char* line)
    {
        if (!line || !*line) return;
        try
        {
            McpBridge::LogLine entry;
            std::string text = Utf8(line);
            const auto colon = text.find(": ");
            if (colon != std::string::npos && colon < 32)
            {
                entry.level = text.substr(0, colon);
                std::transform(entry.level.begin(), entry.level.end(), entry.level.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                text.erase(0, colon + 2);
            }
            entry.text = std::move(text);
            std::lock_guard lock(logLock);
            entry.seq = ++logSeq;
            logRing.push_back(std::move(entry));
            while (logRing.size() > kLogRing) logRing.pop_front();
        }
        catch (...) { /* Inside the engine's logging: nothing may escape. */ }
    }
    Json LogJson(uint64_t since)
    {
        Json lines = Json::array();
        for (const auto& line : McpBridge::LogSince(since, kCallLog)) lines.push_back({{"seq", line.seq}, {"level", line.level}, {"text", line.text}});
        return lines;
    }

    std::mutex recordLock;
    std::vector<McpBridge::CallRecord> records;

    // Approvals, for approval.status: a client that stopped waiting can ask
    // how the review ended and what the call returned.
    struct Ticket { std::string id; uint64_t request = 0; unsigned call = 0; std::string op, label, status; Json outcome; };
    std::mutex ticketLock;
    std::deque<Ticket> tickets;
    std::string NewTicket(uint64_t request, unsigned call, const std::string& op, const std::string& label)
    {
        static std::mt19937 random{std::random_device{}()};
        char id[32];
        snprintf(id, sizeof(id), "t%u-%08x", call, static_cast<unsigned>(random()));
        std::lock_guard lock(ticketLock);
        tickets.push_back({id, request, call, op, label, "pending", Json()});
        while (tickets.size() > kTicketsMax) tickets.pop_front();
        return id;
    }
    // An outcome given with a status the review already settled (denied,
    // stale, cancelled) keeps that status.
    void SetTicket(const std::string& id, const std::string& status, const Json& outcome = Json())
    {
        std::lock_guard lock(ticketLock);
        for (auto& ticket : tickets)
            if (ticket.id == id)
            {
                if (!outcome.is_null()) ticket.outcome = outcome;
                if (outcome.is_null() || ticket.status == "pending" || ticket.status == "approved") ticket.status = status;
            }
    }
    Json TicketStatus(const Json& args)
    {
        const auto wanted = Text(args, "ticket");
        std::lock_guard lock(ticketLock);
        if (tickets.empty()) throw Bridge::Error(Bridge::Code::Invalid, "No change has asked for approval in this editor session.", "no_ticket");
        const Ticket* found = wanted.empty() ? &tickets.back() : nullptr;
        for (const auto& ticket : tickets) if (ticket.id == wanted) found = &ticket;
        if (!found) throw Bridge::Error(Bridge::Code::Invalid, "Unknown approval ticket \"" + wanted.substr(0, 40) + "\"; the editor keeps the last " + std::to_string(kTicketsMax) + ".", "unknown_ticket");
        Json status{{"ticket", found->id}, {"id", found->request}, {"call", found->call}, {"op", found->op}, {"label", found->label}, {"status", found->status}};
        if (!found->outcome.is_null()) status["outcome"] = found->outcome;
        return status;
    }

    // The state block of the last call, for replies the pipe thread gives
    // itself while the frame's thread is busy.
    std::mutex stateLock;
    Json cachedState;
    Json CachedState(const std::string& busy)
    {
        std::lock_guard lock(stateLock);
        Json state = cachedState.is_object() ? cachedState : Json{{"open", nullptr}};
        state["cached"] = true;
        if (!busy.empty()) state["busy"] = busy;
        return state;
    }

    // A plan of reads only is a read: no approval, no stale check, no history.
    const Bridge::OpPolicy kReadPlan{"mcp.plan", Bridge::Policy::Read, false};
    bool ReadOnlyPlan(const Json& args)
    {
        try
        {
            Json plan = args;
            plan.erase("op");
            return !Plan::Mutates(Plan::Parse(plan));
        }
        catch (const std::exception&) { return false; } // mcp.plan itself says what is wrong.
    }

    // The call in the bridge, and the frames waiting for the pipe thread.
    enum class Stage { Queued, Running, Approval };
    struct PendingCall
    {
        uint64_t id = 0, client = 0;
        std::string op, label, clientName;
        Json args = Json::object();
        bool dryRun = false;
        Bridge::Expect expect;
        const Bridge::OpPolicy* policy = nullptr;
        DWORD queuedAt = 0;
        Stage stage = Stage::Queued;   // under callLock
        bool cancelRequested = false;  // under callLock
    };
    struct Outgoing { uint64_t client = 0; std::string frame; };
    std::mutex callLock;
    std::shared_ptr<PendingCall> slot;
    std::deque<Outgoing> outbox;
    HANDLE stopEvent = nullptr, wakeEvent = nullptr;
    std::atomic<HWND> bridgeWindow{nullptr};
    std::atomic<uint64_t> activeClient{0}, clientCounter{0};
    std::atomic<bool> dropRequested{false};

    // The frame's thread only.
    HWND frameWindow = nullptr;
    HANDLE serverThread = nullptr;
    bool stopRequested = false, reviewing = false;
    int depth = 0;
    unsigned callCounter = 0;

    // A message as a frame; one too large for a frame becomes an error saying so.
    std::string Frame(const Json& message)
    {
        try { return Bridge::EncodeFrame(Dump(message)); }
        catch (const std::exception& e)
        {
            const auto id = message.contains("id") && message.at("id").is_number_unsigned() ? message.at("id").get<uint64_t>() : uint64_t{0};
            const auto error = Bridge::ErrorBody(Bridge::Code::EditorError, std::string(e.what()) + " Ask for less (a limit, fewer fields) or for a blob.", "too_large");
            return Bridge::EncodeFrame(Dump(Bridge::ErrorMessage(id, error, message.value("state", Json()))));
        }
    }
    // From the frame's thread: a reply or progress for the pipe thread to write.
    void Push(uint64_t client, const Json& message)
    {
        auto frame = Frame(message);
        {
            std::lock_guard lock(callLock);
            outbox.push_back({client, std::move(frame)});
        }
        if (wakeEvent) SetEvent(wakeEvent);
    }
    void Notify(UINT message)
    {
        if (HWND window = bridgeWindow.load()) PostMessageW(window, message, 0, 0);
    }

    // One pipe instance and its overlapped connect or read. Kept at a fixed
    // address (unique_ptr) while I/O is outstanding.
    struct Pipe
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped{};
        bool pending = false;
        DWORD deadline = 0;
        std::vector<char> buffer;
        Pipe() { overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
        ~Pipe()
        {
            if (handle != INVALID_HANDLE_VALUE)
            {
                // The kernel writes into overlapped and buffer until the read ends.
                if (pending)
                {
                    DWORD bytes = 0;
                    CancelIoEx(handle, &overlapped);
                    GetOverlappedResult(handle, &overlapped, &bytes, TRUE);
                }
                DisconnectNamedPipe(handle);
                CloseHandle(handle);
            }
            if (overlapped.hEvent) CloseHandle(overlapped.hEvent);
        }
        Pipe(const Pipe&) = delete;
        Pipe& operator=(const Pipe&) = delete;
    };

    class Server
    {
    public:
        ~Server()
        {
            client.reset(); listener.reset(); rejected.clear();
            if (writeEvent) CloseHandle(writeEvent);
            if (descriptor) LocalFree(descriptor);
        }
        void Run()
        {
            name = Bridge::kPipePrefix + std::to_wstring(GetCurrentProcessId());
            writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!writeEvent || !Secure()) return;
            if (!Listen(true))
            {
                Logger::log("MCP: the bridge pipe could not be created, error " + std::to_string(failure) +
                            " (another program may hold " + Narrow(name) + ")");
                return;
            }
            Logger::log(L"MCP: the bridge is listening on " + name);
            for (;;)
            {
                HANDLE waits[4];
                DWORD count = 0;
                waits[count++] = stopEvent;
                waits[count++] = wakeEvent;
                if (listener) waits[count++] = listener->overlapped.hEvent;
                if (client && client->pending) waits[count++] = client->overlapped.hEvent;
                if (WaitForMultipleObjects(count, waits, FALSE, 250) == WAIT_FAILED)
                {
                    Logger::log("MCP: the bridge stopped waiting, error " + std::to_string(GetLastError()));
                    break;
                }
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
                if (listener && WaitForSingleObject(listener->overlapped.hEvent, 0) == WAIT_OBJECT_0) Connected();
                if (client && client->pending && WaitForSingleObject(client->overlapped.hEvent, 0) == WAIT_OBJECT_0) Received();
                if (dropRequested.exchange(false)) Drop("disconnected from the editor's menu", true);
                Flush();
                Tick();
            }
            // The last replies (editor.quit's) go out, and the client has a
            // moment to read them before the instances close.
            Flush();
            Drop("the server was turned off", true);
            while (!rejected.empty())
            {
                Expire();
                if (!rejected.empty()) Sleep(50);
            }
        }

    private:
        std::wstring name;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        SECURITY_ATTRIBUTES attributes{};
        std::unique_ptr<Pipe> listener, client;
        std::vector<std::unique_ptr<Pipe>> rejected;
        HANDLE writeEvent = nullptr;
        Bridge::FrameReader reader;
        uint64_t serial = 0;
        bool greeted = false;
        std::string clientName;
        DWORD retryAt = 0; // 0: now
        DWORD failure = 0; // why the last Listen failed

        // Only the user running the editor may open the pipe.
        bool Secure()
        {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) { Logger::log("MCP: cannot read the editor's user"); return false; }
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            std::vector<unsigned char> user(size ? size : 1);
            LPWSTR sid = nullptr;
            const bool known = size && GetTokenInformation(token, TokenUser, user.data(), size, &size)
                && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid);
            CloseHandle(token);
            if (!known) { Logger::log("MCP: cannot read the editor's user"); return false; }
            const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
            LocalFree(sid);
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            {
                Logger::log("MCP: cannot build the pipe's security, error " + std::to_string(GetLastError()));
                return false;
            }
            attributes.nLength = sizeof(attributes);
            attributes.lpSecurityDescriptor = descriptor;
            attributes.bInheritHandle = FALSE;
            return true;
        }
        // A new instance waiting for a client. The first claims the name, so
        // a program that created it before the editor cannot pose as it.
        bool Listen(bool first)
        {
            auto pipe = std::make_unique<Pipe>();
            if (!pipe->overlapped.hEvent) return false;
            pipe->handle = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                            kInstances, 1 << 16, 1 << 16, 0, &attributes);
            if (pipe->handle == INVALID_HANDLE_VALUE) { failure = GetLastError(); return false; }
            if (!ConnectNamedPipe(pipe->handle, &pipe->overlapped))
            {
                const DWORD error = GetLastError();
                if (error == ERROR_IO_PENDING) pipe->pending = true;
                else if (error == ERROR_PIPE_CONNECTED) SetEvent(pipe->overlapped.hEvent);
                else { failure = error; return false; }
            }
            else SetEvent(pipe->overlapped.hEvent);
            listener = std::move(pipe);
            return true;
        }
        void Connected()
        {
            auto pipe = std::move(listener);
            DWORD bytes = 0;
            bool ok = true;
            if (pipe->pending) ok = GetOverlappedResult(pipe->handle, &pipe->overlapped, &bytes, FALSE) != 0;
            pipe->pending = false;
            ResetEvent(pipe->overlapped.hEvent);
            if (!Listen(false)) retryAt = (GetTickCount() + 1000) | 1;
            if (!ok) return; // It went before it was seen.
            if (client) Reject(std::move(pipe));
            else Accept(std::move(pipe));
        }
        void Accept(std::unique_ptr<Pipe> pipe)
        {
            client = std::move(pipe);
            client->buffer.resize(1 << 16);
            reader = Bridge::FrameReader();
            greeted = false;
            clientName.clear();
            serial = ++clientCounter;
            activeClient = serial;
            ULONG pid = 0;
            GetNamedPipeClientProcessId(client->handle, &pid);
            Logger::log("MCP: a client connected (process " + std::to_string(pid) + ")");
            Notify(kClientChanged);
            Read();
        }
        // One client at a time: the next is told so, given a moment to read
        // it, then closed.
        void Reject(std::unique_ptr<Pipe> pipe)
        {
            const auto error = Bridge::ErrorBody(Bridge::Code::Busy,
                "Another MCP client is using this editor. Close it, or choose RE+ Tools > MCP > Disconnect client in the editor.", "another_client");
            Write(*pipe, Bridge::EncodeFrame(Dump(Bridge::ErrorMessage(0, error))), 1000);
            Linger(std::move(pipe));
            Logger::log("MCP: turned away a second client");
        }
        // Closing an instance discards what its client has not read yet, so
        // one told why it is closed stays open a moment first.
        void Linger(std::unique_ptr<Pipe> pipe)
        {
            if (pipe->pending)
            {
                DWORD bytes = 0;
                CancelIoEx(pipe->handle, &pipe->overlapped);
                GetOverlappedResult(pipe->handle, &pipe->overlapped, &bytes, TRUE);
                pipe->pending = false;
            }
            pipe->deadline = GetTickCount() + 2000;
            rejected.push_back(std::move(pipe));
        }
        void Expire()
        {
            const DWORD now = GetTickCount();
            rejected.erase(std::remove_if(rejected.begin(), rejected.end(), [&](const std::unique_ptr<Pipe>& pipe) { return static_cast<int>(now - pipe->deadline) >= 0; }),
                           rejected.end());
        }
        void Read()
        {
            if (!client) return;
            if (ReadFile(client->handle, client->buffer.data(), static_cast<DWORD>(client->buffer.size()), nullptr, &client->overlapped)
                || GetLastError() == ERROR_IO_PENDING)
            {
                client->pending = true;
                return;
            }
            Drop("the pipe broke");
        }
        void Received()
        {
            DWORD bytes = 0;
            const BOOL ok = GetOverlappedResult(client->handle, &client->overlapped, &bytes, FALSE);
            if (!ok && GetLastError() == ERROR_IO_INCOMPLETE) return;
            client->pending = false;
            if (!ok) { Drop("the client closed the pipe"); return; }
            reader.Append(client->buffer.data(), bytes);
            try
            {
                std::string frame;
                while (client && reader.Next(frame)) Handle(frame);
            }
            catch (const std::exception& e)
            {
                // A length the stream cannot be resynchronised after.
                Send(Bridge::ErrorMessage(0, Bridge::ErrorBody(Bridge::Code::Invalid, e.what(), "frame")));
                Drop("it sent a frame over the limit", true);
                return;
            }
            if (client) Read();
        }
        bool Write(Pipe& pipe, const std::string& frame, DWORD limit)
        {
            OVERLAPPED overlapped{};
            overlapped.hEvent = writeEvent;
            ResetEvent(writeEvent);
            if (!WriteFile(pipe.handle, frame.data(), static_cast<DWORD>(frame.size()), nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING) return false;
            const DWORD started = GetTickCount();
            while (WaitForSingleObject(writeEvent, 250) != WAIT_OBJECT_0)
            {
                const bool stopping = WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0;
                if (GetTickCount() - started >= (stopping ? std::min<DWORD>(limit, 2000) : limit))
                {
                    DWORD ignored = 0;
                    CancelIoEx(pipe.handle, &overlapped);
                    GetOverlappedResult(pipe.handle, &overlapped, &ignored, TRUE);
                    return false;
                }
            }
            DWORD written = 0;
            return GetOverlappedResult(pipe.handle, &overlapped, &written, FALSE) && written == frame.size();
        }
        void Send(const Json& message)
        {
            if (client && !Write(*client, Frame(message), kWriteMs)) Drop("it stopped reading");
        }
        void Flush()
        {
            std::deque<Outgoing> pending;
            {
                std::lock_guard lock(callLock);
                pending.swap(outbox);
            }
            for (const auto& out : pending)
                if (client && out.client == serial && !Write(*client, out.frame, kWriteMs)) Drop("it stopped reading");
        }
        void Drop(const char* why, bool linger = false)
        {
            if (!client) return;
            if (linger) Linger(std::move(client));
            client.reset();
            activeClient = 0;
            greeted = false;
            reader = Bridge::FrameReader();
            Logger::log(std::string("MCP: the client disconnected: ") + why);
            {
                // A call it was waiting for: not started, it never runs; in
                // review, the review closes (kClientChanged on the frame's thread).
                std::lock_guard lock(callLock);
                if (slot && slot->client == serial)
                {
                    if (slot->stage == Stage::Queued) slot.reset();
                    else if (slot->stage == Stage::Approval) slot->cancelRequested = true;
                }
            }
            Notify(kClientChanged);
        }
        void Tick()
        {
            const DWORD now = GetTickCount();
            Expire();
            if (!listener && (!retryAt || static_cast<int>(now - retryAt) >= 0)) retryAt = Listen(false) ? 0 : (now + 1000) | 1;
            // A call the frame's thread did not take: it is stuck in something
            // long that does not pump messages (a load, a build).
            std::shared_ptr<PendingCall> late;
            {
                std::lock_guard lock(callLock);
                if (slot && slot->stage == Stage::Queued && now - slot->queuedAt > kPickupMs) { late = slot; slot.reset(); }
            }
            if (!late || late->client != serial) return;
            if (late->op == "editor.state") { const auto state = CachedState("slow_task"); Send(Bridge::ResultMessage(late->id, state, state)); return; }
            Send(Bridge::ErrorMessage(late->id, Bridge::ErrorBody(Bridge::Code::Busy, "The editor did not take the call within 10 seconds: " + Guard::BusyHint("slow_task"), "slow_task"),
                                      CachedState("slow_task")));
        }

        void Handle(const std::string& frame)
        {
            uint64_t id = 0;
            try
            {
                const auto message = Bridge::Parse(frame);
                if (!message.is_object()) throw Bridge::Error(Bridge::Code::Invalid, "A bridge message is a JSON object.");
                if (message.contains("id") && message.at("id").is_number_unsigned()) id = message.at("id").get<uint64_t>();
                const auto type = Text(message, "t");
                if (type == "hello") Greet(message);
                else if (type == "call") Call(message, id);
                else if (type == "cancel") Cancel(id);
                else throw Bridge::Error(Bridge::Code::Invalid, "Unknown message \"" + type.substr(0, 40) + "\": the bridge takes hello, call and cancel.");
            }
            catch (const Bridge::Error& e) { Send(Bridge::ErrorMessage(id, Bridge::ErrorBody(e.code, e.what(), e.reason, e.details))); }
            catch (const std::exception& e) { Send(Bridge::ErrorMessage(id, Bridge::ErrorBody(Bridge::Code::Invalid, std::string("The bridge could not read the message: ") + e.what()))); }
        }
        void Greet(const Json& message)
        {
            std::string why;
            if (!Bridge::Compatible(message, why)) { Send(Bridge::ErrorMessage(0, Bridge::ErrorBody(Bridge::Code::ProtocolMismatch, why))); return; }
            greeted = true;
            clientName = Text(message, "client").substr(0, 80);
            const auto current = McpBridge::Current();
            auto reply = Bridge::HelloReply(RE_PLUS_VERSION, GetCurrentProcessId(), current.enabled, current.requireApproval, Bridge::OpList());
            reply["allowRawExec"] = current.allowRawExec;
            reply["allowPlay"] = current.allowPlay;
            {
                std::lock_guard lock(stateLock);
                if (cachedState.is_object()) reply["state"] = cachedState;
            }
            Send(reply);
            Logger::log("MCP: hello from " + (clientName.empty() ? std::string("a client") : clientName) + " " + Text(message, "version").substr(0, 40));
        }
        void Call(const Json& message, uint64_t id)
        {
            if (!greeted) throw Bridge::Error(Bridge::Code::ProtocolMismatch, "Send hello before any call.");
            if (!message.contains("op") || !message.at("op").is_string()) throw Bridge::Error(Bridge::Code::Invalid, "A call needs an \"op\".");
            auto call = std::make_shared<PendingCall>();
            call->id = id;
            call->client = serial;
            call->clientName = clientName;
            call->op = message.at("op").get<std::string>();
            if (message.contains("args") && !message.at("args").is_null()) call->args = message.at("args");
            if (!call->args.is_object()) throw Bridge::Error(Bridge::Code::Invalid, "A call's \"args\" is a JSON object.");
            call->label = Text(message, "label").substr(0, 200);
            call->dryRun = Flag(message, "dryRun") || (call->op == "mcp.plan" && Flag(call->args, "dryRun"));
            call->expect = Bridge::ReadExpect(message);
            const auto current = McpBridge::Current();
            if (!current.enabled) throw Bridge::Error(Bridge::Code::Disabled, "The MCP server is off in this editor: RE+ Tools > MCP > Enable MCP server.");
            // answers: approval.status (on the pipe thread, from the tickets)
            if (call->op == "approval.status") { Send(Bridge::ResultMessage(id, TicketStatus(call->args))); return; }
            call->policy = Bridge::FindPolicy(call->op);
            if (!call->policy) throw Bridge::Error(Bridge::Code::Refused, "The bridge has no op \"" + call->op.substr(0, 80) + "\".", "unknown_op");
            if (call->policy->policy == Bridge::Policy::Blocked)
                throw Bridge::Error(Bridge::Code::Refused, call->op + " is not available over the bridge: it opens a window in the editor or changes the map without the checks the other ops make.", "blocked");
            if (call->policy->policy == Bridge::Policy::Raw && !current.allowRawExec)
                throw Bridge::Error(Bridge::Code::Refused, "Raw editor commands are off. The user can allow them with [Mcp] AllowRawExec=1 in Reloaded_Editor.ini.", "raw_exec_off");
            if (call->op == "play.level" && !current.allowPlay)
                throw Bridge::Error(Bridge::Code::Refused, "Play Level over the bridge is off. The user can allow it with [Mcp] AllowPlay=1 in Reloaded_Editor.ini.", "play_off");
            if (call->op == "mcp.plan" && ReadOnlyPlan(call->args)) call->policy = &kReadPlan;
            std::optional<Json> answer;
            {
                std::lock_guard lock(callLock);
                if (slot)
                {
                    // Busy, as the guards would say on the frame's thread;
                    // the state and the log are answered from here.
                    const auto reason = slot->stage == Stage::Approval ? std::string("approval_pending") : std::string("in_flight");
                    if (call->op == "editor.state") { const auto state = CachedState(reason); answer = Bridge::ResultMessage(id, state, state); }
                    else if (call->op == "log.tail")
                    {
                        Json lines = Json::array();
                        const auto max = static_cast<size_t>(std::clamp<int64_t>(call->args.value("max", int64_t{500}), 1, 2000));
                        for (const auto& line : McpBridge::LogSince(call->args.value("since", uint64_t{0}), max)) lines.push_back({{"seq", line.seq}, {"level", line.level}, {"text", line.text}});
                        answer = Bridge::ResultMessage(id, Json{{"lines", lines}, {"last", McpBridge::LogLast()}});
                    }
                    else answer = Bridge::ErrorMessage(id, Bridge::ErrorBody(Bridge::Code::Busy, Guard::BusyHint(reason), reason), CachedState(reason));
                }
                else
                {
                    call->queuedAt = GetTickCount();
                    slot = call;
                }
            }
            if (answer) { Send(*answer); return; }
            HWND window = bridgeWindow.load();
            if (!window || !PostMessageW(window, kRun, 0, 0))
            {
                {
                    std::lock_guard lock(callLock);
                    if (slot == call) slot.reset();
                }
                throw Bridge::Error(Bridge::Code::EditorError, "The editor's bridge window is gone; the editor may be closing.", "closing");
            }
        }
        void Cancel(uint64_t id)
        {
            bool dropped = false, review = false;
            {
                std::lock_guard lock(callLock);
                if (slot && slot->id == id && slot->client == serial)
                {
                    if (slot->stage == Stage::Queued) { slot.reset(); dropped = true; }
                    else if (slot->stage == Stage::Approval) { slot->cancelRequested = true; review = true; }
                }
            }
            // A running call cannot be stopped; its result still comes.
            if (dropped) Send(Bridge::ErrorMessage(id, Bridge::ErrorBody(Bridge::Code::Cancelled, "The call was cancelled before it ran.")));
            if (review) Notify(kCancelReview);
        }
    };
    DWORD WINAPI ServerThread(LPVOID)
    {
        try
        {
            Server server;
            server.Run();
        }
        catch (const std::exception& e) { Logger::log(std::string("MCP: the bridge stopped: ") + e.what()); }
        activeClient = 0;
        Notify(kClientChanged);
        return 0;
    }

    // From here down: the frame's thread.
    HWND Frame() { return frameWindow && IsWindow(frameWindow) ? frameWindow : Editor::MainWindow(); }

    void StartServer()
    {
        if (!bridgeWindow.load()) return; // AttachUiThread starts it.
        if (serverThread)
        {
            if (WaitForSingleObject(serverThread, 0) == WAIT_TIMEOUT)
            {
                if (!stopRequested) return;
                // A server turned off a moment ago still holds the pipe's name.
                if (WaitForSingleObject(serverThread, 3000) == WAIT_TIMEOUT)
                {
                    Logger::log("MCP: the previous bridge thread has not ended; turn the server on again in a moment");
                    return;
                }
            }
            CloseHandle(serverThread);
            serverThread = nullptr;
        }
        if (!stopEvent) stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!wakeEvent) wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!stopEvent || !wakeEvent) { Logger::log("MCP: cannot create the bridge's events"); return; }
        ResetEvent(stopEvent);
        stopRequested = false;
        dropRequested = false;
        std::error_code error;
        fs::create_directories(McpBridge::Directory() / "blobs", error);
        fs::create_directories(McpBridge::Directory() / "history", error);
        serverThread = CreateThread(nullptr, 0, ServerThread, nullptr, 0, nullptr);
        if (!serverThread) Logger::log("MCP: cannot start the bridge thread, error " + std::to_string(GetLastError()));
    }
    // Without waiting: the thread ends on its own once it sees the event.
    void StopServer()
    {
        if (serverThread && !stopRequested)
        {
            stopRequested = true;
            SetEvent(stopEvent);
        }
        bool review = false;
        {
            std::lock_guard lock(callLock);
            if (slot && slot->stage == Stage::Queued) slot.reset();
            else if (slot && slot->stage == Stage::Approval) { slot->cancelRequested = true; review = true; }
        }
        if (review && reviewing) MapAuthoringDialog::CancelReview();
    }

    // The map as expect names it: ContentRevision, so a selection made since
    // the caller read the map does not make it stale.
    struct MapMark
    {
        bool open = false;
        uint64_t level = 0;
        unsigned generation = 0, revision = 0;
        bool operator==(const MapMark& other) const { return open == other.open && level == other.level && generation == other.generation && revision == other.revision; }
    };
    MapMark MapNow()
    {
        MapMark mark;
        try
        {
            mark.level = static_cast<uint64_t>(Editor::LevelIdentity());
            mark.generation = Editor::MapGeneration();
            mark.revision = Editor::ContentRevision();
            mark.open = true;
        }
        catch (const std::exception&) { mark = {}; }
        return mark;
    }
    Json MarkJson(uint64_t level, unsigned generation, unsigned revision) { return {{"level", level}, {"generation", generation}, {"revision", revision}}; }
    void CheckExpect(const Bridge::Expect& expect)
    {
        if (!expect.set) return;
        const auto now = MapNow();
        if (now.open && now.level == expect.level && now.generation == expect.generation && now.revision == expect.revision) return;
        Json details{{"expected", MarkJson(expect.level, expect.generation, expect.revision)}};
        if (now.open) details["current"] = MarkJson(now.level, now.generation, now.revision);
        const bool sameMap = now.open && now.level == expect.level && now.generation == expect.generation;
        throw Bridge::Error(Bridge::Code::Stale, sameMap ? "The map was edited since the caller read it. Read it again, then retry."
                                                         : "A different map is open than when the caller read it. Read it again, then retry.",
                            sameMap ? "map_edited" : "map_replaced", details);
    }

    // The state block every reply carries, kept for the pipe thread. expect is
    // what a caller passes back to be refused when the map moves on.
    Json RefreshState()
    {
        Json state;
        try { state = WorkflowTools::Request(Json{{"op", "editor.state"}}); }
        catch (const std::exception& e) { state = {{"open", false}, {"reason", Utf8(e.what())}}; }
        if (!state.is_object()) state = {{"open", nullptr}, {"value", state}};
        if (Flag(state, "open") && state.contains("level") && state.contains("generation") && state.contains("contentRevision"))
            state["expect"] = {{"level", state.at("level")}, {"generation", state.at("generation")}, {"revision", state.at("contentRevision")}};
        std::lock_guard lock(stateLock);
        cachedState = state;
        return state;
    }

    bool SlowTask()
    {
        int value = 0;
        SIZE_T read = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(kSlowTaskFlag), &value, sizeof(value), &read) && read == sizeof(value) && value != 0;
    }
    // The update offer and the crash-recovery question come up at start-up,
    // from other threads than the frame's, and want the user's answer first.
    BOOL CALLBACK FindStartupWindow(HWND window, LPARAM parameter)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(window)) return TRUE;
        wchar_t title[96] = L"";
        DWORD_PTR copied = 0;
        if (!SendMessageTimeoutW(window, WM_GETTEXT, 96, reinterpret_cast<LPARAM>(title), SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied)) return TRUE;
        for (const wchar_t* known : {L"" RE_PLUS_NAME L" Update", L"" RE_PLUS_NAME L" - Recover after a crash"})
            if (!wcscmp(title, known)) { *reinterpret_cast<std::wstring*>(parameter) = title; return FALSE; }
        return TRUE;
    }
    std::string StartupDialog()
    {
        std::wstring found;
        EnumWindows(FindStartupWindow, reinterpret_cast<LPARAM>(&found));
        return Narrow(found);
    }
    Guard::Inputs GuardInputs()
    {
        Guard::Inputs in;
        in.depth = depth;
        in.approvalPending = reviewing;
        const HWND owner = Frame();
        in.frameDisabled = owner && !IsWindowEnabled(owner);
        in.minimised = owner && IsIconic(owner);
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (GetGUIThreadInfo(GetCurrentThreadId(), &info))
        {
            in.menuMode = (info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) != 0;
            in.moveSize = (info.flags & GUI_INMOVESIZE) != 0;
        }
        in.captured = GetCapture() != nullptr;
        try { in.transactionOpen = Editor::UndoRecords().open; }
        catch (const std::exception&) { /* No engine yet: no transaction either. */ }
        in.rebuilding = RebuildAllMaps::Running();
        in.slowTask = SlowTask();
        in.startupDialog = StartupDialog();
        return in;
    }

    // The dialog watchdog: a #32770 dialog that opens during a call is
    // answered, so the call never waits on a question nobody sees. A known
    // notice gets its button; anything else the most cautious one, and the
    // call fails as dialog_blocked. Only windows the call created count (the
    // RE+ tool panels are dialogs too), and never a progress window: its
    // Cancel would stop a build.
    struct KnownDialog { const wchar_t* text; const wchar_t* button; };
    const KnownDialog kKnownDialogs[] = {
        {L"was damaged by an older version of the editor", L"ok"}, // LightmapFix's notice when no damage sink is set
    };
    struct Watch { Json dialogs = Json::array(); bool blocked = false; std::set<HWND> created, seen; };
    Watch* watch = nullptr;
    HHOOK cbtHook = nullptr;
    struct Control { HWND window = nullptr; int id = 0; std::wstring label; };
    struct Controls { std::vector<Control> buttons; std::wstring text; bool progress = false; };
    std::wstring WindowText(HWND window)
    {
        std::wstring text(static_cast<size_t>(std::max(GetWindowTextLengthW(window), 0)) + 1, L'\0');
        text.resize(static_cast<size_t>(std::max(GetWindowTextW(window, text.data(), static_cast<int>(text.size())), 0)));
        return text;
    }
    // "&Cancel" -> "cancel".
    std::wstring Plain(std::wstring label)
    {
        label.erase(std::remove(label.begin(), label.end(), L'&'), label.end());
        for (auto& c : label) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        while (!label.empty() && label.back() == L' ') label.pop_back();
        while (!label.empty() && label.front() == L' ') label.erase(label.begin());
        return label;
    }
    BOOL CALLBACK CollectControl(HWND child, LPARAM parameter)
    {
        auto& controls = *reinterpret_cast<Controls*>(parameter);
        wchar_t type[32] = L"";
        GetClassNameW(child, type, 32);
        if (!_wcsicmp(type, L"msctls_progress32")) controls.progress = true;
        else if (!_wcsicmp(type, L"Static"))
        {
            const auto text = WindowText(child);
            if (!text.empty()) controls.text += (controls.text.empty() ? L"" : L"\n") + text;
        }
        else if (!_wcsicmp(type, L"Button"))
        {
            // Its own style: the dialog itself is not shown yet when it activates.
            const auto style = GetWindowLongW(child, GWL_STYLE);
            const auto kind = style & BS_TYPEMASK;
            if ((style & WS_VISIBLE) && !(style & WS_DISABLED) && (kind == BS_PUSHBUTTON || kind == BS_DEFPUSHBUTTON))
                controls.buttons.push_back({child, GetDlgCtrlID(child), WindowText(child)});
        }
        return TRUE;
    }
    void Answer(HWND dialog)
    {
        wchar_t type[16] = L"";
        GetClassNameW(dialog, type, 16);
        if (wcscmp(type, L"#32770") || !watch->created.count(dialog) || !watch->seen.insert(dialog).second) return;
        Controls controls;
        EnumChildWindows(dialog, CollectControl, reinterpret_cast<LPARAM>(&controls));
        if (controls.progress) return;
        auto button = [&](const wchar_t* label) -> const Control*
        {
            for (const auto& b : controls.buttons) if (Plain(b.label) == label) return &b;
            return nullptr;
        };
        const Control* chosen = nullptr;
        bool known = false;
        for (const auto& entry : kKnownDialogs)
            if (controls.text.find(entry.text) != std::wstring::npos && (chosen = button(entry.button))) { known = true; break; }
        for (const wchar_t* label : {L"cancel", L"no", L"close", L"ok"})
            if (!chosen) chosen = button(label);
        for (const auto& b : controls.buttons) if (!chosen && b.id == IDCANCEL) chosen = &b;
        if (!chosen && !controls.buttons.empty()) chosen = &controls.buttons.front();
        if (chosen) PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(chosen->id, BN_CLICKED), reinterpret_cast<LPARAM>(chosen->window));
        else PostMessageW(dialog, WM_CLOSE, 0, 0);
        const auto title = Narrow(WindowText(dialog)), text = Narrow(controls.text), pressed = chosen ? Narrow(Plain(chosen->label)) : std::string("(closed)");
        watch->dialogs.push_back({{"title", title}, {"text", text.substr(0, 2000)}, {"button", pressed}, {"known", known}});
        if (!known) watch->blocked = true;
        Logger::log("MCP: answered the dialog \"" + title + "\" with " + pressed + (known ? "" : " (unknown dialog: the call fails)"));
    }
    LRESULT CALLBACK CbtProc(int code, WPARAM w, LPARAM l)
    {
        if (watch)
            try
            {
                if (code == HCBT_CREATEWND) watch->created.insert(reinterpret_cast<HWND>(w));
                else if (code == HCBT_DESTROYWND) watch->created.erase(reinterpret_cast<HWND>(w));
                else if (code == HCBT_ACTIVATE) Answer(reinterpret_cast<HWND>(w));
            }
            catch (...) { /* A hook must return to Windows. */ }
        return CallNextHookEx(nullptr, code, w, l);
    }
    struct Watchdog
    {
        Watch* outer;
        explicit Watchdog(Watch& state) : outer(watch)
        {
            watch = &state;
            if (!cbtHook) cbtHook = SetWindowsHookExW(WH_CBT, CbtProc, nullptr, GetCurrentThreadId());
        }
        ~Watchdog()
        {
            watch = outer;
            if (!watch && cbtHook) { UnhookWindowsHookEx(cbtHook); cbtHook = nullptr; }
        }
        Watchdog(const Watchdog&) = delete;
        Watchdog& operator=(const Watchdog&) = delete;
    };
    struct TransactionPrefix
    {
        explicit TransactionPrefix(const std::string& prefix) { Editor::SetTransactionPrefix(prefix); }
        ~TransactionPrefix() { Editor::SetTransactionPrefix({}); }
        TransactionPrefix(const TransactionPrefix&) = delete;
        TransactionPrefix& operator=(const TransactionPrefix&) = delete;
    };

    // An exception as a bridge error. Errors that know their code carry it;
    // a few plain ones say what they are in their first words.
    struct Failure { std::string code = Bridge::Code::EditorError, message, reason; Json details; };
    Failure Classify(const std::string& op)
    {
        Failure failure;
        try { throw; }
        catch (const Bridge::Error& e)
        {
            failure.code = e.code; failure.message = Utf8(e.what()); failure.reason = e.reason; failure.details = e.details;
            return failure;
        }
        catch (const nlohmann::json::exception& e)
        {
            failure.code = Bridge::Code::Invalid;
            failure.message = op + ": the arguments do not have the shape it takes (" + Utf8(e.what()) + ").";
            failure.reason = "arguments";
            return failure;
        }
        catch (const std::bad_alloc&)
        {
            failure.message = "The editor ran out of memory during " + op + ". Save the map and restart the editor.";
            failure.reason = "memory";
            return failure;
        }
        catch (const std::exception& e) { failure.message = Utf8(e.what()); }
        catch (...) { failure.message = op + " failed with an error the editor could not describe."; return failure; }
        static const std::pair<const char*, const char*> prefixes[] = {
            {"Stale:", Bridge::Code::Stale}, {"Invalid:", Bridge::Code::Invalid}, {"Refused:", Bridge::Code::Refused},
            {"Denied:", Bridge::Code::Denied}, {"Busy:", Bridge::Code::Busy}, {"Timeout:", Bridge::Code::Timeout}};
        for (const auto& [prefix, code] : prefixes)
            if (failure.message.rfind(prefix, 0) == 0)
            {
                failure.code = code;
                failure.message.erase(0, strlen(prefix));
                while (!failure.message.empty() && failure.message.front() == ' ') failure.message.erase(failure.message.begin());
                return failure;
            }
        // The snapshot checks of the older ops (MapAuthoringNative.inl, the workbench).
        if (failure.message.find("changed since") != std::string::npos || failure.message.rfind("The map changed", 0) == 0) failure.code = Bridge::Code::Stale;
        else if (failure.message.rfind("Finish the current editor operation first", 0) == 0) { failure.code = Bridge::Code::Busy; failure.reason = "transaction"; }
        else if (failure.message == "Unknown workflow request.") failure.code = Bridge::Code::Invalid;
        return failure;
    }

    // Ops that run as a one-step plan (McpPlanModel.h Single), so policy,
    // approval and the Undo label work as for any plan. Undo and raw commands
    // cannot run inside one; mcp.plan is one already.
    bool Wrapped(const PendingCall& call)
    {
        return Bridge::Mutates(call.policy->policy) && call.policy->policy != Bridge::Policy::Raw && call.op != "mcp.plan"
            && call.op != "undo.call" && call.op != "undo.steps" && call.op != "redo.steps";
    }
    bool Planned(const PendingCall& call) { return call.op == "mcp.plan" || Wrapped(call); }
    std::string Label(const PendingCall& call)
    {
        if (!call.label.empty()) return call.label;
        if (call.op == "mcp.plan" && !Text(call.args, "label").empty()) return Text(call.args, "label").substr(0, 200);
        return call.op;
    }
    Json BuildRequest(const PendingCall& call, const std::string& label, bool dryRun)
    {
        Json request = call.args;
        if (Wrapped(call)) request = Plan::Single(call.op, call.args, label);
        else if (call.op == "mcp.plan") request["label"] = label;
        if (Planned(call))
        {
            if (dryRun) request["dryRun"] = true;
            request["op"] = "mcp.plan";
        }
        else request["op"] = call.op;
        return request;
    }
    // Preview rows for what no dry run can show: undo and raw commands.
    Json SyntheticRows(const PendingCall& call)
    {
        Json row{{"change", call.op}, {"target", ""}, {"property", ""}, {"before", ""}, {"after", "runs on apply"}};
        if (call.op == "undo.call") { row["change"] = "Undo"; row["target"] = "MCP call " + Dump(call.args.value("call", Json())); row["after"] = "that call's Undo steps are undone"; }
        else if (call.op == "undo.steps" || call.op == "redo.steps")
        {
            row["change"] = call.op == "undo.steps" ? "Undo" : "Redo";
            row["target"] = Dump(call.args.value("count", Json(1))) + " step(s)";
        }
        else if (call.op == "exec") { row["change"] = "Command"; row["target"] = Text(call.args, "command").substr(0, 400); }
        return Json::array({row});
    }
    std::string Cell(const Json& row, const char* key)
    {
        if (!row.contains(key) || row.at(key).is_null()) return {};
        return row.at(key).is_string() ? row.at(key).get<std::string>() : Dump(row.at(key));
    }
    // The dry run's rows as the preview window lists them: five text columns
    // and the actor a click selects.
    Json ReviewRows(const Json& preview)
    {
        Json rows = Json::array();
        if (!preview.is_array()) return rows;
        for (const auto& row : preview)
        {
            if (!row.is_object()) continue;
            Json line = Json::object();
            for (const char* key : {"change", "target", "property", "before", "after"}) line[key] = Cell(row, key);
            if (line.at("after").get<std::string>().empty()) line["after"] = row.contains("error") ? "fails: " + Cell(row, "error") : Cell(row, "note");
            if (row.contains("select")) line["select"] = row.at("select");
            rows.push_back(std::move(line));
        }
        return rows;
    }
    std::string Tally(const Json& rows)
    {
        std::map<std::string, size_t> counts;
        for (const auto& row : rows) ++counts[Cell(row, "change")];
        static const std::pair<const char*, const char*> words[] = {{"Add", "added"}, {"Modify", "modified"}, {"Move", "moved"}, {"Delete", "deleted"}, {"Link", "link(s)"}};
        std::string text;
        auto add = [&](const std::string& part) { text += (text.empty() ? "" : ", ") + part; };
        for (const auto& [change, word] : words)
            if (counts.count(change)) { add(std::to_string(counts[change]) + " " + word); counts.erase(change); }
        for (const auto& [change, count] : counts) add(change + (count > 1 ? " x" + std::to_string(count) : std::string()));
        return text.empty() ? "No changes" : text;
    }
    std::string PolicyNote(const Bridge::OpPolicy& policy)
    {
        switch (policy.policy)
        {
        case Bridge::Policy::File: return "It writes files or packages, which Undo does not reach.";
        case Bridge::Policy::Build: return "It rebuilds the map's geometry or lighting.";
        case Bridge::Policy::Process: return "It starts or ends a process (Play Level, or closing the editor).";
        case Bridge::Policy::Raw: return "It runs a raw editor command.";
        default: return "Approve applies it as one Undo step; Edit > Undo or RE+ Tools > MCP > Undo last MCP call takes it back.";
        }
    }

    // RequireApproval: the dry run in the Map JSON preview window, then the
    // call only on Approve, on the map the preview showed.
    std::string Approve(PendingCall& call, unsigned n, const std::string& label, Watch& watched)
    {
        Json rows, dry;
        if (Planned(call))
        {
            {
                Watchdog dog(watched);
                McpOps::Context context;
                context.call = n;
                context.label = "MCP " + std::to_string(n) + ": " + label;
                context.dryRun = true;
                McpOps::Scope scope(context);
                dry = WorkflowTools::Request(BuildRequest(call, label, true));
            }
            if (watched.blocked) throw Bridge::Error(Bridge::Code::DialogBlocked, "A dialog opened while the change was previewed.");
            if (dry.is_object() && dry.contains("errors") && !Flag(dry, "ok"))
            {
                const auto& errors = dry.at("errors");
                const auto first = errors.is_object() && !errors.empty() ? errors.begin().key() + ": " + Cell(errors, errors.begin().key().c_str()) : std::string();
                throw Bridge::Error(Bridge::Code::EditorError, "The change would fail as the map is now, so it was not shown for approval. " + first, "preview_failed", dry);
            }
            rows = ReviewRows(dry.is_object() ? dry.value("preview", Json::array()) : Json::array());
        }
        else rows = SyntheticRows(call);
        const auto tally = Tally(rows);
        const auto before = MapNow();
        const auto ticket = NewTicket(call.id, n, call.op, label);
        Push(call.client, Bridge::ProgressMessage(call.id, "awaiting_approval", {{"ticket", ticket}, {"call", n}, {"rows", rows.size()}, {"summary", tally}}));
        {
            std::lock_guard lock(callLock);
            if (call.cancelRequested || activeClient.load() != call.client)
            {
                SetTicket(ticket, "cancelled");
                throw Bridge::Error(Bridge::Code::Cancelled, "The client went away before the change was shown for approval.", "client_gone", {{"ticket", ticket}});
            }
            call.stage = Stage::Approval;
        }
        std::string map;
        try { map = Editor::AuthoringMap(); }
        catch (const std::exception&) { map = "*"; }
        const auto who = call.clientName.empty() ? std::string("The MCP client") : call.clientName;
        const Json preview{{"rows", rows}, {"tally", tally},
                           {"note", who + " asks to run MCP call " + std::to_string(n) + " (" + label + "). " + PolicyNote(*call.policy)}};
        const Json document{{"map", map}, {"description", "Requested over MCP: " + label + (label == call.op ? std::string() : " (" + call.op + ")")}};
        bool accepted = false;
        reviewing = true;
        try { accepted = MapAuthoringDialog::Review(Frame(), preview, document); }
        catch (...)
        {
            reviewing = false;
            {
                std::lock_guard lock(callLock);
                call.stage = Stage::Running;
            }
            SetTicket(ticket, "failed");
            throw;
        }
        reviewing = false;
        bool cancelled = false;
        {
            std::lock_guard lock(callLock);
            call.stage = Stage::Running;
            cancelled = call.cancelRequested;
        }
        if (!accepted)
        {
            SetTicket(ticket, cancelled ? "cancelled" : "denied");
            if (cancelled) throw Bridge::Error(Bridge::Code::Cancelled, "The approval was cancelled: the client cancelled the call or went away.", "cancelled", {{"ticket", ticket}});
            throw Bridge::Error(Bridge::Code::Denied, "The user declined the change in the editor's approval window.", "user_declined", {{"ticket", ticket}});
        }
        if (!(MapNow() == before))
        {
            SetTicket(ticket, "stale");
            throw Bridge::Error(Bridge::Code::Stale, "The map changed while the change waited for approval. Read it again, then retry.", "changed_during_review", {{"ticket", ticket}});
        }
        SetTicket(ticket, "approved");
        Push(call.client, Bridge::ProgressMessage(call.id, "approved", {{"ticket", ticket}, {"call", n}}));
        return ticket;
    }

    // The newest undoable records stamped between two UndoMarks.
    unsigned StepsSince(unsigned from, unsigned to)
    {
        try
        {
            const auto undo = Editor::UndoRecords();
            const size_t undoable = undo.stamps.size() - std::min(undo.stamps.size(), static_cast<size_t>(std::max(undo.undone, 0)));
            unsigned count = 0;
            for (size_t i = 0; i < undoable; ++i) if (undo.stamps[i] >= from && undo.stamps[i] < to) ++count;
            return count;
        }
        catch (const std::exception&) { return 0; }
    }
    unsigned Mark()
    {
        try { return Editor::UndoMark(); }
        catch (const std::exception&) { return 0; }
    }

    // history\<stamp>-<call>-<op>.json: what was asked, what came back and
    // when, oldest deleted first past HistoryMaxMB.
    void Prune(const fs::path& folder, unsigned maxMB)
    {
        struct Entry { fs::path path; fs::file_time_type time; uintmax_t size = 0; };
        std::vector<Entry> files;
        uintmax_t total = 0;
        std::error_code error;
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        {
            std::error_code item;
            if (!it->is_regular_file(item) || it->path().extension() != ".json") continue;
            Entry entry{it->path(), it->last_write_time(item), it->file_size(item)};
            if (item) continue;
            total += entry.size;
            files.push_back(std::move(entry));
        }
        const uintmax_t limit = static_cast<uintmax_t>(maxMB) * 1024 * 1024;
        if (total <= limit) return;
        std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.time != b.time ? a.time < b.time : a.path < b.path; });
        for (size_t i = 0; i + 1 < files.size() && total > limit; ++i)
        {
            std::error_code removed;
            if (fs::remove(files[i].path, removed) && !removed) total -= files[i].size;
        }
    }
    void WriteHistory(const PendingCall& call, unsigned n, const std::string& label, const SYSTEMTIME& at, double milliseconds, const Json& reply, const std::string& ticket)
    {
        try
        {
            const auto folder = McpBridge::Directory() / "history";
            std::error_code error;
            fs::create_directories(folder, error);
            const unsigned year = at.wYear, month = at.wMonth, day = at.wDay, hour = at.wHour, minute = at.wMinute, second = at.wSecond, ms = at.wMilliseconds;
            char stamp[32], started[40];
            snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u", year, month, day, hour, minute, second);
            snprintf(started, sizeof(started), "%04u-%02u-%02uT%02u:%02u:%02u.%03u", year, month, day, hour, minute, second, ms);
            Json request{{"op", call.op}, {"args", call.args}, {"label", call.label}, {"dryRun", call.dryRun}};
            if (call.expect.set) request["expect"] = MarkJson(call.expect.level, call.expect.generation, call.expect.revision);
            Json record{{"format", "scct.mcp-call"}, {"version", 1}, {"call", n}, {"id", call.id}, {"label", label}, {"client", call.clientName},
                        {"started", started}, {"durationMs", milliseconds}, {"request", request}, {"reply", reply}};
            if (!ticket.empty()) record["ticket"] = ticket;
            std::ofstream out(folder / Bridge::HistoryName(stamp, n, call.op), std::ios::binary | std::ios::trunc);
            out << Dump(record, 2);
            out.close();
            if (!out) throw std::runtime_error("the file could not be written");
            Prune(folder, McpBridge::Current().historyMaxMB);
        }
        catch (const std::exception& e) { Logger::log(std::string("MCP: could not write the call history: ") + e.what()); }
    }

    // One call, start to reply, on the frame's thread.
    Json RunCall(PendingCall& call)
    {
        const auto& policy = *call.policy;
        const bool mutating = Bridge::Mutates(policy.policy);
        const auto busy = Guard::BusyReason(GuardInputs(), policy, call.op == "view.screenshot");
        // The state is a few reads of memory: it answers even while the
        // editor is busy, saying with what.
        if (call.op == "editor.state")
        {
            auto state = RefreshState();
            if (!busy.empty()) state["busy"] = busy;
            return Bridge::ResultMessage(call.id, state, state);
        }
        if (!busy.empty()) return Bridge::ErrorMessage(call.id, Bridge::ErrorBody(Bridge::Code::Busy, Guard::BusyHint(busy), busy), CachedState(busy));

        struct Depth { Depth() { ++depth; } ~Depth() { --depth; } } inside;
        const unsigned n = ++callCounter;
        const auto label = Label(call);
        const auto prefix = "MCP " + std::to_string(n) + ": ";
        const uint64_t logFrom = McpBridge::LogLast();
        SYSTEMTIME startedAt{};
        GetLocalTime(&startedAt);
        const auto started = std::chrono::steady_clock::now();
        Watch watched;
        std::string ticket;
        unsigned markFrom = Mark(), markTo = markFrom, steps = 0;
        std::optional<Json> result;
        Failure failure;
        try
        {
            if (mutating) CheckExpect(call.expect);
            if (mutating && !call.dryRun && McpBridge::Current().requireApproval) ticket = Approve(call, n, label, watched);
            markFrom = Mark();
            Json value;
            {
                Watchdog dog(watched);
                McpOps::Context context;
                context.call = n;
                context.label = prefix + label;
                context.dryRun = call.dryRun;
                context.approved = !ticket.empty();
                McpOps::Scope scope(context);
                TransactionPrefix named(prefix);
                if (mutating && call.dryRun && !Planned(call))
                    value = {{"dryRun", true}, {"preview", SyntheticRows(call)}, {"note", "Runs on apply; nothing was changed."}};
                else value = WorkflowTools::Request(BuildRequest(call, label, call.dryRun));
            }
            // A one-step plan answers as the op itself would.
            if (Wrapped(call) && !call.dryRun && value.is_object() && value.contains("steps") && value.at("steps").is_object() && value.at("steps").contains("s1"))
                value = Json(value.at("steps").at("s1"));
            result = std::move(value);
        }
        catch (...) { failure = Classify(call.op); }
        markTo = Mark();
        steps = StepsSince(markFrom, markTo);
        if (watched.blocked)
        {
            // Whatever the dialog's answer let the call do is taken back.
            size_t undone = 0;
            if (policy.policy == Bridge::Policy::Undoable && steps)
                try { undone = Editor::UndoBackTo(markFrom); }
                catch (const std::exception& e) { Logger::log(std::string("MCP: could not undo the call after its dialog: ") + e.what()); }
            if (undone) { markTo = Mark(); steps = StepsSince(markFrom, markTo); }
            Json details{{"dialogs", watched.dialogs}, {"rolledBack", undone}};
            if (!result) details["cause"] = {{"code", failure.code}, {"message", failure.message}};
            Json first = watched.dialogs.front();
            for (const auto& dialog : watched.dialogs) if (!Flag(dialog, "known")) { first = dialog; break; }
            failure = {Bridge::Code::DialogBlocked,
                       "The editor asked \"" + Text(first, "title") + "\" during " + call.op + ": " + Text(first, "text") + " It was answered \"" + Text(first, "button") + "\"" +
                           (undone ? " and the call's changes were undone." : "."),
                       "unknown_dialog", details};
            result.reset();
        }
        if (steps)
        {
            std::lock_guard lock(recordLock);
            records.push_back({n, label, markFrom, markTo, steps});
            if (records.size() > kRecordsMax) records.erase(records.begin());
        }
        const auto state = RefreshState();
        Json extra{{"call", n}, {"log", LogJson(logFrom)}, {"dialogs", watched.dialogs}, {"undo", {{"call", n}, {"steps", steps}}}};
        if (!ticket.empty()) extra["ticket"] = ticket;
        Json reply;
        if (result) reply = Bridge::ResultMessage(call.id, *result, state, extra);
        else
        {
            reply = Bridge::ErrorMessage(call.id, Bridge::ErrorBody(failure.code, failure.message, failure.reason, failure.details), state);
            for (auto it = extra.begin(); it != extra.end(); ++it) reply[it.key()] = it.value();
            const auto ticketed = failure.details.is_object() ? Text(failure.details, "ticket") : std::string();
            if (ticket.empty() && !ticketed.empty()) ticket = ticketed;
        }
        if (!ticket.empty())
        {
            Json outcome{{"ok", reply.at("ok")}, {"undo", extra.at("undo")}};
            if (result) outcome["result"] = reply.at("result");
            else outcome["error"] = reply.at("error");
            SetTicket(ticket, result ? "done" : "failed", outcome);
        }
        if (mutating && !call.dryRun)
            WriteHistory(call, n, label, startedAt, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(), reply, ticket);
        return reply;
    }

    // kRun: the call in the slot, unless its client has gone.
    void RunQueued()
    {
        std::shared_ptr<PendingCall> call;
        {
            std::lock_guard lock(callLock);
            if (!slot || slot->stage != Stage::Queued) return;
            if (slot->client != activeClient.load()) { slot.reset(); return; }
            call = slot;
            call->stage = Stage::Running;
        }
        Json reply;
        try { reply = RunCall(*call); }
        catch (...)
        {
            const auto failure = Classify(call->op);
            reply = Bridge::ErrorMessage(call->id, Bridge::ErrorBody(failure.code, failure.message, failure.reason, failure.details));
        }
        {
            std::lock_guard lock(callLock);
            if (slot == call) slot.reset();
        }
        Push(call->client, reply);
    }
    // A client that left while its change was in review: the review closes.
    void ClientChanged()
    {
        bool cancel = false;
        {
            std::lock_guard lock(callLock);
            cancel = slot && slot->stage == Stage::Approval && slot->client != activeClient.load();
            if (cancel) slot->cancelRequested = true;
        }
        if (cancel && reviewing) MapAuthoringDialog::CancelReview();
    }
    LRESULT CALLBACK BridgeProc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        if (message == kRun) { RunQueued(); return 0; }
        if (message == kCancelReview) { if (reviewing) MapAuthoringDialog::CancelReview(); return 0; }
        if (message == kClientChanged) { ClientChanged(); return 0; }
        return DefWindowProcW(window, message, w, l);
    }

    // RE+ Tools > MCP.
    void UndoLast()
    {
        if (depth > 0) throw std::runtime_error("An MCP call is still running; undo it once it has finished.");
        const auto list = McpBridge::Records();
        if (list.empty()) throw std::runtime_error("No MCP call has changed the map in this session.");
        // The newest call not undone already.
        for (auto it = list.rbegin(); it != list.rend(); ++it)
            try
            {
                WorkflowTools::Request(Json{{"op", "undo.call"}, {"call", it->call}});
                RefreshState();
                return;
            }
            catch (const Bridge::Error& e)
            {
                if (e.reason == "already_undone") continue;
                throw std::runtime_error(Utf8(e.what()));
            }
        throw std::runtime_error("Every MCP call of this session is undone already.");
    }
    void CopyToClipboard(const std::wstring& text)
    {
        if (!OpenClipboard(Frame())) throw std::runtime_error("Another program is using the clipboard; try again.");
        EmptyClipboard();
        bool copied = false;
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t)))
        {
            if (void* data = GlobalLock(memory))
            {
                memcpy(data, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(memory);
                copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
            }
            if (!copied) GlobalFree(memory);
        }
        CloseClipboard();
        if (!copied) throw std::runtime_error("The text could not be put on the clipboard.");
    }
    void CopySetup(bool desktop)
    {
        const auto exe = SystemDirectory() / L"Reloaded.Editor.Mcp.exe";
        const auto text = desktop ? Wide(Json{{"mcpServers", Json{{"scct-reloaded", Json{{"command", Narrow(exe.wstring())}}}}}}.dump())
                                  : L"claude mcp add scct-reloaded -- \"" + exe.wstring() + L"\"";
        CopyToClipboard(text);
        std::wstring note = L"Copied to the clipboard:\n\n" + text + L"\n\n" +
            (desktop ? L"Put it in Claude Desktop's claude_desktop_config.json (Settings > Developer > Edit Config), then restart Claude Desktop."
                     : L"Run it in a terminal to add this editor to Claude Code.");
        std::error_code error;
        if (!fs::exists(exe, error)) note += L"\n\nReloaded.Editor.Mcp.exe is not in " + exe.parent_path().wstring() + L" yet: it comes with the RE+ release, beside Reloaded.Editor.dll.";
        if (!McpBridge::Current().enabled) note += L"\n\nThe MCP server is off: turn on RE+ Tools > MCP > Enable MCP server so the client can reach this editor.";
        MessageBoxW(Frame(), note.c_str(), L"" RE_PLUS_NAME L" MCP", MB_OK | MB_ICONINFORMATION);
    }
    void OpenHistory()
    {
        const auto folder = McpBridge::Directory() / "history";
        std::error_code error;
        fs::create_directories(folder, error);
        if (error) throw std::runtime_error("Cannot create " + Narrow(folder.wstring()) + ".");
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(Frame(), L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            throw std::runtime_error("Windows could not open " + Narrow(folder.wstring()) + ".");
    }
}

void McpBridge::Initialize()
{
    Store(LoadSettings());
}

void McpBridge::AttachUiThread(HWND frame)
{
    if (bridgeWindow.load()) return;
    frameWindow = frame;
    WNDCLASSW type{};
    type.lpfnWndProc = BridgeProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"ReloadedMcpBridge";
    RegisterClassW(&type); // Fails harmlessly when a retried install registered it already.
    HWND window = CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, nullptr);
    if (!window) { Logger::log("MCP: cannot create the bridge's message window, error " + std::to_string(GetLastError())); return; }
    bridgeWindow = window;
    EngineLog::AddListener(OnEngineLog);
    WriteMissingDefaults();
    if (Current().enabled) StartServer();
}

void McpBridge::Shutdown()
{
    StopServer();
    EngineLog::RemoveListener(OnEngineLog);
    if (HWND window = bridgeWindow.exchange(nullptr)) DestroyWindow(window);
}

bool McpBridge::HandleCommand(UINT command)
{
    if (command < kMenuFirst || command > kMenuLast) return false;
    try
    {
        auto next = Current();
        if (command == kEnable)
        {
            next.enabled = !next.enabled;
            Store(next);
            WriteFlag(L"Enabled", next.enabled);
            if (next.enabled) StartServer();
            else StopServer();
            Logger::log(next.enabled ? "MCP: the server was turned on" : "MCP: the server was turned off");
        }
        else if (command == kRequireApproval)
        {
            next.requireApproval = !next.requireApproval;
            Store(next);
            WriteFlag(L"RequireApproval", next.requireApproval);
        }
        else if (command == kUndoLast) UndoLast();
        else if (command == kDisconnect)
        {
            dropRequested = true;
            if (wakeEvent) SetEvent(wakeEvent);
        }
        else if (command == kCopyClaudeCode) CopySetup(false);
        else if (command == kCopyClaudeDesktop) CopySetup(true);
        else if (command == kOpenHistory) OpenHistory();
    }
    catch (const std::exception& e)
    {
        MessageBoxW(Frame(), Wide(Utf8(e.what())).c_str(), L"" RE_PLUS_NAME L" MCP", MB_OK | MB_ICONINFORMATION);
    }
    return true;
}

HMENU McpBridge::BuildMenu()
{
    HMENU created = CreatePopupMenu();
    if (!created) return nullptr;
    AppendMenuW(created, MF_STRING, kEnable, L"&Enable MCP server");
    AppendMenuW(created, MF_STRING, kRequireApproval, L"Require &approval for changes");
    AppendMenuW(created, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(created, MF_STRING, kUndoLast, L"&Undo last MCP call");
    AppendMenuW(created, MF_STRING, kDisconnect, L"&Disconnect client");
    AppendMenuW(created, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(created, MF_STRING, kCopyClaudeCode, L"Copy Claude &Code command");
    AppendMenuW(created, MF_STRING, kCopyClaudeDesktop, L"Copy Claude Des&ktop config");
    AppendMenuW(created, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(created, MF_STRING, kOpenHistory, L"Open MCP &history folder");
    UpdateMenu(created);
    return created;
}

void McpBridge::UpdateMenu(HMENU menu)
{
    // The submenu, or one it hangs under: by command, Windows looks inside.
    if (!menu || GetMenuState(menu, kEnable, MF_BYCOMMAND) == UINT(-1)) return;
    const auto current = Current();
    const bool connected = Connected();
    bool undoable = false;
    {
        std::lock_guard lock(recordLock);
        undoable = !records.empty();
    }
    ModifyMenuW(menu, kEnable, MF_BYCOMMAND | MF_STRING | (current.enabled ? MF_CHECKED : MF_UNCHECKED), kEnable,
                connected ? L"&Enable MCP server (connected)" : L"&Enable MCP server");
    CheckMenuItem(menu, kRequireApproval, MF_BYCOMMAND | (current.requireApproval ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(menu, kUndoLast, MF_BYCOMMAND | (undoable ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, kDisconnect, MF_BYCOMMAND | (connected ? MF_ENABLED : MF_GRAYED));
}

McpBridge::Settings McpBridge::Current()
{
    std::lock_guard lock(settingsLock);
    return settings;
}

bool McpBridge::Connected() { return activeClient.load() != 0; }

std::filesystem::path McpBridge::Directory() { return SystemDirectory() / "ReloadedEditor" / "Mcp"; }

std::vector<McpBridge::CallRecord> McpBridge::Records()
{
    std::lock_guard lock(recordLock);
    return records;
}

std::vector<McpBridge::LogLine> McpBridge::LogSince(uint64_t seq, size_t max)
{
    std::lock_guard lock(logLock);
    std::vector<LogLine> lines;
    // The newest max of them, oldest first.
    auto it = std::upper_bound(logRing.begin(), logRing.end(), seq, [](uint64_t value, const LogLine& line) { return value < line.seq; });
    const auto available = static_cast<size_t>(logRing.end() - it);
    if (available > max) it += static_cast<std::ptrdiff_t>(available - max);
    lines.assign(it, logRing.end());
    return lines;
}

uint64_t McpBridge::LogLast()
{
    std::lock_guard lock(logLock);
    return logSeq;
}
