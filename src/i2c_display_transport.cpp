#include "arc/i2c_display_transport.hpp"

#include <algorithm>
#include <vector>

namespace arc {
namespace {

constexpr std::size_t kMaxDataBytes = 31;

void write_prefixed(I2cDevice& i2c, std::uint8_t prefix, std::span<const std::uint8_t> bytes) {
  std::vector<std::uint8_t> message;
  message.reserve(bytes.size() + 1);
  message.push_back(prefix);
  message.insert(message.end(), bytes.begin(), bytes.end());
  i2c.write(message);
}

}  // namespace

I2cDisplayTransport::I2cDisplayTransport(I2cDevice& i2c) : i2c_(i2c) {}

void I2cDisplayTransport::write_command(std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) {
    return;
  }
  write_prefixed(i2c_, 0x00, bytes);
}

void I2cDisplayTransport::write_data(std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t count = std::min(kMaxDataBytes, bytes.size() - offset);
    write_prefixed(i2c_, 0x40, bytes.subspan(offset, count));
    offset += count;
  }
}

void I2cDisplayTransport::reset() {}

}  // namespace arc
