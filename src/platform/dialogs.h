// Tony - native Windows file/folder pickers.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace tony {

// filters: {"Models (*.gguf)", "*.gguf"}, ...  Returns "" when cancelled.
std::string pickOpenFile(void* ownerHwnd, const std::string& title, const std::vector<std::pair<std::string, std::string>>& filters);
std::string pickFolder(void* ownerHwnd, const std::string& title);
std::string pickSaveFile(void* ownerHwnd, const std::string& title, const std::string& defaultName);

}  // namespace tony
