// Tony - heuristic classification of PowerShell commands before they run.
// This is a speed bump with good defaults, NOT a sandbox: PowerShell can always do more than a regex can see.
// The real protections are (1) explicit user approval, (2) taint tracking, (3) audit log.
#pragma once
#include <string>
#include <vector>

namespace tony {

enum class Risk { ReadOnly, Normal, Dangerous };

struct RiskReport {
  Risk risk = Risk::Normal;
  std::vector<std::string> reasons;
};

RiskReport classifyPowerShell(const std::string& command);
const char* riskName(Risk r);

}  // namespace tony
