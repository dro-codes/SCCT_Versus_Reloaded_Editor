#include "pch.h"
#include "CompareBackup.h"
#include "CompareBackupModel.h"
#include "MapAuthoringDialog.h"
#include "SaveBackupModel.h"
#include "SaveBackups.h"
#include "WorkflowEditor.h"
#include <commdlg.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <iterator>

namespace fs = std::filesystem;

namespace
{
    std::wstring Wide(const std::string& text)
    {
        if (text.empty()) return {};
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring result(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), length);
        return result;
    }

    // The open map's backup folder when it has one (an open backup's is the folder of its map), else the top.
    fs::path StartFolder()
    {
        const auto folder = SaveBackups::Folder();
        std::error_code error;
        try
        {
            const auto map = Workflow::Editor::MapFile();
            fs::path stem = fs::path(map).stem();
            if (SaveBackups::IsBackupPath(map))
                if (const auto backup = SaveBackups::ParseBackup(map)) stem = fs::path(backup->first);
            if (!stem.empty() && fs::is_directory(folder / stem, error)) return folder / stem;
        }
        catch (const std::exception&) {}
        return fs::is_directory(folder, error) ? folder : fs::path();
    }

    bool Choose(HWND frame, fs::path& chosen)
    {
        wchar_t path[32768] = {};
        const auto start = StartFolder();
        const auto initial = start.wstring();
        OPENFILENAMEW choose{ sizeof(choose) };
        choose.hwndOwner = frame;
        choose.lpstrFile = path;
        choose.nMaxFile = 32768;
        choose.lpstrFilter = L"Maps (*.sdc)\0*.sdc\0";
        choose.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
        choose.lpstrTitle = L"Choose a Backup to Compare";
        // The engine's relative paths depend on the current directory staying System.
        choose.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
        if (!GetOpenFileNameW(&choose))
        {
            if (CommDlgExtendedError()) throw std::runtime_error("File dialog failed.");
            return false;
        }
        chosen = path;
        // Only maps: the copy is loaded as a package, and a script or asset package loaded under another name is
        // nothing to compare.
        if (_wcsicmp(chosen.extension().c_str(), L".sdc") != 0) throw std::runtime_error("Choose a map (.sdc) to compare with.");
        return true;
    }

    std::wstring Heading(const Workflow::Json& comparison, const fs::path& file)
    {
        std::wstring text = L"Backup: " + file.wstring() + L"\r\n";
        try
        {
            auto map = Workflow::Editor::MapFile();
            text += L"Open map: " + (map.empty() ? std::wstring(L"Unsaved map") : fs::path(map).wstring()) + L"\r\n";
        }
        catch (const std::exception&) {}
        text += L"Differences: " + Wide(comparison.at("tally").get<std::string>()) + L"\r\n";
        text += L"\"In backup\" is the backup's value, \"In open map\" the map's now. Tick rows and Bring Back Selected puts the backup's version back "
                L"(a deleted actor is added again, an added one deleted, a property set back) in one Undo step; save the map to keep it. "
                L"Brushes are compared by their properties only: their shapes are not, and a deleted brush cannot be brought back. "
                L"Map settings (the LevelInfo) are not compared.";
        return text;
    }
}

void CompareBackup::Open(HWND frame)
{
    fs::path file;
    if (!Choose(frame, file)) return;
    Workflow::Json comparison;
    {
        const HCURSOR previous = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        try { comparison = Workflow::Editor::CompareWithMapFile(file); }
        catch (...) { SetCursor(previous); throw; }
        SetCursor(previous);
    }
    const auto& rows = comparison.at("rows");
    if (rows.empty())
    {
        MessageBoxW(frame, (L"The open map's actors match " + file.filename().wstring() + L". (Brush shapes and map settings are not compared.)").c_str(),
                    L"Compare with Backup", MB_OK | MB_ICONINFORMATION);
        return;
    }
    MapAuthoringDialog::Review review;
    review.title = L"Compare with Backup";
    review.heading = Heading(comparison, file);
    review.hint = L"Click a row to select its actor in the map. Tick the rows to bring back.";
    review.apply = L"&Bring Back Selected";
    review.cancel = L"Close";
    review.columns[3] = L"In backup";
    review.columns[4] = L"In open map";
    review.checkboxes = true;
    const int widths[] = { 80, 170, 230, 220, 220 };
    std::copy(std::begin(widths), std::end(widths), review.widths);
    review.placement = "CompareWithBackup";
    for (const auto& row : rows)
    {
        MapAuthoringDialog::ReviewRow line;
        line.cells[0] = MapAuthoringDialog::Cell(row.at("change").get<std::string>());
        line.cells[1] = MapAuthoringDialog::Cell(row.at("target").get<std::string>());
        const auto note = row.value("note", std::string{});
        line.cells[2] = MapAuthoringDialog::Cell(row.at("property").get<std::string>() + (note.empty() ? std::string{} : " (" + note + ")"));
        line.cells[3] = MapAuthoringDialog::Cell(row.at("before").get<std::string>());
        line.cells[4] = MapAuthoringDialog::Cell(row.at("after").get<std::string>());
        line.select = row.at("select");
        line.checkable = !row.at("action").get<std::string>().empty();
        review.rows.push_back(std::move(line));
    }
    std::vector<size_t> ticked;
    if (!MapAuthoringDialog::Show(frame, review, &ticked)) return;
    const auto result = Workflow::Editor::BringBackFromComparison(comparison, ticked);
    const auto& document = result.at("document");
    size_t creates = 0, deletes = 0, updates = 0;
    for (const auto& op : document.at("operations"))
    {
        const auto kind = op.at("op").get<std::string>();
        if (kind == "create") ++creates;
        else if (kind == "delete") ++deletes;
        else if (kind == "update") ++updates;
    }
    std::wstring text = L"Brought back from " + file.filename().wstring() + L": ";
    text += std::to_wstring(creates) + L" actor(s) added again, " + std::to_wstring(deletes) + L" deleted, " + std::to_wstring(updates) + L" set back.\r\n"
            L"Undo reverses it in one step. Save the map to keep it.";
    MessageBoxW(frame, text.c_str(), L"Compare with Backup", MB_OK | MB_ICONINFORMATION);
}
