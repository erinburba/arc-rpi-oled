#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace arc {

// Userspace master on a Linux i2c-dev node. address is the 7-bit slave address.
class I2cDevice {
 public:
  struct Config {
    std::string device = "/dev/i2c-1";
    std::uint8_t address = 0x3C;
  };

  explicit I2cDevice(Config config);
  ~I2cDevice();

  I2cDevice(const I2cDevice&) = delete;
  I2cDevice& operator=(const I2cDevice&) = delete;
  I2cDevice(I2cDevice&& other) noexcept;
  I2cDevice& operator=(I2cDevice&& other) noexcept;

  void write(std::span<const std::uint8_t> bytes);

 private:
  int fd_ = -1;

  void close_fd() noexcept;
};

}  // namespace arc
