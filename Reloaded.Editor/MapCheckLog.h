#pragma once

class MapCheckLog
{
public:
    static void Initialize();

    // Set for the length of a bulk run: entries come here and the dialog stays shut.
    // Returns the sink it replaces, for the caller to put back.
    typedef void (__cdecl *Sink)(const char* line);
    static Sink SetSink(Sink sink);

    // Set while Map Check runs the stock CheckForErrors pass: every entry
    // (type 0 error, 1 warning, else a note; the actor; the text) comes here
    // and the stock Map Check dialog is left alone.
    typedef void (__cdecl *Capture)(int type, void* actor, const char* message);
    static void SetCapture(Capture capture);
};
