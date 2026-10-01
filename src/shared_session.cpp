#include "shared_session.h"
#include "share_protocol.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <httplib.h>
#include <thread>
#include <utility>

namespace terminal_app {
namespace {
using namespace std::chrono_literals;

bool authenticated(const httplib::Request &request, const std::string &token) {
  const auto value = request.get_header_value("Authorization");
  const auto expected = "Bearer " + token;
  if (value.size() != expected.size())
    return false;
  unsigned difference = 0;
  for (size_t i = 0; i < value.size(); ++i)
    difference |= static_cast<unsigned char>(value[i] ^ expected[i]);
  return difference == 0;
}

struct Peer {
  std::mutex mutex;
  std::condition_variable ready;
  std::shared_ptr<const std::string> frame;
  Permission permission = Permission::ReadOnly;
  bool permissionChanged = true;
  std::atomic<bool> stopped{false};

  void stop() {
    {
      std::lock_guard lock(mutex);
      stopped = true;
    }
    ready.notify_all();
  }
  void send(httplib::ws::WebSocket &ws) {
    while (!stopped) {
      std::shared_ptr<const std::string> next;
      bool changed;
      Permission granted;
      {
        std::unique_lock lock(mutex);
        ready.wait(lock,
                   [this] { return stopped || frame || permissionChanged; });
        if (stopped)
          break;
        next = std::exchange(frame, {});
        changed = std::exchange(permissionChanged, false);
        granted = permission;
      }
      const char access[] = {'P', static_cast<char>(granted)};
      if ((changed && !ws.send(access, sizeof(access))) ||
          (next && !ws.send(next->data(), next->size())))
        stop();
    }
    ws.close(httplib::ws::CloseStatus::GoingAway);
  }
};
}

struct ShareServer::Impl {
  std::shared_ptr<LocalSession> session;
  std::unique_ptr<httplib::Server> server;
  std::thread worker;
  std::mutex peersMutex;
  std::mutex policyMutex;
  std::vector<std::shared_ptr<Peer>> peers;
  std::shared_ptr<const std::string> latest;
  std::atomic<bool> stopped{true};
  std::atomic<Permission> access{Permission::ReadOnly};
  std::string token;
  std::string address;
  bool observing = false;
  int port = 0;

  explicit Impl(std::shared_ptr<LocalSession> value)
      : session(std::move(value)) {}

  void publish(ScreenFrame frame) {
    auto encoded = protocol::encodeFrame(frame);
    std::lock_guard lock(peersMutex);
    latest = std::make_shared<const std::string>(std::move(encoded));
    for (auto &peer : peers) {
      if (latest->empty()) {
        peer->stop();
        continue;
      }
      std::lock_guard peerLock(peer->mutex);
      peer->frame = latest;
      peer->ready.notify_one();
    }
  }

