#include "arc/ssd1306.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    std::cerr << file << ":" << line << ": " << expression << "\n";
    ++g_failures;
  }
}

#define EXPECT(condition) expect(static_cast<bool>(condition), #condition, __FILE__, __LINE__)

class RecordingTransport : public arc::DisplayTransport {
 public:
  void write_command(std::span<const std::uint8_t> bytes) override {
    commands.emplace_back(bytes.begin(), bytes.end());
  }

  void write_data(std::span<const std::uint8_t> bytes) override {
    data.assign(bytes.begin(), bytes.end());
    ++data_writes;
  }

  void reset() override { ++resets; }

  std::vector<std::vector<std::uint8_t>> commands;
  std::vector<std::uint8_t> data;
  int data_writes = 0;
  int resets = 0;
};

std::uint8_t pixel_byte(const RecordingTransport& transport, int x, int y) {
  const std::size_t index = static_cast<std::size_t>(x) + static_cast<std::size_t>(y / 8) * 128;
  return transport.data.at(index);
}

void test_pixel_packing() {
  RecordingTransport transport;
  arc::Ssd1306 display(transport);
  display.set_pixel(0, 0, true);
  display.set_pixel(0, 7, true);
  display.set_pixel(1, 8, true);
  display.flush();

  EXPECT(transport.data.size() == arc::Ssd1306::kBufferSize);
  EXPECT(pixel_byte(transport, 0, 0) == 0x81);
  EXPECT(pixel_byte(transport, 1, 8) == 0x01);

  display.set_pixel(0, 0, false);
  display.flush();
  EXPECT(pixel_byte(transport, 0, 0) == 0x80);

  display.clear();
  display.fill_rect(126, 62, 4, 4, true);
  display.flush();
  constexpr auto bottom_page = static_cast<std::uint8_t>((1u << 6) | (1u << 7));
  EXPECT(pixel_byte(transport, 126, 62) == bottom_page);
  EXPECT(pixel_byte(transport, 127, 63) == bottom_page);
  EXPECT(pixel_byte(transport, 125, 62) == 0);
  EXPECT(pixel_byte(transport, 0, 0) == 0);
}

void test_out_of_range() {
  RecordingTransport transport;
  arc::Ssd1306 display(transport);
  const int bad_points[][2] = {{-1, 0}, {0, -1}, {128, 0}, {0, 64}};
  for (const auto& point : bad_points) {
    bool threw = false;
    try {
      display.set_pixel(point[0], point[1], true);
    } catch (const std::out_of_range&) {
      threw = true;
    }
    EXPECT(threw);
  }
  display.flush();
  EXPECT(transport.data.size() == arc::Ssd1306::kBufferSize);
  for (std::uint8_t byte : transport.data) {
    EXPECT(byte == 0);
  }
}

void test_init_commands() {
  RecordingTransport transport;
  arc::Ssd1306 display(transport);
  display.init();
  const std::vector<std::vector<std::uint8_t>> expected = {
      {0xAE}, {0xD5, 0x80}, {0xA8, 0x3F}, {0xD3, 0x00}, {0x40}, {0x8D, 0x14},
      {0x20, 0x00}, {0xA1}, {0xC8}, {0xDA, 0x12}, {0x81, 0xCF}, {0xD9, 0xF1},
      {0xDB, 0x40}, {0xA4}, {0xA6}, {0xAF},
  };
  EXPECT(transport.resets == 1);
  EXPECT(transport.commands == expected);
}

void test_flush_and_invert() {
  RecordingTransport transport;
  arc::Ssd1306 display(transport);
  display.flush();
  EXPECT(transport.data_writes == 1);
  EXPECT(transport.data.size() == 1024);
  EXPECT(transport.commands.size() == 2);
  EXPECT((transport.commands[0] == std::vector<std::uint8_t>{0x21, 0x00, 127}));
  EXPECT((transport.commands[1] == std::vector<std::uint8_t>{0x22, 0x00, 0x07}));

  display.set_inverted(true);
  display.set_inverted(false);
  EXPECT(transport.commands.size() == 4);
  EXPECT((transport.commands[2] == std::vector<std::uint8_t>{0xA7}));
  EXPECT((transport.commands[3] == std::vector<std::uint8_t>{0xA6}));
}

}  // namespace

int main() {
  test_pixel_packing();
  test_out_of_range();
  test_init_commands();
  test_flush_and_invert();
  if (g_failures != 0) {
    std::cerr << g_failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "ok\n";
  return EXIT_SUCCESS;
}
