#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace app {
inline constexpr std::string_view TwemojiBase = "/static/assets/twemoji-15/";
struct EmojiPiece {
  std::string text;
  bool picture = false;
  size_t byteOffset = 0, byteLength = 0;
  size_t utf16Offset = 0, utf16Length = 0;
  // Name only, without the base URL or .svg; empty for plain text.
  std::string file;
};
// Owning, ordered runs. Even empty input returns one plain text run, like web.
// Malformed UTF-8 bytes stay in text, counting as one replacement UTF-16 unit.
std::vector<EmojiPiece> SplitEmoji(std::string_view utf8);
std::string EmojiFile(std::u32string_view emoji);
std::string EmojiFile(std::string_view utf8);
struct EmojiEntry { std::string emoji, words; };
struct EmojiGroup { std::string name, icon; std::vector<EmojiEntry> emoji; };
const std::vector<EmojiGroup>& EmojiGroups();
std::string EmojiPlain(std::string_view query);
std::vector<std::string> EmojiSearch(std::string_view query);

// Reactions.tsx keeps these per device, not per account. The screen owns
// persistence (web uses localStorage["harmony.emoji"]) and the clock icon.
inline constexpr size_t EmojiRecentLimit = 12;
inline constexpr std::string_view EmojiRecentKey = "harmony.emoji";
inline constexpr std::string_view EmojiRecentLabel = "usados aqui";
inline constexpr std::string_view EmojiRecentIcon = "clock";
std::vector<std::string> EmojiRecent(const std::vector<std::string>& stored);
std::vector<std::string> RememberEmoji(std::string emoji, const std::vector<std::string>& stored);
}
