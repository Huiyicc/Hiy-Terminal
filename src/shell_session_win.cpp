#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "shell_session.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <windows.h>

namespace terminal_app {
namespace {

struct Handle {
  HANDLE value = nullptr;
  Handle() = default;
  explicit Handle(HANDLE handle) : value(handle) {}
  ~Handle() { reset(); }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  void reset(HANDLE handle = nullptr) {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
    value = handle;
  }
  explicit operator bool() const {
    return value && value != INVALID_HANDLE_VALUE;
  }
};

std::wstring wide(std::string_view text) {
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  std::wstring value(length, L'\0');
  if (length)
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), value.data(), length);
  return value;
}

std::string utf8(std::wstring_view text) {
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
  std::string value(length, '\0');
  if (length)
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        value.data(), length, nullptr, nullptr);
  return value;
}

std::string systemError(DWORD code) {
  wchar_t *message = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, code, 0, reinterpret_cast<wchar_t *>(&message), 0,
                 nullptr);
  std::string result =
      message ? utf8(message) : "Windows error " + std::to_string(code);
  if (message)
    LocalFree(message);
  return result;
}

std::wstring quote(std::wstring_view value) {
  if (!value.empty() &&
      value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
    return std::wstring(value);
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (wchar_t c : value) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    result += c;
  }
  result.append(slashes * 2, L'\\');
  result += L'"';
  return result;
}

std::vector<wchar_t> environment() {
  std::vector<std::wstring> entries;
  wchar_t *block = GetEnvironmentStringsW();
  if (block) {
    for (const wchar_t *p = block; *p; p += wcslen(p) + 1)
      if (_wcsnicmp(p, L"TERM=", 5) && _wcsnicmp(p, L"COLORTERM=", 10))
        entries.emplace_back(p);
    FreeEnvironmentStringsW(block);
  }
  entries.emplace_back(L"TERM=xterm-256color");
  entries.emplace_back(L"COLORTERM=truecolor");
  std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
    return _wcsicmp(a.c_str(), b.c_str()) < 0;
  });
  std::vector<wchar_t> result;
  for (const auto &entry : entries) {
    result.insert(result.end(), entry.begin(), entry.end());
    result.push_back(0);
  }
  result.push_back(0);
  return result;
}

class WinSession final : public ShellSession {
  Handle input_, output_, process_, job_, wake_, written_;
  HPCON console_ = nullptr;
  std::thread reader_, worker_;
  Receiver receiver_;
  std::atomic<bool> closing_{false};
  std::mutex mutex_;
  std::deque<std::string> inputQueue_;
  size_t inputBytes_ = 0;
  COORD pendingSize_{};
  bool resizePending_ = false;

  void receive() {
    char bytes[16384];
    DWORD count = 0;
    while (ReadFile(output_.value, bytes, sizeof(bytes), &count, nullptr) &&
           count)
      receiver_({SessionEvent::Type::Output, std::string(bytes, count)});
  }

  void applyResize() {
    COORD size{};
    {
      std::lock_guard lock(mutex_);
      if (!resizePending_)
        return;
      size = pendingSize_;
      resizePending_ = false;
    }
    const HRESULT result = ResizePseudoConsole(console_, size);
    if (FAILED(result))
      receiver_({SessionEvent::Type::Error, systemError(HRESULT_CODE(result))});
  }

  bool send(std::string_view bytes) {
    while (!bytes.empty() && !closing_) {
      OVERLAPPED operation{};
      operation.hEvent = written_.value;
      ResetEvent(written_.value);
      DWORD count = 0;
      if (!WriteFile(input_.value, bytes.data(),
                     static_cast<DWORD>(bytes.size()), &count, &operation)) {
        if (GetLastError() != ERROR_IO_PENDING)
          return false;
        HANDLE handles[]{written_.value, wake_.value, process_.value};
        while (true) {
          const DWORD ready =
              WaitForMultipleObjects(3, handles, FALSE, INFINITE);
          if (ready == WAIT_OBJECT_0)
            break;
          if (ready == WAIT_OBJECT_0 + 1 && !closing_) {
            applyResize();
            continue;
          }
          CancelIoEx(input_.value, &operation);
          GetOverlappedResult(input_.value, &operation, &count, TRUE);
          return false;
        }
        if (!GetOverlappedResult(input_.value, &operation, &count, FALSE))
          return false;
      }
      if (!count)
        return false;
      bytes.remove_prefix(count);
    }
    return !closing_;
  }

  void releaseConsole() {
    input_.reset();
    if (!reader_.joinable())
      output_.reset();
    if (console_) {
      ClosePseudoConsole(console_);
      console_ = nullptr;
    }
    if (reader_.joinable()) {
      reader_.join();
    }
    output_.reset();
  }

