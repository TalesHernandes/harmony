#include "../src/emoji.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <unordered_set>
#include <utility>

namespace {
using app::EmojiGroup;
struct Range { char32_t first, last; };
struct Mapping { char32_t point; std::u32string_view value; };
struct Combining { char32_t point; unsigned value; };
#define HARMONY_EMOJI_TEST_ORACLES
#include "../src/emoji-data.inc"
int failures = 0;
void Check(bool ok, const char* name) {
  if (!ok) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
}
std::vector<std::string> Pictures(std::string_view text) {
  std::vector<std::string> out;
  for (const auto& p : app::SplitEmoji(text)) if (p.picture) out.push_back(p.text);
  return out;
}
}
int main() {
  using namespace app;
  Check(Pictures("figuras 🔥 ❤️ 👍🏽 ©") == std::vector<std::string>{"🔥", "❤️", "👍🏽"}, "web rodada4 pictures");
  auto sample = SplitEmoji("figuras 🔥 ❤️ 👍🏽 ©");
  Check(sample.back().text == " ©" && !sample.back().picture, "copyright stays in text");
  Check(Pictures("© ® ™ ‼") .empty(), "text default symbols");
  Check(Pictures("©️ ®️ ™️ ‼️") == std::vector<std::string>{"©️", "®️", "™️", "‼️"}, "FE0F opts into pictures");
  Check(Pictures("🇧🇷 1️⃣ 👨‍👩‍👧‍👦") == std::vector<std::string>{"🇧🇷", "1️⃣", "👨‍👩‍👧‍👦"}, "flags keycaps and family");
  const std::string england = "\U0001f3f4\U000e0067\U000e0062\U000e0065\U000e006e\U000e0067\U000e007f";
  Check(Pictures(england) == std::vector<std::string>{england}, "England tag flag is one picture");
  Check(EmojiFile(england) == "1f3f4-e0067-e0062-e0065-e006e-e0067-e007f", "England filename");
  Check(EmojiFile("❤️‍🔥") == "2764-fe0f-200d-1f525", "joined FE0F retained");
  Check(EmojiFile("❤️") == "2764", "unjoined FE0F removed");
  Check(EmojiFile(U"🇧🇷") == "1f1e7-1f1f7" && EmojiFile("1️⃣") == "31-20e3", "flag and keycap filename");
  Check(EmojiFile(std::u32string_view(U"\0", 1)) == "0", "hexadecimal zero");
  Check(TwemojiBase == "/static/assets/twemoji-15/", "server asset base");
  Check(Pictures("🇧🇷🇺 1⃣ #️⃣ *⃣") == std::vector<std::string>{"🇧🇷", "1⃣", "#️⃣", "*⃣"}, "SOURCE alternatives and lone regional");
  Check(Pictures("👍️🏽") == std::vector<std::string>{"👍️"}, "PART consumes selector OR modifier");
  Check(Pictures("🔥‍x🔥") == std::vector<std::string>{"🔥", "🔥"}, "incomplete ZWJ stays text");
  Check(Pictures("©🏽") == std::vector<std::string>{"©🏽"}, "isPicture checks single point only");
  Check(Pictures("‼‍🔥") == std::vector<std::string>{"‼‍🔥"}, "nonpresenting joined PART");

  auto offsets = SplitEmoji("𐐀a🔥❤️👍🏽z");
  Check(offsets.size() == 5, "offset run count");
  if (offsets.size() == 5) {
    const size_t bytes[] = {0, 5, 9, 15, 23}, lengths[] = {5, 4, 6, 8, 1};
    const size_t units[] = {0, 3, 5, 7, 11}, unitLengths[] = {3, 2, 2, 4, 1};
    for (size_t i = 0; i < 5; ++i) {
      Check(offsets[i].byteOffset == bytes[i] && offsets[i].byteLength == lengths[i], "UTF8 byte offsets");
      Check(offsets[i].utf16Offset == units[i] && offsets[i].utf16Length == unitLengths[i], "UTF16 astral offsets");
    }
  }
  for (const auto& text : std::vector<std::string>{"", "plain © end", "🔥", "🔥🔥", "a❤️‍🔥b", england, std::string("x\xff🔥")}) {
    std::string joined;
    size_t byte = 0, unit = 0;
    auto pieces = SplitEmoji(text);
    Check(!pieces.empty(), "web returns a text piece for empty input");
    for (const auto& p : pieces) {
      Check(p.byteOffset == byte && p.utf16Offset == unit, "contiguous offsets");
      Check(p.byteLength == p.text.size(), "byte length matches stored text");
      Check(p.file == (p.picture ? EmojiFile(p.text) : ""), "only pictures have filenames");
      joined += p.text; byte += p.byteLength; unit += p.utf16Length;
    }
    Check(joined == text, "lossless segmentation");
  }
  for (const auto& [query, expected] : kSearchOracle)
    Check(EmojiSearch(query) == expected, ("web search oracle: " + query).c_str());
  for (const auto& [query, expected] : kPlainOracle)
    Check(EmojiPlain(query) == expected, ("web plain oracle: " + query).c_str());
  Check(EmojiSearch("ogo").empty(), "word prefix only, no substring");
  Check(EmojiSearch("fogo top").empty(), "whole query is a single prefix");
  Check(EmojiPlain("cora\u0063\u0327a\u0303o") == "coracao", "decomposed accents");
  size_t entries = 0;
  std::unordered_set<std::string> unique;
  Check(EmojiGroups().size() == 10 && EmojiGroups()[0].name == "Mais usados", "web groups and order");
  for (const auto& group : EmojiGroups()) {
    Check(group.icon == group.emoji.front().emoji, "group icon is its first emoji");
    for (const auto& entry : group.emoji) {
      ++entries; unique.insert(entry.emoji);
      auto pieces = SplitEmoji(entry.emoji);
      Check(pieces.size() == 1 && pieces.front().picture, "every panel entry is one picture");
    }
  }
  Check(entries == 1879 && unique.size() == 1867, "panel duplicates preserved, unique entries");
  Check(EmojiSearch("fogo").front() == "🔥" && EmojiSearch("coracao").front() == "❤️", "first web results");
  std::vector<std::string> recent;
  for (int i = 0; i < 15; ++i) recent = RememberEmoji(std::to_string(i), recent);
  Check(recent.size() == 12 && recent.front() == "14" && recent.back() == "3", "recent cap and newest first");
  recent = RememberEmoji("8", recent);
  Check(recent.front() == "8" && std::count(recent.begin(), recent.end(), "8") == 1 && recent.size() == 12, "recent move to front");
  Check(RememberEmoji("🔥", {"x", "x"}) == std::vector<std::string>{"🔥", "x", "x"}, "recent preserves unrelated duplicates like web");
  Check(EmojiRecentKey == "harmony.emoji" && EmojiRecentLabel == "usados aqui" && EmojiRecentIcon == "clock", "recent web metadata");
  if (failures) return EXIT_FAILURE;
  std::cout << "emoji tests passed; " << entries << " entries, " << unique.size() << " unique\n";
  return EXIT_SUCCESS;
}
