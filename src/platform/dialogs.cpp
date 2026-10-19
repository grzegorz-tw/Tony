#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "dialogs.h"

#include <windows.h>
#include <shobjidl.h>

#include "winutil.h"

namespace tony {

static std::string runDialog(void* owner, const std::string& title, bool folder, bool save, const std::string& defName,
                             const std::vector<std::pair<std::string, std::string>>& filters) {
  std::string result;
  IFileDialog* dlg = nullptr;
  HRESULT hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, save ? IID_IFileSaveDialog : IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg));
  if (FAILED(hr) || !dlg) return result;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? static_cast<DWORD>(FOS_PICKFOLDERS) : 0u));
  dlg->SetTitle(widen(title).c_str());
  if (!defName.empty()) dlg->SetFileName(widen(defName).c_str());
  std::vector<std::wstring> names, specs;
  std::vector<COMDLG_FILTERSPEC> fs;
  if (!folder) {
    for (auto& f : filters) { names.push_back(widen(f.first)); specs.push_back(widen(f.second)); }
    names.push_back(L"All files");
    specs.push_back(L"*.*");
    for (size_t i = 0; i < names.size(); ++i) fs.push_back({names[i].c_str(), specs[i].c_str()});
    dlg->SetFileTypes(static_cast<UINT>(fs.size()), fs.data());
  }
  if (SUCCEEDED(dlg->Show(static_cast<HWND>(owner)))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item)) && item) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { result = narrow(p); CoTaskMemFree(p); }
      item->Release();
    }
  }
  dlg->Release();
  return result;
}

std::string pickOpenFile(void* owner, const std::string& title, const std::vector<std::pair<std::string, std::string>>& filters) { return runDialog(owner, title, false, false, "", filters); }
std::string pickFolder(void* owner, const std::string& title) { return runDialog(owner, title, true, false, "", {}); }
std::string pickSaveFile(void* owner, const std::string& title, const std::string& name) { return runDialog(owner, title, false, true, name, {}); }

}  // namespace tony
