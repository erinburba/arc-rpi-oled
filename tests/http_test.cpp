#include "arc/http_client.hpp"
#include "arc/json.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

int g_failures = 0;

void expect(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    std::cerr << file << ":" << line << ": " << expression << "\n";
    ++g_failures;
  }
}

#define EXPECT(condition) expect(static_cast<bool>(condition), #condition, __FILE__, __LINE__)

struct Socket {
  int fd = -1;
  explicit Socket(int owned = -1) : fd(owned) {}
  ~Socket() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
};

void write_all(int fd, std::string_view text) {
  std::size_t sent = 0;
  while (sent < text.size()) {
    const ssize_t n = ::send(fd, text.data() + sent, text.size() - sent, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("peer write failed");
    }
    sent += static_cast<std::size_t>(n);
  }
}

std::string read_headers(int fd) {
  std::string buffered;
  while (buffered.find("\r\n\r\n") == std::string::npos) {
    pollfd ready{fd, POLLIN, 0};
    if (::poll(&ready, 1, 3000) <= 0) {
      throw std::runtime_error("peer headers timed out");
    }
    char chunk[512];
    const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      throw std::runtime_error("peer headers failed");
    }
    buffered.append(chunk, static_cast<std::size_t>(n));
  }
  return buffered;
}

Socket listen_local(std::uint16_t& port) {
  Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
  if (listener.fd < 0) {
    throw std::runtime_error("socket");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
      ::listen(listener.fd, 1) < 0) {
    throw std::runtime_error("listen");
  }
  socklen_t length = sizeof(address);
  if (::getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
    throw std::runtime_error("getsockname");
  }
  port = ntohs(address.sin_port);
  return listener;
}

Socket accept_one(int listener) {
  pollfd ready{listener, POLLIN, 0};
  if (::poll(&ready, 1, 3000) <= 0) {
    throw std::runtime_error("accept timed out");
  }
  const int fd = ::accept(listener, nullptr, nullptr);
  if (fd < 0) {
    throw std::runtime_error("accept");
  }
  return Socket(fd);
}

void test_url() {
  const arc::HttpRequest home = arc::parse_http_url("http://panel.local/status");
  EXPECT(home.host == "panel.local");
  EXPECT(home.port == 80);
  EXPECT(home.target == "/status");
  EXPECT(!home.tls);

  const arc::HttpRequest secure = arc::parse_http_url("https://panel.local/status?x=1");
  EXPECT(secure.host == "panel.local");
  EXPECT(secure.port == 443);
  EXPECT(secure.target == "/status?x=1");
  EXPECT(secure.tls);

  const arc::HttpRequest explicit_port = arc::parse_http_url("http://127.0.0.1:8080");
  EXPECT(explicit_port.port == 8080);
  EXPECT(explicit_port.target == "/");
}

void test_get_json() {
  std::uint16_t port = 0;
  Socket listener = listen_local(port);
  std::exception_ptr error;
  std::thread server([&] {
    try {
      Socket peer = accept_one(listener.fd);
      const std::string request = read_headers(peer.fd);
      if (request.find("GET /status HTTP/1.1\r\n") != 0) {
        throw std::runtime_error("unexpected request");
      }
      const std::string body = R"({"ok":true,"n":2})";
      write_all(peer.fd, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    } catch (...) {
      error = std::current_exception();
    }
  });
  struct Join {
    std::thread& thread;
    ~Join() {
      if (thread.joinable()) {
        thread.join();
      }
    }
  } join{server};

  const arc::HttpClient client(arc::parse_http_url("http://127.0.0.1:" + std::to_string(port) + "/status"));
  const arc::HttpResponse response = client.get();
  EXPECT(response.status == 200);
  const arc::Json json = arc::Json::parse(response.body);
  EXPECT(json.find("ok")->boolean());
  EXPECT(json.find("n")->number() == "2");
  server.join();
  if (error) {
    std::rethrow_exception(error);
  }
}

void test_chunked_json() {
  std::uint16_t port = 0;
  Socket listener = listen_local(port);
  std::exception_ptr error;
  std::thread server([&] {
    try {
      Socket peer = accept_one(listener.fd);
      read_headers(peer.fd);
      write_all(peer.fd,
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                "7\r\n{\"ok\": \r\n5\r\ntrue}\r\n0\r\n\r\n");
    } catch (...) {
      error = std::current_exception();
    }
  });
  struct Join {
    std::thread& thread;
    ~Join() {
      if (thread.joinable()) {
        thread.join();
      }
    }
  } join{server};

  const arc::HttpClient client(arc::parse_http_url("http://127.0.0.1:" + std::to_string(port) + "/status"));
  const arc::HttpResponse response = client.get();
  EXPECT(response.status == 200);
  EXPECT(arc::Json::parse(response.body).find("ok")->boolean());
  server.join();
  if (error) {
    std::rethrow_exception(error);
  }
}

}  // namespace

int main() {
  test_url();
  try {
    test_get_json();
    test_chunked_json();
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << "\n";
    return EXIT_FAILURE;
  }
  if (g_failures != 0) {
    std::cerr << g_failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "ok\n";
  return EXIT_SUCCESS;
}
