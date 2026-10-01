#ifndef HIYUI_APP_SHELL_SESSION_H
#define HIYUI_APP_SHELL_SESSION_H

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace terminal_app {

struct ShellProfile {
  std::string name;
  std::vector<std::string> command;
};

struct SessionEvent {
  enum class Type { Output, Exited, Error };
  Type type = Type::Output;
  std::string text;
  int exitCode = 0;
};

class ShellSession {
public:
  using Receiver = std::function<void(SessionEvent)>;
  virtual ~ShellSession() = default;
  virtual bool write(std::string_view bytes) = 0;
  virtual void resize(int columns, int rows) = 0;
  virtual void close() = 0;
};

std::vector<ShellProfile> localShells();
std::unique_ptr<ShellSession> startShell(const ShellProfile &profile,
                                         int columns, int rows,
                                         ShellSession::Receiver receiver,
                                         std::string &error);

}
#endif
