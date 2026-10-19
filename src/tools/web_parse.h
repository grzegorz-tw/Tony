// Tony - parsing of search-provider responses (portable, unit-tested).
#pragma once
#include <string>
#include <vector>

namespace tony {

struct SearchHit { std::string title, url, snippet; };

std::vector<SearchHit> parseDuckDuckGoHtml(const std::string& html);
std::vector<SearchHit> parseSearxngJson(const std::string& json);
std::vector<SearchHit> parseBraveJson(const std::string& json);
bool looksLikeBotChallenge(const std::string& html);

std::string formatHits(const std::vector<SearchHit>& hits, size_t maxHits);

}  // namespace tony
