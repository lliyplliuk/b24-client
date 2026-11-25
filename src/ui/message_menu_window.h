#pragma once

#include <functional>

class QImage;
class QMenu;
class QRect;
class QWidget;

namespace bx::ui {

void showMessageMenuWindow(QMenu &menu, const QRect &globalAnchor);
QMenu *createImageContextMenu(QWidget *parent, std::function<void()> download, std::function<QImage()> image);

} // namespace bx::ui
