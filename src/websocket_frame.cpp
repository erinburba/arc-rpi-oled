#include "arc/websocket.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>

namespace arc {
namespace {

constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::uint32_t rotate_left(std::uint32_t value, int bits) {
  return (value << bits) | (value >> (32 - bits));
}

std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> message) {
  std::uint32_t h0 = 0x67452301u;
  std::uint32_t h1 = 0xEFCDAB89u;
  std::uint32_t h2 = 0x98BADCFEu;
  std::uint32_t h3 = 0x10325476u;
  std::uint32_t h4 = 0xC3D2E1F0u;

  const std::uint64_t bit_length = static_cast<std::uint64_t>(message.size()) * 8u;
  std::vector<std::uint8_t> data(message.begin(), message.end());
  data.push_back(0x80);
  while ((data.size() % 64) != 56) {
    data.push_back(0);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    data.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xFF));
  }

  for (std::size_t chunk = 0; chunk < data.size(); chunk += 64) {
    std::uint32_t words[80];
    for (int i = 0; i < 16; ++i) {
      const std::size_t offset = chunk + static_cast<std::size_t>(i) * 4;
      words[i] = (static_cast<std::uint32_t>(data[offset]) << 24) |
                 (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
                 (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
                 static_cast<std::uint32_t>(data[offset + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      words[i] = rotate_left(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    }

    std::uint32_t a = h0;
    std::uint32_t b = h1;
    std::uint32_t c = h2;
    std::uint32_t d = h3;
    std::uint32_t e = h4;
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f = 0;
      std::uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const std::uint32_t temp = rotate_left(a, 5) + f + e + k + words[i];
      e = d;
      d = c;
      c = rotate_left(b, 30);
      b = a;
      a = temp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  const std::uint32_t hashes[5] = {h0, h1, h2, h3, h4};
  std::array<std::uint8_t, 20> digest{};
  for (int i = 0; i < 5; ++i) {
    digest[static_cast<std::size_t>(i) * 4] = static_cast<std::uint8_t>(hashes[i] >> 24);
    digest[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(hashes[i] >> 16);
    digest[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>(hashes[i] >> 8);
    digest[static_cast<std::size_t>(i) * 4 + 3] = static_cast<std::uint8_t>(hashes[i]);
  }
  return digest;
}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  std::size_t index = 0;
  while (index + 3 <= bytes.size()) {
    const std::uint32_t value = (static_cast<std::uint32_t>(bytes[index]) << 16) |
                                (static_cast<std::uint32_t>(bytes[index + 1]) << 8) |
                                static_cast<std::uint32_t>(bytes[index + 2]);
    out.push_back(kBase64[(value >> 18) & 63]);
    out.push_back(kBase64[(value >> 12) & 63]);
    out.push_back(kBase64[(value >> 6) & 63]);
    out.push_back(kBase64[value & 63]);
    index += 3;
  }
  if (index < bytes.size()) {
    std::uint32_t value = static_cast<std::uint32_t>(bytes[index]) << 16;
    out.push_back(kBase64[(value >> 18) & 63]);
    if (index + 1 < bytes.size()) {
      value |= static_cast<std::uint32_t>(bytes[index + 1]) << 8;
      out.push_back(kBase64[(value >> 12) & 63]);
      out.push_back(kBase64[(value >> 6) & 63]);
      out.push_back('=');
    } else {
      out.push_back(kBase64[(value >> 12) & 63]);
      out.push_back('=');
      out.push_back('=');
    }
  }
  return out;
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

bool header_has_token(std::string_view value, std::string_view token) {
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::size_t length = comma == std::string_view::npos ? value.size() - start : comma - start;
    const std::string_view part = trim(value.substr(start, length));
    if (lower_copy(part) == token) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return false;
}

bool known_opcode(std::uint8_t opcode) {
  switch (opcode) {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x8:
    case 0x9:
    case 0xA:
      return true;
    default:
      return false;
  }
}

bool utf8_ok(std::span<const std::uint8_t> bytes) {
  std::size_t index = 0;
  while (index < bytes.size()) {
    const std::uint8_t lead = bytes[index];
    std::size_t width = 1;
    if (lead <= 0x7F) {
      ++index;
      continue;
    }
    if ((lead & 0xE0) == 0xC0) {
      width = 2;
      if (lead < 0xC2) {
        return false;
      }
    } else if ((lead & 0xF0) == 0xE0) {
      width = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      width = 4;
      if (lead > 0xF4) {
        return false;
      }
    } else {
      return false;
    }
    if (index + width > bytes.size()) {
      return false;
    }
    for (std::size_t extra = 1; extra < width; ++extra) {
      if ((bytes[index + extra] & 0xC0) != 0x80) {
        return false;
      }
    }
    if (width == 3) {
      const std::uint32_t codepoint = ((static_cast<std::uint32_t>(lead) & 0x0F) << 12) |
                                      ((static_cast<std::uint32_t>(bytes[index + 1]) & 0x3F) << 6) |
                                      (static_cast<std::uint32_t>(bytes[index + 2]) & 0x3F);
      if (codepoint < 0x800 || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return false;
      }
    } else if (width == 4) {
      const std::uint32_t codepoint = ((static_cast<std::uint32_t>(lead) & 0x07) << 18) |
                                      ((static_cast<std::uint32_t>(bytes[index + 1]) & 0x3F) << 12) |
                                      ((static_cast<std::uint32_t>(bytes[index + 2]) & 0x3F) << 6) |
                                      (static_cast<std::uint32_t>(bytes[index + 3]) & 0x3F);
      if (codepoint < 0x10000 || codepoint > 0x10FFFF) {
        return false;
      }
    }
    index += width;
  }
  return true;
}

}  // namespace

std::string websocket_nonce_key(std::span<const std::uint8_t, 16> nonce) { return base64_encode(nonce); }

std::string websocket_accept_key(std::string_view client_key) {
  std::vector<std::uint8_t> material(client_key.begin(), client_key.end());
  material.insert(material.end(), kWebSocketGuid, kWebSocketGuid + sizeof(kWebSocketGuid) - 1);
  const std::array<std::uint8_t, 20> digest = sha1(material);
  return base64_encode(digest);
}

std::optional<std::size_t> websocket_handshake_header_size(std::string_view response,
                                                           std::string_view client_key) {
  const std::size_t end = response.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    return std::nullopt;
  }

  const std::string_view block = response.substr(0, end);
  const std::size_t status_end = block.find("\r\n");
  const std::string_view status = status_end == std::string_view::npos ? block : block.substr(0, status_end);
  if (status.size() < 12 || status.compare(0, 5, "HTTP/") != 0) {
    throw std::runtime_error("websocket handshake status is invalid");
  }
  const std::size_t code_at = status.find(' ');
  if (code_at == std::string_view::npos || code_at + 3 > status.size() ||
      status.compare(code_at + 1, 3, "101") != 0 ||
      (code_at + 4 < status.size() && status[code_at + 4] != ' ')) {
    throw std::runtime_error("websocket handshake was not accepted");
  }

  bool upgrade = false;
  bool connection = false;
  bool accept = false;
  const std::string expected = websocket_accept_key(client_key);
  std::size_t line_at = status_end == std::string_view::npos ? block.size() : status_end + 2;
  while (line_at < block.size()) {
    const std::size_t next = block.find("\r\n", line_at);
    const std::string_view line = block.substr(line_at, next == std::string_view::npos ? block.size() - line_at : next - line_at);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      throw std::runtime_error("websocket handshake header is invalid");
    }
    const std::string name = lower_copy(trim(line.substr(0, colon)));
    const std::string_view value = trim(line.substr(colon + 1));
    if (name == "upgrade") {
      upgrade = lower_copy(value) == "websocket";
    } else if (name == "connection") {
      connection = header_has_token(value, "upgrade");
    } else if (name == "sec-websocket-accept") {
      accept = value == expected;
    }
    if (next == std::string_view::npos) {
      break;
    }
    line_at = next + 2;
  }
  if (!upgrade || !connection || !accept) {
    throw std::runtime_error("websocket handshake is missing an upgrade header");
  }
  return end + 4;
}

std::vector<std::uint8_t> encode_client_frame(const WebSocketFrame& frame,
                                               std::span<const std::uint8_t, 4> masking_key) {
  if (frame.payload.size() > kMaxWebSocketMessage) {
    throw std::runtime_error("websocket frame is too large");
  }
  const auto opcode = static_cast<std::uint8_t>(frame.opcode);
  if (!known_opcode(opcode)) {
    throw std::runtime_error("websocket opcode is not supported");
  }
  const bool control = (opcode & 0x08) != 0;
  if (control && (!frame.fin || frame.payload.size() > 125)) {
    throw std::runtime_error("websocket control frame is invalid");
  }

  std::vector<std::uint8_t> out;
  out.push_back(static_cast<std::uint8_t>((frame.fin ? 0x80 : 0x00) | opcode));
  const std::uint64_t length = frame.payload.size();
  if (length < 126) {
    out.push_back(static_cast<std::uint8_t>(0x80 | length));
  } else if (length <= 0xFFFF) {
    out.push_back(0x80 | 126);
    out.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(length & 0xFF));
  } else {
    out.push_back(0x80 | 127);
    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<std::uint8_t>((length >> shift) & 0xFF));
    }
  }
  out.insert(out.end(), masking_key.begin(), masking_key.end());
  for (std::size_t i = 0; i < frame.payload.size(); ++i) {
    out.push_back(static_cast<std::uint8_t>(frame.payload[i] ^ masking_key[i % 4]));
  }
  return out;
}

WebSocketParser::WebSocketParser(bool accept_masked) : accept_masked_(accept_masked) {}

void WebSocketParser::append(std::span<const std::uint8_t> bytes) {
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void WebSocketParser::compact() {
  if (offset_ == 0) {
    return;
  }
  if (offset_ == buffer_.size()) {
    buffer_.clear();
    offset_ = 0;
    return;
  }
  if (offset_ >= 4096) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
    offset_ = 0;
  }
}

std::optional<WebSocketFrame> WebSocketParser::next_frame() {
  const auto at = [this](std::size_t index) { return buffer_[offset_ + index]; };
  const std::size_t available = buffer_.size() - offset_;
  if (available < 2) {
    return std::nullopt;
  }

  const std::uint8_t first = at(0);
  const std::uint8_t second = at(1);
  if ((first & 0x70) != 0) {
    throw std::runtime_error("websocket reserved bits are set");
  }
  const std::uint8_t opcode = first & 0x0F;
  if (!known_opcode(opcode)) {
    throw std::runtime_error("websocket opcode is not supported");
  }
  const bool masked = (second & 0x80) != 0;
  if (masked && !accept_masked_) {
    throw std::runtime_error("websocket server frame is masked");
  }

  const std::uint8_t length_code = second & 0x7F;
  std::size_t header = 2;
  if (length_code == 126) {
    header += 2;
  } else if (length_code == 127) {
    header += 8;
  }
  if (masked) {
    header += 4;
  }
  if (available < header) {
    return std::nullopt;
  }

  std::uint64_t payload_length = length_code;
  std::size_t cursor = 2;
  if (length_code == 126) {
    payload_length = (static_cast<std::uint32_t>(at(2)) << 8) | at(3);
    cursor = 4;
  } else if (length_code == 127) {
    if ((at(2) & 0x80) != 0) {
      throw std::runtime_error("websocket payload length is invalid");
    }
    payload_length = 0;
    for (int i = 0; i < 8; ++i) {
      payload_length = (payload_length << 8) | at(static_cast<std::size_t>(2 + i));
    }
    cursor = 10;
  }
  if (payload_length > kMaxWebSocketMessage) {
    throw std::runtime_error("websocket frame is too large");
  }
  const bool control = (opcode & 0x08) != 0;
  if (control && ((first & 0x80) == 0 || payload_length > 125)) {
    throw std::runtime_error("websocket control frame is invalid");
  }

  std::uint8_t mask[4] = {};
  if (masked) {
    for (int i = 0; i < 4; ++i) {
      mask[i] = at(cursor + static_cast<std::size_t>(i));
    }
    cursor += 4;
  }
  if (available < cursor + payload_length) {
    return std::nullopt;
  }

  WebSocketFrame frame;
  frame.fin = (first & 0x80) != 0;
  frame.opcode = static_cast<WebSocketOpcode>(opcode);
  frame.payload.resize(static_cast<std::size_t>(payload_length));
  for (std::size_t i = 0; i < frame.payload.size(); ++i) {
    const std::uint8_t raw = at(cursor + i);
    frame.payload[i] = masked ? static_cast<std::uint8_t>(raw ^ mask[i % 4]) : raw;
  }
  offset_ += cursor + static_cast<std::size_t>(payload_length);
  compact();
  return frame;
}

WebSocketAssembler::Result WebSocketAssembler::push(WebSocketFrame frame) {
  if (frame.opcode == WebSocketOpcode::Ping || frame.opcode == WebSocketOpcode::Pong ||
      frame.opcode == WebSocketOpcode::Close) {
    Result result;
    result.kind = frame.opcode == WebSocketOpcode::Ping   ? Kind::Ping
                  : frame.opcode == WebSocketOpcode::Pong ? Kind::Pong
                                                           : Kind::Close;
    result.payload = std::move(frame.payload);
    return result;
  }

  if (frame.opcode == WebSocketOpcode::Continuation) {
    if (!open_) {
      throw std::runtime_error("websocket continuation has no message");
    }
  } else if (frame.opcode == WebSocketOpcode::Text || frame.opcode == WebSocketOpcode::Binary) {
    if (open_) {
      throw std::runtime_error("websocket message was interrupted");
    }
    open_ = true;
    binary_ = frame.opcode == WebSocketOpcode::Binary;
    data_.clear();
  }

  if (data_.size() + frame.payload.size() > kMaxWebSocketMessage) {
    throw std::runtime_error("websocket message is too large");
  }
  data_.insert(data_.end(), frame.payload.begin(), frame.payload.end());
  if (!frame.fin) {
    return {};
  }
  if (!binary_ && !utf8_ok(data_)) {
    open_ = false;
    data_.clear();
    throw std::runtime_error("websocket text is not UTF-8");
  }

  Result result;
  result.kind = Kind::Message;
  result.message.binary = binary_;
  result.message.data = std::move(data_);
  open_ = false;
  binary_ = false;
  data_.clear();
  return result;
}

}  // namespace arc
