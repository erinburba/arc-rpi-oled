#include "arc/spi_device.hpp"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace arc {
namespace {

[[noreturn]] void throw_error(int err, const std::string& what) {
  throw std::system_error(err, std::generic_category(), what);
}

void ioctl_or_throw(int fd, unsigned long request, void* arg, const std::string& what) {
  if (ioctl(fd, request, arg) < 0) {
    const int err = errno;
    throw_error(err, what);
  }
}

void check_fits_u32(std::size_t size) {
  if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
    if (size > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
      throw std::invalid_argument("SPI transfer is longer than 2^32-1 bytes");
    }
  }
}

}  // namespace

SpiDevice::SpiDevice(Config config) : speed_hz_(config.speed_hz), bits_per_word_(config.bits_per_word) {
  fd_ = ::open(config.device.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0) {
    const int err = errno;
    throw_error(err, "open " + config.device);
  }

  try {
    std::uint8_t mode = config.mode;
    std::uint8_t bits = config.bits_per_word;
    std::uint32_t speed = config.speed_hz;
    ioctl_or_throw(fd_, SPI_IOC_WR_MODE, &mode, "set SPI mode on " + config.device);
    ioctl_or_throw(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits, "set SPI bits per word on " + config.device);
    ioctl_or_throw(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed, "set SPI speed on " + config.device);
  } catch (...) {
    close_fd();
    throw;
  }
}

SpiDevice::~SpiDevice() { close_fd(); }

SpiDevice::SpiDevice(SpiDevice&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      speed_hz_(other.speed_hz_),
      bits_per_word_(other.bits_per_word_) {}

SpiDevice& SpiDevice::operator=(SpiDevice&& other) noexcept {
  if (this != &other) {
    close_fd();
    fd_ = std::exchange(other.fd_, -1);
    speed_hz_ = other.speed_hz_;
    bits_per_word_ = other.bits_per_word_;
  }
  return *this;
}

void SpiDevice::write(std::span<const std::uint8_t> tx) {
  if (tx.empty()) {
    return;
  }
  if (fd_ < 0) {
    throw std::logic_error("write on a moved-from SPI device");
  }
  check_fits_u32(tx.size());

  spi_ioc_transfer transfer{};
  transfer.tx_buf = reinterpret_cast<std::uint64_t>(tx.data());
  transfer.len = static_cast<std::uint32_t>(tx.size());
  transfer.speed_hz = speed_hz_;
  transfer.bits_per_word = bits_per_word_;
  ioctl_or_throw(fd_, SPI_IOC_MESSAGE(1), &transfer, "SPI write");
}

void SpiDevice::transfer(std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx) {
  if (tx.size() != rx.size()) {
    throw std::invalid_argument("SPI transfer tx and rx lengths differ");
  }
  if (tx.empty()) {
    return;
  }
  if (fd_ < 0) {
    throw std::logic_error("transfer on a moved-from SPI device");
  }
  check_fits_u32(tx.size());

  spi_ioc_transfer transfer{};
  transfer.tx_buf = reinterpret_cast<std::uint64_t>(tx.data());
  transfer.rx_buf = reinterpret_cast<std::uint64_t>(rx.data());
  transfer.len = static_cast<std::uint32_t>(tx.size());
  transfer.speed_hz = speed_hz_;
  transfer.bits_per_word = bits_per_word_;
  ioctl_or_throw(fd_, SPI_IOC_MESSAGE(1), &transfer, "SPI transfer");
}

void SpiDevice::close_fd() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace arc
