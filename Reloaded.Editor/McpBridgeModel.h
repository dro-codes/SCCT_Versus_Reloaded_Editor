#pragma once
// The MCP bridge's wire format and op policies, shared by Reloaded.Editor.dll
// (the named-pipe server inside the editor) and Reloaded.Editor.Mcp.exe (the
// MCP server that connects to it). Engine-free and Windows-free.
//
// A frame is a 32-bit little-endian byte count followed by that many bytes of
// UTF-8 JSON. Every message has "t": hello, call, cancel, accepted, progress,
// result or event. See docs/Mcp.md for the conversation.
#include "WorkflowModel.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace Mcp::Bridge
{
    using Json = Workflow::Json;
    constexpr int kProtocolMajor = 1, kProtocolMinor = 0;
    // The editor's pipe is this prefix plus its process id.
    constexpr const wchar_t* kPipePrefix = L"\\\\.\\pipe\\ReloadedEditor.Mcp.";
    constexpr const char* kPipePrefixA = "\\\\.\\pipe\\ReloadedEditor.Mcp.";
    // One message. Bigger results (a map export) are written to a blob file
    // and read back with blob.read in kBlobChunk pieces.
    constexpr size_t kMaxFrame = 16u * 1024 * 1024;
    constexpr size_t kBlobChunk = 4u * 1024 * 1024;
    constexpr size_t kMaxDocument = 128u * 1024 * 1024; // the Map JSON import limit
    constexpr size_t kMaxPlanSteps = 256;
    constexpr int kMaxDepth = 64;

    inline std::string EncodeFrame(const std::string& json)
    {
        if (json.size() > kMaxFrame) throw std::runtime_error("Message exceeds the 16 MiB frame limit; use a blob.");
        const auto size = static_cast<uint32_t>(json.size());
        std::string frame(4, '\0');
        for (int i = 0; i < 4; ++i) frame[i] = static_cast<char>((size >> (8 * i)) & 0xff);
        return frame + json;
    }
    // Collects bytes as they arrive and hands out whole frames.
    class FrameReader
    {
    public:
        void Append(const char* data, size_t size) { buffer.append(data, size); }
        // The next whole frame, or false until more bytes arrive. A length over
        // the limit throws: the stream cannot be resynchronised after it.
        bool Next(std::string& out)
        {
            if (buffer.size() < 4) return false;
            uint32_t size = 0;
            for (int i = 0; i < 4; ++i) size |= static_cast<uint32_t>(static_cast<unsigned char>(buffer[i])) << (8 * i);
            if (size > kMaxFrame) throw std::runtime_error("Frame of " + std::to_string(size) + " bytes exceeds the limit.");
            if (buffer.size() < 4 + static_cast<size_t>(size)) return false;
            out.assign(buffer, 4, size);
            buffer.erase(0, 4 + static_cast<size_t>(size));
            return true;
        }
        size_t Pending() const { return buffer.size(); }
    private:
        std::string buffer;
    };
    // JSON with the import's nesting limit.
    inline Json Parse(const std::string& text)
    {
        return Json::parse(text, [](int depth, Json::parse_event_t, Json&)
        {
            if (depth > kMaxDepth) throw std::runtime_error("Message is nested too deeply.");
            return true;
        });
    }

    // What an op may do, which decides where a plan may run it (McpPlanModel.h),
    // whether approval applies and the tool annotations the server publishes.
    //   Read      changes nothing in the map (a selection or camera move at most)
    //   Undoable  changes the map inside an Undo step; joinable ones can share
    //             one Undo step with the rest of a plan
    //   File      writes files or packages (map saves, imports); not undoable
    //   Build     geometry, lighting or full builds
    //   Process   starts or ends a process (quit, Play Level)
    //   Raw       a console command, off unless [Mcp] AllowRawExec=1
    //   Blocked   never over the pipe (opens UI, or undoes blindly)
    enum class Policy { Read, Undoable, File, Build, Process, Raw, Blocked };
    struct OpPolicy { const char* name; Policy policy; bool joinable; };
    inline const char* PolicyName(Policy policy)
    {
        switch (policy)
        {
        case Policy::Read: return "read";
        case Policy::Undoable: return "undoable";
        case Policy::File: return "file";
        case Policy::Build: return "build";
        case Policy::Process: return "process";
        case Policy::Raw: return "raw";
        default: return "blocked";
        }
    }
    inline bool Mutates(Policy policy) { return policy != Policy::Read && policy != Policy::Blocked; }
    // Every op ReloadedWorkflowRequest answers, and the bridge's own. An op
    // missing here is refused over the pipe (McpOpCoverageTests keeps the
    // table complete).
    inline const std::vector<OpPolicy>& OpPolicies()
    {
        using P = Policy;
        static const std::vector<OpPolicy> table = {
            // The bridge itself.
            {"editor.state", P::Read, true}, {"log.tail", P::Read, true}, {"approval.status", P::Read, true},
            {"blob.read", P::Read, true}, {"blob.free", P::Read, true}, {"mcp.plan", P::Undoable, false},
            {"undo.call", P::Undoable, false}, {"undo.steps", P::Undoable, false}, {"redo.steps", P::Undoable, false},
            {"exec", P::Raw, false},
            // Map lifecycle and builds.
            {"map.new", P::File, false}, {"map.open", P::File, false}, {"map.save", P::File, false},
            {"build.all", P::Build, false}, {"design.build", P::Build, false},
            {"editor.quit", P::Process, false}, {"play.level", P::Process, false},
            // Reading.
            {"actors", P::Read, true}, {"actors.query", P::Read, true}, {"map", P::Read, true}, {"map.file", P::Read, true},
            {"settings.inspect", P::Read, true}, {"authoring.mapid", P::Read, true}, {"authoring.schema", P::Read, true},
            {"authoring.export", P::Read, true}, {"authoring.preview", P::Read, true}, {"assets.search", P::Read, true},
            {"view.screenshot", P::Read, true}, {"view.capture", P::Read, true}, {"view.restore", P::Read, true},
            {"mapcheck.run", P::Read, true}, {"lighting.budget", P::Read, true}, {"scene.trace", P::Read, true},
            {"ge.list", P::Read, true}, {"object.inspect", P::Read, true}, {"mapsettings.get", P::Read, true},
            {"mcp.sidecar.get", P::Read, true}, {"props.copy", P::Read, true}, {"design.pieces", P::Read, true},
            {"design.scene", P::Read, true}, {"design.grid", P::Read, true}, {"design.view", P::Read, true},
            {"design.bspowners", P::Read, true}, {"design.builds", P::Read, true}, {"design.spawns", P::Read, true},
            {"design.clearances", P::Read, true}, {"placement.selection", P::Read, true},
            {"security.actors", P::Read, true}, {"light.list", P::Read, true}, {"objective.actors", P::Read, true},
            {"stage.actors", P::Read, true}, {"stage.read", P::Read, true}, {"stage.preview", P::Read, true},
            {"optimise.report", P::Read, true}, {"magic.classes", P::Read, true}, {"magic.assets", P::Read, true},
            {"magic.inspect", P::Read, true}, {"magic.json.export", P::Read, true}, {"camera.snapshot", P::Read, true},
            {"brush.visibility", P::Read, true}, {"usages", P::Read, true}, {"connections", P::Read, true},
            {"surface.brushes", P::Read, true}, {"mesh.bounds", P::Read, true}, {"brush.snap.bounds", P::Read, true},
            {"vertex.selection", P::Read, true}, {"stairs.state", P::Read, true}, {"tag.preview", P::Read, true},
            {"assembly.capture", P::Read, true}, {"emitterlib.list", P::Read, true}, {"emitterlib.packs", P::Read, true},
            {"emitterlib.cache", P::Read, true}, {"emitterlib.problems", P::Read, true}, {"emitterlib.categories", P::Read, true},
            {"emitterlib.selected", P::Read, true}, {"emitterlib.capture", P::Read, true}, {"skins.settings", P::Read, true},
            {"objectiveplayers.state", P::Read, true}, {"conveyor.state", P::Read, true},
            // Undoable map edits.
            {"authoring.apply", P::Undoable, true}, {"select", P::Undoable, true},
            {"design.block", P::Undoable, true}, {"design.blockbatch", P::Undoable, true}, {"design.align", P::Undoable, true},
            {"design.layer", P::Undoable, true}, {"design.polyflags", P::Undoable, true}, {"design.repairflags", P::Undoable, true},
            {"design.sendtolast", P::Undoable, true}, {"design.flags", P::Undoable, true}, {"design.group", P::Undoable, true},
            {"design.translate", P::Undoable, true}, {"design.lift", P::Undoable, true}, {"design.tempstart", P::Undoable, true},
            {"design.register", P::Undoable, true}, {"design.unregister", P::Undoable, true}, {"mcp.sidecar.set", P::Undoable, true},
            {"placement.drop", P::Undoable, true}, {"placement.move", P::Undoable, true}, {"placement.turn", P::Undoable, true},
            {"placement.copy", P::Undoable, true}, {"props.paste", P::Undoable, true},
            {"start.create", P::Undoable, true}, {"alarm.locks", P::Undoable, true},
            {"security.create", P::Undoable, true}, {"security.sensor", P::Undoable, true}, {"security.link", P::Undoable, true},
            {"security.outputs", P::Undoable, true}, {"security.unlink", P::Undoable, true}, {"security.delete", P::Undoable, true},
            {"security.move", P::Undoable, true}, {"stage.apply", P::Undoable, true},
            {"objectiveplayers.set", P::Undoable, true}, {"objectiveplayers.playlevel", P::Undoable, true},
            {"objectiveplayers.targets", P::Undoable, true}, {"conveyor.add", P::Undoable, true}, {"conveyor.sync", P::Undoable, true},
            {"camera.order", P::Undoable, true}, {"camera.add", P::Undoable, true},
            {"magic.component", P::Undoable, true}, {"magic.key", P::Undoable, true}, {"magic.json.import", P::Undoable, true},
            {"magic.edit", P::Undoable, true}, {"magic.create", P::Undoable, true}, {"magic.link", P::Undoable, true},
            {"objective.add", P::Undoable, true}, {"replace", P::Undoable, true}, {"tag.rename", P::Undoable, true},
            {"assembly.place", P::Undoable, true}, {"emitterlib.place", P::Undoable, true},
            {"brush.visibility.set", P::Undoable, true}, {"builder.fit", P::Undoable, true}, {"builder.fit.brush", P::Undoable, true},
            {"vertex.portal", P::Undoable, true}, {"vertex.snap", P::Undoable, true}, {"brush.snap", P::Undoable, true},
            {"stairs.smooth", P::Undoable, true}, {"stairs.remove", P::Undoable, true},
            {"settings.edit", P::Undoable, true}, {"ge.add", P::Undoable, true}, {"surface.texture", P::Undoable, false},
            // Files and packages.
            {"asset.texture", P::File, false}, {"asset.mesh", P::File, false}, {"package.save", P::File, false}, {"object.set", P::File, false},
            {"interface.texture", P::File, false}, {"minimap.apply", P::File, false}, {"uas.build", P::File, false},
            {"mapsettings.set", P::File, false}, {"optimise.release", P::File, false}, {"conveyor.prepare", P::File, false},
            {"emitterlib.save", P::File, false}, {"emitterlib.delete", P::File, false}, {"emitterlib.update", P::File, false},
            // Never over the pipe: they open windows or undo blindly. light.set
            // writes without the staleness check every other edit has.
            {"light.set", P::Blocked, false}, {"design.removestart", P::Blocked, false}, {"design.play", P::Blocked, false},
            {"package.preview", P::Blocked, false}, {"map.setfile", P::Blocked, false}, {"emitterlib.restore", P::Blocked, false},
            {"skins.apply", P::Blocked, false}, {"skins.remove", P::Blocked, false},
            {"preview.open", P::Blocked, false}, {"preview.show", P::Blocked, false}, {"preview.capture", P::Blocked, false},
            {"preview.clear", P::Blocked, false}, {"preview.close", P::Blocked, false}, {"preview.state", P::Blocked, false},
            {"preview.attach", P::Blocked, false}, {"preview.detach", P::Blocked, false}, {"preview.step", P::Blocked, false},
            {"preview.camera", P::Blocked, false}, {"characters.open", P::Blocked, false}, {"characters.show", P::Blocked, false},
            {"characters.capture", P::Blocked, false}, {"characters.clear", P::Blocked, false}, {"characters.close", P::Blocked, false},
            {"characters.state", P::Blocked, false}, {"characters.camera", P::Blocked, false},
        };
        return table;
    }
    inline const OpPolicy* FindPolicy(const std::string& op)
    {
        for (const auto& entry : OpPolicies()) if (op == entry.name) return &entry;
        return nullptr;
    }

    // Error codes a result can carry; the busy reasons say what to wait for.
    namespace Code
    {
        constexpr const char* ProtocolMismatch = "protocol_mismatch";
        constexpr const char* Disabled = "disabled";
        constexpr const char* Busy = "busy";
        constexpr const char* Stale = "stale";
        constexpr const char* Invalid = "invalid";
        constexpr const char* Refused = "refused";
        constexpr const char* Denied = "denied";
        constexpr const char* DialogBlocked = "dialog_blocked";
        constexpr const char* EditorError = "editor_error";
        constexpr const char* Timeout = "timeout";
        constexpr const char* Cancelled = "cancelled";
        constexpr const char* NotConnected = "not_connected";
    }
    // Busy and timeout are worth retrying; the rest need a different request.
    inline bool Retryable(const std::string& code) { return code == Code::Busy || code == Code::Timeout; }
    struct Error : std::runtime_error
    {
        std::string code, reason;
        Json details;
        Error(std::string errorCode, const std::string& message, std::string why = {}, Json extra = Json())
            : std::runtime_error(message), code(std::move(errorCode)), reason(std::move(why)), details(std::move(extra)) {}
    };
    inline Json ErrorBody(const std::string& code, const std::string& message, const std::string& reason = {}, const Json& details = Json())
    {
        Json body{{"code", code}, {"message", message}, {"retryable", Retryable(code)}};
        if (!reason.empty()) body["reason"] = reason;
        if (!details.is_null()) body["details"] = details;
        return body;
    }

    // The client speaks first. Same major version or nothing; a newer minor
    // only adds ops, which the reply lists.
    inline Json Hello(const std::string& client, const std::string& version)
    {
        return {{"t", "hello"}, {"protocol", {{"major", kProtocolMajor}, {"minor", kProtocolMinor}}}, {"client", client}, {"version", version}};
    }
    inline Json HelloReply(const std::string& dllVersion, unsigned long pid, bool enabled, bool approval, const Json& ops)
    {
        return {{"t", "hello"}, {"protocol", {{"major", kProtocolMajor}, {"minor", kProtocolMinor}}}, {"dll", dllVersion}, {"pid", pid},
                {"enabled", enabled}, {"approval", approval}, {"ops", ops},
                {"limits", {{"frame", kMaxFrame}, {"blobChunk", kBlobChunk}, {"document", kMaxDocument}, {"plan", kMaxPlanSteps}, {"batch", 64}}}};
    }
    inline bool Compatible(const Json& hello, std::string& why)
    {
        if (!hello.is_object() || hello.value("t", std::string()) != "hello" || !hello.contains("protocol")) { why = "Not a bridge hello."; return false; }
        const int major = hello.at("protocol").value("major", -1);
        if (major != kProtocolMajor)
        {
            why = "The editor speaks bridge protocol " + std::to_string(major) + " and this server " + std::to_string(kProtocolMajor) +
                  ". Install the same RE+ version of Reloaded.Editor.dll and Reloaded.Editor.Mcp.exe.";
            return false;
        }
        return true;
    }
    // The op list a hello reply carries, from the policy table.
    inline Json OpList()
    {
        Json ops = Json::array();
        for (const auto& entry : OpPolicies())
            if (entry.policy != Policy::Blocked) ops.push_back({{"name", entry.name}, {"policy", PolicyName(entry.policy)}, {"joinable", entry.joinable}});
        return ops;
    }

    // expect: the map the caller last read. A call that changes the map is
    // refused as stale when the editor has moved on since.
    struct Expect { bool set = false; uint64_t level = 0; unsigned generation = 0, revision = 0; };
    inline Expect ReadExpect(const Json& call)
    {
        Expect e;
        if (!call.contains("expect") || !call.at("expect").is_object()) return e;
        const auto& x = call.at("expect");
        e.set = true;
        e.level = x.value("level", uint64_t{0});
        e.generation = x.value("generation", 0u);
        e.revision = x.value("revision", 0u);
        return e;
    }
    inline Json CallMessage(uint64_t id, const std::string& op, const Json& args, const std::string& label = {}, bool dryRun = false, const Expect& expect = {})
    {
        Json call{{"t", "call"}, {"id", id}, {"op", op}, {"args", args.is_null() ? Json::object() : args}};
        if (!label.empty()) call["label"] = label;
        if (dryRun) call["dryRun"] = true;
        if (expect.set) call["expect"] = {{"level", expect.level}, {"generation", expect.generation}, {"revision", expect.revision}};
        return call;
    }
    inline Json ResultMessage(uint64_t id, const Json& result, const Json& state = Json(), const Json& extra = Json::object())
    {
        Json message{{"t", "result"}, {"id", id}, {"ok", true}, {"result", result}};
        if (!state.is_null()) message["state"] = state;
        for (auto it = extra.begin(); it != extra.end(); ++it) message[it.key()] = it.value();
        return message;
    }
    inline Json ErrorMessage(uint64_t id, const Json& error, const Json& state = Json())
    {
        Json message{{"t", "result"}, {"id", id}, {"ok", false}, {"error", error}};
        if (!state.is_null()) message["state"] = state;
        return message;
    }
    inline Json ProgressMessage(uint64_t id, const std::string& stage, const Json& extra = Json::object())
    {
        Json message{{"t", "progress"}, {"id", id}, {"stage", stage}};
        for (auto it = extra.begin(); it != extra.end(); ++it) message[it.key()] = it.value();
        return message;
    }
    inline Json EventMessage(const std::string& kind, const Json& data = Json::object())
    {
        return {{"t", "event"}, {"kind", kind}, {"data", data}};
    }

    // Paths handed to engine console commands: the engine's parser stops at
    // quotes and line breaks and reads only the ANSI code page.
    inline bool EngineSafePath(const std::string& path)
    {
        if (path.empty() || path.size() > 260) return false;
        for (unsigned char c : path) if (c < 0x20 || c >= 0x80 || c == '"') return false;
        return true;
    }
    // Package names the bridge may write: the map's own (Name, Name_TXT,
    // Name_STM, Amb_Name, Name-i and Name_<anything>), never a stock package.
    inline bool MapOwnedPackage(const std::string& package, const std::string& map)
    {
        if (map.empty() || package.empty()) return false;
        auto fold = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return s; };
        const auto p = fold(package), m = fold(map);
        return p == m || p == m + "-i" || p == "amb_" + m || (p.size() > m.size() + 1 && p.compare(0, m.size() + 1, m + "_") == 0);
    }
    // history\<yyyymmdd-hhmmss>-<call>-<tool>.json
    inline std::string HistoryName(const std::string& stamp, unsigned call, const std::string& tool)
    {
        std::string safe;
        for (char c : tool) safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-') ? c : '_';
        if (safe.size() > 48) safe.resize(48);
        return stamp + "-" + std::to_string(call) + "-" + (safe.empty() ? "call" : safe) + ".json";
    }
    // Standard base64, for screenshots and blob chunks.
    inline std::string Base64(const std::string& bytes)
    {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);
        size_t i = 0;
        for (; i + 2 < bytes.size(); i += 3)
        {
            const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) | static_cast<unsigned char>(bytes[i + 2]);
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += table[(v >> 6) & 63]; out += table[v & 63];
        }
        if (i < bytes.size())
        {
            unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
            if (i + 1 < bytes.size()) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63];
            out += i + 1 < bytes.size() ? table[(v >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }
    inline std::string Unbase64(const std::string& text)
    {
        auto value = [](char c) -> int
        {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };
        std::string out;
        unsigned buffer = 0; int bits = 0;
        for (char c : text)
        {
            if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
            const int v = value(c);
            if (v < 0) throw std::runtime_error("Invalid base64 text.");
            buffer = (buffer << 6) | static_cast<unsigned>(v); bits += 6;
            if (bits >= 8) { bits -= 8; out += static_cast<char>((buffer >> bits) & 0xff); }
        }
        return out;
    }
}
