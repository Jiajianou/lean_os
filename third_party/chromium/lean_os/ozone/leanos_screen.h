// The display, which is the framebuffer.
//
// The kernel answers sys_framebuffer_info for any process - the size of the
// screen is not a secret, only painting on it is - so the one display here is
// the machine's real one, at scale 1, rather than headless's 1x1 default that
// content_shell has to be told about with a switch.

#ifndef LEAN_OS_OZONE_LEANOS_SCREEN_H_
#define LEAN_OS_OZONE_LEANOS_SCREEN_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "ui/display/display.h"
#include "ui/display/display_list.h"
#include "ui/gfx/geometry/point.h"
#include "ui/ozone/public/platform_screen.h"

namespace ui {

class LeanOsWindowManager;

class LeanOsScreen : public PlatformScreen {
 public:
  explicit LeanOsScreen(LeanOsWindowManager* window_manager);

  LeanOsScreen(const LeanOsScreen&) = delete;
  LeanOsScreen& operator=(const LeanOsScreen&) = delete;

  ~LeanOsScreen() override;

  // PlatformScreen:
  const std::vector<display::Display>& GetAllDisplays() const override;
  display::Display GetPrimaryDisplay() const override;
  display::Display GetDisplayForAcceleratedWidget(
      gfx::AcceleratedWidget widget) const override;
  gfx::Point GetCursorScreenPoint() const override;
  gfx::AcceleratedWidget GetAcceleratedWidgetAtScreenPoint(
      const gfx::Point& point) const override;
  display::Display GetDisplayNearestPoint(
      const gfx::Point& point) const override;
  display::Display GetDisplayMatching(
      const gfx::Rect& match_rect) const override;
  void AddObserver(display::DisplayObserver* observer) override;
  void RemoveObserver(display::DisplayObserver* observer) override;

 private:
  raw_ptr<LeanOsWindowManager> window_manager_;
  display::DisplayList display_list_;
};

}  // namespace ui

#endif  // LEAN_OS_OZONE_LEANOS_SCREEN_H_
