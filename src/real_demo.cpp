#include "arc/button.hpp"
#include "arc/http_client.hpp"
#include "arc/i2c_device.hpp"
#include "arc/i2c_display_transport.hpp"
#include "arc/json.hpp"
#include "arc/ssd1306.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct Options {
  std::string i2c = "/dev/i2c-1";
  std::uint8_t address = 0x3D;
  std::string gpiochip = "/dev/gpiochip4";
  std::vector<std::uint32_t> buttons;
  std::string api_url = "https://api.wheretheiss.at/v1/satellites/25544";
  std::optional<arc::HttpRequest> api;
  std::string ca_file;
};

void print_usage() {
  std::cerr
      << "Usage: oled_demo [--i2c PATH] [--address ADDR] [--gpiochip PATH]\n"
      << "                 [--buttons [A B]] [--api http:// or https:// URL] [--ca PEM]\n"
      << "\n"
      << "Defaults: --i2c /dev/i2c-1 --address 0x3d\n"
      << "Without --buttons or --api the demo draws once and exits.\n"
      << "--buttons uses BCM 23 and BCM 22 unless two GPIO numbers are given.\n"
      << "Both buttons are active low with the internal pull-up.\n"
      << "A press on the first inverts the display. A press on the second toggles the block.\n"
      << "--api polls a JSON document every 2 seconds. A changed document is printed,\n"
      << "and the display inverts. https:// checks the server certificate.\n"
      << "--ca adds a PEM trust anchor.\n"
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
        pins = {FEED_BUTTON, FAULT_BUTTON};
      }
      if (pins.size() != 2) {
        throw std::invalid_argument("--buttons expects two GPIO numbers");
      }
      options.buttons = std::move(pins);
    } else if (arg == "--api") {
      // options.api = arc::parse_http_url(require("--api"));
      options.api = arc::parse_http_url(options.api_url);
    } else if (arg == "--ca") {
      options.ca_file = require("--ca");
    } else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }
  if (!options.ca_file.empty() && !options.api.has_value()) {
    throw std::invalid_argument("--ca requires --api");
  }
  if (options.api.has_value() && !options.buttons.empty()) {
    throw std::invalid_argument("--api and --buttons cannot be used together");
  }

  std::vector<std::uint32_t> pins;
  pins = {23, 22};
  options.buttons = std::move(pins);
  options.api = arc::parse_http_url(options.api_url);
  if (options.api.has_value()) {
    options.api->ca_file = options.ca_file;
  }
  return options;
}

constexpr int kReconnectAfterFailures = 5;

void draw_coordinates(arc::Ssd1306& display, const std::string& latitude, const std::string& longitude) {
  display.clear();
  display.draw_text(0, 5, "ISS coordinates:");
  display.draw_text(0, 30, latitude.empty() ? "..." : latitude);
  display.draw_text(0, 50, longitude.empty() ? "..." : longitude);
  display.flush();
}

void draw_reconnecting(arc::Ssd1306& display) {
  display.clear();
  display.draw_text(0, 28, "Reconnecting...");
  display.flush();
}

struct PollStop {
  std::atomic<bool>* stop = nullptr;
  std::thread* thread = nullptr;

