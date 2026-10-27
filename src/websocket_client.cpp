#include "arc/websocket.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

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
  if (config.port != 80) {
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
  if (url.starts_with("wss://")) {
    throw std::invalid_argument("wss:// is not supported");
  }
  constexpr std::string_view kPrefix = "ws://";
  if (!url.starts_with(kPrefix)) {
    throw std::invalid_argument("websocket URL must start with ws://");
  }
  const std::string_view rest = url.substr(kPrefix.size());
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
  if (port_text.empty()) {
    if (colon != std::string_view::npos) {
      throw std::invalid_argument("websocket URL port is invalid");
    }
    config.port = 80;
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
      if (wait_for(fd_, POLLIN, deadline) == 0) {
        throw std::runtime_error("websocket handshake timed out");
      }
      char chunk[1024];
      const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        const int err = errno;
        throw_error(err, "websocket handshake");
      }
      if (n == 0) {
        throw std::runtime_error("websocket closed during handshake");
      }
      buffered.append(chunk, static_cast<std::size_t>(n));
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
      closed_(other.closed_),
      close_sent_(other.close_sent_),
      parser_(std::move(other.parser_)),
      assembler_(std::move(other.assembler_)) {
  other.closed_ = true;
}

WebSocketClient& WebSocketClient::operator=(WebSocketClient&& other) noexcept {
  if (this != &other) {
    close_fd();
    fd_ = std::exchange(other.fd_, -1);
    closed_ = other.closed_;
    close_sent_ = other.close_sent_;
    parser_ = std::move(other.parser_);
    assembler_ = std::move(other.assembler_);
    other.closed_ = true;
  }
  return *this;
}

bool WebSocketClient::closed() const { return closed_; }

void WebSocketClient::close_fd() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void WebSocketClient::send_all(std::span<const std::uint8_t> bytes) {
  if (fd_ < 0) {
    throw std::logic_error("send on a moved-from websocket");
  }
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectTimeoutMs);
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
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return std::nullopt;
    }
    const auto slice = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd ready{fd_, POLLIN, 0};
    const int result = ::poll(&ready, 1, static_cast<int>(slice.count()));
    if (result < 0) {
      if (errno == EINTR) {
        return std::nullopt;
      }
      const int err = errno;
      throw_error(err, "websocket receive");
    }
    if (result == 0) {
      continue;
    }
    std::uint8_t chunk[4096];
    const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      const int err = errno;
      throw_error(err, "websocket receive");
    }
    if (n == 0) {
      closed_ = true;
      return std::nullopt;
    }
    parser_.append({chunk, static_cast<std::size_t>(n)});
  }
}

}  // namespace arc
