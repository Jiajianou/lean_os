#include "lean_os/ozone/leanos_screen.h"

#include <syscall_wrappers.h>
#include <window_manager.h>

#include "lean_os/ozone/leanos_window_manager.h"
#include "ui/gfx/geometry/rect.h"

namespace ui {

namespace {

constexpr int64_t kDisplayId = 1;

gfx::Rect FramebufferBounds() {
  window_manager_framebuffer_info_t info;
  if (sys_framebuffer_info(&info) == 0 && info.width > 0 && info.height > 0) {
    return gfx::Rect(0, 0, static_cast<int>(info.width),
                     static_cast<int>(info.height));
  }
  // What the kernel compiles in when nothing sets a mode, and what every
  // coordinate in the interactive suite is measured against.
  return gfx::Rect(0, 0, 1024, 768);
}

}  // namespace

LeanOsScreen::LeanOsScreen(LeanOsWindowManager* window_manager)
    : window_manager_(window_manager) {
  display::Display display(kDisplayId);
  display.set_bounds(FramebufferBounds());
  display.set_work_area(FramebufferBounds());
  display.set_device_scale_factor(1.0f);
  display_list_.AddDisplay(display, display::DisplayList::Type::PRIMARY);
}

LeanOsScreen::~LeanOsScreen() = default;

const std::vector<display::Display>& LeanOsScreen::GetAllDisplays() const {
  return display_list_.displays();
}

display::Display LeanOsScreen::GetPrimaryDisplay() const {
  return *display_list_.GetPrimaryDisplayIterator();
}

display::Display LeanOsScreen::GetDisplayForAcceleratedWidget(
    gfx::AcceleratedWidget widget) const {
  return GetPrimaryDisplay();
}

gfx::Point LeanOsScreen::GetCursorScreenPoint() const {
  return window_manager_->cursor_screen_point();
}

gfx::AcceleratedWidget LeanOsScreen::GetAcceleratedWidgetAtScreenPoint(
    const gfx::Point& point) const {
  return window_manager_->GetAcceleratedWidgetAtScreenPoint(point);
}

display::Display LeanOsScreen::GetDisplayNearestPoint(
    const gfx::Point& point) const {
  return GetPrimaryDisplay();
}

display::Display LeanOsScreen::GetDisplayMatching(
    const gfx::Rect& match_rect) const {
  return GetPrimaryDisplay();
}

void LeanOsScreen::AddObserver(display::DisplayObserver* observer) {
  display_list_.AddObserver(observer);
}

void LeanOsScreen::RemoveObserver(display::DisplayObserver* observer) {
  display_list_.RemoveObserver(observer);
}

}  // namespace ui
