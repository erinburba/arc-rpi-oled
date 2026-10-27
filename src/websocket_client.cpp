#include "arc/websocket.hpp"

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
#include <array>
#include <cerrno>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace arc {
namespace {

constexpr int kConnectTimeoutMs = 5000;
constexpr int kHandshakeLimit = 8192;

[[noreturn]] void throw_error(int err, const std::string& what) {
  throw std::system_error(err, std::generic_category(), what);
}

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

std::array<std::uint8_t, 16> random_bytes() {
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    const int err = errno;
    throw_error(err, "open /dev/urandom");
  }
  std::array<std::uint8_t, 16> bytes{};
  std::size_t got = 0;
  while (got < bytes.size()) {
    const ssize_t n = ::read(fd, bytes.data() + got, bytes.size() - got);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int err = errno;
      ::close(fd);
      throw_error(err, "read /dev/urandom");
    }
    if (n == 0) {
      ::close(fd);
      throw std::runtime_error("short read from /dev/urandom");
    }
    got += static_cast<std::size_t>(n);
  }
  ::close(fd);
  return bytes;
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
      throw_error(err, "websocket poll");
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
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectTimeoutMs);
      const int ready = wait_for(fd, POLLOUT, deadline);
      if (ready == 0) {
        last_error = ETIMEDOUT;
        ::close(fd);
        continue;
      }
      int so_error = 0;
      socklen_t length = sizeof(so_error);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) < 0) {
        last_error = errno;
        ::close(fd);
        continue;
      }
      if (so_error != 0) {
        last_error = so_error;
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

std::string handshake_request(const WebSocketConfig& config, std::string_view key) {
  std::string host = config.host;
  const std::uint16_t default_port = config.tls ? 443 : 80;
  if (config.port != default_port) {
    host.push_back(':');
    host += std::to_string(config.port);
  }
  std::string request;
  request += "GET ";
  request += config.path;
  request += " HTTP/1.1\r\nHost: ";
  request += host;
  request += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ";
  request += key;
  request += "\r\nSec-WebSocket-Version: 13\r\n\r\n";
  return request;
}

}  // namespace

WebSocketConfig parse_websocket_url(std::string_view url) {
  bool tls = false;
  std::string_view rest;
  if (url.starts_with("wss://")) {
    tls = true;
    rest = url.substr(6);
  } else if (url.starts_with("ws://")) {
    rest = url.substr(5);
  } else {
    throw std::invalid_argument("websocket URL must start with ws:// or wss://");
  }
  const std::size_t slash = rest.find('/');
  const std::string_view authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
  WebSocketConfig config;
  config.path = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
  if (authority.empty() || config.path.find_first_of(" \r\n") != std::string::npos) {
    throw std::invalid_argument("websocket URL is invalid");
  }

  const std::size_t colon = authority.rfind(':');
  std::string_view host = authority;
  std::string_view port_text;
  if (colon != std::string_view::npos) {
    host = authority.substr(0, colon);
    port_text = authority.substr(colon + 1);
  }
  if (host.empty() || host.find_first_of(" \r\n:/") != std::string_view::npos) {
    throw std::invalid_argument("websocket URL host is invalid");
  }
  config.host = std::string(host);
  config.tls = tls;
  if (port_text.empty()) {
    if (colon != std::string_view::npos) {
      throw std::invalid_argument("websocket URL port is invalid");
    }
    config.port = tls ? 443 : 80;
  } else {
    unsigned long port = 0;
    for (const char character : port_text) {
      if (character < '0' || character > '9') {
        throw std::invalid_argument("websocket URL port is invalid");
      }
      port = port * 10 + static_cast<unsigned long>(character - '0');
      if (port > 65535) {
        throw std::invalid_argument("websocket URL port is invalid");
      }
    }
    if (port == 0) {
      throw std::invalid_argument("websocket URL port is invalid");
    }
    config.port = static_cast<std::uint16_t>(port);
  }
  return config;
}

WebSocketClient::WebSocketClient(WebSocketConfig config) {
  if (config.host.empty() || config.path.empty() || config.path.front() != '/') {
    throw std::invalid_argument("websocket target is invalid");
  }
  const int fd = connect_tcp(config.host, config.port);
  try {
    const std::array<std::uint8_t, 16> nonce = random_bytes();
    const std::string key = websocket_nonce_key(nonce);
    const std::string request = handshake_request(config, key);
    fd_ = fd;
    if (config.tls) {
      start_tls(config);
    }
    send_all({reinterpret_cast<const std::uint8_t*>(request.data()), request.size()});

    std::string buffered;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectTimeoutMs);
    while (true) {
      if (const std::optional<std::size_t> header = websocket_handshake_header_size(buffered, key)) {
        if (*header < buffered.size()) {
          const auto* leftover = reinterpret_cast<const std::uint8_t*>(buffered.data() + *header);
          parser_.append({leftover, buffered.size() - *header});
        }
        return;
      }
      if (buffered.size() > kHandshakeLimit) {
        throw std::runtime_error("websocket handshake is too large");
      }
      std::uint8_t chunk[1024];
      const ReadChunk read = read_chunk(chunk, deadline);
      if (read.status == ReadStatus::Interrupted) {
        continue;
      }
      if (read.status == ReadStatus::Timeout) {
        throw std::runtime_error("websocket handshake timed out");
      }
      if (read.status == ReadStatus::Closed) {
        throw std::runtime_error("websocket closed during handshake");
      }
      buffered.append(reinterpret_cast<const char*>(chunk), read.size);
    }
  } catch (...) {
    if (fd_ < 0) {
      ::close(fd);
    } else {
      close_fd();
    }
    throw;
  }
}

