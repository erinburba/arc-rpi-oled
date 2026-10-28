#pragma once

#include "arc/display_transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arc {

// SSD1306 128x64 framebuffer in horizontal addressing order.
// Byte index is x + (y / 8) * 128, and bit (y % 8) is the pixel. Bit 0 is the
// top row of that page.
class Ssd1306 {
 public:
  static constexpr int kWidth = 128;
  static constexpr int kHeight = 64;
  static constexpr std::size_t kBufferSize = static_cast<std::size_t>(kWidth) * kHeight / 8;

  explicit Ssd1306(DisplayTransport& transport);

  void init();
  void clear();
  // Blank the framebuffer and write it to the panel.
  void clear_window();
  void set_pixel(int x, int y, bool on);
  void fill_rect(int x, int y, int width, int height, bool on);
  // 5x7 ASCII. Glyphs are 6 pixels apart and lines are 8 pixels apart.
  // Text wraps at the right edge and stops at the bottom. Pixels past the
  // panel are clipped.
  void draw_text(int x, int y, std::string_view text);
  void flush();
  void set_fault(bool fault);

 private:
  DisplayTransport& transport_;
  std::array<std::uint8_t, kBufferSize> buffer_{};

  void command(std::span<const std::uint8_t> bytes);
};

}  // namespace arc
