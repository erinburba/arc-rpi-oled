#include "arc/json.hpp"

#include <stdexcept>

namespace arc {
namespace {

constexpr int kMaxDepth = 64;

void append_utf8(std::string& out, char32_t codepoint) {
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

}  // namespace

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  Json parse() {
    skip();
    Json value = parse_value();
    skip();
    if (index_ != text_.size()) {
      throw std::runtime_error("invalid JSON");
    }
    return value;
  }

 private:
  std::string_view text_;
  std::size_t index_ = 0;
  int depth_ = 0;

  [[noreturn]] static void fail() { throw std::runtime_error("invalid JSON"); }

  void skip() {
    while (index_ < text_.size()) {
      const char character = text_[index_];
      if (character != ' ' && character != '\t' && character != '\n' && character != '\r') {
        break;
      }
      ++index_;
    }
  }

  char peek() {
    if (index_ >= text_.size()) {
      fail();
    }
    return text_[index_];
  }

  char get() {
    const char character = peek();
    ++index_;
    return character;
  }

  bool consume(char expected) {
    if (index_ < text_.size() && text_[index_] == expected) {
      ++index_;
      return true;
    }
    return false;
  }

  Json parse_value() {
    if (++depth_ > kMaxDepth) {
      fail();
    }
    skip();
    const char character = peek();
    Json value;
    if (character == '{') {
      value = parse_object();
    } else if (character == '[') {
      value = parse_array();
    } else if (character == '"') {
      value.type_ = Json::Type::String;
      value.text_ = parse_string();
    } else if (character == 't') {
      literal("true");
      value.type_ = Json::Type::Bool;
      value.bool_ = true;
    } else if (character == 'f') {
      literal("false");
      value.type_ = Json::Type::Bool;
    } else if (character == 'n') {
      literal("null");
    } else if (character == '-' || (character >= '0' && character <= '9')) {
      value.type_ = Json::Type::Number;
      value.text_ = parse_number();
    } else {
      fail();
    }
    --depth_;
    return value;
  }

  void literal(std::string_view expected) {
    for (const char character : expected) {
      if (get() != character) {
        fail();
      }
    }
  }

  std::string parse_string() {
    if (get() != '"') {
      fail();
    }
    std::string out;
    while (true) {
      const unsigned char character = static_cast<unsigned char>(get());
      if (character == '"') {
        return out;
      }
      if (character == '\\') {
        const char escaped = get();
        switch (escaped) {
          case '"':
          case '\\':
          case '/':
            out.push_back(escaped);
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u':
            append_utf8(out, read_codepoint());
            break;
          default:
            fail();
        }
        continue;
      }
      if (character < 0x20) {
        fail();
      }
      out.push_back(static_cast<char>(character));
    }
  }

  char32_t read_codepoint() {
    const char32_t codepoint = read_hex4();
    if (codepoint < 0xD800 || codepoint > 0xDFFF) {
      return codepoint;
    }
    if (codepoint > 0xDBFF || get() != '\\' || get() != 'u') {
      fail();
    }
    const char32_t low = read_hex4();
    if (low < 0xDC00 || low > 0xDFFF) {
      fail();
    }
    return 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
  }

