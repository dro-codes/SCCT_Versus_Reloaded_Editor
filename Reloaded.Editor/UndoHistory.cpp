#include "pch.h"
#undef min
#undef max
#include "UndoHistory.h"
#include "WindowPlacement.h"
#include "Appearance.h"
#include "UndoHistoryModel.h"
#include "MemoryWriter.h"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

// The editor's undo buffer as a list. GEditor->Trans is a UTransBuffer; its
// layout in this build, from UTransBuffer::Begin (0x1105ad20), Undo
// (0x11059990), Redo (0x11059b40), CanUndo (0x11059870), Reset (0x110592a0)
// and the size check Begin makes before trimming (0x1105ae30):
//   +0x00 vtable 0x1148dd50 (Reset 0x60, Begin 0x64, End 0x68, CanUndo 0x70,
//         CanRedo 0x74, Undo 0x78, Redo 0x7c)
//   +0x28 TArray<FTransaction> UndoBuffer (data, Num +0x2c), oldest first
//   +0x34 UndoCount, +0x38 FString ResetReason, +0x44 ActiveCount,
//   +0x48 MaxMemory (bytes; Begin drops the oldest step while the records
//         hold more), +0x4c Overflow
// FTransaction is 0x28 bytes: TArray<FObjectRecord> Records at +0x08 (Num
// +0x0c), FString Title at +0x14. An FObjectRecord is 0x30 bytes and starts
// with its TArray<BYTE> Data, whose Num (+0x04) is what the size check adds up.
// The stock Edit > Undo / Redo (40019 / 40020) run GEditor->Exec("TRANSACTION
// UNDO" / "REDO", GLog); a step here does exactly that.
namespace UndoHistory
{
namespace
{
    using Address = uintptr_t;
    constexpr Address kEditor = 0x1165DFA0, kLog = 0x115BEFB0, kMainFrame = 0x1165df84;
    constexpr Address kTransBufferVtable = 0x1148dd50;
    constexpr int kTransactionSize = 0x28, kRecordSize = 0x30;
    constexpr UINT kTimer = 1, kTimerMs = 300, kChanged = WM_APP + 0x55;
    enum Id { List = 100, UndoButton, RedoButton, GoButton, CopyButton, Status, JumpList, AddButton };
    // The list's right-click menu (the panel's own commands, not the frame's).
    enum MenuId { MenuGo = 1, MenuName, MenuRemove };

    HWND window = nullptr;
    HFONT font = nullptr, bold = nullptr;
    int rowHeight = 18;
    Snapshot shown;
    bool available = false, busy = false;
    std::string report;
    // Kept while the editor runs, open panel or not: the buffer hooks keep
    // them in step with the buffer once the panel has been opened.
    Checkpoints checkpoints;
    unsigned checkpointVersion = 0, shownVersion = ~0u;
    std::string checkpointNote; // what last removed checkpoints, until shown

