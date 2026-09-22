// The windows this platform has open, and the pixels behind each one.
//
// Two threads look at this. The UI thread creates and destroys windows and
// reads their event pipes. The viz compositor thread - in a --disable-gpu
// browser, the software output device runs there - asks for the buffer a
// widget draws into, and does so through this registry rather than through
// the window object, because the window is a UI-thread object with a
// UI-thread lifetime and the compositor's canvas can outlive it by a frame.
// The buffer is refcounted and outlives whichever side drops it last; its
// `alive` flag is what tells a late frame that the window is gone.

#ifndef LEAN_OS_OZONE_LEANOS_WINDOW_MANAGER_H_
#define LEAN_OS_OZONE_LEANOS_WINDOW_MANAGER_H_

#include <stdint.h>

#include <window_manager_client.h>

#include <atomic>
#include <map>

#include "base/containers/id_map.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/threading/thread_checker.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_ui_types.h"

namespace ui {

class LeanOsWindow;

// The compositor's shared-memory segment for one window: its pixels in the
// compositor's own XRGB word layout, which is byte for byte Skia's N32 on
// this machine, and the window id a present is sent for.
class LeanOsWindowBuffer : public base::RefCountedThreadSafe<LeanOsWindowBuffer> {
 public:
  LeanOsWindowBuffer(int32_t window_id, uint32_t* pixels, const gfx::Size& size);

  LeanOsWindowBuffer(const LeanOsWindowBuffer&) = delete;
  LeanOsWindowBuffer& operator=(const LeanOsWindowBuffer&) = delete;

  int32_t window_id() const { return window_id_; }
  uint32_t* pixels() const { return pixels_; }
  const gfx::Size& size() const { return size_; }
  bool alive() const { return alive_.load(std::memory_order_acquire); }
  void Retire() { alive_.store(false, std::memory_order_release); }

  // Asks the compositor to composite the segment as it is now. Callable from
  // any thread once the window has been shown - see LeanOsWindow::Show for
  // why that ordering matters.
  void Present();

 private:
  friend class base::RefCountedThreadSafe<LeanOsWindowBuffer>;
  ~LeanOsWindowBuffer();

  const int32_t window_id_;
  uint32_t* const pixels_;
  const gfx::Size size_;
  std::atomic<bool> alive_{true};
};

class LeanOsWindowManager {
 public:
  LeanOsWindowManager();

  LeanOsWindowManager(const LeanOsWindowManager&) = delete;
  LeanOsWindowManager& operator=(const LeanOsWindowManager&) = delete;

  ~LeanOsWindowManager();

  // UI thread.
  gfx::AcceleratedWidget AddWindow(LeanOsWindow* window);
  void RemoveWindow(gfx::AcceleratedWidget widget, LeanOsWindow* window);
  LeanOsWindow* GetWindow(gfx::AcceleratedWidget widget);
  gfx::AcceleratedWidget GetAcceleratedWidgetAtScreenPoint(const gfx::Point& point);
  void SetCursorScreenPoint(const gfx::Point& point) { cursor_ = point; }
  gfx::Point cursor_screen_point() const { return cursor_; }

  // Any thread.
  void SetBuffer(gfx::AcceleratedWidget widget, scoped_refptr<LeanOsWindowBuffer> buffer);
  scoped_refptr<LeanOsWindowBuffer> GetBuffer(gfx::AcceleratedWidget widget);

 private:
  base::IDMap<LeanOsWindow*> windows_;
  gfx::Point cursor_;
  base::ThreadChecker thread_checker_;

  base::Lock buffers_lock_;
  std::map<gfx::AcceleratedWidget, scoped_refptr<LeanOsWindowBuffer>> buffers_;
};

}  // namespace ui

#endif  // LEAN_OS_OZONE_LEANOS_WINDOW_MANAGER_H_
