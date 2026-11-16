#pragma once
// Mover Workbench (RE+ Tools > Mover Workbench...): a mover's keyframes as a
// list, edited a key at a time. This header is the pure part: the key list and
// its edits, where the mover stands at each key, what a move in the viewport
// did to it, the preview's poses over time, the ghost outlines and the plain
// names of the mover settings. No Windows, no engine.
//
// How a mover holds its path (Engine.Mover in this game, checked on ShipD):
// KeyPos[8] and KeyRot[8] are offsets from BasePos and BaseRot, NumKeys of
// them in use, and KeyNum is the key the mover sits on in the editor (and
// starts from in game). Setting KeyNum and PostEditChange moves the mover to
// BasePos + KeyPos[KeyNum]; PostEditChange first works BasePos out again as
// Location - OldPos (OldPos being the key it sat on), so a mover moved off its
// key and then edited takes its whole path along.
#include "WorkflowModel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace Workflow::Movers
{
    // KeyPos and KeyRot hold 8 keys in this game.
    constexpr int kDefaultCapacity = 8;
    // A mover this close to a key's place (units) and turn (rotation units) sits on it.
    constexpr double kOnKey = 0.5;
    constexpr int kOnKeyTurn = 2;

    struct Key
    {
        Vector position{};   // KeyPos[i], from BasePos
        Rotation rotation{}; // KeyRot[i], from BaseRot
        bool operator==(const Key&) const = default;
    };
    struct Path
    {
        Vector base{};           // BasePos
        Rotation baseRotation{}; // BaseRot
        std::vector<Key> keys;   // KeyPos/KeyRot [0, NumKeys)
        int current = 0;         // KeyNum
        int capacity = kDefaultCapacity;
        bool operator==(const Path&) const = default;
    };

    inline void CheckPath(const Path& path)
    {
        if (path.capacity < 1 || path.capacity > 255) throw std::runtime_error("This mover's key arrays are not readable.");
        if (path.keys.empty()) throw std::runtime_error("A mover needs at least one key.");
        if (static_cast<int>(path.keys.size()) > path.capacity)
            throw std::runtime_error("A mover holds at most " + std::to_string(path.capacity) + " keys.");
        if (path.current < 0 || path.current >= static_cast<int>(path.keys.size()))
            throw std::runtime_error("The mover sits on key " + std::to_string(path.current) + ", which it does not have.");
        for (double v : path.base) if (!std::isfinite(v) || std::abs(v) > 1e7) throw std::runtime_error("The mover's base is out of range.");
        for (const auto& key : path.keys)
            for (double v : key.position) if (!std::isfinite(v) || std::abs(v) > 1e7) throw std::runtime_error("A key's place is out of range.");
    }
    inline void CheckIndex(const Path& path, int index)
    {
        if (index < 0 || index >= static_cast<int>(path.keys.size())) throw std::runtime_error("Select a key in the list first.");
    }

    // Rotator arithmetic as the engine does it: whole rotation units, kept in a
    // 32-bit int (no wrapping to one turn).
    inline Rotation Add(const Rotation& a, const Rotation& b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
    inline Rotation Subtract(const Rotation& a, const Rotation& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    inline Vector Add(const Vector& a, const Vector& b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
    inline Vector Subtract(const Vector& a, const Vector& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }

    // Where the mover stands when it sits on the key: BasePos + KeyPos[key].
    inline Pose At(const Path& path, int key)
    {
        CheckIndex(path, key);
        return { Add(path.base, path.keys[key].position), Add(path.baseRotation, path.keys[key].rotation) };
    }
    // A place as a key of this path.
    inline Key KeyFor(const Path& path, const Pose& place)
    {
        return { Subtract(place.position, path.base), Subtract(place.rotation, path.baseRotation) };
    }
    inline bool Same(const Pose& a, const Pose& b)
    {
        for (int i = 0; i < 3; ++i)
            if (std::abs(a.position[i] - b.position[i]) > kOnKey || std::abs(a.rotation[i] - b.rotation[i]) > kOnKeyTurn) return false;
        return true;
    }
    inline bool SameKey(const Key& a, const Key& b) { return Same({ a.position, a.rotation }, { b.position, b.rotation }); }
    inline bool OnKey(const Path& path, const Pose& place) { return Same(At(path, path.current), place); }

    // What happened to the mover since the workbench last saw it at rest.
    enum class Moved
    {
        No,       // the same path, the mover on its key
        OffKey,   // the same path, the mover moved off its key (nothing rewrote the path yet)
        Dragged,  // moved as the stock editor moves a mover: at key 0 the base went
                  // with it, at a later key that key was rewritten to the new place
        Changed,  // edited some other way (properties, Undo): the live path is the one to keep
    };
    inline Moved Compare(const Path& rest, const Path& live, const Pose& place)
    {
        if (rest.keys.size() != live.keys.size() || rest.current != live.current || rest.capacity != live.capacity) return Moved::Changed;
        const bool sameBase = Same({ rest.base, rest.baseRotation }, { live.base, live.baseRotation });
        size_t differ = 0, at = 0;
        for (size_t i = 0; i < live.keys.size(); ++i)
            if (!SameKey(rest.keys[i], live.keys[i])) { ++differ; at = i; }
        if (sameBase && !differ) return OnKey(live, place) ? Moved::No : Moved::OffKey;
        if (!OnKey(live, place)) return Moved::Changed;
        // The key it sits on, rewritten (a later key), or the base moved with
        // it (key 0), and nothing else: a viewport move.
        if (live.current > 0 && sameBase && differ == 1 && static_cast<int>(at) == live.current) return Moved::Dragged;
        if (live.current == 0 && !sameBase && !differ) return Moved::Dragged;
        return Moved::Changed;
    }
    // The path a new or updated key is measured against: the one at rest while
    // the live one only differs by a move in the viewport, so the key the mover
    // sat on (or, at key 0, the whole path) stays where it was.
    inline const Path& MeasuredAgainst(const Path* rest, const Path& live, const Pose& place)
    {
        if (!rest) return live;
        const auto moved = Compare(*rest, live, place);
        return moved == Moved::Dragged || moved == Moved::OffKey ? *rest : live;
    }

    // ---- edits. Each returns the new path; the mover then sits on path.current.

    // Add Key Here: the mover's place becomes a new key after the last one,
    // and the mover sits on it (so it does not move).
    inline Path AddKey(Path path, const Pose& place)
    {
        CheckPath(path);
        if (static_cast<int>(path.keys.size()) >= path.capacity)
            throw std::runtime_error("This mover already has all " + std::to_string(path.capacity) + " keys it can hold. Delete one or Update Key instead.");
        path.keys.push_back(KeyFor(path, place));
        path.current = static_cast<int>(path.keys.size()) - 1;
        return path;
    }
    // Update Key: the key becomes the mover's place, and the mover sits on it.
    inline Path UpdateKey(Path path, int index, const Pose& place)
    {
        CheckPath(path);
        CheckIndex(path, index);
        path.keys[index] = KeyFor(path, place);
        path.current = index;
        return path;
    }
    // Delete Key: the later keys move up one. The mover stays on the key it
    // sat on, or on the one that takes the deleted key's place.
    inline Path DeleteKey(Path path, int index)
    {
        CheckPath(path);
        CheckIndex(path, index);
        if (path.keys.size() == 1) throw std::runtime_error("A mover needs at least one key.");
        path.keys.erase(path.keys.begin() + index);
        if (path.current > index) --path.current;
        path.current = std::min(path.current, static_cast<int>(path.keys.size()) - 1);
        return path;
    }
    // Move Up (-1) or Down (+1): the key swaps with its neighbour; the mover
    // keeps sitting on the same key, wherever it now is in the list.
    inline Path MoveKey(Path path, int index, int direction)
    {
        CheckPath(path);
        CheckIndex(path, index);
        if (direction != -1 && direction != 1) throw std::runtime_error("Move a key up or down one place.");
        const int other = index + direction;
        if (other < 0 || other >= static_cast<int>(path.keys.size()))
            throw std::runtime_error(direction < 0 ? "That key is already first." : "That key is already last.");
        std::swap(path.keys[index], path.keys[other]);
        if (path.current == index) path.current = other;
        else if (path.current == other) path.current = index;
        return path;
    }
    // Go to Key: the mover sits on that key (its place: At(path, index)).
    inline Path GoTo(Path path, int index)
    {
        CheckPath(path);
        CheckIndex(path, index);
        path.current = index;
        return path;
    }

    // ---- timing and the preview

    struct Timing
    {
        double moveTime = 1;     // MoveTime: seconds per key to key leg
        double stayOpenTime = 0; // StayOpenTime: the pause at the last key
        bool glide = true;       // MoverGlideType MV_GlideByTime: eases in and out of each leg
        bool shortest = false;   // bUseShortestRotation: turns the short way round
    };
    // When the mover reaches the key going out from key 0, every leg taking MoveTime.
    inline double Arrival(const Timing& timing, int key) { return key * std::max(0.0, timing.moveTime); }
    // The preview's pause at the last key (StayOpenTime, at most 3 s), so a
    // long-open door still comes back soon.
    constexpr double kPreviewPause = 3;
    inline double PreviewPause(const Timing& timing) { return std::clamp(timing.stayOpenTime, 0.0, kPreviewPause); }
    // The preview goes out from key 0 through every key, pauses, and comes back.
    inline double PreviewLength(const Path& path, const Timing& timing)
    {
        if (path.keys.size() < 2) return 0;
        const double legs = static_cast<double>(path.keys.size() - 1);
        return 2 * legs * std::max(0.0, timing.moveTime) + PreviewPause(timing);
    }
    // The engine's glide: 3a^2 - 2a^3, starting and stopping gently.
    inline double Ease(double alpha, bool glide)
    {
        alpha = std::clamp(alpha, 0.0, 1.0);
        return glide ? alpha * alpha * (3 - 2 * alpha) : alpha;
    }
    inline int TurnBetween(int from, int to, double alpha, bool shortest)
    {
        long long delta = static_cast<long long>(to) - from;
        if (shortest)
        {
            delta = ((delta % 65536) + 65536) % 65536;
            if (delta > 32768) delta -= 65536;
        }
        return from + static_cast<int>(std::lround(static_cast<double>(delta) * alpha));
    }
    inline Pose Between(const Pose& a, const Pose& b, double alpha, bool shortest)
    {
        Pose result;
        for (int i = 0; i < 3; ++i)
        {
            result.position[i] = a.position[i] + (b.position[i] - a.position[i]) * alpha;
            result.rotation[i] = TurnBetween(a.rotation[i], b.rotation[i], alpha, shortest);
        }
        return result;
    }
    // Where the preview has the mover `seconds` after it starts; past the end, key 0.
    inline Pose PreviewAt(const Path& path, const Timing& timing, double seconds)
    {
        CheckPath(path);
        const int last = static_cast<int>(path.keys.size()) - 1;
        if (last == 0 || seconds <= 0) return At(path, 0);
        const double leg = std::max(0.0, timing.moveTime);
        const double out = last * leg, pause = PreviewPause(timing);
        auto along = [&](double t, bool outward) {
            if (leg <= 0) return At(path, outward ? last : 0);
            const int step = std::min(last - 1, static_cast<int>(std::floor(t / leg)));
            const double alpha = Ease(t / leg - step, timing.glide);
            const int from = outward ? step : last - step, to = outward ? step + 1 : last - step - 1;
            return Between(At(path, from), At(path, to), alpha, timing.shortest);
        };
        if (seconds < out) return along(seconds, true);
        if (seconds < out + pause) return At(path, last);
        if (seconds < out + pause + out) return along(seconds - out - pause, false);
        return At(path, 0);
    }

    // ---- ghost outlines

    struct Edge { Vector from, to; };
    // The mover's outline (world edges with the mover at `place`) moved to
    // `target`: turned about the mover's pivot (its Location) and carried along.
    inline std::vector<Edge> Ghost(const std::vector<Edge>& edges, const Pose& place, const Pose& target)
    {
        std::vector<Edge> out;
        out.reserve(edges.size());
        const Pose here{ place.position, place.rotation }, there{ target.position, target.rotation };
        for (const auto& e : edges)
            out.push_back({ TransformPoint(TransformPoint(e.from, here, true), there), TransformPoint(TransformPoint(e.to, here, true), there) });
        return out;
    }
    // The outline's lines are capped so a detailed mover cannot stall the
    // viewports; past the cap the box of the outline stands in for it.
    constexpr size_t kGhostEdges = 1500;
    inline std::vector<Edge> Simplify(const std::vector<Edge>& edges)
    {
        if (edges.size() <= kGhostEdges) return edges;
        Vector low = edges.front().from, high = low;
        for (const auto& e : edges)
            for (int i = 0; i < 3; ++i)
            {
                low[i] = std::min({ low[i], e.from[i], e.to[i] });
                high[i] = std::max({ high[i], e.from[i], e.to[i] });
            }
        std::vector<Edge> box;
        auto corner = [&](int c) { return Vector{ (c & 1) ? high[0] : low[0], (c & 2) ? high[1] : low[1], (c & 4) ? high[2] : low[2] }; };
        for (int c = 0; c < 8; ++c)
            for (int axis = 0; axis < 3; ++axis)
                if (!(c & (1 << axis))) box.push_back({ corner(c), corner(c | (1 << axis)) });
        return box;
    }

    // ---- plain names

    // InitialState: what sets the mover going. Unlisted states keep their own name.
    inline std::string StateLabel(const std::string& state)
    {
        struct Named { const char* state; const char* label; };
        static const Named names[] = {
            { "None", "Class default (none set)" },
            { "TriggerOpenTimed", "Triggered: opens, closes after StayOpenTime" },
            { "TriggerOpenOnceOnly", "Triggered: opens once and stays open" },
            { "TriggerCloseOnceOnly", "Triggered: closes once and stays closed" },
            { "TriggerCloseTimed", "Triggered: closes, opens again after StayOpenTime" },
            { "TriggerToggle", "Triggered: each trigger opens or closes it" },
            { "TriggerControl", "Triggered: open while triggered, closes when released" },
            { "TriggerPound", "Triggered: opens and closes over and over while triggered" },
            { "BumpOpenTimed", "Bumped: opens when touched, closes after StayOpenTime" },
            { "BumpButton", "Button: moves when bumped, then comes back" },
            { "StandOpenTimed", "Stood on: moves when stood on, comes back after StayOpenTime" },
        };
        for (const auto& n : names)
            if (Fold(n.state) == Fold(state)) return n.label;
        return state;
    }
    // States a map should not start a mover in: the engine's base and helper
    // states. Kept in the list only when the mover already uses one.
    inline bool InternalState(const std::string& state)
    {
        const auto folded = Fold(state);
        return folded.rfind("s_", 0) == 0 || folded == "opentimedmover" || folded == "closetimedmover";
    }
    // Enum values of the mover settings, in words; anything else as written.
    inline std::string ChoiceLabel(const std::string& property, const std::string& value)
    {
        struct Named { const char* property; const char* value; const char* label; };
        static const Named names[] = {
            { "MoverEncroachType", "ME_StopWhenEncroach", "Stops when blocked" },
            { "MoverEncroachType", "ME_ReturnWhenEncroach", "Goes back when blocked" },
            { "MoverEncroachType", "ME_CrushWhenEncroach", "Crushes what blocks it" },
            { "MoverEncroachType", "ME_IgnoreWhenEncroach", "Passes through what blocks it" },
            { "MoverEncroachType", "ME_WaitAndRetryWhenEncroach", "Waits and tries again when blocked" },
            { "MoverGlideType", "MV_MoveByTime", "Constant speed" },
            { "MoverGlideType", "MV_GlideByTime", "Eases in and out" },
            { "BumpType", "BT_PlayerBump", "Players" },
            { "BumpType", "BT_PawnBump", "Any pawn" },
            { "BumpType", "BT_AnyBump", "Anything" },
        };
        for (const auto& n : names)
            if (Fold(n.property) == Fold(property) && Fold(n.value) == Fold(value)) return std::string(n.label) + " (" + value + ")";
        return value;
    }

    // ---- reading and writing the key arrays (the authoring JSON's text values)

    inline std::string Number(double v)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "%.6f", v);
        return text;
    }
    // KeyPos as Magic JSON: all capacity slots, the unused ones zero.
    inline Json PositionsJson(const Path& path)
    {
        Json out = Json::array();
        for (int i = 0; i < path.capacity; ++i)
        {
            const Vector v = i < static_cast<int>(path.keys.size()) ? path.keys[i].position : Vector{};
            out.push_back({ { "X", Number(v[0]) }, { "Y", Number(v[1]) }, { "Z", Number(v[2]) } });
        }
        return out;
    }
    inline Json RotationsJson(const Path& path)
    {
        Json out = Json::array();
        for (int i = 0; i < path.capacity; ++i)
        {
            const Rotation r = i < static_cast<int>(path.keys.size()) ? path.keys[i].rotation : Rotation{};
            out.push_back({ { "Pitch", std::to_string(r[0]) }, { "Yaw", std::to_string(r[1]) }, { "Roll", std::to_string(r[2]) } });
        }
        return out;
    }
    inline double ParseNumber(const Json& value)
    {
        if (value.is_number()) return value.get<double>();
        const auto text = value.get<std::string>();
        size_t used = 0;
        const double v = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(v)) throw std::runtime_error("Not a number: " + text);
        return v;
    }
    // The path from the mover's values (KeyPos, KeyRot, NumKeys, KeyNum as the
    // authoring JSON writes them) and its base.
    inline Path FromValues(const Json& values, const Vector& base, const Rotation& baseRotation)
    {
        Path path;
        path.base = base;
        path.baseRotation = baseRotation;
        const auto& positions = values.at("KeyPos");
        const auto& rotations = values.at("KeyRot");
        if (!positions.is_array() || !rotations.is_array() || positions.size() != rotations.size() || positions.empty())
            throw std::runtime_error("This mover's key arrays are not readable.");
        path.capacity = static_cast<int>(positions.size());
        const int count = static_cast<int>(ParseNumber(values.at("NumKeys")));
        if (count < 1 || count > path.capacity) throw std::runtime_error("This mover's NumKeys (" + std::to_string(count) + ") is outside 1 to " + std::to_string(path.capacity) + ".");
        for (int i = 0; i < count; ++i)
        {
            Key key;
            key.position = { ParseNumber(positions[i].at("X")), ParseNumber(positions[i].at("Y")), ParseNumber(positions[i].at("Z")) };
            key.rotation = { static_cast<int>(ParseNumber(rotations[i].at("Pitch"))), static_cast<int>(ParseNumber(rotations[i].at("Yaw"))),
                             static_cast<int>(ParseNumber(rotations[i].at("Roll"))) };
            path.keys.push_back(key);
        }
        path.current = std::clamp(static_cast<int>(ParseNumber(values.at("KeyNum"))), 0, count - 1);
        return path;
    }
    // The path between the window and the editor: {base, baseRotation,
    // keys:[{position, rotation}], current, capacity}.
    inline Json PathJson(const Path& path)
    {
        Json keys = Json::array();
        for (const auto& key : path.keys) keys.push_back({ { "position", key.position }, { "rotation", key.rotation } });
        return { { "base", path.base }, { "baseRotation", path.baseRotation }, { "keys", keys }, { "current", path.current }, { "capacity", path.capacity } };
    }
    inline Path PathFrom(const Json& json)
    {
        Path path;
        path.base = json.at("base").get<Vector>();
        path.baseRotation = json.at("baseRotation").get<Rotation>();
        for (const auto& key : json.at("keys")) path.keys.push_back({ key.at("position").get<Vector>(), key.at("rotation").get<Rotation>() });
        path.current = json.at("current").get<int>();
        path.capacity = json.at("capacity").get<int>();
        CheckPath(path);
        return path;
    }
    // A time field as typed: seconds, 0 to 600.
    inline double Seconds(const std::string& text, const char* what)
    {
        double v = 0;
        try { v = ParseNumber(Json(text)); }
        catch (const std::exception&) { throw std::runtime_error(std::string(what) + " must be a number of seconds."); }
        if (v < 0 || v > 600) throw std::runtime_error(std::string(what) + " must be 0 to 600 seconds.");
        return v;
    }
}
