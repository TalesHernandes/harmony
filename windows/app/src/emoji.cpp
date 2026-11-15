#include "emoji.h"
#include <algorithm>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace app {
namespace {
struct Range { char32_t first, last; };
struct Mapping { char32_t point; std::u32string_view value; };
struct Combining { char32_t point; unsigned value; };
#include "emoji-data.inc"

template<size_t N> bool Has(char32_t p, const Range (&ranges)[N]) {
  auto it = std::upper_bound(std::begin(ranges), std::end(ranges), p,
      [](char32_t c, const Range& r) { return c < r.first; });
  return it != std::begin(ranges) && p <= std::prev(it)->last;
}
template<class T, size_t N> const T* Find(char32_t p, const T (&table)[N]) {
  auto it = std::lower_bound(std::begin(table), std::end(table), p,
      [](const T& row, char32_t c) { return row.point < c; });
  return it != std::end(table) && it->point == p ? it : nullptr;
}
struct Point { char32_t value; size_t byte, utf16; };
std::vector<Point> Decode(std::string_view s) {
  std::vector<Point> out;
  size_t u = 0;
  for (size_t i = 0; i < s.size();) {
    const auto first = static_cast<unsigned char>(s[i]);
    size_t n = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
        first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    char32_t p = n == 1 ? first : n ? first & ((1 << (7 - n)) - 1) : 0xfffd;
    bool valid = n && i + n <= s.size();
    for (size_t j = 1; valid && j < n; ++j) {
      auto b = static_cast<unsigned char>(s[i + j]);
      if ((b & 0xc0) != 0x80) valid = false;
      else p = (p << 6) | (b & 0x3f);
    }
    if (!valid || (n == 2 && p < 0x80) || (n == 3 && p < 0x800) ||
        (n == 4 && p < 0x10000) || p > 0x10ffff || (p >= 0xd800 && p <= 0xdfff)) {
      p = 0xfffd; n = 1;
    }
    out.push_back({p, i, u});
    i += n; u += p > 0xffff ? 2 : 1;
  }
  out.push_back({0, s.size(), u}); // Sentinel for run end offsets.
  return out;
}
void Encode(std::string& s, char32_t p) {
  if (p < 0x80) s += static_cast<char>(p);
  else if (p < 0x800) {
    s += static_cast<char>(0xc0 | (p >> 6)); s += static_cast<char>(0x80 | (p & 63));
  } else if (p < 0x10000) {
    s += static_cast<char>(0xe0 | (p >> 12)); s += static_cast<char>(0x80 | ((p >> 6) & 63));
    s += static_cast<char>(0x80 | (p & 63));
  } else {
    s += static_cast<char>(0xf0 | (p >> 18)); s += static_cast<char>(0x80 | ((p >> 12) & 63));
    s += static_cast<char>(0x80 | ((p >> 6) & 63)); s += static_cast<char>(0x80 | (p & 63));
  }
}
// Exact PART: EP (FE0F | Emoji_Modifier)? [E0020-E007F]*.
size_t Part(const std::vector<Point>& p, size_t i) {
  const auto n = p.size() - 1;
  if (i >= n || !Has(p[i].value, kExtendedPictographic)) return i;
  ++i;
  if (i < n && (p[i].value == 0xfe0f || Has(p[i].value, kEmojiModifier))) ++i;
  while (i < n && p[i].value >= 0xe0020 && p[i].value <= 0xe007f) ++i;
  return i;
}
bool Space(char32_t c) {
  // ECMAScript \s and trim, including BOM but excluding NEL and U+180E.
  return (c >= 9 && c <= 13) || c == 0x20 || c == 0xa0 || c == 0x1680 ||
      (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
      c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}
unsigned Class(char32_t c) {
  const auto row = Find(c, kCombining);
  return row ? row->value : 0;
}
bool FinalSigma(const std::u32string& s, size_t i) {
  bool before = false;
  for (size_t j = i; j > 0;) {
    auto c = s[--j];
    if (Has(c, kCaseIgnorable)) continue;
    before = Has(c, kCased); break;
  }
  if (!before) return false;
  for (size_t j = i + 1; j < s.size(); ++j) {
    if (Has(s[j], kCaseIgnorable)) continue;
    return !Has(s[j], kCased);
  }
  return true;
}
}

std::string EmojiFile(std::u32string_view emoji) {
  bool joined = emoji.find(U'\u200d') != emoji.npos;
  std::string out;
  for (auto p : emoji) {
    if (!joined && p == 0xfe0f) continue;
    if (!out.empty()) out += '-';
    char digits[8]; size_t n = 0;
    do { digits[n++] = "0123456789abcdef"[p & 15]; p >>= 4; } while (p);
    while (n) out += digits[--n];
  }
  return out;
}
std::string EmojiFile(std::string_view utf8) {
  auto p = Decode(utf8);
  std::u32string s;
  for (size_t i = 0; i + 1 < p.size(); ++i) s += p[i].value;
  return EmojiFile(s);
}
std::vector<EmojiPiece> SplitEmoji(std::string_view utf8) {
  const auto p = Decode(utf8);
  const auto n = p.size() - 1;
  std::vector<EmojiPiece> out;
  auto append = [&](size_t a, size_t b, bool picture) {
    EmojiPiece piece{std::string(utf8.substr(p[a].byte, p[b].byte - p[a].byte)), picture,
        p[a].byte, p[b].byte - p[a].byte, p[a].utf16, p[b].utf16 - p[a].utf16, {}};
    if (picture) piece.file = EmojiFile(piece.text);
    out.push_back(std::move(piece));
  };
  size_t last = 0;
  for (size_t i = 0; i < n;) {
    size_t end = i;
    if (Has(p[i].value, kRegionalIndicator) && i + 1 < n && Has(p[i + 1].value, kRegionalIndicator)) {
      end = i + 2;
    } else if (p[i].value == '#' || p[i].value == '*' || (p[i].value >= '0' && p[i].value <= '9')) {
      size_t j = i + 1;
      if (j < n && p[j].value == 0xfe0f) ++j;
      if (j < n && p[j].value == 0x20e3) end = j + 1;
    }
    if (end == i) {
      end = Part(p, i);
      while (end > i && end < n && p[end].value == 0x200d) {
        size_t next = Part(p, end + 1);
        if (next == end + 1) break;
        end = next;
      }
    }
    if (end == i) { ++i; continue; }
    // isPicture rejects only a *single* EP lacking Emoji_Presentation.
    bool picture = !(end == i + 1 && Has(p[i].value, kExtendedPictographic) && !Has(p[i].value, kEmojiPresentation));
    if (picture) {
      if (i > last) append(last, i, false);
      append(i, end, true); last = end;
    }
    i = end;
  }
  if (last == 0 || last < n) append(last, n, false);
  return out;
}
const std::vector<EmojiGroup>& EmojiGroups() { return kEmojiGroups; }

std::string EmojiPlain(std::string_view query) {
  auto points = Decode(query);
  std::u32string nfd;
  for (size_t i = 0; i + 1 < points.size(); ++i) {
    auto c = points[i].value;
    if (c >= 0xac00 && c <= 0xd7a3) {
      auto s = c - 0xac00;
      nfd += 0x1100 + s / 588; nfd += 0x1161 + (s % 588) / 28;
      if (s % 28) nfd += 0x11a7 + s % 28;
    } else if (auto row = Find(c, kNfd)) nfd += row->value;
    else nfd += c;
  }
  // Stable canonical ordering, stopping at the preceding starter.
  for (size_t i = 1; i < nfd.size(); ++i) {
    auto cls = Class(nfd[i]);
    for (size_t j = i; cls && j && Class(nfd[j - 1]) > cls; --j)
      std::swap(nfd[j], nfd[j - 1]);
  }
  std::erase_if(nfd, [](char32_t c) { return c >= 0x300 && c <= 0x36f; });
  std::u32string lower;
  for (size_t i = 0; i < nfd.size(); ++i) {
    auto c = nfd[i];
    if (c == 0x3a3 && FinalSigma(nfd, i)) lower += 0x3c2;
    else if (auto row = Find(c, kLower)) lower += row->value;
    else lower += c;
  }
  size_t a = 0, b = lower.size();
  while (a < b && Space(lower[a])) ++a;
  while (b > a && Space(lower[b - 1])) --b;
  std::string out;
  for (; a < b; ++a) Encode(out, lower[a]);
  return out;
}
std::vector<std::string> EmojiSearch(std::string_view query) {
  const auto want = EmojiPlain(query);
  if (want.empty()) return {};
  // Precompute once; group names participate in prefix matching too.
  static const auto index = [] {
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    for (const auto& group : EmojiGroups()) for (const auto& entry : group.emoji) {
      auto text = EmojiPlain(group.name + " " + entry.words);
      auto points = Decode(text);
      std::vector<std::string> words;
      size_t a = 0;
      for (size_t i = 0; i + 1 < points.size(); ++i) if (Space(points[i].value)) {
        words.push_back(text.substr(a, points[i].byte - a)); a = points[i + 1].byte;
      }
      words.push_back(text.substr(a));
      out.emplace_back(entry.emoji, std::move(words));
    }
    return out;
  }();
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  for (const auto& [emoji, words] : index) {
    if (seen.contains(emoji)) continue;
    if (!std::any_of(words.begin(), words.end(), [&](const auto& word) { return word.starts_with(want); })) continue;
    seen.insert(emoji); out.push_back(emoji);
  }
  return out;
}
std::vector<std::string> EmojiRecent(const std::vector<std::string>& stored) {
  return {stored.begin(), stored.begin() + std::min(stored.size(), EmojiRecentLimit)};
}
std::vector<std::string> RememberEmoji(std::string emoji, const std::vector<std::string>& stored) {
  std::vector<std::string> out{emoji};
  for (const auto& old : EmojiRecent(stored)) {
    if (old != emoji && out.size() < EmojiRecentLimit) out.push_back(old);
  }
  return out;
}
}
