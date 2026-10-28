#include "arc/button.hpp"

#include <stdexcept>

namespace arc {

ButtonDebounce::ButtonDebounce(std::chrono::milliseconds interval, bool initially_pressed)
    : interval_(interval), stable_(initially_pressed) {
  if (interval.count() < 0) {
    throw std::invalid_argument("button debounce interval is negative");
  }
}

bool apply_button_edge(std::size_t index, ButtonEvent event, PanelState& state) {
  const bool pressed = event == ButtonEvent::Press;
  if (index == FAULT_BUTTON_INDEX) {
    if (state.inverted == pressed) {
      return false;
    }
    state.inverted = pressed;
    return true;
  }
  if (index == FEED_BUTTON_INDEX) {
    if (state.show_block == pressed) {
      return false;
    }
    state.show_block = pressed;
    return true;
  }
  return false;
}

bool ButtonDebounce::pressed() const { return stable_; }

std::chrono::milliseconds ButtonDebounce::interval() const { return interval_; }

std::optional<ButtonEvent> ButtonDebounce::observe(bool raw_pressed,
                                                    std::chrono::steady_clock::time_point now) {
  if (raw_pressed == stable_) {
    candidate_valid_ = false;
    return std::nullopt;
  }
  if (!candidate_valid_ || candidate_ != raw_pressed) {
    candidate_valid_ = true;
    candidate_ = raw_pressed;
    candidate_since_ = now;
    return std::nullopt;
  }
  if (now - candidate_since_ < interval_) {
    return std::nullopt;
  }
  stable_ = raw_pressed;
  candidate_valid_ = false;
  return raw_pressed ? ButtonEvent::Press : ButtonEvent::Release;
}

}  // namespace arc
