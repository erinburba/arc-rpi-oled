#include "arc/button.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace arc {

Button::Button(Config config)
    : input_(std::move(config.chip), config.offset,
             config.active_low ? Bias::PullUp : Bias::PullDown, Edge::Both),
      active_low_(config.active_low),
      debounce_(config.debounce, level_is_pressed(input_.get(), config.active_low)) {}

Button::Button(Button&& other) noexcept
    : input_(std::move(other.input_)),
      active_low_(other.active_low_),
      debounce_(std::move(other.debounce_)) {}

Button& Button::operator=(Button&& other) noexcept {
  if (this != &other) {
    input_ = std::move(other.input_);
    active_low_ = other.active_low_;
    debounce_ = std::move(other.debounce_);
  }
  return *this;
}

bool Button::pressed() {
  debounce_.observe(raw_pressed(), std::chrono::steady_clock::now());
  return debounce_.pressed();
}

std::optional<ButtonEvent> Button::wait(std::chrono::milliseconds timeout) {
  if (timeout.count() < 0) {
    throw std::invalid_argument("button wait timeout is out of range");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return debounce_.observe(raw_pressed(), now);
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto slice = std::min(debounce_.interval(), remaining);
    input_.wait(slice);
    if (const std::optional<ButtonEvent> event =
            debounce_.observe(raw_pressed(), std::chrono::steady_clock::now())) {
      return event;
    }
  }
}

bool Button::raw_pressed() const { return level_is_pressed(input_.get(), active_low_); }

}  // namespace arc
