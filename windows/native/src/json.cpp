#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace json {
namespace {

const Value& Null() {
  static const Value v;
  return v;
}
const std::string& Empty() {
  static const std::string s;
  return s;
}

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

void DumpString(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  out += '"';
}

void DumpValue(std::string& out, const Value& v) {
  switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.Bool() ? "true" : "false"; break;
    case Value::Type::Number: {
      double n = v.Number();
      char buf[32];
      if (std::floor(n) == n && std::fabs(n) < 9e15)
        std::snprintf(buf, sizeof buf, "%lld", (long long)n);
      else
        std::snprintf(buf, sizeof buf, "%.17g", n);
      out += buf;
      break;
    }
    case Value::Type::String: DumpString(out, v.Str()); break;
    case Value::Type::Array: {
      out += '[';
      bool first = true;
      for (const auto& e : v.Elements()) {
        if (!first) out += ',';
        first = false;
        DumpValue(out, e);
      }
      out += ']';
      break;
    }
    case Value::Type::Object: {
      out += '{';
      bool first = true;
      for (const auto& [k, e] : v.Items()) {
        if (!first) out += ',';
        first = false;
        DumpString(out, k);
        out += ':';
        DumpValue(out, e);
      }
      out += '}';
      break;
    }
  }
}

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}

  bool Document(Value& out) {
    Ws();
    if (!Any(out, 0)) return false;
    Ws();
    return i_ == s_.size();
  }

 private:
  bool Any(Value& out, int depth) {
    if (depth > 64 || i_ >= s_.size()) return false;
    char c = s_[i_];
    if (c == '{') return Obj(out, depth);
    if (c == '[') return Arr(out, depth);
    if (c == '"') {
      std::string str;
      if (!Str(str)) return false;
      out = Value(std::move(str));
      return true;
    }
    if (Lit("true")) { out = Value(true); return true; }
    if (Lit("false")) { out = Value(false); return true; }
    if (Lit("null")) { out = Value(); return true; }
    return Num(out);
  }
  bool Obj(Value& out, int depth) {
    out = Value::Object();
    ++i_;
    Ws();
    if (Peek('}')) { ++i_; return true; }
    for (;;) {
      Ws();
      std::string key;
      if (!Peek('"') || !Str(key)) return false;
      Ws();
      if (!Peek(':')) return false;
      ++i_;
      Ws();
      Value v;
      if (!Any(v, depth + 1)) return false;
      out[key] = std::move(v);
      Ws();
      if (Peek(',')) { ++i_; continue; }
      if (Peek('}')) { ++i_; return true; }
      return false;
    }
  }
  bool Arr(Value& out, int depth) {
    out = Value::Array();
    ++i_;
    Ws();
    if (Peek(']')) { ++i_; return true; }
    for (;;) {
      Ws();
      Value v;
      if (!Any(v, depth + 1)) return false;
      out.Push(std::move(v));
      Ws();
      if (Peek(',')) { ++i_; continue; }
      if (Peek(']')) { ++i_; return true; }
      return false;
    }
  }
  bool Hex4(uint32_t& cp) {
    if (i_ + 4 > s_.size()) return false;
    cp = 0;
    for (int k = 0; k < 4; ++k) {
      char h = s_[i_++];
      cp <<= 4;
      if (h >= '0' && h <= '9') cp |= h - '0';
      else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
      else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
      else return false;
    }
    return true;
  }
  bool Str(std::string& out) {
    ++i_;  // opening quote
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return true;
      // A raw control character is not JSON (RFC 8259 §7).
      if (static_cast<unsigned char>(c) < 0x20) return false;
      if (c != '\\') { out += c; continue; }
      if (i_ >= s_.size()) return false;
      char e = s_[i_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!Hex4(cp)) return false;
          // Surrogates only as a high-low pair; a lone half is not a
          // character and would become invalid UTF-8.
          if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (!(i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u')) return false;
            i_ += 2;
            uint32_t lo;
            if (!Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          AppendUtf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool Num(Value& out) {
    // JSON's grammar first (-?int frac? exp?): strtod alone also takes hex,
    // "inf" and "nan".
    size_t j = i_;
    auto digits = [&] {
      size_t k = j;
      while (j < s_.size() && s_[j] >= '0' && s_[j] <= '9') ++j;
      return j > k;
    };
    if (j < s_.size() && s_[j] == '-') ++j;
    if (j < s_.size() && s_[j] == '0') ++j;
    else if (!digits()) return false;
    if (j < s_.size() && s_[j] == '.') { ++j; if (!digits()) return false; }
    if (j < s_.size() && (s_[j] == 'e' || s_[j] == 'E')) {
      ++j;
      if (j < s_.size() && (s_[j] == '+' || s_[j] == '-')) ++j;
      if (!digits()) return false;
    }
    std::string text = s_.substr(i_, j - i_);
    double n = std::strtod(text.c_str(), nullptr);
    if (!std::isfinite(n)) return false;  // 1e999
    i_ = j;
    out = Value(n);
    return true;
  }
  bool Lit(const char* word) {
    size_t n = std::char_traits<char>::length(word);
    if (s_.compare(i_, n, word) != 0) return false;
    i_ += n;
    return true;
  }
  bool Peek(char c) const { return i_ < s_.size() && s_[i_] == c; }
  void Ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
  }

  const std::string& s_;
  size_t i_ = 0;
};

}  // namespace

const std::string& Value::Str() const { return type_ == Type::String ? str_ : Empty(); }

const Value& Value::operator[](const std::string& key) const {
  if (type_ != Type::Object) return Null();
  auto it = obj_.find(key);
  return it == obj_.end() ? Null() : it->second;
}

Value& Value::operator[](const std::string& key) {
  if (type_ == Type::Null) type_ = Type::Object;
  return obj_[key];
}

bool Value::Has(const std::string& key) const { return type_ == Type::Object && obj_.count(key) > 0; }

const Value& Value::operator[](size_t i) const {
  if (type_ != Type::Array || i >= arr_.size()) return Null();
  return arr_[i];
}

void Value::Push(Value v) {
  if (type_ == Type::Null) type_ = Type::Array;
  arr_.push_back(std::move(v));
}

std::string Value::Dump() const {
  std::string out;
  DumpValue(out, *this);
  return out;
}

std::optional<Value> Parse(const std::string& text) {
  Value v;
  Parser p(text);
  if (!p.Document(v)) return std::nullopt;
  return v;
}

}  // namespace json