    bool CopySafe(void* to, const void* from, size_t size)
    {
        __try { memcpy(to, from, size); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    template<class T> T Read(Address address)
    {
        T value{};
        if (!address || !CopySafe(&value, reinterpret_cast<const void*>(address), sizeof(value)))
            throw std::runtime_error("The editor's undo buffer could not be read.");
        return value;
    }
    // An FString: data pointer, Num counting the terminator.
    std::string String(Address at, size_t cap = 1024)
    {
        const Address data = Read<Address>(at);
        const int count = Read<int>(at + 4);
        if (!data || count <= 1) return {};
        std::string text(std::min<size_t>(static_cast<size_t>(count - 1), cap), '\0');
        if (!CopySafe(text.data(), reinterpret_cast<const void*>(data), text.size()))
            throw std::runtime_error("The editor's undo buffer could not be read.");
        text.resize(strnlen(text.c_str(), text.size()));
        return text;
    }

    // The buffer tells the panel when it changes: End, Undo, Redo and Reset
    // post one coalesced message. Posted messages are not starved the way
    // WM_TIMER is while the frame works through a queue of its own posted
    // commands (thousands of edits in a row leave such a queue), so the timer
    // is only the fallback. The vtable slots are chained, as WorkflowEditor's
    // End observer chains whatever it finds.
    volatile LONG changePosted = 0;
    void Changed()
    {
        if (window && !InterlockedExchange(&changePosted, 1) && !PostMessageA(window, kChanged, 0, 0))
            InterlockedExchange(&changePosted, 0);
    }
    // Each transaction's print for the checkpoints: the address of its title
    // text, which moves with nothing the buffer does while the step lives.
    Checkpoints::Prints ReadPrints(Address trans)
    {
        const Address data = Read<Address>(trans + 0x28);
        const int count = Read<int>(trans + 0x2c);
        if (count < 0 || count > 1000000 || (count && !data)) throw std::runtime_error("The editor's undo buffer looks damaged.");
        Checkpoints::Prints prints(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) prints[i] = Read<Address>(data + static_cast<Address>(i) * kTransactionSize + 0x14);
        return prints;
    }
    void NoteGone(size_t gone)
    {
        if (!gone) return;
        checkpointNote = CheckpointsGoneText(gone);
        ++checkpointVersion;
    }
    // After Undo, Redo or End: the checkpoints still match the buffer.
    void Recheck(void* self)
    {
        if (checkpoints.Empty()) return;
        try { NoteGone(checkpoints.Observe(ReadPrints(reinterpret_cast<Address>(self)))); }
        catch (...) { NoteGone(checkpoints.Reset()); }
    }

    void* previousBegin = nullptr;
    void* previousEnd = nullptr;
    void* previousUndo = nullptr;
    void* previousRedo = nullptr;
    void* previousReset = nullptr;
    // Begin drops the undone steps, trims the oldest when the buffer is
    // full and adds the new step; the checkpoints follow, comparing the
    // buffer before and after. Nested Begins (ActiveCount > 0) change nothing.
    // The result is passed through in case this build's Begin returns one.
    int __fastcall HookBegin(void* self, void*, void* session)
    {
        const Address trans = reinterpret_cast<Address>(self);
        const bool track = !checkpoints.Empty();
        Checkpoints::Prints before;
        int undoBefore = 0, active = 0;
        bool read = true;
        if (track)
            try { before = ReadPrints(trans); undoBefore = Read<int>(trans + 0x34); active = Read<int>(trans + 0x44); }
            catch (...) { read = false; }
        const int r = reinterpret_cast<int(__thiscall*)(void*, void*)>(previousBegin)(self, session);
        if (track && active == 0)
        {
            try
            {
                if (!read) throw std::runtime_error("unreadable");
                NoteGone(checkpoints.Began(before, undoBefore, ReadPrints(trans), Read<int>(trans + 0x34)));
            }
            catch (...) { NoteGone(checkpoints.Reset()); }
        }
        return r;
    }
    void __fastcall HookEnd(void* self, void*) { reinterpret_cast<void(__thiscall*)(void*)>(previousEnd)(self); Recheck(self); Changed(); }
    int __fastcall HookUndo(void* self, void*) { const int r = reinterpret_cast<int(__thiscall*)(void*)>(previousUndo)(self); Recheck(self); Changed(); return r; }
    int __fastcall HookRedo(void* self, void*) { const int r = reinterpret_cast<int(__thiscall*)(void*)>(previousRedo)(self); Recheck(self); Changed(); return r; }
    void __fastcall HookReset(void* self, void*, const char* reason)
    {
        reinterpret_cast<void(__thiscall*)(void*, const char*)>(previousReset)(self, reason);
        if (const size_t gone = checkpoints.Reset())
        {
            checkpointNote = gone == 1 ? "The checkpoint went with the history." : std::to_string(gone) + " checkpoints went with the history.";
            ++checkpointVersion;
        }
        Changed();
    }
    void Patch(size_t slot, void*& previous, void* hook)
    {
        if (previous) return;
        const Address at = kTransBufferVtable + slot;
        if (!CopySafe(&previous, reinterpret_cast<const void*>(at), sizeof(previous)) || !previous) { previous = nullptr; return; }
        if (!MemoryWriter::WriteBytes(at, &hook, sizeof(hook))) previous = nullptr;
    }
    void Observe()
    {
        Patch(0x60, previousReset, reinterpret_cast<void*>(&HookReset));
        Patch(0x64, previousBegin, reinterpret_cast<void*>(&HookBegin));
        Patch(0x68, previousEnd, reinterpret_cast<void*>(&HookEnd));
        Patch(0x78, previousUndo, reinterpret_cast<void*>(&HookUndo));
        Patch(0x7c, previousRedo, reinterpret_cast<void*>(&HookRedo));
    }

    Address Buffer()
    {
        const Address editor = Read<Address>(kEditor);
        if (!editor || !Read<Address>(editor + 0x130)) return 0;
        const Address trans = Read<Address>(editor + 0x148);
        if (!trans || Read<Address>(trans) != kTransBufferVtable) return 0;
        return trans;
    }

    // What changes whenever the list or the current point does, read without
    // walking the buffer, so an idle timer tick costs a handful of reads.
    struct Signature
    {
        Address trans = 0, data = 0, firstTitle = 0, lastTitle = 0, lastRecords = 0, reason = 0;
        int count = -1, undoCount = 0, active = 0, lastRecordCount = 0;
        bool operator==(const Signature&) const = default;
    };
    Signature seen;

    Signature Sign(Address trans)
    {
        Signature s;
        s.trans = trans;
        if (!trans) return s;
        s.data = Read<Address>(trans + 0x28);
        s.count = Read<int>(trans + 0x2c);
        s.undoCount = Read<int>(trans + 0x34);
        s.reason = Read<Address>(trans + 0x38);
        s.active = Read<int>(trans + 0x44);
        if (s.count > 0 && s.data)
        {
            s.firstTitle = Read<Address>(s.data + 0x14);
            const Address last = s.data + static_cast<Address>(s.count - 1) * kTransactionSize;
            s.lastTitle = Read<Address>(last + 0x14);
            s.lastRecords = Read<Address>(last + 0x08);
            s.lastRecordCount = Read<int>(last + 0x0c);
        }
        return s;
    }

    Snapshot ReadSnapshot(Address trans)
    {
        Snapshot s;
        const Address data = Read<Address>(trans + 0x28);
        const int count = Read<int>(trans + 0x2c);
        if (count < 0 || count > 1000000 || (count && !data)) throw std::runtime_error("The editor's undo buffer looks damaged.");
        s.undoCount = Read<int>(trans + 0x34);
        s.resetReason = String(trans + 0x38, 256);
        s.recording = Read<int>(trans + 0x44) != 0;
        s.limit = Read<unsigned>(trans + 0x48);
        s.titles.reserve(count);
        for (int i = 0; i < count; ++i)
        {
            const Address transaction = data + static_cast<Address>(i) * kTransactionSize;
            s.titles.push_back(String(transaction + 0x14, 512));
            const Address records = Read<Address>(transaction + 0x08);
            const int recordCount = Read<int>(transaction + 0x0c);
            if (recordCount < 0 || recordCount > 4000000 || (recordCount && !records)) continue;
            for (int r = 0; r < recordCount; ++r)
                s.used += static_cast<unsigned>(Read<int>(records + static_cast<Address>(r) * kRecordSize + 4));
        }
        return s;
    }

    HWND Child(int id) { return GetDlgItem(window, id); }
    void Layout(int width, int height);

    void ShowStatus()
    {
        std::string text = available ? StatusText(shown) : "Open a map to see its undo history.";
        if (!report.empty()) text = report + "\r\n" + text;
        SetWindowTextA(Child(Status), text.c_str());
        if (Appearance::Current() != Appearance::Theme::Classic)
        {
            // The status block grows to keep its lines in the larger modern font.
            RECT client{};
            GetClientRect(window, &client);
            Layout(client.right, client.bottom);
        }
        const int current = Current(shown);
        EnableWindow(Child(UndoButton), available && current > 0);
        EnableWindow(Child(RedoButton), available && current < Count(shown));
        RedrawWindow(window, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    // The quick-jump list: a heading item, then every checkpoint by row.
    void FillJumpList()
    {
        HWND combo = Child(JumpList);
        const auto marks = available ? checkpoints.List() : std::vector<std::pair<int, std::string>>{};
        SendMessageA(combo, WM_SETREDRAW, FALSE, 0);
        SendMessageA(combo, CB_RESETCONTENT, 0, 0);
        const std::string heading = marks.empty() ? "No checkpoints yet" : "Jump to a checkpoint (" + std::to_string(marks.size()) + ")...";
        SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(heading.c_str()));
        for (const auto& [row, name] : marks)
        {
            const auto text = JumpItemText(shown, row, name);
            const auto index = SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            if (index >= 0) SendMessageA(combo, CB_SETITEMDATA, index, row);
        }
        SendMessageA(combo, CB_SETCURSEL, 0, 0);
        SendMessageA(combo, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(combo, nullptr, TRUE);
        EnableWindow(combo, !marks.empty());
    }

    // Reads the buffer again when it moved; force rereads it regardless.
    void Refresh(bool force)
    {
        if (!window || busy) return;
        Address trans = 0;
        Signature now;
        try { trans = Buffer(); now = Sign(trans); }
        catch (const std::exception&) { trans = 0; now = Signature{}; }
        if (!force && now == seen && shownVersion == checkpointVersion) return;
        seen = now;
        Snapshot next;
        bool ok = trans != 0;
        std::string failure;
        if (ok)
        {
            try { next = ReadSnapshot(trans); }
            catch (const std::exception& error) { ok = false; failure = error.what(); }
        }
        if (ok && !checkpoints.Empty())
        {
            try { NoteGone(checkpoints.Observe(ReadPrints(trans))); }
            catch (...) { NoteGone(checkpoints.Reset()); }
        }
        HWND list = Child(List);
        const bool moved = ok != available || next.titles != shown.titles || next.undoCount != shown.undoCount;
        const bool marksMoved = shownVersion != checkpointVersion;
        // The last jump's report stays until something else moves the buffer.
        if (moved || !failure.empty()) report = failure;
        if (!checkpointNote.empty()) report = report.empty() ? checkpointNote : report + " " + checkpointNote, checkpointNote.clear();
        available = ok;
        shown = ok ? std::move(next) : Snapshot{};
        shownVersion = checkpointVersion;
        if (moved)
        {
            SendMessageA(list, WM_SETREDRAW, FALSE, 0);
            SendMessageA(list, LB_SETCOUNT, available ? Rows(shown) : 0, 0);
            // The selection follows the current point; selecting scrolls it into view.
            if (available) SendMessageA(list, LB_SETCURSEL, Current(shown), 0);
            SendMessageA(list, WM_SETREDRAW, TRUE, 0);
        }
        if (moved || marksMoved)
        {
            InvalidateRect(list, nullptr, TRUE);
            FillJumpList();
        }
        ShowStatus();
    }

    // One stock Undo or Redo. True when the buffer moved.
    bool Step(bool redo)
    {
        const Address trans = Buffer();
        if (!trans) return false;
        const int before = Read<int>(trans + 0x34);
        const Address editor = Read<Address>(kEditor);
        const Address exec = editor + 0x28;
        const auto method = Read<Address>(Read<Address>(exec));
        reinterpret_cast<int(__thiscall*)(void*, const char*, void*)>(method)(
            reinterpret_cast<void*>(exec), redo ? "TRANSACTION REDO" : "TRANSACTION UNDO", reinterpret_cast<void*>(Read<Address>(kLog)));
        const Address after = Buffer();
        return after == trans && Read<int>(trans + 0x34) != before;
    }

    void GoTo(int row)
    {
        Refresh(true);
        if (!available) { report = "Open a map first."; ShowStatus(); return; }
        if (shown.recording) { report = "Finish the current editor operation first."; ShowStatus(); return; }
        const int steps = StepsTo(shown, row);
        JumpResult result;
        std::string failure;
        busy = true;
        HCURSOR previous = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        try { result = Jump(steps, [](bool redo) { return Step(redo); }); }
        catch (const std::exception& error) { result.wanted = steps; result.stopped = true; failure = error.what(); }
        SetCursor(previous);
        busy = false;
        Refresh(true);
        report = JumpReport(result) + (failure.empty() ? "" : " " + failure);
        ShowStatus();
    }

    // One step from wherever the buffer is now, not from what the list last
    // showed: a stock Undo a moment ago may not have reached the list yet.
    void Move(int delta)
    {
        Refresh(true);
        GoTo(Current(shown) + delta);
    }

    // A small modal prompt for a checkpoint's name, in the style of the
    // workflow forms: Enter accepts, Escape cancels. Nothing in it runs
    // unless someone asked for it, so no automated path meets it unawares.
    struct Prompt
    {
        std::string label, value;
        bool done = false, accepted = false;
    };
    LRESULT CALLBACK PromptProc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
    {
        auto prompt = reinterpret_cast<Prompt*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            prompt = static_cast<Prompt*>(reinterpret_cast<CREATESTRUCTA*>(l)->lpCreateParams);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(prompt));
        }
        if (!prompt) return DefWindowProcA(hwnd, message, w, l);
        if (message == WM_CREATE)
        {
            auto add = [&](const char* cls, const char* text, int id, DWORD style, int x, int y, int cx, int cy)
            {
                HWND child = CreateWindowExA(cls[0] == 'E' ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, hwnd,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
                SendMessageA(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
                return child;
            };
            add("STATIC", prompt->label.c_str(), 10, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 12, 12, 356, 20);
            HWND edit = add("EDIT", prompt->value.c_str(), 11, WS_TABSTOP | ES_AUTOHSCROLL, 12, 36, 356, 24);
            SendMessageA(edit, EM_LIMITTEXT, Checkpoints::kNameLimit, 0);
            add("BUTTON", "OK", IDOK, WS_TABSTOP | BS_DEFPUSHBUTTON, 188, 72, 86, 28);
            add("BUTTON", "Cancel", IDCANCEL, WS_TABSTOP | BS_PUSHBUTTON, 282, 72, 86, 28);
            SetFocus(edit);
            SendMessageA(edit, EM_SETSEL, 0, -1);
            return 0;
        }
        if (message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL))
        {
            prompt->accepted = LOWORD(w) == IDOK;
            if (prompt->accepted)
            {
                char text[256] = {};
                GetWindowTextA(GetDlgItem(hwnd, 11), text, sizeof(text));
                prompt->value = text;
            }
            prompt->done = true;
            return 0;
        }
        if (message == WM_CLOSE) { prompt->done = true; return 0; }
        return DefWindowProcA(hwnd, message, w, l);
    }
    bool AskName(const char* title, const std::string& label, std::string& value)
    {
        static bool asking = false;
        if (asking) return false;
        asking = true;
        WNDCLASSA wc{};
        wc.lpfnWndProc = PromptProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = "ReloadedUndoCheckpointName";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        RegisterClassA(&wc);
        Prompt prompt{label, value};
        RECT owner{};
        GetWindowRect(window, &owner);
        RECT frame{0, 0, 380, 112};
        AdjustWindowRectEx(&frame, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE, WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT);
        HWND dialog = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, wc.lpszClassName, title, WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                      owner.left + 20, owner.top + 60, frame.right - frame.left, frame.bottom - frame.top, window, nullptr, wc.hInstance, &prompt);
        if (!dialog) { asking = false; return false; }
        EnableWindow(window, FALSE);
        ShowWindow(dialog, SW_SHOW);
        MSG msg{};
        while (!prompt.done && GetMessageA(&msg, nullptr, 0, 0) > 0)
            if (!IsDialogMessageA(dialog, &msg)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        // A WM_QUIT taken by this loop belongs to the editor's own loop.
        if (!prompt.done) PostQuitMessage(static_cast<int>(msg.wParam));
        EnableWindow(window, TRUE);
        DestroyWindow(dialog);
        SetActiveWindow(window);
        asking = false;
        if (prompt.accepted) value = prompt.value;
        return prompt.accepted;
    }

    // Names a row, the current one unless the list's menu chose another.
    void NameRow(int row)
    {
        Refresh(true);
        if (!available) { report = "Open a map first."; ShowStatus(); return; }
        row = std::clamp(row, 0, Count(shown));
        const auto existing = checkpoints.NameAt(row);
        std::string name = existing.empty() ? checkpoints.NextName() : existing;
        const std::string label = row == 0 ? "Name the start (before the oldest step):" : "Name step " + std::to_string(row) + " (" + CleanTitle(shown.titles[row - 1], 40) + "):";
        if (!AskName(existing.empty() ? "Add Checkpoint" : "Rename Checkpoint", label, name)) return;
        Refresh(true);
        if (!available || row > Count(shown)) { report = "The history changed; the checkpoint was not added."; ShowStatus(); return; }
        Address trans = 0;
        Checkpoints::Prints prints;
        try { trans = Buffer(); if (trans) prints = ReadPrints(trans); }
        catch (const std::exception& error) { report = error.what(); ShowStatus(); return; }
        if (!trans) { report = "Open a map first."; ShowStatus(); return; }
        NoteGone(checkpoints.Observe(prints));
        if (!checkpoints.Set(prints, row, name)) { report = "Give the checkpoint a name."; ShowStatus(); return; }
        ++checkpointVersion;
        report = "Checkpoint \"" + checkpoints.NameAt(row) + "\" on " + (row == 0 ? std::string("the start") : "step " + std::to_string(row)) + ".";
        Refresh(true);
    }

    void RemoveRow(int row)
    {
        const auto name = checkpoints.NameAt(row);
        if (name.empty() || !checkpoints.Remove(row)) return;
        ++checkpointVersion;
        report = "Removed checkpoint \"" + name + "\".";
        Refresh(true);
    }

    // The jump list's choice: walk there with the same steps as the list.
    void JumpToChoice()
    {
        HWND combo = Child(JumpList);
        const auto index = SendMessageA(combo, CB_GETCURSEL, 0, 0);
        if (index <= 0) return;
        const int row = static_cast<int>(SendMessageA(combo, CB_GETITEMDATA, index, 0));
        const auto name = checkpoints.NameAt(row);
        SendMessageA(combo, CB_SETCURSEL, 0, 0);
        GoTo(row);
        if (!name.empty() && available && Current(shown) == row) report += " At \"" + name + "\".";
        ShowStatus();
    }

    void CopyList()
    {
        std::string text;
        for (int row = 0; row < Rows(shown) && available; ++row)
            text += std::string(row == Current(shown) ? "> " : Undone(shown, row) ? "  (redo) " : "  ") + MarkedRowText(shown, row, checkpoints.NameAt(row)) + "\r\n";
        text += StatusText(shown) + "\r\n";
        if (!OpenClipboard(window)) return;
        EmptyClipboard();
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1))
        {
            memcpy(GlobalLock(memory), text.c_str(), text.size() + 1);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_TEXT, memory)) GlobalFree(memory);
        }
        CloseClipboard();
        report = "Copied " + std::to_string(Rows(shown)) + " rows.";
        ShowStatus();
    }

