#include "lean_os/ozone/leanos_window_manager.h"

#include <window_manager_client.h>

#include "lean_os/ozone/leanos_window.h"
#include "ui/gfx/geometry/rect.h"

namespace ui {

LeanOsWindowBuffer::LeanOsWindowBuffer(int32_t window_id, uint32_t* pixels,
                                       const gfx::Size& size)
    : window_id_(window_id), pixels_(pixels), size_(size) {}

LeanOsWindowBuffer::~LeanOsWindowBuffer() = default;

void LeanOsWindowBuffer::Present() {
  if (!alive())
    return;
  window_manager_send_action(window_id_, WINDOW_MANAGER_ACTION_PRESENT);
}

void LeanOsWindowBuffer::PresentRect(const gfx::Rect& damage) {
  if (!alive())
    return;
  if (damage.IsEmpty() || damage.Contains(gfx::Rect(size()))) {
    Present();
    return;
  }
  window_manager_send_damage(window_id_, damage.x(), damage.y(), damage.width(),
                             damage.height());
}

LeanOsWindowManager::LeanOsWindowManager() = default;

LeanOsWindowManager::~LeanOsWindowManager() {
  DCHECK(thread_checker_.CalledOnValidThread());
}

gfx::AcceleratedWidget LeanOsWindowManager::AddWindow(LeanOsWindow* window) {
  DCHECK(thread_checker_.CalledOnValidThread());
  return windows_.Add(window);
}

void LeanOsWindowManager::RemoveWindow(gfx::AcceleratedWidget widget,
                                       LeanOsWindow* window) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK_EQ(window, windows_.Lookup(widget));
  windows_.Remove(widget);
  base::AutoLock lock(buffers_lock_);
  buffers_.erase(widget);
}

LeanOsWindow* LeanOsWindowManager::GetWindow(gfx::AcceleratedWidget widget) {
  DCHECK(thread_checker_.CalledOnValidThread());
  return windows_.Lookup(widget);
}

gfx::AcceleratedWidget LeanOsWindowManager::GetAcceleratedWidgetAtScreenPoint(
    const gfx::Point& point) {
  DCHECK(thread_checker_.CalledOnValidThread());
  for (base::IDMap<LeanOsWindow*>::const_iterator it(&windows_); !it.IsAtEnd();
       it.Advance()) {
    const LeanOsWindow* window = it.GetCurrentValue();
    if (window->GetBoundsInPixels().Contains(point))
      return window->widget();
  }
  return gfx::kNullAcceleratedWidget;
}

void LeanOsWindowManager::SetBuffer(gfx::AcceleratedWidget widget,
                                    scoped_refptr<LeanOsWindowBuffer> buffer) {
  base::AutoLock lock(buffers_lock_);
  if (buffer)
    buffers_[widget] = std::move(buffer);
  else
    buffers_.erase(widget);
}

scoped_refptr<LeanOsWindowBuffer> LeanOsWindowManager::GetBuffer(
    gfx::AcceleratedWidget widget) {
  base::AutoLock lock(buffers_lock_);
  auto it = buffers_.find(widget);
  return it == buffers_.end() ? nullptr : it->second;
}

}  // namespace ui
