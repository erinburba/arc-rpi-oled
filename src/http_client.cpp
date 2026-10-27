#include "arc/http_client.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace arc {
namespace {

constexpr int kTimeoutMs = 5000;

[[noreturn]] void throw_error(int err, const std::string& what) {
  throw std::system_error(err, std::generic_category(), what);
}

[[noreturn]] void fail(const char* what) { throw std::runtime_error(what); }

std::string tls_error(const char* what) {
  const unsigned long code = ERR_get_error();
  if (code == 0) {
    return what;
  }
  char text[256];
  ERR_error_string_n(code, text, sizeof(text));
  return std::string(what) + ": " + text;
}

bool is_ip_literal(const std::string& host) {
  in_addr ipv4{};
  if (::inet_pton(AF_INET, host.c_str(), &ipv4) == 1) {
    return true;
  }
  in6_addr ipv6{};
  return ::inet_pton(AF_INET6, host.c_str(), &ipv6) == 1;
}

char lower_ascii(char value) {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value - 'A' + 'a');
  }
  return value;
}

std::string lower_copy(std::string_view text) {
  std::string out(text);
  for (char& character : out) {
    character = lower_ascii(character);
  }
  return out;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

struct AddrInfoFree {
  void operator()(addrinfo* info) const {
    if (info != nullptr) {
      ::freeaddrinfo(info);
    }
  }
};

void set_cloexec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD);
  if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
    const int err = errno;
    throw_error(err, "set close-on-exec");
  }
}

void set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    const int err = errno;
    throw_error(err, "set nonblocking");
  }
}

int wait_for(int fd, short events, std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return 0;
    }
    const auto slice = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd ready{fd, events, 0};
    const int result = ::poll(&ready, 1, static_cast<int>(slice.count()));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int err = errno;
      throw_error(err, "HTTP poll");
    }
    return result;
  }
}

int connect_tcp(const std::string& host, std::uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* raw = nullptr;
  const std::string service = std::to_string(port);
  const int lookup = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &raw);
  if (lookup != 0) {
    throw std::runtime_error("resolve " + host + ": " + ::gai_strerror(lookup));
  }
  std::unique_ptr<addrinfo, AddrInfoFree> addresses(raw);

  int last_error = ECONNREFUSED;
  for (addrinfo* info = addresses.get(); info != nullptr; info = info->ai_next) {
    const int fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (fd < 0) {
      last_error = errno;
      continue;
    }
    try {
      set_cloexec(fd);
      set_nonblocking(fd);
#ifdef SO_NOSIGPIPE
      const int enabled = 1;
      if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
        const int err = errno;
        throw_error(err, "set SO_NOSIGPIPE");
      }
#endif
      const int connected = ::connect(fd, info->ai_addr, info->ai_addrlen);
      if (connected == 0) {
        return fd;
      }
      if (errno != EINPROGRESS) {
        last_error = errno;
        ::close(fd);
        continue;
      }
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
      if (wait_for(fd, POLLOUT, deadline) == 0) {
        last_error = ETIMEDOUT;
        ::close(fd);
        continue;
      }
      int so_error = 0;
      socklen_t length = sizeof(so_error);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) < 0 || so_error != 0) {
        last_error = so_error != 0 ? so_error : errno;
        ::close(fd);
        continue;
      }
      return fd;
    } catch (...) {
      ::close(fd);
      throw;
    }
  }
  throw_error(last_error, "connect " + host);
}

struct Connection {
  int fd = -1;
  SSL_CTX* ctx = nullptr;
  SSL* ssl = nullptr;
  bool tls_ready = false;

  ~Connection() {
    if (ssl != nullptr) {
      if (tls_ready) {
        SSL_shutdown(ssl);
      }
      SSL_free(ssl);
    }
    if (ctx != nullptr) {
      SSL_CTX_free(ctx);
    }
    if (fd >= 0) {
      ::close(fd);
    }
  }

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection() = default;
};

