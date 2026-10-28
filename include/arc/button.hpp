#pragma once

#include "arc/gpio_input.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

#define FAULT_BUTTON 23
#define FEED_BUTTON 22
#define FAULT_BUTTON_INDEX 0
#define FEED_BUTTON_INDEX 1

namespace arc {

enum class ButtonEvent {
  Press,
  Release,
};

struct IdentifiedButtonEvent {
  std::size_t index = 0;
  ButtonEvent event = ButtonEvent::Press;
};

// What the panel is showing. Button edges write these; drawing reads them.
struct PanelState {
  bool fault = false;
  bool show_feed = true;
};

// Button 0 selects invert while it is held. Button 1 selects the filled block
// while it is held. A release clears that choice. Returns true when state changes.
bool apply_button_edge(std::size_t index, ButtonEvent event, PanelState& state);

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
  friend std::optional<IdentifiedButtonEvent> wait_any(std::span<Button> buttons,
                                                       std::chrono::milliseconds timeout);

  friend class ButtonMonitor;

  int poll_fd() const;
  void drain_edges();
  std::optional<ButtonEvent> observe_now();
  std::chrono::milliseconds debounce_interval() const;

  GpioInput input_;
  bool active_low_ = true;
  ButtonDebounce debounce_;

  bool raw_pressed() const;
};

// Wait until one button emits a debounced press or release. index matches the span order.
std::optional<IdentifiedButtonEvent> wait_any(std::span<Button> buttons,
                                               std::chrono::milliseconds timeout);

// Blocks in poll on each button's GPIO edge file. The line is armed for rising
// and falling edges. A debounced press or release runs handler on this thread.
class ButtonMonitor {
 public:
  using Handler = std::function<void(std::size_t index, ButtonEvent event)>;

  ButtonMonitor(std::span<Button> buttons, Handler handler);
  ~ButtonMonitor();

  ButtonMonitor(const ButtonMonitor&) = delete;
  ButtonMonitor& operator=(const ButtonMonitor&) = delete;

  void rethrow_if_error();

 private:
  void run();

  std::span<Button> buttons_;
  Handler handler_;
  std::atomic<bool> stop_{false};
  std::mutex error_mu_;
  std::exception_ptr error_;
  std::thread thread_;
};

}  // namespace arc
