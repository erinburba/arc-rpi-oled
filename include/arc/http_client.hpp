#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace arc {

// Time from the start of one poll to the start of the next.
inline constexpr std::chrono::seconds kApiPollPeriod{2};

inline constexpr std::size_t kMaxHttpBody = 1024 * 1024;

struct HttpRequest {
  std::string host;
  std::uint16_t port = 80;
  std::string target = "/";
  bool tls = false;
  std::string ca_file;
};

struct HttpResponse {
  int status = 0;
  std::string body;
};

// http://host[:port][/path][?query] or https://. The default ports are 80 and 443.
HttpRequest parse_http_url(std::string_view url);

// One GET. https:// checks the server certificate. ca_file adds a PEM trust anchor.
class HttpClient {
 public:
  explicit HttpClient(HttpRequest request);

  HttpResponse get() const;

 private:
  HttpRequest request_;
};

}  // namespace arc