void start_tls(Connection& connection, const HttpRequest& request) {
  connection.ctx = SSL_CTX_new(TLS_client_method());
  if (connection.ctx == nullptr) {
    throw std::runtime_error(tls_error("TLS setup"));
  }
  SSL_CTX_set_min_proto_version(connection.ctx, TLS1_2_VERSION);
  SSL_CTX_set_verify(connection.ctx, SSL_VERIFY_PEER, nullptr);
  const int system_certs = SSL_CTX_set_default_verify_paths(connection.ctx);
  if (system_certs != 1) {
    ERR_clear_error();
  }
  if (!request.ca_file.empty() &&
      SSL_CTX_load_verify_locations(connection.ctx, request.ca_file.c_str(), nullptr) != 1) {
    throw std::runtime_error("could not load " + request.ca_file);
  }
  if (system_certs != 1 && request.ca_file.empty()) {
    throw std::runtime_error("could not load system certificates");
  }
  connection.ssl = SSL_new(connection.ctx);
  if (connection.ssl == nullptr) {
    throw std::runtime_error(tls_error("TLS setup"));
  }
  SSL_set_fd(connection.ssl, connection.fd);
  SSL_set_mode(connection.ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  if (is_ip_literal(request.host)) {
    if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(connection.ssl), request.host.c_str()) != 1) {
      throw std::runtime_error(tls_error("TLS host check"));
    }
  } else if (SSL_set_tlsext_host_name(connection.ssl, request.host.c_str()) != 1 ||
             SSL_set1_host(connection.ssl, request.host.c_str()) != 1) {
    throw std::runtime_error(tls_error("TLS host check"));
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
  for (;;) {
    const int rc = SSL_connect(connection.ssl);
    if (rc == 1) {
      connection.tls_ready = true;
      return;
    }
    const int err = SSL_get_error(connection.ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      const short events = err == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN;
      if (wait_for(connection.fd, events, deadline) == 0) {
        throw std::runtime_error("HTTP TLS handshake timed out");
      }
      continue;
    }
    const long verify = SSL_get_verify_result(connection.ssl);
    if (verify != X509_V_OK) {
      throw std::runtime_error(std::string("HTTP certificate: ") + X509_verify_cert_error_string(verify));
    }
    throw std::runtime_error(tls_error("HTTP TLS handshake"));
  }
}

void send_all(Connection& connection, std::string_view bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
    if (connection.ssl != nullptr) {
      const int chunk = static_cast<int>(std::min(bytes.size() - sent, static_cast<std::size_t>(16 * 1024)));
      const int n = SSL_write(connection.ssl, bytes.data() + sent, chunk);
      if (n > 0) {
        sent += static_cast<std::size_t>(n);
        continue;
      }
      const int err = SSL_get_error(connection.ssl, n);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        const short events = err == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN;
        if (wait_for(connection.fd, events, deadline) == 0) {
          throw std::runtime_error("HTTP send timed out");
        }
        continue;
      }
      throw std::runtime_error(tls_error("HTTP send"));
    }
    if (wait_for(connection.fd, POLLOUT, deadline) == 0) {
      throw std::runtime_error("HTTP send timed out");
    }
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    const ssize_t n = ::send(connection.fd, bytes.data() + sent, bytes.size() - sent, flags);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      const int err = errno;
      throw_error(err, "HTTP send");
    }
    if (n == 0) {
      fail("HTTP send wrote nothing");
    }
    sent += static_cast<std::size_t>(n);
  }
}

// Positive byte count, 0 at EOF, or -1 when the deadline passes.
int read_some(Connection& connection, char* buffer, int capacity, std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    if (connection.ssl != nullptr) {
      const int n = SSL_read(connection.ssl, buffer, capacity);
      if (n > 0) {
        return n;
      }
      const int err = SSL_get_error(connection.ssl, n);
      if (err == SSL_ERROR_ZERO_RETURN || (err == SSL_ERROR_SYSCALL && n == 0)) {
        return 0;
      }
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE ||
          (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
        if (std::chrono::steady_clock::now() >= deadline ||
            wait_for(connection.fd, err == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN, deadline) == 0) {
          return -1;
        }
        continue;
      }
      throw std::runtime_error(tls_error("HTTP receive"));
    }
    if (std::chrono::steady_clock::now() >= deadline || wait_for(connection.fd, POLLIN, deadline) == 0) {
      return -1;
    }
    const ssize_t n = ::recv(connection.fd, buffer, static_cast<std::size_t>(capacity), 0);
    if (n > 0) {
      return static_cast<int>(n);
    }
    if (n == 0) {
      return 0;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      continue;
    }
    const int err = errno;
    throw_error(err, "HTTP receive");
  }
}

enum class BodyKind { UntilClose, Length, Chunked };

struct Prefix {
  int status = 0;
  BodyKind kind = BodyKind::UntilClose;
  std::size_t content_length = 0;
  std::size_t header_bytes = 0;
};

