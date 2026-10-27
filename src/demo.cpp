#include "arc/button.hpp"
#include "arc/i2c_device.hpp"
#include "arc/i2c_display_transport.hpp"
#include "arc/ssd1306.hpp"
#include "arc/websocket.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct Options {
  std::string i2c = "/dev/i2c-1";
  std::uint8_t address = 0x3D;
  std::string gpiochip = "/dev/gpiochip0";
  std::vector<std::uint32_t> buttons;
  std::optional<arc::WebSocketConfig> websocket;
};

void print_usage() {
  std::cerr
      << "Usage: oled_demo [--i2c PATH] [--address ADDR] [--gpiochip PATH]\n"
      << "                 [--buttons [A B]] [--websocket ws://HOST[:PORT]/PATH]\n"
      << "\n"
      << "Defaults: --i2c /dev/i2c-1 --address 0x3d\n"
      << "Without --buttons or --websocket the demo draws once and exits.\n"
      << "--buttons uses BCM 23 and BCM 22 unless two GPIO numbers are given.\n"
      << "Both buttons are active low with the internal pull-up.\n"
      << "A press on the first inverts the display. A press on the second toggles the block.\n"
      << "--websocket prints each text message and inverts the display when one arrives.\n"
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
    } else if (arg == "--buttons") {
      std::vector<std::uint32_t> pins;
      while (i + 1 < argc && argv[i + 1][0] != '-' && pins.size() < 2) {
        pins.push_back(parse_u32(argv[++i], "--buttons"));
      }
      if (pins.empty()) {
        pins = {23, 22};
      }
      if (pins.size() != 2) {
        throw std::invalid_argument("--buttons expects two GPIO numbers");
      }
      options.buttons = std::move(pins);
    } else if (arg == "--websocket") {
      options.websocket = arc::parse_websocket_url(require("--websocket"));
    } else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }
  if (options.websocket.has_value() && !options.buttons.empty()) {
    throw std::invalid_argument("--websocket and --buttons cannot be used together");
  }
  return options;
}

void draw(arc::Ssd1306& display, bool show_block) {
  display.clear();
  for (int x = 0; x < arc::Ssd1306::kWidth; ++x) {
    display.set_pixel(x, 0, true);
    display.set_pixel(x, arc::Ssd1306::kHeight - 1, true);
  }
  for (int y = 0; y < arc::Ssd1306::kHeight; ++y) {
    display.set_pixel(0, y, true);
    display.set_pixel(arc::Ssd1306::kWidth - 1, y, true);
  }
  if (show_block) {
    display.fill_rect(10, 10, 20, 16, true);
  }
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
    bool show_block = true;
    draw(display, show_block);
    std::cout << "Drew a border and a filled block\n";

    if (options.websocket.has_value()) {
      const arc::WebSocketConfig& endpoint = *options.websocket;
      arc::WebSocketClient client(endpoint);
      std::signal(SIGINT, on_signal);
      std::signal(SIGTERM, on_signal);
      std::cout << "Waiting for messages from ws://" << endpoint.host << ":" << endpoint.port
                << endpoint.path << " (Ctrl-C to exit)\n";
      bool inverted = false;
      while (g_stop == 0 && !client.closed()) {
        const std::optional<arc::WebSocketMessage> message = client.receive(std::chrono::milliseconds(200));
        if (!message.has_value() || g_stop != 0) {
          continue;
        }
        if (message->binary) {
          std::cout << "binary message, " << message->data.size() << " bytes\n";
        } else if (message->data.empty()) {
          std::cout << '\n';
        } else {
          const std::string_view text(reinterpret_cast<const char*>(message->data.data()), message->data.size());
          std::cout << text;
          if (text.back() != '\n') {
            std::cout << '\n';
          }
        }
        std::cout.flush();
        inverted = !inverted;
        display.set_inverted(inverted);
      }
      return EXIT_SUCCESS;
    }

    if (options.buttons.empty()) {
      return EXIT_SUCCESS;
    }

    std::vector<arc::Button> buttons;
    buttons.reserve(options.buttons.size());
    for (const std::uint32_t pin : options.buttons) {
      buttons.emplace_back(arc::Button::Config{options.gpiochip, pin});
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::cout << "Waiting for presses on GPIO " << options.buttons[0] << " and " << options.buttons[1]
              << " (Ctrl-C to exit)\n";
    bool inverted = false;
    while (g_stop == 0) {
      const std::optional<arc::IdentifiedButtonEvent> event =
          arc::wait_any(buttons, std::chrono::milliseconds(200));
      if (!event.has_value() || event->event != arc::ButtonEvent::Press || g_stop != 0) {
        continue;
      }
      if (event->index == 0) {
        inverted = !inverted;
        display.set_inverted(inverted);
      } else {
        show_block = !show_block;
        draw(display, show_block);
        if (inverted) {
          display.set_inverted(true);
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
