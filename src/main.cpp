#include "shell_session.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <hiyui/hiyui.h>
#include <hiyui/misc/async.h>
#include <hiyui/misc/hotkey.h>
#include <map>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace hiyui;

namespace terminal_app {
namespace {

class UiDelivery : public std::enable_shared_from_this<UiDelivery> {
  std::atomic<bool> canceled_{false};
  std::mutex mutex_;
  std::condition_variable wake_;
  ShellSession::Receiver receiver_;

public:
  explicit UiDelivery(ShellSession::Receiver receiver)
      : receiver_(std::move(receiver)) {}
  void cancel() {
    canceled_ = true;
    wake_.notify_all();
  }
  void send(SessionEvent event) {
    auto payload = std::make_shared<SessionEvent>(std::move(event));
    auto self = shared_from_this();
    while (!canceled_) {
      const auto status = async::postUi([self, payload] {
        if (!self->canceled_)
          self->receiver_(std::move(*payload));
      });
      if (status != async::PostStatus::QueueFull)
        return;
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(2),
                     [this] { return canceled_.load(); });
    }
  }
};

struct SessionTab {
  Ref<Terminal> terminal;
  std::shared_ptr<UiDelivery> delivery;
  std::unique_ptr<ShellSession> process;
  std::string shell;
  std::string status;
  bool running = false;
  bool renamed = false;
  ~SessionTab() {
    if (delivery)
      delivery->cancel();
    if (process)
      process->close();
  }
};

class TerminalApp final : public Application {
  Ref<TabsEx> tabs_;
  Ref<Box> content_;
  Ref<Label> status_;
  Ref<Label> empty_;
  Ref<Dropdown> shell_;
  std::vector<ShellProfile> profiles_;
  std::map<int, std::shared_ptr<SessionTab>> sessions_;
  std::vector<HotkeyManager::ShortcutId> shortcuts_;
  int nextId_ = 1;
  bool dark_ = true;

  int indexOf(int id) const {
    if (tabs_)
      for (int i = 0; i < tabs_->itemCount(); ++i)
        if (tabs_->itemData(i) == id)
          return i;
    return -1;
  }

  std::shared_ptr<SessionTab> active() const {
    if (!tabs_)
      return {};
    const auto it = sessions_.find(tabs_->itemData(tabs_->activeIndex()));
    return it == sessions_.end() ? nullptr : it->second;
  }

  void refreshStatus() {
    if (!status_)
      return;
    const auto tab = active();
    if (!tab || !tab->terminal) {
      status_->text(profiles_.empty() ? "未找到本地 shell"
                                      : "选择 shell 并新建会话");
      return;
    }
    const auto size = tab->terminal->terminalSize();
    status_->text(tab->shell + "  ·  " + tab->status + "  ·  " +
                  std::to_string(size.columns) + " × " +
                  std::to_string(size.rows));
  }

  void activate() {
    const auto current = active();
    for (const auto &[id, tab] : sessions_)
      if (tab->terminal)
        tab->terminal->hidden(tab != current);
    if (empty_)
      empty_->hidden(current != nullptr);
    if (current && current->terminal)
      current->terminal->focus();
    refreshStatus();
  }

  void receive(int id, SessionEvent event) {
    const auto it = sessions_.find(id);
    if (it == sessions_.end())
      return;
    const auto tab = it->second;
    if (event.type == SessionEvent::Type::Output) {
      if (tab->terminal)
        tab->terminal->write(event.text);
      return;
    }
    if (event.type == SessionEvent::Type::Exited) {
      tab->running = false;
      tab->status = "已退出（" + std::to_string(event.exitCode) + "）";
      if (tabs_)
        tabs_->connectionState(indexOf(id), TabConnectionState::Disconnected);
    } else {
      tab->status = event.text;
      if (tabs_)
        tabs_->connectionState(indexOf(id), TabConnectionState::Error);
    }
    refreshStatus();
  }

  void newSession() {
    if (!content_ || !tabs_ || profiles_.empty())
      return;
    const int selected = shell_ ? shell_->selectedIndex() : 0;
    if (selected < 0 || selected >= static_cast<int>(profiles_.size()))
      return;
    const auto &profile = profiles_[selected];
    const int id = nextId_++;
    auto tab = std::make_shared<SessionTab>();
    tab->shell = profile.name;
    tab->status = "正在启动";
    sessions_.emplace(id, tab);
    content_.with([&](Box &parent) {
      Terminal view;
      view.init(parent).fill().ref(tab->terminal);
      const std::weak_ptr<SessionTab> weak = tab;
      view.onData([this, weak](std::string_view bytes) {
        const auto tab = weak.lock();
        if (!tab || !tab->running || !tab->process)
          return;
        if (!tab->process->write(bytes)) {
          tab->status = "输入未发送，进程已退出或输入队列已满";
          refreshStatus();
        }
      });
      view.onResize([this, weak](TerminalSize size) {
        if (const auto tab = weak.lock(); tab && tab->process)
          tab->process->resize(size.columns, size.rows);
        refreshStatus();
      });
      view.onTitleChange([this, id, weak](std::string_view title) {
        if (const auto tab = weak.lock();
            tab && !tab->renamed && !title.empty() && tabs_)
          tabs_->rename(indexOf(id), std::string(title).c_str());
      });
    });
    tabs_->append({.title = profile.name.c_str(),
                   .connection = TabConnectionState::Connecting,
                   .data = id});
    tabs_->activeIndex(indexOf(id));
    activate();
    tab->delivery = std::make_shared<UiDelivery>(
        [this, id](SessionEvent event) { receive(id, std::move(event)); });
    const auto size = tab->terminal->terminalSize();
    std::string error;
    tab->process = startShell(
        profile, size.columns, size.rows,
        [delivery = tab->delivery](SessionEvent event) {
          delivery->send(std::move(event));
        },
        error);
    tab->running = tab->process != nullptr;
    tab->status = tab->running ? "运行中" : "启动失败: " + error;
    tabs_->connectionState(indexOf(id), tab->running
                                            ? TabConnectionState::Connected
                                            : TabConnectionState::Error);
    refreshStatus();
  }

