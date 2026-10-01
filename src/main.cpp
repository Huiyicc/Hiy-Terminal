#include "shared_session.h"
#include <atomic>
#include <charconv>
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
  TerminalSession::Receiver receiver_;
  std::shared_ptr<TerminalEvent> pendingScreen_;

public:
  explicit UiDelivery(TerminalSession::Receiver receiver)
      : receiver_(std::move(receiver)) {}
  void cancel() {
    canceled_ = true;
    wake_.notify_all();
  }
  void send(TerminalEvent event) {
    std::shared_ptr<TerminalEvent> payload;
    {
      std::lock_guard lock(mutex_);
      if (event.type == TerminalEvent::Type::Screen && pendingScreen_) {
        *pendingScreen_ = std::move(event);
        return;
      }
      payload = std::make_shared<TerminalEvent>(std::move(event));
      if (payload->type == TerminalEvent::Type::Screen)
        pendingScreen_ = payload;
    }
    auto self = shared_from_this();
    while (!canceled_) {
      const auto status = async::postUi([self, payload] {
        TerminalEvent event;
        {
          std::lock_guard lock(self->mutex_);
          if (self->canceled_)
            return;
          if (payload == self->pendingScreen_)
            self->pendingScreen_.reset();
          event = std::move(*payload);
        }
        self->receiver_(std::move(event));
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
  std::shared_ptr<TerminalSession> process;
  std::unique_ptr<ShareServer> share;
  bool remote = false;
  Permission permission = Permission::All;
  std::string shell;
  std::string status;
  bool running = false;
  bool renamed = false;
  ~SessionTab() {
    if (delivery)
      delivery->cancel();
    share.reset();
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
  Ref<VBox> network_;
  Ref<Input> bindAddress_, bindPort_, endpoint_, credential_;
  Ref<Dropdown> permission_;
  Ref<Button> startShare_, stopShare_, paste_;
  Ref<Label> shareInfo_;
  bool networkVisible_ = false;
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

  void refreshShare() {
    const auto tab = active();
    const bool shared = tab && tab->share;
    if (startShare_)
      startShare_->enabled(tab && !tab->remote && tab->running && !shared);
    if (stopShare_)
      stopShare_->enabled(shared);
    if (paste_)
      paste_->enabled(tab && tab->running &&
                      tab->permission == Permission::All);
    if (bindAddress_)
      bindAddress_->enabled(!shared);
    if (bindPort_)
      bindPort_->enabled(!shared);
    if (shareInfo_)
      shareInfo_->text(
          shared ? "正在共享当前标签 · " + tab->share->address() + ":" +
                       std::to_string(tab->share->port()) +
                       (tab->share->permission() == Permission::All
                            ? " · all"
                            : " · readOnly") +
                       " · ws 未加密"
                 : "当前标签未共享 · ws 未加密，仅用于受信网络或加密隧道");
  }

  void refreshStatus() {
    refreshShare();
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
    if (current && current->share && permission_) {
      permission_->selectedIndex(
          current->share->permission() == Permission::All ? 1 : 0);
      bindAddress_->text(current->share->address());
      bindPort_->text(std::to_string(current->share->port()));
    }
    refreshStatus();
  }

  void receive(int id, TerminalEvent event) {
    const auto it = sessions_.find(id);
    if (it == sessions_.end())
      return;
    const auto tab = it->second;
    if (event.type == TerminalEvent::Type::Screen) {
      if (tab->terminal)
        tab->terminal->terminalSize(event.size).write(event.text);
      refreshStatus();
      return;
    }
    if (event.type == TerminalEvent::Type::Connected ||
        event.type == TerminalEvent::Type::PermissionChanged) {
      tab->running = true;
      tab->permission = event.permission;
      tab->status = event.permission == Permission::All ? "已连接 · all"
                                                        : "已连接 · readOnly";
      if (tab->terminal)
        tab->terminal->readOnly(event.permission == Permission::ReadOnly);
      tabs_->connectionState(indexOf(id), TabConnectionState::Connected);
      refreshStatus();
      return;
    }
    if (event.type == TerminalEvent::Type::Output) {
      if (tab->terminal)
        tab->terminal->write(event.text);
      return;
    }
    if (event.type == TerminalEvent::Type::Exited) {
      tab->running = false;
      tab->status = event.text.empty()
                        ? "已退出（" + std::to_string(event.exitCode) + "）"
                        : event.text;
      if (tab->terminal)
        tab->terminal->readOnly(true);
      if (tabs_)
        tabs_->connectionState(indexOf(id), TabConnectionState::Disconnected);
    } else {
      if (tab->remote) {
        tab->running = false;
        tab->terminal->readOnly(true);
      }
      tab->status = event.text;
      if (tabs_)
        tabs_->connectionState(indexOf(id), TabConnectionState::Error);
    }
    refreshStatus();
  }

  std::pair<int, std::shared_ptr<SessionTab>> createTab(std::string name,
                                                        bool remote) {
    const int id = nextId_++;
    auto tab = std::make_shared<SessionTab>();
    tab->shell = std::move(name);
    tab->remote = remote;
    tab->permission = remote ? Permission::ReadOnly : Permission::All;
    tab->status = remote ? "正在连接" : "正在启动";
    sessions_.emplace(id, tab);
    content_.with([&](Box &parent) {
      Terminal view({.readOnly = remote});
      if (remote)
        view.terminalSize({80, 24});
      view.init(parent).fill().ref(tab->terminal);
      const std::weak_ptr<SessionTab> weak = tab;
      view.onInput([this, weak](const TerminalInput &input) {
        const auto tab = weak.lock();
        if (!tab || !tab->running || !tab->process ||
            tab->permission != Permission::All)
          return;
        if (!tab->process->input(input)) {
          tab->status = "输入未发送，进程已退出或输入队列已满";
          refreshStatus();
        }
      });
      view.onResize([this, weak](TerminalSize size) {
        if (const auto tab = weak.lock(); tab && tab->process && !tab->remote)
          tab->process->resize(size);
        refreshStatus();
      });
      view.onTitleChange([this, id, weak](std::string_view title) {
        if (const auto tab = weak.lock();
            tab && !tab->renamed && !title.empty() && tabs_)
          tabs_->rename(indexOf(id), std::string(title).c_str());
      });
    });
    tabs_->append({.title = tab->shell.c_str(),
                   .connection = TabConnectionState::Connecting,
                   .data = id});
    tabs_->activeIndex(indexOf(id));
    activate();
    tab->delivery = std::make_shared<UiDelivery>(
        [this, id](TerminalEvent event) { receive(id, std::move(event)); });
    return {id, tab};
  }

  void newSession() {
    if (!content_ || !tabs_ || profiles_.empty())
      return;
    const int selected = shell_ ? shell_->selectedIndex() : 0;
    if (selected < 0 || selected >= static_cast<int>(profiles_.size()))
      return;
    const auto &profile = profiles_[selected];
    auto [id, tab] = createTab(profile.name, false);
    std::string error;
    tab->process = LocalSession::start(
        profile, tab->terminal->terminalSize(),
        [delivery = tab->delivery](TerminalEvent event) {
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

  void connectSession() {
    std::string error;
    auto [id, tab] = createTab("远程终端", true);
    tab->process = connectRemote(
        endpoint_->text(), credential_->text(),
        [delivery = tab->delivery](TerminalEvent event) {
          delivery->send(std::move(event));
        },
        error);
    if (!tab->process) {
      tab->status = error;
      tabs_->connectionState(indexOf(id), TabConnectionState::Error);
    }
    refreshStatus();
  }

  void startSharing() {
    const auto tab = active();
    if (!tab || tab->remote || !tab->running || tab->share)
      return;
    const auto portText = bindPort_->text();
    int port = 0;
    const auto parsed = std::from_chars(
        portText.data(), portText.data() + portText.size(), port);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != portText.data() + portText.size() || port < 1 ||
        port > 65535) {
      shareInfo_->text("端口必须为 1–65535");
      return;
    }
    auto share = std::make_unique<ShareServer>(
        std::static_pointer_cast<LocalSession>(tab->process));
    std::string error;
    if (!share->start({bindAddress_->text(), port,
                       permission_->selectedIndex() == 1
                           ? Permission::All
                           : Permission::ReadOnly},
                      error)) {
      shareInfo_->text(error);
      return;
    }
    tab->share = std::move(share);
    refreshStatus();
  }

  void stopSharing() {
    if (const auto tab = active(); tab && tab->share)
      tab->share.reset();
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
    tab->share.reset();
    if (tab->process)
      tab->process->close();
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
      tab->share.reset();
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
                         Button({.text = "共享 / 连接",
                                 .variant = ButtonVariant::Ghost,
                                 .height = 32})
                             .onClick([this] {
                               networkVisible_ = !networkVisible_;
                               network_->hidden(!networkVisible_);
                               refreshShare();
                             }),
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
                             .ref(paste_)
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
        VBox({.padding = 6, .gap = 4})
            .fillWidth()
            .ref(network_)
            .hidden(true)(
                FlowBox({.gap = 6})
                    .fillWidth()(
                        Label({.text = "共享当前标签"}),
                        Input({.text = "127.0.0.1", .width = 160})
                            .ref(bindAddress_)
                            .accessibilityName("监听地址"),
                        Input({.text = "8765", .maxLength = 5, .width = 85})
                            .ref(bindPort_)
                            .accessibilityName("监听端口"),
                        Dropdown({.width = 160})
                            .ref(permission_)
                            .accessibilityName("共享权限")
                            .onChange([this] {
                              if (const auto tab = active(); tab && tab->share)
                                tab->share->permission(
                                    permission_->selectedIndex() == 1
                                        ? Permission::All
                                        : Permission::ReadOnly);
                              refreshShare();
                            }),
                        Button({.text = "开始共享", .height = 32})
                            .ref(startShare_)
                            .onClick([this] { startSharing(); }),
                        Button({.text = "停止共享",
                                .variant = ButtonVariant::Ghost,
                                .height = 32})
                            .ref(stopShare_)
                            .onClick([this] { stopSharing(); }),
                        Button({.text = "复制令牌",
                                .variant = ButtonVariant::Ghost,
                                .height = 32})
                            .onClick([this] {
                              if (const auto tab = active();
                                  tab && tab->share) {
                                pal::setClipboardText(tab->share->token());
                                shareInfo_->text(
                                    "令牌已复制 · 停止后重新共享会生成新令牌");
                              }
                            })),
                Label({.fontSize = 12})
                    .fillWidth()
                    .textColor(ThemeRole::TextSecondary)
                    .ref(shareInfo_),
                FlowBox({.gap = 6})
                    .fillWidth()(Label({.text = "连接共享终端"}),
                                 Input({.text = "ws://127.0.0.1:8765/terminal",
                                        .width = 290})
                                     .ref(endpoint_)
                                     .accessibilityName("共享地址"),
                                 Input({.placeholder = "共享令牌",
                                        .password = true,
                                        .maxLength = 64,
                                        .width = 220})
                                     .ref(credential_),
                                 Button({.text = "连接", .height = 32})
                                     .onClick([this] { connectSession(); })),
                Label({.text = "readOnly 仅查看和复制 · all 可输入和执行命令 · "
                               "终端尺寸由主机决定",
                       .fontSize = 12})
                    .fillWidth()
                    .textColor(ThemeRole::TextSecondary)),
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
    permission_->items({{.text = "readOnly · 只读"}, {.text = "all · 控制"}});
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
