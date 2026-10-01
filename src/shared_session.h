#ifndef HIYUI_APP_SHARED_SESSION_H
#define HIYUI_APP_SHARED_SESSION_H

#include "terminal_session.h"

namespace terminal_app {

struct ShareOptions {
  std::string address = "127.0.0.1";
  int port = 8765;
  Permission permission = Permission::ReadOnly;
};

class ShareServer {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  explicit ShareServer(std::shared_ptr<LocalSession>);
  ~ShareServer();
  bool start(const ShareOptions &, std::string &error);
  void stop();
  void permission(Permission);
  Permission permission() const;
  std::string token() const;
  std::string address() const;
  int port() const;
};

std::shared_ptr<TerminalSession> connectRemote(std::string endpoint,
                                               std::string token,
                                               TerminalSession::Receiver,
                                               std::string &error);

}
#endif
