// Reloaded.Editor.Mcp.exe: the MCP server for the RE+ editor. It lives in the
// game's System folder beside Reloaded.Editor.dll, speaks MCP on stdin and
// stdout, and reaches the editor through its named pipe (BridgeClient).
//   Reloaded.Editor.Mcp.exe              serve MCP on stdio
//   Reloaded.Editor.Mcp.exe --version
//   Reloaded.Editor.Mcp.exe --self-test  check the tools and resources it serves
#ifdef _WIN32
#define NOMINMAX // <windows.h> would turn std::min and std::max into macros
#endif
#include "McpServer.h"
#include "BridgeClient.h"
#include "Embedded.h"
#include "Tools/Common.h"
#include "../Reloaded.Editor/Version.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace
{
    using Mcp::Json;

    std::filesystem::path ExeFolder(const char* argv0)
    {
#ifdef _WIN32
        std::wstring buffer(32768, L'\0');
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size && size < buffer.size()) return std::filesystem::path(buffer.substr(0, size)).parent_path();
#else
        std::error_code error;
        const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
        if (!error) return self.parent_path();
#endif
        std::error_code ignored;
        return std::filesystem::absolute(argv0 ? argv0 : ".", ignored).parent_path();
    }
    // [Mcp] WorkDir, else %ProgramData%\REPlusMcp: where the tools keep their
    // files.
    std::filesystem::path WorkFolder(const std::filesystem::path& system)
    {
        const auto mcp = Mcp::Tools::IniSection(system / "Reloaded_Editor.ini", "Mcp");
        if (const auto found = mcp.find("workdir"); found != mcp.end() && !found->second.empty())
            return std::filesystem::path(found->second); // the INI is in the ANSI code page, as path's narrow form
#ifdef _WIN32
        std::wstring data(32768, L'\0');
        const DWORD size = GetEnvironmentVariableW(L"ProgramData", data.data(), static_cast<DWORD>(data.size()));
        if (size && size < data.size()) return std::filesystem::path(data.substr(0, size)) / "REPlusMcp";
#endif
        return system / "ReloadedEditor" / "Mcp" / "work";
    }

    std::mutex diagnostics;
    void Diagnostic(const std::string& line)
    {
        std::lock_guard<std::mutex> guard(diagnostics);
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }

    // An editor that is never there, for the self-test's resource reads.
    class Offline : public Mcp::IBridge
    {
    public:
        Json Call(const std::string& op, const Json&, const Mcp::CallOptions&) override { throw Mcp::Bridge::Error(Mcp::Bridge::Code::NotConnected, "Self-test: no editor (" + op + ")."); }
        Json Hello() override { throw Mcp::Bridge::Error(Mcp::Bridge::Code::NotConnected, "Self-test: no editor."); }
        bool Connected() override { return false; }
        Json State() override { return Json::object(); }
    };

    void Usage(std::FILE* to)
    {
        std::fputs("Reloaded.Editor.Mcp " RE_PLUS_VERSION ": the MCP server for the RE+ editor (SCCT Versus Reloaded Editor).\n"
                   "  Reloaded.Editor.Mcp.exe              serve MCP on stdin/stdout (what MCP clients run)\n"
                   "  Reloaded.Editor.Mcp.exe --version    print the version\n"
                   "  Reloaded.Editor.Mcp.exe --self-test  check the tools and resources it serves\n"
                   "Claude Code: claude mcp add scct-reloaded -- \"<game>\\System\\Reloaded.Editor.Mcp.exe\"\n",
                   to);
    }
}

