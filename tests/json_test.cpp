#include "arc/json.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

int g_failures = 0;

void expect(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    std::cerr << file << ":" << line << ": " << expression << "\n";
    ++g_failures;
  }
}

#define EXPECT(condition) expect(static_cast<bool>(condition), #condition, __FILE__, __LINE__)

void test_compact() {
  const arc::Json json = arc::Json::parse(R"({"ok":true,"n":2})");
  EXPECT(json.find("ok")->boolean());
  EXPECT(json.find("n")->number() == "2");
}

void test_object() {
  const arc::Json json = arc::Json::parse(R"({"ok": true, "n": 2, "text": "hi", "tags": [null, false]})");
  EXPECT(json.is_object());
  EXPECT(json.find("ok")->boolean());
  EXPECT(json.find("n")->number() == "2");
  EXPECT(json.find("text")->string() == "hi");
  EXPECT(json.find("missing") == nullptr);
  EXPECT(json.find("tags")->at(0).is_null());
  EXPECT(!json.find("tags")->at(1).boolean());
  EXPECT(json.dump() == R"({"ok":true,"n":2,"text":"hi","tags":[null,false]})");
}

void test_escapes() {
  const arc::Json json = arc::Json::parse(R"("a\"b\n\u00e9")");
  EXPECT(json.string() == "a\"b\n\xC3\xA9");
  EXPECT(json.dump() == "\"a\\\"b\\n\xC3\xA9\"");
}

void test_rejects_trailing_junk() {
  bool threw = false;
  try {
    arc::Json::parse("true false");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  EXPECT(threw);
}

void test_duplicate_key_keeps_the_last_value() {
  const arc::Json json = arc::Json::parse(R"({"n":1,"n":2})");
  EXPECT(json.size() == 1);
  EXPECT(json.find("n")->number() == "2");
}

}  // namespace

int main() {
  test_compact();
  test_object();
  test_escapes();
  test_rejects_trailing_junk();
  test_duplicate_key_keeps_the_last_value();
  if (g_failures != 0) {
    std::cerr << g_failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "ok\n";
  return EXIT_SUCCESS;
}