  void work() {
    HANDLE handles[]{wake_.value, process_.value};
    while (!closing_) {
      if (WaitForSingleObject(process_.value, 0) == WAIT_OBJECT_0)
        break;
      applyResize();
      std::string bytes;
      {
        std::lock_guard lock(mutex_);
        if (!inputQueue_.empty()) {
          bytes = std::move(inputQueue_.front());
          inputQueue_.pop_front();
          inputBytes_ -= bytes.size();
        }
      }
      if (!bytes.empty()) {
        if (!send(bytes))
          break;
        continue;
      }
      const DWORD ready = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
      if (ready != WAIT_OBJECT_0)
        break;
    }
    const bool requested = closing_.exchange(true);
    DWORD code = 0;
    if (WaitForSingleObject(process_.value, 0) != WAIT_OBJECT_0)
      TerminateJobObject(job_.value, 1);
    WaitForSingleObject(process_.value, INFINITE);
    GetExitCodeProcess(process_.value, &code);
    TerminateJobObject(job_.value, 1);
    releaseConsole();
    if (!requested)
      receiver_({SessionEvent::Type::Exited, {}, static_cast<int>(code)});
  }

public:
  explicit WinSession(Receiver receiver) : receiver_(std::move(receiver)) {}
  ~WinSession() override {
    close();
    if (worker_.joinable())
      worker_.join();
    else {
      if (process_)
        TerminateProcess(process_.value, 1);
      releaseConsole();
    }
  }

  bool start(const ShellProfile &profile, int columns, int rows,
             std::string &error) {
    wake_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    written_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    static std::atomic<unsigned> serial{0};
    const std::wstring name = L"\\\\.\\pipe\\hiyui-terminal-" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(++serial);
    input_.reset(CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr));
    Handle consoleInput(CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr));
    Handle consoleOutput;
    HANDLE outputRead = nullptr, outputWrite = nullptr;
    if (!wake_ || !written_ || !input_ || !consoleInput ||
        !CreatePipe(&outputRead, &outputWrite, nullptr, 65536)) {
      error = systemError(GetLastError());
      return false;
    }
    output_.reset(outputRead);
    consoleOutput.reset(outputWrite);
    COORD size{static_cast<SHORT>(std::clamp(columns, 1, 32767)),
               static_cast<SHORT>(std::clamp(rows, 1, 32767))};
    const HRESULT result = CreatePseudoConsole(
        size, consoleInput.value, consoleOutput.value, 0, &console_);
    if (FAILED(result)) {
      error = systemError(HRESULT_CODE(result));
      return false;
    }
    std::wstring command;
    for (const auto &argument : profile.command) {
      if (!command.empty())
        command += L' ';
      command += quote(wide(argument));
    }
    const auto application = wide(profile.command.front());
    auto env = environment();
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> storage(attributeBytes);
    auto *attributes =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
      error = systemError(GetLastError());
      return false;
    }
    const bool configured = UpdateProcThreadAttribute(
        attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console_,
        sizeof(console_), nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const bool started =
        configured &&
        CreateProcessW(application.c_str(), command.data(), nullptr, nullptr,
                       FALSE,
                       EXTENDED_STARTUPINFO_PRESENT |
                           CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                       env.data(), nullptr, &startup.StartupInfo, &process);
    const DWORD createError = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!started) {
      error = systemError(createError);
      return false;
    }
    process_.reset(process.hProcess);
    Handle mainThread(process.hThread);
    job_.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ ||
        !SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job_.value, process_.value)) {
      error = systemError(GetLastError());
      return false;
    }
    consoleInput.reset();
    consoleOutput.reset();
    reader_ = std::thread([this] { receive(); });
    worker_ = std::thread([this] { work(); });
    if (ResumeThread(mainThread.value) == static_cast<DWORD>(-1)) {
      error = systemError(GetLastError());
      close();
      return false;
    }
    return true;
  }

  bool write(std::string_view bytes) override {
    std::lock_guard lock(mutex_);
    if (closing_ || bytes.size() > 1024 * 1024 - inputBytes_)
      return false;
    inputQueue_.emplace_back(bytes);
    inputBytes_ += bytes.size();
    SetEvent(wake_.value);
    return true;
  }
  void resize(int columns, int rows) override {
    std::lock_guard lock(mutex_);
    pendingSize_ = {static_cast<SHORT>(std::clamp(columns, 1, 32767)),
                    static_cast<SHORT>(std::clamp(rows, 1, 32767))};
    resizePending_ = true;
    SetEvent(wake_.value);
  }
  void close() override {
    closing_ = true;
    if (wake_)
      SetEvent(wake_.value);
  }
};

std::string executable(const wchar_t *name) {
  std::wstring path(32768, L'\0');
  const DWORD count =
      SearchPathW(nullptr, name, nullptr, static_cast<DWORD>(path.size()),
                  path.data(), nullptr);
  return count && count < path.size()
             ? utf8(std::wstring_view(path.data(), count))
             : std::string{};
}

}

std::vector<ShellProfile> localShells() {
  std::vector<ShellProfile> profiles;
  std::string powershell = executable(L"pwsh.exe");
  if (powershell.empty())
    powershell = executable(L"powershell.exe");
  if (!powershell.empty())
    profiles.push_back({"PowerShell", {powershell, "-NoLogo"}});
  const auto cmd = executable(L"cmd.exe");
  if (!cmd.empty())
    profiles.push_back({"CMD", {cmd, "/K", "chcp 65001 >nul"}});
  std::string bash;
  wchar_t directory[32768];
  const DWORD count =
      GetEnvironmentVariableW(L"ProgramFiles", directory, 32768);
  if (count && count < 32768) {
    const auto path = std::wstring(directory) + L"\\Git\\bin\\bash.exe";
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
      bash = utf8(path);
  }
  if (bash.empty())
    bash = executable(L"bash.exe");
  if (!bash.empty())
    profiles.push_back({"Bash", {bash, "--login", "-i"}});
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
    auto session = std::make_unique<WinSession>(std::move(receiver));
    if (session->start(profile, columns, rows, error))
      return session;
  } catch (const std::exception &exception) {
    error = exception.what();
  }
  return {};
}

}
