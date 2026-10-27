#include "arc/websocket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    std::cerr << file << ":" << line << ": " << expression << "\n";
    ++g_failures;
  }
}

#define EXPECT(condition) expect(static_cast<bool>(condition), #condition, __FILE__, __LINE__)

std::vector<std::uint8_t> bytes_of(std::string_view text) {
  return {text.begin(), text.end()};
}

std::string text_of(const std::vector<std::uint8_t>& bytes) {
  return {bytes.begin(), bytes.end()};
}

struct Socket {
  int fd = -1;

  explicit Socket(int owned = -1) : fd(owned) {}
  ~Socket() {
    if (fd >= 0) {
      ::close(fd);
    }
  }

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      if (fd >= 0) {
        ::close(fd);
      }
      fd = std::exchange(other.fd, -1);
    }
    return *this;
  }
};

void read_exact(int fd, void* data, std::size_t size) {
  auto* out = static_cast<std::uint8_t*>(data);
  std::size_t got = 0;
  while (got < size) {
    pollfd ready{fd, POLLIN, 0};
    if (::poll(&ready, 1, 3000) <= 0) {
      throw std::runtime_error("peer read timed out");
    }
    const ssize_t n = ::recv(fd, out + got, size - got, 0);
    if (n <= 0) {
      throw std::runtime_error("peer read failed");
    }
    got += static_cast<std::size_t>(n);
  }
}

void write_all(int fd, const void* data, std::size_t size) {
  const auto* in = static_cast<const std::uint8_t*>(data);
  std::size_t sent = 0;
  while (sent < size) {
    const ssize_t n = ::send(fd, in + sent, size - sent, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("peer write failed");
    }
    sent += static_cast<std::size_t>(n);
  }
}

void write_all(int fd, std::string_view text) { write_all(fd, text.data(), text.size()); }

std::string read_headers(int fd) {
  std::string buffered;
  while (buffered.find("\r\n\r\n") == std::string::npos) {
    if (buffered.size() > 8192) {
      throw std::runtime_error("peer headers are too large");
    }
    pollfd ready{fd, POLLIN, 0};
    if (::poll(&ready, 1, 3000) <= 0) {
      throw std::runtime_error("peer headers timed out");
    }
    char chunk[512];
    const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      throw std::runtime_error("peer headers failed");
    }
    buffered.append(chunk, static_cast<std::size_t>(n));
  }
  return buffered;
}

std::string header_value(std::string_view request, std::string_view name) {
  const std::size_t at = request.find(name);
  if (at == std::string_view::npos) {
    throw std::runtime_error("missing header");
  }
  std::size_t value = at + name.size();
  while (value < request.size() && request[value] == ' ') {
    ++value;
  }
  const std::size_t end = request.find("\r\n", value);
  if (end == std::string_view::npos) {
    throw std::runtime_error("header has no end");
  }
  return std::string(request.substr(value, end - value));
}

Socket listen_local(std::uint16_t& port) {
  Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
  if (listener.fd < 0) {
    throw std::runtime_error("socket");
  }
  const int enabled = 1;
  ::setsockopt(listener.fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    throw std::runtime_error("bind");
  }
  if (::listen(listener.fd, 1) < 0) {
    throw std::runtime_error("listen");
  }
  socklen_t length = sizeof(address);
  if (::getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
    throw std::runtime_error("getsockname");
  }
  port = ntohs(address.sin_port);
  return listener;
}

Socket accept_one(int listener) {
  pollfd ready{listener, POLLIN, 0};
  if (::poll(&ready, 1, 3000) <= 0) {
    throw std::runtime_error("accept timed out");
  }
  const int fd = ::accept(listener, nullptr, nullptr);
  if (fd < 0) {
    throw std::runtime_error("accept");
  }
  return Socket(fd);
}

std::string accept_upgrade(int fd) {
  const std::string request = read_headers(fd);
  const std::string key = header_value(request, "Sec-WebSocket-Key:");
  const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                               "Sec-WebSocket-Accept: " +
                               arc::websocket_accept_key(key) + "\r\n\r\n";
  write_all(fd, response);
  return request;
}

