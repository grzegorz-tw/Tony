// Tony - conversation persistence and export (portable, tested).
#pragma once
#include <string>
#include <vector>

#include "types.h"

namespace tony {

struct Chat {
  std::string id, title;
  long long updated = 0;  // unix seconds
  std::vector<Message> msgs;
};

// Images attached to user messages are not stored (they would bloat the file); generated images are referenced by path.
json chatToJson(const Chat& c);
bool chatFromJson(const json& j, Chat* out);
std::string chatToMarkdown(const Chat& c);
std::string autoTitle(const std::string& firstUserText);

}  // namespace tony