int Mcp::SelfTest(std::ostream& out)
{
    int failures = 0;
    auto fail = [&](const std::string& what) { out << "FAIL " << what << "\n"; ++failures; };
    Registry registry;
    try { Register(registry); }
    catch (const std::exception& e) { fail(std::string("registering tools: ") + e.what()); }
    std::set<std::string> names;
    for (const auto& tool : registry.tools)
    {
        if (!names.insert(tool.name).second) fail("tool listed twice: " + tool.name);
        if (tool.description.size() < 40) fail("tool " + tool.name + " has no real description");
        if (!tool.inputSchema.is_object() || tool.inputSchema.value("type", std::string()) != "object") fail("tool " + tool.name + " input schema is not an object schema");
        if (!tool.run) fail("tool " + tool.name + " does nothing");
    }
    out << registry.tools.size() << " tools, " << registry.resources.size() << " resources, " << registry.prompts.size() << " prompts, "
        << EmbeddedFiles().size() << " embedded files\n";

    Offline offline;
    ToolContext context{offline, {}, {}};
    for (const auto& resource : registry.resources)
    {
        if (resource.live || resource.isTemplate || !resource.read) continue;
        try
        {
            const auto read = resource.read(resource.uri, context);
            if (!read.is_object() || (!read.contains("text") && !read.contains("blob"))) fail("resource " + resource.uri + " reads as nothing");
        }
        catch (const std::exception& e) { fail("resource " + resource.uri + ": " + e.what()); }
    }
    for (const auto& prompt : registry.prompts)
    {
        Json arguments = Json::object();
        for (const auto& a : prompt.arguments) arguments[a.name] = "self-test";
        try
        {
            const auto messages = prompt.messages ? prompt.messages(arguments, context) : Json();
            if (!messages.is_array() || messages.empty()) fail("prompt " + prompt.name + " has no messages");
        }
        catch (const std::exception& e) { fail("prompt " + prompt.name + ": " + e.what()); }
    }

    out << (failures ? "self-test failed (" + std::to_string(failures) + ")" : std::string("self-test passed")) << "\n";
    return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    // Bytes in and out as they are: MCP's stdio is UTF-8 with \n line ends.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--version" || arg == "-v")
        {
            std::printf("Reloaded.Editor.Mcp %s (bridge protocol %d.%d; MCP %s to %s)\n", RE_PLUS_VERSION, Mcp::Bridge::kProtocolMajor, Mcp::Bridge::kProtocolMinor,
                        Mcp::Protocol::Versions().back().c_str(), Mcp::Protocol::Versions().front().c_str());
            return 0;
        }
        if (arg == "--self-test") return Mcp::SelfTest(std::cout);
        if (arg == "--help" || arg == "-h" || arg == "/?") { Usage(stdout); return 0; }
        if (arg == "serve" || arg == "--stdio") continue;
        Usage(stderr);
        return 2;
    }

    const auto system = ExeFolder(argc > 0 ? argv[0] : nullptr);
    const auto work = WorkFolder(system);
    {
        std::error_code ignored;
        std::filesystem::create_directories(work, ignored);
    }
    Mcp::Registry registry;
    Mcp::Register(registry);

    std::atomic<Mcp::Server*> current{nullptr};
    Mcp::BridgeClient::Options bridgeOptions;
    bridgeOptions.version = RE_PLUS_VERSION;
    bridgeOptions.onEvent = [&current](const Json& event) { if (auto* s = current.load()) s->OnBridgeEvent(event); };
    bridgeOptions.log = [&current](const std::string& line) { if (auto* s = current.load()) s->Log("info", line); else Diagnostic("[info] " + line); };
    Mcp::BridgeClient bridge(bridgeOptions);

    Mcp::ServerOptions options;
    options.systemDir = system;
    options.workDir = work;
    options.version = RE_PLUS_VERSION;
    options.cancel = [&bridge] { bridge.Cancel(); };
    options.resume = [&bridge] { bridge.Resume(); };
    options.log = Diagnostic;
    Mcp::Server server(registry, bridge, options, [](const std::string& line)
    {
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    });
    current = &server;
    Diagnostic("[info] Reloaded.Editor.Mcp " RE_PLUS_VERSION " serving MCP on stdio; System folder " + Mcp::Tools::PathText(system));
    std::string line;
    while (std::getline(std::cin, line)) server.Handle(line);
    const bool stopped = server.Finish(5000, 3000);
    current = nullptr;
    if (!stopped)
    {
        // A tool still waiting on the editor: leave without unwinding it.
        std::fflush(stdout);
        std::_Exit(0);
    }
    bridge.Disconnect();
    return 0;
}
