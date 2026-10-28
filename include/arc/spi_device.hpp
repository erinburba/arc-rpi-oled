#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace arc {

// Userspace SPI master on a Linux spidev node. Mode values match
// linux/spi/spidev.h: 0 is SPI_MODE_0.
class SpiDevice {
 public:
  struct Config {
    std::string device = "/dev/spidev0.0";
    std::uint32_t speed_hz = 8'000'000;
    std::uint8_t mode = 0;
    std::uint8_t bits_per_word = 8;
  };

  explicit SpiDevice(Config config);
  ~SpiDevice();

  SpiDevice(const SpiDevice&) = delete;
  SpiDevice& operator=(const SpiDevice&) = delete;
  SpiDevice(SpiDevice&& other) noexcept;
  SpiDevice& operator=(SpiDevice&& other) noexcept;

  void write(std::span<const std::uint8_t> tx);
  void transfer(std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx);

 private:
  int fd_ = -1;
  std::uint32_t speed_hz_ = 0;
  std::uint8_t bits_per_word_ = 8;

  void close_fd() noexcept;
};

}  // namespace arc
