// A Chromium PlatformWindow that is a window on this desktop.
//
// The compositor hands a client a shared-memory segment and an event pipe,
// and that is the whole contract - user_space/library/window_manager_client.h.
// This class is the ozone end of it: it connects when a top-level window is
// created, watches the pipe from the browser's UI thread, and turns each
// record on it into a ui::Event for the delegate. The bounds it reports are
// the segment's size at (0,0): the compositor can crop a window when it is
// resized but never reallocates a client's buffer, so from Chromium's side
// the window is fixed and the display it sits on is the framebuffer.

#ifndef LEAN_OS_OZONE_LEANOS_WINDOW_H_
#define LEAN_OS_OZONE_LEANOS_WINDOW_H_

#include <stdint.h>
#include <window_manager_client.h>

#include <memory>
#include <optional>

#include "base/files/file_descriptor_watcher_posix.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "lean_os/ozone/leanos_window_manager.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/platform_window/platform_window.h"
#include "ui/platform_window/platform_window_delegate.h"
#include "ui/platform_window/platform_window_init_properties.h"

namespace ui {

class LeanOsWindow : public PlatformWindow {
 public:
  LeanOsWindow(PlatformWindowDelegate* delegate,
               LeanOsWindowManager* manager,
               const PlatformWindowInitProperties& properties);

  LeanOsWindow(const LeanOsWindow&) = delete;
  LeanOsWindow& operator=(const LeanOsWindow&) = delete;

  ~LeanOsWindow() override;

  // PlatformWindow:
  void Show(bool inactive) override;
  void Hide() override;
  void Close() override;
  bool IsVisible() const override;
  void PrepareForShutdown() override;
  void SetBoundsInPixels(const gfx::Rect& bounds) override;
  gfx::Rect GetBoundsInPixels() const override;
  void SetBoundsInDIP(const gfx::Rect& bounds) override;
  gfx::Rect GetBoundsInDIP() const override;
  void SetTitle(const std::u16string& title) override;
  void SetCapture() override;
  void ReleaseCapture() override;
  void SetFullscreen(bool fullscreen, int64_t target_display_id) override;
  bool HasCapture() const override;
  void Maximize() override;
  void Minimize() override;
  void Restore() override;
  PlatformWindowState GetPlatformWindowState() const override;
  void Activate() override;
  void Deactivate() override;
  void SetUseNativeFrame(bool use_native_frame) override;
  bool ShouldUseNativeFrame() const override;
  void SetCursor(scoped_refptr<PlatformCursor> cursor) override;
  void MoveCursorTo(const gfx::Point& location) override;
  void ConfineCursorToBounds(const gfx::Rect& bounds) override;
  void SetRestoredBoundsInDIP(const gfx::Rect& bounds) override;
  gfx::Rect GetRestoredBoundsInDIP() const override;
  void SetWindowIcons(const gfx::ImageSkia& window_icon,
                      const gfx::ImageSkia& app_icon) override;
  void SizeConstraintsChanged() override;

  gfx::AcceleratedWidget widget() const { return widget_; }
  bool connected() const { return connection_ != nullptr; }

 private:
  void OnEventPipeReadable();
  void HandleEvent(const window_manager_event_t& event);
  void DispatchKey(char ch, uint8_t mods);
  void DispatchMouse(int32_t x, int32_t y, uint8_t buttons, uint8_t mods);
  void DispatchWheel(int32_t x, int32_t y, int32_t wheel, uint8_t mods);
  void UpdateWindowState(PlatformWindowState new_window_state);

  raw_ptr<PlatformWindowDelegate> delegate_;
  raw_ptr<LeanOsWindowManager> manager_;
  gfx::Rect bounds_;
  gfx::AcceleratedWidget widget_;

  std::unique_ptr<window_manager_window_t> connection_;
  scoped_refptr<LeanOsWindowBuffer> buffer_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> event_watcher_;

  uint8_t buttons_ = 0;
  bool is_popup_ = false;
  bool has_capture_ = false;
  gfx::Point last_mouse_;
  bool visible_ = false;
  bool activated_ = false;
  std::optional<gfx::Rect> restored_bounds_;
  PlatformWindowState window_state_ = PlatformWindowState::kUnknown;

  base::WeakPtrFactory<LeanOsWindow> weak_ptr_factory_{this};
};

}  // namespace ui

#endif  // LEAN_OS_OZONE_LEANOS_WINDOW_H_