std::optional<Prefix> response_prefix(std::string_view message) {
  const std::size_t end = message.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view block = message.substr(0, end);
  const std::size_t status_end = block.find("\r\n");
  const std::string_view status_line = status_end == std::string_view::npos ? block : block.substr(0, status_end);
  if (status_line.size() < 12 || status_line.compare(0, 5, "HTTP/") != 0) {
    fail("HTTP status is invalid");
  }
  const std::size_t code_at = status_line.find(' ');
  if (code_at == std::string_view::npos || code_at + 3 >= status_line.size()) {
    fail("HTTP status is invalid");
  }
  int status = 0;
  for (int digit = 0; digit < 3; ++digit) {
    const char character = status_line[code_at + 1 + static_cast<std::size_t>(digit)];
    if (character < '0' || character > '9') {
      fail("HTTP status is invalid");
    }
    status = status * 10 + (character - '0');
  }
  if (code_at + 4 < status_line.size() && status_line[code_at + 4] != ' ') {
    fail("HTTP status is invalid");
  }

  Prefix prefix;
  prefix.status = status;
  prefix.header_bytes = end + 4;
  bool saw_length = false;
  bool chunked = false;
  std::size_t line_at = status_end == std::string_view::npos ? block.size() : status_end + 2;
  while (line_at < block.size()) {
    const std::size_t next = block.find("\r\n", line_at);
    const std::string_view line =
        block.substr(line_at, next == std::string_view::npos ? block.size() - line_at : next - line_at);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      fail("HTTP header is invalid");
    }
    const std::string name = lower_copy(trim(line.substr(0, colon)));
    const std::string_view value = trim(line.substr(colon + 1));
    if (name == "content-length") {
      if (value.empty()) {
        fail("HTTP content length is invalid");
      }
      std::size_t length = 0;
      for (const char character : value) {
        if (character < '0' || character > '9') {
          fail("HTTP content length is invalid");
        }
        if (length > (std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(character - '0')) / 10) {
          fail("HTTP body is too large");
        }
        length = length * 10 + static_cast<std::size_t>(character - '0');
      }
      if (length > kMaxHttpBody || (saw_length && length != prefix.content_length)) {
        fail(length > kMaxHttpBody ? "HTTP body is too large" : "HTTP content length is invalid");
      }
      saw_length = true;
      prefix.content_length = length;
    } else if (name == "transfer-encoding") {
      std::size_t start = 0;
      while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::size_t length = comma == std::string_view::npos ? value.size() - start : comma - start;
        const std::string token = lower_copy(trim(value.substr(start, length)));
        if (token == "chunked") {
          chunked = true;
        } else if (!token.empty() && token != "identity") {
          fail("HTTP transfer encoding is not supported");
        }
        if (comma == std::string_view::npos) {
          break;
        }
        start = comma + 1;
      }
    }
    if (next == std::string_view::npos) {
      break;
    }
    line_at = next + 2;
  }
  if (chunked) {
    prefix.kind = BodyKind::Chunked;
  } else if (saw_length) {
    prefix.kind = BodyKind::Length;
  }
  return prefix;
}

std::optional<std::string> decode_chunks(std::string_view input) {
  std::string body;
  std::size_t index = 0;
  while (true) {
    const std::size_t line_end = input.find("\r\n", index);
    if (line_end == std::string_view::npos) {
      return std::nullopt;
    }
    std::string_view size_text = input.substr(index, line_end - index);
    const std::size_t semicolon = size_text.find(';');
    if (semicolon != std::string_view::npos) {
      size_text = size_text.substr(0, semicolon);
    }
    size_text = trim(size_text);
    if (size_text.empty() || size_text.size() > 6) {
      fail(size_text.empty() ? "HTTP chunk size is invalid" : "HTTP body is too large");
    }
    std::size_t size = 0;
    for (const char character : size_text) {
      size <<= 4;
      if (character >= '0' && character <= '9') {
        size += static_cast<std::size_t>(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        size += static_cast<std::size_t>(character - 'a' + 10);
      } else if (character >= 'A' && character <= 'F') {
        size += static_cast<std::size_t>(character - 'A' + 10);
      } else {
        fail("HTTP chunk size is invalid");
      }
      if (size > kMaxHttpBody) {
        fail("HTTP body is too large");
      }
    }
    index = line_end + 2;
    if (size == 0) {
      while (true) {
        const std::size_t trailer = input.find("\r\n", index);
        if (trailer == std::string_view::npos) {
          return std::nullopt;
        }
        if (trailer == index) {
          return body;
        }
        index = trailer + 2;
      }
    }
    if (input.size() < index + size + 2) {
      return std::nullopt;
    }
    if (input[index + size] != '\r' || input[index + size + 1] != '\n') {
      fail("HTTP chunk is invalid");
    }
    if (body.size() + size > kMaxHttpBody) {
      fail("HTTP body is too large");
    }
    body.append(input.substr(index, size));
    index += size + 2;
  }
}

std::string request_text(const HttpRequest& request) {
  std::string host = request.host;
  const std::uint16_t default_port = request.tls ? 443 : 80;
  if (request.port != default_port) {
    host.push_back(':');
    host += std::to_string(request.port);
  }
  std::string text;
  text += "GET ";
  text += request.target;
  text += " HTTP/1.1\r\nHost: ";
  text += host;
  text += "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
  return text;
}

}  // namespace