  void closeSession(int id) {
    const auto it = sessions_.find(id);
    if (it == sessions_.end())
      return;
    const int index = indexOf(id);
    auto tab = it->second;
    if (tab->delivery)
      tab->delivery->cancel();
    tab->process.reset();
    if (tab->terminal)
      tab->terminal->destroy();
    sessions_.erase(it);
    if (tabs_)
      tabs_->removeTab(index);
    activate();
  }

  void stopSessions() {
    for (auto &[id, tab] : sessions_) {
      if (tab->delivery)
        tab->delivery->cancel();
      if (tab->process)
        tab->process->close();
      if (tab->terminal)
        tab->terminal->destroy();
    }
    sessions_.clear();
  }

  void setup() override {
    setTheme(DarkTheme);
    profiles_ = localShells();
    auto &window = createWindow({.title = "HIY Terminal",
                                 .width = 1100,
                                 .height = 720,
                                 .borderless = true});
    window.onCloseRequest([this](WindowCloseEvent &) { stopSessions(); });
    window.root(VBox({.gap = 0})(
        TabsEx{}
            .fillWidth()
            .ref(tabs_)
            .onNewRequest([this] { newSession(); })
            .onChange([this] { activate(); })
            .onCloseRequest(
                [this](const TabsExEvent &event) { closeSession(event.data); })
            .onRename([this](const TabsExEvent &event) {
              if (const auto it = sessions_.find(event.data);
                  it != sessions_.end())
                it->second->renamed = true;
            }),
        FlowBox({.padding = 4, .gap = 4})
            .fillWidth()(Dropdown({.width = 150})
                             .ref(shell_)
                             .accessibilityName("新会话的 shell"),
                         Button({.text = "新建会话", .height = 32})
                             .onClick([this] { newSession(); })
                             .enabled(!profiles_.empty()),
                         Button({.text = "复制",
                                 .variant = ButtonVariant::Ghost,
                                 .height = 32})
                             .onClick([this] {
                               if (auto tab = active(); tab && tab->terminal)
                                 tab->terminal->copySelection();
                             }),
                         Button({.text = "粘贴",
                                 .variant = ButtonVariant::Ghost,
                                 .height = 32})
                             .onClick([this] {
                               if (auto tab = active(); tab && tab->terminal) {
                                 tab->terminal->paste(pal::getClipboardText());
                                 tab->terminal->focus();
                               }
                             }),
                         Button({.text = "关闭会话",
                                 .variant = ButtonVariant::Ghost,
                                 .height = 32})
                             .onClick([this] {
                               if (tabs_)
                                 closeSession(
                                     tabs_->itemData(tabs_->activeIndex()));
                             }),
                         Button({.text = "切换主题",
                                 .variant = ButtonVariant::Ghost,
                                 .height = 32})
                             .onClick([this] {
                               dark_ = !dark_;
                               setTheme(dark_ ? DarkTheme : LightTheme);
                             })),
        Box{}.fillWidth().flex(1).ref(content_)(
            Label({.text = "选择 shell 并新建会话"}).center().ref(empty_)),
        Label({.fontSize = 12, .ellipsis = true})
            .fillWidth()
            .padding(6)
            .textColor(ThemeRole::TextSecondary)
            .ref(status_)));
    std::vector<DropdownItem> items;
    for (const auto &profile : profiles_)
      items.push_back({.text = profile.name.c_str()});
    shell_->items(items);
    auto &keys = HotkeyManager::instance();
    shortcuts_.push_back(
        keys.registerShortcut({.sdlKey = SDLK_t, .ctrl = true, .shift = true},
                              [this] { newSession(); }));
    shortcuts_.push_back(keys.registerShortcut(
        {.sdlKey = SDLK_w, .ctrl = true, .shift = true}, [this] {
          if (tabs_)
            closeSession(tabs_->itemData(tabs_->activeIndex()));
        }));
    newSession();
    refreshStatus();
  }

public:
  ~TerminalApp() override {
    for (auto id : shortcuts_)
      HotkeyManager::instance().unregisterShortcut(id);
    stopSessions();
  }
};

}
}

int main() {
  terminal_app::TerminalApp app;
  return app.run();
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return main(); }
#endif
