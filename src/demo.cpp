#include "arc/button.hpp"
#include "arc/i2c_device.hpp"
#include "arc/i2c_display_transport.hpp"
#include "arc/ssd1306.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct Options {
  std::string i2c = "/dev/i2c-1";
  std::uint8_t address = 0x3D;
  std::string gpiochip = "/dev/gpiochip0";
  std::optional<std::uint32_t> button;
};

void print_usage() {
  std::cerr
      << "Usage: oled_demo [--i2c PATH] [--address ADDR] [--gpiochip PATH] [--button [N]]\n"
      << "\n"
      << "Defaults: --i2c /dev/i2c-1 --address 0x3d\n"
      << "Without --button the demo draws once and exits. --button uses BCM 23 unless N is set.\n"
      << "The button is active low with the internal pull-up. A press inverts the display.\n"
      << "On Pi 5 the header is often /dev/gpiochip4.\n";
}

std::uint32_t parse_u32(const std::string& text, const char* name) {
  try {
    std::size_t used = 0;
    const unsigned long value = std::stoul(text, &used, 10);
    if (used != text.size()) {
      throw std::invalid_argument(name);
    }
    if constexpr (sizeof(unsigned long) > sizeof(std::uint32_t)) {
      if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(name);
      }
    }
    return static_cast<std::uint32_t>(value);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string("expected an integer for ") + name);
  }
}

Options parse_args(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        throw std::invalid_argument(std::string("missing value for ") + name);
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(EXIT_SUCCESS);
    } else if (arg == "--i2c") {
      options.i2c = require("--i2c");
    } else if (arg == "--address") {
      try {
        const unsigned long address = std::stoul(require("--address"), nullptr, 0);
        if (address > 0x7F) {
          throw std::invalid_argument("--address");
        }
        options.address = static_cast<std::uint8_t>(address);
      } catch (const std::exception&) {
        throw std::invalid_argument("expected a 7-bit I2C address for --address");
      }
    } else if (arg == "--gpiochip") {
      options.gpiochip = require("--gpiochip");
    } else if (arg == "--button") {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        options.button = parse_u32(argv[++i], "--button");
      } else {
        options.button = 23;
      }
    } else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }
  return options;
}

void draw(arc::Ssd1306& display) {
  display.clear();
  for (int x = 0; x < arc::Ssd1306::kWidth; ++x) {
    display.set_pixel(x, 0, true);
    display.set_pixel(x, arc::Ssd1306::kHeight - 1, true);
  }
  for (int y = 0; y < arc::Ssd1306::kHeight; ++y) {
    display.set_pixel(0, y, true);
    display.set_pixel(arc::Ssd1306::kWidth - 1, y, true);
  }
  display.fill_rect(10, 10, 20, 16, true);
  display.flush();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_args(argc, argv);
    arc::I2cDevice i2c({options.i2c, options.address});
    arc::I2cDisplayTransport transport(i2c);
    arc::Ssd1306 display(transport);
    display.init();
    draw(display);
    std::cout << "Drew a border and a filled block\n";

    if (!options.button.has_value()) {
      return EXIT_SUCCESS;
    }

    arc::Button button({options.gpiochip, *options.button});
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::cout << "Waiting for a press on GPIO " << *options.button << " (Ctrl-C to exit)\n";
    bool inverted = false;
    while (g_stop == 0) {
      const std::optional<arc::ButtonEvent> event = button.wait(std::chrono::milliseconds(200));
      if (event != arc::ButtonEvent::Press || g_stop != 0) {
        continue;
      }
      inverted = !inverted;
      display.set_inverted(inverted);
    }
    return EXIT_SUCCESS;
  } catch (const std::invalid_argument& ex) {
    std::cerr << ex.what() << "\n";
    print_usage();
    return EXIT_FAILURE;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