WebSocketClient::~WebSocketClient() { close_fd(); }

WebSocketClient::WebSocketClient(WebSocketClient&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      ssl_(std::exchange(other.ssl_, nullptr)),
      ctx_(std::exchange(other.ctx_, nullptr)),
      tls_ready_(other.tls_ready_),
      closed_(other.closed_),
      close_sent_(other.close_sent_),
      parser_(std::move(other.parser_)),
      assembler_(std::move(other.assembler_)) {
  other.tls_ready_ = false;
  other.closed_ = true;
}

WebSocketClient& WebSocketClient::operator=(WebSocketClient&& other) noexcept {
  if (this != &other) {
    close_fd();
    fd_ = std::exchange(other.fd_, -1);
    ssl_ = std::exchange(other.ssl_, nullptr);
    ctx_ = std::exchange(other.ctx_, nullptr);
    tls_ready_ = other.tls_ready_;
    closed_ = other.closed_;
    close_sent_ = other.close_sent_;
    parser_ = std::move(other.parser_);
    assembler_ = std::move(other.assembler_);
    other.tls_ready_ = false;
    other.closed_ = true;
  }
  return *this;
}

bool WebSocketClient::closed() const { return closed_; }

void WebSocketClient::close_fd() noexcept {
  if (ssl_ != nullptr) {
    if (tls_ready_) {
      SSL_shutdown(ssl_);
    }
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  if (ctx_ != nullptr) {
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
  }
  tls_ready_ = false;
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void WebSocketClient::start_tls(const WebSocketConfig& config) {
  ctx_ = SSL_CTX_new(TLS_client_method());
  if (ctx_ == nullptr) {
    throw std::runtime_error(tls_error("TLS setup"));
  }
  SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
  SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
  const int system_certs = SSL_CTX_set_default_verify_paths(ctx_);
  if (system_certs != 1) {
    ERR_clear_error();
  }
  if (!config.ca_file.empty() && SSL_CTX_load_verify_locations(ctx_, config.ca_file.c_str(), nullptr) != 1) {
    throw std::runtime_error("could not load " + config.ca_file);
  }
  if (system_certs != 1 && config.ca_file.empty()) {
    throw std::runtime_error("could not load system certificates");
  }

  ssl_ = SSL_new(ctx_);
  if (ssl_ == nullptr) {
    throw std::runtime_error(tls_error("TLS setup"));
  }
  SSL_set_fd(ssl_, fd_);
  SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  if (is_ip_literal(config.host)) {
    if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl_), config.host.c_str()) != 1) {
      throw std::runtime_error(tls_error("TLS host check"));
    }
  } else {
    if (SSL_set_tlsext_host_name(ssl_, config.host.c_str()) != 1 || SSL_set1_host(ssl_, config.host.c_str()) != 1) {
      throw std::runtime_error(tls_error("TLS host check"));
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectTimeoutMs);
  for (;;) {
    const int rc = SSL_connect(ssl_);
    if (rc == 1) {
      tls_ready_ = true;
      return;
    }
    const int err = SSL_get_error(ssl_, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      const short events = err == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN;
      if (wait_for(fd_, events, deadline) == 0) {
        throw std::runtime_error("websocket TLS handshake timed out");
      }
      continue;
    }
    const long verify = SSL_get_verify_result(ssl_);
    if (verify != X509_V_OK) {
      throw std::runtime_error(std::string("websocket certificate: ") + X509_verify_cert_error_string(verify));
    }
    throw std::runtime_error(tls_error("websocket TLS handshake"));
  }
}

void WebSocketClient::send_all(std::span<const std::uint8_t> bytes) {
  if (fd_ < 0) {
    throw std::logic_error("send on a moved-from websocket");
  }
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectTimeoutMs);
    if (ssl_ != nullptr) {
      const int chunk = static_cast<int>(std::min(bytes.size() - sent, static_cast<std::size_t>(16 * 1024)));
      const int n = SSL_write(ssl_, bytes.data() + sent, chunk);
      if (n > 0) {
        sent += static_cast<std::size_t>(n);
        continue;
      }
      const int err = SSL_get_error(ssl_, n);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        const short events = err == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN;
        if (wait_for(fd_, events, deadline) == 0) {
          throw std::runtime_error("websocket send timed out");
        }
        continue;
      }
      throw std::runtime_error(tls_error("websocket send"));
    }

    if (wait_for(fd_, POLLOUT, deadline) == 0) {
      throw std::runtime_error("websocket send timed out");
    }
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    const ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, flags);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      const int err = errno;
      throw_error(err, "websocket send");
    }
    if (n == 0) {
      throw std::runtime_error("websocket send wrote nothing");
    }
    sent += static_cast<std::size_t>(n);
  }
}

