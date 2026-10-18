#include "pch.h"
#include "SelfUpdater.h"
#include "SelfUpdaterModel.h"
#include "Version.h"
#include "logger.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <zlib.h>
#include <richedit.h>
#include <shellapi.h>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

using Updater::Bytes;
using Updater::Release;
using Updater::Version;
namespace fs = std::filesystem;

namespace
{
    constexpr wchar_t kReleasesUrl[] = L"https://api.github.com/repos/Lumbridge/SCCT_Versus_Reloaded_Editor/releases?per_page=20";
    // + the tag: one release, for the notes of the version running.
    constexpr wchar_t kReleaseByTagUrl[] = L"https://api.github.com/repos/Lumbridge/SCCT_Versus_Reloaded_Editor/releases/tags/";
    constexpr wchar_t kApiHeaders[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    constexpr char kReleasePage[] = "https://github.com/Lumbridge/SCCT_Versus_Reloaded_Editor/releases/tag/v";
    constexpr wchar_t kTitle[] = L"RE+ Update";
    // The release notes window, shown a moment after the frame is up.
    constexpr UINT_PTR kWhatsNewTimer = 0x524E;
    constexpr UINT kWhatsNewDelayMs = 1500;
    constexpr char kSection[] = "Updates";
    // Long enough for the editor to finish opening before anything asks.
    constexpr DWORD kStartupDelayMs = 8000;
    constexpr size_t kMaxListBytes = 4u << 20, kMaxArchiveBytes = 64u << 20;
    // The files a release archive may replace, beside the DLL.
    const char* const kDllName = "Reloaded.Editor.dll";
    const char* const kLauncherName = "Reloaded_Editor.exe";
    // The MCP server, which an MCP client may be running: like the DLL it is
    // renamed aside rather than overwritten.
    const char* const kMcpName = "Reloaded.Editor.Mcp.exe";

    fs::path directory;
    std::atomic<bool> busy{false};
    // Set once an update or a roll back is in place, so a second check before
    // the restart does not offer the same release again.
    std::atomic<bool> installed{false};
    std::wstring installedName; // "RE+ 2.1.0" or "The previous version"
    HMENU helpMenu = nullptr;
    HWND frame = nullptr;
    // Set once the start-up clean-up has kept or removed the last update's files.
    HANDLE cleanedUp = nullptr;
    std::mutex fetchedLock;
    std::optional<Updater::WhatsNew> fetched;

    std::wstring Wide(const std::string& s)
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }
    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    std::string IniPath()
    {
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        return (fs::path(exe).parent_path() / "Reloaded_Editor.ini").string();
    }
    bool CheckOnStartup() { return GetPrivateProfileIntA(kSection, "CheckOnStartup", 1, IniPath().c_str()) != 0; }
    void SetCheckOnStartup(bool on) { WritePrivateProfileStringA(kSection, "CheckOnStartup", on ? "1" : "0", IniPath().c_str()); }
    std::string SkippedVersion()
    {
        char value[64] = {};
        GetPrivateProfileStringA(kSection, "SkippedVersion", "", value, sizeof(value), IniPath().c_str());
        return value;
    }
    void SetSkippedVersion(const std::string& v) { WritePrivateProfileStringA(kSection, "SkippedVersion", v.c_str(), IniPath().c_str()); }
    // Remind me later: RemindVersion is the tag, RemindAfter the time (UTC
    // seconds) before which the start-up check does not offer it again.
    Updater::OfferSettings OfferSettings()
    {
        Updater::OfferSettings s;
        s.skipped = SkippedVersion();
        char value[64] = {};
        GetPrivateProfileStringA(kSection, "RemindVersion", "", value, sizeof(value), IniPath().c_str());
        s.remindVersion = value;
        GetPrivateProfileStringA(kSection, "RemindAfter", "0", value, sizeof(value), IniPath().c_str());
        s.remindAfter = _strtoi64(value, nullptr, 10);
        return s;
    }
    void SetReminder(const std::string& tag, std::int64_t after)
    {
        WritePrivateProfileStringA(kSection, "RemindVersion", tag.c_str(), IniPath().c_str());
        WritePrivateProfileStringA(kSection, "RemindAfter", std::to_string(after).c_str(), IniPath().c_str());
    }
    std::int64_t Now() { return static_cast<std::int64_t>(std::time(nullptr)); }

    // Started by the MCP server's editor_launch, with -reMcp.
    bool StartedForMcp()
    {
        static const bool started = [] {
            int count = 0;
            LPWSTR* list = CommandLineToArgvW(GetCommandLineW(), &count);
            if (!list) return false;
            std::vector<std::string> arguments;
            for (int i = 0; i < count; ++i) arguments.push_back(Narrow(list[i]));
            LocalFree(list);
            return Updater::StartedForMcp(arguments);
        }();
        return started;
    }

    int Ask(const std::wstring& text, UINT flags)
    {
        // The worker thread owns no window, so the box leaves the editor
        // usable; it is kept on top so it does not open behind the frame.
        return MessageBoxW(nullptr, text.c_str(), kTitle, flags | MB_SETFOREGROUND | MB_TOPMOST);
    }

    struct Internet
    {
        HINTERNET h = nullptr;
        explicit Internet(HINTERNET handle) : h(handle) {}
        ~Internet() { if (h) WinHttpCloseHandle(h); }
        Internet(const Internet&) = delete;
        Internet& operator=(const Internet&) = delete;
    };

    [[noreturn]] void FailWin32(const std::string& what)
    {
        throw std::runtime_error(what + " (error " + std::to_string(GetLastError()) + ").");
    }

