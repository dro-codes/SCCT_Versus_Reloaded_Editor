#include "pch.h"
#include "EngineLog.h"
#include "logger.h"
#include "MemoryWriter.h"
#include <string>
#include <cstdio>
#include <cstring>

namespace {

#define EXEC_LOG_DEV            0x115befb0u   // GLog/GNull output device ptr
#define GNAMES_DATA             0x1169cfbcu
#define GNAMES_NUM              0x1169cfc0u
#define FNAME_ENTRY_STR_OFFSET  0x0Cu

// FOutputDevice::Serialize is vtable[0] - __thiscall(this, text, EName) with EName a
// GNames index; __fastcall with a dead second parameter is thiscall as MSVC spells it.
typedef void (__fastcall *SerializeFn)(void* self, void* edx, const char* text, int event);

SerializeFn     g_original;
EngineLog::Sink g_sink;

// Listeners are read on whichever thread logs and added from the UI thread.
// A fixed table under a slim lock needs no construction before DllMain runs.
constexpr int   kListeners = 8;
EngineLog::Sink g_listeners[kListeners];
SRWLOCK         g_listenersLock = SRWLOCK_INIT;

void Notify(const char* line)
{
    EngineLog::Sink listeners[kListeners] = {};
    AcquireSRWLockShared(&g_listenersLock);
    for (int i = 0; i < kListeners; ++i)
        listeners[i] = g_listeners[i];
    ReleaseSRWLockShared(&g_listenersLock);
    for (auto listener : listeners)
        if (listener)
            listener(line);
}

const char* EventName(int index)
{
    void** namesData = *reinterpret_cast<void***>(GNAMES_DATA);
    const int namesNum = *reinterpret_cast<int*>(GNAMES_NUM);
    if (!namesData || index < 0 || index >= namesNum)
        return nullptr;

    void* entry = namesData[index];
    return entry ? static_cast<char*>(entry) + FNAME_ENTRY_STR_OFFSET : nullptr;
}

bool IsProblem(const char* eventName)
{
    return _stricmp(eventName, "Error") == 0
        || _stricmp(eventName, "Warning") == 0
        || _stricmp(eventName, "Critical") == 0
        || _stricmp(eventName, "ExecWarning") == 0
        || _stricmp(eventName, "ScriptWarning") == 0;
}

void __fastcall SerializeHook(void* self, void* edx, const char* text, int event)
{
    if (text && *text)
    {
        const char* eventName = EventName(event);
        if (eventName && IsProblem(eventName))
        {
            char line[512];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "%s: %s", eventName, text);
            Logger::log(line);
            if (const auto sink = g_sink)
                sink(line);
            Notify(line);
        }
    }

    if (g_original)
        g_original(self, edx, text, event);
}

// GLog does not exist yet at DLL attach - the editor builds it during startup.
DWORD WINAPI InstallThread(LPVOID)
{
    for (int i = 0; i < 600; ++i)   // up to ~60s
    {
        void* logDev = *reinterpret_cast<void**>(EXEC_LOG_DEV);
        void** vtable = logDev ? *reinterpret_cast<void***>(logDev) : nullptr;
        if (vtable && vtable[0])
        {
            g_original = reinterpret_cast<SerializeFn>(vtable[0]);
            void* hook = &SerializeHook;
            if (MemoryWriter::WriteBytes(reinterpret_cast<uintptr_t>(vtable), &hook, sizeof(hook)))
                Logger::log("EngineLog: engine errors and warnings are being captured");
            else
                g_original = nullptr;
            return 0;
        }
        Sleep(100);
    }
    return 0;
}

} // namespace

void EngineLog::Initialize()
{
    HANDLE h = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

EngineLog::Sink EngineLog::SetSink(Sink sink)
{
    const Sink previous = g_sink;
    g_sink = sink;
    return previous;
}

void EngineLog::AddListener(Sink listener)
{
    if (!listener)
        return;
    AcquireSRWLockExclusive(&g_listenersLock);
    bool added = false;
    for (auto& slot : g_listeners)
        if (slot == listener)
            added = true;
    for (auto& slot : g_listeners)
        if (!added && !slot)
        {
            slot = listener;
            added = true;
        }
    ReleaseSRWLockExclusive(&g_listenersLock);
    if (!added)
        Logger::log("EngineLog: no room for another log listener");
}

void EngineLog::RemoveListener(Sink listener)
{
    AcquireSRWLockExclusive(&g_listenersLock);
    for (auto& slot : g_listeners)
        if (slot == listener)
            slot = nullptr;
    ReleaseSRWLockExclusive(&g_listenersLock);
}
