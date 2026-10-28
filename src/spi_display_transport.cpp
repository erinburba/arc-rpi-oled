#include "arc/spi_display_transport.hpp"

#include <chrono>
#include <thread>

namespace arc {

SpiDisplayTransport::SpiDisplayTransport(SpiDevice& spi, GpioOutput& dc, GpioOutput* reset)
    : spi_(spi), dc_(dc), reset_(reset) {}

void SpiDisplayTransport::write_command(std::span<const std::uint8_t> bytes) {
  dc_.set(false);
  spi_.write(bytes);
}

void SpiDisplayTransport::write_data(std::span<const std::uint8_t> bytes) {
  dc_.set(true);
  spi_.write(bytes);
}

void SpiDisplayTransport::reset() {
  if (reset_ == nullptr) {
    return;
  }
  reset_->set(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  reset_->set(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

}  // namespace arc
