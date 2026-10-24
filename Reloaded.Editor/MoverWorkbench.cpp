#include "pch.h"
#undef min
#undef max
#include "MoverWorkbench.h"
#include "MoverModel.h"
#include "BudgetWindow.h"
#include "MeasureTool.h"
#include "WindowPlacement.h"
#include "Appearance.h"
#include "WorkflowEditor.h"
#include "WorkflowTools.h"
#include "logger.h"
#include <commctrl.h>
#include <optional>
#include <stdexcept>
#pragma comment(lib, "comctl32.lib")

namespace MoverWorkbench
{
namespace
{
using Workflow::Json;
using Workflow::Pose;
namespace Editor = Workflow::Editor;
namespace Mov = Workflow::Movers;

const char kTitle[] = "Mover Workbench";
const char kPlacement[] = "MoverWorkbench";
const char kOverlay[] = "mover";
constexpr UINT_PTR kFollowTimer = 1, kPreviewTimer = 2;

enum class Kind { Seconds, Whole, Choice, Check, Text };
struct FieldDef { const char* property; const char* label; Kind kind; int column; };
const FieldDef kFields[] = {
    { "MoveTime", "Seconds per key to key", Kind::Seconds, 0 },
    { "StayOpenTime", "Stays open for (s)", Kind::Seconds, 0 },
    { "DelayTime", "DelayTime (s)", Kind::Seconds, 0 },
    { "DelayTimeLastKey", "DelayTimeLastKey (s)", Kind::Seconds, 0 },
    { "InitialState", "Starts moving when", Kind::Choice, 0 },
    { "MoverEncroachType", "When something is in the way", Kind::Choice, 0 },
    { "EncroachDamage", "Damage when in the way", Kind::Whole, 0 },
    { "MoverGlideType", "Motion", Kind::Choice, 0 },
    { "BumpType", "Bumped by", Kind::Choice, 0 },
    { "bTriggerOnceOnly", "Can be triggered once only", Kind::Check, 0 },
    { "bUseShortestRotation", "Turns the short way round", Kind::Check, 0 },
    { "Event", "Event it triggers", Kind::Text, 1 },
    { "OpeningSound", "Opening sound", Kind::Text, 1 },
    { "OpenedSound", "Opened sound", Kind::Text, 1 },
    { "ClosingSound", "Closing sound", Kind::Text, 1 },
    { "ClosedSound", "Closed sound", Kind::Text, 1 },
    { "MoveAmbientSound", "Sound while moving", Kind::Text, 1 },
};
constexpr int kFieldCount = static_cast<int>(sizeof(kFields) / sizeof(kFields[0]));
constexpr int kLabelFirst = 300;
constexpr int kBottom = 380; // the settings and wiring below the key list

struct State
{
    HWND window = nullptr, list = nullptr;
    Json identity;                 // the mover shown
    Json state;                    // its last MoverState
    std::optional<Mov::Path> rest; // the path when last seen at rest (MoverModel.h Compare)
    unsigned revision = ~0u;
    uintptr_t level = 0;
    std::vector<std::vector<std::string>> choices; // per field: the raw values in its combo
    Json outline;                  // edges at the mover's place, read at outlineRevision
    unsigned outlineRevision = ~0u;
    Json candidates = Json::array();
    bool previewing = false;
    DWORD previewStart = 0;
    Mov::Path previewPath;
    Mov::Timing previewTiming;
    std::string players;           // the Players in Match label
} s;

std::string Trim(double v)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.2f", v);
    std::string t = text;
    while (!t.empty() && t.back() == '0') t.pop_back();
    if (!t.empty() && t.back() == '.') t.pop_back();
    return t == "-0" ? "0" : t;
}
std::string Text(HWND control)
{
    const int length = GetWindowTextLengthA(control);
    std::string text(static_cast<size_t>(std::max(0, length)) + 1, '\0');
    GetWindowTextA(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(std::max(0, length)));
    return text;
}
HWND Item(int id) { return GetDlgItem(s.window, id); }
void SetStatus(const std::string& text) { if (s.window) SetWindowTextA(Item(kStatus), text.c_str()); }
std::string ValueText(const Json& value) { return value.is_string() ? value.get<std::string>() : value.is_null() ? std::string() : value.dump(); }

Mov::Path LivePath() { return Mov::PathFrom(s.state.at("path")); }
Pose Place() { return { s.state.at("location").get<Workflow::Vector>(), s.state.at("rotation").get<Workflow::Rotation>() }; }
int SelectedKey() { return ListView_GetNextItem(s.list, -1, LVNI_SELECTED); }
void SelectKey(int key)
{
    if (key < 0 || key >= ListView_GetItemCount(s.list)) return;
    ListView_SetItemState(s.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(s.list, key, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(s.list, key, FALSE);
}
Mov::Timing TimingOf(const Json& values)
{
    Mov::Timing t;
    auto number = [&](const char* name, double fallback) {
        try { return values.contains(name) ? Mov::ParseNumber(values.at(name)) : fallback; } catch (const std::exception&) { return fallback; }
    };
    t.moveTime = number("MoveTime", 1);
    t.stayOpenTime = number("StayOpenTime", 0);
    t.glide = Workflow::Fold(ValueText(values.value("MoverGlideType", Json("MV_GlideByTime")))) != "mv_movebytime";
    t.shortest = Workflow::Fold(ValueText(values.value("bUseShortestRotation", Json("False")))) == "true";
    return t;
}

// ---- ghosts: the mover's outline at every key, and the line through the keys

void ClearGhosts() { MeasureTool::SetOverlay(kOverlay, {}); }
void ShowGhosts()
{
    if (!s.window || s.state.is_null() || SendMessageA(Item(kGhosts), BM_GETCHECK, 0, 0) != BST_CHECKED) { ClearGhosts(); return; }
    if (s.outlineRevision != s.revision || s.outline.is_null())
    {
        s.outline = Editor::MoverOutline(s.identity);
        s.outlineRevision = s.revision;
    }
    std::vector<Mov::Edge> edges;
    for (const auto& e : s.outline.at("edges")) edges.push_back({ e.at(0).get<Workflow::Vector>(), e.at(1).get<Workflow::Vector>() });
    edges = Mov::Simplify(edges);
    const Pose place{ s.outline.at("location").get<Workflow::Vector>(), s.outline.at("rotation").get<Workflow::Rotation>() };
    const auto path = LivePath();
    const bool onKey = Mov::OnKey(path, Place());
    const int selected = SelectedKey();
    std::vector<MeasureTool::OverlayLine> lines;
    auto add = [&](const Workflow::Vector& a, const Workflow::Vector& b, uint32_t colour) {
        lines.push_back({ { a[0], a[1], a[2] }, { b[0], b[1], b[2] }, colour });
    };
    for (int k = 0; k < static_cast<int>(path.keys.size()); ++k)
    {
        if (onKey && k == path.current && k != selected) continue; // the mover itself is drawn there
        const uint32_t colour = k == selected ? 0xFFFFFF40u : k == 0 ? 0xFF60FF60u : 0xFF40C0FFu;
        for (const auto& e : Mov::Ghost(edges, place, Mov::At(path, k))) add(e.from, e.to, colour);
    }
    for (int k = 0; k + 1 < static_cast<int>(path.keys.size()); ++k)
        add(Mov::At(path, k).position, Mov::At(path, k + 1).position, 0xFFFF9020u);
    MeasureTool::SetOverlay(kOverlay, lines);
}

// ---- the window's contents

void FillList()
{
    const int keep = SelectedKey();
    const auto path = LivePath();
    const auto timing = TimingOf(s.state.at("values"));
    const bool onKey = Mov::OnKey(path, Place());
    SendMessageA(s.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(s.list);
    for (int k = 0; k < static_cast<int>(path.keys.size()); ++k)
    {
        const auto& key = path.keys[k];
        std::string here = k == path.current ? (onKey ? "here" : "here (moved off it)") : "";
        Budget::Row(s.list, k, k, { std::to_string(k) + (k == 0 ? " (closed)" : ""),
            Trim(key.position[0]), Trim(key.position[1]), Trim(key.position[2]),
            std::to_string(key.rotation[0]), std::to_string(key.rotation[1]), std::to_string(key.rotation[2]),
            Trim(Mov::Arrival(timing, k)) + " s", here });
    }
    SendMessageA(s.list, WM_SETREDRAW, TRUE, 0);
    SelectKey(keep >= 0 && keep < static_cast<int>(path.keys.size()) ? keep : path.current);
}
void FillChoice(int field, const Json& values, const Json& choices)
{
    const auto& def = kFields[field];
    HWND combo = Item(kFieldFirst + field);
    const auto current = ValueText(values.value(def.property, Json("")));
    std::vector<std::string> raw;
    if (std::string(def.property) == "InitialState")
    {
        raw.push_back("None");
        for (const auto& state : choices.value(def.property, Json::array()))
        {
            const auto name = state.get<std::string>();
            if (!Mov::InternalState(name) || Workflow::Fold(name) == Workflow::Fold(current)) raw.push_back(name);
        }
    }
    else for (const auto& choice : choices.value(def.property, Json::array())) raw.push_back(choice.get<std::string>());
    if (std::none_of(raw.begin(), raw.end(), [&](const std::string& r) { return Workflow::Fold(r) == Workflow::Fold(current); }) && !current.empty()) raw.push_back(current);
    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    int chosen = -1;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        std::string label = std::string(def.property) == "InitialState" ? Mov::StateLabel(raw[i]) : Mov::ChoiceLabel(def.property, raw[i]);
        if (std::string(def.property) == "InitialState" && label != raw[i]) label += " (" + raw[i] + ")";
        SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (Workflow::Fold(raw[i]) == Workflow::Fold(current)) chosen = static_cast<int>(i);
    }
    SendMessageA(combo, CB_SETCURSEL, chosen, 0);
    s.choices[field] = raw;
}
// The fields show the mover's values; a field whose value did not change
// since the last fill keeps what is typed in it.
void FillFields(const Json& previous, bool force)
{
    const auto& values = s.state.at("values");
    const auto& choices = s.state.at("choices");
    for (int i = 0; i < kFieldCount; ++i)
    {
        const auto& def = kFields[i];
        const bool present = values.contains(def.property);
        EnableWindow(Item(kFieldFirst + i), present);
        if (!present) continue;
        const auto value = values.at(def.property);
        if (!force && previous.is_object() && previous.contains(def.property) && previous.at(def.property) == value) continue;
        HWND control = Item(kFieldFirst + i);
        switch (def.kind)
        {
        case Kind::Seconds: SetWindowTextA(control, Trim(Mov::ParseNumber(value)).c_str()); break;
        case Kind::Whole: case Kind::Text:
        {
            auto text = ValueText(value);
            if (def.kind == Kind::Text && Workflow::Fold(text) == "none") text.clear();
            SetWindowTextA(control, text.c_str());
            break;
        }
        case Kind::Check: SendMessageA(control, BM_SETCHECK, Workflow::Fold(ValueText(value)) == "true" ? BST_CHECKED : BST_UNCHECKED, 0); break;
        case Kind::Choice: FillChoice(i, values, choices); break;
        }
    }
}
void FillWiring()
{
    const auto& values = s.state.at("values");
    const auto tag = ValueText(values.value("Tag", Json("None")));
    std::string by;
    for (const auto& link : s.state.at("triggeredBy"))
    {
        auto actor = link.at("actor").get<std::string>();
        actor = actor.substr(actor.find_last_of('.') + 1);
        if (!by.empty()) by += ", ";
        by += actor + (link.at("property").get<std::string>() == "Event" ? "" : " (" + link.at("property").get<std::string>() + ")");
    }
    std::string text = "Tag: " + tag + ".  Triggered by: " + (by.empty() ? "nothing yet (wire a trigger or button below)." : by + ".");
    SetWindowTextA(Item(kTriggeredBy), text.c_str());
    // The Players in Match rule, as the actor menu shows it.
    s.players.clear();
    if (HMENU menu = CreatePopupMenu())
    {
        try
        {
            if (WorkflowTools::AppendObjectiveMenu(menu, s.identity))
            {
                char label[256] = {};
                GetMenuStringA(menu, GetMenuItemCount(menu) - 1, label, sizeof(label), MF_BYPOSITION);
                for (char c : std::string(label)) if (c != '&') s.players += c;
            }
        }
        catch (const std::exception&) {}
        DestroyMenu(menu);
    }
    SetWindowTextA(Item(kPlayersText), s.players.empty() ? "Players in Match: not available for this mover." : (s.players + ".").c_str());
    EnableWindow(Item(kPlayers), !s.players.empty());
}
void FillCandidates()
{
    HWND combo = Item(kWireList);
    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    s.candidates = Editor::MoverTriggerCandidates(s.identity);
    const auto tag = Workflow::Fold(ValueText(s.state.at("values").value("Tag", Json("None"))));
    for (const auto& c : s.candidates)
    {
        auto path = c.at("path").get<std::string>(), cls = c.at("class").get<std::string>(), event = c.at("event").get<std::string>();
        std::string label = path.substr(path.find_last_of('.') + 1) + " (" + cls.substr(cls.find_last_of('.') + 1) + ")";
        if (Workflow::Fold(event) == tag && tag != "none") label += " - already wired";
        else if (Workflow::Fold(event) != "none" && !event.empty()) label += " - Event " + event;
        SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageA(combo, CB_SETCURSEL, s.candidates.empty() ? -1 : 0, 0);
}
void FillInfo()
{
    const auto path = LivePath();
    const auto name = s.state.at("name").get<std::string>(), cls = s.state.at("class").get<std::string>();
    std::string text = name + " (" + cls.substr(cls.find_last_of('.') + 1) + "): " + std::to_string(path.keys.size()) + " of " + std::to_string(path.capacity)
        + " keys, sitting on key " + std::to_string(path.current) + ". Offsets are from key 0's base; rotations in Unreal units (16384 = 90 degrees).";
    if (path.current != 0) text += " In game it starts at key " + std::to_string(path.current) + ": Go to Key 0 before saving for a closed start.";
    SetWindowTextA(Item(kInfo), text.c_str());
}
void Show(const Json& next, bool forceFields)
{
    const Json previous = s.state.is_null() ? Json() : s.state.at("values");
    const bool sameMover = !s.state.is_null() && s.state.at("actor") == next.at("actor");
    s.state = next;
    s.revision = next.at("revision").get<unsigned>();
    s.level = next.at("level").get<uintptr_t>();
    const auto live = LivePath();
    const auto place = Place();
    std::string moved;
    if (!sameMover || !s.rest) s.rest = live;
    else
    {
        switch (Mov::Compare(*s.rest, live, place))
        {
        case Mov::Moved::No: case Mov::Moved::Changed: s.rest = live; break;
        case Mov::Moved::OffKey:
            moved = "Moved off key " + std::to_string(live.current) + ": Add Key Here makes this place a new key; Update Key stores it in the selected key.";
            break;
        case Mov::Moved::Dragged:
            moved = live.current == 0
                ? "Moved at key 0: the editor takes the whole path along. Add Key Here keeps the path where it was and makes this place a new key."
                : "Moved at key " + std::to_string(live.current) + ": the editor stored it in key " + std::to_string(live.current)
                  + ". Add Key Here puts that key back and makes this place a new key; Update Key keeps it.";
            break;
        }
    }
    FillInfo();
    FillList();
    FillFields(sameMover ? previous : Json(), forceFields || !sameMover);
    FillWiring();
    if (!sameMover) FillCandidates();
    ShowGhosts();
    if (!moved.empty()) SetStatus(moved);
}
void Load(const Json& identity, bool forceFields = true)
{
    if (!identity.is_null() && (s.identity.is_null() || s.identity != identity))
    {
        s.identity = identity;
        s.state = Json();
        s.rest.reset();
        s.outline = Json();
    }
    if (s.identity.is_null()) { SetWindowTextA(Item(kInfo), "Select a mover in the map (one actor), then press Refresh or keep the window open: it follows the selection."); return; }
    Show(Editor::MoverState(s.identity), forceFields);
}

// ---- edits

void Apply(const Mov::Path& next, const std::string& label, const std::string& done)
{
    Show(Editor::SetMoverPath(s.identity, Mov::PathJson(next), label), false);
    s.rest = LivePath();
    SelectKey(next.current);
    ShowGhosts();
    SetStatus(done + " One Undo step.");
}
void KeyCommand(int id)
{
    if (s.state.is_null()) throw std::runtime_error("Select a mover first.");
    Load(Json(), false); // fresh values before any edit
    const auto live = LivePath();
    const auto place = Place();
    const int selected = SelectedKey();
    const auto& reference = Mov::MeasuredAgainst(s.rest ? &*s.rest : nullptr, live, place);
    switch (id)
    {
    case kAddKey:
    {
        auto next = Mov::AddKey(reference, place);
        Apply(next, "add key " + std::to_string(next.current), "Added key " + std::to_string(next.current) + " where the mover is.");
        break;
    }
    case kUpdateKey:
    {
        if (selected < 0) throw std::runtime_error("Select the key to update in the list.");
        Apply(Mov::UpdateKey(reference, selected, place), "update key " + std::to_string(selected), "Key " + std::to_string(selected) + " is now where the mover is.");
        break;
    }
    case kDeleteKey:
    {
        if (selected < 0) throw std::runtime_error("Select the key to delete in the list.");
        auto next = Mov::DeleteKey(live, selected);
        Apply(next, "delete key " + std::to_string(selected), "Deleted key " + std::to_string(selected) + "; the mover sits on key " + std::to_string(next.current) + ".");
        break;
    }
    case kMoveUp: case kMoveDown:
    {
        if (selected < 0) throw std::runtime_error("Select the key to move in the list.");
        const int direction = id == kMoveUp ? -1 : 1;
        auto next = Mov::MoveKey(live, selected, direction);
        Apply(next, "move key " + std::to_string(selected), "Key " + std::to_string(selected) + " is now key " + std::to_string(selected + direction) + ".");
        SelectKey(selected + direction);
        ShowGhosts();
        break;
    }
    case kGoToKey:
    {
        if (selected < 0) throw std::runtime_error("Select the key to go to in the list.");
        Apply(Mov::GoTo(live, selected), "go to key " + std::to_string(selected), "The mover sits on key " + std::to_string(selected) + ", as setting KeyNum does.");
        break;
    }
    }
}
void ApplySettings()
{
    if (s.state.is_null()) throw std::runtime_error("Select a mover first.");
    Load(Json(), false);
    const auto& values = s.state.at("values");
    Json changes = Json::object();
    for (int i = 0; i < kFieldCount; ++i)
    {
        const auto& def = kFields[i];
        if (!values.contains(def.property)) continue;
        HWND control = Item(kFieldFirst + i);
        const auto current = ValueText(values.at(def.property));
        std::string wanted;
        switch (def.kind)
        {
        case Kind::Seconds:
        {
            const double v = Mov::Seconds(Text(control), def.label);
            if (std::abs(v - Mov::ParseNumber(values.at(def.property))) < 1e-6) continue;
            wanted = Mov::Number(v);
            break;
        }
        case Kind::Whole:
        {
            const auto text = Text(control);
            size_t used = 0;
            int v = 0;
            try { v = std::stoi(text, &used); } catch (const std::exception&) { used = 0; }
            if (text.empty() || used != text.size() || v < 0 || v > 100000) throw std::runtime_error(std::string(def.label) + " must be a whole number, 0 to 100000.");
            wanted = std::to_string(v);
            break;
        }
        case Kind::Check: wanted = SendMessageA(control, BM_GETCHECK, 0, 0) == BST_CHECKED ? "True" : "False"; break;
        case Kind::Choice:
        {
            const auto at = SendMessageA(control, CB_GETCURSEL, 0, 0);
            if (at < 0 || at >= static_cast<LRESULT>(s.choices[i].size())) continue;
            wanted = s.choices[i][static_cast<size_t>(at)];
            break;
        }
        case Kind::Text:
        {
            wanted = Text(control);
            while (!wanted.empty() && wanted.back() == ' ') wanted.pop_back();
            while (!wanted.empty() && wanted.front() == ' ') wanted.erase(wanted.begin());
            if (wanted.empty()) wanted = "None";
            break;
        }
        }
        if (Workflow::Fold(wanted) != Workflow::Fold(current)) changes[def.property] = wanted;
    }
    if (changes.empty()) { SetStatus("Nothing to apply: the settings are as the mover has them."); return; }
    Show(Editor::SetMoverSettings(s.identity, changes), true);
    std::string names;
    for (auto it = changes.begin(); it != changes.end(); ++it) names += (names.empty() ? "" : ", ") + it.key();
    SetStatus("Applied " + names + ". One Undo step.");
}
void Wire()
{
    if (s.state.is_null()) throw std::runtime_error("Select a mover first.");
    const auto at = SendMessageA(Item(kWireList), CB_GETCURSEL, 0, 0);
    if (at < 0 || at >= static_cast<LRESULT>(s.candidates.size())) throw std::runtime_error("Choose the trigger or button to wire to this mover.");
    const Json chosen = s.candidates.at(static_cast<size_t>(at)); // a copy: FillCandidates replaces the list
    auto result = Editor::WireMoverTrigger(s.identity, { { "path", chosen.at("path") }, { "class", chosen.at("class") } });
    Show(result.at("state"), true);
    FillCandidates();
    auto path = chosen.at("path").get<std::string>();
    SetStatus(path.substr(path.find_last_of('.') + 1) + " now triggers Tag " + result.at("tag").get<std::string>()
              + (result.at("newTag").get<bool>() ? " (a new Tag for this mover)." : ".") + " One Undo step.");
}
void Players()
{
    if (s.state.is_null()) throw std::runtime_error("Select a mover first.");
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    UINT command = 0;
    try
    {
        if (WorkflowTools::AppendObjectiveMenu(menu, s.identity))
            if (HMENU sub = GetSubMenu(menu, GetMenuItemCount(menu) - 1))
            {
                RECT r{};
                GetWindowRect(Item(kPlayers), &r);
                command = static_cast<UINT>(TrackPopupMenu(sub, TPM_RETURNCMD | TPM_RIGHTBUTTON, r.left, r.bottom, 0, s.window, nullptr));
            }
    }
    catch (...) { DestroyMenu(menu); throw; }
    DestroyMenu(menu);
    if (!command) return;
    WorkflowTools::RunObjectiveCommand(command, { { "actor", s.identity } });
    Load(Json(), false);
}

// ---- preview

void StopPreview(const char* why)
{
    if (!s.previewing) return;
    s.previewing = false;
    KillTimer(s.window, kPreviewTimer);
    Editor::MoverRestorePose();
    SetWindowTextA(Item(kPreview), "&Preview");
    for (int id : { kAddKey, kUpdateKey, kDeleteKey, kMoveUp, kMoveDown, kGoToKey, kApply, kWire }) EnableWindow(Item(id), TRUE);
    ShowGhosts();
    if (why) SetStatus(why);
}
void StartPreview()
{
    if (s.state.is_null()) throw std::runtime_error("Select a mover first.");
    Load(Json(), false);
    s.previewPath = LivePath();
    s.previewTiming = TimingOf(s.state.at("values"));
    const double length = Mov::PreviewLength(s.previewPath, s.previewTiming);
    if (length <= 0) throw std::runtime_error("The mover has one key: add a key to have something to play.");
    s.previewing = true;
    s.previewStart = GetTickCount();
    SetWindowTextA(Item(kPreview), "Stop &Preview");
    for (int id : { kAddKey, kUpdateKey, kDeleteKey, kMoveUp, kMoveDown, kGoToKey, kApply, kWire }) EnableWindow(Item(id), FALSE);
    SetTimer(s.window, kPreviewTimer, 33, nullptr);
    SetStatus("Playing out through " + std::to_string(s.previewPath.keys.size() - 1) + " legs and back (" + Trim(length) + " s); it goes back where it was after.");
}
void PreviewTick()
{
    const double seconds = (GetTickCount() - s.previewStart) / 1000.0;
    if (seconds > Mov::PreviewLength(s.previewPath, s.previewTiming)) { StopPreview("Preview finished; the mover is back where it was."); return; }
    Editor::MoverPreviewPose(s.identity, Mov::PreviewAt(s.previewPath, s.previewTiming, seconds));
}

// ---- following the map

void Follow()
{
    if (s.previewing) return;
    if (Editor::LevelIdentity() != s.level && !s.state.is_null())
    {
        s.identity = Json(); s.state = Json(); s.rest.reset(); s.outline = Json();
        ClearGhosts();
        ListView_DeleteAllItems(s.list);
    }
    const auto selected = Editor::SelectedMover();
    if (!selected.is_null() && selected != s.identity) { Load(selected); SetStatus("Showing the selected mover."); return; }
    if (s.identity.is_null()) { Load(Json()); return; }
    if (Editor::ContentRevision() != s.revision)
    {
        try { Load(Json(), false); }
        catch (const std::exception&)
        {
            s.identity = Json(); s.state = Json(); s.rest.reset();
            ClearGhosts();
            ListView_DeleteAllItems(s.list);
            Load(Json());
            SetStatus("The mover shown is no longer in the map.");
        }
    }
}

// ---- layout

void Layout()
{
    RECT r{};
    GetClientRect(s.window, &r);
    const int width = r.right, height = r.bottom;
    const int listTop = 44, listBottom = std::max(listTop + 80, height - kBottom);
    MoveWindow(Item(kInfo), 12, 6, width - 24, 34, TRUE);
    MoveWindow(s.list, 12, listTop, width - 24, listBottom - listTop, TRUE);
    int y = listBottom + 6, x = 12;
    for (int id : { kAddKey, kUpdateKey, kDeleteKey, kMoveUp, kMoveDown, kGoToKey, kPreview, kRefresh })
    {
        const int w = id == kAddKey ? 100 : id == kPreview ? 96 : 84;
        MoveWindow(Item(id), x, y, w, 26, TRUE);
        x += w + 6;
    }
    y += 30;
    MoveWindow(Item(kGhosts), 12, y, 220, 22, TRUE);
    MoveWindow(Item(kStatus), 240, y, width - 252, 40, TRUE);
    y += 44;
    // The right column's labels ("Sound while moving:") are wider in the larger modern font.
    const int top = y, left = 12, right = width * 11 / 20, labelLeft = 170, labelRight = Appearance::Current() == Appearance::Theme::Classic ? 116 : 136;
    int ly = top, ry = top;
    for (int i = 0; i < kFieldCount; ++i)
    {
        const auto& def = kFields[i];
        HWND label = Item(kLabelFirst + i), control = Item(kFieldFirst + i);
        if (def.column == 0)
        {
            if (def.kind == Kind::Check) { MoveWindow(control, left, ly, right - left - 20, 20, TRUE); ShowWindow(label, SW_HIDE); ly += 22; continue; }
            MoveWindow(label, left, ly + 3, labelLeft - 6, 18, TRUE);
            MoveWindow(control, left + labelLeft, ly, right - left - labelLeft - 20, def.kind == Kind::Choice ? 300 : 22, TRUE);
            ly += 25;
        }
        else
        {
            MoveWindow(label, right, ry + 3, labelRight - 6, 18, TRUE);
            MoveWindow(control, right + labelRight, ry, width - right - labelRight - 12, 22, TRUE);
            ry += 25;
        }
    }
    MoveWindow(Item(kApply), right + labelRight, ry + 2, 140, 26, TRUE);
    ry += 34;
    MoveWindow(Item(kTriggeredBy), right, ry, width - right - 12, 34, TRUE);
    ry += 36;
    MoveWindow(Item(kRenameTag), right, ry, 100, 24, TRUE);
    MoveWindow(Item(kWireList), right + 106, ry, width - right - 106 - 12 - 136, 300, TRUE);
    MoveWindow(Item(kWire), width - 12 - 130, ry, 130, 24, TRUE);
    ry += 30;
    MoveWindow(Item(kPlayers), right, ry, 130, 24, TRUE);
    MoveWindow(Item(kPlayersText), right + 136, ry + 4, width - right - 136 - 12, 34, TRUE);
}

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    try
    {
        switch (message)
        {
        case WM_CREATE:
        {
            s.window = window;
            auto control = [&](const char* type, const char* text, DWORD style, int id) { return Budget::Control(window, type, text, style, id, 0, 0, 10, 10); };
            control("STATIC", "", SS_LEFT, kInfo);
            s.list = Budget::ReportList(window, kList);
            Budget::Columns(s.list, { { "Key", 80 }, { "Offset X", 70 }, { "Y", 70 }, { "Z", 70 }, { "Pitch", 60 }, { "Yaw", 60 }, { "Roll", 60 }, { "Arrives", 70 }, { "Mover", 130 } }, { "Mover" });
            control("BUTTON", "&Add Key Here", BS_PUSHBUTTON | WS_TABSTOP, kAddKey);
            control("BUTTON", "&Update Key", BS_PUSHBUTTON | WS_TABSTOP, kUpdateKey);
            control("BUTTON", "&Delete Key", BS_PUSHBUTTON | WS_TABSTOP, kDeleteKey);
            control("BUTTON", "Move U&p", BS_PUSHBUTTON | WS_TABSTOP, kMoveUp);
            control("BUTTON", "Move Do&wn", BS_PUSHBUTTON | WS_TABSTOP, kMoveDown);
            control("BUTTON", "&Go to Key", BS_PUSHBUTTON | WS_TABSTOP, kGoToKey);
            control("BUTTON", "&Preview", BS_PUSHBUTTON | WS_TABSTOP, kPreview);
            control("BUTTON", "&Refresh", BS_PUSHBUTTON | WS_TABSTOP, kRefresh);
            HWND ghosts = control("BUTTON", "Show ghost outlines at each key", BS_AUTOCHECKBOX | WS_TABSTOP, kGhosts);
            SendMessageA(ghosts, BM_SETCHECK, BST_CHECKED, 0);
            control("STATIC", "Move the mover in a viewport, then Add Key Here. Go to Key moves it to a key, as setting KeyNum does.", SS_LEFT, kStatus);
            for (int i = 0; i < kFieldCount; ++i)
            {
                const auto& def = kFields[i];
                control("STATIC", (std::string(def.label) + ":").c_str(), SS_LEFT, kLabelFirst + i);
                switch (def.kind)
                {
                case Kind::Choice: control("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kFieldFirst + i); break;
                case Kind::Check: control("BUTTON", def.label, BS_AUTOCHECKBOX | WS_TABSTOP, kFieldFirst + i); break;
                default: control("EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, kFieldFirst + i); break;
                }
            }
            s.choices.assign(kFieldCount, {});
            control("BUTTON", "Apply &Settings", BS_PUSHBUTTON | WS_TABSTOP, kApply);
            control("STATIC", "", SS_LEFT, kTriggeredBy);
            control("BUTTON", "Rename &Tag...", BS_PUSHBUTTON | WS_TABSTOP, kRenameTag);
            control("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kWireList);
            control("BUTTON", "Wire to This &Mover", BS_PUSHBUTTON | WS_TABSTOP, kWire);
            control("BUTTON", "Players in Match...", BS_PUSHBUTTON | WS_TABSTOP, kPlayers);
            control("STATIC", "", SS_LEFT, kPlayersText);
            Layout();
            SetTimer(window, kFollowTimer, 500, nullptr);
            return 0;
        }
        case WM_SIZE: if (s.list) Layout(); return 0;
        case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = { 860, 560 }; return 0;
        case WM_TIMER:
            if (w == kPreviewTimer) PreviewTick();
            else if (w == kFollowTimer) Follow();
            return 0;
        case WM_NOTIFY:
        {
            auto header = reinterpret_cast<NMHDR*>(l);
            if (header->hwndFrom == s.list && header->code == LVN_ITEMCHANGED)
            {
                auto change = reinterpret_cast<NMLISTVIEW*>(l);
                if ((change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED) && (change->uNewState & LVIS_SELECTED)) ShowGhosts();
            }
            if (header->hwndFrom == s.list && header->code == NM_DBLCLK && !s.previewing) KeyCommand(kGoToKey);
            return 0;
        }
        case WM_COMMAND:
        {
            const int id = LOWORD(w);
            switch (id)
            {
            case kAddKey: case kUpdateKey: case kDeleteKey: case kMoveUp: case kMoveDown: case kGoToKey:
                if (!s.previewing) KeyCommand(id);
                return 0;
            case kPreview: if (s.previewing) StopPreview("Preview stopped; the mover is back where it was."); else StartPreview(); return 0;
            case kRefresh:
                if (!s.previewing)
                {
                    const auto selected = Editor::SelectedMover();
                    if (!selected.is_null()) s.identity = selected;
                    s.rest.reset();
                    s.outline = Json();
                    if (!s.identity.is_null()) { s.state = Json(); Show(Editor::MoverState(s.identity), true); FillCandidates(); }
                    else Load(Json());
                    SetStatus("Refreshed; the mover's place now counts as at rest.");
                }
                return 0;
            case kGhosts: ShowGhosts(); return 0;
            case kApply: if (!s.previewing) ApplySettings(); return 0;
            case kWire: if (!s.previewing) Wire(); return 0;
            case kPlayers: if (!s.previewing) Players(); return 0;
            case kRenameTag:
                if (!s.previewing && !s.identity.is_null())
                {
                    WorkflowTools::RenameActorTag(window, s.identity.at("path").get<std::string>(), s.identity.at("class").get<std::string>());
                    Load(Json(), true);
                    FillCandidates();
                }
                return 0;
            case IDCANCEL: DestroyWindow(window); return 0;
            }
            break;
        }
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_DESTROY:
            KillTimer(window, kFollowTimer);
            if (s.previewing) { s.previewing = false; KillTimer(window, kPreviewTimer); Editor::MoverRestorePose(); }
            ClearGhosts();
            WindowPlacement::Remember(window, kPlacement);
            s = State{};
            return 0;
        }
    }
    catch (const std::exception& e)
    {
        // A modal box here would stall the in-editor tests; report in the status line.
        SetStatus(e.what());
        if (message == WM_CREATE) return -1;
        return 0;
    }
    return DefWindowProcA(window, message, w, l);
}
} // namespace

void Open(HWND owner)
{
    if (!IsWindow(s.window))
    {
        INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&controls);
        Budget::RegisterClassOnce("ReloadedMoverWorkbench", Proc);
        HWND main = Editor::MainWindow();
        HWND window = CreateWindowExA(WS_EX_TOOLWINDOW, "ReloadedMoverWorkbench", kTitle, WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 1000, 720, main ? main : owner, nullptr, GetModuleHandle(nullptr), nullptr);
        if (!window) throw std::runtime_error("Could not open the Mover Workbench.");
        WindowPlacement::Restore(window, kPlacement);
        ShowWindow(window, SW_SHOW);
    }
    else
    {
        ShowWindow(s.window, SW_RESTORE);
        SetForegroundWindow(s.window);
    }
    try
    {
        if (s.previewing) return;
        const auto selected = Editor::SelectedMover();
        if (!selected.is_null()) Load(selected);
        else if (s.identity.is_null()) { Load(Json()); SetStatus("Select a mover (one actor) to edit its keys."); }
    }
    catch (const std::exception& e) { SetStatus(e.what()); }
}

bool HandleCommand(UINT command)
{
    if (command != kOpen) return false;
    try { Open(GetActiveWindow()); }
    catch (const std::exception& e) { MessageBoxA(GetActiveWindow(), e.what(), kTitle, MB_OK | MB_ICONINFORMATION); }
    return true;
}
}
