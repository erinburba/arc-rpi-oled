#pragma once

#include "arc/gpio_input.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace arc {

enum class ButtonEvent {
  Press,
  Release,
};

// A button wired to ground is active low: the line reads low while it is held.
inline bool level_is_pressed(bool line_high, bool active_low) {
  return active_low ? !line_high : line_high;
}

// Debounce state with no GPIO access, so it can be tested on the host.
class ButtonDebounce {
 public:
  ButtonDebounce(std::chrono::milliseconds interval, bool initially_pressed);

  bool pressed() const;
  std::chrono::milliseconds interval() const;

  // raw_pressed is already converted from the pin level. A new level becomes
  // stable only after it has been observed for the debounce interval.
  std::optional<ButtonEvent> observe(bool raw_pressed, std::chrono::steady_clock::time_point now);

 private:
  std::chrono::milliseconds interval_;
  bool stable_ = false;
  bool candidate_valid_ = false;
  bool candidate_ = false;
  std::chrono::steady_clock::time_point candidate_since_{};
};

// One GPIO button. Active-low with the internal pull-up matches a switch to ground.
class Button {
 public:
  struct Config {
    std::string chip = "/dev/gpiochip0";
    std::uint32_t offset = 23;
    bool active_low = true;
    std::chrono::milliseconds debounce{25};
  };

  explicit Button(Config config);

  Button(const Button&) = delete;
  Button& operator=(const Button&) = delete;
  Button(Button&&) noexcept;
  Button& operator=(Button&&) noexcept;

  bool pressed();
  std::optional<ButtonEvent> wait(std::chrono::milliseconds timeout);

 private:
  GpioInput input_;
  bool active_low_ = true;
  ButtonDebounce debounce_;

  bool raw_pressed() const;
};

}  // namespace arc
