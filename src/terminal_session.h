#ifndef HIYUI_APP_TERMINAL_SESSION_H
#define HIYUI_APP_TERMINAL_SESSION_H

#include "shell_session.h"
#include <cstdint>
#include <hiyui/misc/terminal_model.h>
#include <mutex>

namespace terminal_app {

enum class Permission : uint8_t { ReadOnly, All };

struct ScreenFrame {
  hiyui::TerminalSize size;
  uint64_t revision = 0;
  bool running = true;
  int exitCode = 0;
  std::string screen;
};

struct TerminalEvent {
  enum class Type {
    Output,
    Screen,
    Connected,
    PermissionChanged,
    Exited,
    Error
  };
  Type type = Type::Output;
  std::string text;
  hiyui::TerminalSize size;
  Permission permission = Permission::ReadOnly;
  int exitCode = 0;
};

class TerminalSession {
public:
  using Receiver = std::function<void(TerminalEvent)>;
  virtual ~TerminalSession() = default;
  virtual bool input(const hiyui::TerminalInput &) = 0;
  virtual void resize(hiyui::TerminalSize) = 0;
  virtual void close() = 0;
};

class LocalSession final : public TerminalSession {
public:
  using Observer = std::function<void(ScreenFrame)>;
  static std::shared_ptr<LocalSession> start(const ShellProfile &,
                                             hiyui::TerminalSize, Receiver,
                                             std::string &error);
  ~LocalSession() override;
  bool input(const hiyui::TerminalInput &) override;
  void resize(hiyui::TerminalSize) override;
  void close() override;
  void observe(Observer);

private:
  LocalSession(hiyui::TerminalSize, Receiver);
  void receive(SessionEvent);
  void publish();
  std::mutex mutex_;
  hiyui::TerminalModel model_;
  std::unique_ptr<ShellSession> shell_;
  Receiver receiver_;
  Observer observer_;
  uint64_t revision_ = 0;
  bool running_ = true;
  int exitCode_ = 0;
};

}
#endif