  ~PollStop() {
    if (stop != nullptr) {
      stop->store(true);
    }
    if (thread != nullptr && thread->joinable()) {
      thread->join();
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_args(argc, argv);
    arc::I2cDevice i2c({options.i2c, options.address});
    arc::I2cDisplayTransport transport(i2c);
    arc::Ssd1306 display(transport);
    display.init();

    const arc::HttpRequest& endpoint = *options.api;
    const arc::HttpClient client(endpoint);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::cout << "Endpoint established\n";


   std::vector<arc::Button> buttons;
    buttons.reserve(options.buttons.size());
    for (const std::uint32_t pin : options.buttons) {
      buttons.emplace_back(arc::Button::Config{options.gpiochip, pin});
    }
    arc::PanelState state;
    state.show_feed = false;
    for (std::size_t index = 0; index < buttons.size(); ++index) {
      const arc::ButtonEvent event =
          buttons[index].pressed() ? arc::ButtonEvent::Press : arc::ButtonEvent::Release;
      arc::apply_button_edge(index, event, state);
    }
    std::mutex state_mu;
    std::condition_variable state_cv;
    bool dirty = state.fault || state.show_feed;
    std::string latitude;
    std::string longitude;
    bool coordinates_dirty = false;
    int consecutive_failures = 0;
    bool reconnecting = false;
    arc::ButtonMonitor monitor(buttons, [&](std::size_t index, arc::ButtonEvent event) {
      std::lock_guard lock(state_mu);
      if (arc::apply_button_edge(index, event, state)) {
        dirty = true;
        state_cv.notify_one();
      }
    });
    std::atomic<bool> poll_stop{false};
    std::thread poll_thread([&] {
      auto note_failure = [&] {
        std::lock_guard lock(state_mu);
        if (consecutive_failures < kReconnectAfterFailures) {
          ++consecutive_failures;
        }
        if (consecutive_failures >= kReconnectAfterFailures && !reconnecting) {
          reconnecting = true;
          coordinates_dirty = true;
          state_cv.notify_one();
        }
      };
      auto note_success = [&](std::string next_latitude, std::string next_longitude) {
        std::lock_guard lock(state_mu);
        const bool recovered = reconnecting;
        consecutive_failures = 0;
        reconnecting = false;
        const bool changed = next_latitude != latitude || next_longitude != longitude;
        if (changed) {
          latitude = std::move(next_latitude);
          longitude = std::move(next_longitude);
          std::cout << latitude << "\n" << longitude << "\n";
          std::cout.flush();
        }
        if (changed || recovered) {
          coordinates_dirty = true;
          state_cv.notify_one();
        }
      };
      while (!poll_stop.load() && g_stop == 0) {
        const auto started = std::chrono::steady_clock::now();
        try {
          const arc::HttpResponse response = client.get();
          if (poll_stop.load() || g_stop != 0) {
            break;
          }
          if (response.status != 200) {
            std::cerr << "HTTP " << response.status << "\n";
            note_failure();
          } else {
            const arc::Json json_obj = arc::Json::parse(response.body);
            const arc::Json* latitude_value = json_obj.find("latitude");
            const arc::Json* longitude_value = json_obj.find("longitude");
            if (latitude_value == nullptr || longitude_value == nullptr) {
              std::cerr << "missing coordinates\n";
              note_failure();
            } else {
              note_success(latitude_value->dump(), longitude_value->dump());
            }
          }
        } catch (const std::exception& ex) {
          if (poll_stop.load() || g_stop != 0) {
            break;
          }
          std::cerr << ex.what() << "\n";
          note_failure();
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed < arc::kApiPollPeriod) {
          auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(arc::kApiPollPeriod - elapsed);
          while (!poll_stop.load() && g_stop == 0 && remaining.count() > 0) {
            const auto slice = std::min(remaining, std::chrono::milliseconds(200));
            std::this_thread::sleep_for(slice);
            remaining -= slice;
          }
        }
      }
    });
    PollStop poll_join{&poll_stop, &poll_thread};
    std::cout << "Buttons established\n";
    std::cout << "Waiting for presses on GPIO " << options.buttons[0] << " and " << options.buttons[1]
              << " (Ctrl-C to exit)\n";
    std::cout << "Polling " << (endpoint.tls ? "https://" : "http://") << endpoint.host << ":" << endpoint.port
              << endpoint.target << " every 2 seconds (Ctrl-C to exit)\n";

    if (!state.fault && !state.show_feed) {
      display.clear_window();
    }
    while (g_stop == 0) {
      monitor.rethrow_if_error();
      arc::PanelState snapshot;
      std::string shown_latitude;
      std::string shown_longitude;
      bool shown_reconnecting = false;
      bool buttons_changed = false;
      bool coordinates_changed = false;
      {
        std::unique_lock lock(state_mu);
        state_cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return dirty || coordinates_dirty; });
        if (g_stop != 0) {
          break;
        }
        if (!dirty && !coordinates_dirty) {
          continue;
        }
        snapshot = state;
        shown_latitude = latitude;
        shown_longitude = longitude;
        shown_reconnecting = reconnecting;
        buttons_changed = dirty;
        coordinates_changed = coordinates_dirty;
        dirty = false;
        coordinates_dirty = false;
      }

      if (snapshot.fault) {
        display.set_fault(true);
        if (buttons_changed) {
          display.clear();
          display.draw_text(10, 10, "!! ERROR !!");
          display.flush();
        }
      } else if (snapshot.show_feed) {
        display.set_fault(false);
        if (buttons_changed || coordinates_changed) {
          if (shown_reconnecting) {
            draw_reconnecting(display);
          } else {
            draw_coordinates(display, shown_latitude, shown_longitude);
          }
        }
      } else if (buttons_changed) {
        display.set_fault(false);
        display.clear_window();
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
