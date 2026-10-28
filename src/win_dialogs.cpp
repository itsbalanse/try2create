#include "win_dialogs.h"
#include <shobjidl.h>
#include <shellapi.h>

namespace {

std::wstring ShellItemPath(IShellItem* item) {
    std::wstring result;
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        result = path;
        CoTaskMemFree(path);
    }
    return result;
}

} // namespace

std::wstring OpenImageFileDialog(HWND owner) {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        COMDLG_FILTERSPEC filters[] = {
            {L"Images and videos", L"*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.mp4;*.webm;*.mkv;*.mov;*.m4v;*.avi"},
            {L"All Files", L"*.*"},
        };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                result = ShellItemPath(item);
                item->Release();
            }
        }
        dialog->Release();
    }
    return result;
}

std::wstring OpenFolderDialog(HWND owner) {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                result = ShellItemPath(item);
                item->Release();
            }
        }
        dialog->Release();
    }
    return result;
}

std::wstring SaveCsvJsonFileDialog(HWND owner, bool json) {
    std::wstring result;
    IFileSaveDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        COMDLG_FILTERSPEC filters[] = {
            json ? COMDLG_FILTERSPEC{L"JSON File", L"*.json"} : COMDLG_FILTERSPEC{L"CSV File", L"*.csv"},
            {L"All Files", L"*.*"},
        };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetDefaultExtension(json ? L"json" : L"csv");
        dialog->SetFileName(json ? L"metadata_export.json" : L"metadata_export.csv");
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                result = ShellItemPath(item);
                item->Release();
            }
        }
        dialog->Release();
    }
    return result;
}

std::wstring SavePngFileDialog(HWND owner, const std::wstring& suggestedName) {
    std::wstring result;
    IFileSaveDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        COMDLG_FILTERSPEC filters[] = {{L"PNG picture", L"*.png"}};
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetDefaultExtension(L"png");
        if (!suggestedName.empty()) dialog->SetFileName(suggestedName.c_str());
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                result = ShellItemPath(item);
                item->Release();
            }
        }
        dialog->Release();
    }
    return result;
}

bool DeleteFilesToRecycleBin(HWND owner, const std::vector<std::wstring>& paths, std::wstring* errorOut) {
    if (paths.empty()) return true;

    // SHFileOperationW wants pFrom as a list of paths packed back-to-back,
    // each NUL-terminated, with one extra trailing NUL to mark the end.
    std::wstring packed;
    for (const auto& p : paths) {
        packed += p;
        packed.push_back(L'\0');
    }
    packed.push_back(L'\0');

    SHFILEOPSTRUCTW op = {};
    op.hwnd = owner;
    op.wFunc = FO_DELETE;
    op.pFrom = packed.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;

    int result = SHFileOperationW(&op);
    if (result != 0) {
        if (errorOut) *errorOut = L"Shell delete operation failed (code " + std::to_wstring(result) + L")";
        return false;
    }
    if (op.fAnyOperationsAborted) {
        if (errorOut) *errorOut = L"Delete was aborted";
        return false;
    }
    return true;
}

std::wstring OpenCsvJsonFileDialog(HWND owner) {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        COMDLG_FILTERSPEC filters[] = {
            {L"CSV or JSON Files", L"*.csv;*.json"},
            {L"All Files", L"*.*"},
        };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                result = ShellItemPath(item);
                item->Release();
            }
        }
        dialog->Release();
    }
    return result;
}

std::wstring OpenWorkflowFileDialog(HWND owner) {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        COMDLG_FILTERSPEC filters[] = {
            {L"Workflow, image or video", L"*.json;*.png;*.webp;*.jpg;*.jpeg;*.mp4;*.webm;*.mkv;*.mov"},
            {L"All Files", L"*.*"},
        };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                result = ShellItemPath(item);
                item->Release();
            }
        }
        dialog->Release();
    }
    return result;
}
