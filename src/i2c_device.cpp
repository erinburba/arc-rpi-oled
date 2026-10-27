#include "arc/i2c_device.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace arc {
namespace {

[[noreturn]] void throw_error(int err, const std::string& what) {
  throw std::system_error(err, std::generic_category(), what);
}

}  // namespace

I2cDevice::I2cDevice(Config config) {
  if (config.address > 0x7F) {
    throw std::invalid_argument("I2C address must be a 7-bit value");
  }
  fd_ = ::open(config.device.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0) {
    const int err = errno;
    throw_error(err, "open " + config.device);
  }
  if (ioctl(fd_, I2C_SLAVE, static_cast<unsigned long>(config.address)) < 0) {
    const int err = errno;
    close_fd();
    throw_error(err, "set I2C address on " + config.device);
  }
}

I2cDevice::~I2cDevice() { close_fd(); }

I2cDevice::I2cDevice(I2cDevice&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

I2cDevice& I2cDevice::operator=(I2cDevice&& other) noexcept {
  if (this != &other) {
    close_fd();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

void I2cDevice::write(std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) {
    return;
  }
  if (fd_ < 0) {
    throw std::logic_error("write on a moved-from I2C device");
  }
  const ssize_t written = ::write(fd_, bytes.data(), bytes.size());
  if (written < 0) {
    const int err = errno;
    throw_error(err, "I2C write");
  }
  if (static_cast<std::size_t>(written) != bytes.size()) {
    throw std::runtime_error("short I2C write");
  }
}

void I2cDevice::close_fd() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace arc
