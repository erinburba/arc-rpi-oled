#pragma once

#include "arc/display_transport.hpp"
#include "arc/i2c_device.hpp"

namespace arc {

// SSD1306 I2C link. A leading 0x00 marks command bytes and 0x40 marks pixel bytes.
class I2cDisplayTransport : public DisplayTransport {
 public:
  explicit I2cDisplayTransport(I2cDevice& i2c);

  void write_command(std::span<const std::uint8_t> bytes) override;
  void write_data(std::span<const std::uint8_t> bytes) override;
  void reset() override;

 private:
  I2cDevice& i2c_;
};

}  // namespace arc
