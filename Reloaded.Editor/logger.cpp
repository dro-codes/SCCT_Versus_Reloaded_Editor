#include "pch.h"
#include "logger.h"
#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <iomanip>
#include <sstream>
#include <Windows.h>
#include <thread>
#include <memory>
#include <chrono>
#include <ctime>
#include <filesystem>

static std::wstring logFilePath;

namespace
{
// The MCP bridge's pipe thread and the engine's log hook write from other
// threads than the editor's: one line at a time, whole. Never freed, so a
// thread still logging while the DLL unloads finds it intact.
CRITICAL_SECTION& LogLock()
{
    static CRITICAL_SECTION* lock = [] { auto section = new CRITICAL_SECTION; InitializeCriticalSection(section); return section; }();
    return *lock;
}
struct LogGuard
{
    LogGuard() { EnterCriticalSection(&LogLock()); }
    ~LogGuard() { LeaveCriticalSection(&LogLock()); }
    LogGuard(const LogGuard&) = delete;
    LogGuard& operator=(const LogGuard&) = delete;
};
}

std::string TimePrefix() {
    auto now = std::chrono::system_clock::now();
    std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime;

    // Use localtime_s for thread safety
    localtime_s(&localTime, &currentTime);

    std::ostringstream oss;
    oss << std::put_time(&localTime, "[%H:%M:%S] ");
    return oss.str();
}

std::wstring TimePrefixW() {
    auto now = std::chrono::system_clock::now();
    std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime;

    // Use localtime_s for thread safety
    localtime_s(&localTime, &currentTime);

    std::wostringstream woss;
    woss << std::put_time(&localTime, L"[%H:%M:%S] ");
    return woss.str();
}

void Logger::log(const std::string& message) {
    LogGuard guard;
    std::ofstream logFile(std::filesystem::path(logFilePath), std::ios::app);
    if (!logFile.is_open()) {
        std::cerr << "Error: Could not open the file for logging." << "\n";
        return;
    }
    auto output = TimePrefix() + message;
    logFile << output << "\n";
    logFile.close();

    debug_cout << output << "\n";
}

void Logger::log(const std::wstring& message) {
    LogGuard guard;
    std::wofstream logFile(std::filesystem::path(logFilePath), std::ios::app);
    if (!logFile.is_open()) {
        std::wcerr << L"Error: Could not open the file for logging." << "\n";
        return;
    }
    auto output = TimePrefixW() + message;
    logFile << output << "\n";
    logFile.close();

    debug_wcout << output << "\n";
}

void Logger::Initialize(const std::wstring& dllPath)
{
    LogGuard guard;
    logFilePath = dllPath + L".log";
    std::wofstream logFile(std::filesystem::path(logFilePath), std::ios::trunc);
    if (!logFile.is_open()) {
        std::wcerr << L"Error: Could not open the file for clearing." << "\n";
        return;
    }
    logFile.close();
}
