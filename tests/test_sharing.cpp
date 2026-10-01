#include "share_protocol.h"
#include "shared_session.h"
#include <chrono>
#include <condition_variable>
#include <httplib.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

using namespace terminal_app;
using namespace std::chrono_literals;

namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

int shellFixture() {
#ifdef _WIN32
  FILE *stream = nullptr;
  if (freopen_s(&stream, "CONOUT$", "w", stdout))
    return 2;
  const auto input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
  if (input == INVALID_HANDLE_VALUE || !SetConsoleMode(input, 0))
    return 3;
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
  const auto output = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
  SetConsoleMode(output,
                 ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
  termios mode{};
  tcgetattr(STDIN_FILENO, &mode);
  cfmakeraw(&mode);
  tcsetattr(STDIN_FILENO, TCSANOW, &mode);
#endif
  std::cout << "\x1b[2J\x1b[HREADY" << std::flush;
  char c;
  while (true) {
#ifdef _WIN32
    DWORD count = 0;
    if (!ReadFile(input, &c, 1, &count, nullptr) || count != 1)
      break;
#else
    if (read(STDIN_FILENO, &c, 1) != 1)
      break;
#endif
    if (c == '!')
      break;
    if (c == '~')
      std::cout << "\x1b[?1049h\x1b[38;2;12;34;56mALT中文\x1b[3;4H";
    else if (c == '`')
      std::cout << "\x1b[?1049l";
    else
      std::cout << "\r\nSEEN:" << c;
    std::cout.flush();
  }
#ifdef _WIN32
  CloseHandle(input);
#endif
  return 0;
}

struct Inbox {
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<TerminalEvent> events;
  void receive(TerminalEvent event) {
    std::lock_guard lock(mutex);
    events.push_back(std::move(event));
    ready.notify_all();
  }
  template <class F> bool wait(F predicate) {
    std::unique_lock lock(mutex);
    return ready.wait_for(lock, 8s, [&] {
      return std::any_of(events.begin(), events.end(), predicate);
    });
  }
};

httplib::Headers headers(const std::string &token) {
  return {{"Authorization", "Bearer " + token},
          {"Sec-WebSocket-Protocol", protocol::Name}};
}

ScreenFrame frame(httplib::ws::WebSocketClient &client,
                  std::string_view text = {}) {
  client.set_read_timeout(200ms);
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  std::string message;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = client.read(message);
    require(result != httplib::ws::Fail,
            "Connection closed before frame arrived");
    if (result == httplib::ws::Timeout)
      continue;
    ScreenFrame value;
    if (protocol::decodeFrame(message, value)) {
      hiyui::TerminalModel model(value.size);
      model.write(value.screen);
      if (text.empty() || model.text().find(text) != std::string::npos)
        return value;
    }
  }
  throw std::runtime_error("Expected terminal frame did not arrive");
}

void protocolTests() {
  const std::string binary("a\0中文", 8);
  hiyui::TerminalInput input{.type = hiyui::TerminalInputType::Paste,
                             .text = binary};
  auto encoded = protocol::encodeInput(input);
  hiyui::TerminalInput decoded;
  require(protocol::decodeInput(encoded, decoded) && decoded.text == binary,
          "Binary input was truncated");
  for (size_t n = 0; n < 15; ++n)
    require(
        !protocol::decodeInput(std::string_view(encoded).substr(0, n), decoded),
        "Truncated input accepted");
  encoded[1] = 99;
  require(!protocol::decodeInput(encoded, decoded),
          "Unknown input type accepted");
  std::string oversized(protocol::MaxInput + 1, 'x');
  require(protocol::encodeInput({.text = oversized}).empty(),
          "Oversized paste accepted");
  require(protocol::validEndpoint("ws://127.0.0.1:8765/terminal") &&
              !protocol::validEndpoint("ws://secret@host/terminal") &&
              !protocol::validEndpoint("wss://host/terminal"),
          "Endpoint validation mismatch");
  ScreenFrame original{{80, 24}, 0x123456789ULL, false, -1, "\x1b[31m中文"},
      copy;
  const auto wire = protocol::encodeFrame(original);
  require(protocol::decodeFrame(wire, copy) &&
              copy.revision == original.revision && copy.exitCode == -1 &&
              copy.screen == original.screen,
          "Frame roundtrip failed");
  for (size_t n = 0; n < 18; ++n)
    require(!protocol::decodeFrame(std::string_view(wire).substr(0, n), copy),
            "Truncated screen accepted");
  original.size.columns = protocol::MaxColumns + 1;
  require(protocol::encodeFrame(original).empty(), "Oversized grid accepted");
  const auto first = protocol::generateToken();
  require(first.size() == 64 && first != protocol::generateToken(),
          "Tokens are not independent");
}

