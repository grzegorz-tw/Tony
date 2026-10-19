// Tony - turns Settings into the files/arguments llama-server needs (router mode with a models preset).
// Portable and unit-tested; the process handling lives in llama_service.cpp.
#pragma once
#include <string>
#include <vector>

#include "core/settings.h"

namespace tony {

// Router model id: letters, digits and . _ : - only.
std::string presetId(const std::string& name);

// models.ini for `llama-server --models-preset`. One section per configured model plus a [*] section.
std::string buildPresetIni(const Settings& s);

// Command line (without the exe) for the router.
std::vector<std::string> buildServerArgs(const Settings& s, const std::string& iniPath, int port, const std::string& apiKey);

}  // namespace tony