  void serve(httplib::ws::WebSocket &ws) {
    auto peer = std::make_shared<Peer>();
    bool accepted = false;
    {
      std::lock_guard lock(peersMutex);
      if (!stopped && peers.size() < 8 && latest && !latest->empty()) {
        peer->frame = latest;
        peer->permission = access;
        peers.push_back(peer);
        accepted = true;
      }
    }
    if (!accepted) {
      ws.close(httplib::ws::CloseStatus::PolicyViolation, "share unavailable");
      return;
    }
    ws.set_read_timeout(5s);
    std::thread sender([&ws, peer] { peer->send(ws); });
    std::string message;
    while (!stopped && !peer->stopped) {
      const auto result = ws.read(message);
      if (result == httplib::ws::Timeout)
        continue;
      if (result == httplib::ws::Fail)
        break;
      hiyui::TerminalInput input;
      if (result != httplib::ws::Binary ||
          !protocol::decodeInput(message, input))
        break;
      std::lock_guard lock(policyMutex);
      if (stopped || access != Permission::All || !session->input(input))
        break;
    }
    peer->stop();
    sender.join();
    std::lock_guard lock(peersMutex);
    std::erase(peers, peer);
  }
};

ShareServer::ShareServer(std::shared_ptr<LocalSession> session)
    : impl_(std::make_unique<Impl>(std::move(session))) {}
ShareServer::~ShareServer() { stop(); }

bool ShareServer::start(const ShareOptions &options, std::string &error) {
  auto &s = *impl_;
  if (!s.stopped) {
    error = "当前会话已经共享";
    return false;
  }
  if (!s.session || options.address.empty() || options.port < 0 ||
      options.port > 65535 ||
      (options.permission != Permission::ReadOnly &&
       options.permission != Permission::All)) {
    error = "监听地址、端口或权限无效";
    return false;
  }
  try {
    s.token = protocol::generateToken();
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
  s.access = options.permission;
  s.server = std::make_unique<httplib::Server>();
  s.server->set_socket_options([](auto socket) {
#ifdef _WIN32
    httplib::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
    httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
  });
  s.server->new_task_queue = [] { return new httplib::ThreadPool(4, 12, 16); };
  s.server->set_read_timeout(5, 0);
  s.server->set_write_timeout(5, 0);
  s.server->set_payload_max_length(1024);
  s.server->set_websocket_ping_interval(10);
  s.server->set_websocket_max_missed_pongs(2);
  s.server->set_pre_request_handler(
      [&s](const httplib::Request &req, httplib::Response &res) {
        if (req.path != protocol::Path) {
          res.status = 404;
          return httplib::Server::HandlerResponse::Handled;
        }
        if (req.has_header("Origin") || !authenticated(req, s.token)) {
          res.status = 401;
          return httplib::Server::HandlerResponse::Handled;
        }
        if (s.stopped ||
            req.get_header_value("Sec-WebSocket-Protocol") != protocol::Name) {
          res.status = 400;
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
      });
  s.server->WebSocket(
      protocol::Path,
      [&s](const httplib::Request &, httplib::ws::WebSocket &ws) {
        s.serve(ws);
      },
      [](const std::vector<std::string> &) {
        return std::string(protocol::Name);
      });
  s.port = options.port;
  const bool bound =
      options.port == 0
          ? (s.port = s.server->bind_to_any_port(options.address)) > 0
          : s.server->bind_to_port(options.address, options.port);
  if (!bound) {
    s.port = 0;
    error = "无法监听该地址或端口，端口可能已被占用";
    return false;
  }
  s.stopped = false;
  s.address = options.address;
  s.observing = true;
  s.session->observe([&s](ScreenFrame frame) { s.publish(std::move(frame)); });
  bool valid;
  {
    std::lock_guard lock(s.peersMutex);
    valid = s.latest && !s.latest->empty();
  }
  if (!valid) {
    error = "终端网格超过共享上限 512 × 128";
    stop();
    return false;
  }
  s.worker = std::thread([&s] { s.server->listen_after_bind(); });
  return true;
}

void ShareServer::stop() {
  auto &s = *impl_;
  {
    std::lock_guard lock(s.policyMutex);
    s.stopped = true;
  }
  if (std::exchange(s.observing, false))
    s.session->observe({});
  {
    std::lock_guard lock(s.peersMutex);
    for (auto &peer : s.peers)
      peer->stop();
  }
  if (s.server)
    s.server->stop();
  if (s.worker.joinable())
    s.worker.join();
  s.server.reset();
  s.latest.reset();
  s.token.clear();
  s.port = 0;
}

void ShareServer::permission(Permission value) {
  if (value != Permission::ReadOnly && value != Permission::All)
    return;
  auto &s = *impl_;
  std::lock_guard policy(s.policyMutex);
  s.access = value;
  std::lock_guard lock(s.peersMutex);
  for (auto &peer : s.peers) {
    std::lock_guard peerLock(peer->mutex);
    peer->permission = value;
    peer->permissionChanged = true;
    peer->ready.notify_one();
  }
}
Permission ShareServer::permission() const { return impl_->access; }
std::string ShareServer::token() const { return impl_->token; }
std::string ShareServer::address() const { return impl_->address; }
int ShareServer::port() const { return impl_->port; }

namespace {
class RemoteSession final : public TerminalSession {
  httplib::ws::WebSocketClient socket_;
  Receiver receiver_;
  std::thread worker_;
  std::atomic<bool> stopped_{false};
  std::atomic<bool> connected_{false};
  std::atomic<Permission> permission_{Permission::ReadOnly};
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::string> pending_;
  size_t pendingBytes_ = 0;

  void stopInput() {
    {
      std::lock_guard lock(mutex_);
      stopped_ = true;
      pending_.clear();
      pendingBytes_ = 0;
    }
    ready_.notify_all();
  }

  void send() {
    while (true) {
      std::string input;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return stopped_ || !pending_.empty(); });
        if (stopped_)
          return;
        input = std::move(pending_.front());
        pending_.pop_front();
        pendingBytes_ -= input.size();
      }
      if (permission_ == Permission::All &&
          !socket_.send(input.data(), input.size())) {
        socket_.close();
        return;
      }
    }
  }

  void run() {
    const auto connected = socket_.connect();
    if (!connected) {
      if (!stopped_)
        receiver_(
            {.type = TerminalEvent::Type::Error,
             .text =
                 connected.status() == 401
                     ? "认证失败，请检查共享令牌"
                     : "连接失败: " +
                           std::string(httplib::to_string(connected.error())) +
                           "（HTTP " + std::to_string(connected.status()) +
                           "）"});
      stopInput();
      return;
    }
    connected_ = true;
    std::thread sender([this] { send(); });
    bool welcomed = false;
    uint64_t revision = 0;
    std::string message;
    while (!stopped_) {
      const auto result = socket_.read(message);
      if (stopped_)
        break;
      if (result == httplib::ws::Timeout)
        continue;
      if (result == httplib::ws::Fail)
        break;
      if (result != httplib::ws::Binary)
        break;
      if (message.size() == 2 && message[0] == 'P' &&
          (message[1] == 0 || message[1] == 1)) {
        permission_ = static_cast<Permission>(message[1]);
        receiver_({.type = welcomed ? TerminalEvent::Type::PermissionChanged
                                    : TerminalEvent::Type::Connected,
                   .permission = permission_.load()});
        welcomed = true;
        continue;
      }
      ScreenFrame frame;
      if (!welcomed || !protocol::decodeFrame(message, frame) ||
          frame.revision <= revision)
        break;
      revision = frame.revision;
      receiver_({.type = TerminalEvent::Type::Screen,
                 .text = std::move(frame.screen),
                 .size = frame.size});
      if (!frame.running) {
        receiver_({.type = TerminalEvent::Type::Exited,
                   .text = "远端 shell 已退出",
                   .exitCode = frame.exitCode});
        stopped_ = true;
      }
    }
    if (!stopped_)
      receiver_({.type = TerminalEvent::Type::Exited, .text = "连接已断开"});
    stopInput();
    permission_ = Permission::ReadOnly;
    socket_.close();
    sender.join();
  }

public:
  RemoteSession(const std::string &endpoint, const std::string &token,
                Receiver receiver)
      : socket_(endpoint, {{"Authorization", "Bearer " + token},
                           {"Sec-WebSocket-Protocol", protocol::Name}}),
        receiver_(std::move(receiver)) {
    socket_.set_connection_timeout(5, 0);
    socket_.set_read_timeout(5s);
    socket_.set_write_timeout(5, 0);
    socket_.set_websocket_ping_interval(10);
    socket_.set_websocket_max_missed_pongs(2);
    worker_ = std::thread([this] { run(); });
  }
  ~RemoteSession() override { close(); }
  bool input(const hiyui::TerminalInput &input) override {
    if (stopped_ || permission_ != Permission::All)
      return false;
    auto message = protocol::encodeInput(input);
    if (message.empty())
      return false;
    std::lock_guard lock(mutex_);
    if (stopped_ || permission_ != Permission::All ||
        pendingBytes_ + message.size() > 1024 * 1024)
      return false;
    pendingBytes_ += message.size();
    pending_.push_back(std::move(message));
    ready_.notify_one();
    return true;
  }
  void resize(hiyui::TerminalSize) override {}
  void close() override {
    stopInput();
    if (connected_)
      socket_.close();
    if (worker_.joinable())
      worker_.join();
  }
};
}

std::shared_ptr<TerminalSession>
connectRemote(std::string endpoint, std::string token,
              TerminalSession::Receiver receiver, std::string &error) {
  if (!protocol::validEndpoint(endpoint)) {
    error = "地址格式为 ws://主机:端口/terminal";
    return {};
  }
  if (token.size() != 64 ||
      !std::all_of(token.begin(), token.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })) {
    error = "共享令牌应为 64 位十六进制字符";
    return {};
  }
  return std::make_shared<RemoteSession>(endpoint, token, std::move(receiver));
}

}
