#ifndef HIYUI_TERMINAL_APP_LAYOUT_H
#define HIYUI_TERMINAL_APP_LAYOUT_H

#include <hiyui/style/theme.h>
#include <hiyui/widget/label.h>
#include <hiyui/widget/vbox.h>

namespace terminal_app {

inline auto statusBar(hiyui::Ref<hiyui::Label> &status) {
  return hiyui::VBox({.padding = 6})
      .fillWidth()(hiyui::Label({.fontSize = 12, .ellipsis = true})
                       .fillWidth()
                       .textColor(hiyui::ThemeRole::TextSecondary)
                       .ref(status));
}

}

#endif
