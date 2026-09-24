// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "controller.hpp"
#include <QQuickView>

namespace LayerShellQt {
class Window;
}
class ShellView : public QQuickView {
    Q_OBJECT
  public:
    ShellView(ShellController &controller, QScreen *screen, bool desktop, bool preview);
    Q_INVOKABLE void setExpanded(bool expanded);
    QScreen *outputScreen() const { return outputScreen_; }

  private:
    ShellController &controller_;
    bool desktop_, preview_, expanded_ = false;
    LayerShellQt::Window *layer_ = nullptr;
    QScreen *outputScreen_;
    void resizeForContent();
};
