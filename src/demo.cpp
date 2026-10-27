#include "arc/gpio_input.hpp"
#include "arc/gpio_output.hpp"
#include "arc/spi_device.hpp"
#include "arc/spi_display_transport.hpp"
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
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct Options {
  std::string spi = "/dev/spidev0.0";
  std::string gpiochip = "/dev/gpiochip0";
  std::uint32_t dc = 24;
  std::optional<std::uint32_t> reset = 25;
  std::optional<std::uint32_t> button;
};

void print_usage() {
  std::cerr
      << "Usage: oled_demo [--spi PATH] [--gpiochip PATH] [--dc N] [--reset N] [--no-reset]\n"
      << "                 [--button [N]]\n"
      << "\n"
      << "Defaults: --spi /dev/spidev0.0 --gpiochip /dev/gpiochip0 --dc 24 --reset 25\n"
      << "Without --button the demo draws once and exits. --button uses BCM 23 unless N is set.\n"
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
    } else if (arg == "--spi") {
      options.spi = require("--spi");
    } else if (arg == "--gpiochip") {
      options.gpiochip = require("--gpiochip");
    } else if (arg == "--dc") {
      options.dc = parse_u32(require("--dc"), "--dc");
    } else if (arg == "--reset") {
      options.reset = parse_u32(require("--reset"), "--reset");
    } else if (arg == "--no-reset") {
      options.reset.reset();
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
    arc::SpiDevice spi({options.spi});
    arc::GpioOutput dc(options.gpiochip, options.dc, false);
    std::optional<arc::GpioOutput> reset;
    if (options.reset.has_value()) {
      reset.emplace(options.gpiochip, *options.reset, true);
    }
    arc::SpiDisplayTransport transport(spi, dc, reset ? &*reset : nullptr);
    arc::Ssd1306 display(transport);
    display.init();
    draw(display);
    std::cout << "Drew a border and a filled block\n";

    if (!options.button.has_value()) {
      return EXIT_SUCCESS;
    }

    arc::GpioInput button(options.gpiochip, *options.button, arc::Bias::PullUp, arc::Edge::Falling);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::cout << "Waiting for a falling edge on GPIO " << *options.button << " (Ctrl-C to exit)\n";
    bool inverted = false;
    while (g_stop == 0) {
      const std::optional<arc::Edge> edge = button.wait(std::chrono::milliseconds(200));
      if (!edge.has_value() || g_stop != 0) {
        continue;
      }
      inverted = !inverted;
      display.set_inverted(inverted);
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      for (;;) {
        if (!button.wait(std::chrono::milliseconds(0)).has_value()) {
          break;
        }
      }
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
