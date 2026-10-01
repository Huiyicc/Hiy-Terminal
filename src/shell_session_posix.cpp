#include "shell_session.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

extern char **environ;

namespace terminal_app {
namespace {

bool configureDescriptor(int fd) {
  const int flags = fcntl(fd, F_GETFL);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
         fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

class PosixSession final : public ShellSession {
  int master_ = -1;
  int wake_[2]{-1, -1};
  pid_t child_ = -1;
  std::thread worker_;
  Receiver receiver_;
  std::atomic<bool> closing_{false};
  std::mutex mutex_;
  std::deque<std::string> queue_;
  size_t inputBytes_ = 0;
  winsize size_{};
  bool resizePending_ = false;

  void wake() {
    if (wake_[1] >= 0) {
      const char value = 1;
      ssize_t result;
      do {
        result = ::write(wake_[1], &value, 1);
      } while (result < 0 && errno == EINTR);
    }
  }

  void work() {
    std::string pending;
    size_t position = 0;
    bool ended = false;
    while (!closing_ && !ended) {
      winsize pendingSize{};
      bool resize = false;
      {
        std::lock_guard lock(mutex_);
        if (resizePending_) {
          pendingSize = size_;
          resize = true;
          resizePending_ = false;
        }
        if (pending.empty() && !queue_.empty()) {
          pending = std::move(queue_.front());
          queue_.pop_front();
          inputBytes_ -= pending.size();
          position = 0;
        }
      }
      if (resize && ioctl(master_, TIOCSWINSZ, &pendingSize) < 0)
        receiver_({SessionEvent::Type::Error, std::strerror(errno)});
      pollfd descriptors[]{
          {master_,
           static_cast<short>(POLLIN | (pending.empty() ? 0 : POLLOUT)), 0},
          {wake_[0], POLLIN, 0}};
      const int ready = poll(descriptors, 2, -1);
      if (ready < 0) {
        if (errno == EINTR)
          continue;
        receiver_({SessionEvent::Type::Error, std::strerror(errno)});
        break;
      }
      if (descriptors[1].revents & POLLIN) {
        char bytes[64];
        while (::read(wake_[0], bytes, sizeof(bytes)) > 0) {
        }
      }
      if (descriptors[0].revents & POLLNVAL)
        break;
      if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) {
        char bytes[16384];
        for (int batch = 0; batch < 16 && !closing_; ++batch) {
          const ssize_t count = ::read(master_, bytes, sizeof(bytes));
          if (count > 0)
            receiver_({SessionEvent::Type::Output, std::string(bytes, count)});
          else if (count < 0 && errno == EINTR)
            continue;
          else {
            ended = count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);
            break;
          }
        }
      }
      if (!pending.empty() && (descriptors[0].revents & POLLOUT)) {
        const auto count = ::write(master_, pending.data() + position,
                                   pending.size() - position);
        if (count > 0) {
          position += static_cast<size_t>(count);
          if (position == pending.size())
            pending.clear();
        } else if (count < 0 && errno != EINTR && errno != EAGAIN &&
                   errno != EWOULDBLOCK)
          ended = true;
      }
    }
    const bool requested = closing_.exchange(true);
    const pid_t foreground = tcgetpgrp(master_);
    if (requested) {
      if (foreground > 0)
        kill(-foreground, SIGKILL);
      kill(-child_, SIGKILL);
      kill(child_, SIGKILL);
    }
    ::close(master_);
    master_ = -1;
    int status = 0;
    pid_t result = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    do {
      result = waitpid(child_, &status, WNOHANG);
      if (result == child_ || (result < 0 && errno != EINTR))
        break;
      poll(nullptr, 0, 10);
    } while (std::chrono::steady_clock::now() < deadline);
    if (result == 0 || (result < 0 && errno == EINTR)) {
      if (foreground > 0)
        kill(-foreground, SIGKILL);
      kill(-child_, SIGKILL);
      kill(child_, SIGKILL);
      do {
        result = waitpid(child_, &status, 0);
      } while (result < 0 && errno == EINTR);
    }
    child_ = -1;
    if (!requested)
      receiver_({SessionEvent::Type::Exited,
                 {},
                 result < 0          ? -1
                 : WIFEXITED(status) ? WEXITSTATUS(status)
                                     : 128 + WTERMSIG(status)});
  }

public:
  explicit PosixSession(Receiver receiver) : receiver_(std::move(receiver)) {}
  ~PosixSession() override {
    close();
    if (worker_.joinable())
      worker_.join();
    if (child_ > 0) {
      kill(child_, SIGKILL);
      while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
    if (master_ >= 0)
      ::close(master_);
    for (int fd : wake_)
      if (fd >= 0)
        ::close(fd);
  }

  bool start(const ShellProfile &profile, int columns, int rows,
             std::string &error) {
    std::vector<char *> arguments;
    for (const auto &argument : profile.command)
      arguments.push_back(const_cast<char *>(argument.c_str()));
    arguments.push_back(nullptr);
    std::vector<std::string> entries;
    for (char **entry = environ; *entry; ++entry)
      if (std::strncmp(*entry, "TERM=", 5) &&
          std::strncmp(*entry, "COLORTERM=", 10))
        entries.emplace_back(*entry);
    entries.emplace_back("TERM=xterm-256color");
    entries.emplace_back("COLORTERM=truecolor");
    std::vector<char *> env;
    for (auto &entry : entries)
      env.push_back(entry.data());
    env.push_back(nullptr);
    if (pipe(wake_) < 0) {
      error = std::strerror(errno);
      return false;
    }
    for (int fd : wake_)
      if (!configureDescriptor(fd)) {
        error = std::strerror(errno);
        return false;
      }
    termios settings{};
    settings.c_iflag = ICRNL | IXON;
#ifdef IUTF8
    settings.c_iflag |= IUTF8;
#endif
    settings.c_oflag = OPOST | ONLCR;
    settings.c_cflag = CS8 | CREAD;
    settings.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    settings.c_cc[VINTR] = 3;
    settings.c_cc[VQUIT] = 28;
    settings.c_cc[VERASE] = 127;
    settings.c_cc[VKILL] = 21;
    settings.c_cc[VEOF] = 4;
    settings.c_cc[VSTART] = 17;
    settings.c_cc[VSTOP] = 19;
    settings.c_cc[VSUSP] = 26;
    settings.c_cc[VWERASE] = 23;
    settings.c_cc[VLNEXT] = 22;
    settings.c_cc[VREPRINT] = 18;
    settings.c_cc[VMIN] = 1;
    cfsetispeed(&settings, B38400);
    cfsetospeed(&settings, B38400);
    size_ = {static_cast<unsigned short>(std::clamp(rows, 1, 65535)),
             static_cast<unsigned short>(std::clamp(columns, 1, 65535)), 0, 0};
    child_ = forkpty(&master_, nullptr, &settings, &size_);
    if (child_ == 0) {
      execve(arguments[0], arguments.data(), env.data());
      const char message[] = "Unable to execute shell\r\n";
      const auto ignored = ::write(STDERR_FILENO, message, sizeof(message) - 1);
      (void)ignored;
      _exit(127);
    }
    if (child_ < 0) {
      error = std::strerror(errno);
      return false;
    }
    if (!configureDescriptor(master_)) {
      error = std::strerror(errno);
      return false;
    }
    worker_ = std::thread([this] { work(); });
    return true;
  }
  bool write(std::string_view bytes) override {
    std::lock_guard lock(mutex_);
    if (closing_ || bytes.size() > 1024 * 1024 - inputBytes_)
      return false;
    queue_.emplace_back(bytes);
    inputBytes_ += bytes.size();
    wake();
    return true;
  }
  void resize(int columns, int rows) override {
    std::lock_guard lock(mutex_);
    size_ = {static_cast<unsigned short>(std::clamp(rows, 1, 65535)),
             static_cast<unsigned short>(std::clamp(columns, 1, 65535)), 0, 0};
    resizePending_ = true;
    wake();
  }
  void close() override {
    closing_ = true;
    wake();
  }
};

}

std::vector<ShellProfile> localShells() {
  std::vector<ShellProfile> profiles;
  if (const char *shell = std::getenv("SHELL");
      shell && *shell == '/' && access(shell, X_OK) == 0) {
    const std::string path(shell);
    profiles.push_back({path.substr(path.find_last_of('/') + 1), {path, "-i"}});
  }
  for (const char *path : {"/bin/bash", "/bin/zsh", "/bin/sh"}) {
    if (access(path, X_OK) || std::any_of(profiles.begin(), profiles.end(),
                                          [path](const ShellProfile &p) {
                                            return p.command[0] == path;
                                          }))
      continue;
    const std::string value(path);
    profiles.push_back(
        {value.substr(value.find_last_of('/') + 1), {value, "-i"}});
  }
  return profiles;
}

std::unique_ptr<ShellSession> startShell(const ShellProfile &profile,
                                         int columns, int rows,
                                         ShellSession::Receiver receiver,
                                         std::string &error) {
  if (profile.command.empty()) {
    error = "No shell command";
    return {};
  }
  try {
    auto session = std::make_unique<PosixSession>(std::move(receiver));
    if (session->start(profile, columns, rows, error))
      return session;
  } catch (const std::exception &exception) {
    error = exception.what();
  }
  return {};
}

}