WebSocketClient::ReadChunk WebSocketClient::read_chunk(std::span<std::uint8_t> buffer,
                                                       std::chrono::steady_clock::time_point deadline) {
  if (buffer.empty()) {
    throw std::invalid_argument("websocket read buffer is empty");
  }
  for (;;) {
    if (ssl_ != nullptr) {
      const int n = SSL_read(ssl_, buffer.data(), static_cast<int>(buffer.size()));
      if (n > 0) {
        return {ReadStatus::Bytes, static_cast<std::size_t>(n)};
      }
      const int err = SSL_get_error(ssl_, n);
      if (err == SSL_ERROR_ZERO_RETURN) {
        return {ReadStatus::Closed, 0};
      }
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE ||
          (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
        const short events = err == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          return {ReadStatus::Timeout, 0};
        }
        const auto slice = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd ready{fd_, events, 0};
        const int result = ::poll(&ready, 1, static_cast<int>(slice.count()));
        if (result < 0) {
          if (errno == EINTR) {
            return {ReadStatus::Interrupted, 0};
          }
          const int poll_error = errno;
          throw_error(poll_error, "websocket receive");
        }
        if (result == 0) {
          return {ReadStatus::Timeout, 0};
        }
        continue;
      }
      if (err == SSL_ERROR_SYSCALL && n == 0) {
        return {ReadStatus::Closed, 0};
      }
      throw std::runtime_error(tls_error("websocket receive"));
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return {ReadStatus::Timeout, 0};
    }
    const auto slice = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd ready{fd_, POLLIN, 0};
    const int result = ::poll(&ready, 1, static_cast<int>(slice.count()));
    if (result < 0) {
      if (errno == EINTR) {
        return {ReadStatus::Interrupted, 0};
      }
      const int err = errno;
      throw_error(err, "websocket receive");
    }
    if (result == 0) {
      return {ReadStatus::Timeout, 0};
    }
    const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (n < 0) {
      if (errno == EINTR) {
        return {ReadStatus::Interrupted, 0};
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      const int err = errno;
      throw_error(err, "websocket receive");
    }
    if (n == 0) {
      return {ReadStatus::Closed, 0};
    }
    return {ReadStatus::Bytes, static_cast<std::size_t>(n)};
  }
}

void WebSocketClient::send_frame(const WebSocketFrame& frame) {
  const std::array<std::uint8_t, 16> nonce = random_bytes();
  const std::array<std::uint8_t, 4> mask{nonce[0], nonce[1], nonce[2], nonce[3]};
  const std::vector<std::uint8_t> encoded = encode_client_frame(frame, mask);
  send_all(encoded);
}

std::optional<WebSocketMessage> WebSocketClient::drain() {
  while (const std::optional<WebSocketFrame> frame = parser_.next_frame()) {
    WebSocketAssembler::Result result = assembler_.push(std::move(*frame));
    if (result.kind == WebSocketAssembler::Kind::Pending) {
      continue;
    }
    if (result.kind == WebSocketAssembler::Kind::Message) {
      return std::move(result.message);
    }
    if (result.kind == WebSocketAssembler::Kind::Pong) {
      continue;
    }
    if (result.kind == WebSocketAssembler::Kind::Ping) {
      WebSocketFrame pong;
      pong.opcode = WebSocketOpcode::Pong;
      pong.payload = std::move(result.payload);
      send_frame(pong);
      continue;
    }
    if (!close_sent_) {
      WebSocketFrame reply;
      reply.opcode = WebSocketOpcode::Close;
      if (result.payload.size() >= 2) {
        reply.payload.assign(result.payload.begin(), result.payload.begin() + 2);
      } else {
        reply.payload = {0x03, 0xE8};
      }
      try {
        send_frame(reply);
      } catch (const std::exception&) {
        // The peer often drops the TCP connection along with its close frame.
      }
      close_sent_ = true;
    }
    closed_ = true;
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<WebSocketMessage> WebSocketClient::receive(std::chrono::milliseconds timeout) {
  if (fd_ < 0) {
    throw std::logic_error("receive on a moved-from websocket");
  }
  if (timeout.count() < 0 || timeout.count() > 2'147'483'647) {
    throw std::invalid_argument("websocket receive timeout is out of range");
  }
  if (closed_) {
    return std::nullopt;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (const std::optional<WebSocketMessage> message = drain()) {
      return message;
    }
    if (closed_) {
      return std::nullopt;
    }
    std::uint8_t chunk[4096];
    const ReadChunk read = read_chunk(chunk, deadline);
    if (read.status == ReadStatus::Interrupted || read.status == ReadStatus::Timeout) {
      return std::nullopt;
    }
    if (read.status == ReadStatus::Closed) {
      closed_ = true;
      return std::nullopt;
    }
    parser_.append({chunk, read.size});
  }
}

}  // namespace arc
