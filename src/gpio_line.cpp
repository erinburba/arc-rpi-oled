#include "arc/gpio_input.hpp"
#include "arc/gpio_output.hpp"

#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace arc {
namespace {

[[noreturn]] void throw_error(int err, const std::string& what) {
  throw std::system_error(err, std::generic_category(), what);
}

int request_line(const std::string& chip, std::uint32_t offset, const gpio_v2_line_config& config) {
  const int chip_fd = ::open(chip.c_str(), O_RDONLY | O_CLOEXEC);
  if (chip_fd < 0) {
    const int err = errno;
    throw_error(err, "open " + chip);
  }

  gpio_v2_line_request request{};
  request.offsets[0] = offset;
  request.num_lines = 1;
  constexpr char kConsumer[] = "arc-rpi-oled";
  static_assert(sizeof(kConsumer) <= sizeof(request.consumer));
  std::memcpy(request.consumer, kConsumer, sizeof(kConsumer));
  request.config = config;

  if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
    const int err = errno;
    ::close(chip_fd);
    throw_error(err, "request GPIO line " + std::to_string(offset) + " on " + chip);
  }
  ::close(chip_fd);

  if (request.fd < 0) {
    throw std::runtime_error("GPIO line request returned no file descriptor");
  }
  const int flags = ::fcntl(request.fd, F_GETFD);
  if (flags >= 0) {
    ::fcntl(request.fd, F_SETFD, flags | FD_CLOEXEC);
  }
  return request.fd;
}

gpio_v2_line_config output_config(bool initial_level) {
  gpio_v2_line_config config{};
  config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
  config.num_attrs = 1;
  config.attrs[0].mask = 1;
  config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
  config.attrs[0].attr.values = initial_level ? 1ULL : 0ULL;
  return config;
}

std::uint64_t input_flags(Bias bias, Edge edge) {
  std::uint64_t flags = GPIO_V2_LINE_FLAG_INPUT;
  switch (bias) {
    case Bias::Disabled:
      flags |= GPIO_V2_LINE_FLAG_BIAS_DISABLED;
      break;
    case Bias::PullUp:
      flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
      break;
    case Bias::PullDown:
      flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
      break;
  }
  switch (edge) {
    case Edge::None:
      break;
    case Edge::Rising:
      flags |= GPIO_V2_LINE_FLAG_EDGE_RISING;
      break;
    case Edge::Falling:
      flags |= GPIO_V2_LINE_FLAG_EDGE_FALLING;
      break;
    case Edge::Both:
      flags |= GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
      break;
  }
  return flags;
}

}  // namespace

GpioOutput::GpioOutput(std::string chip_path, std::uint32_t offset, bool initial_level)
    : fd_(request_line(chip_path, offset, output_config(initial_level))) {}

GpioOutput::~GpioOutput() { close_fd(); }

GpioOutput::GpioOutput(GpioOutput&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

GpioOutput& GpioOutput::operator=(GpioOutput&& other) noexcept {
  if (this != &other) {
    close_fd();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

void GpioOutput::set(bool level) {
  if (fd_ < 0) {
    throw std::logic_error("set on a moved-from GPIO output");
  }
  gpio_v2_line_values values{};
  values.mask = 1;
  values.bits = level ? 1ULL : 0ULL;
  if (ioctl(fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0) {
    const int err = errno;
    throw_error(err, "GPIO set");
  }
}

void GpioOutput::close_fd() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

GpioInput::GpioInput(std::string chip_path, std::uint32_t offset, Bias bias, Edge edge) : edge_(edge) {
  gpio_v2_line_config config{};
  config.flags = input_flags(bias, edge);
  fd_ = request_line(chip_path, offset, config);
}

GpioInput::~GpioInput() { close_fd(); }

GpioInput::GpioInput(GpioInput&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), edge_(other.edge_) {}

GpioInput& GpioInput::operator=(GpioInput&& other) noexcept {
  if (this != &other) {
    close_fd();
    fd_ = std::exchange(other.fd_, -1);
    edge_ = other.edge_;
  }
  return *this;
}

int GpioInput::poll_fd() const {
  if (fd_ < 0) {
    throw std::logic_error("poll on a moved-from GPIO input");
  }
  return fd_;
}

bool GpioInput::get() const {
  if (fd_ < 0) {
    throw std::logic_error("get on a moved-from GPIO input");
  }
  gpio_v2_line_values values{};
  values.mask = 1;
  if (ioctl(fd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) {
    const int err = errno;
    throw_error(err, "GPIO get");
  }
  return (values.bits & 1ULL) != 0;
}

std::optional<Edge> GpioInput::wait(std::chrono::milliseconds timeout) const {
  if (edge_ == Edge::None) {
    throw std::logic_error("GpioInput::wait requires an edge");
  }
  if (fd_ < 0) {
    throw std::logic_error("wait on a moved-from GPIO input");
  }
  if (timeout.count() < 0 || timeout.count() > 2'147'483'647) {
    throw std::invalid_argument("GPIO wait timeout is out of range");
  }

  pollfd poll_fd{};
  poll_fd.fd = fd_;
  poll_fd.events = POLLIN;
  const int ready = ::poll(&poll_fd, 1, static_cast<int>(timeout.count()));
  if (ready < 0) {
    if (errno == EINTR) {
      return std::nullopt;
    }
    const int err = errno;
    throw_error(err, "GPIO wait");
  }
  if (ready == 0) {
    return std::nullopt;
  }

  gpio_v2_line_event event{};
  const ssize_t n = ::read(fd_, &event, sizeof(event));
  if (n < 0) {
    if (errno == EINTR) {
      return std::nullopt;
    }
    const int err = errno;
    throw_error(err, "GPIO read event");
  }
  if (n != static_cast<ssize_t>(sizeof(event))) {
    throw std::runtime_error("short GPIO event read");
  }
  if (event.id == GPIO_V2_LINE_EVENT_RISING_EDGE) {
    return Edge::Rising;
  }
  if (event.id == GPIO_V2_LINE_EVENT_FALLING_EDGE) {
    return Edge::Falling;
  }
  throw std::runtime_error("unexpected GPIO event id");
}

void GpioInput::close_fd() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace arc
