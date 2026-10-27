#pragma once

#include "arc/display_transport.hpp"
#include "arc/gpio_output.hpp"
#include "arc/spi_device.hpp"

namespace arc {

// 4-wire SSD1306 link: SPI writes, D/C selects command vs data, optional RESET.
// spi, dc, and reset must outlive this object. reset may be null.
class SpiDisplayTransport : public DisplayTransport {
 public:
  SpiDisplayTransport(SpiDevice& spi, GpioOutput& dc, GpioOutput* reset);

  void write_command(std::span<const std::uint8_t> bytes) override;
  void write_data(std::span<const std::uint8_t> bytes) override;
  void reset() override;

 private:
  SpiDevice& spi_;
  GpioOutput& dc_;
  GpioOutput* reset_;
};

}  // namespace arc
