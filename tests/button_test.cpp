#include "arc/button.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

int g_failures = 0;

void expect(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    std::cerr << file << ":" << line << ": " << expression << "\n";
    ++g_failures;
  }
}

#define EXPECT(condition) expect(static_cast<bool>(condition), #condition, __FILE__, __LINE__)

using clock = std::chrono::steady_clock;

clock::time_point at(int milliseconds) { return clock::time_point{} + std::chrono::milliseconds(milliseconds); }

void test_active_level() {
  EXPECT(arc::level_is_pressed(false, true));
  EXPECT(!arc::level_is_pressed(true, true));
  EXPECT(arc::level_is_pressed(true, false));
  EXPECT(!arc::level_is_pressed(false, false));
}

void test_press_and_release() {
  arc::ButtonDebounce debounce(std::chrono::milliseconds(25), false);
  EXPECT(!debounce.pressed());
  EXPECT(!debounce.observe(true, at(0)).has_value());
  EXPECT(!debounce.observe(true, at(24)).has_value());
  EXPECT(!debounce.pressed());

  const std::optional<arc::ButtonEvent> press = debounce.observe(true, at(25));
  EXPECT(press == arc::ButtonEvent::Press);
  EXPECT(debounce.pressed());

  EXPECT(!debounce.observe(false, at(40)).has_value());
  const std::optional<arc::ButtonEvent> release = debounce.observe(false, at(65));
  EXPECT(release == arc::ButtonEvent::Release);
  EXPECT(!debounce.pressed());
}

void test_bounce_is_ignored() {
  arc::ButtonDebounce debounce(std::chrono::milliseconds(25), false);
  EXPECT(!debounce.observe(true, at(0)).has_value());
  EXPECT(!debounce.observe(false, at(10)).has_value());
  EXPECT(!debounce.pressed());
  EXPECT(!debounce.observe(false, at(50)).has_value());
  EXPECT(!debounce.pressed());

  EXPECT(!debounce.observe(true, at(100)).has_value());
  const std::optional<arc::ButtonEvent> press = debounce.observe(true, at(130));
  EXPECT(press == arc::ButtonEvent::Press);
  EXPECT(debounce.pressed());
}

void test_edges_update_panel_state() {
  arc::PanelState state;
  EXPECT(!state.fault);
  EXPECT(state.show_feed);

  EXPECT(arc::apply_button_edge(0, arc::ButtonEvent::Press, state));
  EXPECT(state.fault);
  EXPECT(!arc::apply_button_edge(0, arc::ButtonEvent::Press, state));

  EXPECT(arc::apply_button_edge(0, arc::ButtonEvent::Release, state));
  EXPECT(!state.fault);

  EXPECT(!arc::apply_button_edge(1, arc::ButtonEvent::Press, state));
  EXPECT(state.show_feed);
  EXPECT(arc::apply_button_edge(1, arc::ButtonEvent::Release, state));
  EXPECT(!state.show_feed);
  EXPECT(!state.fault);

  EXPECT(!arc::apply_button_edge(2, arc::ButtonEvent::Press, state));
  EXPECT(!state.show_feed);
}

void test_initial_press_can_release() {
  arc::ButtonDebounce debounce(std::chrono::milliseconds(20), true);
  EXPECT(debounce.pressed());
  EXPECT(!debounce.observe(false, at(0)).has_value());
  const std::optional<arc::ButtonEvent> release = debounce.observe(false, at(20));
  EXPECT(release == arc::ButtonEvent::Release);
  EXPECT(!debounce.pressed());
}

}  // namespace

int main() {
  test_active_level();
  test_press_and_release();
  test_bounce_is_ignored();
  test_edges_update_panel_state();
  test_initial_press_can_release();
  if (g_failures != 0) {
    std::cerr << g_failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "ok\n";
  return EXIT_SUCCESS;
}
