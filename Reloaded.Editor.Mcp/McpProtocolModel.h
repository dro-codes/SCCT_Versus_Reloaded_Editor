#pragma once
// MCP's JSON-RPC 2.0 side, without threads or I/O: message envelopes, protocol
// version negotiation, the capability block, and how tools, resources and
// prompts are written into list and call replies for each protocol version.
// McpServer.cpp moves the bytes. Tests: tests/McpProtocolModelTests.cpp.
#include "McpTool.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace Mcp::Protocol
{
    using Json = Workflow::Json;
    // Newest first. A client asking for another version is offered kFallback,
    // which every current client speaks.
    inline const std::vector<std::string>& Versions()
    {
        static const std::vector<std::string> versions{"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
        return versions;
    }
    constexpr const char* kFallback = "2025-06-18";
    inline std::string Negotiate(const Json& requested)
    {
        if (requested.is_string())
            for (const auto& version : Versions()) if (requested.get<std::string>() == version) return version;
        return kFallback;
    }
    // Versions are dates, so they compare as text.
    inline bool AtLeast(const std::string& version, const char* than) { return version.compare(than) >= 0; }
    // structuredContent, outputSchema and title fields came with 2025-06-18,
    // tool annotations and progress messages with 2025-03-26.
    inline bool Structured(const std::string& version) { return AtLeast(version, "2025-06-18"); }
    inline bool Titled(const std::string& version) { return AtLeast(version, "2025-06-18"); }
    inline bool Annotated(const std::string& version) { return AtLeast(version, "2025-03-26"); }

    namespace ErrorCode
    {
        constexpr int Parse = -32700, InvalidRequest = -32600, MethodNotFound = -32601, InvalidParams = -32602, Internal = -32603;
        constexpr int ResourceNotFound = -32002;
    }

    // One incoming message, sorted by what it asks of the server.
    struct Message
    {
        enum class Kind { Request, Notification, Response, Invalid } kind = Kind::Invalid;
        Json id;              // requests and responses
        std::string method;   // requests and notifications
        Json params = Json::object();
        std::string problem;  // why it is invalid
    };
    inline bool ValidId(const Json& id) { return id.is_string() || id.is_number_integer() || id.is_number_unsigned(); }
    inline Message Classify(const Json& message)
    {
        Message m;
        if (!message.is_object()) { m.problem = "A JSON-RPC message is an object."; return m; }
        if (!message.contains("jsonrpc") || message.at("jsonrpc") != "2.0") { m.problem = "Only JSON-RPC 2.0 is spoken (\"jsonrpc\": \"2.0\")."; if (message.contains("id") && ValidId(message.at("id"))) m.id = message.at("id"); return m; }
        const bool hasId = message.contains("id") && !message.at("id").is_null();
        if (hasId) m.id = message.at("id");
        if (message.contains("method"))
        {
            if (!message.at("method").is_string()) { m.problem = "method must be a string."; return m; }
            m.method = message.at("method").get<std::string>();
            if (message.contains("params"))
            {
                const auto& params = message.at("params");
                if (!params.is_object() && !params.is_null()) { m.problem = "params must be an object."; return m; }
                if (params.is_object()) m.params = params;
            }
            if (!hasId) { m.kind = Message::Kind::Notification; return m; }
            if (!ValidId(m.id)) { m.problem = "A request id is a string or an integer."; m.id = Json(); return m; }
            m.kind = Message::Kind::Request;
            return m;
        }
        if (hasId && (message.contains("result") || message.contains("error"))) { m.kind = Message::Kind::Response; return m; }
        m.problem = "The message has neither a method nor a result.";
        return m;
    }
    inline Json Response(const Json& id, const Json& result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}; }
    inline Json Error(const Json& id, int code, const std::string& message, const Json& data = Json())
    {
        Json error{{"code", code}, {"message", message}};
        if (!data.is_null()) error["data"] = data;
        return {{"jsonrpc", "2.0"}, {"id", id}, {"error", error}};
    }
    inline Json Notification(const std::string& method, const Json& params = Json::object())
    {
        Json message{{"jsonrpc", "2.0"}, {"method", method}};
        if (!params.is_null()) message["params"] = params;
        return message;
    }
    // One line of the stdio stream. Text from the engine is ANSI and may not
    // be valid UTF-8, so bad bytes are replaced rather than failing the reply.
    inline std::string Line(const Json& message) { return message.dump(-1, ' ', false, Json::error_handler_t::replace); }
    // Parses one line with a nesting limit (a level spec is deep, but not this deep).
    inline Json ParseLine(const std::string& line, int maxDepth = 96)
    {
        return Json::parse(line, [maxDepth](int depth, Json::parse_event_t, Json&)
        {
            if (depth > maxDepth) throw std::runtime_error("The message is nested too deeply.");
            return true;
        });
    }

    inline Json Capabilities()
    {
        return {{"tools", {{"listChanged", true}}}, {"resources", {{"subscribe", true}, {"listChanged", true}}},
                {"prompts", {{"listChanged", true}}}, {"logging", Json::object()}};
    }
    inline Json ServerInfo(const std::string& version, const std::string& protocol)
    {
        Json info{{"name", "scct-reloaded"}, {"version", version}};
        if (Titled(protocol)) info["title"] = "SCCT Versus Reloaded Editor (RE+)";
        return info;
    }
    inline Json InitializeResult(const std::string& protocol, const std::string& version, const std::string& instructions)
    {
        Json result{{"protocolVersion", protocol}, {"capabilities", Capabilities()}, {"serverInfo", ServerInfo(version, protocol)}};
        if (!instructions.empty()) result["instructions"] = instructions;
        return result;
    }

    inline Json ToolJson(const Tool& tool, const std::string& protocol)
    {
        Json out{{"name", tool.name}, {"description", tool.description},
                 {"inputSchema", tool.inputSchema.is_object() ? tool.inputSchema : Json{{"type", "object"}}}};
        if (Titled(protocol) && !tool.title.empty()) out["title"] = tool.title;
        if (Structured(protocol) && tool.outputSchema.is_object()) out["outputSchema"] = tool.outputSchema;
        if (Annotated(protocol))
        {
            Json hints{{"readOnlyHint", tool.readOnly}, {"openWorldHint", tool.openWorld}};
            // The other two hints only mean something for a tool that changes things.
            if (!tool.readOnly) { hints["destructiveHint"] = tool.destructive; hints["idempotentHint"] = tool.idempotent; }
            // From 2025-06-18 the tool's own title says it; before, only here.
            if (!tool.title.empty() && !Titled(protocol)) hints["title"] = tool.title;
            out["annotations"] = hints;
        }
        return out;
    }
    inline Json ContentJson(const Content& content)
    {
        if (content.type == "image") return {{"type", "image"}, {"data", content.data}, {"mimeType", content.mimeType.empty() ? "image/png" : content.mimeType}};
        if (content.type == "resource") return {{"type", "resource"}, {"resource", content.resource}};
        return {{"type", "text"}, {"text", content.text}};
    }
    inline Json ResourceJson(const Resource& resource, const std::string& protocol)
    {
        Json out{{(resource.isTemplate ? "uriTemplate" : "uri"), resource.uri}, {"name", resource.name.empty() ? resource.uri : resource.name}};
        if (Titled(protocol) && !resource.title.empty()) out["title"] = resource.title;
        if (!resource.description.empty()) out["description"] = resource.description;
        if (!resource.mimeType.empty()) out["mimeType"] = resource.mimeType;
        return out;
    }
    inline Json PromptJson(const Prompt& prompt, const std::string& protocol)
    {
        Json out{{"name", prompt.name}};
        if (Titled(protocol) && !prompt.title.empty()) out["title"] = prompt.title;
        if (!prompt.description.empty()) out["description"] = prompt.description;
        Json arguments = Json::array();
        for (const auto& a : prompt.arguments) arguments.push_back({{"name", a.name}, {"description", a.description}, {"required", a.required}});
        out["arguments"] = arguments;
        return out;
    }
    // A resource's read ({text} or {blob}, optional mimeType) as resources/read contents.
    inline Json ReadContents(const std::string& uri, const Json& read, const std::string& mimeType)
    {
        Json item{{"uri", uri}, {"mimeType", read.is_object() ? read.value("mimeType", mimeType) : mimeType}};
        if (read.is_object() && read.contains("blob")) item["blob"] = read.at("blob");
        else if (read.is_object() && read.contains("text")) item["text"] = read.at("text");
        else item["text"] = read.dump(2, ' ', false, Json::error_handler_t::replace);
        return {{"contents", Json::array({item})}};
    }
    inline Json ProgressParams(const Json& token, double progress, double total, const std::string& message, const std::string& protocol)
    {
        Json params{{"progressToken", token}, {"progress", progress}};
        if (total > 0) params["total"] = total;
        if (!message.empty() && Annotated(protocol)) params["message"] = message;
        return params;
    }

    // logging/setLevel and notifications/message use the syslog levels.
    inline int LogRank(const std::string& level)
    {
        static const std::vector<std::string> levels{"debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"};
        for (size_t i = 0; i < levels.size(); ++i) if (levels[i] == level) return static_cast<int>(i);
        return -1;
    }

    // Arguments checked against a tool's inputSchema before it runs, so a
    // mistake comes back as a sentence naming the field. The common JSON
    // Schema keywords are checked (type, required, properties,
    // additionalProperties, enum, const, items, min/max, minItems/maxItems,
    // minLength/maxLength, anyOf/oneOf); the rest are left to the tool.
    namespace Detail
    {
        inline bool IsType(const Json& value, const std::string& type)
        {
            if (type == "object") return value.is_object();
            if (type == "array") return value.is_array();
            if (type == "string") return value.is_string();
            if (type == "boolean") return value.is_boolean();
            if (type == "null") return value.is_null();
            if (type == "number") return value.is_number();
            if (type == "integer")
            {
                if (value.is_number_integer() || value.is_number_unsigned()) return true;
                if (!value.is_number_float()) return false;
                const double v = value.get<double>();
                return std::isfinite(v) && std::floor(v) == v;
            }
            return true;
        }
        inline std::string TypeText(const Json& type)
        {
            if (type.is_string()) return type.get<std::string>();
            std::string out;
            for (const auto& t : type) out += (out.empty() ? "" : " or ") + t.get<std::string>();
            return out;
        }
        inline std::string Choices(const Json& values)
        {
            std::string out;
            for (const auto& v : values) out += (out.empty() ? "" : ", ") + (v.is_string() ? v.get<std::string>() : v.dump());
            return out;
        }
        inline std::string Check(const Json& schema, const Json& value, const std::string& where, int depth)
        {
            if (!schema.is_object() || depth > 32) return {};
            if (schema.contains("type"))
            {
                const auto& type = schema.at("type");
                bool fits = false;
                if (type.is_string()) fits = IsType(value, type.get<std::string>());
                else if (type.is_array()) for (const auto& t : type) fits = fits || (t.is_string() && IsType(value, t.get<std::string>()));
                else fits = true;
                if (!fits) return where + " must be " + TypeText(type) + (value.is_null() ? "" : ", not " + std::string(value.type_name()));
            }
            if (schema.contains("const") && value != schema.at("const")) return where + " must be " + schema.at("const").dump();
            if (schema.contains("enum") && schema.at("enum").is_array())
            {
                const auto& values = schema.at("enum");
                if (std::find(values.begin(), values.end(), value) == values.end()) return where + " must be one of " + Choices(values);
            }
            for (const char* combinator : {"anyOf", "oneOf"})
            {
                if (!schema.contains(combinator) || !schema.at(combinator).is_array()) continue;
                std::string first;
                bool any = false;
                for (const auto& option : schema.at(combinator))
                {
                    auto problem = Check(option, value, where, depth + 1);
                    if (problem.empty()) { any = true; break; }
                    if (first.empty()) first = problem;
                }
                if (!any) return first.empty() ? where + " does not match any allowed form" : first;
            }
            if (value.is_number())
            {
                const double v = value.get<double>();
                if (schema.contains("minimum") && schema.at("minimum").is_number() && v < schema.at("minimum").get<double>()) return where + " must be at least " + schema.at("minimum").dump();
                if (schema.contains("maximum") && schema.at("maximum").is_number() && v > schema.at("maximum").get<double>()) return where + " must be at most " + schema.at("maximum").dump();
            }
            if (value.is_string())
            {
                const auto size = value.get_ref<const std::string&>().size();
                if (schema.contains("minLength") && schema.at("minLength").is_number() && size < schema.at("minLength").get<size_t>()) return where + " must not be empty";
                if (schema.contains("maxLength") && schema.at("maxLength").is_number() && size > schema.at("maxLength").get<size_t>()) return where + " is longer than " + schema.at("maxLength").dump() + " characters";
            }
            if (value.is_array())
            {
                if (schema.contains("minItems") && schema.at("minItems").is_number() && value.size() < schema.at("minItems").get<size_t>()) return where + " needs at least " + schema.at("minItems").dump() + " items";
                if (schema.contains("maxItems") && schema.at("maxItems").is_number() && value.size() > schema.at("maxItems").get<size_t>()) return where + " holds at most " + schema.at("maxItems").dump() + " items";
                if (schema.contains("items") && schema.at("items").is_object())
                    for (size_t i = 0; i < value.size(); ++i)
                        if (auto problem = Check(schema.at("items"), value[i], where + "[" + std::to_string(i) + "]", depth + 1); !problem.empty()) return problem;
            }
            if (value.is_object())
            {
                const Json properties = schema.value("properties", Json::object());
                if (schema.contains("required") && schema.at("required").is_array())
                    for (const auto& name : schema.at("required"))
                        if (name.is_string() && !value.contains(name.get<std::string>())) return where + "." + name.get<std::string>() + " is required";
                const bool closed = schema.contains("additionalProperties") && schema.at("additionalProperties").is_boolean() && !schema.at("additionalProperties").get<bool>();
                for (auto it = value.begin(); it != value.end(); ++it)
                {
                    if (properties.is_object() && properties.contains(it.key()))
                    {
                        if (auto problem = Check(properties.at(it.key()), it.value(), where + "." + it.key(), depth + 1); !problem.empty()) return problem;
                    }
                    else if (closed)
                    {
                        std::string known;
                        if (properties.is_object()) for (auto p = properties.begin(); p != properties.end(); ++p) known += (known.empty() ? "" : ", ") + p.key();
                        return where + "." + it.key() + " is not a known field" + (known.empty() ? "" : " (known: " + known + ")");
                    }
                    else if (schema.contains("additionalProperties") && schema.at("additionalProperties").is_object())
                    {
                        if (auto problem = Check(schema.at("additionalProperties"), it.value(), where + "." + it.key(), depth + 1); !problem.empty()) return problem;
                    }
                }
            }
            return {};
        }
    }
    // "" when the arguments fit the schema, else the first problem found.
    inline std::string CheckArguments(const Json& schema, const Json& arguments)
    {
        if (!arguments.is_object()) return "arguments must be an object";
        return Detail::Check(schema, arguments, "arguments", 0);
    }
    // "" when a tool's structured result fits its outputSchema (none declared
    // fits anything), else the first problem found.
    inline std::string CheckStructured(const Json& outputSchema, const Json& structured)
    {
        if (!outputSchema.is_object()) return {};
        if (!structured.is_object()) return "structuredContent must be an object";
        return Detail::Check(outputSchema, structured, "structuredContent", 0);
    }

    // A tools/call result. A tool's outputSchema describes what it returns
    // when it works; an error's details ({error, code, hint, ...}) need not
    // have that shape, and clients validate any structuredContent they get
    // (the TypeScript SDK even with isError), so an error that does not fit
    // carries its details in the text block only. The field is optional in
    // every protocol version.
    inline Json ToolResultJson(const ToolResult& result, const std::string& protocol, const Json& outputSchema = Json())
    {
        Json content = Json::array();
        for (const auto& c : result.content) content.push_back(ContentJson(c));
        if (content.empty()) content.push_back({{"type", "text"}, {"text", result.structured.dump(2, ' ', false, Json::error_handler_t::replace)}});
        Json out{{"content", content}, {"isError", result.isError}};
        if (Structured(protocol) && result.structured.is_object() && (!result.isError || CheckStructured(outputSchema, result.structured).empty()))
            out["structuredContent"] = result.structured;
        return out;
    }
}