  char32_t read_hex4() {
    char32_t value = 0;
    for (int nibble = 0; nibble < 4; ++nibble) {
      const char character = get();
      value <<= 4;
      if (character >= '0' && character <= '9') {
        value += static_cast<char32_t>(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        value += static_cast<char32_t>(character - 'a' + 10);
      } else if (character >= 'A' && character <= 'F') {
        value += static_cast<char32_t>(character - 'A' + 10);
      } else {
        fail();
      }
    }
    return value;
  }

  std::string parse_number() {
    const std::size_t start = index_;
    consume('-');
    if (peek() == '0') {
      ++index_;
    } else if (peek() >= '1' && peek() <= '9') {
      while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
        ++index_;
      }
    } else {
      fail();
    }
    if (consume('.')) {
      if (index_ >= text_.size() || text_[index_] < '0' || text_[index_] > '9') {
        fail();
      }
      while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
        ++index_;
      }
    }
    if (index_ < text_.size() && (text_[index_] == 'e' || text_[index_] == 'E')) {
      ++index_;
      if (index_ < text_.size() && (text_[index_] == '+' || text_[index_] == '-')) {
        ++index_;
      }
      if (index_ >= text_.size() || text_[index_] < '0' || text_[index_] > '9') {
        fail();
      }
      while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
        ++index_;
      }
    }
    return std::string(text_.substr(start, index_ - start));
  }

  Json parse_array() {
    if (get() != '[') {
      fail();
    }
    Json value;
    value.type_ = Json::Type::Array;
    skip();
    if (consume(']')) {
      return value;
    }
    while (true) {
      value.array_.push_back(parse_value());
      skip();
      if (consume(']')) {
        return value;
      }
      if (!consume(',')) {
        fail();
      }
    }
  }

  Json parse_object() {
    if (get() != '{') {
      fail();
    }
    Json value;
    value.type_ = Json::Type::Object;
    skip();
    if (consume('}')) {
      return value;
    }
    while (true) {
      skip();
      std::string key = parse_string();
      skip();
      if (!consume(':')) {
        fail();
      }
      Json field = parse_value();
      bool replaced = false;
      for (auto& entry : value.object_) {
        if (entry.first == key) {
          entry.second = std::move(field);
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        value.object_.emplace_back(std::move(key), std::move(field));
      }
      skip();
      if (consume('}')) {
        return value;
      }
      if (!consume(',')) {
        fail();
      }
    }
  }
};

namespace {

void append_string(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const unsigned char character : text) {
    switch (character) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (character < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[character >> 4]);
          out.push_back(kHex[character & 0x0F]);
        } else {
          out.push_back(static_cast<char>(character));
        }
        break;
    }
  }
  out.push_back('"');
}

}  // namespace

Json Json::parse(std::string_view text) { return JsonParser(text).parse(); }

bool Json::is_null() const { return type_ == Type::Null; }
bool Json::is_bool() const { return type_ == Type::Bool; }
bool Json::is_number() const { return type_ == Type::Number; }
bool Json::is_string() const { return type_ == Type::String; }
bool Json::is_array() const { return type_ == Type::Array; }
bool Json::is_object() const { return type_ == Type::Object; }

bool Json::boolean() const {
  if (type_ != Type::Bool) {
    throw std::logic_error("JSON value is not a boolean");
  }
  return bool_;
}

std::string_view Json::number() const {
  if (type_ != Type::Number) {
    throw std::logic_error("JSON value is not a number");
  }
  return text_;
}

std::string_view Json::string() const {
  if (type_ != Type::String) {
    throw std::logic_error("JSON value is not a string");
  }
  return text_;
}

std::size_t Json::size() const {
  if (type_ == Type::Array) {
    return array_.size();
  }
  if (type_ == Type::Object) {
    return object_.size();
  }
  throw std::logic_error("JSON value has no size");
}

const Json& Json::at(std::size_t index) const {
  if (type_ != Type::Array || index >= array_.size()) {
    throw std::out_of_range("JSON array index");
  }
  return array_[index];
}

const Json* Json::find(std::string_view key) const {
  if (type_ != Type::Object) {
    throw std::logic_error("JSON value is not an object");
  }
  for (const auto& entry : object_) {
    if (entry.first == key) {
      return &entry.second;
    }
  }
  return nullptr;
}

std::string Json::dump() const {
  std::string out;
  switch (type_) {
    case Type::Null:
      out = "null";
      break;
    case Type::Bool:
      out = bool_ ? "true" : "false";
      break;
    case Type::Number:
      out = text_;
      break;
    case Type::String:
      append_string(out, text_);
      break;
    case Type::Array:
      out.push_back('[');
      for (std::size_t index = 0; index < array_.size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        out += array_[index].dump();
      }
      out.push_back(']');
      break;
    case Type::Object:
      out.push_back('{');
      for (std::size_t index = 0; index < object_.size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        append_string(out, object_[index].first);
        out.push_back(':');
        out += object_[index].second.dump();
      }
      out.push_back('}');
      break;
  }
  return out;
}

}  // namespace arc