void sharingTests(const char *executable) {
  Inbox local;
  std::string error;
  auto session = LocalSession::start(
      {"fixture", {executable, "--shell-fixture"}}, {80, 24},
      [&](TerminalEvent event) { local.receive(std::move(event)); }, error);
  require(session != nullptr, error.c_str());
  require(local.wait([](const TerminalEvent &e) {
    return e.text.find("READY") != std::string::npos;
  }),
          "PTY fixture failed to start");
  ShareServer server(session);
  require(server.start({"127.0.0.1", 0, Permission::ReadOnly}, error),
          error.c_str());
  const auto endpoint =
      "ws://127.0.0.1:" + std::to_string(server.port()) + "/terminal";
  const auto token = server.token();
  {
    ShareServer occupied(session);
    require(
        !occupied.start({"127.0.0.1", server.port(), Permission::All}, error),
        "Occupied port was reused");
  }

  httplib::ws::WebSocketClient unauthenticated(endpoint);
  const auto rejected = unauthenticated.connect();
  require(!rejected && rejected.status() == 401,
          "Unauthenticated upgrade accepted");
  httplib::ws::WebSocketClient incorrect(endpoint,
                                         headers(std::string(64, '0')));
  require(!incorrect.connect(), "Incorrect credential accepted");
  auto browserHeaders = headers(token);
  browserHeaders.emplace("Origin", "https://example.invalid");
  httplib::ws::WebSocketClient browser(endpoint, browserHeaders);
  require(!browser.connect(), "Unsolicited browser origin accepted");

  httplib::ws::WebSocketClient readonly(endpoint, headers(token));
  require(bool(readonly.connect()), "Read-only connection failed");
  frame(readonly, "READY");
  const auto attempted = protocol::encodeInput({.text = "x"});
  require(readonly.send(attempted.data(), attempted.size()),
          "Read-only attack send failed");
  readonly.set_read_timeout(2, 0);
  std::string response;
  require(readonly.read(response) == httplib::ws::Fail,
          "Read-only input was not rejected by server");
  {
    std::lock_guard lock(local.mutex);
    require(std::none_of(local.events.begin(), local.events.end(),
                         [](const TerminalEvent &e) {
                           return e.text.find("SEEN:x") != std::string::npos;
                         }),
            "Read-only input reached PTY");
  }

  Inbox remote;
  auto client = connectRemote(
      endpoint, token, [&](TerminalEvent e) { remote.receive(std::move(e)); },
      error);
  require(bool(client), error.c_str());
  require(remote.wait([](const TerminalEvent &e) {
    return e.type == TerminalEvent::Type::Connected &&
           e.permission == Permission::ReadOnly;
  }),
          "Initial permission missing");
  require(!client->input({.text = "x"}), "Read-only client allowed input");
  server.permission(Permission::All);
  require(remote.wait([](const TerminalEvent &e) {
    return e.type == TerminalEvent::Type::PermissionChanged &&
           e.permission == Permission::All;
  }),
          "Permission upgrade missing");
  require(client->input({.text = "z"}), "Controller input failed");
  if (!remote.wait([](const TerminalEvent &e) {
        if (e.type != TerminalEvent::Type::Screen)
          return false;
        hiyui::TerminalModel model(e.size);
        model.write(e.text);
        return model.text().find("SEEN:z") != std::string::npos;
      })) {
    std::lock_guard lock(local.mutex);
    std::string output;
    for (const auto &event : local.events)
      output += event.text;
    throw std::runtime_error("Controller echo missing; PTY output: " + output);
  }
  {
    std::lock_guard lock(local.mutex);
    std::string all;
    for (const auto &event : local.events)
      all += event.text;
    const auto position = all.find("SEEN:z");
    require(position != std::string::npos &&
                all.find("SEEN:z", position + 1) == std::string::npos,
            "Single input was delivered more than once");
  }
  require(client->input({.text = "~"}), "Alternate-screen command failed");
  require(remote.wait([](const TerminalEvent &e) {
    return e.type == TerminalEvent::Type::Screen &&
           e.text.find("ALT中文") != std::string::npos;
  }),
          "Alternate screen missing");
  httplib::ws::WebSocketClient late(endpoint, headers(token));
  require(bool(late.connect()), "Late connection failed");
  const auto initial = frame(late, "ALT中文");
  require(initial.screen.find("38;2;12;34;56") != std::string::npos,
          "Late-join color lost");
  session->resize({100, 30});
  const auto resized = frame(late, "ALT中文");
  require(resized.size.columns == 100 && resized.size.rows == 30 &&
              resized.revision > initial.revision,
          "Resize/order was lost");
  require(client->input({.text = "q"}), "Multi-client input failed");
  frame(late, "SEEN:q");
  require(remote.wait([](const TerminalEvent &e) {
    return e.type == TerminalEvent::Type::Screen &&
           e.text.find("SEEN:q") != std::string::npos;
  }),
          "Second client did not receive shared output");

  server.permission(Permission::ReadOnly);
  require(remote.wait([](const TerminalEvent &e) {
    return e.type == TerminalEvent::Type::PermissionChanged &&
           e.permission == Permission::ReadOnly;
  }),
          "Revocation missing");
  require(!client->input({.text = "x"}), "Revoked client can still write");
  const auto malicious = protocol::encodeInput({.text = "x"});
  late.send(malicious.data(), malicious.size());
  late.set_read_timeout(2, 0);
  while (late.read(response) == httplib::ws::Binary) {
  }
  require(!late.is_open(), "Revoked server permission still allowed input");

  server.stop();
  require(remote.wait([](const TerminalEvent &e) {
    return e.type == TerminalEvent::Type::Exited;
  }),
          "Stop did not disconnect client");
  client->close();
  require(session->input({.text = "`"}), "Stopping sharing closed local shell");
  require(server.start({"127.0.0.1", 0, Permission::All}, error),
          "Restart failed");
  require(server.token() != token, "Restart reused revoked token");
  const auto restarted =
      "ws://127.0.0.1:" + std::to_string(server.port()) + "/terminal";
  httplib::ws::WebSocketClient old(restarted, headers(token));
  require(!old.connect(), "Old token authenticated after restart");
  httplib::ws::WebSocketClient invalid(restarted, headers(server.token()));
  require(bool(invalid.connect()), "Reconnect failed");
  frame(invalid);
  invalid.send("resize:1,1");
  invalid.set_read_timeout(2, 0);
  require(invalid.read(response) == httplib::ws::Fail,
          "Unrecognized resize command accepted");
  httplib::ws::WebSocketClient oversized(restarted, headers(server.token()));
  require(bool(oversized.connect()), "Oversized input test connection failed");
  frame(oversized);
  auto excessive = protocol::encodeInput({.text = "x"});
  excessive.resize(protocol::MaxInput + 16, 'x');
  oversized.send(excessive.data(), excessive.size());
  oversized.set_read_timeout(2, 0);
  require(oversized.read(response) == httplib::ws::Fail,
          "Oversized input was not rejected");

  Inbox ending;
  auto observer = connectRemote(
      restarted, server.token(),
      [&](TerminalEvent e) { ending.receive(std::move(e)); }, error);
  require(bool(observer), "Exit observer failed");
  require(ending.wait([](const TerminalEvent &e) {
    return e.type == TerminalEvent::Type::Connected;
  }),
          "Exit observer did not connect");
  require(session->input({.text = "!"}), "Shell exit input failed");
  require(ending.wait([](const TerminalEvent &e) {
    return e.type == TerminalEvent::Type::Exited &&
           e.text == "远端 shell 已退出";
  }),
          "Shell exit was not delivered");
  observer->close();
  server.stop();
  session->close();
}

