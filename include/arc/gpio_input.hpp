#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace arc {

enum class Bias {
  Disabled,
  PullUp,
  PullDown,
};

enum class Edge {
  None,
  Rising,
  Falling,
  Both,
};

// One GPIO v2 input line. wait() is only valid when an edge was requested;
// the optional it returns is Rising or Falling for the event that arrived.
class GpioInput {
 public:
  GpioInput(std::string chip_path, std::uint32_t offset, Bias bias = Bias::PullUp,
            Edge edge = Edge::None);
  ~GpioInput();

  GpioInput(const GpioInput&) = delete;
  GpioInput& operator=(const GpioInput&) = delete;
  GpioInput(GpioInput&& other) noexcept;
  GpioInput& operator=(GpioInput&& other) noexcept;

  bool get() const;
  std::optional<Edge> wait(std::chrono::milliseconds timeout) const;
  // One queued rising or falling event. nullopt when the queue is empty.
  std::optional<Edge> read_edge() const;
  int poll_fd() const;

 private:
  int fd_ = -1;
  Edge edge_ = Edge::None;

  void close_fd() noexcept;
};

}  // namespace arc
