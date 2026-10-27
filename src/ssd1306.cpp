#include "arc/ssd1306.hpp"

#include <stdexcept>

namespace arc {

Ssd1306::Ssd1306(DisplayTransport& transport) : transport_(transport) {}

void Ssd1306::init() {
  transport_.reset();
  const std::uint8_t display_off[] = {0xAE};
  const std::uint8_t clock_div[] = {0xD5, 0x80};
  const std::uint8_t multiplex[] = {0xA8, 0x3F};
  const std::uint8_t display_offset[] = {0xD3, 0x00};
  const std::uint8_t start_line[] = {0x40};
  const std::uint8_t charge_pump[] = {0x8D, 0x14};
  const std::uint8_t addressing[] = {0x20, 0x00};
  const std::uint8_t segment_remap[] = {0xA1};
  const std::uint8_t com_scan[] = {0xC8};
  const std::uint8_t com_pins[] = {0xDA, 0x12};
  const std::uint8_t contrast[] = {0x81, 0xCF};
  const std::uint8_t precharge[] = {0xD9, 0xF1};
  const std::uint8_t vcomh[] = {0xDB, 0x40};
  const std::uint8_t display_ram[] = {0xA4};
  const std::uint8_t normal[] = {0xA6};
  const std::uint8_t display_on[] = {0xAF};
  command(display_off);
  command(clock_div);
  command(multiplex);
  command(display_offset);
  command(start_line);
  command(charge_pump);
  command(addressing);
  command(segment_remap);
  command(com_scan);
  command(com_pins);
  command(contrast);
  command(precharge);
  command(vcomh);
  command(display_ram);
  command(normal);
  command(display_on);
}

void Ssd1306::clear() { buffer_.fill(0); }

void Ssd1306::set_pixel(int x, int y, bool on) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) {
    throw std::out_of_range("pixel is outside the 128x64 panel");
  }
  const std::size_t index =
      static_cast<std::size_t>(x) + static_cast<std::size_t>(y / 8) * static_cast<std::size_t>(kWidth);
  const auto bit = static_cast<std::uint8_t>(1u << (y % 8));
  if (on) {
    buffer_[index] |= bit;
  } else {
    buffer_[index] = static_cast<std::uint8_t>(buffer_[index] & static_cast<std::uint8_t>(~bit));
  }
}

void Ssd1306::fill_rect(int x, int y, int width, int height, bool on) {
  if (width <= 0 || height <= 0) {
    return;
  }
  const int x_end = x + width;
  const int y_end = y + height;
  for (int row = y; row < y_end; ++row) {
    if (row < 0 || row >= kHeight) {
      continue;
    }
    for (int col = x; col < x_end; ++col) {
      if (col < 0 || col >= kWidth) {
        continue;
      }
      set_pixel(col, row, on);
    }
  }
}

void Ssd1306::flush() {
  const std::uint8_t column[] = {0x21, 0x00, 127};
  const std::uint8_t page[] = {0x22, 0x00, 0x07};
  command(column);
  command(page);
  transport_.write_data(buffer_);
}

void Ssd1306::set_inverted(bool inverted) {
  const std::uint8_t command_byte[] = {inverted ? std::uint8_t{0xA7} : std::uint8_t{0xA6}};
  command(command_byte);
}

void Ssd1306::command(std::span<const std::uint8_t> bytes) { transport_.write_command(bytes); }

}  // namespace arc
