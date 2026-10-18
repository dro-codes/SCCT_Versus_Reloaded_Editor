#pragma once

class EngineLog
{
public:
    static void Initialize();

    // Second destination for engine errors and warnings, on top of the Reloaded log.
    // Returns the sink it replaces, for the caller to put back.
    typedef void (__cdecl *Sink)(const char* line);
    static Sink SetSink(Sink sink);

    // More destinations for the same lines ("Warning: text"), kept apart from the
    // sink so a bulk run that routes the sink elsewhere does not take them away
    // (the MCP bridge's log). Called on the thread that logs, which need not be
    // the editor's, so a listener must be quick and do its own locking. Adding
    // one twice keeps one; removing one not added does nothing.
    static void AddListener(Sink listener);
    static void RemoveListener(Sink listener);
};
