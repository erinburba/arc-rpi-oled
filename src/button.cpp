#include "arc/button.hpp"

#include <poll.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

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

int Button::poll_fd() const { return input_.poll_fd(); }

void Button::drain_edges() {
  while (input_.read_edge().has_value()) {
  }
}

std::optional<ButtonEvent> Button::observe_now() {
  return debounce_.observe(raw_pressed(), std::chrono::steady_clock::now());
}

std::chrono::milliseconds Button::debounce_interval() const { return debounce_.interval(); }

std::optional<IdentifiedButtonEvent> wait_any(std::span<Button> buttons,
                                               std::chrono::milliseconds timeout) {
  if (buttons.empty()) {
    throw std::invalid_argument("wait_any requires at least one button");
  }
  if (timeout.count() < 0 || timeout.count() > 2'147'483'647) {
    throw std::invalid_argument("button wait timeout is out of range");
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::vector<pollfd> poll_fds(buttons.size());
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      for (std::size_t index = 0; index < buttons.size(); ++index) {
        if (const std::optional<ButtonEvent> event = buttons[index].observe_now()) {
          return IdentifiedButtonEvent{index, *event};
        }
      }
      return std::nullopt;
    }

    auto slice = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    for (Button& button : buttons) {
      slice = std::min(slice, button.debounce_interval());
    }
    for (std::size_t index = 0; index < buttons.size(); ++index) {
      poll_fds[index].fd = buttons[index].poll_fd();
      poll_fds[index].events = POLLIN;
      poll_fds[index].revents = 0;
    }
    const int ready = ::poll(poll_fds.data(), poll_fds.size(), static_cast<int>(slice.count()));
    if (ready < 0) {
      if (errno == EINTR) {
        return std::nullopt;
      }
      throw std::system_error(errno, std::generic_category(), "button wait");
    }
    for (std::size_t index = 0; index < buttons.size(); ++index) {
      if ((poll_fds[index].revents & POLLIN) != 0) {
        buttons[index].drain_edges();
      }
      if (const std::optional<ButtonEvent> event = buttons[index].observe_now()) {
        return IdentifiedButtonEvent{index, *event};
      }
    }
  }
}

ButtonMonitor::ButtonMonitor(std::span<Button> buttons, Handler handler)
    : buttons_(buttons), handler_(std::move(handler)) {
  if (buttons_.empty()) {
    throw std::invalid_argument("button monitor requires at least one button");
  }
  if (!handler_) {
    throw std::invalid_argument("button monitor requires a handler");
  }
  thread_ = std::thread(&ButtonMonitor::run, this);
}

ButtonMonitor::~ButtonMonitor() {
  stop_.store(true, std::memory_order_relaxed);
  if (thread_.joinable()) {
    thread_.join();
  }
}

void ButtonMonitor::rethrow_if_error() {
  std::lock_guard lock(error_mu_);
  if (error_) {
    std::rethrow_exception(error_);
  }
}

void ButtonMonitor::run() {
  try {
    std::vector<pollfd> poll_fds(buttons_.size());
    while (!stop_.load(std::memory_order_relaxed)) {
      int timeout_ms = 200;
      for (const Button& button : buttons_) {
        const auto interval = static_cast<int>(button.debounce_interval().count());
        timeout_ms = std::min(timeout_ms, std::max(interval, 0));
      }
      for (std::size_t index = 0; index < buttons_.size(); ++index) {
        poll_fds[index].fd = buttons_[index].poll_fd();
        poll_fds[index].events = POLLIN;
        poll_fds[index].revents = 0;
      }
      const int ready = ::poll(poll_fds.data(), poll_fds.size(), timeout_ms);
      if (stop_.load(std::memory_order_relaxed)) {
        return;
      }
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::system_error(errno, std::generic_category(), "button monitor");
      }
      for (std::size_t index = 0; index < buttons_.size(); ++index) {
        if ((poll_fds[index].revents & POLLIN) != 0) {
          buttons_[index].drain_edges();
        }
        if (const std::optional<ButtonEvent> event = buttons_[index].observe_now()) {
          handler_(index, *event);
        }
      }
    }
  } catch (...) {
    std::lock_guard lock(error_mu_);
    error_ = std::current_exception();
  }
}

}  // namespace arc