    std::wstring Wide(const std::string& text)
    {
        if (text.empty()) return {};
        const int count = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(count, L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
        return wide;
    }

    void DrawRow(const DRAWITEMSTRUCT& item)
    {
        if (item.itemID == static_cast<UINT>(-1)) return;
        const int row = static_cast<int>(item.itemID);
        const bool selected = (item.itemState & ODS_SELECTED) != 0, current = available && row == Current(shown);
        const bool undone = available && Undone(shown, row);
        HDC dc = item.hDC;
        RECT rect = item.rcItem;
        FillRect(dc, &rect, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : undone ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
        HGDIOBJ old = SelectObject(dc, current ? bold : font);
        RECT marker = rect;
        marker.left += 4;
        marker.right = marker.left + 14;
        if (current) DrawTextW(dc, L"\x25B6", 1, &marker, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        RECT text = rect;
        text.left = marker.right + 2;
        text.right -= 4;
        // A checkpoint: a coloured bar at the left edge and its name in a
        // tag at the right, in the system's link colour.
        if (const auto name = available ? checkpoints.NameAt(row) : std::string{}; !name.empty())
        {
            const COLORREF accent = GetSysColor(COLOR_HOTLIGHT);
            RECT bar{rect.left, rect.top + 1, rect.left + 3, rect.bottom - 1};
            HBRUSH brush = CreateSolidBrush(selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : accent);
            FillRect(dc, &bar, brush);
            DeleteObject(brush);
            const auto tag = Wide(name);
            SelectObject(dc, bold);
            RECT measure{0, 0, 0, 0};
            DrawTextW(dc, tag.c_str(), static_cast<int>(tag.size()), &measure, DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
            const int width = std::min<int>(measure.right + 12, std::max<int>(40, (text.right - text.left) / 2));
            RECT box{text.right - width, rect.top + 2, text.right, rect.bottom - 2};
            HPEN pen = CreatePen(PS_SOLID, 1, selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : accent);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, box.left, box.top, box.right, box.bottom, 6, 6);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            SetTextColor(dc, selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : accent);
            RECT inner{box.left + 6, box.top, box.right - 6, box.bottom};
            DrawTextW(dc, tag.c_str(), static_cast<int>(tag.size()), &inner, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, current ? bold : font);
            SetTextColor(dc, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : undone ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
            text.right = box.left - 6;
        }
        const auto line = Wide(RowText(shown, row) + (undone ? "   (redo)" : ""));
        DrawTextW(dc, line.c_str(), static_cast<int>(line.size()), &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        // A rule under the current point: what is below it is waiting for Redo.
        if (current && row < Count(shown))
        {
            HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_GRAYTEXT));
            HGDIOBJ previous = SelectObject(dc, pen);
            MoveToEx(dc, rect.left, rect.bottom - 1, nullptr);
            LineTo(dc, rect.right, rect.bottom - 1);
            SelectObject(dc, previous);
            DeleteObject(pen);
        }
        if (item.itemState & ODS_FOCUS) DrawFocusRect(dc, &rect);
    }

    void Layout(int width, int height)
    {
        const int statusHeight = Appearance::FitHeight(Child(Status), std::max(40, width - 16), 50, height / 3);
        const int buttons = height - 42 - statusHeight, status = height - 6 - statusHeight, top = 40;
        const int add = 130;
        MoveWindow(Child(JumpList), 8, 9, std::max(40, width - 16 - add - 6), 300, TRUE);
        MoveWindow(Child(AddButton), std::max(54, width - 8 - add), 8, add, 26, TRUE);
        MoveWindow(Child(List), 8, top, std::max(40, width - 16), std::max(40, buttons - 8 - top), TRUE);
        const int w = std::max(40, (width - 16 - 3 * 6) / 4);
        MoveWindow(Child(UndoButton), 8, buttons, w, 28, TRUE);
        MoveWindow(Child(RedoButton), 8 + (w + 6), buttons, w, 28, TRUE);
        MoveWindow(Child(GoButton), 8 + 2 * (w + 6), buttons, w, 28, TRUE);
        MoveWindow(Child(CopyButton), 8 + 3 * (w + 6), buttons, w, 28, TRUE);
        MoveWindow(Child(Status), 8, status, std::max(40, width - 16), statusHeight, TRUE);
    }

    int Selected() { return static_cast<int>(SendMessageA(Child(List), LB_GETCURSEL, 0, 0)); }

    LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
    {
        try
        {
            switch (message)
            {
            case WM_MEASUREITEM:
                reinterpret_cast<MEASUREITEMSTRUCT*>(l)->itemHeight = rowHeight;
                return TRUE;
            case WM_DRAWITEM:
                if (w == List) { DrawRow(*reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE; }
                break;
            case WM_SIZE:
                Layout(LOWORD(l), HIWORD(l));
                return 0;
            case WM_TIMER:
                if (w == kTimer) Refresh(false);
                return 0;
            case kChanged:
                InterlockedExchange(&changePosted, 0);
                Refresh(false);
                return 0;
            case WM_VKEYTOITEM:
            {
                const UINT key = LOWORD(w);
                const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (key == VK_RETURN) { GoTo(Selected()); return -2; }
                if (control && key == 'Z') { Move(-1); return -2; }
                if (control && key == 'Y') { Move(1); return -2; }
                if (key == VK_DELETE) { RemoveRow(Selected()); return -2; }
                return -1;
            }
            case WM_CONTEXTMENU:
            {
                if (reinterpret_cast<HWND>(w) != Child(List) || !available) break;
                POINT at{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
                int row = Selected();
                if (at.x != -1 || at.y != -1)
                {
                    POINT client = at;
                    ScreenToClient(Child(List), &client);
                    const auto hit = SendMessageA(Child(List), LB_ITEMFROMPOINT, 0, MAKELPARAM(client.x, client.y));
                    if (HIWORD(hit)) break; // below the last row
                    row = LOWORD(hit);
                    SendMessageA(Child(List), LB_SETCURSEL, row, 0);
                }
                else
                {
                    RECT item{};
                    SendMessageA(Child(List), LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&item));
                    at = {item.left + 20, item.bottom};
                    ClientToScreen(Child(List), &at);
                }
                if (row < 0) break;
                const bool marked = !checkpoints.NameAt(row).empty();
                HMENU menu = CreatePopupMenu();
                AppendMenuA(menu, MF_STRING, MenuGo, "&Go to this step");
                AppendMenuA(menu, MF_STRING, MenuName, marked ? "Re&name checkpoint..." : "Add &checkpoint here...");
                AppendMenuA(menu, MF_STRING | (marked ? 0 : MF_GRAYED), MenuRemove, "&Remove checkpoint\tDel");
                const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (choice == MenuGo) GoTo(row);
                else if (choice == MenuName) NameRow(row);
                else if (choice == MenuRemove) RemoveRow(row);
                return 0;
            }
            case WM_COMMAND:
            {
                const int id = LOWORD(w), code = HIWORD(w);
                if (id == AddButton) { Refresh(true); NameRow(Current(shown)); return 0; }
                if (id == JumpList && code == CBN_SELENDOK) { JumpToChoice(); return 0; }
                if (id == List && code == LBN_DBLCLK) { const int row = Selected(); if (row >= 0) GoTo(row); return 0; }
                if (id == UndoButton) { Move(-1); return 0; }
                if (id == RedoButton) { Move(1); return 0; }
                if (id == GoButton) { const int row = Selected(); if (row >= 0) GoTo(row); return 0; }
                if (id == CopyButton) { CopyList(); return 0; }
                break;
            }
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_NCDESTROY:
                WindowPlacement::Remember(hwnd, "UndoHistory");
                window = nullptr;
                if (font) DeleteObject(font);
                if (bold) DeleteObject(bold);
                font = bold = nullptr;
                break;
            }
        }
        catch (const std::exception& error)
        {
            report = error.what();
            if (window) ShowStatus();
        }
        return DefWindowProcA(hwnd, message, w, l);
    }

    HWND Owner()
    {
        Address frame = 0;
        if (CopySafe(&frame, reinterpret_cast<const void*>(kMainFrame), sizeof(frame)) && frame)
        {
            HWND hwnd = nullptr;
            if (CopySafe(&hwnd, reinterpret_cast<const void*>(frame + 4), sizeof(hwnd)) && IsWindow(hwnd)) return hwnd;
        }
        return GetActiveWindow();
    }
}

void Open()
{
    if (window)
    {
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window);
        Refresh(true);
        return;
    }
    WNDCLASSA wc{};
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpfnWndProc = Proc;
    wc.lpszClassName = "ReloadedUndoHistory";
    RegisterClassA(&wc);

    NONCLIENTMETRICSA metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    font = CreateFontIndirectA(&metrics.lfMessageFont);
    LOGFONTA heavy = metrics.lfMessageFont;
    heavy.lfWeight = FW_BOLD;
    bold = CreateFontIndirectA(&heavy);
    if (HDC dc = GetDC(nullptr))
    {
        HGDIOBJ old = SelectObject(dc, font);
        TEXTMETRICA tm{};
        GetTextMetricsA(dc, &tm);
        rowHeight = tm.tmHeight + 5;
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }

    // A tool window owned by the frame: it stays above the editor and goes
    // with it, without covering other programs.
    Observe();
    window = CreateWindowExA(WS_EX_TOOLWINDOW, wc.lpszClassName, "Undo History", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 380, 560, Owner(), nullptr, wc.hInstance, nullptr);
    if (!window) throw std::runtime_error("Cannot open the Undo History window.");
    WindowPlacement::Restore(window, "UndoHistory");
    auto add = [](const char* cls, const char* text, int id, DWORD style)
    {
        HWND child = CreateWindowExA(cls[0] == 'L' ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
        SendMessageA(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    };
    // No data in the list box itself: rows are drawn from the snapshot, so a
    // buffer of thousands of steps costs one LB_SETCOUNT.
    add("COMBOBOX", "", JumpList, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST);
    add("BUTTON", "Add c&heckpoint...", AddButton, WS_TABSTOP | BS_PUSHBUTTON);
    add("LISTBOX", "", List, WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_NODATA | LBS_NOINTEGRALHEIGHT | LBS_WANTKEYBOARDINPUT);
    add("BUTTON", "&Undo", UndoButton, WS_TABSTOP | BS_PUSHBUTTON);
    add("BUTTON", "&Redo", RedoButton, WS_TABSTOP | BS_PUSHBUTTON);
    add("BUTTON", "&Go to step", GoButton, WS_TABSTOP | BS_PUSHBUTTON);
    add("BUTTON", "&Copy list", CopyButton, WS_TABSTOP | BS_PUSHBUTTON);
    add("STATIC", "", Status, SS_LEFT | SS_NOPREFIX);
    RECT client{};
    GetClientRect(window, &client);
    Layout(client.right, client.bottom);
    shown = Snapshot{};
    seen = Signature{};
    shownVersion = ~0u;
    available = false;
    Refresh(true);
    report = "Double-click a step to undo or redo to it. Add checkpoint names the current step.";
    ShowStatus();
    SetTimer(window, kTimer, kTimerMs, nullptr);
    ShowWindow(window, SW_SHOW);
    SetFocus(Child(List));
}

bool HandleCommand(UINT command)
{
    if (command == kOpen) { Open(); return true; }
    if (command == kAddCheckpoint)
    {
        Open();
        if (window) { Refresh(true); NameRow(Current(shown)); }
        return true;
    }
    return false;
}

void InstallMenu(HMENU bar)
{
    if (!bar) return;
    for (int i = 0; i < GetMenuItemCount(bar); ++i)
    {
        HMENU sub = GetSubMenu(bar, i);
        if (!sub || GetMenuState(sub, 40020, MF_BYCOMMAND) == UINT(-1)) continue;
        if (GetMenuState(sub, kOpen, MF_BYCOMMAND) != UINT(-1)) return;
        for (int pos = 0; pos < GetMenuItemCount(sub); ++pos)
            if (GetMenuItemID(sub, pos) == 40020)
            {
                InsertMenuA(sub, pos + 1, MF_BYPOSITION | MF_STRING, kOpen, "Undo &History...");
                InsertMenuA(sub, pos + 2, MF_BYPOSITION | MF_STRING, kAddCheckpoint, "Add Undo Chec&kpoint...");
                return;
            }
    }
}
}
