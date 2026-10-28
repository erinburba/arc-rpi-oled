#pragma once

#include <cstdint>
#include <span>

namespace arc {

class DisplayTransport {
 public:
  virtual ~DisplayTransport() = default;

  virtual void write_command(std::span<const std::uint8_t> bytes) = 0;
  virtual void write_data(std::span<const std::uint8_t> bytes) = 0;
  virtual void reset() = 0;
};

}  // namespace arc
