// Tony - minimal Hugging Face client: list a repo's files and build download URLs (models are downloaded by Tony's own HTTP stack).
#pragma once
#include <string>
#include <vector>

#include "core/settings.h"

namespace tony {

struct HfFile {
  std::string path;
  long long size = 0;
  bool isDir = false;
};

bool validRepoId(const std::string& repo);                 // "owner/name"
std::vector<HfFile> parseHfTree(const std::string& json);  // portable, tested
bool hfList(const std::string& repo, const std::string& subpath, const NetworkSettings& ns, std::vector<HfFile>* out, std::string* err);
std::string hfFileUrl(const std::string& repo, const std::string& path);

}  // namespace tony