HttpRequest parse_http_url(std::string_view url) {
  bool tls = false;
  std::string_view rest;
  if (url.starts_with("https://")) {
    tls = true;
    rest = url.substr(8);
  } else if (url.starts_with("http://")) {
    rest = url.substr(7);
  } else {
    throw std::invalid_argument("API URL must start with http:// or https://");
  }
  if (rest.empty() || rest.find('@') != std::string_view::npos) {
    throw std::invalid_argument("API URL is invalid");
  }
  const std::size_t slash = rest.find('/');
  const std::size_t question = rest.find('?');
  const std::size_t authority_end = std::min(slash, question);
  const std::string_view authority = rest.substr(0, authority_end);
  HttpRequest request;
  request.tls = tls;
  if (slash == std::string_view::npos && question == std::string_view::npos) {
    request.target = "/";
  } else if (question != std::string_view::npos && (slash == std::string_view::npos || question < slash)) {
    request.target = "/";
    request.target.append(rest.substr(question));
  } else {
    request.target = std::string(rest.substr(slash));
  }
  if (authority.empty() || request.target.find_first_of(" \r\n") != std::string::npos) {
    throw std::invalid_argument("API URL is invalid");
  }

  const std::size_t colon = authority.rfind(':');
  std::string_view host = authority;
  std::string_view port_text;
  if (colon != std::string_view::npos) {
    host = authority.substr(0, colon);
    port_text = authority.substr(colon + 1);
  }
  if (host.empty() || host.find_first_of(" \r\n:/") != std::string_view::npos) {
    throw std::invalid_argument("API URL host is invalid");
  }
  request.host = std::string(host);
  if (port_text.empty()) {
    if (colon != std::string_view::npos) {
      throw std::invalid_argument("API URL port is invalid");
    }
    request.port = tls ? 443 : 80;
  } else {
    unsigned long port = 0;
    for (const char character : port_text) {
      if (character < '0' || character > '9') {
        throw std::invalid_argument("API URL port is invalid");
      }
      port = port * 10 + static_cast<unsigned long>(character - '0');
      if (port > 65535) {
        throw std::invalid_argument("API URL port is invalid");
      }
    }
    if (port == 0) {
      throw std::invalid_argument("API URL port is invalid");
    }
    request.port = static_cast<std::uint16_t>(port);
  }
  return request;
}

HttpClient::HttpClient(HttpRequest request) : request_(std::move(request)) {
  if (request_.host.empty() || request_.target.empty() || request_.target.front() != '/') {
    throw std::invalid_argument("API target is invalid");
  }
}

HttpResponse HttpClient::get() const {
  Connection connection;
  connection.fd = connect_tcp(request_.host, request_.port);
  if (request_.tls) {
    start_tls(connection, request_);
  }
  const std::string request = request_text(request_);
  send_all(connection, request);

  std::string buffered;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
  while (true) {
    if (const std::optional<Prefix> prefix = response_prefix(buffered)) {
      if (prefix->kind == BodyKind::Chunked) {
        if (const std::optional<std::string> body = decode_chunks(buffered.substr(prefix->header_bytes))) {
          return HttpResponse{prefix->status, *body};
        }
      } else if (prefix->kind == BodyKind::Length) {
        if (buffered.size() >= prefix->header_bytes + prefix->content_length) {
          return HttpResponse{prefix->status, buffered.substr(prefix->header_bytes, prefix->content_length)};
        }
      }
    }
    if (buffered.size() > kMaxHttpBody + 65536) {
      fail("HTTP response is too large");
    }
    char chunk[4096];
    const int n = read_some(connection, chunk, static_cast<int>(sizeof(chunk)), deadline);
    if (n < 0) {
      fail("HTTP response timed out");
    }
    if (n == 0) {
      const std::optional<Prefix> prefix = response_prefix(buffered);
      if (!prefix.has_value()) {
        fail("HTTP response ended during the headers");
      }
      if (prefix->kind == BodyKind::Chunked) {
        const std::optional<std::string> body = decode_chunks(buffered.substr(prefix->header_bytes));
        if (!body.has_value()) {
          fail("HTTP response ended during the body");
        }
        return HttpResponse{prefix->status, *body};
      }
      if (prefix->kind == BodyKind::Length) {
        if (buffered.size() < prefix->header_bytes + prefix->content_length) {
          fail("HTTP response ended during the body");
        }
        return HttpResponse{prefix->status, buffered.substr(prefix->header_bytes, prefix->content_length)};
      }
      return HttpResponse{prefix->status, buffered.substr(prefix->header_bytes)};
    }
    buffered.append(chunk, static_cast<std::size_t>(n));
  }
}

}  // namespace arc
