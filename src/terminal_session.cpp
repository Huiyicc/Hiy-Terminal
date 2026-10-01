#include "terminal_session.h"

namespace terminal_app {

LocalSession::LocalSession(hiyui::TerminalSize size, Receiver receiver)
    : model_(size), receiver_(std::move(receiver)) {}

std::shared_ptr<LocalSession> LocalSession::start(const ShellProfile &profile,
                                                  hiyui::TerminalSize size,
                                                  Receiver receiver,
                                                  std::string &error) {
  auto session = std::shared_ptr<LocalSession>(
      new LocalSession(size, std::move(receiver)));
  std::lock_guard lock(session->mutex_);
  session->shell_ = startShell(
      profile, size.columns, size.rows,
      [owner = session.get()](SessionEvent event) {
        owner->receive(std::move(event));
      },
      error);
  if (!session->shell_)
    return {};
  return session;
}

LocalSession::~LocalSession() { close(); }

void LocalSession::publish() {
  ++revision_;
  if (observer_)
    observer_({model_.size(), revision_, running_, exitCode_, model_.screen()});
}

void LocalSession::receive(SessionEvent event) {
  TerminalEvent update;
  Receiver receiver;
  {
    std::lock_guard lock(mutex_);
    if (!running_)
      return;
    receiver = receiver_;
    if (event.type == SessionEvent::Type::Output) {
      model_.write(event.text);
      const auto reply = model_.takeOutput();
      if (!reply.empty() && shell_)
        shell_->write(reply);
      publish();
      update.text = std::move(event.text);
    } else if (event.type == SessionEvent::Type::Exited) {
      running_ = false;
      exitCode_ = event.exitCode;
      publish();
      update.type = TerminalEvent::Type::Exited;
      update.exitCode = exitCode_;
    } else {
      update.type = TerminalEvent::Type::Error;
      update.text = std::move(event.text);
    }
  }
  if (receiver)
    receiver(std::move(update));
}

bool LocalSession::input(const hiyui::TerminalInput &input) {
  std::lock_guard lock(mutex_);
  if (!running_ || !shell_)
    return false;
  model_.input(input);
  const auto bytes = model_.takeOutput();
  return bytes.empty() || shell_->write(bytes);
}

void LocalSession::resize(hiyui::TerminalSize size) {
  std::lock_guard lock(mutex_);
  if (!running_ || !shell_ || size.columns < 1 || size.rows < 1)
    return;
  if (size.columns == model_.size().columns && size.rows == model_.size().rows)
    return;
  model_.resize(size);
  shell_->resize(size.columns, size.rows);
  const auto bytes = model_.takeOutput();
  if (!bytes.empty())
    shell_->write(bytes);
  publish();
}

void LocalSession::observe(Observer observer) {
  std::lock_guard lock(mutex_);
  observer_ = std::move(observer);
  publish();
}

void LocalSession::close() {
  std::unique_ptr<ShellSession> shell;
  {
    std::lock_guard lock(mutex_);
    running_ = false;
    observer_ = {};
    receiver_ = {};
    shell = std::move(shell_);
  }
  if (shell)
    shell->close();
}

}