void test_accept_key() {
  EXPECT(arc::websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

void test_text_frame() {
  const std::uint8_t raw[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
  arc::WebSocketParser parser;
  parser.append(raw);
  const std::optional<arc::WebSocketFrame> frame = parser.next_frame();
  EXPECT(frame.has_value());
  EXPECT(frame->fin);
  EXPECT(frame->opcode == arc::WebSocketOpcode::Text);
  EXPECT(text_of(frame->payload) == "Hello");
  EXPECT(!parser.next_frame().has_value());
}

void test_extended_length() {
  std::vector<std::uint8_t> raw = {0x82, 126, 0x00, 126};
  raw.insert(raw.end(), 126, static_cast<std::uint8_t>(0xAB));
  arc::WebSocketParser parser;
  parser.append({raw.data(), 2});
  EXPECT(!parser.next_frame().has_value());
  parser.append({raw.data() + 2, raw.size() - 2});
  const std::optional<arc::WebSocketFrame> frame = parser.next_frame();
  EXPECT(frame.has_value());
  EXPECT(frame->opcode == arc::WebSocketOpcode::Binary);
  EXPECT(frame->payload.size() == 126);
  EXPECT(frame->payload.front() == 0xAB);
}

void test_masked_server_frame_is_rejected() {
  const std::uint8_t raw[] = {0x81, 0x81, 0, 0, 0, 0, 'Z'};
  arc::WebSocketParser parser;
  parser.append(raw);
  bool threw = false;
  try {
    parser.next_frame();
  } catch (const std::runtime_error&) {
    threw = true;
  }
  EXPECT(threw);
}

void test_client_frame_round_trip() {
  arc::WebSocketFrame frame;
  frame.opcode = arc::WebSocketOpcode::Text;
  frame.payload = bytes_of(std::string(200, 'Q'));
  const std::array<std::uint8_t, 4> mask{1, 2, 3, 4};
  const std::vector<std::uint8_t> encoded = arc::encode_client_frame(frame, mask);
  arc::WebSocketParser parser(true);
  parser.append(encoded);
  const std::optional<arc::WebSocketFrame> decoded = parser.next_frame();
  EXPECT(decoded.has_value());
  EXPECT(decoded->payload == frame.payload);
}

void test_fragment_with_ping() {
  arc::WebSocketAssembler assembler;
  arc::WebSocketFrame first;
  first.fin = false;
  first.opcode = arc::WebSocketOpcode::Text;
  first.payload = bytes_of("AB");
  EXPECT(assembler.push(first).kind == arc::WebSocketAssembler::Kind::Pending);

  arc::WebSocketFrame ping;
  ping.opcode = arc::WebSocketOpcode::Ping;
  ping.payload = bytes_of("Z");
  const arc::WebSocketAssembler::Result control = assembler.push(ping);
  EXPECT(control.kind == arc::WebSocketAssembler::Kind::Ping);
  EXPECT(text_of(control.payload) == "Z");

  arc::WebSocketFrame rest;
  rest.opcode = arc::WebSocketOpcode::Continuation;
  rest.payload = bytes_of("C");
  const arc::WebSocketAssembler::Result message = assembler.push(rest);
  EXPECT(message.kind == arc::WebSocketAssembler::Kind::Message);
  EXPECT(!message.message.binary);
  EXPECT(text_of(message.message.data) == "ABC");
}

void test_invalid_utf8() {
  arc::WebSocketAssembler assembler;
  arc::WebSocketFrame frame;
  frame.opcode = arc::WebSocketOpcode::Text;
  frame.payload = {0x80};
  bool threw = false;
  try {
    assembler.push(frame);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  EXPECT(threw);
}

void test_handshake_response() {
  EXPECT(!arc::websocket_handshake_header_size("HTTP/1.1 101 Switching Protocols\r\n", "key").has_value());
  const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
  const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: keep-alive, "
                               "Upgrade\r\nSec-WebSocket-Accept: " +
                               arc::websocket_accept_key(key) + "\r\n\r\nTRAILING";
  const std::optional<std::size_t> size = arc::websocket_handshake_header_size(response, key);
  EXPECT(size.has_value());
  EXPECT(response.substr(*size) == "TRAILING");

  bool threw = false;
  try {
    arc::websocket_handshake_header_size("HTTP/1.1 400 No\r\n\r\n", key);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  EXPECT(threw);
}

void test_url() {
  const arc::WebSocketConfig home = arc::parse_websocket_url("ws://panel.local/oled");
  EXPECT(home.host == "panel.local");
  EXPECT(home.port == 80);
  EXPECT(home.path == "/oled");

  const arc::WebSocketConfig explicit_port = arc::parse_websocket_url("ws://127.0.0.1:8765");
  EXPECT(explicit_port.host == "127.0.0.1");
  EXPECT(explicit_port.port == 8765);
  EXPECT(explicit_port.path == "/");

  bool rejected_tls = false;
  try {
    arc::parse_websocket_url("wss://panel.local/oled");
  } catch (const std::invalid_argument&) {
    rejected_tls = true;
  }
  EXPECT(rejected_tls);
}

void test_receive_text() {
  std::uint16_t port = 0;
  Socket listener = listen_local(port);
  std::exception_ptr error;
  std::thread server([&] {
    try {
      Socket peer = accept_one(listener.fd);
      accept_upgrade(peer.fd);
      const std::uint8_t frame[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
      write_all(peer.fd, frame, sizeof(frame));
    } catch (...) {
      error = std::current_exception();
    }
  });

  struct Join {
    std::thread& thread;
    ~Join() {
      if (thread.joinable()) {
        thread.join();
      }
    }
  } join{server};

  arc::WebSocketClient client(arc::WebSocketConfig{"127.0.0.1", port, "/oled"});
  const std::optional<arc::WebSocketMessage> message = client.receive(std::chrono::milliseconds(2000));
  EXPECT(message.has_value());
  EXPECT(!message->binary);
  EXPECT(text_of(message->data) == "Hello");
  server.join();
  if (error) {
    std::rethrow_exception(error);
  }
}

void test_ping_then_text() {
  std::uint16_t port = 0;
  Socket listener = listen_local(port);
  std::exception_ptr error;
  std::thread server([&] {
    try {
      Socket peer = accept_one(listener.fd);
      accept_upgrade(peer.fd);
      const std::uint8_t ping[] = {0x89, 0x02, 'p', 'q'};
      write_all(peer.fd, ping, sizeof(ping));
      std::uint8_t header[2];
      read_exact(peer.fd, header, 2);
      if (header[0] != 0x8A || (header[1] & 0x80) == 0 || (header[1] & 0x7F) != 2) {
        throw std::runtime_error("expected a masked pong");
      }
      std::uint8_t mask[4];
      read_exact(peer.fd, mask, 4);
      std::uint8_t payload[2];
      read_exact(peer.fd, payload, 2);
      if (static_cast<char>(payload[0] ^ mask[0]) != 'p' || static_cast<char>(payload[1] ^ mask[1]) != 'q') {
        throw std::runtime_error("pong payload mismatch");
      }
      const std::uint8_t frame[] = {0x81, 0x02, 'o', 'k'};
      write_all(peer.fd, frame, sizeof(frame));
    } catch (...) {
      error = std::current_exception();
    }
  });

  struct Join {
    std::thread& thread;
    ~Join() {
      if (thread.joinable()) {
        thread.join();
      }
    }
  } join{server};

  arc::WebSocketClient client(arc::WebSocketConfig{"127.0.0.1", port, "/"});
  const std::optional<arc::WebSocketMessage> message = client.receive(std::chrono::milliseconds(2000));
  EXPECT(message.has_value());
  EXPECT(text_of(message->data) == "ok");
  server.join();
  if (error) {
    std::rethrow_exception(error);
  }
}

void test_receive_timeout() {
  std::uint16_t port = 0;
  Socket listener = listen_local(port);
  std::exception_ptr error;
  std::thread server([&] {
    try {
      Socket peer = accept_one(listener.fd);
      accept_upgrade(peer.fd);
      pollfd ready{peer.fd, POLLIN, 0};
      if (::poll(&ready, 1, 2000) <= 0) {
        throw std::runtime_error("expected the client to close");
      }
      char ignored = 0;
      ::recv(peer.fd, &ignored, 1, 0);
    } catch (...) {
      error = std::current_exception();
    }
  });

  struct Join {
    std::thread& thread;
    ~Join() {
      if (thread.joinable()) {
        thread.join();
      }
    }
  } join{server};

  {
    arc::WebSocketClient client(arc::WebSocketConfig{"127.0.0.1", port, "/"});
    const auto started = std::chrono::steady_clock::now();
    const std::optional<arc::WebSocketMessage> timed_out = client.receive(std::chrono::milliseconds(50));
    EXPECT(!timed_out.has_value());
    EXPECT(!client.closed());
    EXPECT(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1000));
  }
  server.join();
  if (error) {
    std::rethrow_exception(error);
  }
}

void test_peer_close() {
  std::uint16_t port = 0;
  Socket listener = listen_local(port);
  std::exception_ptr error;
  std::thread server([&] {
    try {
      Socket peer = accept_one(listener.fd);
      accept_upgrade(peer.fd);
      const std::uint8_t close_frame[] = {0x88, 0x00};
      write_all(peer.fd, close_frame, sizeof(close_frame));
      std::uint8_t header[2];
      read_exact(peer.fd, header, 2);
      if (header[0] != 0x88) {
        throw std::runtime_error("expected a close reply");
      }
    } catch (...) {
      error = std::current_exception();
    }
  });

  struct Join {
    std::thread& thread;
    ~Join() {
      if (thread.joinable()) {
        thread.join();
      }
    }
  } join{server};

  arc::WebSocketClient client(arc::WebSocketConfig{"127.0.0.1", port, "/"});
  const std::optional<arc::WebSocketMessage> closed = client.receive(std::chrono::milliseconds(2000));
  EXPECT(!closed.has_value());
  EXPECT(client.closed());
  server.join();
  if (error) {
    std::rethrow_exception(error);
  }
}

}  // namespace

int main() {
  test_accept_key();
  test_text_frame();
  test_extended_length();
  test_masked_server_frame_is_rejected();
  test_client_frame_round_trip();
  test_fragment_with_ping();
  test_invalid_utf8();
  test_handshake_response();
  test_url();
  try {
    test_receive_text();
    test_ping_then_text();
    test_receive_timeout();
    test_peer_close();
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << "\n";
    return EXIT_FAILURE;
  }
  if (g_failures != 0) {
    std::cerr << g_failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "ok\n";
  return EXIT_SUCCESS;
}
