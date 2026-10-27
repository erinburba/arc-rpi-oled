#pragma once

#include <cstdint>
#include <string>

namespace arc {

// One GPIO v2 output line. The chip path and line offset are requested from
// the kernel and released in the destructor.
class GpioOutput {
 public:
  GpioOutput(std::string chip_path, std::uint32_t offset, bool initial_level);
  ~GpioOutput();

  GpioOutput(const GpioOutput&) = delete;
  GpioOutput& operator=(const GpioOutput&) = delete;
  GpioOutput(GpioOutput&& other) noexcept;
  GpioOutput& operator=(GpioOutput&& other) noexcept;

  void set(bool level);

 private:
  int fd_ = -1;

  void close_fd() noexcept;
};

}  // namespace arc