    // An HTTPS GET, following redirects, refused beyond limit bytes.
    Bytes Get(const std::wstring& url, const wchar_t* headers, size_t limit)
    {
        URL_COMPONENTS parts{sizeof(parts)};
        wchar_t host[256] = {}, path[2048] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = ARRAYSIZE(host);
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = ARRAYSIZE(path);
        wchar_t extra[2048] = {};
        parts.lpszExtraInfo = extra;
        parts.dwExtraInfoLength = ARRAYSIZE(extra);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
            throw std::runtime_error("Unusable update address.");
        const std::wstring agent = L"RE-Plus/" + Wide(RE_PLUS_VERSION);
        HINTERNET opened = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!opened) opened = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        Internet session(opened);
        if (!session.h) FailWin32("Could not start a connection");
        WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
        Internet connection(WinHttpConnect(session.h, host, parts.nPort, 0));
        if (!connection.h) FailWin32("Could not connect to GitHub");
        const std::wstring object = std::wstring(path) + extra;
        Internet request(WinHttpOpenRequest(connection.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!request.h) FailWin32("Could not create the request");
        if (!WinHttpSendRequest(request.h, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? static_cast<DWORD>(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(request.h, nullptr))
            FailWin32("Could not reach GitHub");
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 403 || status == 429)
            throw std::runtime_error("GitHub refused the request, probably because too many were made from this network. Try again in an hour.");
        if (status != 200) throw std::runtime_error("GitHub answered with HTTP status " + std::to_string(status) + ".");
        Bytes body;
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.h, &available)) FailWin32("The download was interrupted");
            if (!available) break;
            if (body.size() + available > limit) throw std::runtime_error("The download is larger than expected.");
            const size_t at = body.size();
            body.resize(at + available);
            DWORD read = 0;
            if (!WinHttpReadData(request.h, body.data() + at, available, &read)) FailWin32("The download was interrupted");
            body.resize(at + read);
        }
        return body;
    }

    std::string Sha256(const Bytes& data)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        unsigned char digest[32] = {};
        bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0
                  && BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0
                  && BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) >= 0
                  && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!ok) throw std::runtime_error("Could not check the download's SHA-256.");
        static const char hex[] = "0123456789abcdef";
        std::string out;
        for (unsigned char b : digest) out += hex[b >> 4], out += hex[b & 15];
        return out;
    }

    Bytes Extract(const Bytes& zip, const Updater::ZipEntry& entry)
    {
        Bytes out(entry.size);
        if (entry.method == 0)
        {
            if (entry.compressedSize != entry.size) throw std::runtime_error("The update archive is damaged.");
            std::copy_n(zip.begin() + entry.dataOffset, entry.size, out.begin());
        }
        else if (entry.method == 8)
        {
            z_stream s{};
            if (inflateInit2(&s, -MAX_WBITS) != Z_OK) throw std::runtime_error("Could not unpack the update archive.");
            s.next_in = const_cast<Bytef*>(zip.data() + entry.dataOffset);
            s.avail_in = entry.compressedSize;
            s.next_out = out.data();
            s.avail_out = static_cast<uInt>(out.size());
            const int result = inflate(&s, Z_FINISH);
            const bool complete = result == Z_STREAM_END && s.total_out == entry.size;
            inflateEnd(&s);
            if (!complete) throw std::runtime_error("The update archive is damaged: " + entry.name + " does not unpack.");
        }
        else throw std::runtime_error("The update archive uses an unsupported compression method.");
        if (Updater::Crc32(out.data(), out.size()) != entry.crc)
            throw std::runtime_error("The update archive is damaged: " + entry.name + " fails its checksum.");
        return out;
    }

    Bytes Load(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return in ? Bytes(std::istreambuf_iterator<char>(in), {}) : Bytes{};
    }

    void Store(const fs::path& path, const Bytes& data)
    {
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!out) throw std::runtime_error("Could not write " + path.string() + ". Is the folder read-only?");
        }
        if (Load(path) != data) throw std::runtime_error("Could not write " + path.string() + " intact.");
    }

    fs::path Old(const fs::path& target) { return fs::path(target.wstring() + L".old"); }
    fs::path Staged(const fs::path& target) { return fs::path(target.wstring() + L".update"); }

    // Puts the staged file in place of target, keeping the old one as
    // target.old. Renaming works on a DLL or EXE that is running.
    void Swap(const fs::path& target)
    {
        const auto staged = Staged(target), old = Old(target);
        const bool existed = fs::exists(target);
        if (existed && !MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            FailWin32("Could not move " + target.filename().string() + " aside");
        if (!MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const DWORD error = GetLastError();
            if (existed) MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            SetLastError(error);
            FailWin32("Could not put the new " + target.filename().string() + " in place");
        }
    }

    // Reloaded.Editor.previous.dll, Reloaded_Editor.previous.exe: the version
    // an update replaced, which Roll Back puts back.
    fs::path Previous(const fs::path& target)
    {
        return target.parent_path() / (target.stem().wstring() + L".previous" + target.extension().wstring());
    }

    // Puts image in place of target and keeps the running target as its
    // Previous, replacing the one image was read from: a swap that leaves the
    // newer version ready to switch back to.
    void SwapWithPrevious(const fs::path& target, const Bytes& image)
    {
        const auto staged = Staged(target), previous = Previous(target);
        Store(staged, image);
        if (!MoveFileExW(target.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const DWORD error = GetLastError();
            std::error_code ignored;
            fs::remove(staged, ignored);
            SetLastError(error);
            FailWin32("Could not move " + target.filename().string() + " aside");
        }
        if (!MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const DWORD error = GetLastError();
            // The running file back, then the previous one from its copy.
            MoveFileExW(previous.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            MoveFileExW(staged.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            SetLastError(error);
            FailWin32("Could not put the previous " + target.filename().string() + " in place");
        }
    }

    // What the last update left behind. The old DLL is no longer loaded once
    // the editor has restarted, and renaming works even while another open
    // editor still uses it, so it is kept as the previous version; staged
    // files of an interrupted install are removed.
    void CleanUp()
    {
        using Updater::CleanUpAction;
        const auto dll = directory / kDllName, launcher = directory / kLauncherName, mcp = directory / kMcpName;
        std::error_code e;
        Updater::Leftovers found;
        found.oldDll = fs::exists(Old(dll), e);
        found.oldLauncher = fs::exists(Old(launcher), e);
        found.previousLauncher = fs::exists(Previous(launcher), e);
        found.stagedDll = fs::exists(Staged(dll), e);
        found.stagedLauncher = fs::exists(Staged(launcher), e);
        found.oldMcp = fs::exists(Old(mcp), e);
        found.previousMcp = fs::exists(Previous(mcp), e);
        found.stagedMcp = fs::exists(Staged(mcp), e);
        auto keep = [](const fs::path& from, const fs::path& to) {
            if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                Logger::log("Updater: kept " + from.filename().string() + " as " + to.filename().string());
                return true;
            }
            Logger::log("Updater: could not keep " + from.filename().string() + " as " + to.filename().string()
                        + " yet (error " + std::to_string(GetLastError()) + ")");
            return false;
        };
        auto remove = [](const fs::path& path) {
            std::error_code error;
            if (fs::remove(path, error) || !fs::exists(path, error)) return true;
            Logger::log("Updater: could not remove " + path.string() + " yet");
            return false;
        };
        bool keptDll = true;
        for (const auto& step : Updater::CleanUpPlan(found))
        {
            if (step.needsDll && !keptDll) continue;
            switch (step.action)
            {
            case CleanUpAction::KeepOldDll: keptDll = keep(Old(dll), Previous(dll)); break;
            case CleanUpAction::KeepOldLauncher: keep(Old(launcher), Previous(launcher)); break;
            case CleanUpAction::RemovePreviousLauncher: remove(Previous(launcher)); break;
            case CleanUpAction::RemoveOldLauncher: remove(Old(launcher)); break;
            case CleanUpAction::RemoveStagedDll: remove(Staged(dll)); break;
            case CleanUpAction::RemoveStagedLauncher: remove(Staged(launcher)); break;
            // A server an MCP client still runs can be renamed but not removed;
            // the removal is tried again at the next start.
            case CleanUpAction::KeepOldMcp: keep(Old(mcp), Previous(mcp)); break;
            case CleanUpAction::RemovePreviousMcp: remove(Previous(mcp)); break;
            case CleanUpAction::RemoveOldMcp: remove(Old(mcp)); break;
            case CleanUpAction::RemoveStagedMcp: remove(Staged(mcp)); break;
            }
        }
    }

    // The version in a DLL's file properties; nullopt for 1.x builds, which
    // did not record it.
    std::optional<Version> FileVersion(const fs::path& file)
    {
        DWORD ignored = 0;
        const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
        if (!size) return std::nullopt;
        std::vector<BYTE> data(size);
        if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return std::nullopt;
        struct Translation { WORD language, codePage; };
        Translation* translation = nullptr;
        UINT length = 0;
        if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translation), &length) || length < sizeof(Translation))
            return std::nullopt;
        wchar_t key[64] = {};
        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductVersion", translation->language, translation->codePage);
        wchar_t* value = nullptr;
        if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &length) || !length || !value) return std::nullopt;
        return Updater::ProductVersion(Narrow(value));
    }

    // What's new: the notes of the release last installed.
    fs::path WhatsNewPath() { return directory / "ReloadedEditor" / "WhatsNew.txt"; }
    std::optional<Updater::WhatsNew> LoadWhatsNew()
    {
        const auto bytes = Load(WhatsNewPath());
        return Updater::WhatsNew::Parse(std::string(bytes.begin(), bytes.end()));
    }
    void SaveWhatsNew(const Updater::WhatsNew& record)
    {
        try
        {
            std::error_code e;
            fs::create_directories(WhatsNewPath().parent_path(), e);
            const auto text = record.Format();
            Store(WhatsNewPath(), Bytes(text.begin(), text.end()));
        }
        catch (const std::exception& e) { Logger::log(std::string("Updater: could not save the release notes: ") + e.what()); }
    }

    // The notes window, in two kinds: What's New (the notes alone) and the
    // offer of a newer release (the notes, the download size and Install /
    // Remind me later / Skip this version). Notes are laid out in a rich edit
    // control from the Markdown GitHub gives; links open in the browser.
    enum NotesId { NotesText = 1, OfferStatus, OfferSnooze, OfferInstall, OfferRemind, OfferSkip };
    constexpr UINT kInstallFinished = WM_APP + 0x31;
    HWND whatsNewWindow = nullptr, offerWindow = nullptr;
    bool richEdit = false;
    // The release the offer window shows; the frame's thread only.
    std::optional<Release> offered;
    bool installing = false, offerDone = false;
    // From the check and install workers to the frame's thread.
    std::mutex offerLock;
    std::optional<Release> pendingOffer;
    std::string installOutcome;
    bool installOk = false;
    HFONT uiFont = nullptr;

    void StartInstall(HWND window);
    void UpdateRollBackItem();

    bool IsOffer(HWND window) { return GetWindowLongPtrW(window, GWLP_USERDATA) == 1; }

    void LayOut(HWND window, int width, int height)
    {
        if (!IsOffer(window))
        {
            MoveWindow(GetDlgItem(window, NotesText), 8, 8, (std::max)(1, width - 16), (std::max)(1, height - 16), TRUE);
            return;
        }
        const int buttons = height - 38, status = buttons - 28;
        MoveWindow(GetDlgItem(window, NotesText), 8, 8, (std::max)(1, width - 16), (std::max)(1, status - 14), TRUE);
        MoveWindow(GetDlgItem(window, OfferStatus), 10, status, (std::max)(1, width - 20), 22, TRUE);
        int x = width - 8;
        auto place = [&](int id, int w, int h = 28) { x -= w; MoveWindow(GetDlgItem(window, id), x, buttons + (28 - h) / 2 + (id == OfferSnooze ? 3 : 0), w, id == OfferSnooze ? 200 : h, TRUE); x -= 6; };
        place(OfferInstall, 100);
        place(OfferRemind, 120);
        place(OfferSnooze, 130, 24);
        MoveWindow(GetDlgItem(window, OfferSkip), 8, buttons, 130, 28, TRUE);
    }

    void SetStatus(HWND window, const std::string& text) { SetWindowTextW(GetDlgItem(window, OfferStatus), Wide(text).c_str()); }

    LRESULT CALLBACK NotesProc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            const bool offer = reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams != nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, offer ? 1 : 0);
            if (!uiFont)
            {
                NONCLIENTMETRICSW metrics{};
                metrics.cbSize = sizeof(metrics);
                SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
                uiFont = CreateFontIndirectW(&metrics.lfMessageFont);
            }
            HINSTANCE instance = GetModuleHandle(nullptr);
            static const bool loaded = LoadLibraryW(L"Msftedit.dll") != nullptr;
            richEdit = loaded;
            HWND notes = CreateWindowExW(WS_EX_CLIENTEDGE, richEdit ? MSFTEDIT_CLASS : L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                         8, 8, 600, 400, window, reinterpret_cast<HMENU>(NotesText), instance, nullptr);
            if (richEdit)
            {
                SendMessageW(notes, EM_SETBKGNDCOLOR, 0, GetSysColor(COLOR_WINDOW));
                SendMessageW(notes, EM_AUTOURLDETECT, TRUE, 0);
                SendMessageW(notes, EM_SETEVENTMASK, 0, ENM_LINK);
                SendMessageW(notes, EM_EXLIMITTEXT, 0, 1 << 20);
            }
            else
            {
                SendMessageW(notes, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), TRUE);
                SendMessageW(notes, EM_SETLIMITTEXT, 0, 0);
            }
            if (!offer) return 0;
            auto add = [&](const wchar_t* cls, const wchar_t* text, int id, DWORD style) {
                HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), FALSE);
                return child;
            };
            add(L"STATIC", L"", OfferStatus, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
            HWND snooze = add(L"COMBOBOX", L"", OfferSnooze, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST);
            for (auto s : {Updater::Snooze::NextStart, Updater::Snooze::OneDay, Updater::Snooze::ThreeDays, Updater::Snooze::OneWeek})
                SendMessageW(snooze, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Wide(Updater::SnoozeText(s)).c_str()));
            SendMessageW(snooze, CB_SETCURSEL, 0, 0);
            add(L"BUTTON", L"&Install", OfferInstall, WS_TABSTOP | BS_DEFPUSHBUTTON);
            add(L"BUTTON", L"&Remind me later", OfferRemind, WS_TABSTOP | BS_PUSHBUTTON);
            add(L"BUTTON", L"&Skip this version", OfferSkip, WS_TABSTOP | BS_PUSHBUTTON);
            return 0;
        }
        case WM_SIZE:
            LayOut(window, LOWORD(l), HIWORD(l));
            return 0;
        case WM_NOTIFY:
        {
            const auto* link = reinterpret_cast<ENLINK*>(l);
            if (link->nmhdr.idFrom != NotesText || link->nmhdr.code != EN_LINK || link->msg != WM_LBUTTONUP) break;
            const LONG length = link->chrg.cpMax - link->chrg.cpMin;
            if (length <= 0 || length > 2048) return 0;
            std::wstring url(static_cast<size_t>(length) + 1, L'\0');
            TEXTRANGEW range{link->chrg, url.data()};
            SendMessageW(link->nmhdr.hwndFrom, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&range));
            url.resize(wcslen(url.c_str()));
            // Only web links: the notes come from GitHub, not from this machine.
            if (url.rfind(L"https://", 0) == 0) ShellExecuteW(window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 1;
        }
        case WM_COMMAND:
        {
            if (!IsOffer(window) || !offered) break;
            const int id = LOWORD(w);
            if (id == OfferInstall)
            {
                if (offerDone) DestroyWindow(window);
                else StartInstall(window);
                return 0;
            }
            if (id == OfferRemind)
            {
                auto choice = static_cast<int>(SendMessageW(GetDlgItem(window, OfferSnooze), CB_GETCURSEL, 0, 0));
                const auto snooze = static_cast<Updater::Snooze>(std::clamp(choice, 0, 3));
                SetReminder(offered->tag, Updater::RemindAfter(Now(), snooze));
                Logger::log("Updater: reminding about " + offered->tag + " " + Updater::Lower(Updater::SnoozeText(snooze)));
                DestroyWindow(window);
                return 0;
            }
            if (id == OfferSkip)
            {
                SetSkippedVersion(offered->tag);
                Logger::log("Updater: skipping " + offered->tag);
                DestroyWindow(window);
                return 0;
            }
            break;
        }
        case kInstallFinished:
        {
            std::string outcome;
            bool ok = false;
            {
                std::lock_guard lock(offerLock);
                outcome = installOutcome;
                ok = installOk;
            }
            installing = false;
            SetStatus(window, outcome);
            for (int id : {OfferSnooze, OfferRemind, OfferSkip}) ShowWindow(GetDlgItem(window, id), ok ? SW_HIDE : SW_SHOW), EnableWindow(GetDlgItem(window, id), TRUE);
            EnableWindow(GetDlgItem(window, OfferInstall), TRUE);
            SetWindowTextW(GetDlgItem(window, OfferInstall), ok ? L"&Close" : L"Try &again");
            offerDone = ok;
            if (ok) UpdateRollBackItem();
            return 0;
        }
        case WM_CLOSE:
            // The install finishes and reports here; it cannot be stopped half way.
            if (installing) return 0;
            DestroyWindow(window);
            return 0;
        case WM_NCDESTROY:
            if (window == whatsNewWindow) whatsNewWindow = nullptr;
            if (window == offerWindow) offerWindow = nullptr, offered.reset(), offerDone = false;
            break;
        }
        return DefWindowProcW(window, message, w, l);
    }

    HWND NotesWindow(bool offer, const std::wstring& caption, int width, int height)
    {
        HWND& window = offer ? offerWindow : whatsNewWindow;
        if (window) { SetWindowTextW(window, caption.c_str()); return window; }
        static bool registered = false;
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpfnWndProc = NotesProc;
        wc.lpszClassName = L"ReloadedWhatsNew";
        if (!registered) registered = RegisterClassW(&wc) != 0;
        window = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, caption.c_str(), WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, width, height, frame, nullptr, wc.hInstance, offer ? reinterpret_cast<void*>(1) : nullptr);
        if (!window) Logger::log("Updater: could not open the release notes window");
        return window;
    }

    void SetNotes(HWND window, const std::string& title, const std::string& subtitle, const std::string& notes, bool markdown, const std::string& footer)
    {
        HWND control = GetDlgItem(window, NotesText);
        const std::string body = notes.empty() ? std::string("This release has no notes.") : notes;
        if (richEdit)
        {
            const auto rtf = Updater::NotesRtf(title, subtitle, Updater::ParseNotes(body, markdown && !notes.empty()), footer);
            SETTEXTEX how{ST_DEFAULT, CP_ACP};
            SendMessageW(control, EM_SETTEXTEX, reinterpret_cast<WPARAM>(&how), reinterpret_cast<LPARAM>(rtf.c_str()));
        }
        else
        {
            std::string text = title + "\r\n" + (subtitle.empty() ? "" : subtitle + "\r\n") + "\r\n"
                               + (markdown ? Updater::PlainNotes(body, Updater::WhatsNew::kNotesLimit) : body) + "\r\n\r\n" + footer;
            SetWindowTextW(control, Wide(text).c_str());
        }
        SendMessageW(control, EM_SETSEL, 0, 0);
        SendMessageW(control, EM_SCROLLCARET, 0, 0);
    }

    // A resizable, read-only window: release notes can be long.
    void ShowWhatsNew(const Updater::WhatsNew& record)
    {
        HWND window = NotesWindow(false, L"What's New in " RE_PLUS_NAME L" " + Wide(record.version), 640, 520);
        if (!window) return;
        SetNotes(window, record.title.empty() ? RE_PLUS_NAME " " + record.version : record.title, "", record.notes, record.markdown,
                 kReleasePage + record.version);
        ShowWindow(window, SW_SHOWNORMAL);
        SetForegroundWindow(window);
    }

    // The offer of a newer release: its notes and download size, and what to do.
    void ShowOffer(const Release& release)
    {
        if (installing) return;
        HWND window = NotesWindow(true, L"RE+ Update", 700, 580);
        if (!window) return;
        offered = release;
        offerDone = false;
        const std::string title = "RE+ " + release.version.ToString() + (release.prerelease ? " (release candidate)" : "") + " is available";
        SetNotes(window, title, Updater::OfferSummary(release, RE_PLUS_VERSION), release.notes, true,
                 release.page.empty() ? kReleasePage + release.version.ToString() : release.page);
        SetStatus(window, "Install now, or choose when to be reminded.");
        for (int id : {OfferSnooze, OfferRemind, OfferSkip, OfferInstall}) ShowWindow(GetDlgItem(window, id), SW_SHOW), EnableWindow(GetDlgItem(window, id), TRUE);
        SetWindowTextW(GetDlgItem(window, OfferInstall), L"&Install");
        ShowWindow(window, SW_SHOWNORMAL);
        SetForegroundWindow(window);
        SetFocus(GetDlgItem(window, OfferInstall));
    }

    // Once, at the first start of a version the updater installed.
    void CALLBACK WhatsNewAtStart(HWND window, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(window, id);
        auto record = LoadWhatsNew();
        if (!record || !record->ShowAtStart(RE_PLUS_VERSION)) return;
        record->shown = true;
        SaveWhatsNew(*record);
        Logger::log("Updater: showing what's new in " + record->version);
        ShowWhatsNew(*record);
    }

    // Help > What's New when the notes are not kept here: the editor was
    // installed by hand, or the notes belong to another version.
    DWORD WINAPI FetchNotesThread(LPVOID)
    {
        try
        {
            const auto body = Get(kReleaseByTagUrl + Wide("v" RE_PLUS_VERSION), kApiHeaders, kMaxListBytes);
            const auto release = Updater::Json::parse(body.begin(), body.end(), nullptr, false);
            if (release.is_discarded()) throw std::runtime_error("GitHub returned a release that could not be read.");
            const auto [title, notes] = Updater::TitleAndNotes(release);
            {
                std::lock_guard lock(fetchedLock);
                fetched = Updater::WhatsNew{RE_PLUS_VERSION, title, Updater::ClipUtf8(notes, Updater::WhatsNew::kNotesLimit), true, true};
            }
            if (!frame || !PostMessageW(frame, WM_COMMAND, SelfUpdater::kShowFetchedNotes, 0)) throw std::runtime_error("The editor window went away.");
        }
        catch (const std::exception& e)
        {
            Logger::log(std::string("Updater: could not fetch the release notes: ") + e.what());
            Ask(L"Could not fetch the notes for " RE_PLUS_DISPLAY_VERSION L".\r\n\r\n" + Wide(e.what())
                    + L"\r\n\r\nEvery release's notes are at https://github.com/Lumbridge/SCCT_Versus_Reloaded_Editor/releases",
                MB_OK | MB_ICONWARNING);
        }
        return 0;
    }

    std::wstring PreviousName(const std::optional<Version>& version)
    {
        return version ? L"RE+ " + Wide(version->ToString()) : L"the previous version";
    }
    // "Roll Back to RE+ 2.0.0..." (or Forward, after rolling back), greyed
    // when no previous version is kept or a change waits for a restart.
    void UpdateRollBackItem()
    {
        if (!helpMenu) return;
        const auto previous = Previous(directory / kDllName);
        std::error_code e;
        std::wstring label;
        UINT flags = MF_BYCOMMAND | MF_STRING;
        if (installed) label = L"Roll &Back (restart the editor to finish the change)", flags |= MF_GRAYED;
        else if (!fs::exists(previous, e)) label = L"Roll &Back to a Previous Version", flags |= MF_GRAYED;
        else
        {
            const auto version = FileVersion(previous);
            const auto current = Version::Parse(RE_PLUS_VERSION);
            const bool forward = version && current && *current < *version;
            label = std::wstring(forward ? L"Roll &Forward to " : L"Roll &Back to ") + (version ? PreviousName(version) : L"the Previous Version") + L"...";
        }
        ModifyMenuW(helpMenu, SelfUpdater::kRollBack, flags, SelfUpdater::kRollBack, label.c_str());
    }

    void RollBack()
    {
        const auto dll = directory / kDllName, launcher = directory / kLauncherName, mcp = directory / kMcpName, previous = Previous(dll);
        auto tell = [](const std::wstring& text, UINT icon) { MessageBoxW(frame, text.c_str(), L"RE+ Roll Back", MB_OK | icon); };
        bool mcpSwapped = false;
        if (installed)
        {
            tell(installedName + L" is already in place. Save your work and restart the editor to start using it.", MB_ICONINFORMATION);
            return;
        }
        std::error_code e;
        if (!fs::exists(previous, e))
        {
            tell(L"No previous version is kept. One is kept from the next update on.", MB_ICONINFORMATION);
            UpdateRollBackItem();
            return;
        }
        const auto version = FileVersion(previous);
        const auto current = Version::Parse(RE_PLUS_VERSION);
        const bool forward = version && current && *current < *version;
        const auto skip = Updater::SkipAfterRollBack(RE_PLUS_VERSION, version);
        auto name = PreviousName(version);
        std::wstring text = std::wstring(forward ? L"Switch to " : L"Roll back to ") + name + L"?\r\n\r\n"
                            RE_PLUS_DISPLAY_VERSION L" is kept in its place, so you can switch back from the Help menu. "
                            L"The change takes effect when you restart the editor.";
        if (!skip.empty())
            text += L"\r\n\r\nThe update check at startup will not offer " RE_PLUS_DISPLAY_VERSION
                    L" again; Help > Check for RE+ Updates still can.";
        if (MessageBoxW(frame, text.c_str(), L"RE+ Roll Back", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        if (busy.exchange(true))
        {
            tell(L"An update check is running. Try again when it has finished.", MB_ICONINFORMATION);
            return;
        }
        struct Done { ~Done() { busy = false; } } done;
        try
        {
            const auto image = Load(previous);
            if (Updater::ImageKind(image) != Updater::Image::Dll)
                throw std::runtime_error("Reloaded.Editor.previous.dll is not a 32-bit Windows DLL.");
            SwapWithPrevious(dll, image);
            if (fs::exists(Previous(launcher), e))
                try
                {
                    const auto exe = Load(Previous(launcher));
                    if (Updater::ImageKind(exe) != Updater::Image::Exe) throw std::runtime_error("it is not a 32-bit Windows program");
                    SwapWithPrevious(launcher, exe);
                }
                catch (const std::exception& error)
                {
                    // The launcher only injects the DLL; either one starts either DLL.
                    Logger::log(std::string("Updater: launcher left as it was: ") + error.what());
                }
            // The MCP server kept with the previous DLL speaks its pipe protocol.
            if (fs::exists(Previous(mcp), e))
                try
                {
                    const auto exe = Load(Previous(mcp));
                    if (Updater::ImageKind(exe) != Updater::Image::Exe) throw std::runtime_error("it is not a 32-bit Windows program");
                    SwapWithPrevious(mcp, exe);
                    mcpSwapped = true;
                }
                catch (const std::exception& error)
                {
                    Logger::log(std::string("Updater: MCP server left as it was: ") + error.what());
                }
        }
        catch (const std::exception& error)
        {
            Logger::log(std::string("Updater: roll back failed: ") + error.what());
            tell(L"Could not roll back. The editor is unchanged.\r\n\r\n" + Wide(error.what()), MB_ICONERROR);
            return;
        }
        if (!skip.empty()) SetSkippedVersion(skip);
        name[0] = towupper(name[0]);
        installedName = name;
        installed = true;
        Logger::log("Updater: rolled " + std::string(forward ? "forward" : "back") + " from " RE_PLUS_VERSION " to " + Narrow(name));
        UpdateRollBackItem();
        tell(name + L" is in place. Save your work and restart the editor to start using it."
                 + (mcpSwapped ? L" Restart your MCP client too, so it starts the matching MCP server." : L""),
             MB_ICONINFORMATION);
    }

    // A pre-release (2.1.0-rc.1) is only offered to an editor already running
    // one; everyone else waits for the release.
    std::optional<Release> Fetch(bool includePrereleases)
    {
        const auto body = Get(kReleasesUrl, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", kMaxListBytes);
        const auto list = Updater::Json::parse(body.begin(), body.end(), nullptr, false);
        if (list.is_discarded()) throw std::runtime_error("GitHub returned a releases list that could not be read.");
        return Updater::Newest(list, includePrereleases);
    }

    // Empty, or a note for the outcome when the MCP server could not be replaced.
    std::string Install(const Release& release)
    {
        Logger::log("Updater: downloading " + release.assetUrl);
        const auto zip = Get(Wide(release.assetUrl), nullptr, kMaxArchiveBytes);
        if (release.assetSize && zip.size() != release.assetSize)
            throw std::runtime_error("The download is incomplete. Try again.");
        if (!release.sha256.empty())
        {
            if (Sha256(zip) != release.sha256) throw std::runtime_error("The download does not match the SHA-256 GitHub published for it. Try again.");
        }
        else Logger::log("Updater: GitHub published no SHA-256 for " + release.assetName + "; relying on the zip checksums");

        const auto entries = Updater::ReadZip(zip);
        const auto* dllEntry = Updater::FindEntry(entries, kDllName);
        if (!dllEntry) throw std::runtime_error(std::string("The update archive does not contain ") + kDllName + ".");
        const auto dll = Extract(zip, *dllEntry);
        if (Updater::ImageKind(dll) != Updater::Image::Dll) throw std::runtime_error(std::string("The update's ") + kDllName + " is not a 32-bit Windows DLL.");
        Bytes launcher;
        if (const auto* e = Updater::FindEntry(entries, kLauncherName))
        {
            launcher = Extract(zip, *e);
            if (Updater::ImageKind(launcher) != Updater::Image::Exe) throw std::runtime_error(std::string("The update's ") + kLauncherName + " is not a 32-bit Windows program.");
        }
        // Releases before the MCP server have none; an installed one is then kept.
        Bytes mcp;
        if (const auto* e = Updater::FindEntry(entries, kMcpName))
        {
            mcp = Extract(zip, *e);
            if (Updater::ImageKind(mcp) != Updater::Image::Exe) throw std::runtime_error(std::string("The update's ") + kMcpName + " is not a 32-bit Windows program.");
        }

        // Stage everything before replacing anything.
        const auto dllPath = directory / kDllName, launcherPath = directory / kLauncherName, mcpPath = directory / kMcpName;
        const bool newLauncher = !launcher.empty() && Load(launcherPath) != launcher;
        const bool newMcp = !mcp.empty() && Load(mcpPath) != mcp;
        auto unstage = [&] {
            std::error_code e;
            fs::remove(Staged(dllPath), e);
            fs::remove(Staged(launcherPath), e);
            fs::remove(Staged(mcpPath), e);
        };
        try
        {
            Store(Staged(dllPath), dll);
            if (newLauncher) Store(Staged(launcherPath), launcher);
            if (newMcp) Store(Staged(mcpPath), mcp);
            Swap(dllPath);
        }
        catch (...)
        {
            unstage();
            throw;
        }
        if (newLauncher)
            try { Swap(launcherPath); }
            catch (const std::exception& e)
            {
                // The launcher only injects the DLL; the old one starts the new DLL fine.
                Logger::log(std::string("Updater: launcher left as it was: ") + e.what());
                std::error_code ignored;
                fs::remove(Staged(launcherPath), ignored);
            }
        std::string note;
        if (newMcp)
            try { Swap(mcpPath); }
            catch (const std::exception& e)
            {
                // The old server cannot talk to a DLL with a newer pipe protocol,
                // but the editor itself is fine; say so rather than undo it all.
                Logger::log(std::string("Updater: MCP server left as it was: ") + e.what());
                std::error_code ignored;
                fs::remove(Staged(mcpPath), ignored);
                note = std::string(" ") + kMcpName + " could not be replaced (" + e.what() + "); copy it from the release archive by hand.";
            }
        Logger::log("Updater: installed " + release.version.ToString());
        return note;
    }

    void Check(bool interactive)
    {
        if (busy.exchange(true))
        {
            if (interactive) Ask(L"An update check is already running.", MB_OK | MB_ICONINFORMATION);
            return;
        }
        struct Done { ~Done() { busy = false; } } done;
        const auto current = Version::Parse(RE_PLUS_VERSION);
        try
        {
            if (installed)
            {
                if (interactive)
                    Ask(installedName + L" is already in place. Save your work and restart the editor to start using it.", MB_OK | MB_ICONINFORMATION);
                return;
            }
            const auto newest = Fetch(current && !current->pre.empty());
            if (!newest || !current || !(*current < newest->version))
            {
                Logger::log("Updater: up to date (" RE_PLUS_VERSION ")");
                if (interactive) Ask(L"You have the latest version, " RE_PLUS_DISPLAY_VERSION L".", MB_OK | MB_ICONINFORMATION);
                return;
            }
            const auto& release = *newest;
            if (!interactive && !Updater::OfferAtStartup(release.tag, OfferSettings(), Now()))
            {
                Logger::log("Updater: " + release.tag + " is available; skipped or reminding later");
                return;
            }
            Logger::log("Updater: " + release.tag + " is available");
            {
                std::lock_guard lock(offerLock);
                pendingOffer = release;
            }
            // The offer window belongs to the frame's thread. At start-up the
            // frame may not have its menu yet, which is when it is known here.
            for (int i = 0; !frame && i < 120; ++i) Sleep(500);
            if (!frame || !PostMessageW(frame, WM_COMMAND, SelfUpdater::kShowOffer, 0))
                Logger::log("Updater: could not show the offer; the editor window is not up");
        }
        catch (const std::exception& e)
        {
            // An automatic check stays quiet: offline is normal.
            Logger::log(std::string("Updater: check failed: ") + e.what());
            if (interactive) Ask(L"Could not check for updates.\r\n\r\n" + Wide(e.what()), MB_OK | MB_ICONWARNING);
        }
    }

    // Install from the offer window: the download and swap run on a worker,
    // which reports to the window when done.
    struct InstallJob
    {
        Release release;
        HWND window;
    };
    DWORD WINAPI InstallThread(LPVOID parameter)
    {
        std::unique_ptr<InstallJob> job(static_cast<InstallJob*>(parameter));
        const auto& release = job->release;
        std::string outcome;
        bool ok = false;
        {
            struct Done { ~Done() { busy = false; } } done;
            try
            {
                const auto note = Install(release);
                installedName = L"RE+ " + Wide(release.version.ToString());
                installed = true;
                SetSkippedVersion("");
                SetReminder("", 0);
                // Shown once by the first start of the new version.
                SaveWhatsNew({release.version.ToString(), release.name.empty() ? release.tag : release.name,
                              Updater::ClipUtf8(release.notes, Updater::WhatsNew::kNotesLimit), false, true});
                outcome = "RE+ " + release.version.ToString() + " is installed. Save your work and restart the editor to start using it." + note;
                ok = true;
            }
            catch (const std::exception& e)
            {
                Logger::log(std::string("Updater: install failed: ") + e.what());
                outcome = std::string("Could not install the update; the editor is unchanged. ") + e.what();
            }
        }
        {
            std::lock_guard lock(offerLock);
            installOutcome = outcome;
            installOk = ok;
        }
        if (!PostMessageW(job->window, kInstallFinished, 0, 0))
            Ask(Wide(outcome), MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        return 0;
    }
    void StartInstall(HWND window)
    {
        if (!offered || installing) return;
        if (installed)
        {
            SetStatus(window, Narrow(installedName) + " is already in place. Save your work and restart the editor to start using it.");
            return;
        }
        if (busy.exchange(true))
        {
            SetStatus(window, "An update check is running. Try again in a moment.");
            return;
        }
        installing = true;
        for (int id : {OfferSnooze, OfferRemind, OfferSkip, OfferInstall}) EnableWindow(GetDlgItem(window, id), FALSE);
        SetStatus(window, "Downloading and installing " + offered->assetName + " (" + Updater::SizeText(offered->assetSize) + ")...");
        auto* job = new InstallJob{*offered, window};
        if (HANDLE h = CreateThread(nullptr, 0, InstallThread, job, 0, nullptr)) { CloseHandle(h); return; }
        delete job;
        busy = false;
        installing = false;
        for (int id : {OfferSnooze, OfferRemind, OfferSkip, OfferInstall}) EnableWindow(GetDlgItem(window, id), TRUE);
        SetStatus(window, "Could not start the install.");
    }

    DWORD WINAPI StartupThread(LPVOID)
    {
        CleanUp();
        SetEvent(cleanedUp);
        // An editor the MCP server started would answer its calls busy while
        // the offer is up; Help > Check for RE+ Updates still works.
        if (StartedForMcp())
        {
            Logger::log("Updater: started for MCP (-reMcp); no update check at start-up");
            return 0;
        }
        if (!CheckOnStartup()) return 0;
        Sleep(kStartupDelayMs);
        Check(false);
        return 0;
    }
    DWORD WINAPI CheckThread(LPVOID) { Check(true); return 0; }
}

void SelfUpdater::Initialize(const std::wstring& dllPath)
{
    directory = fs::path(dllPath).parent_path();
    cleanedUp = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // DllMain holds the loader lock; everything happens on the worker.
    if (HANDLE h = CreateThread(nullptr, 0, StartupThread, nullptr, 0, nullptr)) CloseHandle(h);
    else if (cleanedUp) SetEvent(cleanedUp);
}

void SelfUpdater::AppendHelpMenu(HMENU help, HWND frameWindow)
{
    if (GetMenuState(help, kCheckNow, MF_BYCOMMAND) != UINT(-1)) return;
    helpMenu = help;
    frame = frameWindow;
    // The Roll Back item names the kept version, which the clean-up may only
    // just have put in place.
    if (cleanedUp) WaitForSingleObject(cleanedUp, 3000);
    AppendMenuA(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(help, MF_STRING, kCheckNow, "Check for " RE_PLUS_NAME " &Updates...");
    AppendMenuA(help, MF_STRING | (CheckOnStartup() ? MF_CHECKED : MF_UNCHECKED), kToggleStartupCheck, "Check for Updates at S&tartup");
    AppendMenuA(help, MF_STRING, kWhatsNew, "What's &New in " RE_PLUS_NAME "...");
    AppendMenuA(help, MF_STRING, kRollBack, "Roll &Back");
    UpdateRollBackItem();
    AppendMenuA(help, MF_STRING, kAbout, "&About " RE_PLUS_NAME "...");
    // Not in an editor the MCP server started: the notes stay unshown until
    // the next start by hand.
    static bool scheduled = false;
    if (!scheduled && frame && !StartedForMcp()) scheduled = SetTimer(frame, kWhatsNewTimer, kWhatsNewDelayMs, WhatsNewAtStart) != 0;
}

bool SelfUpdater::HandleCommand(UINT command)
{
    if (command == kAbout)
    {
        MessageBoxA(GetActiveWindow(),
            RE_PLUS_DISPLAY_VERSION "\r\n\r\n"
            RE_PLUS_FULL_NAME ": a patch for the Splinter Cell: Chaos Theory Versus map editor, "
            "built on AllyPal's Reloaded Editor 1.21.\r\n\r\n"
            "https://github.com/Lumbridge/SCCT_Versus_Reloaded_Editor",
            "About " RE_PLUS_NAME, MB_OK | MB_ICONINFORMATION);
        return true;
    }
    if (command == kCheckNow)
    {
        if (HANDLE h = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr)) CloseHandle(h);
        return true;
    }
    if (command == kToggleStartupCheck)
    {
        const bool on = !CheckOnStartup();
        SetCheckOnStartup(on);
        if (helpMenu) CheckMenuItem(helpMenu, kToggleStartupCheck, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        return true;
    }
    if (command == kWhatsNew)
    {
        if (const auto record = LoadWhatsNew(); record && record->For(RE_PLUS_VERSION)) ShowWhatsNew(*record);
        else if (HANDLE h = CreateThread(nullptr, 0, FetchNotesThread, nullptr, 0, nullptr)) CloseHandle(h);
        return true;
    }
    if (command == kShowFetchedNotes)
    {
        std::optional<Updater::WhatsNew> notes;
        {
            std::lock_guard lock(fetchedLock);
            notes.swap(fetched);
        }
        if (notes) ShowWhatsNew(*notes);
        return true;
    }
    if (command == kShowOffer)
    {
        std::optional<Release> release;
        {
            std::lock_guard lock(offerLock);
            release.swap(pendingOffer);
        }
        if (release) ShowOffer(*release);
        return true;
    }
    if (command == kRollBack)
    {
        RollBack();
        return true;
    }
    return false;
}
