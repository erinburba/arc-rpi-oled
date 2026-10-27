#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ssl_st;
struct ssl_ctx_st;

namespace arc {

inline constexpr std::size_t kMaxWebSocketMessage = 1024 * 1024;

enum class WebSocketOpcode : std::uint8_t {
  Continuation = 0x0,
  Text = 0x1,
  Binary = 0x2,
  Close = 0x8,
  Ping = 0x9,
  Pong = 0xA,
};

struct WebSocketFrame {
  bool fin = true;
  WebSocketOpcode opcode = WebSocketOpcode::Text;
  std::vector<std::uint8_t> payload;
};

struct WebSocketMessage {
  bool binary = false;
  std::vector<std::uint8_t> data;
};

// Sec-WebSocket-Accept for a Sec-WebSocket-Key value, including the trailing padding.
std::string websocket_accept_key(std::string_view client_key);

// Base64 of a 16-byte nonce, used as Sec-WebSocket-Key.
std::string websocket_nonce_key(std::span<const std::uint8_t, 16> nonce);

// nullopt while the HTTP response headers are still incomplete.
// The size includes the final blank line. Throws if the completed handshake is rejected.
std::optional<std::size_t> websocket_handshake_header_size(std::string_view response,
                                                           std::string_view client_key);

// Client-to-server frames are masked. masking_key is the 4-byte key stored in the frame.
std::vector<std::uint8_t> encode_client_frame(const WebSocketFrame& frame,
                                               std::span<const std::uint8_t, 4> masking_key);

// accept_masked is for tests that decode a client frame. A server frame must be unmasked.
class WebSocketParser {
 public:
  explicit WebSocketParser(bool accept_masked = false);

  void append(std::span<const std::uint8_t> bytes);
  std::optional<WebSocketFrame> next_frame();

 private:
  bool accept_masked_ = false;
  std::vector<std::uint8_t> buffer_;
  std::size_t offset_ = 0;

  void compact();
};

// Reassembles data frames into one message. Control frames are returned immediately
// and do not disturb a fragmented message that is still open.
class WebSocketAssembler {
 public:
  enum class Kind { Pending, Message, Ping, Pong, Close };

  struct Result {
    Kind kind = Kind::Pending;
    WebSocketMessage message;
    std::vector<std::uint8_t> payload;
  };

  Result push(WebSocketFrame frame);

 private:
  bool open_ = false;
  bool binary_ = false;
  std::vector<std::uint8_t> data_;
};

// ws://host[:port][/path] or wss://host[:port][/path].
// The default port is 80 for ws and 443 for wss. The default path is /.
// ca_file is an extra PEM trust anchor. Empty uses the system trust store.
struct WebSocketConfig {
  std::string host;
  std::uint16_t port = 80;
  std::string path = "/";
  bool tls = false;
  std::string ca_file;
};

WebSocketConfig parse_websocket_url(std::string_view url);

// Receives messages from one WebSocket server. wss:// checks the server certificate.
// Pings are answered with a pong. receive returns nullopt on timeout, and also after
// the peer closes; closed() tells them apart.
class WebSocketClient {
 public:
  explicit WebSocketClient(WebSocketConfig config);
  ~WebSocketClient();

  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;
  WebSocketClient(WebSocketClient&& other) noexcept;
  WebSocketClient& operator=(WebSocketClient&& other) noexcept;

  bool closed() const;
  std::optional<WebSocketMessage> receive(std::chrono::milliseconds timeout);

 private:
  enum class ReadStatus { Bytes, Closed, Timeout, Interrupted };

  struct ReadChunk {
    ReadStatus status = ReadStatus::Closed;
    std::size_t size = 0;
  };

  int fd_ = -1;
  ssl_st* ssl_ = nullptr;
  ssl_ctx_st* ctx_ = nullptr;
  bool tls_ready_ = false;
  bool closed_ = false;
  bool close_sent_ = false;
  WebSocketParser parser_;
  WebSocketAssembler assembler_;

  void close_fd() noexcept;
  void start_tls(const WebSocketConfig& config);
  void send_all(std::span<const std::uint8_t> bytes);
  void send_frame(const WebSocketFrame& frame);
  ReadChunk read_chunk(std::span<std::uint8_t> buffer, std::chrono::steady_clock::time_point deadline);
  std::optional<WebSocketMessage> drain();
};

}  // namespace arc
