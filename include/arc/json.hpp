#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace arc {

// A JSON value. Numbers keep the digits from the text so a poll can compare them exactly.
class JsonParser;

class Json {
 public:
  static Json parse(std::string_view text);

  bool is_null() const;
  bool is_bool() const;
  bool is_number() const;
  bool is_string() const;
  bool is_array() const;
  bool is_object() const;

  bool boolean() const;
  std::string_view number() const;
  std::string_view string() const;
  std::size_t size() const;
  const Json& at(std::size_t index) const;
  // The first value stored for key, or nullptr.
  const Json* find(std::string_view key) const;
  std::string dump() const;

 private:
  friend class JsonParser;

  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type_ = Type::Null;
  bool bool_ = false;
  std::string text_;
  std::vector<Json> array_;
  std::vector<std::pair<std::string, Json>> object_;
};

}  // namespace arc