void sessionLifetimeTests(const char *executable) {
  std::string error;
  for (int i = 0; i < 3; ++i) {
    Inbox events;
    auto session = LocalSession::start(
        {"fixture", {executable, "--shell-fixture"}}, {80, 24},
        [&](TerminalEvent event) { events.receive(std::move(event)); }, error);
    require(bool(session), "Lifecycle fixture failed to start");
    require(events.wait([](const TerminalEvent &event) {
      return event.text.find("READY") != std::string::npos;
    }),
            "Lifecycle fixture did not become ready");
    ShareServer share(session);
    require(share.start({"127.0.0.1", 0, Permission::All}, error),
            "Lifecycle listener failed");
    const auto endpoint =
        "ws://127.0.0.1:" + std::to_string(share.port()) + "/terminal";
    Inbox remote;
    auto client = connectRemote(
        endpoint, share.token(),
        [&](TerminalEvent event) { remote.receive(std::move(event)); }, error);
    require(bool(client), "Lifecycle connection failed");
    require(remote.wait([](const TerminalEvent &event) {
      return event.type == TerminalEvent::Type::Connected;
    }),
            "Lifecycle connection did not become ready");
    session->input({.text = "pending"});
    client->close();
    client->close();
    share.stop();
    share.stop();
    session.reset();
  }
}
}

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--shell-fixture")
    return shellFixture();
  try {
    protocolTests();
    sharingTests(argv[0]);
    sessionLifetimeTests(argv[0]);
    std::cout << "terminal protocol and loopback sharing passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
