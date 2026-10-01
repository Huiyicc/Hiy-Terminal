#include "terminal_layout.h"
#include <hiyui/hiyui.h>
#include <iostream>
#include <stdexcept>

using namespace hiyui;

namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

class LayoutTest final : public Application {
  void setup() override {
    try {
      auto &window = createWindow({.title = "Terminal layout test",
                                   .width = 1100,
                                   .height = 720,
                                   .borderless = true,
                                   .shadow = false,
                                   .initiallyHidden = true});
      Ref<VBox> root, network;
      Ref<Box> content;
      Ref<Label> status;
      window.root(VBox{}.ref(root)(
          TabsEx{}.fillWidth(),
          FlowBox({.padding = 4, .gap = 4})
              .fillWidth()(Dropdown({.width = 150}),
                           Button({.text = "共享 / 连接"})),
          VBox({.padding = 6, .gap = 4})
              .fillWidth()
              .ref(network)(
                  Input({.text = "127.0.0.1", .width = 160}),
                  Label({.text = "正在共享 · readOnly", .fontSize = 12}),
                  Input(
                      {.text = "ws://127.0.0.1:8765/terminal", .width = 290})),
          Box{}.fillWidth().flex(1).ref(content)(Terminal{}.fill()),
          terminal_app::statusBar(status)));
      Label reference({.text = "状态文字", .fontSize = 12});
      root.with([&](VBox &parent) { reference.init(parent).hidden(true); });
      const float originalDpi = systemDpiScale();
      for (const auto dpi : {1.0f, 1.5f, 2.0f}) {
        detail::handleDpiChanged(window.display(), dpi * 96);
        for (bool dark : {false, true}) {
          setTheme(dark ? DarkTheme : LightTheme);
          for (const Size size :
               {Size{1100, 720}, Size{760, 480}, Size{480, 600}}) {
            window.size(size.width, size.height);
            for (bool expanded : {false, true}) {
              network->hidden(!expanded);
              for (const std::string text :
                   {std::string("CMD · 运行中 · 80 × 24"),
                    std::string("远程终端 · 连接失败 · ") +
                        std::string(500, 'x')}) {
                status->text(text);
                require(status->contentHeightPx() >= reference.heightPx(),
                        "Status padding clipped the text line");
                const auto body = content->boundsPx();
                const auto label = status->boundsPx();
                const auto page = root->boundsPx();
                require(body.h > 0 && body.y + body.h <= label.y,
                        "Terminal body overlapped the status bar");
                require(label.y >= page.y &&
                            label.y + label.h <= page.y + page.h,
                        "Status bar overflowed the window");
                require(status->contentHeightPx() == reference.heightPx(),
                        "Long status text expanded beyond one line");
              }
            }
          }
        }
      }
      detail::handleDpiChanged(window.display(), originalDpi * 96);
      require(!window.isShown(), "Layout test exposed a visible window");
    } catch (const std::exception &e) {
      error = e.what();
    }
    quit();
  }

public:
  std::string error;
};
}

int main() {
  LayoutTest app;
  const auto result = app.run();
  if (!app.error.empty()) {
    std::cerr << app.error << '\n';
    return 1;
  }
  if (result == 0)
    std::cout << "terminal status layout passed\n";
  return result;
}
