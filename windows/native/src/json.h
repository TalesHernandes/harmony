// A small JSON value: enough for the harmony API (PROTOCOL-HARMONY.md), which
// sends objects, arrays, strings, numbers and booleans and nothing exotic.
// libwebrtc's headers mention jsoncpp but the library does not contain it, and
// a whole parser for a dozen message shapes is not worth a fourth dependency.
#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : type_(Type::Bool), bool_(b) {}
  Value(int n) : type_(Type::Number), num_(n) {}
  Value(int64_t n) : type_(Type::Number), num_(double(n)) {}
  Value(double n) : type_(Type::Number), num_(n) {}
  Value(const char* s) : type_(Type::String), str_(s) {}
  Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
  static Value Array() { Value v; v.type_ = Type::Array; return v; }
  static Value Object() { Value v; v.type_ = Type::Object; return v; }

  Type type() const { return type_; }
  bool IsNull() const { return type_ == Type::Null; }
  bool IsObject() const { return type_ == Type::Object; }
  bool IsArray() const { return type_ == Type::Array; }
  bool IsString() const { return type_ == Type::String; }

  // Lenient readers: a missing or mistyped field reads as the fallback, the
  // same rule the Android client follows (unknown things are ignored).
  bool Bool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
  double Number(double fallback = 0) const { return type_ == Type::Number ? num_ : fallback; }
  // Whole numbers that fit; anything else reads as the fallback (a hostile
  // 1e300 converted to an integer is undefined behaviour).
  int64_t Int(int64_t fallback = 0) const {
    return type_ == Type::Number && num_ >= -9.2e18 && num_ <= 9.2e18 && num_ == double(int64_t(num_)) ? int64_t(num_)
                                                                                                  : fallback;
  }
  const std::string& Str() const;

  // Object access. operator[] on a const value returns a shared null for a
  // missing key; on a non-const value it inserts (and turns Null into Object).
  const Value& operator[](const std::string& key) const;
  Value& operator[](const std::string& key);
  bool Has(const std::string& key) const;
  const std::map<std::string, Value>& Items() const { return obj_; }

  // Array access.
  size_t Size() const { return arr_.size(); }
  const Value& operator[](size_t i) const;
  const Value& operator[](int i) const { return (*this)[size_t(i)]; }
  void Push(Value v);
  const std::vector<Value>& Elements() const { return arr_; }

  std::string Dump() const;

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0;
  std::string str_;
  std::vector<Value> arr_;
  std::map<std::string, Value> obj_;
};

// Parses a whole document; nullopt on any syntax error.
std::optional<Value> Parse(const std::string& text);

}  // namespace json
