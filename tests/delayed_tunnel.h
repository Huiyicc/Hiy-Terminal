#ifndef HIYUI_TERMINAL_TEST_DELAYED_TUNNEL_H
#define HIYUI_TERMINAL_TEST_DELAYED_TUNNEL_H

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <httplib.h>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

class DelayedTunnel final : public httplib::Server {
  int upstreamPort_;
  int port_;
  std::chrono::milliseconds delay_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<socket_t> sockets_;
  bool stopping_ = false;
  bool forwarded_ = false;
  std::thread listener_;

  void relay(socket_t source, socket_t target) {
    httplib::detail::SocketStream input(source, -1, 0, 5, 0);
    httplib::detail::SocketStream output(target, -1, 0, 5, 0);
    std::array<char, 16384> data;
    while (true) {
      const auto count = input.read(data.data(), data.size());
      if (count <= 0 || output.write(data.data(), 1) != 1)
        break;
      {
        std::unique_lock lock(mutex_);
        forwarded_ = true;
        wake_.notify_all();
        if (wake_.wait_for(lock, delay_, [this] { return stopping_; }))
          break;
      }
      size_t sent = 1;
      while (sent < static_cast<size_t>(count)) {
        const auto size = output.write(data.data() + sent, count - sent);
        if (size <= 0)
          return;
        sent += size;
      }
    }
  }

  bool process_and_close_socket(socket_t downstream) override {
    const auto upstream = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(upstreamPort_));
    if (upstream == INVALID_SOCKET ||
        ::connect(upstream, reinterpret_cast<sockaddr *>(&address),
                  sizeof(address)) != 0) {
      if (upstream != INVALID_SOCKET)
        httplib::detail::close_socket(upstream);
      httplib::detail::close_socket(downstream);
      return false;
    }
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        httplib::detail::close_socket(upstream);
        httplib::detail::close_socket(downstream);
        return false;
      }
      sockets_.push_back(upstream);
      sockets_.push_back(downstream);
    }
    std::thread upload([&] {
      relay(downstream, upstream);
      httplib::detail::shutdown_socket(upstream);
      httplib::detail::shutdown_socket(downstream);
    });
    relay(upstream, downstream);
    httplib::detail::shutdown_socket(upstream);
    httplib::detail::shutdown_socket(downstream);
    upload.join();
    std::lock_guard lock(mutex_);
    std::erase(sockets_, upstream);
    std::erase(sockets_, downstream);
    httplib::detail::close_socket(upstream);
    httplib::detail::close_socket(downstream);
    return true;
  }

public:
  explicit DelayedTunnel(int upstreamPort, std::chrono::milliseconds delay)
      : upstreamPort_(upstreamPort), delay_(delay) {
    port_ = bind_to_any_port("127.0.0.1");
    if (port_ <= 0)
      throw std::runtime_error("TCP tunnel could not bind");
    listener_ = std::thread([this] { listen_after_bind(); });
    wait_until_ready();
  }
  ~DelayedTunnel() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      for (const auto socket : sockets_)
        httplib::detail::shutdown_socket(socket);
    }
    wake_.notify_all();
    stop();
    listener_.join();
  }
  std::string endpoint() const {
    return "ws://127.0.0.1:" + std::to_string(port_) + "/terminal";
  }
  bool waitForTraffic() {
    std::unique_lock lock(mutex_);
    return wake_.wait_for(lock, std::chrono::seconds(3),
                          [this] { return forwarded_; });
  }
};

#endif
